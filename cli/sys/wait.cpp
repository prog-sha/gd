/**************************************************************************/
/*  wait.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement shared I/O readiness waiting declared in wait.h.
//
// Keep endpoint registrations in the kernel and sleep until one becomes ready.
// Avoid busy spinning in persistent serving.
//
// Retain registrations instead of rebuilding the descriptor set for every wait.
// Use a dedicated wakeup source: EVFILT_USER on macOS or eventfd on Linux.
// Use WSAPoll with a loopback socket pair on Windows.
// Coalesce duplicate wakeups with one compare-and-swap.
// Round sub-millisecond waits up to one millisecond to avoid spinning at zero.

#include "cli/sys/wait.h"

#include "cli/sys/task.h"

#include "core/object/callable_mp.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/variant.h"
#include <cerrno>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#if defined(MACOS_ENABLED) || defined(IOS_ENABLED) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define GD_WAIT_KQUEUE
#include <sys/event.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined(LINUXBSD_ENABLED) || defined(ANDROID_ENABLED)
#define GD_WAIT_EPOLL
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#elif defined(WINDOWS_ENABLED)
#define GD_WAIT_WSAPOLL
#include <winsock2.h>
#endif

namespace {

// Terminate on lost runtime infrastructure without reentering the engine or spawning diagnostics.
[[noreturn]] void fatal(const char *p_operation, int p_error) {
	std::fprintf(stderr, "runtime: %s failed (%d)\n", p_operation, p_error);
	std::fflush(stderr);
	std::abort();
}

// Flag coalescing duplicate wakeup requests.
SafeFlag g_wake_sig;
Mutex g_ready_mutex; // Protect registration changes and readiness collection.
// Track registration generations and delivery state to prevent stale-fd notifications reaching new connections.
struct ReadySlot {
	Callable call; // Handler for descriptor readiness.
	Callable delivery; // Reusable dispatch retaining only the descriptor and registration generation.
	uint64_t seq = 0; // Registration generation.
	bool pending = false; // Deduplicate notifications from collection through callback completion.
};
// Identify one collected descriptor without retaining its registration.
struct ReadyKey {
	uint64_t fd = 0; // Descriptor number reported by the kernel.
	uint64_t seq = 0; // Registration generation rejecting stale reuse.
};
HashMap<uint64_t, ReadySlot> g_callbacks; // Per-descriptor delivery targets and state.
uint64_t g_ready_seq = 0; // Sequence distinguishing registrations even when descriptor numbers are reused.
LocalVector<ReadyKey> g_ready_fds; // Reusable FIFO preserving kernel notification order.

// Event-array capacity for one kernel wait call.
#ifdef GD_WAIT_KQUEUE
constexpr int WAIT_MAX = 64;
#else
constexpr int WAIT_MAX = 128;
#endif

// Convert timeout to milliseconds, preserving infinity and rounding positive fractions upward.
int timeout_ms_of(uint64_t p_usec) {
	if (p_usec == UINT64_MAX) {
		return -1;
	}
	if (p_usec == 0) {
		return 0;
	}
	const uint64_t ms = p_usec / 1000 + (p_usec % 1000 != 0);
	return ms > (uint64_t)INT32_MAX ? INT32_MAX : (int)ms;
}

// Resolve a queued notification against the current descriptor registration.
void deliver_ready(uint64_t p_fd, uint64_t p_seq);

// Attach a direct delivery target to a monitored descriptor.
void remember_callback(intptr_t p_fd, const Callable &p_ready) {
	MutexLock lock(g_ready_mutex);
	const uint64_t fd = uint64_t(p_fd);
	const ReadySlot *current = g_callbacks.getptr(fd);
	if (current && current->call == p_ready) {
		return; // Preserve an undelivered generation when the callback is unchanged.
	}
	if (p_ready.is_valid()) {
		ReadySlot slot;
		slot.call = p_ready;
		slot.seq = ++g_ready_seq;
		slot.delivery = callable_mp_static(&deliver_ready).bind(fd, slot.seq);
		g_callbacks.insert(fd, slot);
	} else {
		g_callbacks.erase(fd);
	}
}

// Remove a closing descriptor's target and pending-delivery state.
void forget_callback(intptr_t p_fd) {
	MutexLock lock(g_ready_mutex);
	const uint64_t fd = uint64_t(p_fd);
	g_callbacks.erase(fd);
}

// Queue kernel-ready descriptors for the next main-loop turn.
void mark_ready(intptr_t p_fd) {
	MutexLock lock(g_ready_mutex);
	const uint64_t fd = uint64_t(p_fd);
	ReadySlot *slot = g_callbacks.getptr(fd);
	if (!slot) {
		Async::notify(); // Use observer notification only for backends without individual callbacks.
	}
	if (slot && !slot->pending) {
		slot->pending = true;
		g_ready_fds.push_back(ReadyKey{ fd, slot->seq });
	}
}

// Deliver only matching registration generations and coalesce level notifications during callbacks.
void deliver_ready(uint64_t p_fd, uint64_t p_seq) {
	Callable call;
	{
		MutexLock lock(g_ready_mutex);
		const ReadySlot *slot = g_callbacks.getptr(p_fd);
		if (!slot || slot->seq != p_seq) {
			return;
		}
		call = slot->call;
	}
	if (call.is_valid()) {
		call.call();
	}
	MutexLock lock(g_ready_mutex);
	ReadySlot *slot = g_callbacks.getptr(p_fd);
	if (slot && slot->seq == p_seq) {
		slot->pending = false; // Unconsumed I/O will be reported by the next kernel scan.
	}
}

#if defined(GD_WAIT_KQUEUE)

// Fixed identifier distinguishing the EVFILT_USER wakeup source.
constexpr uintptr_t WAKE_IDENT = 0xee1eb9f4;

std::atomic<int> g_kq{-1}; // Publish only a fully registered wakeup source.
SafeNumeric<int> g_count;

// Retry wakeup registration or triggering only on interruption; fail explicitly if delivery is impossible.
void change_wake(int p_queue, uint16_t p_flags, uint32_t p_notes) {
	struct kevent ev;
	EV_SET(&ev, WAKE_IDENT, EVFILT_USER, p_flags, p_notes, 0, nullptr);
	int result;
	do { result = ::kevent(p_queue, &ev, 1, nullptr, 0, nullptr); } while (result < 0 && errno == EINTR);
	if (result < 0) {
		fatal("kevent wake", errno);
	}
}

// Initialize waiting and waking together, treating missing runtime infrastructure as fatal.
void ensure_init() {
	if (g_kq >= 0) {
		return;
	}
	// Serialize first use and register the event before exposing its descriptor.
	static const int queue = []() {
		const int fd = ::kqueue();
		if (fd < 0) {
			fatal("kqueue", errno);
		}
		change_wake(fd, EV_ADD | EV_CLEAR, 0);
		return fd;
	}();
	g_kq = queue;
}

#elif defined(GD_WAIT_EPOLL)

std::atomic<int> g_ep{-1}; // Publish only a fully registered wakeup source.
int g_event_fd = -1; // Immutable after the poller descriptor is published.
SafeNumeric<int> g_count;

// Initialize waiting and waking together, treating missing runtime infrastructure as fatal.
void ensure_init() {
	if (g_ep >= 0) {
		return;
	}
	// Serialize first use and retain both descriptors privately until registration succeeds.
	static const int queue = []() {
		const int fd = ::epoll_create1(EPOLL_CLOEXEC);
		if (fd < 0) {
			fatal("epoll_create1", errno);
		}
		const int wake = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (wake < 0) {
			fatal("eventfd", errno);
		}
		epoll_event ev = {};
		ev.events = EPOLLIN; // Use level triggering so pending notifications are not lost.
		ev.data.fd = wake;
		const int registered = ::epoll_ctl(fd, EPOLL_CTL_ADD, wake, &ev);
		if (registered < 0) {
			fatal("epoll_ctl wake registration", errno);
		}
		g_event_fd = wake;
		return fd;
	}();
	g_ep = queue;
}

#elif defined(GD_WAIT_WSAPOLL)

Mutex g_mutex; // Protect registrations against concurrent add/remove calls.
LocalVector<SOCKET> g_fds;
LocalVector<uint8_t> g_fd_events; // Read/write interests indexed alongside sockets.

constexpr uint8_t WAIT_READ = 1; // Readable-readiness interest.
constexpr uint8_t WAIT_WRITE = 2; // Writable-readiness interest.

// Hold the connected sockets used by every Windows wait and wake operation.
struct WakePair {
	SOCKET rd; // Receiver monitored by WSAPoll.
	SOCKET wr; // Sender available to every producer thread.
};

// Close incomplete sockets while preserving the operation's original error.
[[noreturn]] void fail_wake(const char *p_operation, int p_error, SOCKET p_listener, SOCKET p_rd, SOCKET p_wr) {
	if (p_listener != INVALID_SOCKET) {
		::closesocket(p_listener);
	}
	if (p_rd != INVALID_SOCKET) {
		::closesocket(p_rd);
	}
	if (p_wr != INVALID_SOCKET) {
		::closesocket(p_wr);
	}
	fatal(p_operation, p_error);
}

// Create the wakeup pair on loopback so remote peers cannot connect.
// A socket pair lets wakeups use the same Windows polling interface as other readiness.
WakePair make_wake_pair() {
	const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == INVALID_SOCKET) {
		fatal("wake listener socket", WSAGetLastError());
	}
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK); // Accept connections only from the local machine.
	addr.sin_port = 0; // Let the OS select an unused port.
	int len = (int)sizeof(addr);
	if (::bind(listener, (sockaddr *)&addr, len) != 0) {
		fail_wake("wake listener bind", WSAGetLastError(), listener, INVALID_SOCKET, INVALID_SOCKET);
	}
	if (::listen(listener, 1) != 0) {
		fail_wake("wake listener listen", WSAGetLastError(), listener, INVALID_SOCKET, INVALID_SOCKET);
	}
	if (::getsockname(listener, (sockaddr *)&addr, &len) != 0) {
		fail_wake("wake listener address", WSAGetLastError(), listener, INVALID_SOCKET, INVALID_SOCKET);
	}
	const SOCKET writer = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (writer == INVALID_SOCKET) {
		fail_wake("wake writer socket", WSAGetLastError(), listener, INVALID_SOCKET, INVALID_SOCKET);
	}
	if (::connect(writer, (sockaddr *)&addr, len) != 0) {
		fail_wake("wake writer connect", WSAGetLastError(), listener, INVALID_SOCKET, writer);
	}
	const SOCKET reader = ::accept(listener, nullptr, nullptr);
	if (reader == INVALID_SOCKET) {
		fail_wake("wake reader accept", WSAGetLastError(), listener, INVALID_SOCKET, writer);
	}
	u_long nb = 1;
	const int reading = ::ioctlsocket(reader, FIONBIO, &nb); // Make it possible to drain all queued wake bytes.
	if (reading != 0) {
		fail_wake("wake reader configuration", WSAGetLastError(), listener, reader, writer);
	}
	const int writing = ::ioctlsocket(writer, FIONBIO, &nb); // Do not block the wakeup sender when its buffer fills.
	if (writing != 0) {
		fail_wake("wake writer configuration", WSAGetLastError(), listener, reader, writer);
	}
	::closesocket(listener);
	return { reader, writer };
}

// Return the fully constructed process-wide wakeup pair.
const WakePair &wake_pair() {
	static const WakePair pair = make_wake_pair();
	return pair;
}

// Prepare the wait backend before any producer can publish work.
void ensure_init() {
	(void)wake_pair();
}

#else

// Leave initialization empty when no native wait backend exists.
void ensure_init() {
}

#endif

} //namespace

// Initialize the selected platform backend under runtime ownership.
void IdleWait::init() {
	ensure_init();
}

#if defined(GD_WAIT_KQUEUE)

// Register the descriptor's requested readiness interests.
int IdleWait::add(intptr_t p_fd, bool p_read, bool p_write, const Callable &p_ready) {
	if (p_fd < 0 || (!p_read && !p_write)) {
		return EINVAL;
	}
	ensure_init();
	if (g_kq < 0) {
		return EBADF;
	}
	struct kevent events[2];
	int n = 0;
	if (p_read) {
		EV_SET(&events[n++], p_fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
	}
	if (p_write) {
		EV_SET(&events[n++], p_fd, EVFILT_WRITE, EV_ADD, NOTE_LOWAT, 1, nullptr); // Resume large writes as soon as even one byte of capacity is available.
	}
	if (::kevent(g_kq, events, n, nullptr, 0, nullptr) < 0) return errno;
	remember_callback(p_fd, p_ready);
	g_count.increment();
	return 0;
}

// Remove monitoring, tolerating descriptors already removed by kernel closure.
void IdleWait::remove(intptr_t p_fd, bool p_read, bool p_write) {
	if (p_fd < 0 || g_kq < 0) {
		return;
	}
	struct kevent events[2];
	int n = 0;
	if (p_read) {
		EV_SET(&events[n++], p_fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
	}
	if (p_write) {
		EV_SET(&events[n++], p_fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
	}
	if (n > 0) {
		::kevent(g_kq, events, n, nullptr, 0, nullptr);
	}
	forget_callback(p_fd);
	g_count.decrement();
}

// Return the monitored count.
int IdleWait::count() {
	return MAX(0, g_count.get());
}

// Wake the sleeping backend from any thread.
void IdleWait::wake(bool p_notify) {
	if (p_notify) { Async::notify(); }
	ensure_init();
	// Do nothing when a wakeup is already pending.
	if (!g_wake_sig.set_if_clear()) {
		return; // A wakeup is already pending.
	}
	change_wake(g_kq, 0, NOTE_TRIGGER);
}

// Sleep until readiness or deadline.
int IdleWait::wait(uint64_t p_timeout_usec) {
	ensure_init();
	if (g_kq < 0) {
		return 0;
	}
	timespec ts;
	timespec *tp = nullptr;
	if (p_timeout_usec == 0) {
		ts.tv_sec = 0;
		ts.tv_nsec = 0;
		tp = &ts;
	} else if (p_timeout_usec != UINT64_MAX) {
		const uint64_t ms = (uint64_t)timeout_ms_of(p_timeout_usec);
		ts.tv_sec = (time_t)(ms / 1000);
		ts.tv_nsec = (long)((ms % 1000) * 1000000);
		tp = &ts;
	}
	struct kevent events[WAIT_MAX];
	int n;
	for (;;) {
		n = ::kevent(g_kq, nullptr, 0, events, WAIT_MAX, tp);
		if (n >= 0) break;
		const int error = errno;
		if (error != EINTR && error != ETIMEDOUT) {
			fatal("kevent wait", error);
		}
		if (p_timeout_usec > 0 && p_timeout_usec != UINT64_MAX) return 0; // Let the caller recompute the remaining finite timeout.
	}
	int ready = 0;
	for (int i = 0; i < n; i++) {
		if (events[i].filter == EVFILT_USER && events[i].ident == WAKE_IDENT) {
			g_wake_sig.clear();
			// Reissue wakeups consumed by a nonblocking scan so a sleeping waiter cannot lose them.
			if (p_timeout_usec == 0) {
				IdleWait::wake(false);
			}
			ready++;
			continue;
		}
		mark_ready((intptr_t)events[i].ident);
		ready++;
	}
	return ready;
}

#elif defined(GD_WAIT_EPOLL)

// Register the descriptor's requested readiness interests.
int IdleWait::add(intptr_t p_fd, bool p_read, bool p_write, const Callable &p_ready) {
	if (p_fd < 0 || (!p_read && !p_write)) {
		return EINVAL;
	}
	ensure_init();
	if (g_ep < 0) {
		return EBADF;
	}
	epoll_event ev;
	ev.events = 0;
	if (p_read) {
		ev.events |= EPOLLIN | EPOLLRDHUP;
	}
	if (p_write) {
		ev.events |= EPOLLOUT;
	}
	ev.data.fd = p_fd;
	if (::epoll_ctl(g_ep, EPOLL_CTL_ADD, p_fd, &ev) < 0) return errno;
	remember_callback(p_fd, p_ready);
	g_count.increment();
	return 0;
}

// Remove monitoring, tolerating descriptors already removed by kernel closure.
void IdleWait::remove(intptr_t p_fd, bool p_read, bool p_write) {
	if (p_fd < 0 || g_ep < 0) {
		return;
	}
	(void)p_read;
	(void)p_write;
	::epoll_ctl(g_ep, EPOLL_CTL_DEL, p_fd, nullptr);
	forget_callback(p_fd);
	g_count.decrement();
}


// Return the monitored count.
int IdleWait::count() {
	return MAX(0, g_count.get());
}

// Wake the sleeping backend from any thread.
void IdleWait::wake(bool p_notify) {
	if (p_notify) { Async::notify(); }
	ensure_init();
	if (!g_wake_sig.set_if_clear()) {
		return; // A wakeup is already pending.
	}
	uint64_t one = 1;
	// A full event counter already contains a wakeup, so another write is unnecessary.
	for (;;) {
		const ssize_t sent = ::write(g_event_fd, &one, sizeof(one));
		if (sent == (ssize_t)sizeof(one)) return;
		const int error = sent < 0 ? errno : 0;
		if (error == EINTR) continue;
		if (error == EAGAIN) return; // The eventfd counter already holds an undelivered wakeup.
		fatal("eventfd wake write", error);
	}
}

// Sleep until readiness or deadline.
int IdleWait::wait(uint64_t p_timeout_usec) {
	ensure_init();
	if (g_ep < 0) {
		return 0;
	}
	epoll_event events[WAIT_MAX];
	int n;
	for (;;) {
		n = ::epoll_wait(g_ep, events, WAIT_MAX, timeout_ms_of(p_timeout_usec));
		if (n >= 0) break;
		const int error = errno;
		if (error != EINTR) {
			fatal("epoll_wait", error);
		}
		if (p_timeout_usec > 0 && p_timeout_usec != UINT64_MAX) return 0; // Let the caller recompute the remaining finite timeout.
	}
	int ready = 0;
	for (int i = 0; i < n; i++) {
		if (events[i].data.fd == g_event_fd) {
			// Clear wake state only in a blocking wait; nonblocking scans preserve the notification.
			if (p_timeout_usec != 0) {
				uint64_t drained = 0;
				ssize_t got = 0;
				do {
					got = ::read(g_event_fd, &drained, sizeof(drained));
				} while (got < 0 && errno == EINTR);
				if (got == (ssize_t)sizeof(drained)) {
					g_wake_sig.clear();
				}
			}
			ready++;
			continue;
		}
		mark_ready(events[i].data.fd);
		ready++;
	}
	return ready;
}

#elif defined(GD_WAIT_WSAPOLL)

// Register the descriptor's requested readiness interests.
int IdleWait::add(intptr_t p_fd, bool p_read, bool p_write, const Callable &p_ready) {
	if (p_fd < 0 || (!p_read && !p_write)) {
		return WSAEINVAL;
	}
	ensure_init();
	MutexLock lock(g_mutex);
	const SOCKET s = (SOCKET)(uintptr_t)p_fd;
	const int64_t at = g_fds.find(s);
	const uint8_t events = (p_read ? WAIT_READ : 0) | (p_write ? WAIT_WRITE : 0);
	if (at == -1) {
		g_fds.push_back(s);
		g_fd_events.push_back(events);
	} else {
		g_fd_events[at] = events;
	}
	remember_callback(p_fd, p_ready);
	return 0;
}

// Remove monitoring.
void IdleWait::remove(intptr_t p_fd, bool p_read, bool p_write) {
	if (p_fd < 0) {
		return;
	}
	(void)p_read;
	(void)p_write;
	MutexLock lock(g_mutex);
	const int64_t at = g_fds.find((SOCKET)(uintptr_t)p_fd);
	if (at != -1) {
		g_fds.remove_at_unordered(at);
		g_fd_events.remove_at_unordered(at);
	}
	forget_callback(p_fd);
}


// Return the monitored count.
int IdleWait::count() {
	MutexLock lock(g_mutex);
	return g_fds.size();
}

// Wake the sleeping backend from any thread.
void IdleWait::wake(bool p_notify) {
	if (p_notify) { Async::notify(); }
	const WakePair &wake = wake_pair();
	if (!g_wake_sig.set_if_clear()) {
		return; // A wakeup is already pending.
	}
	const char one = 1;
	for (;;) {
		const int sent = ::send(wake.wr, &one, 1, 0);
		if (sent == 1) return;
		const int error = sent == SOCKET_ERROR ? WSAGetLastError() : 0;
		if (error == WSAEINTR) continue;
		if (error == WSAEWOULDBLOCK) return; // A full notification queue will deliver wake bytes already sent.
		fatal("socket wake send", error);
	}
}

// Sleep until readiness or deadline.
int IdleWait::wait(uint64_t p_timeout_usec) {
	const WakePair &wake = wake_pair();
	LocalVector<WSAPOLLFD> pfds;
	{
		MutexLock lock(g_mutex);
		pfds.resize(g_fds.size() + 1);
		pfds[0].fd = wake.rd;
		pfds[0].events = POLLRDNORM;
		pfds[0].revents = 0;
		uint32_t at = 1;
		for (uint32_t i = 0; i < g_fds.size(); i++, at++) {
			pfds[at].fd = g_fds[i];
			pfds[at].events = (g_fd_events[i] & WAIT_READ) != 0 ? POLLRDNORM : 0;
			if ((g_fd_events[i] & WAIT_WRITE) != 0) {
				pfds[at].events |= POLLWRNORM; // Wake when remaining output can be transmitted.
			}
			pfds[at].revents = 0;
		}
	}
	if (pfds.is_empty()) {
		return 0;
	}
	const int n = ::WSAPoll(pfds.ptr(), (ULONG)pfds.size(), timeout_ms_of(p_timeout_usec));
	if (n == SOCKET_ERROR) {
		fatal("WSAPoll wait", WSAGetLastError());
	}
	if (n == 0) {
		return 0;
	}
	// Drain wake bytes only after a sleeping wait consumes the notification, enabling the next wakeup.
	if (pfds[0].revents != 0 && p_timeout_usec != 0) {
		char drop[64];
		while (::recv(wake.rd, drop, sizeof(drop), 0) > 0) {
		}
		g_wake_sig.clear();
	}
	for (uint32_t i = 1; i < pfds.size(); i++) {
		if (pfds[i].revents != 0) {
			mark_ready((intptr_t)(uintptr_t)pfds[i].fd);
		}
	}
	return n;
}

#else

// Fallback configuration without a native wait backend.

// Accept registration without retaining it when no wait backend exists.
int IdleWait::add(intptr_t p_fd, bool p_read, bool p_write, const Callable &p_ready) {
	(void)p_fd;
	(void)p_read;
	(void)p_write;
	(void)p_ready;
	return ENOSYS;
}

// Removal is a no-op without retained registrations.
void IdleWait::remove(intptr_t p_fd, bool p_read, bool p_write) {
	(void)p_fd;
	(void)p_read;
	(void)p_write;
}

// Return zero because this backend monitors no descriptors.
int IdleWait::count() {
	return 0;
}

// Set wake state without signalling a non-sleeping backend.
void IdleWait::wake(bool p_notify) {
	if (p_notify) { Async::notify(); }
	g_wake_sig.set();
}

// Return immediately when no wait backend exists.
int IdleWait::wait(uint64_t p_timeout_usec) {
	return 0;
}

#endif

// Return the backend's original registration system call for public error context.
const char *IdleWait::operation() {
#if defined(GD_WAIT_KQUEUE)
	return "kevent";
#elif defined(GD_WAIT_EPOLL)
	return "epoll_ctl";
#elif defined(GD_WAIT_WSAPOLL)
	return "WSAPoll";
#else
	return "poll";
#endif
}

// Replace registered interests, leaving no registration behind when replacement fails.
int IdleWait::change(intptr_t p_fd, bool p_old_read, bool p_old_write, bool p_read, bool p_write, const Callable &p_ready) {
#ifdef GD_WAIT_EPOLL
	epoll_event event = {};
	event.events = (p_read ? EPOLLIN | EPOLLRDHUP : 0) | (p_write ? EPOLLOUT : 0);
	event.data.fd = p_fd;
	if (::epoll_ctl(g_ep, EPOLL_CTL_MOD, p_fd, &event) == 0) {
		remember_callback(p_fd, p_ready);
		return 0;
	}
	const int error = errno;
	remove(p_fd, p_old_read, p_old_write);
	return error;
#else
	remove(p_fd, p_old_read, p_old_write);
	return add(p_fd, p_read, p_write, p_ready);
#endif
}

// Deduplicate ready-descriptor handlers and invoke them on the main thread.
void IdleWait::callback(intptr_t p_fd, const Callable &p_call) {
	remember_callback(p_fd, p_call);
}

// Transfer ready descriptors and their registration generations to the ready queue.
void IdleWait::dispatch() {
	LocalVector<Callable> calls;
	{
		MutexLock lock(g_ready_mutex);
		calls.reserve(g_ready_fds.size());
		for (const ReadyKey &key : g_ready_fds) {
			const ReadySlot *slot = g_callbacks.getptr(key.fd);
			if (slot && slot->seq == key.seq) {
				calls.push_back(slot->delivery);
			}
		}
		g_ready_fds.clear();
	}
	Async::post_many(calls); // Retain individual FIFO turns while publishing the batch under one lock.
}
