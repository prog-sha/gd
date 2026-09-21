// Use nonblocking TCP system calls and distinguish readiness waits from communication failures.
#include "cli/net/stream.h"
#include "cli/net/address.h"
#include "cli/sys/fork.h"

#include <cstdio>
#include <cstring>
#ifdef WINDOWS_ENABLED
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#else
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <unistd.h>
#ifdef MACOS_ENABLED
#include <sys/sysctl.h>
#endif
#endif

namespace {
#ifdef WINDOWS_ENABLED
using Socket = SOCKET; // Winsock handle width.
using AddrSize = int; // Winsock address length.
#else
using Socket = int; // POSIX descriptor
using AddrSize = socklen_t; // POSIX address length.
#endif

// Identify an in-progress connection or an interruption that permits retry.
bool connecting(int p_error) {
#ifdef WINDOWS_ENABLED
	return p_error == WSAEWOULDBLOCK || p_error == WSAEINPROGRESS || p_error == WSAEALREADY || p_error == WSAEINTR;
#else
	return p_error == EINPROGRESS || p_error == EALREADY || p_error == EINTR;
#endif
}

// Treat an already-connected OS result as an ordinary successful connection.
bool connected(int p_error) {
#ifdef WINDOWS_ENABLED
	return p_error == WSAEISCONN;
#else
	return p_error == EISCONN;
#endif
}

// Read the OS listen-queue setting, falling back to its constant only when the query fails.
int backlog() {
	static const int count = []() { // Read the OS backlog setting once.
		int value = SOMAXCONN;
#if defined(LINUXBSD_ENABLED) && defined(__linux__)
		if (FILE *file = ::fopen("/proc/sys/net/core/somaxconn", "r")) {
			int configured = 0;
			if (::fscanf(file, "%d", &configured) == 1 && configured > 0) value = configured;
			::fclose(file);
		}
		if (value > UINT16_MAX) {
			utsname system = {};
			int major = 0;
			int minor = 0;
			if (::uname(&system) != 0 || ::sscanf(system.release, "%d.%d", &major, &minor) != 2 ||
					major < 4 || (major == 4 && minor < 1)) value = UINT16_MAX;
		}
#elif defined(MACOS_ENABLED)
		size_t size = sizeof(value);
		if (::sysctlbyname("kern.ipc.somaxconn", &value, &size, nullptr, 0) != 0 || value <= 0) value = SOMAXCONN;
		value = MIN(value, int(UINT16_MAX)); // Avoid overflowing the BSD backlog's 16-bit representation.
#endif
		return value;
	}();
	return count;
}
}

// Send small writes promptly and enable the default keepalive policy.
void GDStream::defaults() {
	const Socket socket = Socket(fd);
	const int on = 1; // Enable TCP_NODELAY and keepalive.
	const int idle = 15; // Idle seconds before the first keepalive probe.
	const int interval = 15; // Seconds between keepalive probes.
	const int probes = 9; // Number of unanswered keepalive probes.
	::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof(on));
	::setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char *>(&on), sizeof(on));
#ifdef WINDOWS_ENABLED
	tcp_keepalive keep{ 1, ULONG(idle * 1000), ULONG(interval * 1000) };
	DWORD returned = 0;
	::WSAIoctl(socket, SIO_KEEPALIVE_VALS, &keep, sizeof(keep), nullptr, 0, &returned, nullptr, nullptr);
