// Perform UDP system calls without blocking the event loop, distinguishing EINTR from EAGAIN.
#include "cli/net/datagram.h"
#include "cli/net/address.h"


#ifdef WINDOWS_ENABLED
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <sys/socket.h>
#endif

namespace {
#ifdef WINDOWS_ENABLED
using Socket = SOCKET; // Preserve the native socket handle width.
using AddrSize = int; // Winsock address length.
constexpr DWORD UDP_NETRESET = _WSAIOW(IOC_VENDOR, 15); // Stable network-reset IOCTL, independent of header aliases.
#else
using Socket = int; // POSIX descriptor.
using AddrSize = socklen_t; // POSIX address length.
#endif

// Set the source address and port in the packet result.
void source(const sockaddr_storage &p_addr, Dictionary &r_packet) {
	r_packet["host"] = GDAddress::text(reinterpret_cast<const sockaddr *>(&p_addr));
	if (p_addr.ss_family == AF_INET) {
		const sockaddr_in &addr = reinterpret_cast<const sockaddr_in &>(p_addr);
		r_packet["port"] = ntohs(addr.sin_port);
	} else {
		const sockaddr_in6 &addr = reinterpret_cast<const sockaddr_in6 &>(p_addr);
		r_packet["port"] = ntohs(addr.sin6_port);
	}
}
}

// Share numeric IP parsing between binding and sending.
bool GDDatagram::is_ip(const String &p_host) {
	sockaddr_storage addr;
	return p_host != "*" && GDAddress::parse(p_host, 0, addr);
}

// Open a nonblocking, noninheritable socket and configure its OS receive buffer.
Error GDDatagram::open(const String &p_host, int p_port, int p_buffer) {
	close();
	sockaddr_storage addr;
	const AddrSize size = GDAddress::parse(p_host, p_port, addr);
	if (!size) return ERR_INVALID_PARAMETER;
	family = addr.ss_family;
	const Error opened = open_socket(family, SOCK_DGRAM);
	if (opened != OK) return opened;
	const Socket sock = Socket(fd);
#ifdef WINDOWS_ENABLED
	DWORD returned = 0;
	BOOL reset = FALSE; // Do not surface ICMP resets for other packets as receive errors.
	if (::WSAIoctl(sock, SIO_UDP_CONNRESET, &reset, sizeof(reset), nullptr, 0, &returned, nullptr, nullptr) != 0 ||
			::WSAIoctl(sock, UDP_NETRESET, &reset, sizeof(reset), nullptr, 0, &returned, nullptr, nullptr) != 0) {
		const Error err = failure(last_error());
		close();
		return err;
	}
#endif
	const int both = 0; // Accept IPv4 on an IPv6 wildcard socket.
	const int broadcast = 1; // Enable UDP broadcast transmission.
#ifdef WINDOWS_ENABLED
	const bool use_broadcast = family == AF_INET; // Winsock IPv6 sockets do not support this broadcast option.
#else
	const bool use_broadcast = true; // Apply the option to both address families on Unix.
#endif
	// Request dual-stack operation, retaining the native family if the OS cannot enable it.
	if (family == AF_INET6) {
		::setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&both), sizeof(both));
	}
	if ((use_broadcast && ::setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&broadcast), sizeof(broadcast)) != 0) ||
			(p_buffer > 0 && ::setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&p_buffer), sizeof(p_buffer)) != 0) ||
			::bind(sock, reinterpret_cast<sockaddr *>(&addr), size) != 0) {
		const Error err = failure(last_error());
		close();
		return err;
	}
	return OK;
}

// Receive one packet directly into the requested buffer without a read-ahead ring.
Error GDDatagram::read(int p_max, PackedByteArray &r_data, Dictionary &r_packet) {
	if (r_data.size() != p_max && r_data.resize(p_max) != OK) return ERR_OUT_OF_MEMORY;
	char empty = 0; // Pass a valid address to the native API even for a zero-length buffer.
	char *buffer = p_max ? reinterpret_cast<char *>(r_data.ptrw()) : &empty;
	sockaddr_storage addr = {};
	bool truncated = false;
	int got = 0;
	for (;;) {
#ifdef WINDOWS_ENABLED
		AddrSize size = sizeof(addr);
		WSABUF buf{ ULONG(p_max ? p_max : 1), buffer };
		DWORD received = 0;
		DWORD flags = 0;
		const int rc = ::WSARecvFrom(Socket(fd), &buf, 1, &received, &flags, reinterpret_cast<sockaddr *>(&addr), &size, nullptr, nullptr);
		const int err = rc == 0 ? 0 : last_error();
		truncated = err == WSAEMSGSIZE;
		if (rc == 0 || truncated) {
			got = int(received);
			truncated = truncated || got > p_max;
			got = MIN(got, p_max);
			break;
		}
#else
		// Use one byte to distinguish packet arrival from a zero-length receive that returns immediately.
		iovec buf{ buffer, size_t(p_max ? p_max : 1) };
		msghdr msg = {};
		msg.msg_name = &addr;
		msg.msg_namelen = sizeof(addr);
		msg.msg_iov = &buf;
		msg.msg_iovlen = 1;
		const ssize_t rc = ::recvmsg(Socket(fd), &msg, 0);
		if (rc >= 0) {
			got = MIN(int(rc), p_max);
			truncated = (msg.msg_flags & MSG_TRUNC) != 0 || rc > p_max;
			break;
		}
		const int err = last_error();
#endif
		if (!interrupted(err)) return failure(err);
	}
	r_data.resize(got);
	r_packet["data"] = r_data;
	r_packet["truncated"] = truncated;
	source(addr, r_packet);
	return OK;
}

// Send an unsplit packet, waiting for writable readiness only when the send buffer fills.
Error GDDatagram::write(const PackedByteArray &p_data, const String &p_host, int p_port) {
	sockaddr_storage addr;
	const AddrSize size = GDAddress::parse(p_host, p_port, addr, family == AF_INET6);
	if (!size) return ERR_INVALID_PARAMETER;
	const char *buffer = p_data.is_empty() ? "" : reinterpret_cast<const char *>(p_data.ptr());
	for (;;) {
		const auto sent = ::sendto(Socket(fd), buffer, int(p_data.size()), 0, reinterpret_cast<sockaddr *>(&addr), size);
		if (sent >= 0) return sent == p_data.size() ? OK : FAILED;
		const int err = last_error();
		if (!interrupted(err)) return failure(err);
	}
}

// Return the selected local port.
int GDDatagram::port() const {
	sockaddr_storage addr = {};
	AddrSize size = sizeof(addr);
	if (::getsockname(Socket(fd), reinterpret_cast<sockaddr *>(&addr), &size) != 0) return 0;
	return addr.ss_family == AF_INET ? ntohs(reinterpret_cast<const sockaddr_in &>(addr).sin_port) : ntohs(reinterpret_cast<const sockaddr_in6 &>(addr).sin6_port);
}
