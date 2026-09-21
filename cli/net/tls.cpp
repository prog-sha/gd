// Run independent TLS negotiation and record cryptography on exclusive workers with readiness-driven native I/O.
#include "cli/net/tls.h"
#include "cli/net/connection_core.h"
#include "cli/net/record_core.h"
#include "cli/sys/file_job.h"
#include "cli/sys/task.h"
#include "cli/sys/os.h"
#include "core/object/callable_mp.h"
#include <algorithm>

// Release immutable credentials after every accepted connection has relinquished ownership.
GDTLSIdentity::~GDTLSIdentity() = default;

// Validate configured certificate blocks and reject unusable signing keys before accepting clients.
Ref<R> GDTLSIdentity::load(const String &p_cert, const String &p_key, const Dictionary &p_opts) {
	for (const KeyValue<Variant,Variant> &entry : p_opts) if (String(entry.key) != "client_auth" && String(entry.key) != "client_ca") return R::err(vformat("unknown TLS listener option: %s",entry.key),Err::INVALID_DATA);
	const Variant auth = p_opts.get("client_auth","none"), ca = p_opts.get("client_ca","");
	if (auth.get_type() != Variant::STRING || ca.get_type() != Variant::STRING) return R::err("TLS client_auth and client_ca must be strings",Err::INVALID_DATA);
	const char *const modes[] = {"none","request","require","verify_if_given","require_and_verify"}; // Certificate presence and trust requirements are independent policy choices.
	int mode = -1; for (int at = 0; at != 5; ++at) if (String(auth) == modes[at]) mode = at;
	if (mode < 0) return R::err("invalid TLS client_auth policy",Err::INVALID_DATA);
	const Ref<R> cert = Os::read_bytes(p_cert), key = Os::read_bytes(p_key);
	if (!cert->get_ok()) return cert;
	if (!key->get_ok()) return key;
	const PackedByteArray chain = cert->get_v(), secret = key->get_v();
	const std::string_view certificates(reinterpret_cast<const char *>(chain.ptr()),chain.size()), private_key(reinterpret_cast<const char *>(secret.ptr()),secret.size());
	auto config = GDCrypto::TLSIdentity::read(certificates,private_key);
	if (!config) return R::err("invalid TLS certificate chain or private key",Err::INVALID_DATA);
	// Every declared certificate must decode; malformed trailing configuration is not silently discarded.
	constexpr std::string_view marker = "-----BEGIN CERTIFICATE-----"; // Exact textual certificate-container boundary.
	size_t count = 0;
	for (size_t at = certificates.find(marker); at != certificates.npos; at = certificates.find(marker,at+marker.size())) if (!at || certificates[at-1] == '\n') ++count;
	if (count != config->chain().size() || (config->signer().kind() == GDCrypto::KeyKind::RSA && config->signer().bits() < 1024)) return R::err("invalid TLS certificate chain or private key",Err::INVALID_DATA);
	Ref<GDTLSIdentity> identity; identity.instantiate(); identity->config = std::move(config); identity->client_auth = mode;
	if (!String(ca).is_empty() || mode >= GDCrypto::TLSOptions::VERIFY_IF_GIVEN) {
		const Ref<R> roots = GDTrust::load(ca); if (!roots->get_ok()) return roots;
		identity->clients = roots->get_v();
	}
	return R::ok(identity);
}

// Keep protocol state and incomplete record framing private to the active cryptographic worker.
struct GDTLS::Context {
	GDCrypto::TLSConnection connection; // Independent protocol and directional-key ownership.
	Ref<GDStream> tcp; // Descriptor pinned before each worker job.
	Ref<GDTLSIdentity> identity; // Immutable server credentials retained through setup.
	Ref<GDTrust> trust; // Immutable trust configuration used inside peer authentication.
	String host, why; // Verification name and worker-local terminal reason.
	Ref<Err> failure; // Original cause owned exclusively by the cryptographic worker.
	PackedStringArray protocols; // Ordered application-protocol offers retained before worker setup.
	State state = CLOSED; // Worker-visible cryptographic phase.
	std::vector<uint8_t> record; // One incomplete encrypted record, never an application-length allocation.
	size_t record_at = 0; // Bytes received into the current record.
	bool server = false, insecure = false, eof = false, initialized = false; // Endpoint role, trust policy, and receive completion.
	Error fail(const String &p_reason, const Ref<Err> &p_error = Ref<Err>()); // Retain a terminal worker failure without consulting external error state.
	Error setup(); // Configure the protocol and trust callback before emitting the first handshake flight.
	Error receive(); // Read exactly one framed record without blocking or reading ahead across application demand.
	Error advance(std::vector<uint8_t> &p_application); // Process control records until handshake completion, application input, or a kernel wait.
	void run(Work &p_work); // Execute one readiness-driven group of exclusively owned operations.
};

