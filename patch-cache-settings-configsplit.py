from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{label}: expected 1 marker, found {count}')
    return text.replace(old, new, 1)

# Keep the compressed cache implementation free of the full foobar SDK.
p = Path('persistent_stem_cache.cpp')
s = p.read_text(encoding='utf-8-sig')
s = replace_once(
    s,
    '#include <windows.h>\n#include <compressapi.h>\n#include <foobar2000/SDK/foobar2000.h>\n',
    '#include <windows.h>\n#include <compressapi.h>\n',
    'remove SDK include from compression module')

old_cfg = '''static const GUID g_cache_enabled_guid =
    {0x8e6f2201,0x3aa6,0x43c8,{0x9d,0x2e,0x81,0x6d,0x61,0xa4,0x77,0x10}};
static const GUID g_cache_max_gb_guid =
    {0x8e6f2202,0x3aa6,0x43c8,{0x9d,0x2e,0x81,0x6d,0x61,0xa4,0x77,0x10}};

cfg_int g_cache_enabled_cfg(g_cache_enabled_guid, 1);
cfg_int g_cache_max_gb_cfg(g_cache_max_gb_guid, 10);

std::mutex g_cache_mutex;
ULONGLONG g_last_cleanup_tick = 0;

unsigned normalized_max_gb() {
    const int configured = static_cast<int>(g_cache_max_gb_cfg.get());
    if (configured < 1) return 10u;
    return static_cast<unsigned>(configured > 200 ? 200 : configured);
}

uint64_t configured_max_bytes() {
    return static_cast<uint64_t>(normalized_max_gb()) * kBytesPerGiB;
}

uint64_t configured_trim_bytes() {
    return configured_max_bytes() * 4ull / 5ull;
}
'''
new_cfg = '''std::mutex g_cache_mutex;
ULONGLONG g_last_cleanup_tick = 0;

uint64_t configured_max_bytes() {
    return max_bytes();
}

uint64_t configured_trim_bytes() {
    return configured_max_bytes() * 4ull / 5ull;
}
'''
s = replace_once(s, old_cfg, new_cfg, 'move cfg storage out of compression module')

old_public = '''bool enabled() {
    return static_cast<int>(g_cache_enabled_cfg.get()) != 0;
}

void set_enabled(bool value) {
    g_cache_enabled_cfg = value ? 1 : 0;
}

unsigned max_gb() {
    return normalized_max_gb();
}

uint64_t max_bytes() {
    return configured_max_bytes();
}

void set_max_gb(unsigned value) {
    if (value < 1u) value = 1u;
    if (value > 200u) value = 200u;
    g_cache_max_gb_cfg = static_cast<int>(value);

    std::lock_guard<std::mutex> guard(g_cache_mutex);
    g_last_cleanup_tick = 0;
    prune_cache_locked({});
}

uint64_t current_size_bytes() {
'''
new_public = '''uint64_t max_bytes() {
    return static_cast<uint64_t>(max_gb()) * kBytesPerGiB;
}

namespace detail {
void enforce_limit() {
    std::lock_guard<std::mutex> guard(g_cache_mutex);
    g_last_cleanup_tick = 0;
    prune_cache_locked({});
}
} // namespace detail

uint64_t current_size_bytes() {
'''
s = replace_once(s, old_public, new_public, 'move cfg accessors out of compression module')
p.write_text(s, encoding='utf-8')

# Add the small internal hook used after changing the configured limit.
h = Path('persistent_stem_cache.h')
s = h.read_text(encoding='utf-8-sig')
s = replace_once(
    s,
    '''uint64_t current_size_bytes();
bool clear();

std::vector<segment> load''',
    '''uint64_t current_size_bytes();
bool clear();

namespace detail {
void enforce_limit();
}

std::vector<segment> load''',
    'detail enforcement declaration')
h.write_text(s, encoding='utf-8')

# Store foobar-owned settings in the existing SDK-facing component TU.
f = Path('foo_stem_separator.cpp')
s = f.read_text(encoding='utf-8-sig')
marker = '''} // namespace stem_precache

namespace {
'''
insert = '''} // namespace stem_precache

namespace persistent_stem_cache {
namespace {
static const GUID g_cache_enabled_guid =
    {0x8e6f2201,0x3aa6,0x43c8,{0x9d,0x2e,0x81,0x6d,0x61,0xa4,0x77,0x10}};
static const GUID g_cache_max_gb_guid =
    {0x8e6f2202,0x3aa6,0x43c8,{0x9d,0x2e,0x81,0x6d,0x61,0xa4,0x77,0x10}};

cfg_int g_cache_enabled_cfg(g_cache_enabled_guid, 1);
cfg_int g_cache_max_gb_cfg(g_cache_max_gb_guid, 10);
} // namespace

bool enabled() {
    return static_cast<int>(g_cache_enabled_cfg.get()) != 0;
}

void set_enabled(bool value) {
    g_cache_enabled_cfg = value ? 1 : 0;
}

unsigned max_gb() {
    const int configured = static_cast<int>(g_cache_max_gb_cfg.get());
    if (configured < 1) return 10u;
    return static_cast<unsigned>(configured > 200 ? 200 : configured);
}

void set_max_gb(unsigned value) {
    if (value < 1u) value = 1u;
    if (value > 200u) value = 200u;
    g_cache_max_gb_cfg = static_cast<int>(value);
    detail::enforce_limit();
}

} // namespace persistent_stem_cache

namespace {
'''
s = replace_once(s, marker, insert, 'SDK-owned cache config block')
f.write_text(s, encoding='utf-8')

print('Moved persistent cache configuration into SDK-facing component source.')
