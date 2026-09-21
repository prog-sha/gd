// Manage the safety budget for all OS threads created by the runtime.
#pragma once

#include <cstdint>

class GDThreadLimit {
public:
	static void acquire(); // Reserve a slot before creating a thread.
	static void release(); // Return the slot of a finished thread.
	static int64_t set(int64_t p_max); // Set the limit and return its previous value.
};
