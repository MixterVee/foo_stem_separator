from pathlib import Path

path = Path("stem_dsp.cpp")
s = path.read_text(encoding="utf-8")


def replace_once(old: str, new: str, label: str) -> None:
    global s
    count = s.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, found {count}")
    s = s.replace(old, new, 1)
    print(f"patched: {label}")


# 1) DJ-engine control constants: Mixxx-style rate filtering / target correction.
old = """constexpr double kScrubRateRampSeconds = 0.006;\nconstexpr double kScrubMaxCursorErrorSeconds = 0.060;\nconstexpr double kScrubReversalReanchorErrorSeconds = 0.015;\n"""
new = """constexpr double kScrubRateRampSeconds = 0.006;\nconstexpr double kScrubMaxCursorErrorSeconds = 0.060;\nconstexpr double kScrubReversalReanchorErrorSeconds = 0.015;\n// DJ-engine scratch controller. Mixxx drives its audio scaler from a filtered\n// signed rate derived from platter target error; xwax likewise keeps a\n// continuous stylus position and only uses absolute position as drift\n// correction. Keep those responsibilities in the realtime renderer instead of\n// replaying wall-clock mouse trajectory slices as miniature seeks.\nconstexpr double kDjScratchPhaseCorrectionSeconds = 0.050;\nconstexpr double kDjScratchMaxCorrectionRate = 6.0;\nconstexpr double kDjScratchRateFilter = 0.40;\nconstexpr double kDjScratchStrongDecel = 0.10;\nconstexpr double kDjScratchMotionGraceSeconds = 0.050;\nconstexpr double kDjScratchStoppedRate = 0.010;\n"""
replace_once(old, new, "DJ scratch constants")


# 2) Cubic/Hermite interpolation, matching the mature DJ-player approach.
marker = """bool convert_to_cache_stereo(\n"""
insert = """inline float scratch_hermite4(\n    float frac,\n    float xm1,\n    float x0,\n    float x1,\n    float x2) {\n\n    const float c = (x1 - xm1) * 0.5f;\n    const float v = x0 - x1;\n    const float w = c + v;\n    const float a = w + v + (x2 - x0) * 0.5f;\n    const float b_neg = w + a;\n    return (((a * frac - b_neg) * frac + c) * frac + x0);\n}\n\n"""
if insert not in s:
    idx = s.find(marker)
    if idx < 0:
        raise SystemExit("convert_to_cache_stereo marker not found")
    s = s[:idx] + insert + s[idx:]
    print("patched: cubic scratch interpolation helper")


# 3) Treat the already-heard Original rolling buffer as transport-ready PCM.
old = """    bool range_ready_locked(stemmode::mode mode, double start_seconds, double end_seconds) const {\n        if (end_seconds <= start_seconds + 1.0e-6) return true;\n\n        double cursor = start_seconds;\n        while (cursor < end_seconds - 1.0e-6) {\n            double furthest = cursor;\n            for (const auto& seg_ptr : m_segments) {\n                const cache_segment& seg = *seg_ptr;\n                if (!segment_has_mode(seg, mode)) continue;\n                if (seg.start_seconds <= cursor + 1.0e-6 &&\n                    seg.end_seconds > furthest) {\n                    furthest = seg.end_seconds;\n                }\n            }\n            if (furthest <= cursor + 1.0e-6) return false;\n            cursor = furthest;\n        }\n        return true;\n    }\n"""
new = """    bool range_ready_locked(stemmode::mode mode, double start_seconds, double end_seconds) const {\n        if (end_seconds <= start_seconds + 1.0e-6) return true;\n\n        // The live Original deque is already resident PCM. Do not launch a\n        // random Media Foundation preview job when the platter's requested\n        // directional margin is already inside that RAM region. One cache frame\n        // of edge tolerance lets a reverse grab begin on the newest decoded\n        // sample rather than reporting a false miss at the live boundary.\n        if (mode == stemmode::mode::original && !m_live_original.empty()) {\n            const double live_start =\n                static_cast<double>(m_live_original_start_frame) /\n                static_cast<double>(kCacheRate);\n            const double live_end = live_start +\n                static_cast<double>(m_live_original.size() / kCacheChannels) /\n                static_cast<double>(kCacheRate);\n            const double edge = 1.0 / static_cast<double>(kCacheRate);\n            if (start_seconds >= live_start - edge &&\n                end_seconds <= live_end + edge) {\n                return true;\n            }\n        }\n\n        double cursor = start_seconds;\n        while (cursor < end_seconds - 1.0e-6) {\n            double furthest = cursor;\n            for (const auto& seg_ptr : m_segments) {\n                const cache_segment& seg = *seg_ptr;\n                if (!segment_has_mode(seg, mode)) continue;\n                if (seg.start_seconds <= cursor + 1.0e-6 &&\n                    seg.end_seconds > furthest) {\n                    furthest = seg.end_seconds;\n                }\n            }\n            if (furthest <= cursor + 1.0e-6) return false;\n            cursor = furthest;\n        }\n        return true;\n    }\n"""
replace_once(old, new, "resident Original readiness")


