from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')

old = '''        if (!m_path.empty() &&\n            stemmode::get() !=\n                stemmode::mode::original) {\n\n            queue_job_locked(seconds, true);\n        }\n\n        m_cv.notify_one();\n'''
new = '''        if (!m_path.empty()) {\n            const stemmode::mode mode = stemmode::get();\n\n            if (mode != stemmode::mode::original) {\n                queue_job_locked(seconds, true);\n            } else {\n                // Spectral Waveform arms HOLD and then seeks to the same sample\n                // to flush queued output. That seek used to clear the Original\n                // transport request made by set_hold(), leaving SCRUB with no PCM\n                // until the mouse had already moved. Re-prime a cheap decoder-only\n                // transport window here. No Spleeter inference is involved.\n                const double preview_start = (std::max)(0.0, seconds - 0.5);\n                m_jobs.emplace_front(cache_job{\n                    m_generation, m_path, preview_start, true, true, false});\n                m_job_pending = true;\n            }\n        }\n\n        m_cv.notify_one();\n'''
if old not in s:
    raise SystemExit('seek cache anchor not found')
s = s.replace(old, new, 1)

p.write_text(s, encoding='utf-8')
print('patched Original scrub cache priming after seek')
