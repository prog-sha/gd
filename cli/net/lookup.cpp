// Delegate name resolution to the OS, preserving address order and failure details.
#include "cli/net/lookup.h"
#include "cli/net/address.h"
#include "core/templates/hash_set.h"

#ifdef WINDOWS_ENABLED
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

// Resolve on a worker and return all address candidates in OS order.
Ref<R> GDLookup::all(const String &p_host) {
	if (p_host.is_empty()) return R::err("host must not be empty", Err::INVALID_DATA);
	for (int i = 0; i < p_host.length(); i++) {
		if (p_host[i] == 0) return R::err("host contains NUL", Err::INVALID_DATA);
	}
	sockaddr_storage literal;
	if (p_host != "*" && GDAddress::parse(p_host, 0, literal)) {
		PackedStringArray addresses;
		addresses.push_back(GDAddress::text(reinterpret_cast<sockaddr *>(&literal)));
		return R::ok(addresses);
	}
#ifdef WINDOWS_ENABLED
	ADDRINFOW hints = {};
	ADDRINFOW *found = nullptr;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	const Char16String host = p_host.utf16();
	const int error = ::GetAddrInfoW(reinterpret_cast<const wchar_t *>(host.get_data()), nullptr, &hints, &found);
#else
	addrinfo hints = {};
	addrinfo *found = nullptr;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_CANONNAME;
#if defined(AI_V4MAPPED) && defined(AI_ALL) && !defined(__OpenBSD__) && !defined(__NetBSD__)
	hints.ai_flags |= AI_V4MAPPED | AI_ALL;
#endif
#ifdef AI_MASK
	hints.ai_flags &= AI_MASK;
#endif
	const int error = ::getaddrinfo(p_host.utf8().get_data(), nullptr, &hints, &found);
#endif
	if (error) {
		Dictionary info;
		info["host"] = p_host;
		info["resolver_error"] = error;
		info["temporary"] = error == EAI_AGAIN;
		return R::err(Err::make(vformat("cannot resolve %s (resolver error %d)", p_host, error),
				error == EAI_NONAME ? Err::NOT_FOUND : Err::NONE, info));
	}
	PackedStringArray addresses;
	HashSet<String> seen; // Deduplicate resolver results in time proportional to the number of candidates.
	for (auto *at = found; at; at = at->ai_next) {
		if (!at->ai_addr || at->ai_socktype != SOCK_STREAM) continue;
		const String address = GDAddress::text(at->ai_addr);
		if (!address.is_empty() && !seen.has(address)) {
			seen.insert(address);
			addresses.push_back(address);
		}
	}
#ifdef WINDOWS_ENABLED
	if (found) ::FreeAddrInfoW(found);
#else
	if (found) ::freeaddrinfo(found);
#endif
	return addresses.is_empty() ? R::err(vformat("no address for %s", p_host), Err::NOT_FOUND) : R::ok(addresses);
}