# 4) Let reverse start exactly at the live edge and use cubic interpolation.
old = """                if (need_start >= live_start - 1.0e-9 &&\n                    need_end < live_end && live_frames != 0) {\n                    out.assign(frames * kCacheChannels, 0.0f);\n                    const double dt = 1.0 / static_cast<double>(output_rate);\n                    for (size_t f = 0; f < frames; ++f) {\n                        const double t = start_seconds +\n                            source_rate * static_cast<double>(f) * dt;\n                        double source_pos =\n                            t * static_cast<double>(kCacheRate) -\n                            static_cast<double>(m_live_original_start_frame);\n                        if (source_pos < 0.0) source_pos = 0.0;\n                        size_t i0 = static_cast<size_t>(source_pos);\n                        if (i0 >= live_frames) i0 = live_frames - 1;\n                        const size_t i1 = (std::min)(i0 + 1, live_frames - 1);\n                        const float frac = static_cast<float>(\n                            source_pos - static_cast<double>(i0));\n                        for (unsigned ch = 0; ch < kCacheChannels; ++ch) {\n                            const float a = m_live_original[i0 * kCacheChannels + ch];\n                            const float b = m_live_original[i1 * kCacheChannels + ch];\n                            out[f * kCacheChannels + ch] = a + (b - a) * frac;\n                        }\n                    }\n"""
new = """                const double live_edge = 1.0 / static_cast<double>(kCacheRate);\n                if (need_start >= live_start - live_edge &&\n                    need_end <= live_end + live_edge && live_frames != 0) {\n                    out.assign(frames * kCacheChannels, 0.0f);\n                    const double dt = 1.0 / static_cast<double>(output_rate);\n                    for (size_t f = 0; f < frames; ++f) {\n                        const double t = start_seconds +\n                            source_rate * static_cast<double>(f) * dt;\n                        double source_pos =\n                            t * static_cast<double>(kCacheRate) -\n                            static_cast<double>(m_live_original_start_frame);\n                        source_pos = std::clamp(\n                            source_pos, 0.0, static_cast<double>(live_frames - 1));\n                        const size_t i0 = static_cast<size_t>(std::floor(source_pos));\n                        const size_t im1 = i0 > 0 ? i0 - 1 : 0;\n                        const size_t i1 = (std::min)(i0 + 1, live_frames - 1);\n                        const size_t i2 = (std::min)(i0 + 2, live_frames - 1);\n                        const float frac = static_cast<float>(\n                            source_pos - static_cast<double>(i0));\n                        for (unsigned ch = 0; ch < kCacheChannels; ++ch) {\n                            out[f * kCacheChannels + ch] = scratch_hermite4(\n                                frac,\n                                m_live_original[im1 * kCacheChannels + ch],\n                                m_live_original[i0 * kCacheChannels + ch],\n                                m_live_original[i1 * kCacheChannels + ch],\n                                m_live_original[i2 * kCacheChannels + ch]);\n                        }\n                    }\n"""
replace_once(old, new, "live cubic interpolation and edge tolerance")


