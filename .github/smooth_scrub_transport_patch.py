from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')


def replace_once(old: str, new: str) -> None:
    global s
    count = s.count(old)
    assert count == 1, f'expected one match, got {count}: {old[:120]!r}'
    s = s.replace(old, new, 1)

# Render transport at an arbitrary signed source rate. This lets a scrub block
# traverse exactly from the previous platter position to the newest one instead
# of emitting repeated 1x forward snippets.
replace_once(
'''    bool render(\n        stemmode::mode mode,\n        double start_seconds,\n        unsigned output_rate,\n        size_t frames,\n        std::vector<float>& out,\n        bool reverse = false) {''',
'''    bool render(\n        stemmode::mode mode,\n        double start_seconds,\n        unsigned output_rate,\n        size_t frames,\n        std::vector<float>& out,\n        double source_rate = 1.0) {''')

replace_once(
'''            const double direction = reverse ? -1.0 : 1.0;\n            const double t =\n                start_seconds +\n                direction * static_cast<double>(f) * dt;''',
'''            const double t =\n                start_seconds +\n                source_rate * static_cast<double>(f) * dt;''')

# Do not rewind the renderer to every new mouse target. The renderer remains at
# the last position it actually auditioned; each DSP block then bridges from
# that point to the latest target in one continuous forward/reverse trajectory.
replace_once(
'''    void set_scrub(double seconds) {\n        seconds = (std::max)(0.0, seconds);\n        bool retarget = true;\n        {\n            std::lock_guard<std::mutex> lock(m_mutex);\n            retarget =\n                m_state != stem_transport_scrub ||\n                std::abs(seconds - m_position_seconds) >\n                    kScrubKeepaliveToleranceSeconds;\n\n            m_state = stem_transport_scrub;\n            m_position_seconds = seconds;\n            if (retarget) {\n                m_render_seconds = seconds;\n            }\n            m_scrub_audible_until =\n                GetTickCount64() + kScrubAudibleSafetyMs;\n        }\n\n        // A timer keepalive for the same mouse target should extend audibility\n        // only. Do not restart rendering or enqueue another cache request.\n        if (retarget) {\n            cache_manager().request_transport(seconds, false);\n        }\n    }''',
'''    void set_scrub(double seconds) {\n        seconds = (std::max)(0.0, seconds);\n        bool retarget = true;\n        bool reverse = false;\n        {\n            std::lock_guard<std::mutex> lock(m_mutex);\n            const int previous_state = m_state;\n            retarget =\n                previous_state != stem_transport_scrub ||\n                std::abs(seconds - m_position_seconds) >\n                    kScrubKeepaliveToleranceSeconds;\n\n            // A centered grab enters from HOLD, whose render cursor is the exact\n            // sample that was under the playhead when the platter was grabbed.\n            // Preserve that cursor so the first audible block also follows the\n            // hand instead of jumping straight to the new target.\n            if (previous_state != stem_transport_scrub &&\n                previous_state != stem_transport_hold) {\n                m_render_seconds = seconds;\n            }\n\n            reverse = seconds < m_render_seconds;\n            m_state = stem_transport_scrub;\n            m_position_seconds = seconds;\n\n            // Only real target movement opens an audible scrub window. Repeated\n            // keepalives for an unchanged target must never make a stopped\n            // platter emit another piece of audio.\n            if (retarget) {\n                m_scrub_audible_until =\n                    GetTickCount64() + kScrubAudibleSafetyMs;\n            }\n        }\n\n        if (retarget) {\n            cache_manager().request_transport(seconds, reverse);\n        }\n    }''')

replace_once(
'''    void advance_scrub(double seconds) {\n        std::lock_guard<std::mutex> lock(m_mutex);\n        if (m_state == stem_transport_scrub) {\n            m_render_seconds = (std::max)(0.0, m_render_seconds + seconds);\n        }\n    }''',
'''    void complete_scrub(double seconds) {\n        std::lock_guard<std::mutex> lock(m_mutex);\n        if (m_state == stem_transport_scrub) {\n            m_render_seconds = (std::max)(0.0, seconds);\n        }\n    }''')

