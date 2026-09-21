// Reassemble bounded handshake messages and enforce authenticated record ordering across key epochs.
#include "connection_state.h"
#include <algorithm>

namespace GDCrypto {
// Dispatch a complete message under one explicit phase; authentication proofs consume their prior transcript.
bool TLSConnection::Context::message(Bytes encoded) {
	const uint8_t type = encoded.data[0]; const Bytes body{encoded.data+4,encoded.size-4};
	if (phase == READY) return post_handshake(type,body);
	if (phase == CLIENT_HELLO) return type == 1 ? accept_hello(body) : fail(UNEXPECTED);
	if (phase == SERVER_HELLO) return type == 2 ? accept_server_hello(body) : fail(UNEXPECTED);
	if (negotiated == 0x0303) return legacy_message(encoded);
	bool accepted = false;
	switch (phase) {
		case ENCRYPTED_EXTENSIONS: if (type == 8) accepted = extensions(body); break;
		case SERVER_CERTIFICATE:
			if (type == 13 && !requested) accepted = request_certificate(body);
			else if (type == 11) accepted = certificate(body);
			break;
		case CLIENT_CERTIFICATE: if (type == 11) accepted = certificate(body); break;
		case SERVER_CERT_VERIFY: case CLIENT_CERT_VERIFY: if (type == 15) accepted = certificate_verify(body); break;
		case SERVER_FINISHED: case CLIENT_FINISHED: return type == 20 ? finished(body) : fail(UNEXPECTED);
		default: break;
	}
	if (!accepted) return failure ? false : fail(UNEXPECTED);
	return track(encoded) ? true : fail(INTERNAL_ERROR);
}

// Inspect the four-byte message header before allocating its body, preserving message-specific receive policy.
bool TLSConnection::Context::handshake(Bytes fragment) {
	size_t at = 0;
	while (at < fragment.size) {
		if (hand.size() < 4) {
			const size_t count = std::min<size_t>(4-hand.size(),fragment.size-at);
			hand.insert(hand.end(),fragment.data+at,fragment.data+at+count); at += count;
			if (hand.size() < 4) return true;
		}
		const size_t size = size_t(hand[1])<<16 | size_t(hand[2])<<8 | hand[3];
		const size_t limit = negotiated && hand[0] == 11 ? 262144 : 65536; // Message-specific peer receive policy, applied before body allocation.
		if (size > limit) return fail(INTERNAL_ERROR);
		const size_t count = std::min(size+4-hand.size(),fragment.size-at);
		hand.insert(hand.end(),fragment.data+at,fragment.data+at+count); at += count;
		if (hand.size() != size+4) return true;
		Buffer encoded = std::move(hand); hand.clear(); const uint64_t epoch = read_epoch;
		if (!message(tls_bytes(encoded))) return false;
		if (read_epoch != epoch && at != fragment.size) return fail(UNEXPECTED);
	}
	return true;
}

// Keep post-handshake notices out of the finished transcript and advance update keys in their directional order.
bool TLSConnection::Context::post_handshake(uint8_t type, Bytes body) {
	if (++idle_records > 16) return fail(UNEXPECTED); // Consecutive non-advancing message policy, reset by nonempty application input.
	if (negotiated != 0x0304) return fail(UNEXPECTED);
	if (type == 4 && !options.server) {
		TLSView reader(body); uint32_t lifetime, age; Bytes nonce, ticket, encoded; TLSExtensions extensions;
		if (!reader.number(4,lifetime) || !reader.number(4,age) || !reader.vector(1,nonce) || !reader.vector(2,ticket) || !ticket.size || !reader.vector(2,encoded) || !reader.empty() || !tls_extensions(encoded,extensions)) return fail(DECODE_ERROR);
		if (auto value = extensions.find(42); value != extensions.end()) {TLSView field(value->second); uint32_t maximum; if (!field.number(4,maximum) || !field.empty()) return fail(DECODE_ERROR);}
		return true;
	}
	if (type != 24) return fail(UNEXPECTED);
	if (body.size != 1 || body.data[0] > 1) return fail(ILLEGAL_PARAMETER);
	Secret fresh(kdf.size());
	if (!kdf.expand(tls_bytes(read_secret),tls_text("traffic upd"),{},fresh.data(),fresh.size()) || !install(std::move(fresh),true)) return fail(INTERNAL_ERROR);
	if (body.data[0]) {
		const uint8_t update[] = {24,0,0,1,0}; // Reply under the current outgoing epoch before deriving its successor.
		if (!queue(22,{update,sizeof(update)})) return fail(INTERNAL_ERROR);
		fresh.resize(kdf.size());
		if (!kdf.expand(tls_bytes(write_secret),tls_text("traffic upd"),{},fresh.data(),fresh.size()) || !install(std::move(fresh),false)) return fail(INTERNAL_ERROR);
	}
	return true;
}

// Validate public record fields before any peer-controlled payload allocation or incomplete-body wait.
bool TLSConnection::Context::record_size(Bytes header, size_t &output) {
	if (phase == NEW || phase == FAILED || phase == CLOSED || read_closed) return false;
	if (!header.data || header.size != 5) return fail(DECODE_ERROR);
	const uint8_t type = header.data[0]; const uint16_t version = uint16_t(header.data[1])<<8 | header.data[2];
	const size_t size = size_t(header.data[3])<<8 | header.data[4];
	if (negotiated ? version != 0x0303 : ((type != 21 && type != 22) || version >= 0x1000)) return fail(PROTOCOL_VERSION);
	if (size > 18432 || (negotiated == 0x0304 && size > 16640)) return fail(RECORD_OVERFLOW);
	output = size; return true;
}

// Authenticate one complete framed record and publish plaintext only after the configured handshake succeeds.
bool TLSConnection::Context::receive(Bytes record, Buffer &application) {
	if (!record.data || record.size < 5) return fail(DECODE_ERROR);
	size_t size = 0; if (!record_size({record.data,5},size)) return false;
	if (size != record.size-5) return fail(DECODE_ERROR);
	uint8_t type = record.data[0];
	Buffer plain; Bytes payload{record.data+5,size};
	const bool compatibility = negotiated == 0x0304 && type == 20;
	if (read_keys && !compatibility) {if (!incoming.open(record,type,plain)) return fail(incoming.alert()); payload = tls_bytes(plain);}
	if (payload.size > 16384) return fail(RECORD_OVERFLOW);
	if (type == 23 && !read_keys) return fail(UNEXPECTED);
	if (negotiated == 0x0304 && type != 22 && !hand.empty()) return fail(UNEXPECTED);
	if ((type == 23 || (type == 22 && phase != READY)) && payload.size) idle_records = 0;
	if (type == 22) {
		if (!payload.size) return fail(UNEXPECTED);
		return handshake(payload);
	}
	if (type == 23) {
		if (phase != READY || !peer_verified) return fail(UNEXPECTED);
		if (payload.size) {application.insert(application.end(),payload.data,payload.data+payload.size); return true;}
	} else if (type == 20) {
		if (payload.size != 1 || payload.data[0] != 1) return fail(DECODE_ERROR);
		if (!hand.empty()) return fail(UNEXPECTED);
		if (!compatibility) return legacy_ccs();
	} else if (type == 21) {
		if (payload.size != 2) return fail(UNEXPECTED);
		if (!payload.data[1]) {read_closed = true; return true;}
		if (negotiated == 0x0304) {if (payload.data[1] != 90) return fail(payload.data[1],false);}
		else if (payload.data[0] == 2) return fail(payload.data[1],false);
		else if (payload.data[0] != 1) return fail(UNEXPECTED);
	} else return fail(UNEXPECTED);
	return ++idle_records > 16 ? fail(UNEXPECTED) : true;
}
}
