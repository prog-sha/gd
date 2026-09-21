// Share a lock between descriptor inheritance setup and fork.
#pragma once

#include <mutex>

namespace GDFork {
inline std::mutex mutex; // Protect creation through CLOEXEC setup where atomic creation flags are unavailable.
}
