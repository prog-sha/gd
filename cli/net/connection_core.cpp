// Bind transport-independent connection ownership to directional record and transcript operations.
#include "connection_state.h"
#include "handshake_protocol.h"
#include <algorithm>

namespace GDCrypto {
// Allocate inert private state; entropy and key work begin only with start.
TLSConnection::TLSConnection() : context(std::make_unique<Context>()) {}
// Destroy traffic secrets and ephemeral keys after exclusive worker ownership ends.
TLSConnection::~TLSConnection() = default;
// Transfer immutable role and authentication configuration into one new handshake.
bool TLSConnection::start(TLSOptions options) { return context->start(std::move(options)); }
// Apply receive-header policy before the transport allocates or waits for the declared payload.
bool TLSConnection::record_size(Bytes header, size_t &size) { return context->record_size(header,size); }
// Process one framed record without performing socket reads or writes.
bool TLSConnection::receive(Bytes record, std::vector<uint8_t> &application) { return context->receive(record,application); }
// Publish encrypted output through the owning transport's ordered queue.
std::vector<uint8_t> TLSConnection::drain() { auto bytes = std::move(context->output); context->output.clear(); return bytes; }
// Require complete handshake sequencing and its configured peer-authentication decision.
bool TLSConnection::ready() const { return context->phase == Context::READY && context->peer_verified; }
// Keep peer closure distinct from fatal protocol failure.
bool TLSConnection::closed() const { return context->read_closed || context->phase == Context::CLOSED; }
// Report the original sticky alert without consulting thread-local external state.
uint8_t TLSConnection::alert() const { return context->failure; }
// Expose only the negotiated protocol generation.
uint16_t TLSConnection::version() const { return context->negotiated; }
// Expose negotiated record protection for application-protocol policy checks.
uint16_t TLSConnection::cipher() const { return context->suite; }
// Retain the negotiated application identifier for the serving protocol dispatcher.
const std::string &TLSConnection::protocol() const { return context->application_protocol; }
// Retain peer identity independently of temporary record storage.
const std::vector<Cert::Ptr> &TLSConnection::peer() const { return context->certificates; }

// Encrypt one complete application fragment without changing the consumed count on failure.
bool TLSConnection::write(Bytes application, size_t &written) {
	if (!ready() || (application.size && !application.data)) return false;
	const size_t count = std::min<size_t>(application.size,16384); // Protocol plaintext-fragment width, not a logical write limit.
	if (count && !context->queue(23,{application.data,count})) return context->fail(Context::INTERNAL_ERROR);
	written = count; return true;
}

// Close the local connection with a directional authenticated notification when keys are usable.
void TLSConnection::close() {
	if (context->phase == Context::READY) {const uint8_t notice[] = {1,0}; context->queue(21,{notice,sizeof(notice)});}
	if (context->phase != Context::FAILED) context->phase = Context::CLOSED;
}

// Retain the first failure and avoid recursive alerts when a record-key epoch is already unusable.
bool TLSConnection::Context::fail(uint8_t alert, bool reply) {
	if (!failure) {
		failure = alert; phase = FAILED;
		if (reply) {const uint8_t notice[] = {2,alert}; queue(21,{notice,sizeof(notice)});}
	}
	return false;
}

// Select the suite's digest after both hello transcript candidates have been retained.
TLSHash &TLSConnection::Context::transcript() { return cipher.sha384 ? wide : narrow; }

// Keep both candidates only while the peer has not committed to a cipher suite.
bool TLSConnection::Context::track(Bytes message) {
	if (retain_legacy && message.size) legacy_transcript.insert(legacy_transcript.end(),message.data,message.data+message.size);
	return suite ? transcript().write(message) : narrow.write(message) && wide.write(message);
}

// Fragment logical output into authenticated or explicitly plaintext records without socket-capacity coupling.
bool TLSConnection::Context::queue(uint8_t type, Bytes payload, bool plaintext) {
	if (payload.size && !payload.data) return false;
	size_t at = 0;
	do {
		const size_t count = std::min<size_t>(payload.size-at,16384); // Wire plaintext fragment ceiling.
		Buffer record; const Bytes part{payload.data ? payload.data+at : nullptr,count};
		if (write_keys && !plaintext) {if (!outgoing.seal(type,part,record)) return false;}
		else {
			TLSWriter writer; const uint16_t version = !negotiated && !options.server ? 0x0301 : 0x0303;
			if (!writer.number(1,type) || !writer.number(2,version) || !writer.vector(2,part) || !writer.finish(record)) return false;
		}
		output.insert(output.end(),record.begin(),record.end()); at += count;
	} while (at != payload.size);
	return true;
}

// Preserve exact transcript encodings independently of later record fragmentation.
bool TLSConnection::Context::emit(uint8_t type, Bytes body) {
	TLSWriter writer; Buffer encoded;
	if (!writer.number(1,type) || !writer.vector(3,body) || !writer.finish(encoded) || !track(tls_bytes(encoded)) || !queue(22,tls_bytes(encoded))) return fail(INTERNAL_ERROR);
	return true;
}

// Derive and install one fresh record epoch before replacing its retained traffic secret.
bool TLSConnection::Context::install(Secret secret, bool read) {
	Secret key(cipher.key), iv(cipher.iv);
	if (!kdf.expand(tls_bytes(secret),tls_text("key"),{},key.data(),key.size()) || !kdf.expand(tls_bytes(secret),tls_text("iv"),{},iv.data(),iv.size())) return fail(INTERNAL_ERROR);
	TLSRecord &records = read ? incoming : outgoing;
	if (!records.reset(suite,tls_bytes(key),tls_bytes(iv))) return fail(INTERNAL_ERROR);
	Secret &previous = read ? read_secret : write_secret; wipe_storage(previous); previous = std::move(secret);
	(read ? read_keys : write_keys) = true; if (read) ++read_epoch; return true;
}

// Separate early, handshake, and master extraction while binding directional keys to the hello transcript.
bool TLSConnection::Context::handshake_keys(Bytes shared) {
	Secret early(kdf.size()), derived(kdf.size()), handshake(kdf.size()), client(kdf.size()), server(kdf.size()); master.resize(kdf.size());
	TLSHash empty(cipher.sha384);
	if (!kdf.extract({}, {},early.data()) || !kdf.derive(tls_bytes(early),tls_text("derived"),empty,derived.data()) || !kdf.extract(shared,tls_bytes(derived),handshake.data()) ||
			!kdf.derive(tls_bytes(handshake),tls_text("c hs traffic"),transcript(),client.data()) || !kdf.derive(tls_bytes(handshake),tls_text("s hs traffic"),transcript(),server.data()) ||
			!kdf.derive(tls_bytes(handshake),tls_text("derived"),empty,derived.data()) || !kdf.extract({},tls_bytes(derived),master.data())) return fail(INTERNAL_ERROR);
	shares.clear();
	return options.server ? install(std::move(client),true) && install(std::move(server),false) : install(std::move(server),true) && install(std::move(client),false);
}

// Derive both application directions from exactly the server-finished checkpoint, then erase the master secret.
bool TLSConnection::Context::application_keys() {
	Secret client(kdf.size()), server(kdf.size());
	if (!kdf.derive(tls_bytes(master),tls_text("c ap traffic"),transcript(),client.data()) || !kdf.derive(tls_bytes(master),tls_text("s ap traffic"),transcript(),server.data())) return fail(INTERNAL_ERROR);
	pending_read = options.server ? std::move(client) : std::move(server); pending_write = options.server ? std::move(server) : std::move(client);
	wipe_storage(master); master.clear(); return true;
}

// Validate explicit protocol configuration before generating entropy or exposing any initial output.
bool TLSConnection::Context::start(TLSOptions selected) {
	if (phase != NEW) return false;
	options = std::move(selected);
	if (options.minimum < 0x0303 || options.maximum > 0x0304 || options.minimum > options.maximum || (options.server && !options.identity) ||
			(!options.server && (options.hostname.find('\0') != options.hostname.npos || (!options.insecure && (options.hostname.empty() || !options.verify))))) return fail(INTERNAL_ERROR,false);
	if (options.groups.empty()) options.groups = {29,23,24,25};
	for (uint16_t group : options.groups) if (!TLSShare::bits(group)) return fail(INTERNAL_ERROR,false);
	if (options.suites.empty()) options.suites = tls_suites();
	for (uint16_t choice : options.suites) {TLSCipher config; if (!TLSCipher::find(choice,config)) return fail(INTERNAL_ERROR,false);}
	if (options.client_auth < TLSOptions::NONE || options.client_auth > TLSOptions::REQUIRE_AND_VERIFY ||
			(options.server && options.client_auth >= TLSOptions::VERIFY_IF_GIVEN && !options.verify)) return fail(INTERNAL_ERROR,false);
	for (const auto &protocol : options.protocols) if (protocol.empty() || protocol.size() > 255) return fail(INTERNAL_ERROR,false);
	if (options.server) {phase = CLIENT_HELLO; peer_verified = options.client_auth == TLSOptions::NONE; return true;}
	phase = SERVER_HELLO; return client_hello();
}
}