// Transfer stable input and return results through the worker completion barrier.
struct GDTLS::Work {
	bool handshake = false, reading = false, writing = false, closing = false; // Operations selected by runtime demand.
	PackedByteArray data, output, ciphertext; // Pending plaintext write, authenticated input, and ordered encrypted output.
	State state = HANDSHAKE; // Resulting cryptographic state.
	String why; // Worker-local failure description.
	Ref<Err> failure; // Cause transferred through the worker completion barrier.
	String protocol; // Verified application-protocol selection.
	int version = 0; // Negotiated protocol version.
	bool multiplex_ok = false; // Application-protocol policy derived inside the completed handshake worker.
	int sent = 0; // Successfully encrypted application bytes.
	Error read_error = ERR_BUSY, write_error = ERR_BUSY; // Distinguish kernel waits from completed progress.
	bool eof = false; // Receive completion consumed by the readiness owner.
};

// Allocate inert storage without starting cryptographic work on the event loop.
GDTLS::GDTLS() { ctx = memnew(Context); }

// Keep the descriptor and private state alive until the final worker releases ownership.
GDTLS::~GDTLS() {
	if (tcp.is_valid()) tcp->close();
	memdelete(ctx);
}

// Preserve the original terminal reason across later cleanup and canceled operations.
Error GDTLS::Context::fail(const String &p_reason, const Ref<Err> &p_error) {
	if (failure.is_null()) failure = p_error.is_valid() ? p_error : Err::make(p_reason, Err::NONE);
	if (state != BROKEN) why = failure->text();
	state = BROKEN; return FAILED;
}

// Perform peer trust verification before the protocol can send client credentials or publish application readiness.
Error GDTLS::Context::setup() {
	GDCrypto::TLSOptions options; options.server = server; options.insecure = insecure;
	const CharString name = host.utf8(); options.hostname.assign(name.get_data(),name.length());
	if (identity.is_valid()) options.identity = identity->config;
	for (const String &protocol : protocols) {const CharString value = protocol.utf8(); options.protocols.emplace_back(value.get_data(),value.length());}
	if (server) {
		if (identity.is_null() || !identity->config) return fail("missing TLS server identity");
		options.protocols = {"h2", "http/1.1"};
		options.client_auth = GDCrypto::TLSOptions::ClientAuth(identity->client_auth);
		trust = identity->clients;
		if (trust.is_valid()) options.authorities = trust->names();
	} else if (!insecure && trust.is_null()) {
		const Ref<R> loaded = GDTrust::load(String());
		if (!loaded->get_ok()) return fail("TLS trust loading failed", loaded->get_e());
		trust = loaded->get_v();
	}
	options.verify = [this](const std::vector<GDCrypto::Cert::Ptr> &certificates, bool peer_server) {
		if (trust.is_null()) return false;
		const Ref<R> checked = trust->verify(certificates,host,peer_server);
		if (!checked->get_ok() && failure.is_null()) failure = checked->get_e();
		return checked->get_ok();
	};
	if (!connection.start(std::move(options))) return fail(vformat("TLS configuration failed (%d)",connection.alert()));
	initialized = true; state = HANDSHAKE; return OK;
}

// Fill a single protocol record while preserving partial-header and partial-body EOF detection.
Error GDTLS::Context::receive() {
	if (record.empty()) record.resize(5); // Record header width; payload allocation follows validation.
	while (record_at < record.size()) {
		int got = 0; const Error error = tcp->read(record.data()+record_at,int(record.size()-record_at),got);
		if (error == ERR_BUSY) return ERR_BUSY;
		if (error == ERR_FILE_EOF) {
			if (record_at || state != READY) return fail("unexpected EOF in TLS record");
			eof = true; return ERR_FILE_EOF;
		}
		if (error != OK || got <= 0) return fail("TLS transport read failed", Err::make("TLS transport read failed", Err::of(error)));
		record_at += got;
		if (record_at == 5 && record.size() == 5) {
			size_t size = 0;
			if (!connection.record_size({record.data(),5},size)) return fail(vformat("invalid TLS record header (%d)",connection.alert()));
			record.resize(5+size);
		}
	}
	return OK;
}