# Smooth transport block boundaries and fade a stopped scrub to true silence in
# only a couple of milliseconds. This removes the click/burst that otherwise
# appears when a non-zero preview sample is followed by a zero-filled HOLD block.
replace_once(
'''            auto write_silence = [&]() {\n                std::vector<audio_sample> zeros(frames * kCacheChannels, 0);\n                chunk->set_data(\n                    zeros.data(), frames, channels, rate, chunk->get_channel_config());\n            };\n\n            auto write_preview = [&](const std::vector<float>& rendered) {\n                std::vector<audio_sample> output(rendered.size());\n                for (size_t i = 0; i < rendered.size(); ++i) {\n                    output[i] = static_cast<audio_sample>(rendered[i]);\n                }\n                chunk->set_data(\n                    output.data(), frames, channels, rate, chunk->get_channel_config());\n            };''',
'''            auto write_silence = [&]() {\n                std::vector<audio_sample> zeros(frames * kCacheChannels, 0);\n\n                if (m_transportTailValid && frames != 0) {\n                    const size_t fade_frames = (std::min)(\n                        frames,\n                        (std::max)(static_cast<size_t>(1),\n                            static_cast<size_t>(static_cast<double>(rate) * 0.0025)));\n\n                    for (size_t f = 0; f < fade_frames; ++f) {\n                        const double gain =\n                            1.0 - static_cast<double>(f + 1) /\n                                static_cast<double>(fade_frames);\n                        for (unsigned ch = 0; ch < kCacheChannels; ++ch) {\n                            zeros[f * kCacheChannels + ch] =\n                                static_cast<audio_sample>(\n                                    static_cast<double>(m_transportTail[ch]) * gain);\n                        }\n                    }\n                    m_transportTailValid = false;\n                }\n\n                chunk->set_data(\n                    zeros.data(), frames, channels, rate, chunk->get_channel_config());\n            };\n\n            auto write_preview = [&](const std::vector<float>& rendered) {\n                std::vector<audio_sample> output(rendered.size());\n                for (size_t i = 0; i < rendered.size(); ++i) {\n                    output[i] = static_cast<audio_sample>(rendered[i]);\n                }\n\n                if (m_transportTailValid && frames != 0) {\n                    const size_t blend_frames = (std::min)(\n                        frames,\n                        (std::max)(static_cast<size_t>(1),\n                            static_cast<size_t>(static_cast<double>(rate) * 0.0010)));\n                    for (size_t f = 0; f < blend_frames; ++f) {\n                        const double alpha =\n                            static_cast<double>(f + 1) /\n                            static_cast<double>(blend_frames);\n                        for (unsigned ch = 0; ch < kCacheChannels; ++ch) {\n                            const size_t i = f * kCacheChannels + ch;\n                            output[i] = static_cast<audio_sample>(\n                                static_cast<double>(m_transportTail[ch]) * (1.0 - alpha) +\n                                static_cast<double>(output[i]) * alpha);\n                        }\n                    }\n                }\n\n                chunk->set_data(\n                    output.data(), frames, channels, rate, chunk->get_channel_config());\n\n                if (frames != 0) {\n                    const size_t last = (frames - 1) * kCacheChannels;\n                    for (unsigned ch = 0; ch < kCacheChannels; ++ch) {\n                        m_transportTail[ch] = output[last + ch];\n                    }\n                    m_transportTailValid = true;\n                }\n            };''')