# 5) Cubic interpolation for decoded cache segments too.
old = """                size_t i0 =\n                    static_cast<size_t>(\n                        source_pos);\n\n                if (i0 >=\n                    total_frames - 1) {\n\n                    i0 =\n                        total_frames - 1;\n\n                    return data[\n                        i0 * kCacheChannels +\n                        ch];\n                }\n\n                const size_t i1 =\n                    i0 + 1;\n\n                const float frac =\n                    static_cast<float>(\n                        source_pos -\n                        static_cast<double>(\n                            i0));\n\n                return\n                    data[\n                        i0 * kCacheChannels +\n                        ch] *\n                        (1.0f - frac) +\n                    data[\n                        i1 * kCacheChannels +\n                        ch] *\n                        frac;\n"""
new = """                source_pos = std::clamp(\n                    source_pos, 0.0, static_cast<double>(total_frames - 1));\n\n                const size_t i0 =\n                    static_cast<size_t>(std::floor(source_pos));\n                const size_t im1 = i0 > 0 ? i0 - 1 : 0;\n                const size_t i1 = (std::min)(i0 + 1, total_frames - 1);\n                const size_t i2 = (std::min)(i0 + 2, total_frames - 1);\n                const float frac = static_cast<float>(\n                    source_pos - static_cast<double>(i0));\n\n                return scratch_hermite4(\n                    frac,\n                    data[im1 * kCacheChannels + ch],\n                    data[i0 * kCacheChannels + ch],\n                    data[i1 * kCacheChannels + ch],\n                    data[i2 * kCacheChannels + ch]);\n"""
replace_once(old, new, "cache cubic interpolation")


# 6) Replace trajectory-replay scratch synthesis with a continuous stylus/rate engine.
start_marker = "            if (ts.state == stem_transport_scrub) {\n"
end_marker = "            if (ts.state == stem_transport_reverse) {\n"
start = s.find(start_marker)
if start < 0:
    raise SystemExit("scrub block start not found")
end = s.find(end_marker, start)
if end < 0:
    raise SystemExit("scrub block end not found")

