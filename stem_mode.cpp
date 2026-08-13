#include "stem_mode.h"

namespace stemmode {

static std::atomic<int> g_mode{static_cast<int>(mode::original)};

mode get() noexcept {
    return static_cast<mode>(g_mode.load(std::memory_order_relaxed));
}

void set(mode value) noexcept {
    g_mode.store(static_cast<int>(value), std::memory_order_relaxed);
}

const char* name(mode value) noexcept {
    switch (value) {
    case mode::original: return "Original";
    case mode::vocals: return "Vocals";
    case mode::instrumental: return "Instrumental";
    default: return "Unknown";
    }
}

} // namespace stemmode
