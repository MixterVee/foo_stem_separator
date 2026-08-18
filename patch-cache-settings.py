from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{label}: expected 1 marker, found {count}')
    return text.replace(old, new, 1)

# -----------------------------------------------------------------------------
# persistent_stem_cache.h
# -----------------------------------------------------------------------------
h = Path('persistent_stem_cache.h')
s = h.read_text(encoding='utf-8-sig')
s = replace_once(
    s,
    '''std::vector<segment> load(const std::wstring& source_path);\n\nbool save(\n''',
    '''bool enabled();\nvoid set_enabled(bool value);\n\nunsigned max_gb();\nuint64_t max_bytes();\nvoid set_max_gb(unsigned value);\n\nuint64_t current_size_bytes();\nbool clear();\n\nstd::vector<segment> load(const std::wstring& source_path);\n\nbool save(\n''',
    'cache settings API')
h.write_text(s, encoding='utf-8')

# -----------------------------------------------------------------------------
# persistent_stem_cache.cpp
# -----------------------------------------------------------------------------
p = Path('persistent_stem_cache.cpp')
s = p.read_text(encoding='utf-8-sig')
s = replace_once(
    s,
    '#include "persistent_stem_cache.h"\n',
    '#include <foobar2000/SDK/foobar2000.h>\n#include "persistent_stem_cache.h"\n',
    'foobar SDK include')

s = replace_once(
    s,
    '''constexpr uint64_t kMaxSegmentFrames = static_cast<uint64_t>(kRate) * 10u;\nconstexpr uint64_t kMaxCacheBytes = 10ull * 1024ull * 1024ull * 1024ull;\nconstexpr uint64_t kTrimCacheBytes = 8ull * 1024ull * 1024ull * 1024ull;\nconstexpr ULONGLONG kCleanupIntervalMs = 60ull * 1000ull;\n''',
    '''constexpr uint64_t kMaxSegmentFrames = static_cast<uint64_t>(kRate) * 10u;\nconstexpr uint64_t kBytesPerGiB = 1024ull * 1024ull * 1024ull;\nconstexpr ULONGLONG kCleanupIntervalMs = 60ull * 1000ull;\n''',
    'dynamic cache size constants')

s = replace_once(
    s,
    '''std::mutex g_cache_mutex;\nULONGLONG g_last_cleanup_tick = 0;\n''',
    '''static const GUID g_cache_enabled_guid =\n    {0x8e6f2201,0x3aa6,0x43c8,{0x9d,0x2e,0x81,0x6d,0x61,0xa4,0x77,0x10}};\nstatic const GUID g_cache_max_gb_guid =\n    {0x8e6f2202,0x3aa6,0x43c8,{0x9d,0x2e,0x81,0x6d,0x61,0xa4,0x77,0x10}};\n\ncfg_int g_cache_enabled_cfg(g_cache_enabled_guid, 1);\ncfg_int g_cache_max_gb_cfg(g_cache_max_gb_guid, 10);\n\nstd::mutex g_cache_mutex;\nULONGLONG g_last_cleanup_tick = 0;\n\nunsigned normalized_max_gb() {\n    const int configured = static_cast<int>(g_cache_max_gb_cfg.get());\n    if (configured < 1) return 10u;\n    return static_cast<unsigned>((std::min)(configured, 200));\n}\n\nuint64_t configured_max_bytes() {\n    return static_cast<uint64_t>(normalized_max_gb()) * kBytesPerGiB;\n}\n\nuint64_t configured_trim_bytes() {\n    return configured_max_bytes() * 4ull / 5ull;\n}\n''',
    'persistent cache config')

s = replace_once(
    s,
    '''    if (total_bytes <= kMaxCacheBytes) return;\n''',
    '''    if (total_bytes <= configured_max_bytes()) return;\n''',
    'dynamic max prune threshold')

s = replace_once(
    s,
    '''        if (total_bytes <= kTrimCacheBytes) break;\n''',
    '''        if (total_bytes <= configured_trim_bytes()) break;\n''',
    'dynamic trim threshold')

s = replace_once(
    s,
    '''} // namespace\n\nstd::vector<segment> load(const std::wstring& source_path) {\n''',
    '''} // namespace\n\nbool enabled() {\n    return static_cast<int>(g_cache_enabled_cfg.get()) != 0;\n}\n\nvoid set_enabled(bool value) {\n    g_cache_enabled_cfg = value ? 1 : 0;\n}\n\nunsigned max_gb() {\n    return normalized_max_gb();\n}\n\nuint64_t max_bytes() {\n    return configured_max_bytes();\n}\n\nvoid set_max_gb(unsigned value) {\n    value = (std::max)(1u, (std::min)(value, 200u));\n    g_cache_max_gb_cfg = static_cast<int>(value);\n\n    std::lock_guard<std::mutex> guard(g_cache_mutex);\n    g_last_cleanup_tick = 0;\n    prune_cache_locked({});\n}\n\nuint64_t current_size_bytes() {\n    std::lock_guard<std::mutex> guard(g_cache_mutex);\n    const fs::path root = root_path();\n    if (root.empty()) return 0;\n\n    std::error_code ec;\n    if (!fs::exists(root, ec) || ec) return 0;\n\n    uint64_t total = 0;\n    for (fs::recursive_directory_iterator it(root, ec), end;\n         !ec && it != end; it.increment(ec)) {\n        if (!it->is_regular_file(ec)) {\n            ec.clear();\n            continue;\n        }\n        const uint64_t bytes = static_cast<uint64_t>(it->file_size(ec));\n        if (!ec) total += bytes;\n        ec.clear();\n    }\n    return total;\n}\n\nbool clear() {\n    std::lock_guard<std::mutex> guard(g_cache_mutex);\n    const fs::path root = root_path();\n    if (root.empty()) return false;\n\n    std::error_code ec;\n    if (fs::exists(root, ec) && !ec) {\n        fs::remove_all(root, ec);\n        if (ec) return false;\n    }\n    g_last_cleanup_tick = 0;\n    return true;\n}\n\nstd::vector<segment> load(const std::wstring& source_path) {\n''',
    'public cache settings functions')

