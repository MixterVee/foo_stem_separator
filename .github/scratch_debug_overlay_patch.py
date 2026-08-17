from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')

# Global diagnostic counters. Read-only diagnostics must not affect transport.
old = '''constexpr double kScrubPhaseCorrectionMix = 0.22;\n'''
new = '''constexpr double kScrubPhaseCorrectionMix = 0.22;\n\nstd::atomic<uint64_t> g_dbg_render_attempts{0};\nstd::atomic<uint64_t> g_dbg_render_successes{0};\nstd::atomic<uint64_t> g_dbg_live_hits{0};\nstd::atomic<uint64_t> g_dbg_cache_hits{0};\nstd::atomic<uint64_t> g_dbg_render_misses{0};\nstd::atomic<uint64_t> g_dbg_scrub_audio_writes{0};\nstd::atomic<uint64_t> g_dbg_scrub_silence_writes{0};\nstd::atomic<int> g_dbg_last_render_source{stem_debug_source_none};\nstd::atomic<int> g_dbg_last_render_ok{0};\nstd::atomic<double> g_dbg_last_render_start{0.0};\nstd::atomic<double> g_dbg_last_source_rate{0.0};\n\nvoid reset_scratch_debug() {\n    g_dbg_render_attempts.store(0, std::memory_order_relaxed);\n    g_dbg_render_successes.store(0, std::memory_order_relaxed);\n    g_dbg_live_hits.store(0, std::memory_order_relaxed);\n    g_dbg_cache_hits.store(0, std::memory_order_relaxed);\n    g_dbg_render_misses.store(0, std::memory_order_relaxed);\n    g_dbg_scrub_audio_writes.store(0, std::memory_order_relaxed);\n    g_dbg_scrub_silence_writes.store(0, std::memory_order_relaxed);\n    g_dbg_last_render_source.store(stem_debug_source_none, std::memory_order_relaxed);\n    g_dbg_last_render_ok.store(0, std::memory_order_relaxed);\n    g_dbg_last_render_start.store(0.0, std::memory_order_relaxed);\n    g_dbg_last_source_rate.store(0.0, std::memory_order_relaxed);\n}\n'''
if old not in s:
    raise SystemExit('debug globals anchor not found')
s = s.replace(old, new, 1)

# Reset diagnostics on a new track.
old = '''    void new_track(\n        const std::wstring& path) {\n\n        std::lock_guard<std::mutex> lock(\n            m_mutex);\n'''
new = '''    void new_track(\n        const std::wstring& path) {\n\n        reset_scratch_debug();\n\n        std::lock_guard<std::mutex> lock(\n            m_mutex);\n'''
if old not in s:
    raise SystemExit('new_track anchor not found')
s = s.replace(old, new, 1)

# Expose the exact RAM platter range for the overlay.
old = '''    double anchor_time() const {\n        std::lock_guard<std::mutex> lock(\n            m_mutex);\n\n        return m_anchor_seconds;\n    }\n'''
new = '''    double anchor_time() const {\n        std::lock_guard<std::mutex> lock(\n            m_mutex);\n\n        return m_anchor_seconds;\n    }\n\n    bool debug_live_range(double& start_seconds, double& end_seconds) const {\n        std::lock_guard<std::mutex> lock(m_mutex);\n        if (m_live_original.empty()) {\n            start_seconds = -1.0;\n            end_seconds = -1.0;\n            return false;\n        }\n        const uint64_t frames = static_cast<uint64_t>(\n            m_live_original.size() / kCacheChannels);\n        start_seconds = static_cast<double>(m_live_original_start_frame) /\n            static_cast<double>(kCacheRate);\n        end_seconds = start_seconds + static_cast<double>(frames) /\n            static_cast<double>(kCacheRate);\n        return true;\n    }\n'''
if old not in s:
    raise SystemExit('anchor_time anchor not found')
s = s.replace(old, new, 1)

# Start each actual render attempt with a fresh diagnostic record.
old = '''        if (output_rate == 0 ||\n            frames == 0) {\n            return false;\n        }\n\n        std::vector<std::shared_ptr<const cache_segment>> snapshot;\n'''
new = '''        if (output_rate == 0 ||\n            frames == 0) {\n            return false;\n        }\n\n        g_dbg_render_attempts.fetch_add(1, std::memory_order_relaxed);\n        g_dbg_last_render_start.store(start_seconds, std::memory_order_relaxed);\n        g_dbg_last_source_rate.store(source_rate, std::memory_order_relaxed);\n        g_dbg_last_render_source.store(stem_debug_source_none, std::memory_order_relaxed);\n        g_dbg_last_render_ok.store(0, std::memory_order_relaxed);\n\n        std::vector<std::shared_ptr<const cache_segment>> snapshot;\n'''
if old not in s:
    raise SystemExit('render entry anchor not found')
s = s.replace(old, new, 1)

# Mark direct rolling-RAM success.
old = '''                    }\n                    return true;\n                }\n            }\n\n            if (m_segments.empty()) {\n                return false;\n            }\n'''
new = '''                    }\n                    g_dbg_render_successes.fetch_add(1, std::memory_order_relaxed);\n                    g_dbg_live_hits.fetch_add(1, std::memory_order_relaxed);\n                    g_dbg_last_render_source.store(stem_debug_source_live, std::memory_order_relaxed);\n                    g_dbg_last_render_ok.store(1, std::memory_order_relaxed);\n                    return true;\n                }\n            }\n\n            if (m_segments.empty()) {\n                g_dbg_render_misses.fetch_add(1, std::memory_order_relaxed);\n                g_dbg_last_render_source.store(stem_debug_source_miss, std::memory_order_relaxed);\n                return false;\n            }\n'''
if old not in s:
    raise SystemExit('live success anchor not found')
