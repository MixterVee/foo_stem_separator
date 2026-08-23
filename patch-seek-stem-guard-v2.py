from pathlib import Path
import runpy

# Apply test1 first so v2 preserves the fail-closed post-seek guard.
runpy.run_path('patch-seek-stem-guard-v1.py', run_name='__main__')


def replace_once(path, old, new, label):
    p = Path(path)
    text = p.read_text(encoding='utf-8')
    if old not in text:
        raise RuntimeError(f'{label}: expected text not found in {path}')
    p.write_text(text.replace(old, new, 1), encoding='utf-8')


# Transport release must not consider Spectral Waveform's external analysis tiles
# sufficient for audible playback. Only Stem Separator's own internal PCM is
# guaranteed to be accepted by the subsequent normal-playback path.
replace_once(
    'stem_dsp.cpp',
    '''        std::lock_guard<std::mutex> lock(m_mutex);\n        for (const auto& seg_ptr : m_segments) {\n                const cache_segment& seg = *seg_ptr;\n            if (!segment_has_mode(seg, mode)) continue;\n            if (position_seconds >= seg.start_seconds &&\n                position_seconds < seg.end_seconds) {\n                return true;\n            }\n        }''',
    '''        std::lock_guard<std::mutex> lock(m_mutex);\n        for (const auto& seg_ptr : m_segments) {\n            const cache_segment& seg = *seg_ptr;\n            if (seg.external_waveform) continue;\n            if (!segment_has_mode(seg, mode)) continue;\n            if (position_seconds >= seg.start_seconds &&\n                position_seconds < seg.end_seconds) {\n                return true;\n            }\n        }''',
    'internal-only transport readiness',
)

# Likewise, an external waveform tile must not suppress the on-demand internal
# transport job. If the internal stem range is absent, queue Spleeter work even
# when Spectral has already published a visual-analysis tile for that location.
replace_once(
    'stem_dsp.cpp',
    '''        if (range_ready_locked(mode, need_start, need_end)) return;''',
    '''        if (original_preview) {\n            if (range_ready_locked(mode, need_start, need_end)) return;\n        } else {\n            if (internal_stem_range_ready_locked(need_start, need_end)) return;\n        }''',
    'internal-only separated transport range check',
)

# RELEASE_WAIT itself must render from internal stem PCM only. If that PCM is not
# ready yet, its existing fail-closed behavior writes silence and keeps waiting.
replace_once(
    'stem_dsp.cpp',
    '''                if (cache_manager().render(\n                        mode, m_position_seconds, rate, frames, preview, 1.0) &&''',
    '''                if (cache_manager().render(\n                        mode, m_position_seconds, rate, frames, preview, 1.0, false) &&''',
    'disable external fallback during release wait',
)

replace_once(
    'foo_stem_separator.cpp',
    '"2.8.1-test1 Seek Stem Guard",',
    '"2.8.1-test2 Internal Seek Stem Guard",',
    'test2 component version',
)

print('Seek Stem Guard v2 patch applied successfully')
