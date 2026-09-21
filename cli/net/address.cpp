// Format OS addresses and share IPv6 zone comparison with network permission checks.
#include "cli/net/address.h"
#include "cli/sys/source_error.h"
#include <cerrno>

#ifdef WINDOWS_ENABLED
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#endif

namespace {
// Return the original enumeration error with its route-operation context.
Ref<R> local_error(const char *p_call, uint32_t p_code) {
#ifdef WINDOWS_ENABLED
	SourceError::win32(p_code);
#else
	SourceError::posix(p_code);
#endif
	Dictionary info;
	info["op"] = "route";
	info["net"] = "ip+net";
	info["syscall"] = p_call;
	const Error error = SourceError::put(info, FAILED);
	return R::err(Err::make("cannot enumerate local addresses", Err::of(error), info));
}
}

// Enumerate all interface addresses, distinguishing an empty result from failure.
Ref<R> GDAddress::local() {
	SourceError::clear();
	PackedStringArray out;
	const auto add = [&](const sockaddr *addr) {
		if (!addr || (addr->sa_family != AF_INET && addr->sa_family != AF_INET6)) return;
		const String value = text(addr);
		if (!value.is_empty() && !out.has(value)) out.push_back(value);
	};
#ifdef WINDOWS_ENABLED
	ULONG size = 15000; // Initial buffer size recommended for Windows adapter enumeration.
	PackedByteArray buffer;
	for (;;) {
		if (buffer.resize(size) != OK) return R::err("cannot allocate address buffer", Err::LIMITED);
		auto *list = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.ptrw());
		const ULONG result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size);
		if (result == ERROR_BUFFER_OVERFLOW && size > uint64_t(buffer.size())) continue;
		if (result != NO_ERROR) return local_error("getadaptersaddresses", result);
		if (size == 0) break;
		for (auto *item = list; item; item = item->Next)
			for (auto *address = item->FirstUnicastAddress; address; address = address->Next) add(address->Address.lpSockaddr);
		break;
	}
#else
	ifaddrs *list = nullptr;
	if (::getifaddrs(&list) != 0) return local_error("getifaddrs", errno);
	for (auto *item = list; item; item = item->ifa_next) add(item->ifa_addr);
	::freeifaddrs(list);
#endif
	return R::ok(out);
}

// Normalize mapped IPv4 before comparing native addresses.
bool GDAddress::equal(const String &p_left, const String &p_right) {
	sockaddr_storage left, right;
	if (!parse(p_left, 0, left) || !parse(p_right, 0, right)) return false;
	return text(reinterpret_cast<const sockaddr *>(&left)) == text(reinterpret_cast<const sockaddr *>(&right));
}

// Identify native loopback addresses covered by localhost permissions.
bool GDAddress::loopback(const String &p_host) {
	sockaddr_storage addr;
	if (!parse(p_host, 0, addr)) return false;
	if (addr.ss_family == AF_INET) return ntohl(reinterpret_cast<const sockaddr_in *>(&addr)->sin_addr.s_addr) >> 24 == 127;
	const auto &v6 = reinterpret_cast<const sockaddr_in6 *>(&addr)->sin6_addr;
	return IN6_IS_ADDR_LOOPBACK(&v6) || (IN6_IS_ADDR_V4MAPPED(&v6) && v6.s6_addr[12] == 127);
}

// Preserve interface-name case and normalize numeric scopes to the same index.
bool GDAddress::zone(const String &p_name, uint32_t &r_index) {
	if (p_name.is_empty()) return false;
	bool digits = true;
	for (int i = 0; i < p_name.length(); i++) {
		if (p_name[i] == 0) return false;
		digits = digits && p_name[i] >= '0' && p_name[i] <= '9';
	}
	if (digits) {
		const int64_t value = p_name.to_int();
		if (value < 0 || value > UINT32_MAX) return false;
		r_index = uint32_t(value);
		return true;
	}
	r_index = ::if_nametoindex(p_name.utf8().get_data());
	return r_index != 0;
}

// Convert sockaddr to numeric notation, retaining mapped IPv4 and IPv6 scope.
String GDAddress::text(const sockaddr *p_addr) {
	char out[INET6_ADDRSTRLEN] = {}; // OS-defined capacity for an IPv6 literal and its terminator.
	if (p_addr->sa_family == AF_INET) {
		const auto *addr = reinterpret_cast<const sockaddr_in *>(p_addr);
		return ::inet_ntop(AF_INET, &addr->sin_addr, out, sizeof(out)) ? String(out) : String();
	}
	if (p_addr->sa_family == AF_INET6) {
		const auto *addr = reinterpret_cast<const sockaddr_in6 *>(p_addr);
		const bool mapped = IN6_IS_ADDR_V4MAPPED(&addr->sin6_addr);
		const void *bytes = mapped ? addr->sin6_addr.s6_addr + 12 : addr->sin6_addr.s6_addr;
		if (!::inet_ntop(mapped ? AF_INET : AF_INET6, bytes, out, sizeof(out))) return String();
		return addr->sin6_scope_id ? String(out) + "%" + itos(addr->sin6_scope_id) : String(out);
	}
	return String();
}

// Convert numeric IP addresses to sockaddr while preserving IPv6 scope and dual-stack behavior.
int GDAddress::parse(const String &p_host, int p_port, sockaddr_storage &r_addr, bool p_v6) {
	int r_size = 0;
	for (int i = 0; i < p_host.length(); i++) {
		if (p_host[i] == 0) return 0;
	}
	r_addr = {};
	sockaddr_in *v4 = reinterpret_cast<sockaddr_in *>(&r_addr);
	sockaddr_in6 *v6 = reinterpret_cast<sockaddr_in6 *>(&r_addr);
	const String host = p_host == "*" ? "::" : p_host;
	const CharString raw = host.utf8();
	in_addr ip4 = {};
	if (::inet_pton(AF_INET, raw.get_data(), &ip4) == 1) {
		if (p_v6) {
			v6->sin6_family = AF_INET6;
			v6->sin6_port = htons(p_port);
			v6->sin6_addr.s6_addr[10] = 0xff;
			v6->sin6_addr.s6_addr[11] = 0xff;
			memcpy(v6->sin6_addr.s6_addr + 12, &ip4, sizeof(ip4));
			r_size = sizeof(sockaddr_in6);
		} else {
			v4->sin_family = AF_INET;
			v4->sin_port = htons(p_port);
			v4->sin_addr = ip4;
			r_size = sizeof(sockaddr_in);
		}
		return r_size;
	}
	const int percent = host.find("%");
	const CharString ip = (percent < 0 ? host : host.substr(0, percent)).utf8();
	if (::inet_pton(AF_INET6, ip.get_data(), &v6->sin6_addr) != 1) {
		return 0;
	}
	if (percent >= 0) {
		// Parse through the portable type before assigning the platform's scope field.
		uint32_t scope = 0;
		if (!GDAddress::zone(host.substr(percent + 1), scope)) return 0;
		v6->sin6_scope_id = scope;
	}
	v6->sin6_family = AF_INET6;
	v6->sin6_port = htons(p_port);
	r_size = sizeof(sockaddr_in6);
	return r_size;
}
