from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')

# Add wall-clock trajectory constants next to the existing scrub timing constants.
anchor = '''constexpr double kScrubGestureDefaultDt = 0.016;\nconstexpr double kScrubGestureMinDt = 0.001;\nconstexpr double kScrubGestureMaxDt = 0.250;\nconstexpr double kScrubMaxSourceRate = 24.0;\n'''
replacement = '''constexpr double kScrubGestureDefaultDt = 0.016;\nconstexpr double kScrubGestureMinDt = 0.001;\nconstexpr double kScrubGestureMaxDt = 0.250;\nconstexpr double kScrubMaxSourceRate = 24.0;\n// Render a short, fixed-delay window of the real mouse path. This decouples\n// scratch audio from arbitrary foobar DSP block boundaries and averages away\n// 1-2 ms Windows mouse-message timing spikes without adding a perceptible delay.\nconstexpr double kScrubTrajectoryLagSeconds = 0.010;\nconstexpr double kScrubTrajectorySliceSeconds = 0.008;\nconstexpr double kScrubTrajectoryExtrapolateSeconds = 0.012;\nconstexpr double kScrubTrajectoryHistorySeconds = 0.750;\n'''
if anchor not in s:
    raise SystemExit('constant anchor not found')
s = s.replace(anchor, replacement, 1)

# Soft idle must preserve recent history so an audio callback that arrives a few
# milliseconds later can still play the end of the real hand gesture.
old = '''                m_render_seconds = seconds;\n                // Soft idle is a real stationary platter point. Drop any already\n                // obsolete path so a later DSP callback cannot replay it.\n                m_scrub_motion_events.clear();\n                m_scrub_motion_events.push_back(\n                    scrub_motion_event{seconds, now_clock});\n'''
new = '''                m_render_seconds = seconds;\n                // Soft idle is a real stationary platter point. Keep the recent\n                // trajectory and append a stationary endpoint; the DSP renders a\n                // fixed wall-clock window, so preserving the tail cannot replay old\n                // motion but does let the last few milliseconds drain naturally.\n                if (m_scrub_motion_events.empty() ||\n                    m_scrub_motion_events.back().when != now_clock) {\n                    m_scrub_motion_events.push_back(\n                        scrub_motion_event{seconds, now_clock});\n                }\n                while (m_scrub_motion_events.size() > 128) {\n                    m_scrub_motion_events.pop_front();\n                }\n'''
if old not in s:
    raise SystemExit('soft idle anchor not found')
s = s.replace(old, new, 1)

# Replace destructive motion consumption with a non-destructive recent-history snapshot.
start = s.find('''    bool take_scrub_motion(std::vector<scrub_motion_event>& out) {\n''')
end = s.find('''    double visible_position() const {\n''', start)
if start < 0 or end < 0:
    raise SystemExit('take_scrub_motion block not found')
new_method = r'''    bool snapshot_scrub_motion(std::vector<scrub_motion_event>& out) {
        out.clear();
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state != stem_transport_scrub || m_scrub_motion_events.empty()) {
            return false;
        }

        const auto cutoff = std::chrono::steady_clock::now() -
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(kScrubTrajectoryHistorySeconds));

        // Retain one predecessor before the cutoff so interpolation at the start
        // of the requested wall-clock window remains continuous.
        while (m_scrub_motion_events.size() > 2 &&
               m_scrub_motion_events[1].when < cutoff) {
            m_scrub_motion_events.pop_front();
        }

        out.assign(
            m_scrub_motion_events.begin(),
            m_scrub_motion_events.end());
        return !out.empty();
    }

'''
s = s[:start] + new_method + s[end:]

# Replace event-sized output fragments with a continuous wall-clock trajectory renderer.
start = s.find('''            if (ts.state == stem_transport_scrub) {\n                const double move_epsilon =\n''')
end = s.find('''            if (ts.state == stem_transport_reverse) {\n''', start)
if start < 0 or end < 0:
    raise SystemExit('scrub branch anchors not found')