new_block = r'''            if (ts.state == stem_transport_scrub) {
                // DJ-style platter engine: the hand supplies a target position and
                // velocity, while the audio thread owns one continuous virtual
                // stylus. This is the same separation used by mature DJ engines:
                // controller updates never become decoder seeks or standalone
                // chunks of mouse trajectory.
                if (!m_scrubRateValid) {
                    m_scrubRenderPosition = (std::max)(0.0, ts.render_seconds);
                    m_scrubPreviousRate = 0.0;
                    m_scrubRateValid = true;
                }

                const ULONGLONG now = GetTickCount64();
                const double motion_age = ts.scrub_motion_tick == 0
                    ? 999.0
                    : static_cast<double>(now - ts.scrub_motion_tick) / 1000.0;

                double hand_rate = motion_age <= kDjScratchMotionGraceSeconds
                    ? ts.scrub_velocity
                    : 0.0;
                hand_rate = std::clamp(
                    hand_rate, -kScrubMaxSourceRate, kScrubMaxSourceRate);

                double error = ts.position_seconds - m_scrubRenderPosition;
                const double half_sample =
                    0.5 / static_cast<double>(rate);

                // Once the hand is stationary and the stylus has reached it,
                // behave like a stopped record: no repeated sample/DC tone.
                if (std::abs(hand_rate) <= kDjScratchStoppedRate &&
                    std::abs(error) <= half_sample) {
                    m_scrubRenderPosition = ts.position_seconds;
                    m_scrubPreviousRate = 0.0;
                    transport().complete_scrub(m_scrubRenderPosition);
                    write_silence();
                    m_position_seconds += chunk_seconds;
                    m_using_stem = false;
                    return true;
                }

                // Correct target-vs-stylus phase with a bounded velocity term.
                // The hand velocity remains primary; position is only the servo
                // that prevents accumulated drift, rather than a seek request.
                const double correction = std::clamp(
                    error / kDjScratchPhaseCorrectionSeconds,
                    -kDjScratchMaxCorrectionRate,
                    kDjScratchMaxCorrectionRate);
                const double requested_rate = std::clamp(
                    hand_rate + correction,
                    -kScrubMaxSourceRate,
                    kScrubMaxSourceRate);

                const double previous_rate = m_scrubPreviousRate;
                const bool reversal =
                    previous_rate * requested_rate < 0.0;
                const bool strong_deceleration =
                    std::abs(requested_rate) + kDjScratchStrongDecel <
                    std::abs(previous_rate);

                // Mixxx filters ordinary acceleration but deliberately lets hard
                // deceleration through immediately so the platter does not
                // overshoot when the hand stops. Direction changes are handled
                // below by explicitly crossing through zero inside the buffer.
                double next_rate = requested_rate;
                if (!reversal && !strong_deceleration) {
                    next_rate =
                        previous_rate * (1.0 - kDjScratchRateFilter) +
                        requested_rate * kDjScratchRateFilter;
                }

                next_rate = std::clamp(
                    next_rate, -kScrubMaxSourceRate, kScrubMaxSourceRate);

                cache_manager().request_transport(
                    m_scrubRenderPosition, next_rate < 0.0);

                std::vector<float> preview(
                    frames * kCacheChannels, 0.0f);
                const size_t slice_frames = (std::max)(
                    static_cast<size_t>(1),
                    static_cast<size_t>(std::llround(
                        kScrubTrajectorySliceSeconds *
                        static_cast<double>(rate))));

                auto ramp_rate_at = [&](double x) -> double {
                    x = std::clamp(x, 0.0, 1.0);
                    if (reversal) {
                        // Mixxx's linear scaler treats a sign flip specially:
                        // old rate -> zero for the first half, then zero -> new
                        // rate for the second half. This avoids an instantaneous
                        // forward/reverse discontinuity.
                        if (x < 0.5) {
                            return previous_rate * (1.0 - 2.0 * x);
                        }
                        return next_rate * (2.0 * x - 1.0);
                    }
                    return previous_rate +
                        (next_rate - previous_rate) * x;
                };

                bool any_audio = false;
                size_t rendered_frames = 0;
                double source_position = m_scrubRenderPosition;

                while (rendered_frames < frames) {
                    const size_t count = (std::min)(
                        slice_frames, frames - rendered_frames);
                    const double midpoint =
                        (static_cast<double>(rendered_frames) +
                         0.5 * static_cast<double>(count)) /
                        static_cast<double>(frames);
                    double local_rate = ramp_rate_at(midpoint);

                    // Very close to zero, leave this slice silent just like a
                    // stationary record. Crucially, the source cursor does not
                    // advance while silent.
                    if (std::abs(local_rate) > kDjScratchStoppedRate) {
                        std::vector<float> part;
                        if (cache_manager().render(
                                mode,
                                source_position,
                                rate,
                                count,
                                part,
                                local_rate) &&
                            part.size() == count * kCacheChannels) {
                            std::copy(
                                part.begin(), part.end(),
                                preview.begin() +
                                    static_cast<std::ptrdiff_t>(
                                        rendered_frames * kCacheChannels));
                            source_position = (std::max)(
                                0.0,
                                source_position +
                                    local_rate *
                                    static_cast<double>(count) /
                                    static_cast<double>(rate));
                            any_audio = true;
                        } else {
                            // Do not move the stylus on a cache miss. The
                            // producer can catch up without creating the old
                            // silence cascade where every subsequent render was
                            // requested from a position we never actually heard.
                            cache_manager().request_transport(
                                source_position, local_rate < 0.0);
                        }
                    }

                    rendered_frames += count;
                }

                if (any_audio) {
                    write_preview(preview);
                } else {
                    write_silence();
                }

                m_scrubRenderPosition = source_position;
                m_scrubPreviousRate = next_rate;
                g_dbg_last_source_rate.store(
                    next_rate, std::memory_order_relaxed);
                transport().complete_scrub(m_scrubRenderPosition);

                m_position_seconds += chunk_seconds;
                m_using_stem = false;
                return true;
            }

'''
s = s[:start] + new_block + s[end:]
print("patched: continuous DJ scratch renderer")


# 7) Add/reset the persistent stylus position.
old = """        m_scrubRateValid = false;\n        m_scrubPreviousRate = 0.0;\n    }\n\n    bool m_have_position = false;\n"""
new = """        m_scrubRateValid = false;\n        m_scrubPreviousRate = 0.0;\n        m_scrubRenderPosition = 0.0;\n    }\n\n    bool m_have_position = false;\n"""
replace_once(old, new, "reset scratch stylus")

old = """    bool m_scrubRateValid = false;\n    double m_scrubPreviousRate = 0.0;\n};\n"""
new = """    bool m_scrubRateValid = false;\n    double m_scrubPreviousRate = 0.0;\n    double m_scrubRenderPosition = 0.0;\n};\n"""
replace_once(old, new, "scratch stylus member")

path.write_text(s, encoding="utf-8")
print("DJ scratch engine patch complete")
