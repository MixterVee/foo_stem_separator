from pathlib import Path
p=Path('stem_dsp.cpp')
s=p.read_text(encoding='utf-8')
count=s.count('std::max(')
s=s.replace('std::max(', '(std::max)(')
p.write_text(s,encoding='utf-8')
print(f'fixed {count} std::max calls')