// Advance control records without inventing a per-frame work quota or exposing unauthenticated plaintext.
Error GDTLS::Context::advance(std::vector<uint8_t> &p_application) {
	const bool handshake = state == HANDSHAKE;
	for (;;) {
		if (connection.closed()) {eof = true; return ERR_FILE_EOF;}
		const Error received = receive(); if (received != OK) return received;
		const bool valid = connection.receive({record.data(),record.size()},p_application); record.clear(); record_at = 0;
		if (!valid) return fail(vformat("TLS protocol failed (%d)",connection.alert()));
		if (connection.ready()) state = READY;
		if (!p_application.empty() || (handshake && state == READY)) return OK;
	}
}

// Perform every handshake, trust, key-update, and record operation under one worker ownership interval.
void GDTLS::Context::run(Work &p_work) {
	if (!initialized && !p_work.closing) setup();
	if (p_work.closing) {if (state == READY) connection.close(); state = CLOSED;}
	else if (state == HANDSHAKE && p_work.handshake) {
		std::vector<uint8_t> application; advance(application);
	}
	if (state == READY && p_work.writing) {
		size_t sent = 0;
		if (!connection.write({p_work.data.ptr(),size_t(p_work.data.size())},sent)) p_work.write_error = fail(vformat("TLS record write failed (%d)",connection.alert()));
		else {p_work.sent = int(sent); p_work.write_error = OK;}
	}
	if (state == READY && p_work.reading) {
		std::vector<uint8_t> application;
		p_work.read_error = eof ? ERR_FILE_EOF : advance(application);
		if (p_work.output.resize(application.size()) != OK) fail("cannot allocate TLS record output", Err::make("cannot allocate TLS record output", Err::LIMITED));
		else if (!application.empty()) memcpy(p_work.output.ptrw(),application.data(),application.size());
	}
	const auto ciphertext = connection.drain();
	if (p_work.ciphertext.resize(ciphertext.size()) != OK) fail("cannot allocate TLS ciphertext output", Err::make("cannot allocate TLS ciphertext output", Err::LIMITED));
	else if (!ciphertext.empty()) memcpy(p_work.ciphertext.ptrw(),ciphertext.data(),ciphertext.size());
	p_work.state = state; p_work.why = why; p_work.failure = failure; p_work.eof = eof;
	if (state == READY) {
		p_work.protocol = String::utf8(connection.protocol().data(),connection.protocol().size()); p_work.version = connection.version();
		GDCrypto::TLSCipher cipher;
		p_work.multiplex_ok = connection.version() >= 0x0303 && GDCrypto::TLSCipher::find(connection.cipher(),cipher) && cipher.mode != GDCrypto::TLSCipher::CBC;
	}
}

// Prepare runtime ownership without invoking the cryptographic backend.
Error GDTLS::start(const Ref<GDStream> &p_tcp, const String &p_host, const Ref<GDTrust> &p_trust, bool p_insecure, const Ref<GDTLSIdentity> &p_identity, const PackedStringArray &p_protocols) {
	if (state != CLOSED || tcp.is_valid()) return ERR_ALREADY_IN_USE;
	tcp = p_tcp;
	for (int i = 0; i < p_host.length(); ++i) if (!p_host[i]) return fail("invalid TLS server name");
	ctx->tcp = tcp;
	ctx->host = p_host;
	ctx->trust = p_trust;
	ctx->insecure = p_insecure;
	ctx->identity = p_identity;
	ctx->protocols = p_protocols;
	state = HANDSHAKE;
	return OK;
}

// Retain a shared identity while deferring connection setup to its worker.
Error GDTLS::accept(const Ref<GDStream> &p_tcp, const Ref<GDTLSIdentity> &p_identity) {
	if (state != CLOSED || tcp.is_valid()) return ERR_ALREADY_IN_USE;
	tcp = p_tcp;
	if (p_identity.is_null()) return fail("missing TLS server identity");
	ctx->tcp = tcp;
	ctx->identity = p_identity;
	ctx->server = true;
	state = HANDSHAKE;
	return OK;
}

// Record a terminal error and unregister the descriptor before it can be reused.
Error GDTLS::fail(const String &p_reason, const Ref<Err> &p_error) {
	if (failure.is_null()) failure = p_error.is_valid() ? p_error : Err::make(p_reason, Err::NONE);
	why = failure->text();
	state = BROKEN;
	if (tcp.is_valid()) tcp->close();
	return FAILED;
}

