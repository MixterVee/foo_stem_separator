#pragma once
#include <atomic>

namespace stemmode {

enum class mode : int {
    original = 0,
    vocals = 1,
    instrumental = 2,
    blend = 3
};

mode get() noexcept;
void set(mode value) noexcept;

int vocal_percent() noexcept;
int instrumental_percent() noexcept;
void set_vocal_percent(int value) noexcept;
void set_instrumental_percent(int value) noexcept;

const char* name(mode value) noexcept;

} // namespace stemmode
