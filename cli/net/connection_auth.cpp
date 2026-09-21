// Authenticate peer identities and finished messages before publishing application-ready key epochs.
#include "connection_state.h"
#include "handshake_protocol.h"
#include <algorithm>
#include <cstring>

namespace GDCrypto {
// Bind proof of private-key possession to the endpoint role and the pre-signature transcript.
static bool proof(const TLSHash &transcript, bool client, std::vector<uint8_t> &output) {
	const std::string_view label = client ? "TLS 1.3, client CertificateVerify" : "TLS 1.3, server CertificateVerify";
	output.assign(64,0x20); // Protocol-defined domain separation from ordinary signed messages.
	output.insert(output.end(),label.begin(),label.end()); output.push_back(0);
	const size_t at = output.size(); output.resize(at+transcript.size()); return transcript.sum(output.data()+at);
}

// Accept only offered application parameters in the encrypted negotiation message.
bool TLSConnection::Context::extensions(Bytes body) {
	TLSView reader(body); Bytes bytes; TLSExtensions values;
	if (!reader.vector(2,bytes) || !reader.empty() || !tls_extensions(bytes,values)) return fail(DECODE_ERROR);
	for (const auto &[kind,value] : values) {
		if (kind == 0) {if (!server_name || value.size) return fail(UNSUPPORTED_EXTENSION);}
		else if (kind == 16) {
			std::vector<std::string> names;
			if (!tls_protocols(value,names) || names.size() != 1) return fail(DECODE_ERROR);
			if (std::find(options.protocols.begin(),options.protocols.end(),names.front()) == options.protocols.end()) return fail(UNSUPPORTED_EXTENSION);
			application_protocol = std::move(names.front());
		} else if (kind == 10) {std::vector<uint16_t> groups; if (!tls_list(value,2,groups)) return fail(DECODE_ERROR);}
		else return fail(UNSUPPORTED_EXTENSION);
	}
	phase = SERVER_CERTIFICATE; return true;
}

// Retain a complete request without silently enabling post-handshake authentication.
bool TLSConnection::Context::request_certificate(Bytes body) {
	TLSView reader(body); Bytes context, encoded; TLSExtensions values;
	if (!reader.vector(1,context) || context.size || !reader.vector(2,encoded) || !reader.empty() || !tls_extensions(encoded,values)) return fail(DECODE_ERROR);
	auto schemes = values.find(13);
	if (schemes == values.end()) return fail(MISSING_EXTENSION);
	if (!tls_list(schemes->second,2,peer_schemes)) return fail(DECODE_ERROR);
	if (auto value = values.find(50); value != values.end()) {std::vector<uint16_t> signatures; if (!tls_list(value->second,2,signatures)) return fail(DECODE_ERROR);}
	if (auto value = values.find(47); value != values.end()) {
		TLSView field(value->second); Bytes entries;
		if (!field.vector(2,entries) || !field.empty()) return fail(DECODE_ERROR);
		TLSView names(entries);
		while (!names.empty()) {Bytes name; if (!names.vector(2,name) || !name.size) return fail(DECODE_ERROR); authorities.emplace_back(name.data,name.data+name.size);}
	}
	requested = true; return true;
}

// Parse every certificate under the handshake RSA policy before invoking the worker-side trust decision.
bool TLSConnection::Context::certificate(Bytes body) {
	TLSView reader(body); Bytes context, entries;
	if ((negotiated == 0x0304 && (!reader.vector(1,context) || context.size)) || !reader.vector(3,entries) || !reader.empty()) return fail(DECODE_ERROR);
	TLSView chain(entries); certificates.clear();
	while (!chain.empty()) {
		Bytes raw, extensions; TLSExtensions values;
		if (!chain.vector(3,raw) || !raw.size || (negotiated == 0x0304 && (!chain.vector(2,extensions) || !tls_extensions(extensions,values)))) return fail(DECODE_ERROR);
		// No certificate-entry extensions are offered, so accepting one would create unauthenticated policy ambiguity.
		if (!values.empty()) return fail(UNSUPPORTED_EXTENSION);
		auto cert = Cert::read(raw,8192); // Peer RSA verification bound applied by the consuming handshake, before precomputation.
		if (!cert) return fail(BAD_CERTIFICATE);
		certificates.push_back(std::move(cert));
	}
	if (certificates.empty()) {
		if (!options.server || options.client_auth == TLSOptions::REQUIRE || options.client_auth == TLSOptions::REQUIRE_AND_VERIFY) return fail(CERTIFICATE_REQUIRED);
		peer_verified = true; phase = CLIENT_FINISHED; return true;
	}
	if (!certificates.front()->key()) return fail(BAD_CERTIFICATE);
	const bool verify = options.server ? options.client_auth >= TLSOptions::VERIFY_IF_GIVEN : !options.insecure;
	if (verify && (!options.verify || !options.verify(certificates,!options.server))) return fail(UNKNOWN_CA);
	peer_verified = true; phase = options.server ? CLIENT_CERT_VERIFY : SERVER_CERT_VERIFY; return true;
}

// Reject a signature scheme not offered by this endpoint before verifying the role-bound transcript.
bool TLSConnection::Context::certificate_verify(Bytes body) {
	TLSView reader(body); uint32_t scheme; Bytes signature;
	if (!reader.number(2,scheme) || !reader.vector(2,signature) || !reader.empty() || !signature.size || certificates.empty()) return fail(DECODE_ERROR);
	if (std::find(tls_signatures.begin(),tls_signatures.end(),scheme) == tls_signatures.end()) return fail(ILLEGAL_PARAMETER);
	const auto *key = certificates.front()->key(); SignatureInfo algorithm; Buffer signed_bytes;
	if (!key || !tls_signature(uint16_t(scheme),key->kind(),key->bits(),negotiated == 0x0304,algorithm)) return fail(ILLEGAL_PARAMETER);
	if (negotiated == 0x0304 && !proof(transcript(),options.server,signed_bytes)) return fail(INTERNAL_ERROR);
	if (!key->verify(algorithm,negotiated == 0x0304 ? tls_bytes(signed_bytes) : tls_bytes(legacy_transcript),signature)) return fail(DECRYPT_ERROR);
	phase = options.server ? CLIENT_FINISHED : SERVER_FINISHED; return true;
}

// Send encrypted server parameters followed by optional client-auth policy and the complete server proof.
bool TLSConnection::Context::send_server_flight() {
	TLSWriter extensions, empty;
	if (server_name && !tls_extension(extensions,0,empty)) return fail(INTERNAL_ERROR);
	if (!application_protocol.empty()) {
		TLSWriter names, protocols; names.vector(1,tls_text(application_protocol)); protocols.vector(2,names.view());
		if (!names.ok() || !tls_extension(extensions,16,protocols)) return fail(INTERNAL_ERROR);
	}
	TLSWriter encrypted; encrypted.vector(2,extensions.view());
	if (!encrypted.ok() || !emit(8,encrypted.view())) return fail(INTERNAL_ERROR);
	requested = options.client_auth != TLSOptions::NONE;
	if (requested) {
		TLSWriter request, extensions, schemes, signatures;
		for (uint16_t scheme : tls_signatures) schemes.number(2,scheme);
		signatures.vector(2,schemes.view()); if (!schemes.ok() || !tls_extension(extensions,13,signatures)) return fail(INTERNAL_ERROR);
		if (!options.authorities.empty()) {
			TLSWriter names, authorities; for (const auto &name : options.authorities) names.vector(2,tls_bytes(name)); authorities.vector(2,names.view());
			if (!names.ok() || !tls_extension(extensions,47,authorities)) return fail(INTERNAL_ERROR);
		}
		request.vector(1,{}); request.vector(2,extensions.view());
		if (!request.ok() || !emit(13,request.view())) return fail(INTERNAL_ERROR);
	}
	if (!send_certificate(false) || !send_certificate_verify(false) || !send_finished() || !application_keys() || !install(std::move(pending_write),false)) return false;
	phase = requested ? CLIENT_CERTIFICATE : CLIENT_FINISHED; return true;
}

// Select a requested identity only when its certificate chain and signing scheme satisfy the peer request.
bool TLSConnection::Context::send_certificate(bool client) {
	local_certificate = bool(options.identity);
	if (client && local_certificate) {
		const auto &key = options.identity->signer(); bool compatible = false;
		for (uint16_t scheme : peer_schemes) {SignatureInfo algorithm; if (tls_signature(scheme,key.kind(),key.bits(),negotiated == 0x0304,algorithm)) {compatible = true; break;}}
		if (negotiated == 0x0303) {const uint8_t family = key.kind() == KeyKind::RSA ? 1 : 64; compatible &= std::find(certificate_types.begin(),certificate_types.end(),family) != certificate_types.end();}
		if (!authorities.empty()) {
			bool found = false;
			for (const auto &cert : options.identity->chain()) for (const auto &name : authorities) {
				const Bytes issuer = cert->info().issuer;
				if (issuer.size == name.size() && !std::memcmp(issuer.data,name.data(),name.size())) found = true;
			}
			compatible &= found;
		}
		local_certificate = compatible;
	}
	TLSWriter chain, body;
	if (local_certificate) for (const auto &cert : options.identity->chain()) {chain.vector(3,cert->info().raw); if (negotiated == 0x0304) chain.vector(2,{});}
	if (!chain.ok()) return fail(INTERNAL_ERROR);
	if (negotiated == 0x0304) body.vector(1,{}); body.vector(3,chain.view());
	return body.ok() ? emit(11,body.view()) : fail(INTERNAL_ERROR);
}

// Choose a mutually supported key-compatible signature and emit its complete proof atomically.
bool TLSConnection::Context::send_certificate_verify(bool client) {
	if (!local_certificate || !options.identity) return fail(INTERNAL_ERROR);
	const auto &key = options.identity->signer(); uint16_t selected = 0; SignatureInfo algorithm;
	for (uint16_t scheme : tls_signatures) if (std::find(peer_schemes.begin(),peer_schemes.end(),scheme) != peer_schemes.end() && tls_signature(scheme,key.kind(),key.bits(),negotiated == 0x0304,algorithm)) {selected = scheme; break;}
	if (!selected) return fail(HANDSHAKE_FAILURE);
	Buffer signed_bytes, signature;
	if (negotiated == 0x0304 && !proof(transcript(),client,signed_bytes)) return fail(INTERNAL_ERROR);
	if (!key.sign(algorithm,negotiated == 0x0304 ? tls_bytes(signed_bytes) : tls_bytes(legacy_transcript),signature)) return fail(INTERNAL_ERROR);
	TLSWriter body; body.number(2,selected); body.vector(2,tls_bytes(signature));
	return body.ok() ? emit(15,body.view()) : fail(INTERNAL_ERROR);
}

// Authenticate all prior handshake bytes under the current outgoing handshake traffic secret.
bool TLSConnection::Context::send_finished() {
	Secret value(kdf.size());
	if (!kdf.finished(tls_bytes(write_secret),transcript(),value.data())) return fail(INTERNAL_ERROR);
	return emit(20,tls_bytes(value));
}

// Advance key epochs only after an exact constant-work finished verification and transcript checkpoint.
bool TLSConnection::Context::finished(Bytes body) {
	Secret expected(kdf.size());
	if (body.size != expected.size()) return fail(DECODE_ERROR);
	if (!kdf.finished(tls_bytes(read_secret),transcript(),expected.data())) return fail(INTERNAL_ERROR);
	if (!equal(body.data,expected.data(),expected.size())) return fail(DECRYPT_ERROR);
	TLSWriter encoded; encoded.number(1,20); encoded.vector(3,body);
	if (!encoded.ok() || !track(encoded.view())) return fail(INTERNAL_ERROR);
	if (options.server) {if (!install(std::move(pending_read),true)) return false;}
	else {
		if (!application_keys() || !install(std::move(pending_read),true)) return false;
		if (requested && (!send_certificate(true) || (local_certificate && !send_certificate_verify(true)))) return false;
		if (!send_finished() || !install(std::move(pending_write),false)) return false;
	}
	phase = READY; return true;
}
}