#else
#ifdef TCP_KEEPIDLE
	::setsockopt(socket, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#elif defined(TCP_KEEPALIVE)
	::setsockopt(socket, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
#endif
	::setsockopt(socket, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
#endif
#ifdef TCP_KEEPCNT
	::setsockopt(socket, IPPROTO_TCP, TCP_KEEPCNT, reinterpret_cast<const char *>(&probes), sizeof(probes));
#else
	(void)probes;
#endif
}

// Start a numeric-address connection and leave asynchronous completion to write monitoring.
Error GDStream::dial(const String &p_host, int p_port) {
	close();
	eof = false;
	dial_error = 0;
	sockaddr_storage address;
	const AddrSize size = GDAddress::parse(p_host, p_port, address);
	if (!size) return ERR_INVALID_PARAMETER;
	const Error opened = open_socket(address.ss_family, SOCK_STREAM);
	if (opened != OK) return opened;
	state = CONNECTING;
	if (::connect(Socket(fd), reinterpret_cast<sockaddr *>(&address), size) == 0) {
		state = CONNECTED;
		defaults();
		return OK;
	}
	const int error = last_error();
	dial_error = error;
	if (connected(error)) {
		state = CONNECTED;
		defaults();
		return OK;
	}
	if (connecting(error)) return OK;
	close();
	return failure(error);
}

// Confirm actual connection establishment even if socket readiness arrives early.
void GDStream::poll() {
	if (status() != CONNECTING) return;
	int error = 0;
	AddrSize size = sizeof(error);
	if (::getsockopt(Socket(fd), SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size) != 0) {
		dial_error = last_error();
		state = BROKEN;
		return;
	}
	if (connecting(error)) return;
	if (connected(error)) {
		state = CONNECTED;
		defaults();
		return;
	}
	if (error) {
		dial_error = error;
		state = BROKEN;
		return;
	}
	sockaddr_storage address;
	size = sizeof(address);
	if (::getpeername(Socket(fd), reinterpret_cast<sockaddr *>(&address), &size) == 0) {
		state = CONNECTED;
		defaults();
	}
}

// Retry only local-port selection failures and rare self-connections.
bool GDStream::retryable() const {
#ifdef WINDOWS_ENABLED
	if (dial_error == WSAEADDRNOTAVAIL) return true;
#else
	if (dial_error == EADDRNOTAVAIL) return true;
#endif
	if (status() != CONNECTED) return false;
	sockaddr_storage local = {};
	sockaddr_storage remote = {};
	AddrSize size = sizeof(local);
	if (::getsockname(Socket(fd), reinterpret_cast<sockaddr *>(&local), &size) != 0) return true;
	size = sizeof(remote);
	if (::getpeername(Socket(fd), reinterpret_cast<sockaddr *>(&remote), &size) != 0) return true;
	if (local.ss_family != remote.ss_family) return false;
	if (local.ss_family == AF_INET) {
		const auto &a = reinterpret_cast<const sockaddr_in &>(local);
		const auto &b = reinterpret_cast<const sockaddr_in &>(remote);
		return a.sin_port == b.sin_port && a.sin_addr.s_addr == b.sin_addr.s_addr;
	}
	const auto &a = reinterpret_cast<const sockaddr_in6 &>(local);
	const auto &b = reinterpret_cast<const sockaddr_in6 &>(remote);
	return a.sin6_port == b.sin6_port && memcmp(&a.sin6_addr, &b.sin6_addr, sizeof(a.sin6_addr)) == 0;
}

// Listen using the OS backlog setting rather than an application connection-count cap.
Error GDStream::listen(const String &p_host, int p_port, bool p_reuse) {
	close();
	sockaddr_storage address;
	const AddrSize size = GDAddress::parse(p_host, p_port, address);
	if (!size) return ERR_INVALID_PARAMETER;
	const Error opened = open_socket(address.ss_family, SOCK_STREAM);
	if (opened != OK) return opened;
	if (p_reuse) {
#if defined(WINDOWS_ENABLED)
		const int reuse = 1; // Enable Windows port sharing only for explicitly shared workers.
		if (::setsockopt(Socket(fd), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse)) != 0) {
			const Error error = failure(last_error());
			close();
			return error;
		}
#elif defined(SO_REUSEPORT)
		const int reuse = 1; // Share the port only for an explicitly requested multiprocess listener.
		if (::setsockopt(Socket(fd), SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char *>(&reuse), sizeof(reuse)) != 0) {
			const Error error = failure(last_error());
			close();
			return error;
		}
#else
		close();
		return ERR_UNAVAILABLE;
#endif
	}
#ifndef WINDOWS_ENABLED
	const int reuse = 1; // Reuse a previous Unix listen address without permitting Windows port takeover.
	if (::setsockopt(Socket(fd), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
		const Error error = failure(last_error());
		close();
		return error;
	}
#endif
	if (address.ss_family == AF_INET6) {
		const int both = 0; // Retain the native address family when dual stack is unavailable.
		::setsockopt(Socket(fd), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&both), sizeof(both));
	}
	if (::bind(Socket(fd), reinterpret_cast<sockaddr *>(&address), size) != 0 || ::listen(Socket(fd), backlog()) != 0) {
		const Error error = failure(last_error());
		close();
		return error;
	}
	state = LISTENING;
	return OK;
}

// Make an arrived connection noninheritable and nonblocking before returning it.
Error GDStream::accept(Ref<GDStream> &r_peer) {
	for (;;) {
#if defined(LINUXBSD_ENABLED) && defined(__linux__)
		const Socket accepted = ::accept4(Socket(fd), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
		std::unique_lock<std::mutex> lock(GDFork::mutex);
		const Socket accepted = ::accept(Socket(fd), nullptr, nullptr);
#endif
#ifdef WINDOWS_ENABLED
		const bool valid = accepted != INVALID_SOCKET;
#else
		const bool valid = accepted >= 0;
#endif
		if (!valid) {
			const int error = last_error();
#ifdef WINDOWS_ENABLED
			if (error == WSAECONNABORTED || error == WSAECONNRESET || interrupted(error)) continue;
#else
			if (error == ECONNABORTED || interrupted(error)) continue;
#endif
			return failure(error);
		}
		Ref<GDStream> peer;
		peer.instantiate();
		peer->fd = intptr_t(accepted);
#if !defined(LINUXBSD_ENABLED) || !defined(__linux__)
		const Error configured = configure(peer->fd);
		if (configured != OK) {
			peer->close();
			return configured;
		}
#endif
		peer->state = CONNECTED;
		peer->defaults();
		r_peer = peer;
		return OK;
	}
}

// Let protocol readers distinguish a zero-byte wait from peer EOF.
void GDStream::probe() {
	if (status() != CONNECTED || eof) return;
	char byte;
	for (;;) {
		const auto got = ::recv(Socket(fd), &byte, 1, MSG_PEEK);
		if (got >= 0) { eof = got == 0; return; }
		const int error = last_error();
		if (interrupted(error)) continue;
		if (failure(error) != ERR_BUSY) state = BROKEN;
		return;
	}
}

// Query readable bytes to avoid unnecessarily large buffer allocations.
int GDStream::available() const {
#ifdef WINDOWS_ENABLED
	u_long count = 0;
	return ::ioctlsocket(Socket(fd), FIONREAD, &count) == 0 ? int(MIN(count, u_long(INT_MAX))) : 0;
#else
	int count = 0;
	return ::ioctl(Socket(fd), FIONREAD, &count) == 0 ? MAX(0, count) : 0;
#endif
}

// Return available bytes immediately; a received FIN does not close the send direction.
Error GDStream::read(uint8_t *p_data, int p_size, int &r_got) {
	r_got = 0;
	if (!p_size) return OK;
	if (eof) return ERR_FILE_EOF;
	for (;;) {
		const auto got = ::recv(Socket(fd), reinterpret_cast<char *>(p_data), p_size, 0);
		if (got > 0) { r_got = int(got); return OK; }
		if (got == 0) { eof = true; return ERR_FILE_EOF; }
		const int error = last_error();
		if (!interrupted(error)) return failure(error);
	}
}

// Send only what fits and return EAGAIN to the caller's write wait.
Error GDStream::write(const uint8_t *p_data, int p_size, int &r_sent) {
	r_sent = 0;
	if (p_size < 0) return ERR_INVALID_PARAMETER;
	if (!p_size) return OK;
	p_size = MIN(p_size, MAX_WRITE);
	int flags = 0;
#ifdef MSG_NOSIGNAL
	flags = MSG_NOSIGNAL;
#endif
	for (;;) {
		const auto sent = ::send(Socket(fd), reinterpret_cast<const char *>(p_data), p_size, flags);
		if (sent >= 0) { r_sent = int(sent); return sent ? OK : FAILED; }
		const int error = last_error();
		if (!interrupted(error)) return failure(error);
	}
}

// Gather response regions in order and preserve partial progress for the caller.
Error GDStream::write_parts(GDWrites p_parts, int64_t &r_sent) {
	r_sent = 0;
	// A single region needs no descriptor import in the kernel.
	if (p_parts.count() == 1) {
		GDWrite part;
		if (!p_parts.next(part) || part.size < 0) return ERR_INVALID_PARAMETER;
		int sent = 0;
		const Error error = write(part.data, int(MIN(part.size, int64_t(MAX_WRITE))), sent);
		r_sent = sent;
		return error;
	}
	// Join small framing and payloads; larger responses retain scatter/gather ownership.
	uint8_t joined[2048]; // Scratch capacity only; oversized output uses native vectors below.
	GDWrites scan = p_parts;
	GDWrite piece;
	int used = 0;
	bool fits = true;
	while (scan.next(piece)) {
		if (piece.size < 0) return ERR_INVALID_PARAMETER;
		if (piece.size > int64_t(sizeof(joined)) - used) { fits = false; break; }
		memcpy(joined + used, piece.data, size_t(piece.size));
		used += int(piece.size);
	}
	if (fits) {
		int sent = 0;
		const Error error = write(joined, used, sent);
		r_sent = sent;
		return error;
	}
	int count = 0;
#ifdef WINDOWS_ENABLED
	using Part = WSABUF; // Native descriptor for a retained byte region.
	const uint64_t width = p_parts.count();
	int64_t left = UINT32_MAX; // Keep one reported byte count within the native DWORD result.
#else
	using Part = iovec; // Native descriptor for a retained byte region.
	static const long max_parts = sysconf(_SC_IOV_MAX); // Host syscall width, not a response limit.
	const uint64_t width = MIN(p_parts.count(), uint64_t(max_parts > 0 ? max_parts : 1));
#endif
	Part small[8]; // Stack descriptors for short gathered writes; larger batches grow dynamically.
	LocalVector<Part> extra;
	const uint64_t inline_count = sizeof(small) / sizeof(small[0]); // Storage capacity, not a transfer limit.
	if (width > inline_count) extra.resize(width);
	Part *parts = width > inline_count ? extra.ptr() : small;
	// Split only the native transfer size; the caller retains every unsent byte.
	GDWrite part;
	while (uint64_t(count) < width && p_parts.next(part)) {
		if (part.size < 0) return ERR_INVALID_PARAMETER;
		int size = int(MIN(part.size, int64_t(MAX_WRITE)));
#ifdef WINDOWS_ENABLED
		size = int(MIN(int64_t(size), left));
		if (!left) break;
		left -= size;
#endif
		if (!size) continue;
#ifdef WINDOWS_ENABLED
		parts[count] = {ULONG(size), reinterpret_cast<char *>(const_cast<uint8_t *>(part.data))};
#else
		parts[count] = {const_cast<uint8_t *>(part.data), size_t(size)};
#endif
		++count;
		if (size < part.size) break;
	}
	if (!count) return OK;
#ifndef WINDOWS_ENABLED
	msghdr message = {};
	message.msg_iov = parts;
	message.msg_iovlen = count;
	int flags = 0;
#ifdef MSG_NOSIGNAL
	flags = MSG_NOSIGNAL;
#endif
#endif
	for (;;) {
#ifdef WINDOWS_ENABLED
		DWORD sent = 0;
		if (::WSASend(Socket(fd), parts, count, &sent, 0, nullptr, nullptr) == 0) {
#else
		const ssize_t sent = ::sendmsg(Socket(fd), &message, flags);
		if (sent >= 0) {
#endif
			r_sent = sent;
			return sent ? OK : FAILED;
		}
		const int error = last_error();
		if (!interrupted(error)) return failure(error);
	}
}

// Read socket addresses while preserving scope and address family.
Dictionary GDStream::addr(bool p_remote) const {
	Dictionary out;
	if (!is_open()) return out;
	sockaddr_storage address;
	AddrSize size = sizeof(address);
	const int result = p_remote ? ::getpeername(Socket(fd), reinterpret_cast<sockaddr *>(&address), &size) :
			::getsockname(Socket(fd), reinterpret_cast<sockaddr *>(&address), &size);
	if (result != 0) return out;
	out["network"] = "tcp";
	out["host"] = GDAddress::text(reinterpret_cast<sockaddr *>(&address));
	out["port"] = address.ss_family == AF_INET ? ntohs(reinterpret_cast<sockaddr_in &>(address).sin_port) :
			ntohs(reinterpret_cast<sockaddr_in6 &>(address).sin6_port);
	return out;
}
