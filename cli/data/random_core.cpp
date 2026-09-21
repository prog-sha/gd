// Obtain cryptographic randomness directly from each supported operating system.
#include "hash_core.h"
#include <algorithm>
#include <climits>
#include <cstdlib>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sys/random.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif

// Fill every requested byte; never substitute a predictable PRNG on failure.
bool GDCrypto::random_fill(uint8_t *p_out, size_t p_size) {
#if defined(_WIN32)
	// Resolve the OS process generator once; it accepts a pointer-sized byte count.
	using Generate = BOOL(WINAPI *)(PBYTE, SIZE_T);
	static const Generate generate = []() -> Generate {
		HMODULE module = LoadLibraryExW(L"bcryptprimitives.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		return module ? reinterpret_cast<Generate>(GetProcAddress(module, "ProcessPrng")) : nullptr;
	}();
	return !p_size || (generate && generate(p_out, p_size));
#elif defined(__APPLE__) || defined(__OpenBSD__)
	if (p_size) arc4random_buf(p_out, p_size);
	return true;
#elif defined(__linux__)
	while (p_size) {
		const ssize_t n = getrandom(p_out, std::min(p_size, size_t(INT_MAX)), 0);
		if (n > 0) { p_out += n; p_size -= n; continue; }
		if (n < 0 && errno == EINTR) continue;
		if (n < 0 && errno == ENOSYS) {
			int fd;
			do { fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC); } while (fd < 0 && errno == EINTR);
			if (fd < 0) return false;
			while (p_size) {
				const ssize_t count = read(fd, p_out, std::min(p_size, size_t(INT_MAX)));
				if (count < 0 && errno == EINTR) continue;
				if (count <= 0) { close(fd); return false; }
				p_out += count; p_size -= count;
			}
			close(fd);
			return true;
		}
		return false;
	}
	return true;
#else
	return p_size == 0;
#endif
}
