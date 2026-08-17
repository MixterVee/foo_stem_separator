from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')

old = '''struct transport_snapshot {\n    int state = stem_transport_normal;\n    double position_seconds = 0.0;\n    double render_seconds = 0.0;\n    ULONGLONG scrub_audible_until = 0;\n    double scrub_velocity = 0.0;\n    ULONGLONG scrub_motion_tick = 0;\n};\n\nclass transport_controller {\n'''
new = '''struct transport_snapshot {\n    int state = stem_transport_normal;\n    double position_seconds = 0.0;\n    double render_seconds = 0.0;\n    ULONGLONG scrub_audible_until = 0;\n    double scrub_velocity = 0.0;\n    ULONGLONG scrub_motion_tick = 0;\n};\n\nstruct scrub_motion_event {\n    double position_seconds = 0.0;\n    std::chrono::steady_clock::time_point when{};\n};\n\nclass transport_controller {\n'''
if old not in s:
    raise SystemExit('transport snapshot anchor not found')
s = s.replace(old, new, 1)

old = '''            m_scrub_velocity = 0.0;\n            m_scrub_motion_tick = GetTickCount64();\n            m_scrub_motion_clock = std::chrono::steady_clock::now();\n        }\n'''
new = '''            m_scrub_velocity = 0.0;\n            m_scrub_motion_tick = GetTickCount64();\n            m_scrub_motion_clock = std::chrono::steady_clock::now();\n            m_scrub_motion_events.clear();\n            m_scrub_motion_events.push_back(\n                scrub_motion_event{seconds, m_scrub_motion_clock});\n        }\n'''
if old not in s:
    raise SystemExit('hold event anchor not found')
s = s.replace(old, new, 1)

old = '''                m_scrub_velocity = measured;\n                m_scrub_motion_tick = now;\n                m_scrub_motion_clock = now_clock;\n                m_scrub_audible_until = now + kScrubAudibleSafetyMs;\n            } else if (previous_state == stem_transport_scrub) {\n'''
new = '''                m_scrub_velocity = measured;\n                m_scrub_motion_tick = now;\n                m_scrub_motion_clock = now_clock;\n                m_scrub_audible_until = now + kScrubAudibleSafetyMs;\n\n                // Preserve the real mouse trajectory instead of extrapolating one\n                // velocity across an arbitrary foobar DSP block. HOLD already seeds\n                // the queue with the grab position. For any other entry path, create\n                // a synthetic predecessor at the measured event interval.\n                if (previous_state != stem_transport_scrub &&\n                    previous_state != stem_transport_hold) {\n                    m_scrub_motion_events.clear();\n                    m_scrub_motion_events.push_back(scrub_motion_event{\n                        previous_position,\n                        now_clock - std::chrono::duration_cast<\n                            std::chrono::steady_clock::duration>(\n                                std::chrono::duration<double>(dt))});\n                }\n                if (m_scrub_motion_events.empty()) {\n                    m_scrub_motion_events.push_back(scrub_motion_event{\n                        previous_position,\n                        now_clock - std::chrono::duration_cast<\n                            std::chrono::steady_clock::duration>(\n                                std::chrono::duration<double>(dt))});\n                }\n                m_scrub_motion_events.push_back(\n                    scrub_motion_event{seconds, now_clock});\n                while (m_scrub_motion_events.size() > 96) {\n                    m_scrub_motion_events.pop_front();\n                }\n            } else if (previous_state == stem_transport_scrub) {\n'''
if old not in s:
    raise SystemExit('scrub event push anchor not found')
s = s.replace(old, new, 1)

old = '''                m_scrub_motion_tick = now;\n                m_scrub_motion_clock = now_clock;\n                m_render_seconds = seconds;\n            }\n'''
new = '''                m_scrub_motion_tick = now;\n                m_scrub_motion_clock = now_clock;\n                m_render_seconds = seconds;\n                // Soft idle is a real stationary platter point. Drop any already\n                // obsolete path so a later DSP callback cannot replay it.\n                m_scrub_motion_events.clear();\n                m_scrub_motion_events.push_back(\n                    scrub_motion_event{seconds, now_clock});\n            }\n'''
if old not in s:
    raise SystemExit('soft idle event anchor not found')
s = s.replace(old, new, 1)

