from pathlib import Path
import re


def replace_once(path, old, new, label):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    if old not in text:
        raise RuntimeError(f"{label}: expected text not found in {path}")
    text = text.replace(old, new, 1)
    p.write_text(text, encoding="utf-8")


def regex_once(path, pattern, replacement, label):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    text2, count = re.subn(pattern, replacement, text, count=1, flags=re.S)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match in {path}, got {count}")
    p.write_text(text2, encoding="utf-8")


# -----------------------------------------------------------------------------
# stem_mode: add a fourth mode plus future-proof independent stem gains.
# -----------------------------------------------------------------------------
replace_once(
    "stem_mode.h",
    """enum class mode : int {\n    original = 0,\n    vocals = 1,\n    instrumental = 2\n};\n\nmode get() noexcept;\nvoid set(mode value) noexcept;\nconst char* name(mode value) noexcept;\n""",
    """enum class mode : int {\n    original = 0,\n    vocals = 1,\n    instrumental = 2,\n    blend = 3\n};\n\nmode get() noexcept;\nvoid set(mode value) noexcept;\n\nint vocal_percent() noexcept;\nint instrumental_percent() noexcept;\nvoid set_vocal_percent(int value) noexcept;\nvoid set_instrumental_percent(int value) noexcept;\n\nconst char* name(mode value) noexcept;\n""",
    "stem_mode declarations",
)

replace_once(
    "stem_mode.cpp",
    """static std::atomic<int> g_mode{static_cast<int>(mode::original)};\n""",
    """static std::atomic<int> g_mode{static_cast<int>(mode::original)};\nstatic std::atomic<int> g_vocal_percent{100};\nstatic std::atomic<int> g_instrumental_percent{100};\n\nstatic int clamp_percent(int value) noexcept {\n    if (value < 0) return 0;\n    if (value > 100) return 100;\n    return value;\n}\n""",
    "stem gain atomics",
)

replace_once(
    "stem_mode.cpp",
    """void set(mode value) noexcept {\n    g_mode.store(static_cast<int>(value), std::memory_order_relaxed);\n}\n\nconst char* name(mode value) noexcept {\n""",
    """void set(mode value) noexcept {\n    g_mode.store(static_cast<int>(value), std::memory_order_relaxed);\n}\n\nint vocal_percent() noexcept {\n    return g_vocal_percent.load(std::memory_order_relaxed);\n}\n\nint instrumental_percent() noexcept {\n    return g_instrumental_percent.load(std::memory_order_relaxed);\n}\n\nvoid set_vocal_percent(int value) noexcept {\n    g_vocal_percent.store(clamp_percent(value), std::memory_order_relaxed);\n}\n\nvoid set_instrumental_percent(int value) noexcept {\n    g_instrumental_percent.store(clamp_percent(value), std::memory_order_relaxed);\n}\n\nconst char* name(mode value) noexcept {\n""",
    "stem gain accessors",
)

replace_once(
    "stem_mode.cpp",
    """    case mode::instrumental: return \"Instrumental\";\n    default: return \"Unknown\";\n""",
    """    case mode::instrumental: return \"Instrumental\";\n    case mode::blend: return \"Blend\";\n    default: return \"Unknown\";\n""",
    "blend mode name",
)


# -----------------------------------------------------------------------------
# DSP cache renderer: Blend reads the already-cached vocal + instrumental PCM.
# No extra inference and no cache format change.
# -----------------------------------------------------------------------------
replace_once(
    "stem_dsp.cpp",
    """        if (output_rate == 0 ||\n            frames == 0) {\n            return false;\n        }\n\n        g_dbg_render_attempts.fetch_add(1, std::memory_order_relaxed);\n""",
    """        if (output_rate == 0 ||\n            frames == 0) {\n            return false;\n        }\n\n        const float blend_vocal_gain =\n            mode == stemmode::mode::blend\n                ? static_cast<float>(stemmode::vocal_percent()) / 100.0f\n                : 0.0f;\n        const float blend_instrumental_gain =\n            mode == stemmode::mode::blend\n                ? static_cast<float>(stemmode::instrumental_percent()) / 100.0f\n                : 0.0f;\n\n        g_dbg_render_attempts.fetch_add(1, std::memory_order_relaxed);\n""",
    "render gain snapshot",
)

