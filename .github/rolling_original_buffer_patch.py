from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')

old = '''constexpr double kOriginalDecodeSeekPrerollSeconds = 0.50;\nconstexpr double kFirstBlockFadeSeconds = 0.005;\n'''
new = '''constexpr double kOriginalDecodeSeekPrerollSeconds = 0.50;\n// Keep already-decoded Original PCM in RAM for platter work. At 44.1 kHz stereo\n// float, 45 seconds is only about 15 MB and removes decoder-seek latency from\n// normal scratches around the current playhead.\nconstexpr double kOriginalRollingSeconds = 45.0;\nconstexpr uint64_t kOriginalRollingJoinToleranceFrames = 8;\nconstexpr double kFirstBlockFadeSeconds = 0.005;\n'''
if old not in s:
    raise SystemExit('rolling constants anchor not found')
s = s.replace(old, new, 1)

# Insert live rolling-cache publisher before publish_external_segment.
old = '''    bool publish_external_segment(\n        const std::wstring& path,\n'''
new = '''    void publish_live_original(\n        double start_seconds,\n        const audio_sample* input,\n        size_t frames,\n        unsigned channels,\n        unsigned sample_rate) {\n\n        if (input == nullptr || frames == 0 || channels == 0 || sample_rate == 0) return;\n        if (start_seconds < 0.0) start_seconds = 0.0;\n\n        // audio_sample is foobar's decoded float PCM. Convert/resample outside the\n        // cache lock so the realtime callback only holds the mutex while appending.\n        std::vector<float> source(frames * channels);\n        for (size_t i = 0; i < source.size(); ++i) {\n            source[i] = static_cast<float>(input[i]);\n        }\n\n        std::vector<float> cache_pcm;\n        if (!convert_to_cache_stereo(\n                source.data(), frames, channels, sample_rate, cache_pcm) ||\n            cache_pcm.empty()) {\n            return;\n        }\n\n        const uint64_t new_start_frame = static_cast<uint64_t>(\n            start_seconds * static_cast<double>(kCacheRate) + 0.5);\n        const uint64_t new_frames = static_cast<uint64_t>(\n            cache_pcm.size() / kCacheChannels);\n        if (new_frames == 0) return;\n\n        std::lock_guard<std::mutex> lock(m_mutex);\n        if (m_path.empty()) return;\n\n        if (m_live_original.empty()) {\n            m_live_original_start_frame = new_start_frame;\n        } else {\n            const uint64_t live_frames = static_cast<uint64_t>(\n                m_live_original.size() / kCacheChannels);\n            const uint64_t live_end_frame = m_live_original_start_frame + live_frames;\n\n            // A far seek starts a new rolling region. A HOLD self-seek or ordinary\n            // overlapping callback stays inside the existing region and preserves\n            // the already-heard history behind the platter.\n            if (new_start_frame > live_end_frame + kOriginalRollingJoinToleranceFrames ||\n                new_start_frame + new_frames + kOriginalRollingJoinToleranceFrames <\n                    m_live_original_start_frame) {\n                m_live_original.clear();\n                m_live_original_start_frame = new_start_frame;\n            }\n        }\n\n        uint64_t live_frames = static_cast<uint64_t>(\n            m_live_original.size() / kCacheChannels);\n        uint64_t live_end_frame = m_live_original_start_frame + live_frames;\n\n        // If the new callback begins a few rounding frames after our end, align it\n        // rather than inserting silence. Larger gaps were handled as a new region.\n        uint64_t effective_start = new_start_frame;\n        if (effective_start > live_end_frame &&\n            effective_start <= live_end_frame + kOriginalRollingJoinToleranceFrames) {\n            effective_start = live_end_frame;\n        }\n\n        size_t skip_frames = 0;\n        if (effective_start < live_end_frame) {\n            const uint64_t overlap = live_end_frame - effective_start;\n            if (overlap >= new_frames) {\n                return;\n            }\n            skip_frames = static_cast<size_t>(overlap);\n        }\n\n        const size_t first_value = skip_frames * kCacheChannels;\n        for (size_t i = first_value; i < cache_pcm.size(); ++i) {\n            m_live_original.push_back(cache_pcm[i]);\n        }\n\n        const uint64_t max_frames = static_cast<uint64_t>(\n            kOriginalRollingSeconds * static_cast<double>(kCacheRate) + 0.5);\n        live_frames = static_cast<uint64_t>(m_live_original.size() / kCacheChannels);\n        if (live_frames > max_frames) {\n            const uint64_t drop_frames = live_frames - max_frames;\n            const size_t drop_values = static_cast<size_t>(\n                drop_frames * kCacheChannels);\n            for (size_t i = 0; i < drop_values; ++i) {\n                m_live_original.pop_front();\n            }\n            m_live_original_start_frame += drop_frames;\n        }\n    }\n\n    bool publish_external_segment(\n        const std::wstring& path,\n'''
if old not in s:
    raise SystemExit('publish_external insertion anchor not found')
s = s.replace(old, new, 1)

# Clear live buffer on a new track.
old = '''        m_segments.clear();\n        m_jobs.clear();\n        m_job_pending = false;\n'''
new = '''        m_segments.clear();\n        m_live_original.clear();\n        m_live_original_start_frame = 0;\n        m_jobs.clear();\n        m_job_pending = false;\n'''
if s.count(old) < 2:
    raise SystemExit('new_track/stop clear anchor count too small')