# Clear queued hand motion when entering non-scrub states.
s = s.replace('''            m_scrub_motion_tick = 0;\n            m_scrub_motion_clock = {};\n        }\n        cache_manager().request_transport(seconds, true);\n''', '''            m_scrub_motion_tick = 0;\n            m_scrub_motion_clock = {};\n            m_scrub_motion_events.clear();\n        }\n        cache_manager().request_transport(seconds, true);\n''', 1)
s = s.replace('''            m_scrub_motion_tick = 0;\n            m_scrub_motion_clock = {};\n        }\n        cache_manager().request_transport(seconds, false);\n''', '''            m_scrub_motion_tick = 0;\n            m_scrub_motion_clock = {};\n            m_scrub_motion_events.clear();\n        }\n        cache_manager().request_transport(seconds, false);\n''', 1)
s = s.replace('''        m_scrub_motion_tick = 0;\n        m_scrub_motion_clock = {};\n    }\n\n    transport_snapshot snapshot() const {\n''', '''        m_scrub_motion_tick = 0;\n        m_scrub_motion_clock = {};\n        m_scrub_motion_events.clear();\n    }\n\n    transport_snapshot snapshot() const {\n''', 1)

old = '''    double visible_position() const {\n        std::lock_guard<std::mutex> lock(m_mutex);\n        return m_position_seconds;\n    }\n'''
new = '''    bool take_scrub_motion(std::vector<scrub_motion_event>& out) {\n        out.clear();\n        std::lock_guard<std::mutex> lock(m_mutex);\n        if (m_state != stem_transport_scrub ||\n            m_scrub_motion_events.size() < 2) {\n            return false;\n        }\n\n        // Keep only recent movement. Replaying an old gesture after the audio\n        // callback was delayed is worse than dropping it; live platter response\n        // must always favor the newest hand position. Retain one predecessor so\n        // the oldest surviving segment still has a start point.\n        const auto cutoff = std::chrono::steady_clock::now() -\n            std::chrono::milliseconds(250);\n        while (m_scrub_motion_events.size() > 2 &&\n               m_scrub_motion_events[1].when < cutoff) {\n            m_scrub_motion_events.pop_front();\n        }\n\n        out.assign(\n            m_scrub_motion_events.begin(),\n            m_scrub_motion_events.end());\n\n        const scrub_motion_event last = m_scrub_motion_events.back();\n        m_scrub_motion_events.clear();\n        m_scrub_motion_events.push_back(last);\n        return out.size() >= 2;\n    }\n\n    double visible_position() const {\n        std::lock_guard<std::mutex> lock(m_mutex);\n        return m_position_seconds;\n    }\n'''
if old not in s:
    raise SystemExit('take motion insertion anchor not found')
s = s.replace(old, new, 1)

old = '''    ULONGLONG m_scrub_motion_tick = 0;\n    std::chrono::steady_clock::time_point m_scrub_motion_clock{};\n};\n'''
new = '''    ULONGLONG m_scrub_motion_tick = 0;\n    std::chrono::steady_clock::time_point m_scrub_motion_clock{};\n    std::deque<scrub_motion_event> m_scrub_motion_events;\n};\n'''
if old not in s:
    raise SystemExit('motion queue field anchor not found')
s = s.replace(old, new, 1)

start = s.find('''            if (ts.state == stem_transport_scrub) {\n                const double delta =\n''')
end = s.find('''            if (ts.state == stem_transport_reverse) {\n''', start)
if start < 0 or end < 0:
    raise SystemExit('scrub branch anchors not found')

