#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace persistent_stem_cache {

struct segment {
    uint64_t start_frame = 0;
    std::vector<float> vocals;
    std::vector<float> instrumental;
};

bool enabled();
void set_enabled(bool value);

unsigned max_gb();
uint64_t max_bytes();
void set_max_gb(unsigned value);

uint64_t current_size_bytes();
bool clear();

namespace detail {
void enforce_limit();
}

std::vector<segment> load(const std::wstring& source_path);

bool save(
    const std::wstring& source_path,
    uint64_t start_frame,
    const std::vector<float>& vocals,
    const std::vector<float>& instrumental);

} // namespace persistent_stem_cache