replace_once(
'''            if (ts.state == stem_transport_scrub) {\n                if (GetTickCount64() <= ts.scrub_audible_until) {\n                    cache_manager().request_transport(ts.render_seconds, false);\n                    std::vector<float> preview;\n                    if (cache_manager().render(\n                            mode, ts.render_seconds, rate, frames, preview, false) &&\n                        preview.size() == frames * kCacheChannels) {\n                        write_preview(preview);\n                        transport().advance_scrub(chunk_seconds);\n                    } else {\n                        // Never substitute Original when a selected stem is missing.\n                        write_silence();\n                    }\n                } else {\n                    write_silence();\n                }\n                m_position_seconds += chunk_seconds;\n                m_using_stem = false;\n                return true;\n            }''',
'''            if (ts.state == stem_transport_scrub) {\n                const double delta =\n                    ts.position_seconds - ts.render_seconds;\n                const double move_epsilon =\n                    0.5 / static_cast<double>(rate);\n                const bool fresh_motion =\n                    GetTickCount64() <= ts.scrub_audible_until;\n\n                if (fresh_motion && std::abs(delta) > move_epsilon) {\n                    // Map the exact hand movement onto one output block. Positive\n                    // delta plays forward, negative delta plays backward, and the\n                    // magnitude naturally changes pitch/speed like a real platter.\n                    const double output_span =\n                        frames > 1\n                            ? static_cast<double>(frames - 1) /\n                                static_cast<double>(rate)\n                            : chunk_seconds;\n                    const double source_rate =\n                        output_span > 0.0 ? delta / output_span : 0.0;\n\n                    cache_manager().request_transport(\n                        ts.position_seconds, source_rate < 0.0);\n\n                    std::vector<float> preview;\n                    if (source_rate != 0.0 &&\n                        cache_manager().render(\n                            mode, ts.render_seconds, rate, frames, preview, source_rate) &&\n                        preview.size() == frames * kCacheChannels) {\n                        write_preview(preview);\n                    } else {\n                        // Never play a delayed stale slice after the hand has moved\n                        // on. Missing cache is silence for this gesture block.\n                        write_silence();\n                    }\n\n                    // Whether audible or not, consume this exact mouse movement.\n                    // A stationary target therefore has no queued audio left.\n                    transport().complete_scrub(ts.position_seconds);\n                } else {\n                    write_silence();\n                    if (std::abs(delta) > move_epsilon) {\n                        // The safety window expired before this movement could be\n                        // rendered. Discard it rather than producing a late burst.\n                        transport().complete_scrub(ts.position_seconds);\n                    }\n                }\n                m_position_seconds += chunk_seconds;\n                m_using_stem = false;\n                return true;\n            }''')

# Existing callers that used the old reverse bool now pass an explicit signed
# source rate. Normal playback still uses the default +1.0 rate.
replace_once(
'''                        mode, ts.render_seconds, rate, frames, preview, true) &&''',
'''                        mode, ts.render_seconds, rate, frames, preview, -1.0) &&''')
replace_once(
'''                        mode, m_position_seconds, rate, frames, preview, false) &&''',
'''                        mode, m_position_seconds, rate, frames, preview, 1.0) &&''')

# Once transport is normal again, an old preview tail must not be reused by a
# later HOLD gesture.
replace_once(
'''        // V26: optional track-start pre-cache.''',
'''        m_transportTailValid = false;\n\n        // V26: optional track-start pre-cache.''')

replace_once(
'''        m_precache_handled = false;\n    }''',
'''        m_precache_handled = false;\n        m_transportTailValid = false;\n    }''')

replace_once(
'''        m_generation = 0;\n    }\n\n    bool m_have_position = false;''',
'''        m_generation = 0;\n        m_transportTailValid = false;\n        m_transportTail[0] = 0;\n        m_transportTail[1] = 0;\n    }\n\n    bool m_have_position = false;''')

replace_once(
'''    uint64_t m_generation = 0;\n    double m_position_seconds = 0.0;''',
'''    uint64_t m_generation = 0;\n    double m_position_seconds = 0.0;\n\n    audio_sample m_transportTail[kCacheChannels] = {};\n    bool m_transportTailValid = false;''')

assert 'advance_scrub(' not in s
assert 'bool reverse = false' not in s

p.write_text(s, encoding='utf-8')
