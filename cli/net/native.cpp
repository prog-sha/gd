// Use shared ownership rules to create, monitor, and destroy TCP and UDP descriptors.
#include "cli/net/native.h"
#include "cli/sys/fork.h"
#include "cli/sys/wait.h"
#include "cli/sys/source_error.h"
#include "cli/sys/task.h"

#ifdef WINDOWS_ENABLED
#include <winsock2.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// Capture the OS error immediately after a system call.
int GDNative::last_error() {
#ifdef WINDOWS_ENABLED
	return WSAGetLastError();
#else
	return errno;
#endif
}

// Retry an interrupted system call instead of registering a readiness wait.
bool GDNative::interrupted(int p_error) {
#ifdef WINDOWS_ENABLED
	return p_error == WSAEINTR;
#else
	return p_error == EINTR;
#endif
}

// Distinguish kernel waits, permission failures, and packet-size errors.
Error GDNative::failure(int p_error) {
#ifdef WINDOWS_ENABLED
	if (p_error == WSAEWOULDBLOCK) return ERR_BUSY;
	if (p_error == WSAEACCES) return ERR_UNAUTHORIZED;
	if (p_error == WSAEMSGSIZE) return ERR_INVALID_PARAMETER;
#else
	if (p_error == EAGAIN || p_error == EWOULDBLOCK) return ERR_BUSY;
	if (p_error == EACCES || p_error == EPERM) return ERR_UNAUTHORIZED;
	if (p_error == EMSGSIZE) return ERR_INVALID_PARAMETER;
#endif
	return FAILED;
}

// Make an acquired descriptor nonblocking and noninheritable while preserving its other flags.
Error GDNative::configure(intptr_t p_fd) {
#ifdef WINDOWS_ENABLED
	u_long on = 1;
	if (::ioctlsocket(SOCKET(p_fd), FIONBIO, &on) != 0) return failure(last_error());
	return ::SetHandleInformation(reinterpret_cast<HANDLE>(p_fd), HANDLE_FLAG_INHERIT, 0) ? OK : FAILED;
#else
	const int status = ::fcntl(int(p_fd), F_GETFL);
	const int flags = ::fcntl(int(p_fd), F_GETFD);
	if (status < 0 || flags < 0 || ::fcntl(int(p_fd), F_SETFL, status | O_NONBLOCK) != 0 ||
			::fcntl(int(p_fd), F_SETFD, flags | FD_CLOEXEC) != 0) return failure(last_error());
#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
	const int on = 1; // Suppress broken-pipe signals for the descriptor's entire lifetime.
	if (::setsockopt(int(p_fd), SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) != 0) return failure(last_error());
#endif
	return OK;
#endif
}

// Protect socket creation through CLOEXEC setup from concurrent forks, releasing the descriptor on failure.
Error GDNative::open_socket(int p_family, int p_type) {
	close();
	if (users) return ERR_BUSY;
	closing = false;
	poll_error.unref();
#ifdef WINDOWS_ENABLED
	const SOCKET socket = ::WSASocketW(p_family, p_type, 0, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
	if (socket == INVALID_SOCKET) return failure(last_error());
	fd = intptr_t(socket);
	const Error error = configure(fd);
#elif defined(LINUXBSD_ENABLED) && defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
	fd = ::socket(p_family, p_type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) return failure(last_error());
	const Error error = OK;
#else
	std::unique_lock<std::mutex> lock(GDFork::mutex);
	fd = ::socket(p_family, p_type, 0);
	if (fd < 0) return failure(last_error());
	const Error error = configure(fd);
#endif
	if (error != OK) close();
	return error;
}

// Update the kernel registration only when the read or write interest changes.
void GDNative::watch(bool p_read, bool p_write, const Callable &p_call) {
	ready = p_call;
	if (!is_open()) return;
	if (reading != p_read || writing != p_write) {
		int error = 0;
		if ((reading || writing) && (p_read || p_write)) {
			error = IdleWait::change(fd, reading, writing, p_read, p_write, p_call);
		} else {
			if (reading || writing) IdleWait::remove(fd, reading, writing);
			reading = writing = false;
			if (p_read || p_write) error = IdleWait::add(fd, p_read, p_write, p_call);
		}
		if (error) {
			reading = writing = false; // Failed replacement has already removed the old registration.
#ifdef WINDOWS_ENABLED
			SourceError::win32(error);
#else
			SourceError::posix(error);
#endif
			Dictionary info;
			info["op"] = "register";
			info["syscall"] = IdleWait::operation();
			const Error code = SourceError::put(info, FAILED);
			poll_error = R::err(Err::make("cannot register socket readiness", Err::of(code), info));
			const Callable notify = p_call; // Keep the destination before close clears the readiness callback.
			close(); // Destroy the descriptor to remove any partially installed kernel registration.
			Async::post(Ref<RefCounted>(this), notify); // Deliver failure after the caller has connected its await continuation.
			return;
		}
		reading = p_read;
		writing = p_write;
	} else if (reading || writing) {
		IdleWait::callback(fd, p_call);
	}
}

// Remove kernel monitoring before closing to prevent misdelivery after descriptor reuse.
void GDNative::close() {
	if (!is_open()) return;
	watch(false, false, Callable());
	closing = true;
	if (!users) destroy();
}

// Keep descriptor identity stable until work dispatched before closure has returned.
bool GDNative::retain_io() {
	if (!is_open()) return false;
	++users;
	return true;
}

// Perform deferred physical closure without waiting for worker execution on the runtime.
void GDNative::release_io() {
	ERR_FAIL_COND(!users);
	if (!--users && closing) destroy();
}

// Release an unmonitored descriptor that no worker can still use.
void GDNative::destroy() {
	if (fd == -1) return;
#ifdef WINDOWS_ENABLED
	::closesocket(SOCKET(fd));
#else
	::close(int(fd));
#endif
	fd = -1;
}

// Release the owned native resources.
GDNative::~GDNative() {
	close();
}
