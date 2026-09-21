// Share inline and expandable cryptographic storage while erasing released dynamic allocations.
#pragma once
#include "crypto_core.h"
#include <array>
#include <memory>
#include <type_traits>
#include <vector>

namespace GDCrypto {
// Clear the entire allocated element range before returning secret-bearing memory to its allocator.
template <class T> struct ErasingAllocator {
	using value_type = T; // Element type required by allocator traits.
	using is_always_equal = std::true_type; // Allocator instances have no distinct external ownership.
	ErasingAllocator() = default; // Use the process allocator without retained state.
	template <class U> ErasingAllocator(const ErasingAllocator<U> &) {} // Preserve stateless ownership when containers rebind element types.
	T *allocate(size_t count) { return std::allocator<T>{}.allocate(count); } // Delegate capacity and allocation failures without a protocol size cap.
	void deallocate(T *data, size_t count) noexcept { erase(data, count * sizeof(T)); std::allocator<T>{}.deallocate(data, count); } // Clear allocation capacity, including previously used spare elements.
	template <class U> bool operator==(const ErasingAllocator<U> &) const noexcept { return true; } // Permit ownership transfer between equivalent allocators.
	template <class U> bool operator!=(const ErasingAllocator<U> &) const noexcept { return false; } // Keep allocator equality consistent for all rebound types.
};

template <class T, size_t Count> using CryptoStorage = std::conditional_t<Count == 0, std::vector<T, ErasingAllocator<T>>, std::array<T, Count>>; // Zero capacity selects modulus-sized dynamic storage rather than a key-size policy.

// Initialize zeroed arithmetic storage with either compile-time or modulus-derived capacity.
template <class T, size_t Count> CryptoStorage<T, Count> crypto_storage(size_t count) {
	if constexpr (Count == 0) return CryptoStorage<T, Count>(count);
	else return {};
}

// Erase live elements through their backing storage rather than overwriting container metadata.
template <class Storage> void wipe_storage(Storage &value) {
	erase(value.data(), value.size() * sizeof(typename Storage::value_type));
}
}
