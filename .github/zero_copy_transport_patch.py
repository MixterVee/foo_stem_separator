from pathlib import Path
import re

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')

# shared_ptr owns cache segments across short audio-thread snapshots.
needle = '#include <mutex>\n#include <string>\n'
assert needle in s
s = s.replace(needle, '#include <mutex>\n#include <memory>\n#include <string>\n', 1)

# Store stable shared segment objects instead of movable value objects.
needle = '    std::deque<cache_segment> m_segments;'
assert needle in s
s = s.replace(needle, '    std::deque<std::shared_ptr<cache_segment>> m_segments;', 1)

# Both live-worker and external-publish insertions now transfer ownership once.
needle = 'm_segments.push_back(std::move(seg));'
count = s.count(needle)
assert count == 2, count
s = s.replace(needle, 'm_segments.push_back(std::make_shared<cache_segment>(std::move(seg)));')

# Back element access in forward prefetch.
needle = 'm_segments.back().end_seconds'
assert s.count(needle) == 1
s = s.replace(needle, 'm_segments.back()->end_seconds', 1)

# External duplicate replacement iterator now dereferences the shared object.
for old, new in [
    ('it->external_waveform', '(*it)->external_waveform'),
    ('it->start_seconds', '(*it)->start_seconds'),
    ('it->end_seconds', '(*it)->end_seconds'),
]:
    assert old in s
    s = s.replace(old, new, 1)

# Every read loop over m_segments binds a reference to the pointed-to segment.
pattern = re.compile(r'for \(const auto& seg\s*:\s*m_segments\) \{')
matches = list(pattern.finditer(s))
assert len(matches) >= 4, len(matches)
s = pattern.sub('for (const auto& seg_ptr : m_segments) {\n                const cache_segment& seg = *seg_ptr;', s)

# The audio callback snapshot must be shallow: only shared_ptr refcounts move.
old = '''        std::vector<cache_segment> snapshot;

        {
            std::lock_guard<std::mutex> lock(
                m_mutex);

            if (m_segments.empty()) {
                return false;
            }

            snapshot.assign(
                m_segments.begin(),
                m_segments.end());
        }'''
new = '''        std::vector<std::shared_ptr<const cache_segment>> snapshot;

        {
            std::lock_guard<std::mutex> lock(
                m_mutex);

            if (m_segments.empty()) {
                return false;
            }

            snapshot.reserve(m_segments.size());
            for (const auto& seg : m_segments) {
                snapshot.emplace_back(seg);
            }
        }'''
assert old in s
s = s.replace(old, new, 1)

# Render loops dereference the shallow snapshot; first/second remain raw pointers
# valid for the callback lifetime because snapshot owns the shared objects.
pattern = re.compile(r'for \(const auto& seg\s*:\s*snapshot\) \{')
matches = list(pattern.finditer(s))
assert len(matches) == 2, len(matches)
s = pattern.sub('for (const auto& seg_ptr : snapshot) {\n                const cache_segment& seg = *seg_ptr;', s)

# Safety check: the old value-owned segment deque and deep snapshot must be gone.
assert 'std::deque<cache_segment> m_segments;' not in s
assert 'std::vector<cache_segment> snapshot;' not in s

p.write_text(s, encoding='utf-8')