new_branch = r'''            if (ts.state == stem_transport_scrub) {
                const double move_epsilon =
                    0.5 / static_cast<double>(rate);

                std::vector<scrub_motion_event> motion;
                const bool have_history =
                    transport().snapshot_scrub_motion(motion);

                if (have_history && !motion.empty()) {
                    using scrub_clock = std::chrono::steady_clock;
                    const auto now_clock = scrub_clock::now();
                    const auto lag = std::chrono::duration_cast<scrub_clock::duration>(
                        std::chrono::duration<double>(kScrubTrajectoryLagSeconds));
                    const auto window_end = now_clock - lag;
                    const auto window_start = window_end -
                        std::chrono::duration_cast<scrub_clock::duration>(
                            std::chrono::duration<double>(chunk_seconds));

                    auto position_at = [&](scrub_clock::time_point when,
                                           double& position,
                                           bool& moving) {
                        moving = false;
                        if (motion.empty()) {
                            position = ts.position_seconds;
                            return;
                        }

                        if (when <= motion.front().when) {
                            position = motion.front().position_seconds;
                            return;
                        }

                        for (size_t i = 1; i < motion.size(); ++i) {
                            if (when <= motion[i].when) {
                                const auto& a = motion[i - 1];
                                const auto& b = motion[i];
                                const double dt = std::chrono::duration<double>(
                                    b.when - a.when).count();
                                if (dt <= 1.0e-9) {
                                    position = b.position_seconds;
                                    moving = std::abs(
                                        b.position_seconds - a.position_seconds) >
                                        move_epsilon;
                                    return;
                                }
                                const double elapsed = std::chrono::duration<double>(
                                    when - a.when).count();
                                const double alpha = std::clamp(
                                    elapsed / dt, 0.0, 1.0);
                                position =
                                    a.position_seconds +
                                    (b.position_seconds - a.position_seconds) * alpha;
                                moving = std::abs(
                                    b.position_seconds - a.position_seconds) >
                                    move_epsilon;
                                return;
                            }
                        }

                        const auto& last = motion.back();
                        position = last.position_seconds;
                        const double age = std::chrono::duration<double>(
                            when - last.when).count();
                        if (age <= 0.0 ||
                            age > kScrubTrajectoryExtrapolateSeconds ||
                            motion.size() < 2) {
                            return;
                        }

                        // Estimate the newest speed over at least ~8 ms whenever
                        // possible. This avoids treating a single 1 ms mouse packet
                        // as a 20x-24x platter impulse.
                        size_t prev = motion.size() - 2;
                        while (prev > 0) {
                            const double span = std::chrono::duration<double>(
                                last.when - motion[prev].when).count();
                            if (span >= kScrubTrajectorySliceSeconds) break;
                            --prev;
                        }
                        const double dt = std::chrono::duration<double>(
                            last.when - motion[prev].when).count();
                        if (dt <= 1.0e-9) return;

                        double velocity =
                            (last.position_seconds -
                             motion[prev].position_seconds) / dt;
                        velocity = std::clamp(
                            velocity,
                            -kScrubMaxSourceRate,
                            kScrubMaxSourceRate);
                        position = (std::max)(
                            0.0,
                            last.position_seconds + velocity * age);
                        moving = std::abs(velocity) > 1.0e-4;
                    };

                    std::vector<float> preview(
                        frames * kCacheChannels, 0.0f);
                    const size_t slice_frames = (std::max)(
                        static_cast<size_t>(1),
                        static_cast<size_t>(std::llround(
                            kScrubTrajectorySliceSeconds *
                            static_cast<double>(rate))));

                    bool any_audio = false;
                    double newest_rate = 0.0;
                    double final_position = ts.position_seconds;
                    size_t rendered_frames = 0;

                    while (rendered_frames < frames) {
                        const size_t count = (std::min)(
                            slice_frames, frames - rendered_frames);
                        const double slice_seconds =
                            static_cast<double>(count) /
                            static_cast<double>(rate);

                        const auto t0 = window_start +
                            std::chrono::duration_cast<scrub_clock::duration>(
                                std::chrono::duration<double>(
                                    static_cast<double>(rendered_frames) /
                                    static_cast<double>(rate)));
                        const auto t1 = t0 +
                            std::chrono::duration_cast<scrub_clock::duration>(
                                std::chrono::duration<double>(slice_seconds));

                        double p0 = ts.position_seconds;
                        double p1 = ts.position_seconds;
                        bool moving0 = false;
                        bool moving1 = false;
                        position_at(t0, p0, moving0);
                        position_at(t1, p1, moving1);
                        p0 = (std::max)(0.0, p0);
                        p1 = (std::max)(0.0, p1);
                        final_position = p1;

                        const double delta = p1 - p0;
                        double local_rate =
                            slice_seconds > 0.0 ? delta / slice_seconds : 0.0;

                        // The 8 ms wall-clock slice itself is the jitter filter.
                        // Keep only a very high safety clamp; ordinary scratching
                        // should no longer hit it just because two mouse messages
                        // happened 1 ms apart.
                        local_rate = std::clamp(
                            local_rate,
                            -kScrubMaxSourceRate,
                            kScrubMaxSourceRate);

                        if ((moving0 || moving1) &&
                            std::abs(local_rate) > 1.0e-4 &&
                            std::abs(delta) > move_epsilon) {
                            cache_manager().request_transport(
                                p0, local_rate < 0.0);

                            std::vector<float> part;
                            if (cache_manager().render(
                                    mode, p0, rate, count,
                                    part, local_rate) &&
                                part.size() == count * kCacheChannels) {
                                std::copy(
                                    part.begin(), part.end(),
                                    preview.begin() +
                                        static_cast<std::ptrdiff_t>(
                                            rendered_frames * kCacheChannels));
                                any_audio = true;
                                newest_rate = local_rate;
                            }
                        }

                        rendered_frames += count;
                    }

                    if (any_audio) {
                        write_preview(preview);
                        g_dbg_last_source_rate.store(
                            newest_rate, std::memory_order_relaxed);
                    } else {
                        write_silence();
                    }

                    // Debug/render position follows the trajectory time that was
                    // actually synthesized, not an arbitrarily old DSP snapshot.
                    transport().complete_scrub(final_position);
                    m_scrubPreviousRate = 0.0;
                    m_scrubRateValid = false;
                } else {
                    write_silence();
                    m_scrubPreviousRate = 0.0;
                    m_scrubRateValid = false;
                    transport().complete_scrub(ts.position_seconds);
                }

                m_position_seconds += chunk_seconds;
                m_using_stem = false;
                return true;
            }

'''
s = s[:start] + new_branch + s[end:]

p.write_text(s, encoding='utf-8')
print('patched continuous wall-clock scratch trajectory')