s = replace_once(
    s,
    '''std::vector<segment> load(const std::wstring& source_path) {\n    std::lock_guard<std::mutex> guard(g_cache_mutex);\n    std::vector<segment> out;\n\n''',
    '''std::vector<segment> load(const std::wstring& source_path) {\n    std::lock_guard<std::mutex> guard(g_cache_mutex);\n    std::vector<segment> out;\n\n    if (!enabled()) return out;\n\n''',
    'disabled load gate')

s = replace_once(
    s,
    '''    std::lock_guard<std::mutex> guard(g_cache_mutex);\n\n    if (source_path.empty() || vocals.empty() || vocals.size() != instrumental.size()) {\n''',
    '''    std::lock_guard<std::mutex> guard(g_cache_mutex);\n\n    if (!enabled()) return false;\n\n    if (source_path.empty() || vocals.empty() || vocals.size() != instrumental.size()) {\n''',
    'disabled save gate')

p.write_text(s, encoding='utf-8')

# -----------------------------------------------------------------------------
# foo_stem_separator.cpp menu UI
# -----------------------------------------------------------------------------
f = Path('foo_stem_separator.cpp')
s = f.read_text(encoding='utf-8-sig')
s = replace_once(
    s,
    '#include "onnx_stem_engine.h"\n#include "stem_mode.h"\n',
    '#include "onnx_stem_engine.h"\n#include "stem_mode.h"\n#include "persistent_stem_cache.h"\n',
    'cache header include')