replace_once(
    "stem_dsp.cpp",
    """    static bool segment_has_mode(const cache_segment& seg, stemmode::mode mode) {\n        if (mode == stemmode::mode::original) return !seg.original.empty();\n        if (mode == stemmode::mode::vocals) return !seg.vocals.empty();\n        return !seg.instrumental.empty();\n    }\n""",
    """    static bool segment_has_mode(const cache_segment& seg, stemmode::mode mode) {\n        if (mode == stemmode::mode::original) return !seg.original.empty();\n        if (mode == stemmode::mode::vocals) return !seg.vocals.empty();\n        if (mode == stemmode::mode::instrumental) return !seg.instrumental.empty();\n        if (mode == stemmode::mode::blend) {\n            return !seg.vocals.empty() && !seg.instrumental.empty();\n        }\n        return false;\n    }\n""",
    "segment blend readiness",
)

regex_once(
    "stem_dsp.cpp",
    r"            auto sample_from =\n.*?            };\n\n            for \(unsigned ch = 0;",
    """            auto sample_vector =\n                [t](\n                    const std::vector<float>& data,\n                    const cache_segment& seg,\n                    unsigned ch) -> float {\n\n                const double rel =\n                    t - seg.start_seconds;\n\n                double source_pos =\n                    rel * static_cast<double>(kCacheRate);\n\n                if (source_pos < 0.0) {\n                    source_pos = 0.0;\n                }\n\n                const size_t total_frames =\n                    data.size() / kCacheChannels;\n\n                if (total_frames == 0) {\n                    return 0.0f;\n                }\n\n                const double max_pos =\n                    static_cast<double>(total_frames - 1);\n                if (source_pos > max_pos) {\n                    source_pos = max_pos;\n                }\n\n                const size_t i0 =\n                    static_cast<size_t>(source_pos);\n                const size_t im1 = i0 > 0 ? i0 - 1 : i0;\n                const size_t i1 = (std::min)(i0 + 1, total_frames - 1);\n                const size_t i2 = (std::min)(i0 + 2, total_frames - 1);\n                const float frac = static_cast<float>(\n                    source_pos - static_cast<double>(i0));\n\n                return scratch_hermite4(\n                    frac,\n                    data[im1 * kCacheChannels + ch],\n                    data[i0 * kCacheChannels + ch],\n                    data[i1 * kCacheChannels + ch],\n                    data[i2 * kCacheChannels + ch]);\n            };\n\n            auto sample_from =\n                [mode, blend_vocal_gain, blend_instrumental_gain, &sample_vector](\n                    const cache_segment& seg,\n                    unsigned ch) -> float {\n\n                if (mode == stemmode::mode::blend) {\n                    return\n                        sample_vector(seg.vocals, seg, ch) * blend_vocal_gain +\n                        sample_vector(seg.instrumental, seg, ch) * blend_instrumental_gain;\n                }\n\n                const std::vector<float>& data =\n                    mode == stemmode::mode::original\n                        ? seg.original\n                        : (mode == stemmode::mode::vocals\n                            ? seg.vocals\n                            : seg.instrumental);\n\n                return sample_vector(data, seg, ch);\n            };\n\n            for (unsigned ch = 0;""",
    "cache sample mixer",
)


# -----------------------------------------------------------------------------
# Stem Separator context menu UI.
# -----------------------------------------------------------------------------
impl = Path("foo_stem_separator_impl.cpp")
text = impl.read_text(encoding="utf-8")

anchor = """static contextmenu_group_popup_factory g_stem_cache_size_context_group_factory(\n    g_stem_cache_size_context_group,\n    g_stem_cache_context_group,\n    \"Maximum Cache Size\",\n    20);\n"""
if anchor not in text:
    raise RuntimeError("blend menu group anchor not found")
