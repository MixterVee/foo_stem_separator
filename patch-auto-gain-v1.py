from pathlib import Path


def replace_once(path, old, new, label):
    p = Path(path)
    text = p.read_text(encoding='utf-8')
    if old not in text:
        raise RuntimeError(f'{label}: expected text not found in {path}')
    p.write_text(text.replace(old, new, 1), encoding='utf-8')


# -----------------------------------------------------------------------------
# Persistent Automatic Gain Matching setting, default ON.
# -----------------------------------------------------------------------------
replace_once(
    'foo_stem_separator_impl.cpp',
    '''} // namespace stem_precache\n\nnamespace persistent_stem_cache {''',
    '''} // namespace stem_precache\n\nnamespace stem_gain_match {\nnamespace {\nstatic const GUID g_gain_match_enabled_guid =\n    {0x8e6f2203,0x3aa6,0x43c8,{0x9d,0x2e,0x81,0x6d,0x61,0xa4,0x77,0x10}};\ncfg_int g_gain_match_enabled_cfg(g_gain_match_enabled_guid, 1);\n} // namespace\n\nbool enabled() {\n    return static_cast<int>(g_gain_match_enabled_cfg.get()) != 0;\n}\n\nvoid set_enabled(bool value) {\n    g_gain_match_enabled_cfg = value ? 1 : 0;\n}\n\n} // namespace stem_gain_match\n\nnamespace persistent_stem_cache {''',
    'persistent gain-match setting',
)


# -----------------------------------------------------------------------------
# Context-menu toggle. Keep this independent from the cache and Blend settings.
# -----------------------------------------------------------------------------
replace_once(
    'foo_stem_separator_impl.cpp',
    '''class stem_cache_context_menu : public contextmenu_item_simple {''',
    '''class stem_gain_match_context_menu : public contextmenu_item_simple {\npublic:\n    GUID get_parent() override { return g_stem_separator_context_group; }\n\n    unsigned get_num_items() override { return 1; }\n\n    void get_item_name(unsigned, pfc::string_base& out) override {\n        out = stem_gain_match::enabled()\n            ? "Automatic Gain Matching: ON"\n            : "Automatic Gain Matching: OFF";\n    }\n\n    GUID get_item_guid(unsigned) override {\n        return {0xa92a1050,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x50}};\n    }\n\n    bool get_item_description(unsigned, pfc::string_base& out) override {\n        out = "Smoothly match live Vocals, Instrumental and Blend playback level to the Original mix. "\n              "Correction is conservative and peak-protected; exports and cached stem files are unchanged.";\n        return true;\n    }\n\n    void context_command(unsigned, metadb_handle_list_cref, const GUID&) override {\n        const bool next = !stem_gain_match::enabled();\n        stem_gain_match::set_enabled(next);\n        console::print(next\n            ? "Stem Separator: automatic gain matching ON"\n            : "Stem Separator: automatic gain matching OFF");\n    }\n};\n\nstatic contextmenu_item_factory_t<stem_gain_match_context_menu>\n    g_stem_gain_match_context_menu;\n\nclass stem_cache_context_menu : public contextmenu_item_simple {''',
    'gain-match menu item',
)


# -----------------------------------------------------------------------------
# DSP: compare the live rendered stem chunk with the true Original chunk.
# Smooth correction between callbacks and cap normal correction to +/-6 dB.
# A peak guard may attenuate farther when necessary to avoid clipping.
# -----------------------------------------------------------------------------
replace_once(
    'stem_dsp.cpp',
    '''namespace stem_precache {\nbool enabled();\n}\n''',
    '''namespace stem_precache {\nbool enabled();\n}\n\nnamespace stem_gain_match {\nbool enabled();\n}\n''',
    'gain-match DSP declaration',
)