s = replace_once(
    s,
    '''static contextmenu_group_popup_factory\n    g_stem_separator_context_group_factory(\n        g_stem_separator_context_group,\n        contextmenu_groups::root,\n        "Stem Separator",\n        0);\n\nclass stem_mode_context_menu :\n''',
    '''static contextmenu_group_popup_factory\n    g_stem_separator_context_group_factory(\n        g_stem_separator_context_group,\n        contextmenu_groups::root,\n        "Stem Separator",\n        0);\n\nstatic const GUID g_stem_cache_context_group =\n    {0x72a4f1d1,0x4ad3,0x4bb6,{0x98,0x2d,0x7f,0x42,0x31,0x90,0x45,0x11}};\nstatic contextmenu_group_popup_factory g_stem_cache_context_group_factory(\n    g_stem_cache_context_group,\n    g_stem_separator_context_group,\n    "Cache Settings",\n    50);\n\nstatic const GUID g_stem_cache_size_context_group =\n    {0x72a4f1d2,0x4ad3,0x4bb6,{0x98,0x2d,0x7f,0x42,0x31,0x90,0x45,0x12}};\nstatic contextmenu_group_popup_factory g_stem_cache_size_context_group_factory(\n    g_stem_cache_size_context_group,\n    g_stem_cache_context_group,\n    "Maximum Cache Size",\n    20);\n\nclass stem_cache_context_menu : public contextmenu_item_simple {\npublic:\n    GUID get_parent() override { return g_stem_cache_context_group; }\n\n    enum command_id : unsigned {\n        cmd_enabled = 0,\n        cmd_status,\n        cmd_clear,\n        cmd_count\n    };\n\n    unsigned get_num_items() override { return cmd_count; }\n\n    void get_item_name(unsigned index, pfc::string_base& out) override {\n        if (index == cmd_enabled) {\n            out = persistent_stem_cache::enabled()\n                ? "Persistent Cache: ON"\n                : "Persistent Cache: OFF";\n            return;\n        }\n        if (index == cmd_status) {\n            const uint64_t bytes = persistent_stem_cache::current_size_bytes();\n            char text[96] = {};\n            const double gib = static_cast<double>(bytes) /\n                (1024.0 * 1024.0 * 1024.0);\n            if (gib >= 1.0) {\n                _snprintf_s(text, sizeof(text), _TRUNCATE,\n                    "Current Cache: %.2f GB", gib);\n            } else {\n                const double mib = static_cast<double>(bytes) /\n                    (1024.0 * 1024.0);\n                _snprintf_s(text, sizeof(text), _TRUNCATE,\n                    "Current Cache: %.1f MB", mib);\n            }\n            out = text;\n            return;\n        }\n        if (index == cmd_clear) {\n            out = "Clear Stem Cache...";\n            return;\n        }\n        out = "Cache Settings";\n    }\n\n    GUID get_item_guid(unsigned index) override {\n        static const GUID ids[cmd_count] = {\n            {0xa92a1011,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x11}},\n            {0xa92a1012,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x12}},\n            {0xa92a1013,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x13}}\n        };\n        return ids[index < cmd_count ? index : 0];\n    }\n\n    bool get_item_description(unsigned index, pfc::string_base& out) override {\n        if (index == cmd_enabled) {\n            out = "Enable or disable loading and saving the persistent disk stem cache.";\n            return true;\n        }\n        if (index == cmd_status) {\n            out = "Show the current persistent stem cache size and configured maximum.";\n            return true;\n        }\n        if (index == cmd_clear) {\n            out = "Delete all persistent stem cache files from disk.";\n            return true;\n        }\n        return false;\n    }\n\n    void context_command(unsigned index, metadb_handle_list_cref, const GUID&) override {\n        if (index == cmd_enabled) {\n            const bool next = !persistent_stem_cache::enabled();\n            persistent_stem_cache::set_enabled(next);\n            console::print(next\n                ? "Stem Separator: persistent cache ON"\n                : "Stem Separator: persistent cache OFF");\n            return;\n        }\n\n        if (index == cmd_status) {\n            const uint64_t bytes = persistent_stem_cache::current_size_bytes();\n            const double gib = static_cast<double>(bytes) /\n                (1024.0 * 1024.0 * 1024.0);\n            wchar_t message[512] = {};\n            _snwprintf_s(\n                message, _countof(message), _TRUNCATE,\n                L"Persistent Cache: %s\\nCurrent size: %.2f GB\\nMaximum size: %u GB\\n\\n"\n                L"When the cache exceeds the maximum, least-recently-used whole-track "\n                L"caches are removed until usage falls to about 80%% of the limit.",\n                persistent_stem_cache::enabled() ? L"ON" : L"OFF",\n                gib,\n                persistent_stem_cache::max_gb());\n            MessageBoxW(nullptr, message, L"Stem Separator - Cache Status",\n                MB_OK | MB_ICONINFORMATION);\n            return;\n        }\n\n        if (index == cmd_clear) {\n            const int answer = MessageBoxW(\n                nullptr,\n                L"Delete all persistent stem cache files?\\n\\n"\n                L"The current track can continue from its in-memory cache. "\n                L"Newly processed stems may begin filling the disk cache again.",\n                L"Stem Separator - Clear Cache",\n                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);\n            if (answer != IDYES) return;\n\n            const bool ok = persistent_stem_cache::clear();\n            MessageBoxW(\n                nullptr,\n                ok ? L"Persistent stem cache cleared."\n                   : L"The persistent stem cache could not be completely cleared.",\n                L"Stem Separator",\n                MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));\n            return;\n        }\n    }\n};\n\nstatic contextmenu_item_factory_t<stem_cache_context_menu>\n    g_stem_cache_context_menu;\n\nclass stem_cache_size_context_menu : public contextmenu_item_simple {\npublic:\n    GUID get_parent() override { return g_stem_cache_size_context_group; }\n\n    static unsigned size_for(unsigned index) {\n        static const unsigned sizes[] = {2, 5, 10, 20, 50, 100};\n        return sizes[index < 6 ? index : 2];\n    }\n\n    unsigned get_num_items() override { return 6; }\n\n    void get_item_name(unsigned index, pfc::string_base& out) override {\n        const unsigned value = size_for(index);\n        pfc::string_formatter text;\n        text << value << " GB";\n        if (persistent_stem_cache::max_gb() == value) text << " (current)";\n        out = text;\n    }\n\n    GUID get_item_guid(unsigned index) override {\n        static const GUID ids[6] = {\n            {0xa92a1021,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x21}},\n            {0xa92a1022,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x22}},\n            {0xa92a1023,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x23}},\n            {0xa92a1024,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x24}},\n            {0xa92a1025,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x25}},\n            {0xa92a1026,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x26}}\n        };\n        return ids[index < 6 ? index : 2];\n    }\n\n    bool get_item_description(unsigned, pfc::string_base& out) override {\n        out = "Set the maximum size of the persistent stem cache. "\n              "If necessary, old track caches are removed immediately.";\n        return true;\n    }\n\n    void context_command(unsigned index, metadb_handle_list_cref, const GUID&) override {\n        const unsigned value = size_for(index);\n        persistent_stem_cache::set_max_gb(value);\n        pfc::string_formatter msg;\n        msg << "Stem Separator: persistent cache maximum set to "\n            << value << " GB";\n        console::print(msg);\n    }\n};\n\nstatic contextmenu_item_factory_t<stem_cache_size_context_menu>\n    g_stem_cache_size_context_menu;\n\nclass stem_mode_context_menu :\n''',
    'cache settings menu')

f.write_text(s, encoding='utf-8')
print('Applied cache settings test patch.')
