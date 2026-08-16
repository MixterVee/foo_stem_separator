from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')

old = '''            // Prefer the newest Spectral Waveform PCM tile. It already contains\n            // the contextual Spleeter result that produced the visible waveform,\n            // so using it avoids both duplicate inference and transport delay.\n            for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it) {\n                if (!it->external_waveform || !segment_has_mode(*it, mode)) continue;\n                if (t >= it->start_seconds && t < it->end_seconds) {\n                    first = &*it;\n                    break;\n                }\n            }\n\n            if (!first) {\n                for (const auto& seg : snapshot) {\n                    if (seg.external_waveform || !segment_has_mode(seg, mode)) continue;\n                    if (t >= seg.start_seconds && t < seg.end_seconds) {\n                        if (!first) first = &seg;\n                        else { second = &seg; break; }\n                    }\n                }\n            }'''
new = '''            // Prefer Spectral Waveform PCM. Widened contextual publishes overlap\n            // adjacent 5-second tiles, so retain the two time-adjacent external\n            // segments when both cover this sample and crossfade their handoff\n            // below. This avoids both duplicate inference and hard tile seams.\n            for (const auto& seg : snapshot) {\n                if (!seg.external_waveform || !segment_has_mode(seg, mode)) continue;\n                if (t < seg.start_seconds || t >= seg.end_seconds) continue;\n\n                if (!first || seg.start_seconds < first->start_seconds) {\n                    second = first;\n                    first = &seg;\n                } else if (!second || seg.start_seconds < second->start_seconds) {\n                    second = &seg;\n                }\n            }\n\n            if (!first) {\n                for (const auto& seg : snapshot) {\n                    if (seg.external_waveform || !segment_has_mode(seg, mode)) continue;\n                    if (t >= seg.start_seconds && t < seg.end_seconds) {\n                        if (!first) first = &seg;\n                        else { second = &seg; break; }\n                    }\n                }\n            }'''
assert old in s
s = s.replace(old, new, 1)

old = '''                if (second && !first->external_waveform) {'''
new = '''                if (second) {'''
assert old in s
s = s.replace(old, new, 1)

p.write_text(s, encoding='utf-8')