replace_once(
    'stem_dsp.cpp',
    '''constexpr double kFirstBlockFadeSeconds = 0.005;\n''',
    '''constexpr double kFirstBlockFadeSeconds = 0.005;\n\n// Automatic gain matching is deliberately conservative. The target is derived\n// from the true decoded Original chunk and the already-rendered live stem chunk,\n// so no cache-format change or second inference pass is needed.\nconstexpr float kGainMatchMinGain = 0.50118723f; // -6 dB\nconstexpr float kGainMatchMaxGain = 1.99526231f; // +6 dB\nconstexpr float kGainMatchPeakCeiling = 0.98f;\nconstexpr float kGainMatchBlockSmoothing = 0.12f;\nconstexpr double kGainMatchRmsFloor = 1.0e-5;\n\nfloat gain_match_target(\n    const audio_sample* original,\n    const std::vector<float>& rendered) {\n\n    if (!stem_gain_match::enabled() ||\n        original == nullptr ||\n        rendered.empty()) {\n        return 1.0f;\n    }\n\n    double original_energy = 0.0;\n    double stem_energy = 0.0;\n    float stem_peak = 0.0f;\n    size_t valid = 0;\n\n    for (size_t i = 0; i < rendered.size(); ++i) {\n        const double o = static_cast<double>(original[i]);\n        const double s = static_cast<double>(rendered[i]);\n        if (!std::isfinite(o) || !std::isfinite(s)) continue;\n\n        original_energy += o * o;\n        stem_energy += s * s;\n        stem_peak = (std::max)(stem_peak, static_cast<float>(std::abs(s)));\n        ++valid;\n    }\n\n    if (valid == 0) return 1.0f;\n\n    const double original_rms = std::sqrt(original_energy / static_cast<double>(valid));\n    const double stem_rms = std::sqrt(stem_energy / static_cast<double>(valid));\n\n    if (original_rms < kGainMatchRmsFloor ||\n        stem_rms < kGainMatchRmsFloor) {\n        return 1.0f;\n    }\n\n    float desired = static_cast<float>(original_rms / stem_rms);\n    desired = std::clamp(desired, kGainMatchMinGain, kGainMatchMaxGain);\n\n    if (stem_peak > 1.0e-6f) {\n        const float peak_limited = kGainMatchPeakCeiling / stem_peak;\n        if (peak_limited < desired) desired = (std::max)(0.0f, peak_limited);\n    }\n\n    return desired;\n}\n''',
    'gain-match constants and target function',
)

replace_once(
    'stem_dsp.cpp',
    '''        std::vector<audio_sample> output(\n            rendered.size());\n\n        const size_t fade_frames =''',
    '''        std::vector<audio_sample> output(\n            rendered.size());\n\n        const float raw_gain_target = gain_match_target(original, rendered);\n        const float gain_start = m_gainMatchCurrent;\n        const float gain_end = stem_gain_match::enabled()\n            ? gain_start +\n                (raw_gain_target - gain_start) * kGainMatchBlockSmoothing\n            : 1.0f;\n\n        const size_t fade_frames =''',
    'gain-match target in live playback',
)

replace_once(
    'stem_dsp.cpp',
    '''            float mix = 1.0f;\n\n            if (!m_using_stem &&''',
    '''            float mix = 1.0f;\n\n            const float gain_alpha = frames > 0\n                ? static_cast<float>(f + 1) / static_cast<float>(frames)\n                : 1.0f;\n            const float stem_gain =\n                gain_start + (gain_end - gain_start) * gain_alpha;\n\n            if (!m_using_stem &&''',
    'gain ramp per frame',
)

replace_once(
    'stem_dsp.cpp',
    '''                const float stem_sample =\n                    rendered[i];\n''',
    '''                const float stem_sample =\n                    rendered[i] * stem_gain;\n''',
    'apply gain to stem sample',
)

replace_once(
    'stem_dsp.cpp',
    '''        m_using_stem = true;\n\n        m_position_seconds +=''',
    '''        m_using_stem = true;\n        m_gainMatchCurrent = gain_end;\n\n        m_position_seconds +=''',
    'store smoothed gain',
)

replace_once(
    'stem_dsp.cpp',
    '''        m_scrubPreviousRate = 0.0;\n    }\n''',
    '''        m_scrubPreviousRate = 0.0;\n        m_gainMatchCurrent = 1.0f;\n    }\n''',
    'reset gain on flush',
)

replace_once(
    'stem_dsp.cpp',
    '''        m_scrubPreviousRate = 0.0;\n        m_scrubRenderPosition = 0.0;\n    }\n''',
    '''        m_scrubPreviousRate = 0.0;\n        m_scrubRenderPosition = 0.0;\n        m_gainMatchCurrent = 1.0f;\n    }\n''',
    'reset gain on track reset',
)

replace_once(
    'stem_dsp.cpp',
    '''    double m_scrubPreviousRate = 0.0;\n    double m_scrubRenderPosition = 0.0;\n};''',
    '''    double m_scrubPreviousRate = 0.0;\n    double m_scrubRenderPosition = 0.0;\n    float m_gainMatchCurrent = 1.0f;\n};''',
    'gain-match DSP member',
)


# Test identity only. Stable remains untouched until the user validates behavior.
replace_once(
    'foo_stem_separator.cpp',
    '"2.7.0 Stable Stem Blend",',
    '"2.8.0-test1 Automatic Gain Matching",',
    'test component version',
)

print('Automatic Gain Matching v1 patch applied successfully')
