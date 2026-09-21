// Preserve authenticated ephemeral legacy handshakes with transcript-bound master secrets and explicit key activation.
#include "connection_state.h"
#include "handshake_protocol.h"
#include "cli/data/hash_core.h"
#include <algorithm>
#include <cstring>

namespace GDCrypto {
// Require the signature family encoded by the selected ephemeral cipher suite.
static bool suite_key(uint16_t suite, KeyKind kind) {
	const bool elliptic = suite == 0xc02b || suite == 0xc02c || suite == 0xcca9 || suite == 0xc009 || suite == 0xc00a;
	return elliptic ? kind == KeyKind::EC || kind == KeyKind::ED25519 : kind == KeyKind::RSA;
}

// Bind both public randoms to the encoded ephemeral parameters before signing or verifying them.
static std::vector<uint8_t> exchange_proof(Bytes client, Bytes server, Bytes params) {
	std::vector<uint8_t> output; output.insert(output.end(),client.data,client.data+client.size);
	output.insert(output.end(),server.data,server.data+server.size); output.insert(output.end(),params.data,params.data+params.size); return output;
}

// Apply legacy negotiation only to full handshakes, preserving authenticated version and extension choices.
bool TLSConnection::Context::legacy_hello(Bytes body) {
	TLSHello hello; if (!TLSHello::read(body,options.server,hello)) return fail(DECODE_ERROR);
	if (retried) return fail(ILLEGAL_PARAMETER);
	if (auto value = hello.extensions.find(23); value != hello.extensions.end()) {if (value->second.size) return fail(DECODE_ERROR); extended_master = true;}
	if (auto value = hello.extensions.find(0xff01); value != hello.extensions.end()) {if (value->second.size != 1 || value->second.data[0]) return fail(HANDSHAKE_FAILURE);}
	if (auto value = hello.extensions.find(11); value != hello.extensions.end()) {
		TLSView reader(value->second); Bytes formats;
		if (!reader.vector(1,formats) || !reader.empty() || !formats.size || !std::memchr(formats.data,0,formats.size)) return fail(ILLEGAL_PARAMETER);
	}
	if (auto value = hello.extensions.find(16); value != hello.extensions.end()) {
		std::vector<std::string> protocols; if (!tls_protocols(value->second,protocols)) return fail(DECODE_ERROR);
		if (options.server) {
			for (const auto &name : options.protocols) if (std::find(protocols.begin(),protocols.end(),name) != protocols.end()) {application_protocol = name; break;}
			if (!options.protocols.empty() && application_protocol.empty()) return fail(NO_APPLICATION_PROTOCOL);
		} else {
			if (protocols.size() != 1 || std::find(options.protocols.begin(),options.protocols.end(),protocols.front()) == options.protocols.end()) return fail(UNSUPPORTED_EXTENSION);
			application_protocol = protocols.front();
		}
	}
	retain_legacy = true;
	TLSWriter encoded; encoded.number(1,options.server ? 1 : 2); encoded.vector(3,body);
	if (!options.server) {
		for (const auto &[kind,value] : hello.extensions) if (kind != 23 && kind != 0xff01 && kind != 11 && kind != 16 && !(kind == 0 && server_name && !value.size)) return fail(UNSUPPORTED_EXTENSION);
		const uint8_t downgrade[] = {'D','O','W','N','G','R','D',1}; // Authenticated downgrade sentinel for a modern-capable server.
		if (options.maximum >= 0x0304 && (!std::memcmp(hello.random.data+24,downgrade,8) || (!std::memcmp(hello.random.data+24,downgrade,7) && hello.random.data[31] == 0))) return fail(ILLEGAL_PARAMETER);
		std::copy_n(hello.random.data,32,server_random.begin());
		TLSWriter first; first.number(1,1); first.vector(3,tls_bytes(initial_hello));
		if (!first.ok()) return fail(INTERNAL_ERROR);
		legacy_transcript.assign(first.view().data,first.view().data+first.view().size);
		if (!encoded.ok() || !track(encoded.view())) return fail(INTERNAL_ERROR);
		initial_hello.clear(); phase = LEGACY; legacy_step = L_CERT; return true;
	}
	std::vector<uint16_t> suites, groups; TLSWriter field; field.vector(2,hello.suites);
	if (!tls_list(field.view(),2,suites)) return fail(DECODE_ERROR);
	if (auto value = hello.extensions.find(10); value != hello.extensions.end()) {if (!tls_list(value->second,2,groups)) return fail(DECODE_ERROR);}
	for (uint16_t value : options.groups) if (std::find(groups.begin(),groups.end(),value) != groups.end()) {selected_group = value; break;}
	if (!selected_group) return fail(HANDSHAKE_FAILURE);
	const auto &signer = options.identity->signer();
	for (uint16_t value : options.suites) {TLSCipher candidate; TLSCipher::find(value,candidate); if (!candidate.modern && suite_key(value,signer.kind()) && std::find(suites.begin(),suites.end(),value) != suites.end()) {suite = value; cipher = candidate; break;}}
	if (!suite) return fail(HANDSHAKE_FAILURE);
	kdf = TLSKDF(cipher.sha384);
	std::copy_n(hello.random.data,32,client_random.begin());
	if (!encoded.ok() || !track(encoded.view()) || !random_fill(server_random.data(),server_random.size())) return fail(INTERNAL_ERROR);
	if (options.maximum >= 0x0304) {const uint8_t downgrade[] = {'D','O','W','N','G','R','D',1}; std::copy_n(downgrade,8,server_random.begin()+24);}
	TLSWriter extensions, empty, renegotiation, formats;
	const uint8_t zero[] = {0}; renegotiation.vector(1,{}); formats.vector(1,{zero,1});
	if ((extended_master && !tls_extension(extensions,23,empty)) || !tls_extension(extensions,0xff01,renegotiation) || !tls_extension(extensions,11,formats)) return fail(INTERNAL_ERROR);
	if (!application_protocol.empty()) {TLSWriter names, protocols; names.vector(1,tls_text(application_protocol)); protocols.vector(2,names.view()); if (!names.ok() || !tls_extension(extensions,16,protocols)) return fail(INTERNAL_ERROR);}
	TLSWriter response; response.number(2,0x0303); response.append(tls_bytes(server_random)); response.vector(1,{}); response.number(2,suite); response.number(1,0); response.vector(2,extensions.view());
	if (!response.ok() || !emit(2,response.view()) || !send_certificate(false)) return fail(INTERNAL_ERROR);
	auto share = std::make_unique<TLSShare>(); if (!share->reset(selected_group)) return fail(INTERNAL_ERROR);
	TLSWriter params; params.number(1,3); params.number(2,selected_group); params.vector(1,share->public_key()); shares.clear(); shares.emplace(selected_group,std::move(share));
	SignatureInfo algorithm; uint16_t selected = 0;
	for (uint16_t scheme : tls_signatures) if (std::find(peer_schemes.begin(),peer_schemes.end(),scheme) != peer_schemes.end() && tls_signature(scheme,signer.kind(),signer.bits(),false,algorithm)) {selected = scheme; break;}
	if (!selected) return fail(HANDSHAKE_FAILURE);
	Buffer signature; const Buffer proof = exchange_proof(tls_bytes(client_random),tls_bytes(server_random),params.view());
	if (!params.ok() || !signer.sign(algorithm,tls_bytes(proof),signature)) return fail(INTERNAL_ERROR);
	params.number(2,selected); params.vector(2,tls_bytes(signature));
	if (!params.ok() || !emit(12,params.view())) return fail(INTERNAL_ERROR);
	requested = options.client_auth != TLSOptions::NONE;
	if (requested) {
		TLSWriter request, schemes; const uint8_t types[] = {1,64}; request.vector(1,{types,sizeof(types)});
		for (uint16_t scheme : tls_signatures) schemes.number(2,scheme);
		TLSWriter names; for (const auto &name : options.authorities) names.vector(2,tls_bytes(name));
		request.vector(2,schemes.view()); request.vector(2,names.view());
		if (!names.ok() || !schemes.ok() || !request.ok() || !emit(13,request.view())) return fail(INTERNAL_ERROR);
	}
	if (!emit(14,{})) return false;
	phase = LEGACY; legacy_step = requested ? L_CLIENT_CERT : L_CLIENT_KEY;
	if (!requested) {retain_legacy = false; legacy_transcript.clear();}
	return true;
}

// Derive both pending record-key blocks after the authenticated client-key exchange enters the transcript.
bool TLSConnection::Context::legacy_keys(Bytes shared) {
	Secret seed(extended_master ? transcript().size() : 64); master.resize(48); // Fixed legacy master-secret width defined by the key schedule.
	if (extended_master) {if (!transcript().sum(seed.data())) return fail(INTERNAL_ERROR);}
	else {std::copy(client_random.begin(),client_random.end(),seed.begin()); std::copy(server_random.begin(),server_random.end(),seed.begin()+32);}
	if (!kdf.prf(shared,tls_text(extended_master ? "extended master secret" : "master secret"),tls_bytes(seed),master.data(),master.size())) return fail(INTERNAL_ERROR);
	seed.resize(64); std::copy(server_random.begin(),server_random.end(),seed.begin()); std::copy(client_random.begin(),client_random.end(),seed.begin()+32);
	const size_t width = cipher.mac+cipher.key+cipher.iv; Secret block(2*width), client(width), server(width);
	if (!kdf.prf(tls_bytes(master),tls_text("key expansion"),tls_bytes(seed),block.data(),block.size())) return fail(INTERNAL_ERROR);
	size_t from = 0, to = 0;
	for (size_t count : {cipher.mac,cipher.key,cipher.iv}) {std::copy_n(block.data()+from,count,client.data()+to); from += count; std::copy_n(block.data()+from,count,server.data()+to); from += count; to += count;}
	pending_read = options.server ? std::move(client) : std::move(server); pending_write = options.server ? std::move(server) : std::move(client);
	shares.clear(); peer_point.clear(); return true;
}

// Activate a pending record block once, keeping sequence ownership directional and erasing the derivation output.
bool TLSConnection::Context::legacy_install(bool read) {
	Secret &block = read ? pending_read : pending_write;
	if (block.size() != cipher.mac+cipher.key+cipher.iv) return fail(INTERNAL_ERROR);
	TLSRecord &records = read ? incoming : outgoing;
	if (!records.reset(suite,{block.data()+cipher.mac,cipher.key},{block.data()+cipher.mac+cipher.key,cipher.iv},{block.data(),cipher.mac})) return fail(INTERNAL_ERROR);
	wipe_storage(block); block.clear(); (read ? read_keys : write_keys) = true; if (read) ++read_epoch; return true;
}

// Permit the plaintext key-activation marker only after all required peer proofs have been processed.
bool TLSConnection::Context::legacy_ccs() {
	if (negotiated != 0x0303 || phase != LEGACY || legacy_step != L_CCS || read_keys) return fail(UNEXPECTED);
	if (!legacy_install(true)) return false;
	legacy_step = L_FINISHED; return true;
}

// Authenticate the legacy transcript before producing or accepting a finished record.
bool TLSConnection::Context::legacy_finish(bool send, Bytes body) {
	Secret digest(transcript().size()); std::array<uint8_t,12> expected{}; // Legacy finished width is independent of the selected transcript hash.
	const bool client = send ? !options.server : options.server;
	if (!transcript().sum(digest.data()) || !kdf.prf(tls_bytes(master),tls_text(client ? "client finished" : "server finished"),tls_bytes(digest),expected.data(),expected.size())) return fail(INTERNAL_ERROR);
	if (send) return emit(20,tls_bytes(expected));
	if (body.size != expected.size()) return fail(DECODE_ERROR);
	if (!equal(body.data,expected.data(),expected.size())) return fail(DECRYPT_ERROR);
	TLSWriter encoded; encoded.number(1,20); encoded.vector(3,body); if (!encoded.ok() || !track(encoded.view())) return fail(INTERNAL_ERROR);
	if (options.server) {const uint8_t marker[] = {1}; if (!queue(20,{marker,1}) || !legacy_install(false) || !legacy_finish(true)) return false;}
	wipe_storage(master); master.clear(); phase = READY; return true;
}

// Send client authentication and key agreement before changing the outgoing record epoch.
bool TLSConnection::Context::legacy_client_flight() {
	if (requested && !send_certificate(true)) return false;
	else if (!requested) local_certificate = false;
	auto share = std::make_unique<TLSShare>(); Secret shared;
	if (!share->reset(selected_group) || !share->agree(tls_bytes(peer_point),shared)) return fail(ILLEGAL_PARAMETER);
	TLSWriter key; key.vector(1,share->public_key());
	if (!key.ok() || !emit(16,key.view()) || !legacy_keys(tls_bytes(shared))) return false;
	if (local_certificate && !send_certificate_verify(true)) return false;
	retain_legacy = false; legacy_transcript.clear();
	const uint8_t marker[] = {1};
	if (!queue(20,{marker,1}) || !legacy_install(false) || !legacy_finish(true)) return false;
	phase = LEGACY; legacy_step = L_CCS; return true;
}

// Enforce the full legacy handshake sequence without accepting abbreviated or renegotiated flights.
bool TLSConnection::Context::legacy_message(Bytes encoded) {
	if (phase != LEGACY) return fail(UNEXPECTED);
	const uint8_t type = encoded.data[0]; const Bytes body{encoded.data+4,encoded.size-4}; TLSView reader(body);
	if (legacy_step == L_FINISHED) return type == 20 ? legacy_finish(false,body) : fail(UNEXPECTED);
	if (legacy_step == L_CERT || legacy_step == L_CLIENT_CERT) {
		if (type != 11) return fail(UNEXPECTED);
		if (!certificate(body)) return false;
		if (!options.server && !suite_key(suite,certificates.front()->key()->kind())) return fail(ILLEGAL_PARAMETER);
		phase = LEGACY; legacy_step = options.server ? L_CLIENT_KEY : L_KEY;
	} else if (legacy_step == L_KEY) {
		if (type != 12 || certificates.empty()) return fail(UNEXPECTED);
		uint32_t form, group, scheme; Bytes point, signature;
		if (!reader.number(1,form) || form != 3 || !reader.number(2,group) || !reader.vector(1,point)) return fail(DECODE_ERROR);
		const Bytes params{body.data,body.size-reader.remaining().size};
		if (!reader.number(2,scheme) || !reader.vector(2,signature) || !reader.empty()) return fail(DECODE_ERROR);
		if (std::find(options.groups.begin(),options.groups.end(),group) == options.groups.end() || std::find(tls_signatures.begin(),tls_signatures.end(),scheme) == tls_signatures.end()) return fail(ILLEGAL_PARAMETER);
		SignatureInfo algorithm; const auto *key = certificates.front()->key();
		if (!tls_signature(uint16_t(scheme),key->kind(),key->bits(),false,algorithm)) return fail(ILLEGAL_PARAMETER);
		const Buffer proof = exchange_proof(tls_bytes(client_random),tls_bytes(server_random),params);
		if (!key->verify(algorithm,tls_bytes(proof),signature)) return fail(DECRYPT_ERROR);
		selected_group = uint16_t(group); peer_point.assign(point.data,point.data+point.size); legacy_step = L_REQUEST;
	} else if (legacy_step == L_REQUEST && type == 13) {
		Bytes types, schemes, names;
		if (!reader.vector(1,types) || !types.size || !reader.vector(2,schemes) || !reader.vector(2,names) || !reader.empty()) return fail(DECODE_ERROR);
		certificate_types.assign(types.data,types.data+types.size);
		TLSWriter field; field.vector(2,schemes); if (!tls_list(field.view(),2,peer_schemes)) return fail(DECODE_ERROR);
		TLSView authorities_reader(names);
		while (!authorities_reader.empty()) {Bytes name; if (!authorities_reader.vector(2,name) || !name.size) return fail(DECODE_ERROR); authorities.emplace_back(name.data,name.data+name.size);}
		requested = true; legacy_step = L_DONE;
	} else if (legacy_step == L_REQUEST || legacy_step == L_DONE) {
		if (type != 14 || body.size) return fail(UNEXPECTED);
		if (!track(encoded)) return fail(INTERNAL_ERROR);
		return legacy_client_flight();
	} else if (legacy_step == L_CLIENT_KEY) {
		if (type != 16) return fail(UNEXPECTED);
		Bytes point; Secret shared;
		if (!reader.vector(1,point) || !reader.empty()) return fail(DECODE_ERROR);
		if (!shares.count(selected_group) || !shares.at(selected_group)->agree(point,shared)) return fail(ILLEGAL_PARAMETER);
		if (!track(encoded) || !legacy_keys(tls_bytes(shared))) return fail(INTERNAL_ERROR);
		legacy_step = certificates.empty() ? L_CCS : L_CLIENT_VERIFY;
		if (certificates.empty()) {retain_legacy = false; legacy_transcript.clear();}
		return true;
	} else if (legacy_step == L_CLIENT_VERIFY) {
		if (type != 15) return fail(UNEXPECTED);
		if (!certificate_verify(body)) return false;
		phase = LEGACY; legacy_step = L_CCS; retain_legacy = false; legacy_transcript.clear();
	} else return fail(UNEXPECTED);
	return track(encoded) ? true : fail(INTERNAL_ERROR);
}
}
