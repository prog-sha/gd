// Negotiate hello parameters and bind fresh key agreement to the exact authenticated transcript.
#include "connection_state.h"
#include "handshake_protocol.h"
#include "cli/data/hash_core.h"
#include "ip.h"
#include <algorithm>
#include <cstring>

namespace GDCrypto {
// Compare public protocol identifiers without normalizing their original wire representation.
static bool same(Bytes a, Bytes b) { return a.size == b.size && (!a.size || !std::memcmp(a.data,b.data,a.size)); }

// Test membership without assigning meaning to unsupported peer identifiers.
static bool offered(const std::vector<uint16_t> &values, uint16_t value) { return std::find(values.begin(),values.end(),value) != values.end(); }

// Encode the configured client negotiation and retain stable retry fields without retaining arbitrary handshake history.
bool TLSConnection::Context::client_hello() {
	if (!retried) {
		if (!random_fill(client_random.data(),client_random.size())) return fail(INTERNAL_ERROR);
		session.resize(32); // Compatibility session identifier has the protocol's maximum encoded width.
		if (!random_fill(session.data(),session.size())) return fail(INTERNAL_ERROR);
	}
	TLSWriter body, extensions, suites, groups, signatures, versions, shares_wire;
	for (uint16_t value : options.suites) {TLSCipher config; TLSCipher::find(value,config); if ((config.modern && options.maximum >= 0x0304) || (!config.modern && options.minimum <= 0x0303)) suites.number(2,value);}
	for (uint16_t value : options.groups) groups.number(2,value);
	for (uint16_t value : tls_signatures) signatures.number(2,value);
	for (uint16_t value = options.maximum; value >= options.minimum; --value) versions.number(2,value);
	if (!suites.view().size || !suites.ok() || !groups.ok() || !signatures.ok() || !versions.ok()) return fail(INTERNAL_ERROR);
	TLSWriter group_list, signature_list, version_list, formats, renegotiation, empty;
	group_list.vector(2,groups.view()); signature_list.vector(2,signatures.view()); version_list.vector(1,versions.view());
	const uint8_t zero[] = {0}; formats.vector(1,{zero,1}); renegotiation.vector(1,{});
	if (!tls_extension(extensions,10,group_list) || !tls_extension(extensions,13,signature_list) || !tls_extension(extensions,11,formats) ||
			!tls_extension(extensions,23,empty) || !tls_extension(extensions,0xff01,renegotiation)) return fail(INTERNAL_ERROR);
	// Address literals are trust inputs, not DNS names for virtual-host routing.
	std::string host = options.hostname;
	while (!host.empty() && host.back() == '.') host.pop_back();
	server_name = !host.empty() && host.find(':') == host.npos && host.front() != '[' && !GDIP::parse(host).bit_len();
	if (server_name) {
		TLSWriter name, names; name.number(1,0); name.vector(2,tls_text(host)); names.vector(2,name.view());
		if (!name.ok() || !tls_extension(extensions,0,names)) return fail(INTERNAL_ERROR);
	}
	if (!options.protocols.empty()) {
		TLSWriter names, protocols; for (const auto &name : options.protocols) names.vector(1,tls_text(name)); protocols.vector(2,names.view());
		if (!names.ok() || !tls_extension(extensions,16,protocols)) return fail(INTERNAL_ERROR);
	}
	if (options.maximum >= 0x0304) {
		const uint16_t group = retried && selected_group ? selected_group : options.groups.front();
		if (!retried || selected_group) {
			auto share = std::make_unique<TLSShare>(); if (!share->reset(group)) return fail(INTERNAL_ERROR);
			shares.clear(); shares.emplace(group,std::move(share));
		}
		if (!shares.count(group)) return fail(INTERNAL_ERROR);
		shares_wire.number(2,group); shares_wire.vector(2,shares.at(group)->public_key());
		TLSWriter list; list.vector(2,shares_wire.view());
		if (!shares_wire.ok() || !tls_extension(extensions,43,version_list) || !tls_extension(extensions,51,list)) return fail(INTERNAL_ERROR);
	}
	if (!cookie.empty()) {TLSWriter value; value.vector(2,tls_bytes(cookie)); if (!tls_extension(extensions,44,value)) return fail(INTERNAL_ERROR);}
	body.number(2,0x0303); body.append(tls_bytes(client_random)); body.vector(1,tls_bytes(session)); body.vector(2,suites.view()); body.vector(1,{zero,1}); body.vector(2,extensions.view());
	if (!body.ok()) return fail(INTERNAL_ERROR);
	if (!retried) initial_hello.assign(body.view().data,body.view().data+body.view().size);
	return emit(1,body.view());
}

// Validate a client hello, including retry immutability, before producing a fresh server share.
bool TLSConnection::Context::accept_hello(Bytes message) {
	TLSHello hello; if (!TLSHello::read(message,true,hello)) return fail(DECODE_ERROR);
	std::vector<uint16_t> versions, suites, groups, schemes;
	TLSWriter list; list.vector(2,hello.suites);
	if (!tls_list(list.view(),2,suites)) return fail(DECODE_ERROR);
	if (auto value = hello.extensions.find(43); value != hello.extensions.end()) {if (!tls_list(value->second,1,versions)) return fail(DECODE_ERROR);}
	else if (hello.version >= 0x0303) versions.push_back(0x0303);
	uint16_t version = 0;
	for (uint16_t value = options.maximum; value >= options.minimum; --value) if (offered(versions,value)) {version = value; break;}
	if (!version) return fail(PROTOCOL_VERSION);
	if (version == 0x0303 && offered(suites,0x5600) && hello.version < options.maximum) return fail(INAPPROPRIATE_FALLBACK);
	if (retried && version != negotiated) return fail(ILLEGAL_PARAMETER);
	negotiated = version;
	if (hello.version != 0x0303 || !std::memchr(hello.compression.data,0,hello.compression.size) || (version == 0x0304 && hello.compression.size != 1)) return fail(ILLEGAL_PARAMETER);
	if (auto value = hello.extensions.find(10); value != hello.extensions.end()) {if (!tls_list(value->second,2,groups)) return fail(DECODE_ERROR);}
	if (auto value = hello.extensions.find(13); value != hello.extensions.end()) {if (!tls_list(value->second,2,schemes)) return fail(DECODE_ERROR);}
	if (schemes.empty()) return fail(MISSING_EXTENSION);
	peer_schemes = std::move(schemes);
	if (negotiated == 0x0303) return legacy_hello(message);
	if (hello.extensions.count(42)) return fail(UNSUPPORTED_EXTENSION);
	if (auto value = hello.extensions.find(0); value != hello.extensions.end()) {
		TLSView names(value->second); Bytes entries, name; uint32_t type;
		if (!names.vector(2,entries) || !names.empty()) return fail(DECODE_ERROR);
		TLSView entry(entries); if (!entry.number(1,type) || type || !entry.vector(2,name) || !entry.empty() || !name.size || std::memchr(name.data,0,name.size) || name.data[name.size-1] == '.') return fail(ILLEGAL_PARAMETER);
		server_name = true;
	}
	if (auto value = hello.extensions.find(16); value != hello.extensions.end()) {
		std::vector<std::string> protocols; if (!tls_protocols(value->second,protocols)) return fail(DECODE_ERROR);
		for (const auto &name : options.protocols) if (std::find(protocols.begin(),protocols.end(),name) != protocols.end()) {application_protocol = name; break;}
		if (!options.protocols.empty() && application_protocol.empty()) return fail(NO_APPLICATION_PROTOCOL);
	}
	uint16_t choice = 0;
	for (uint16_t value : options.suites) if (value >= 0x1301 && value <= 0x1303 && offered(suites,value)) {choice = value; break;}
	if (!choice) return fail(HANDSHAKE_FAILURE);
	if (retried && choice != suite) return fail(ILLEGAL_PARAMETER);
	suite = choice; TLSCipher::find(suite,cipher); kdf = TLSKDF(cipher.sha384);
	std::map<uint16_t,Bytes> public_shares;
	if (auto value = hello.extensions.find(51); value != hello.extensions.end()) {
		TLSView field(value->second); Bytes items;
		if (!field.vector(2,items) || !field.empty()) return fail(DECODE_ERROR);
		TLSView entries(items);
		while (!entries.empty()) {uint32_t group; Bytes key; if (!entries.number(2,group) || !entries.vector(2,key) || !key.size || !offered(groups,uint16_t(group)) || !public_shares.emplace(uint16_t(group),key).second) return fail(ILLEGAL_PARAMETER);}
	} else return fail(MISSING_EXTENSION);
	if (retried) {
		TLSHello first; if (!TLSHello::read(tls_bytes(initial_hello),true,first)) return fail(INTERNAL_ERROR);
		if (!same(first.random,hello.random) || !same(first.session,hello.session) || !same(first.suites,hello.suites) || public_shares.size() != 1 || !public_shares.count(selected_group)) return fail(ILLEGAL_PARAMETER);
		for (const auto &[kind,value] : first.extensions) if (kind != 51 && kind != 21 && kind != 41 && kind != 42 && kind != 44) {
			auto other = hello.extensions.find(kind); if (other == hello.extensions.end() || !same(value,other->second)) return fail(ILLEGAL_PARAMETER);
		}
		for (const auto &[kind,value] : hello.extensions) if (!first.extensions.count(kind) && kind != 44 && kind != 21) return fail(ILLEGAL_PARAMETER);
	} else {
		initial_hello.assign(message.data,message.data+message.size); session.assign(hello.session.data,hello.session.data+hello.session.size);
		std::copy_n(hello.random.data,32,client_random.begin());
		for (uint16_t group : options.groups) if (offered(groups,group)) {selected_group = group; break;}
		if (!selected_group) return fail(HANDSHAKE_FAILURE);
		// Prefer a mutually supported supplied share before spending a round trip on a different preference.
		for (uint16_t group : options.groups) if (public_shares.count(group)) {selected_group = group; break;}
	}
	// The hello itself enters the transcript only after its version and retry checks succeed.
	TLSWriter encoded; encoded.number(1,1); encoded.vector(3,message); if (!encoded.ok() || !track(encoded.view())) return fail(INTERNAL_ERROR);
	TLSWriter extensions, supported, key;
	supported.number(2,0x0304); if (!tls_extension(extensions,43,supported)) return fail(INTERNAL_ERROR);
	const bool retry = !public_shares.count(selected_group);
	if (retry) {
		if (retried) return fail(ILLEGAL_PARAMETER);
		retried = true; transcript().retry(); key.number(2,selected_group);
	} else {
		if (!random_fill(server_random.data(),server_random.size())) return fail(INTERNAL_ERROR);
		auto share = std::make_unique<TLSShare>(); if (!share->reset(selected_group)) return fail(INTERNAL_ERROR);
		key.number(2,selected_group); key.vector(2,share->public_key()); shares.emplace(selected_group,std::move(share));
	}
	if (!tls_extension(extensions,51,key)) return fail(INTERNAL_ERROR);
	TLSWriter body; body.number(2,0x0303); body.append(retry ? tls_bytes(tls_retry_random) : tls_bytes(server_random)); body.vector(1,tls_bytes(session)); body.number(2,suite); body.number(1,0); body.vector(2,extensions.view());
	if (!body.ok() || !emit(2,body.view())) return fail(INTERNAL_ERROR);
	if (retry) return true;
	Secret shared;
	if (!shares.at(selected_group)->agree(public_shares.at(selected_group),shared)) return fail(ILLEGAL_PARAMETER);
	if (!handshake_keys(tls_bytes(shared))) return false;
	initial_hello.clear(); return send_server_flight();
}

// Reject unsolicited server choices and complete the hello transcript before deriving either key direction.
bool TLSConnection::Context::accept_server_hello(Bytes message) {
	TLSHello hello; if (!TLSHello::read(message,false,hello)) return fail(DECODE_ERROR);
	uint16_t version = hello.version;
	if (auto value = hello.extensions.find(43); value != hello.extensions.end()) {TLSView field(value->second); uint32_t v; if (!field.number(2,v) || !field.empty()) return fail(DECODE_ERROR); version = uint16_t(v);}
	if (version < options.minimum || version > options.maximum) return fail(PROTOCOL_VERSION);
	if (retried && (version != negotiated || hello.suite != suite)) return fail(ILLEGAL_PARAMETER);
	if (!offered(options.suites,hello.suite) || !TLSCipher::find(hello.suite,cipher) || cipher.modern != (version == 0x0304)) return fail(ILLEGAL_PARAMETER);
	negotiated = version; suite = hello.suite; kdf = TLSKDF(cipher.sha384);
	if (hello.version != 0x0303 || hello.compression.data[0]) return fail(ILLEGAL_PARAMETER);
	if (version == 0x0303) return legacy_hello(message);
	if (!same(hello.session,tls_bytes(session))) return fail(ILLEGAL_PARAMETER);
	const bool retry = same(hello.random,tls_bytes(tls_retry_random));
	for (const auto &[kind,value] : hello.extensions) if (kind != 43 && kind != 51 && !(retry && kind == 44)) return fail(UNSUPPORTED_EXTENSION);
	auto key = hello.extensions.find(51);
	if (key == hello.extensions.end() && !retry) return fail(MISSING_EXTENSION);
	TLSView field(key == hello.extensions.end() ? Bytes{} : key->second); uint32_t group = 0; Bytes point;
	if (key != hello.extensions.end() && (!field.number(2,group) || !offered(options.groups,uint16_t(group)))) return fail(ILLEGAL_PARAMETER);
	selected_group = uint16_t(group);
	if (retry) {
		if (retried || !field.empty() || shares.count(selected_group)) return fail(ILLEGAL_PARAMETER);
		if (auto value = hello.extensions.find(44); value != hello.extensions.end()) {TLSView value_reader(value->second); Bytes value_bytes; if (!value_reader.vector(2,value_bytes) || !value_reader.empty() || !value_bytes.size) return fail(DECODE_ERROR); cookie.assign(value_bytes.data,value_bytes.data+value_bytes.size);}
		if (!selected_group && cookie.empty()) return fail(ILLEGAL_PARAMETER);
		retried = true; transcript().retry();
	} else if (!field.vector(2,point) || !field.empty() || !shares.count(selected_group)) return fail(ILLEGAL_PARAMETER);
	TLSWriter encoded; encoded.number(1,2); encoded.vector(3,message); if (!encoded.ok() || !track(encoded.view())) return fail(INTERNAL_ERROR);
	if (retry) return client_hello();
	Secret shared;
	if (!shares.at(selected_group)->agree(point,shared)) return fail(ILLEGAL_PARAMETER);
	std::copy_n(hello.random.data,32,server_random.begin());
	if (!handshake_keys(tls_bytes(shared))) return false;
	initial_hello.clear(); phase = ENCRYPTED_EXTENSIONS; return true;
}
}
