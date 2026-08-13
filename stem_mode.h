#pragma once
#include <atomic>

namespace stemmode {

enum class mode : int {
    original = 0,
    vocals = 1,
    instrumental = 2
};

mode get() noexcept;
void set(mode value) noexcept;
const char* name(mode value) noexcept;

} // namespace stemmode
