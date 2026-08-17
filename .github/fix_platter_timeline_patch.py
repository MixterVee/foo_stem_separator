from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')

# 1) Let the converter target an exact number of cache frames. This is needed
# for a continuous 48k -> 44.1k rolling timeline: per-chunk rounding drifts.
old = '''bool convert_to_cache_stereo(\n    const float* input,\n    size_t frames,\n    unsigned channels,\n    unsigned sample_rate,\n    std::vector<float>& out) {\n'''
new = '''bool convert_to_cache_stereo(\n    const float* input,\n    size_t frames,\n    unsigned channels,\n    unsigned sample_rate,\n    std::vector<float>& out,\n    size_t exact_output_frames = 0) {\n'''
if old not in s:
    raise SystemExit('convert signature anchor not found')
s = s.replace(old, new, 1)

old = '''    size_t output_frames = frames;\n    if (sample_rate != kCacheRate) {\n        output_frames = (std::max)(\n            static_cast<size_t>(1),\n            static_cast<size_t>(std::llround(\n                static_cast<double>(frames) *\n                static_cast<double>(kCacheRate) /\n                static_cast<double>(sample_rate))));\n    }\n'''
new = '''    size_t output_frames = exact_output_frames;\n    if (output_frames == 0) {\n        output_frames = frames;\n        if (sample_rate != kCacheRate) {\n            output_frames = (std::max)(\n                static_cast<size_t>(1),\n                static_cast<size_t>(std::llround(\n                    static_cast<double>(frames) *\n                    static_cast<double>(kCacheRate) /\n                    static_cast<double>(sample_rate))));\n        }\n    }\n'''
if old not in s:
    raise SystemExit('convert frame count anchor not found')
s = s.replace(old, new, 1)

# 2) Build each live Original chunk against absolute 44.1k frame boundaries.
# Consecutive 48k chunks therefore alternate their rounded output length as
# needed instead of accumulating +/- fractional-frame error.
old = '''        std::vector<float> cache_pcm;\n        if (!convert_to_cache_stereo(\n                source.data(), frames, channels, sample_rate, cache_pcm) ||\n            cache_pcm.empty()) {\n            return;\n        }\n\n        const uint64_t new_start_frame = static_cast<uint64_t>(\n            start_seconds * static_cast<double>(kCacheRate) + 0.5);\n        const uint64_t new_frames = static_cast<uint64_t>(\n            cache_pcm.size() / kCacheChannels);\n'''
new = '''        const uint64_t new_start_frame = static_cast<uint64_t>(\n            start_seconds * static_cast<double>(kCacheRate) + 0.5);\n        const double source_duration =\n            static_cast<double>(frames) / static_cast<double>(sample_rate);\n        const uint64_t new_end_frame = static_cast<uint64_t>(\n            (start_seconds + source_duration) *\n                static_cast<double>(kCacheRate) + 0.5);\n        const size_t exact_cache_frames = static_cast<size_t>((std::max)(\n            static_cast<uint64_t>(1),\n            new_end_frame > new_start_frame\n                ? new_end_frame - new_start_frame\n                : static_cast<uint64_t>(1)));\n\n        std::vector<float> cache_pcm;\n        if (!convert_to_cache_stereo(\n                source.data(), frames, channels, sample_rate, cache_pcm,\n                exact_cache_frames) ||\n            cache_pcm.empty()) {\n            return;\n        }\n\n        const uint64_t new_frames = static_cast<uint64_t>(\n            cache_pcm.size() / kCacheChannels);\n'''
if old not in s:
    raise SystemExit('live conversion anchor not found')
s = s.replace(old, new, 1)

# 3) A transport-triggered playback_seek is only an output-pipeline flush. It
# must update the DSP anchor but must NOT bump cache generation / cancel jobs.
old = '''    void stop() {\n        std::lock_guard<std::mutex> lock(\n            m_mutex);\n'''
new = '''    void transport_flush_seek(double seconds) {\n        if (seconds < 0.0) seconds = 0.0;\n        std::lock_guard<std::mutex> lock(m_mutex);\n        // Spectral Waveform uses a seek to the already-armed HOLD/REVERSE/RELEASE\n        // position only to flush queued output. The track and all position-indexed\n        // PCM remain valid, so preserve generation, jobs, rolling history and the\n        // sequential ahead decoder. flush() will make the DSP pick up this anchor.\n        m_anchor_seconds = seconds;\n    }\n\n    void stop() {\n        std::lock_guard<std::mutex> lock(\n            m_mutex);\n'''
if old not in s:
    raise SystemExit('transport flush insertion anchor not found')
s = s.replace(old, new, 1)

old = '''    void on_playback_seek(\n        double time) override {\n\n        cache_manager().seek(time);\n    }\n'''
new = '''    void on_playback_seek(\n        double time) override {\n\n        // HOLD, SCRUB, REVERSE and RELEASE all arm transport first and then seek\n        // to that same sample solely to flush foobar's queued output. Treat that\n        // as a timeline re-anchor, not as a real user seek; otherwise every grab\n        // cancels the platter prefetch job we just started.\n        const int state = transport().state();\n        if (state != stem_transport_normal) {\n            const double transport_position = transport().visible_position();\n            if (std::abs(time - transport_position) <= 0.050) {\n                cache_manager().transport_flush_seek(time);\n                return;\n            }\n        }\n\n        cache_manager().seek(time);\n    }\n'''
if old not in s:
    raise SystemExit('playback seek observer anchor not found')
s = s.replace(old, new, 1)

p.write_text(s, encoding='utf-8')
print('patched platter timeline continuity and transport flush preservation')