# Replace only first occurrence (new_track). stop gets a separate anchor below.
s = s.replace(old, new, 1)

# Clear on stop too.
old = '''        m_path.clear();\n        m_anchor_seconds = 0.0;\n\n        m_segments.clear();\n        m_jobs.clear();\n'''
new = '''        m_path.clear();\n        m_anchor_seconds = 0.0;\n\n        m_segments.clear();\n        m_live_original.clear();\n        m_live_original_start_frame = 0;\n        m_jobs.clear();\n'''
if old not in s:
    raise SystemExit('stop clear anchor not found')
s = s.replace(old, new, 1)

# Prefer rolling Original PCM at the beginning of render while holding m_mutex.
old = '''        std::vector<std::shared_ptr<const cache_segment>> snapshot;\n\n        {\n            std::lock_guard<std::mutex> lock(\n                m_mutex);\n\n            if (m_segments.empty()) {\n                return false;\n            }\n\n            snapshot.reserve(m_segments.size());\n'''
new = '''        std::vector<std::shared_ptr<const cache_segment>> snapshot;\n\n        {\n            std::lock_guard<std::mutex> lock(\n                m_mutex);\n\n            if (mode == stemmode::mode::original && !m_live_original.empty()) {\n                const size_t live_frames = m_live_original.size() / kCacheChannels;\n                const double live_start = static_cast<double>(\n                    m_live_original_start_frame) / static_cast<double>(kCacheRate);\n                const double live_end = live_start +\n                    static_cast<double>(live_frames) / static_cast<double>(kCacheRate);\n                const double last_t = start_seconds +\n                    source_rate * static_cast<double>(frames - 1) /\n                    static_cast<double>(output_rate);\n                const double need_start = (std::min)(start_seconds, last_t);\n                const double need_end = (std::max)(start_seconds, last_t);\n\n                if (need_start >= live_start - 1.0e-9 &&\n                    need_end < live_end && live_frames != 0) {\n                    out.assign(frames * kCacheChannels, 0.0f);\n                    const double dt = 1.0 / static_cast<double>(output_rate);\n                    for (size_t f = 0; f < frames; ++f) {\n                        const double t = start_seconds +\n                            source_rate * static_cast<double>(f) * dt;\n                        double source_pos =\n                            t * static_cast<double>(kCacheRate) -\n                            static_cast<double>(m_live_original_start_frame);\n                        if (source_pos < 0.0) source_pos = 0.0;\n                        size_t i0 = static_cast<size_t>(source_pos);\n                        if (i0 >= live_frames) i0 = live_frames - 1;\n                        const size_t i1 = (std::min)(i0 + 1, live_frames - 1);\n                        const float frac = static_cast<float>(\n                            source_pos - static_cast<double>(i0));\n                        for (unsigned ch = 0; ch < kCacheChannels; ++ch) {\n                            const float a = m_live_original[i0 * kCacheChannels + ch];\n                            const float b = m_live_original[i1 * kCacheChannels + ch];\n                            out[f * kCacheChannels + ch] = a + (b - a) * frac;\n                        }\n                    }\n                    return true;\n                }\n            }\n\n            if (m_segments.empty()) {\n                return false;\n            }\n\n            snapshot.reserve(m_segments.size());\n'''
if old not in s:
    raise SystemExit('render rolling insertion anchor not found')
s = s.replace(old, new, 1)

# Add rolling members.
old = '''    std::deque<cache_job> m_jobs;\n    std::deque<std::shared_ptr<cache_segment>> m_segments;\n};\n'''
new = '''    std::deque<cache_job> m_jobs;\n    std::deque<std::shared_ptr<cache_segment>> m_segments;\n\n    // Directly harvested from foobar's decoded Original stream. Unlike cache\n    // jobs, this region is already in RAM and cannot be invalidated by rapid\n    // platter retargeting.\n    std::deque<float> m_live_original;\n    uint64_t m_live_original_start_frame = 0;\n};\n'''
if old not in s:
    raise SystemExit('rolling members anchor not found')
s = s.replace(old, new, 1)

# Feed the rolling buffer before transport can overwrite the decoded chunk.
old = '''        const stemmode::mode mode =\n            stemmode::get();\n\n        // Transport preview keeps foobar's audio clock running while replacing\n'''
new = '''        const stemmode::mode mode =\n            stemmode::get();\n\n        // Harvest foobar's already-decoded Original PCM before HOLD/SCRUB/REVERSE\n        // replaces this chunk. During transport the underlying decoder keeps moving,\n        // so the RAM platter buffer naturally grows ahead while retaining history.\n        if (mode == stemmode::mode::original) {\n            cache_manager().publish_live_original(\n                m_position_seconds, chunk->get_data(), frames, channels, rate);\n        }\n\n        // Transport preview keeps foobar's audio clock running while replacing\n'''
if old not in s:
    raise SystemExit('DSP publish anchor not found')
s = s.replace(old, new, 1)

p.write_text(s, encoding='utf-8')
print('patched rolling Original platter PCM buffer')
