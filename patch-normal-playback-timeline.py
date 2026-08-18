from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8-sig')

def repl(old, new, label):
    global s
    if s.count(old) != 1:
        raise RuntimeError(f'{label}: expected 1 marker, found {s.count(old)}')
    s = s.replace(old, new, 1)

repl(
'''        std::vector<float>& out,\n        double source_rate = 1.0) {\n''',
'''        std::vector<float>& out,\n        double source_rate = 1.0,\n        bool allow_external_fallback = true) {\n''',
    'render signature')

repl(
'''            if (!first) {\n                // External Spectral PCM is fallback-only. If two contextual tiles\n''',
'''            if (!first && allow_external_fallback) {\n                // External Spectral PCM is fallback-only. If two contextual tiles\n''',
    'external fallback gate')

repl(
'''        const bool have_cache =\n            cache_manager().render(\n                mode,\n                m_position_seconds,\n                rate,\n                frames,\n                rendered);\n''',
'''        const bool have_cache =\n            cache_manager().render(\n                mode,\n                m_position_seconds,\n                rate,\n                frames,\n                rendered,\n                1.0,\n                false);\n''',
    'ordinary playback internal-only')

p.write_text(s, encoding='utf-8')
print('Normal playback restricted to sample-locked internal/persisted stem cache')
