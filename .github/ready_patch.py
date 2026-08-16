from pathlib import Path
p=Path('stem_dsp.cpp')
s=p.read_text(encoding='utf-8')

def rep(old,new):
    global s
    if old not in s:
        raise SystemExit('anchor not found: '+old[:200])
    s=s.replace(old,new,1)

rep('''    void request_transport(double position_seconds, bool reverse) {\n''', '''    bool transport_position_ready(double position_seconds) const {\n        const stemmode::mode mode = stemmode::get();\n        if (mode == stemmode::mode::original) return true;\n        if (position_seconds < 0.0) position_seconds = 0.0;\n\n        std::lock_guard<std::mutex> lock(m_mutex);\n        for (const auto& seg : m_segments) {\n            if (!segment_has_mode(seg, mode)) continue;\n            if (position_seconds >= seg.start_seconds &&\n                position_seconds < seg.end_seconds) {\n                return true;\n            }\n        }\n        return false;\n    }\n\n    void request_transport(double position_seconds, bool reverse) {\n''')

rep('''    double get_position_seconds() override { return transport().visible_position(); }\n};\n''', '''    double get_position_seconds() override { return transport().visible_position(); }\n    bool is_position_ready(double seconds) override {\n        return cache_manager().transport_position_ready(seconds);\n    }\n};\n''')

p.write_text(s,encoding='utf-8')
print('ready patch applied')