replacement = r'''            if (ts.state == stem_transport_scrub) {
                const double move_epsilon =
                    0.5 / static_cast<double>(rate);
                const bool fresh_motion =
                    GetTickCount64() <= ts.scrub_audible_until;

                std::vector<scrub_motion_event> motion;
                const bool have_motion =
                    fresh_motion && transport().take_scrub_motion(motion);

                if (have_motion) {
                    struct motion_segment {
                        double start_seconds = 0.0;
                        double end_seconds = 0.0;
                        double duration_seconds = 0.0;
                        double source_rate = 0.0;
                        size_t output_frames = 0;
                    };

                    std::vector<motion_segment> segments;
                    segments.reserve(motion.size());
                    for (size_t i = 1; i < motion.size(); ++i) {
                        double dt = std::chrono::duration<double>(
                            motion[i].when - motion[i - 1].when).count();
                        dt = std::clamp(
                            dt, kScrubGestureMinDt, kScrubGestureMaxDt);
                        const double delta =
                            motion[i].position_seconds -
                            motion[i - 1].position_seconds;
                        if (std::abs(delta) <= move_epsilon) continue;

                        const double source_rate = std::clamp(
                            delta / dt,
                            -kScrubMaxSourceRate,
                            kScrubMaxSourceRate);
                        if (std::abs(source_rate) <= 1.0e-6) continue;

                        size_t count = static_cast<size_t>(std::llround(
                            dt * static_cast<double>(rate)));
                        count = (std::max)(static_cast<size_t>(1), count);
                        segments.push_back(motion_segment{
                            motion[i - 1].position_seconds,
                            motion[i].position_seconds,
                            dt,
                            source_rate,
                            count});
                    }

                    // The output block may be shorter than the accumulated mouse
                    // path. Drop the OLDEST movement first so what reaches the
                    // speakers is the most recent hand motion, not stale history.
                    size_t total_motion_frames = 0;
                    for (const auto& seg : segments) {
                        total_motion_frames += seg.output_frames;
                    }
                    size_t trim_frames =
                        total_motion_frames > frames
                            ? total_motion_frames - frames
                            : 0;

                    std::vector<float> preview(
                        frames * kCacheChannels, 0.0f);
                    size_t write_frame = 0;
                    bool any_audio = false;
                    double newest_rate = 0.0;

                    for (const auto& seg : segments) {
                        if (write_frame >= frames) break;

                        size_t skip = (std::min)(trim_frames, seg.output_frames);
                        trim_frames -= skip;
                        size_t count = seg.output_frames - skip;
                        if (count == 0) continue;
                        count = (std::min)(count, frames - write_frame);

                        const double skip_seconds =
                            static_cast<double>(skip) /
                            static_cast<double>(rate);
                        double source_start =
                            seg.start_seconds +
                            seg.source_rate * skip_seconds;
                        source_start = (std::max)(0.0, source_start);

                        // Never extrapolate past the actual mouse endpoint. If
                        // clamping/rounding changed the requested duration slightly,
                        // trim this segment to the recorded hand distance.
                        const double available_source =
                            std::abs(seg.end_seconds - source_start);
                        const double requested_source =
                            std::abs(seg.source_rate) *
                            static_cast<double>(count) /
                            static_cast<double>(rate);
                        if (requested_source > available_source + 1.0e-9) {
                            const size_t bounded = static_cast<size_t>(std::floor(
                                available_source * static_cast<double>(rate) /
                                std::abs(seg.source_rate)));
                            count = (std::min)(count, bounded);
                        }
                        if (count == 0) continue;

                        cache_manager().request_transport(
                            source_start, seg.source_rate < 0.0);

                        std::vector<float> part;
                        if (!cache_manager().render(
                                mode, source_start, rate, count,
                                part, seg.source_rate) ||
                            part.size() != count * kCacheChannels) {
                            // Keep timing intact with silence for just this segment.
                            write_frame += count;
                            continue;
                        }

                        std::copy(
                            part.begin(), part.end(),
                            preview.begin() + static_cast<std::ptrdiff_t>(
                                write_frame * kCacheChannels));
                        write_frame += count;
                        any_audio = true;
                        newest_rate = seg.source_rate;
                    }

                    if (any_audio) {
                        // Stop cleanly at the newest mouse point. A tiny amplitude
                        // taper prevents a hard step into the stationary (silent)
                        // part of a large foobar chunk without changing scratch pitch.
                        if (write_frame != 0 && write_frame < frames) {
                            const size_t fade_frames = (std::min)(
                                write_frame,
                                (std::max)(static_cast<size_t>(1),
                                    static_cast<size_t>(
                                        0.0015 * static_cast<double>(rate))));
                            for (size_t f = 0; f < fade_frames; ++f) {
                                const double gain =
                                    static_cast<double>(fade_frames - f - 1) /
                                    static_cast<double>(fade_frames);
                                const size_t frame_index =
                                    write_frame - fade_frames + f;
                                for (unsigned ch = 0; ch < kCacheChannels; ++ch) {
                                    preview[frame_index * kCacheChannels + ch] =
                                        static_cast<float>(
                                            static_cast<double>(
                                                preview[frame_index * kCacheChannels + ch]) *
                                            gain);
                                }
                            }
                        }

                        write_preview(preview);
                        g_dbg_last_source_rate.store(
                            newest_rate, std::memory_order_relaxed);
                    } else {
                        write_silence();
                    }

                    // Position is dictated by the hand, never by integration of
                    // output block duration. This is the core trajectory change.
                    transport().complete_scrub(ts.position_seconds);
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
s = s[:start] + replacement + s[end:]

p.write_text(s, encoding='utf-8')
print('patched timestamped hand trajectory scratch renderer')
