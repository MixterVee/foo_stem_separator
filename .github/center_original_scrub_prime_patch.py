from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')
old = '''                const double preview_start = (std::max)(0.0, seconds - 0.5);\n'''
new = '''                // Center the cheap 20-second Original window on the grab point.\n                // A forward-biased window left only 0.5 s available for an\n                // immediate backward scratch. Centering gives the platter useful\n                // cached travel in both directions before any follow-up prefetch.\n                const double preview_start = (std::max)(\n                    0.0, seconds - kCacheSeconds * 0.5);\n'''
if old not in s:
    raise SystemExit('Original scrub preview start anchor not found')
s = s.replace(old, new, 1)
p.write_text(s, encoding='utf-8')
print('centered Original scrub cache prime')