s = s.replace(old, new, 1)

# Mark negative-time miss.
old = '''            if (t < 0.0) return false;\n'''
new = '''            if (t < 0.0) {\n                g_dbg_render_misses.fetch_add(1, std::memory_order_relaxed);\n                g_dbg_last_render_source.store(stem_debug_source_miss, std::memory_order_relaxed);\n                return false;\n            }\n'''
if old not in s:
    raise SystemExit('negative render anchor not found')
s = s.replace(old, new, 1)

# Mark uncovered cache miss.
old = '''            if (!first) return false;\n\n            auto sample_from =\n'''
new = '''            if (!first) {\n                g_dbg_render_misses.fetch_add(1, std::memory_order_relaxed);\n                g_dbg_last_render_source.store(stem_debug_source_miss, std::memory_order_relaxed);\n                return false;\n            }\n\n            auto sample_from =\n'''
if old not in s:
    raise SystemExit('cache miss anchor not found')
s = s.replace(old, new, 1)

# Mark completed position-indexed cache render.
old = '''        }\n\n        return true;\n    }\n\nprivate:\n    static bool segment_has_mode'''
new = '''        }\n\n        g_dbg_render_successes.fetch_add(1, std::memory_order_relaxed);\n        g_dbg_cache_hits.fetch_add(1, std::memory_order_relaxed);\n        g_dbg_last_render_source.store(stem_debug_source_cache, std::memory_order_relaxed);\n        g_dbg_last_render_ok.store(1, std::memory_order_relaxed);\n        return true;\n    }\n\nprivate:\n    static bool segment_has_mode'''
if old not in s:
    raise SystemExit('render success anchor not found')
s = s.replace(old, new, 1)

# Count what the DSP actually writes while SCRUB is active.
old = '''            auto write_silence = [&]() {\n                std::vector<audio_sample> zeros(frames * kCacheChannels, 0);\n'''
new = '''            auto write_silence = [&]() {\n                if (ts.state == stem_transport_scrub) {\n                    g_dbg_scrub_silence_writes.fetch_add(1, std::memory_order_relaxed);\n                }\n                std::vector<audio_sample> zeros(frames * kCacheChannels, 0);\n'''
if old not in s:
    raise SystemExit('write_silence anchor not found')
s = s.replace(old, new, 1)

old = '''            auto write_preview = [&](const std::vector<float>& rendered) {\n                std::vector<audio_sample> output(rendered.size());\n'''
new = '''            auto write_preview = [&](const std::vector<float>& rendered) {\n                if (ts.state == stem_transport_scrub) {\n                    g_dbg_scrub_audio_writes.fetch_add(1, std::memory_order_relaxed);\n                }\n                std::vector<audio_sample> output(rendered.size());\n'''
if old not in s:
    raise SystemExit('write_preview anchor not found')
s = s.replace(old, new, 1)

# Implement the diagnostics service snapshot.
old = '''    bool is_position_ready(double seconds) override {\n        return cache_manager().transport_position_ready(seconds);\n    }\n    bool publish_cache_block(\n'''
new = '''    bool is_position_ready(double seconds) override {\n        return cache_manager().transport_position_ready(seconds);\n    }\n    bool get_debug_status(stem_transport_debug_status& out) override {\n        try {\n            const transport_snapshot ts = transport().snapshot();\n            out = stem_transport_debug_status{};\n            out.state = ts.state;\n            out.mode = static_cast<int>(stemmode::get());\n            out.position_seconds = ts.position_seconds;\n            out.render_seconds = ts.render_seconds;\n            out.scrub_velocity = ts.scrub_velocity;\n            out.last_render_source = g_dbg_last_render_source.load(std::memory_order_relaxed);\n            out.last_render_ok = g_dbg_last_render_ok.load(std::memory_order_relaxed);\n            out.last_render_start_seconds = g_dbg_last_render_start.load(std::memory_order_relaxed);\n            out.last_source_rate = g_dbg_last_source_rate.load(std::memory_order_relaxed);\n            cache_manager().debug_live_range(out.live_start_seconds, out.live_end_seconds);\n            out.render_attempts = g_dbg_render_attempts.load(std::memory_order_relaxed);\n            out.render_successes = g_dbg_render_successes.load(std::memory_order_relaxed);\n            out.live_hits = g_dbg_live_hits.load(std::memory_order_relaxed);\n            out.cache_hits = g_dbg_cache_hits.load(std::memory_order_relaxed);\n            out.render_misses = g_dbg_render_misses.load(std::memory_order_relaxed);\n            out.scrub_audio_writes = g_dbg_scrub_audio_writes.load(std::memory_order_relaxed);\n            out.scrub_silence_writes = g_dbg_scrub_silence_writes.load(std::memory_order_relaxed);\n            return true;\n        } catch (...) {\n            return false;\n        }\n    }\n    bool publish_cache_block(\n'''
if old not in s:
    raise SystemExit('service diagnostics anchor not found')
s = s.replace(old, new, 1)

p.write_text(s, encoding='utf-8')
print('patched visible scratch diagnostics')
