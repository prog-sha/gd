// Detect optional digest instructions without changing the minimum supported CPU.
#pragma once

#if !defined(GD_HASH_PORTABLE) && !defined(GD_CRYPTO_PORTABLE)
#if defined(__x86_64__) || defined(_M_X64)
#define GD_SHA_X86
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#if defined(__clang__)
#include <shaintrin.h>
#include <tmmintrin.h>
#define GD_SHA_TARGET __attribute__((target("sha,ssse3")))
#elif defined(_MSC_VER)
#define GD_SHA_TARGET
#else
#define GD_SHA_TARGET __attribute__((target("sha,ssse3")))
#endif
#elif defined(__aarch64__)
#define GD_SHA_ARM
#include <arm_neon.h>
#if defined(__clang__)
#define GD_SHA_TARGET __attribute__((target("sha2")))
#else
#define GD_SHA_TARGET __attribute__((target("+crypto")))
#endif
#if defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#endif
#endif

namespace SHAHW {
// Cache public CPU capability; unsupported operating systems retain portable rounds.
inline bool available() {
	static const bool value = [] {
#ifdef GD_SHA_X86
#ifdef _MSC_VER
		int words[4];
		__cpuid(words, 0);
		if (words[0] < 7) return false;
		__cpuidex(words, 7, 0);
		if (!(unsigned(words[1]) & (1U << 29))) return false;
		__cpuid(words, 1);
		return (unsigned(words[2]) & (1U << 9)) != 0;
#else
		unsigned a, b, c, d;
		if (!__get_cpuid_count(7, 0, &a, &b, &c, &d) || !(b & (1U << 29))) return false;
		return __get_cpuid(1, &a, &b, &c, &d) && (c & (1U << 9));
#endif
#elif defined(GD_SHA_ARM) && defined(__linux__)
		return (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
#elif defined(GD_SHA_ARM) && defined(__APPLE__)
		int enabled = 0;
		size_t size = sizeof(enabled);
		return sysctlbyname("hw.optional.arm.FEAT_SHA256", &enabled, &size, nullptr, 0) == 0 && enabled;
#elif defined(GD_SHA_ARM) && defined(_WIN32)
		return IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE) != 0;
#else
		return false;
#endif
	}();
	return value;
}
}
