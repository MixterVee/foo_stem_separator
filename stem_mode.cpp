#include "stem_mode.h"

namespace stemmode {

static std::atomic<int> g_mode{static_cast<int>(mode::original)};
static std::atomic<int> g_vocal_percent{100};
static std::atomic<int> g_instrumental_percent{100};

static int clamp_percent(int value) noexcept {
    if (value < 0) return 0;
    if (value > 100) return 100;
    return value;
}

mode get() noexcept {
    return static_cast<mode>(g_mode.load(std::memory_order_relaxed));
}

void set(mode value) noexcept {
    g_mode.store(static_cast<int>(value), std::memory_order_relaxed);
}

int vocal_percent() noexcept {
    return g_vocal_percent.load(std::memory_order_relaxed);
}

int instrumental_percent() noexcept {
    return g_instrumental_percent.load(std::memory_order_relaxed);
}

void set_vocal_percent(int value) noexcept {
    g_vocal_percent.store(clamp_percent(value), std::memory_order_relaxed);
}

void set_instrumental_percent(int value) noexcept {
    g_instrumental_percent.store(clamp_percent(value), std::memory_order_relaxed);
}

const char* name(mode value) noexcept {
    switch (value) {
    case mode::original: return "Original";
    case mode::vocals: return "Vocals";
    case mode::instrumental: return "Instrumental";
    case mode::blend: return "Blend";
    default: return "Unknown";
    }
}

} // namespace stemmode