text = text.replace(anchor, anchor + """\nstatic const GUID g_stem_blend_context_group =\n    {0x72a4f1e1,0x4ad3,0x4bb6,{0x98,0x2d,0x7f,0x42,0x31,0x90,0x45,0x21}};\nstatic contextmenu_group_popup_factory g_stem_blend_context_group_factory(\n    g_stem_blend_context_group,\n    g_stem_separator_context_group,\n    \"Stem Blend\",\n    20);\n\nstatic const GUID g_stem_blend_vocal_context_group =\n    {0x72a4f1e2,0x4ad3,0x4bb6,{0x98,0x2d,0x7f,0x42,0x31,0x90,0x45,0x22}};\nstatic contextmenu_group_popup_factory g_stem_blend_vocal_context_group_factory(\n    g_stem_blend_vocal_context_group,\n    g_stem_blend_context_group,\n    \"Vocals\",\n    0);\n\nstatic const GUID g_stem_blend_instrumental_context_group =\n    {0x72a4f1e3,0x4ad3,0x4bb6,{0x98,0x2d,0x7f,0x42,0x31,0x90,0x45,0x23}};\nstatic contextmenu_group_popup_factory g_stem_blend_instrumental_context_group_factory(\n    g_stem_blend_instrumental_context_group,\n    g_stem_blend_context_group,\n    \"Instrumental\",\n    10);\n""", 1)

blend_classes = r'''
class stem_blend_vocal_context_menu : public contextmenu_item_simple {
public:
    GUID get_parent() override { return g_stem_blend_vocal_context_group; }

    static int level_for(unsigned index) {
        static const int levels[] = {0, 25, 50, 75, 100};
        return levels[index < 5 ? index : 4];
    }

    unsigned get_num_items() override { return 5; }

    void get_item_name(unsigned index, pfc::string_base& out) override {
        const int value = level_for(index);
        pfc::string_formatter text;
        text << value << "%";
        if (stemmode::vocal_percent() == value) text << " (current)";
        out = text;
    }

    GUID get_item_guid(unsigned index) override {
        static const GUID ids[5] = {
            {0xa92a1030,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x30}},
            {0xa92a1031,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x31}},
            {0xa92a1032,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x32}},
            {0xa92a1033,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x33}},
            {0xa92a1034,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x34}}
        };
        return ids[index < 5 ? index : 4];
    }

    bool get_item_description(unsigned, pfc::string_base& out) override {
        out = "Set the vocal level for Stem Blend. Selecting a level activates Blend mode.";
        return true;
    }

    void context_command(unsigned index, metadb_handle_list_cref, const GUID&) override {
        const int value = level_for(index);
        stemmode::set_vocal_percent(value);
        stemmode::set(stemmode::mode::blend);
        pfc::string_formatter msg;
        msg << "Stem Blend: vocals " << value << "% / instrumental "
            << stemmode::instrumental_percent() << "%";
        console::print(msg);
    }
};

static contextmenu_item_factory_t<stem_blend_vocal_context_menu>
    g_stem_blend_vocal_context_menu;

class stem_blend_instrumental_context_menu : public contextmenu_item_simple {
public:
    GUID get_parent() override { return g_stem_blend_instrumental_context_group; }

    static int level_for(unsigned index) {
        static const int levels[] = {0, 25, 50, 75, 100};
        return levels[index < 5 ? index : 4];
    }

    unsigned get_num_items() override { return 5; }

    void get_item_name(unsigned index, pfc::string_base& out) override {
        const int value = level_for(index);
        pfc::string_formatter text;
        text << value << "%";
        if (stemmode::instrumental_percent() == value) text << " (current)";
        out = text;
    }

    GUID get_item_guid(unsigned index) override {
        static const GUID ids[5] = {
            {0xa92a1040,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x40}},
            {0xa92a1041,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x41}},
            {0xa92a1042,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x42}},
            {0xa92a1043,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x43}},
            {0xa92a1044,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x44}}
        };
        return ids[index < 5 ? index : 4];
    }

    bool get_item_description(unsigned, pfc::string_base& out) override {
        out = "Set the instrumental level for Stem Blend. Selecting a level activates Blend mode.";
        return true;
    }

    void context_command(unsigned index, metadb_handle_list_cref, const GUID&) override {
        const int value = level_for(index);
        stemmode::set_instrumental_percent(value);
        stemmode::set(stemmode::mode::blend);
        pfc::string_formatter msg;
        msg << "Stem Blend: vocals " << stemmode::vocal_percent()
            << "% / instrumental " << value << "%";
        console::print(msg);
    }
};

static contextmenu_item_factory_t<stem_blend_instrumental_context_menu>
    g_stem_blend_instrumental_context_menu;

'''

