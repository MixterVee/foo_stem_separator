from pathlib import Path


def replace_once(path, old, new, label):
    p = Path(path)
    text = p.read_text(encoding='utf-8')
    if old not in text:
        raise RuntimeError(f'{label}: expected text not found in {path}')
    p.write_text(text.replace(old, new, 1), encoding='utf-8')


# Re-arm a guard only for a real non-track-start seek while a separated mode is
# selected. Track-start pre-cache retains its existing behavior.
replace_once(
    'stem_dsp.cpp',
    '''            m_have_position = true;\n            m_using_stem = false;\n            m_precache_handled = false;\n        }''',
    '''            m_have_position = true;\n            m_using_stem = false;\n            m_precache_handled = false;\n            m_seekStemGuard =\n                stemmode::get() != stemmode::mode::original &&\n                !cache_manager().is_track_start_generation(m_generation);\n        }''',
    'arm post-seek stem guard',
)

# A transport release already has its own stem-safe silent wait. Once it renders
# the selected stem successfully, clear the normal-playback seek guard so there
# is no extra fade/dip on the following chunk.
replace_once(
    'stem_dsp.cpp',
    '''                    write_preview(preview);\n                    transport().finish_release();\n                    m_using_stem = true;''',
    '''                    write_preview(preview);\n                    transport().finish_release();\n                    m_using_stem = true;\n                    m_seekStemGuard = false;''',
    'clear guard after transport release',
)

# Original mode is never guarded.
replace_once(
    'stem_dsp.cpp',
    '''            m_precache_handled = true;\n\n            m_position_seconds +=\n''',
    '''            m_precache_handled = true;\n            m_seekStemGuard = false;\n\n            m_position_seconds +=\n''',
    'clear guard in original mode',
)

# Preserve V23's nonblocking behavior for ordinary forward cache misses, but a
# post-seek miss in Vocal/Instrumental/Blend must never leak Original. Output
# silence while the already-queued seek job catches up.
replace_once(
    'stem_dsp.cpp',
    '''        if (!have_cache ||\n            rendered.size() !=\n                frames *\n                kCacheChannels) {\n\n            // Critical V23 behavior:\n            // NEVER stall foobar waiting for Spleeter.\n            // After a seek we temporarily play the original mix until\n            // the position-indexed cache catches up.\n            m_position_seconds +=\n                static_cast<double>(\n                    frames) /\n                static_cast<double>(\n                    rate);\n\n            m_using_stem = false;\n            return true;\n        }''',
    '''        if (!have_cache ||\n            rendered.size() !=\n                frames *\n                kCacheChannels) {\n\n            // Never stall foobar waiting for Spleeter. Ordinary forward-playback\n            // misses retain the historical Original fallback, but immediately\n            // after a seek a selected stem must be fail-closed: silence is safer\n            // than briefly exposing the full mix. The seek worker is already\n            // queued by live_cache_manager::seek().\n            if (m_seekStemGuard) {\n                std::vector<audio_sample> zeros(\n                    frames * kCacheChannels, 0);\n                chunk->set_data(\n                    zeros.data(),\n                    frames,\n                    channels,\n                    rate,\n                    chunk->get_channel_config());\n            }\n\n            m_position_seconds +=\n                static_cast<double>(\n                    frames) /\n                static_cast<double>(\n                    rate);\n\n            m_using_stem = false;\n            return true;\n        }''',
    'silence post-seek cache miss',
)

# When the first stem block becomes available after the silent guard, do not use
# the normal Original->stem crossfade (which would reintroduce instruments). Fade
# the stem itself in from silence over 5 ms instead.
replace_once(
    'stem_dsp.cpp',
    '''        const size_t fade_frames =\n            static_cast<size_t>(\n                kSwitchFadeSeconds *\n                static_cast<double>(\n                    rate));\n''',
    '''        const bool seek_stem_handoff = m_seekStemGuard;\n        const size_t fade_frames =\n            static_cast<size_t>(\n                kSwitchFadeSeconds *\n                static_cast<double>(\n                    rate));\n        const size_t seek_fade_frames =\n            static_cast<size_t>(\n                kFirstBlockFadeSeconds *\n                static_cast<double>(\n                    rate));\n''',
    'post-seek handoff fade setup',
)

replace_once(
    'stem_dsp.cpp',
    '''            if (!m_using_stem &&\n                fade_frames > 0 &&\n                f < fade_frames) {''',
    '''            if (!seek_stem_handoff &&\n                !m_using_stem &&\n                fade_frames > 0 &&\n                f < fade_frames) {''',
    'disable original crossfade after seek',
)

replace_once(
    'stem_dsp.cpp',
    '''                const float stem_sample =\n                    rendered[i] * stem_gain;\n\n                const float original_sample =''',
    '''                float stem_sample =\n                    rendered[i] * stem_gain;\n\n                if (seek_stem_handoff &&\n                    seek_fade_frames > 0 &&\n                    f < seek_fade_frames) {\n                    const float seek_gain =\n                        static_cast<float>(f + 1) /\n                        static_cast<float>(seek_fade_frames);\n                    stem_sample *= seek_gain;\n                }\n\n                const float original_sample =''',
    'fade stem from silence after seek',
)

replace_once(
    'stem_dsp.cpp',
    '''                const float v =\n                    original_sample *\n                        (1.0f - mix) +\n                    stem_sample *\n                        mix;''',
    '''                const float v =\n                    seek_stem_handoff\n                        ? stem_sample\n                        : original_sample *\n                            (1.0f - mix) +\n                          stem_sample *\n                            mix;''',
    'never mix original into guarded handoff',
)

replace_once(
    'stem_dsp.cpp',
    '''        m_using_stem = true;\n        m_gainMatchCurrent = gain_end;''',
    '''        m_using_stem = true;\n        m_seekStemGuard = false;\n        m_gainMatchCurrent = gain_end;''',
    'clear guard after first successful stem block',
)

replace_once(
    'stem_dsp.cpp',
    '''        m_precache_handled = false;\n        m_transportTailValid = false;''',
    '''        m_precache_handled = false;\n        m_seekStemGuard = false;\n        m_transportTailValid = false;''',
    'reset guard on flush',
)

replace_once(
    'stem_dsp.cpp',
    '''        m_precache_handled = false;\n        m_position_seconds = 0.0;''',
    '''        m_precache_handled = false;\n        m_seekStemGuard = false;\n        m_position_seconds = 0.0;''',
    'reset guard on track reset',
)

replace_once(
    'stem_dsp.cpp',
    '''    bool m_have_position = false;\n    bool m_using_stem = false;\n    bool m_precache_handled = false;''',
    '''    bool m_have_position = false;\n    bool m_using_stem = false;\n    bool m_precache_handled = false;\n    bool m_seekStemGuard = false;''',
    'seek guard member',
)

replace_once(
    'foo_stem_separator.cpp',
    '"2.8.0 Stable Automatic Gain Matching",',
    '"2.8.1-test1 Seek Stem Guard",',
    'test component version',
)

print('Seek Stem Guard v1 patch applied successfully')
