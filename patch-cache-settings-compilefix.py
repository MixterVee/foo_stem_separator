from pathlib import Path

p = Path('persistent_stem_cache.cpp')
s = p.read_text(encoding='utf-8-sig')

old = '#include <foobar2000/SDK/foobar2000.h>\n#include "persistent_stem_cache.h"\n'
if s.count(old) != 1:
    raise RuntimeError(f'expected SDK include marker once, found {s.count(old)}')
s = s.replace(old, '#include "persistent_stem_cache.h"\n', 1)

old = '#include <windows.h>\n#include <compressapi.h>\n'
new = '#include <windows.h>\n#include <compressapi.h>\n#include <foobar2000/SDK/foobar2000.h>\n'
if s.count(old) != 1:
    raise RuntimeError(f'expected Windows/compression include marker once, found {s.count(old)}')
s = s.replace(old, new, 1)

s = s.replace(
    'return static_cast<unsigned>((std::min)(configured, 200));',
    'return static_cast<unsigned>(configured > 200 ? 200 : configured);')
s = s.replace(
    'value = (std::max)(1u, (std::min)(value, 200u));',
    'if (value < 1u) value = 1u;\n    if (value > 200u) value = 200u;')

p.write_text(s, encoding='utf-8')
print('Applied cache settings Windows compile fix.')
