from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')

old = '''constexpr double kFirstBlockFadeSeconds = 0.005;'''
new = '''constexpr double kFirstBlockFadeSeconds = 0.005;\n// Spectral Waveform now explicitly returns scrub transport to HOLD after real\n// mouse motion stops. Keep this slightly longer timeout as a safety net only.\nconstexpr ULONGLONG kScrubAudibleSafetyMs = 320;\nconstexpr double kScrubKeepaliveToleranceSeconds = 0.002;'''
assert old in s
s = s.replace(old, new, 1)

old = '''    void set_scrub(double seconds) {\n        seconds = (std::max)(0.0, seconds);\n        {\n            std::lock_guard<std::mutex> lock(m_mutex);\n            m_state = stem_transport_scrub;\n            m_position_seconds = seconds;\n            m_render_seconds = seconds;\n            m_scrub_audible_until = GetTickCount64() + 150;\n        }\n        cache_manager().request_transport(seconds, false);\n    }'''
new = '''    void set_scrub(double seconds) {\n        seconds = (std::max)(0.0, seconds);\n        bool retarget = true;\n        {\n            std::lock_guard<std::mutex> lock(m_mutex);\n            retarget =\n                m_state != stem_transport_scrub ||\n                std::abs(seconds - m_position_seconds) >\n                    kScrubKeepaliveToleranceSeconds;\n\n            m_state = stem_transport_scrub;\n            m_position_seconds = seconds;\n            if (retarget) {\n                m_render_seconds = seconds;\n            }\n            m_scrub_audible_until =\n                GetTickCount64() + kScrubAudibleSafetyMs;\n        }\n\n        // A timer keepalive for the same mouse target should extend audibility\n        // only. Do not restart rendering or enqueue another cache request.\n        if (retarget) {\n            cache_manager().request_transport(seconds, false);\n        }\n    }'''
assert old in s
s = s.replace(old, new, 1)

p.write_text(s, encoding='utf-8')