// Translate caller demand into kernel waits only when no worker owns the context.
void GDTLS::interests() {
	if (tcp.is_null()) return;
	bool rd = false, wr = !ciphertext.is_empty();
	if (!busy && state == HANDSHAKE) rd = true;
	else if (!busy && state == READY) {
		rd = (reading || read_requested) && !eof && !available();
		wr = wr || writing;
	}
	if (state == CLOSED || state == BROKEN || !callback.is_valid()) rd = wr = false;
	tcp->watch(rd, wr, callable_mp(this, &GDTLS::socket_ready));
}

// Drain completed ciphertext while preserving read-side progress under send backpressure.
void GDTLS::flush() {
	while (!ciphertext.is_empty() && state != CLOSED && state != BROKEN) {
		const PackedByteArray &bytes = ciphertext.front()->get();
		int count = 0;
		const Error error = tcp->write(bytes.ptr() + cipher_at, int(MIN(bytes.size() - cipher_at, int64_t(GDStream::MAX_WRITE))), count);
		if (error == ERR_BUSY) return;
		if (error != OK || !count) { fail("TLS transport write failed", Err::make("TLS transport write failed", Err::of(error))); return; }
		cipher_at += count;
		if (cipher_at == bytes.size()) { ciphertext.pop_front(); cipher_at = 0; }
	}
}

// Separate kernel readiness from worker-completion notifications to avoid retry spins.
void GDTLS::socket_ready() {
	can_read = can_write = can_handshake = true;
	flush();
	if (callback.is_valid()) callback.call();
}

// Preserve logical demand while serializing cryptographic context ownership.
void GDTLS::watch(bool p_read, bool p_write, const Callable &p_callback) {
	reading = p_read;
	writing = p_write;
	if (!reading) read_requested = false;
	callback = p_callback;
	interests();
}

// Submit a single worker job; shared descriptor ownership prevents concurrent close/reuse.
void GDTLS::dispatch(bool p_close) {
	if (busy || (state != HANDSHAKE && state != READY)) return;
	const bool hand = state == HANDSHAKE && can_handshake && !p_close;
	const bool rd = state == READY && read_requested && !available() && !eof && can_read && !p_close;
	const bool wr = state == READY && !output.is_empty() && !sent && !write_failed && can_write && !p_close;
	if (!hand && !rd && !wr && !p_close) return;
	if (!tcp->retain_io()) { fail("TLS transport is closed"); return; }
	job = std::make_shared<Work>();
	job->handshake = hand;
	job->reading = rd;
	job->writing = wr;
	job->closing = p_close;
	if (wr) job->data = output;
	launch();
}

// Hold both the context and its descriptor through completion-signal delivery.
void GDTLS::launch() {
	busy = true;
	self_hold = Ref<GDTLS>(this);
	interests();
	const Ref<GDTLS> keep(this);
	const std::shared_ptr<Work> work = job;
	Signal done = GDFileCall::start([keep, work]() -> Ref<R> { keep->ctx->run(*work); return R::ok(); }, true);
	done.connect(callable_mp(this, &GDTLS::completed), Object::CONNECT_ONE_SHOT);
}

// Publish results after the completion barrier, keeping cancellation authoritative.
void GDTLS::completed(const Ref<R> &p_result) {
	const Ref<GDTLS> keep(this);
	self_hold.unref();
	const std::shared_ptr<Work> work = std::move(job);
	busy = false;
	// Finish an already-running operation before constructing the close alert on the same leased descriptor.
	if (state == CLOSED && !work->closing && work->state == READY && work->ciphertext.is_empty() && !write_failed && p_result.is_valid() && p_result->get_ok() && !Pool::is_stopping()) {
		job = std::make_shared<Work>();
		job->closing = true;
		launch();
		return;
	}
	// A final alert can use its lease after logical close; make only one nonblocking send attempt.
	if (state == CLOSED && work->closing && !work->ciphertext.is_empty()) {
		int ignored = 0;
		tcp->write(work->ciphertext.ptr(), int(work->ciphertext.size()), ignored);
	}
	tcp->release_io();
	if (state == CLOSED || state == BROKEN) return;
	// Alert delivery cannot replace the worker failure that required the alert.
	if (failure.is_null() && work->state == BROKEN) failure = work->failure;
	if (!work->ciphertext.is_empty() && !write_failed) { ciphertext.push_back(work->ciphertext); flush(); }
	if (state == BROKEN) { if (callback.is_valid()) Async::post(keep, callback); return; }
	if (p_result.is_null() || !p_result->get_ok()) fail("TLS worker failed", p_result.is_valid() ? p_result->get_e() : Ref<Err>());
	else if (work->state == BROKEN) fail(work->why, work->failure);
	else {
		eof = work->eof;
		if (work->handshake) can_handshake = false;
		if (work->reading) {
			read_requested = false;
			can_read = work->read_error == OK;
			input = work->output;
			input_at = 0;
		}
		if (work->writing) {
			can_write = work->write_error == OK;
			sent = work->sent;
		}
		if (work->handshake && work->state == READY) {
			state = READY;
			protocol = work->protocol;
			version = work->version;
			multiplex_ok = work->multiplex_ok;
		}
	}
	// Dispatch writes queued during a read job before invoking read-first consumers.
	if (state == READY && !output.is_empty() && !sent && !write_failed && can_write) dispatch();
	interests();
	if (callback.is_valid()) Async::post(Ref<RefCounted>(this), callback);
}