anchor2 = """static contextmenu_item_factory_t<stem_cache_size_context_menu>\n    g_stem_cache_size_context_menu;\n\nclass stem_mode_context_menu :\n"""
if anchor2 not in text:
    raise RuntimeError("blend menu class anchor not found")
text = text.replace(
    anchor2,
    """static contextmenu_item_factory_t<stem_cache_size_context_menu>\n    g_stem_cache_size_context_menu;\n\n""" + blend_classes + "class stem_mode_context_menu :\n",
    1,
)

text = text.replace(
    """        cmd_original = 0,\n        cmd_vocals,\n        cmd_instrumental,\n        cmd_save_vocals,\n""",
    """        cmd_original = 0,\n        cmd_vocals,\n        cmd_instrumental,\n        cmd_blend,\n        cmd_save_vocals,\n""",
    1,
)

text = text.replace(
    """        case cmd_instrumental:\n            out = \"Instrumental\";\n            break;\n\n        case cmd_save_vocals:\n""",
    """        case cmd_instrumental:\n            out = \"Instrumental\";\n            break;\n\n        case cmd_blend: {\n            pfc::string_formatter text;\n            text << \"Blend (V \" << stemmode::vocal_percent()\n                 << \"% / I \" << stemmode::instrumental_percent() << \"%)\";\n            out = text;\n            break;\n        }\n\n        case cmd_save_vocals:\n""",
    1,
)

instrumental_guid = """            {0xa92a1003,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x03}},\n"""
if instrumental_guid not in text:
    raise RuntimeError("instrumental command GUID anchor not found")
text = text.replace(
    instrumental_guid,
    instrumental_guid + """            {0xa92a1009,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x09}},\n""",
    1,
)

text = text.replace(
    """        case cmd_instrumental:\n            out = \"Play the Spleeter accompaniment stem.\";\n            return true;\n\n        case cmd_save_vocals:\n""",
    """        case cmd_instrumental:\n            out = \"Play the Spleeter accompaniment stem.\";\n            return true;\n\n        case cmd_blend:\n            out = \"Play the cached vocals and instrumental together using the current Stem Blend levels.\";\n            return true;\n\n        case cmd_save_vocals:\n""",
    1,
)

text = text.replace(
    """        if (index == cmd_vocals) {\n            next = stemmode::mode::vocals;\n        }\n        else if (index == cmd_instrumental) {\n            next = stemmode::mode::instrumental;\n        }\n\n        stemmode::set(next);\n""",
    """        if (index == cmd_vocals) {\n            next = stemmode::mode::vocals;\n        }\n        else if (index == cmd_instrumental) {\n            next = stemmode::mode::instrumental;\n        }\n        else if (index == cmd_blend) {\n            next = stemmode::mode::blend;\n        }\n\n        stemmode::set(next);\n""",
    1,
)

impl.write_text(text, encoding="utf-8")

# Test-build identity only; stable main remains 2.6.0.
replace_once(
    "foo_stem_separator.cpp",
    '"2.6.0 Persistent Cache & Cache Settings",',
    '"2.7.0-test1 Stem Blend",',
    "test component version",
)

print("Stem Blend v1 patch applied successfully")