// Advance only when demand and a retry token justify another worker operation.
void GDTLS::poll() {
	if (state == READY && reading) read_requested = true;
	dispatch();
}

// Consume one completed plaintext slice without exposing live cryptographic state.
Error GDTLS::read(uint8_t *p_data, int p_size, int &r_got) {
	r_got = 0;
	if (state != READY) return ERR_UNCONFIGURED;
	if (p_size < 0) return ERR_INVALID_PARAMETER;
	if (!p_size) return OK;
	if (available()) {
		r_got = MIN(p_size, available());
		memcpy(p_data, input.ptr() + input_at, r_got);
		input_at += r_got;
		if (!available()) { input = PackedByteArray(); input_at = 0; }
		return OK;
	}
	if (eof) return ERR_FILE_EOF;
	read_requested = true;
	dispatch();
	return state == READY ? ERR_BUSY : FAILED;
}

// Preserve the standard one-slice write interface.
Error GDTLS::write(const uint8_t *p_data, int p_size, int &r_sent) {
	int64_t sent = 0;
	const GDWrite part{p_data, p_size};
	const Error error = write_parts(GDWrites(part), sent);
	r_sent = int(sent);
	return error;
}

// Retain one plaintext record until the worker has completed the same write.
Error GDTLS::write_parts(GDWrites p_parts, int64_t &r_sent) {
	r_sent = 0;
	if (state != READY || write_failed) return ERR_UNCONFIGURED;
	GDWrites scan = p_parts;
	GDWrite part;
	int size = 0;
	while (size < 16384 && scan.next(part)) {
		if (part.size < 0) return ERR_INVALID_PARAMETER;
		size += int(MIN(part.size, int64_t(16384 - size)));
	}
	if (!size) return OK;
	const bool fresh = output.is_empty();
	if (fresh && output.resize(size) != OK) return fail("cannot allocate TLS record input", Err::make("cannot allocate TLS record input", Err::LIMITED));
	if (size < output.size()) {
		write_failed = true;
		return ERR_INVALID_DATA;
	}
	// Preserve the pending record even when retry regions split its plaintext differently.
	int at = 0;
	while (at < output.size() && p_parts.next(part)) {
		const int count = int(MIN(part.size, output.size() - at));
		if (fresh) memcpy(output.ptrw() + at, part.data, count);
		else if (memcmp(output.ptr() + at, part.data, count) != 0) {
			write_failed = true;
			return ERR_INVALID_DATA;
		}
		at += count;
	}
	flush();
	if (state != READY) return FAILED;
	if (sent && ciphertext.is_empty()) {
		r_sent = sent;
		sent = 0;
		output = PackedByteArray();
		return OK;
	}
	dispatch();
	return state == READY ? ERR_BUSY : FAILED;
}

// Stop pending record output after cancellation or a write deadline, without retrying its remainder.
void GDTLS::abort_write() {
	write_failed = true;
	ciphertext.clear();
	cipher_at = 0;
	interests();
}

// Close logically now; an idle established connection can send its alert on a final worker job.
void GDTLS::close() {
	if (state == CLOSED) return;
	// Abandoning an encrypted record also forbids a later alert with a skipped record sequence number.
	if (!output.is_empty() || !ciphertext.is_empty()) abort_write();
	if (!busy && state == READY && !write_failed && !Pool::is_stopping()) dispatch(true);
	state = CLOSED;
	callback = Callable();
	if (tcp.is_valid()) tcp->close();
}
