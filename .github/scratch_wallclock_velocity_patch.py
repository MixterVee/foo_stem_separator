from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')

# Wall-clock gesture velocity constants. Scratch pitch must come from how fast
# the hand moved, not from whichever foobar DSP block size happened to arrive.
anchor = '''constexpr double kScrubSubBlockSeconds = 0.004;\nconstexpr double kScrubCarrySlopeLimit = 2.0;\n'''
replacement = '''constexpr double kScrubSubBlockSeconds = 0.004;\nconstexpr double kScrubCarrySlopeLimit = 2.0;\nconstexpr double kScrubGestureDefaultDt = 0.016;\nconstexpr double kScrubGestureMinDt = 0.004;\nconstexpr double kScrubGestureMaxDt = 0.080;\nconstexpr double kScrubMaxSourceRate = 24.0;\nconstexpr double kScrubPhaseCorrectionMix = 0.22;\n'''
if anchor not in s:
    raise SystemExit('constants anchor not found')
s = s.replace(anchor, replacement, 1)

old = '''struct transport_snapshot {\n    int state = stem_transport_normal;\n    double position_seconds = 0.0;\n    double render_seconds = 0.0;\n    ULONGLONG scrub_audible_until = 0;\n};\n'''
new = '''struct transport_snapshot {\n    int state = stem_transport_normal;\n    double position_seconds = 0.0;\n    double render_seconds = 0.0;\n    ULONGLONG scrub_audible_until = 0;\n    double scrub_velocity = 0.0;\n    ULONGLONG scrub_motion_tick = 0;\n};\n'''
if old not in s:
    raise SystemExit('transport snapshot not found')
s = s.replace(old, new, 1)

old = '''            m_state = stem_transport_hold;\n            m_position_seconds = seconds;\n            m_render_seconds = seconds;\n            m_scrub_audible_until = 0;\n'''
new = '''            m_state = stem_transport_hold;\n            m_position_seconds = seconds;\n            m_render_seconds = seconds;\n            m_scrub_audible_until = 0;\n            m_scrub_velocity = 0.0;\n            m_scrub_motion_tick = GetTickCount64();\n'''
if old not in s:
    raise SystemExit('set_hold block not found')
s = s.replace(old, new, 1)

start = s.index('    void set_scrub(double seconds) {')
end = s.index('    void set_reverse(double seconds) {', start)
old = s[start:end]
new = '''    void set_scrub(double seconds) {\n        seconds = (std::max)(0.0, seconds);\n        bool retarget = true;\n        bool reverse = false;\n        {\n            std::lock_guard<std::mutex> lock(m_mutex);\n            const int previous_state = m_state;\n            const double previous_position = m_position_seconds;\n            const ULONGLONG now = GetTickCount64();\n\n            retarget =\n                previous_state != stem_transport_scrub ||\n                std::abs(seconds - previous_position) >\n                    kScrubKeepaliveToleranceSeconds;\n\n            // A centered grab enters from HOLD, whose render cursor is the exact\n            // sample that was under the playhead when the platter was grabbed.\n            if (previous_state != stem_transport_scrub &&\n                previous_state != stem_transport_hold) {\n                m_render_seconds = seconds;\n            }\n\n            if (retarget) {\n                double dt = kScrubGestureDefaultDt;\n                if (previous_state == stem_transport_scrub &&\n                    m_scrub_motion_tick != 0 && now > m_scrub_motion_tick) {\n                    dt = std::clamp(\n                        static_cast<double>(now - m_scrub_motion_tick) / 1000.0,\n                        kScrubGestureMinDt, kScrubGestureMaxDt);\n                }\n\n                double measured =\n                    dt > 0.0 ? (seconds - previous_position) / dt : 0.0;\n                measured = std::clamp(\n                    measured, -kScrubMaxSourceRate, kScrubMaxSourceRate);\n\n                // Preserve quick direction changes. Only a small amount of\n                // same-direction carry removes mouse timestamp jitter; unlike the\n                // previous experiment, this is not a low-pass platter filter.\n                if (previous_state == stem_transport_scrub &&\n                    measured * m_scrub_velocity > 0.0) {\n                    measured = 0.20 * m_scrub_velocity + 0.80 * measured;\n                }\n\n                m_scrub_velocity = measured;\n                m_scrub_motion_tick = now;\n                m_scrub_audible_until = now + kScrubAudibleSafetyMs;\n            }\n\n            m_state = stem_transport_scrub;\n            m_position_seconds = seconds;\n            reverse = m_scrub_velocity < 0.0 || seconds < m_render_seconds;\n        }\n\n        if (retarget) {\n            cache_manager().request_transport(seconds, reverse);\n        }\n    }\n\n'''
s = s[:start] + new + s[end:]

# Clear measured gesture velocity whenever scrub transport is not active.
for old, new in [
('''            m_state = stem_transport_reverse;\n            m_position_seconds = seconds;\n            m_render_seconds = seconds;\n            m_scrub_audible_until = 0;\n''',
 '''            m_state = stem_transport_reverse;\n            m_position_seconds = seconds;\n            m_render_seconds = seconds;\n            m_scrub_audible_until = 0;\n            m_scrub_velocity = 0.0;\n            m_scrub_motion_tick = 0;\n'''),
('''            m_state = stem_transport_release_wait;\n            m_position_seconds = seconds;\n            m_render_seconds = seconds;\n            m_scrub_audible_until = 0;\n''',
 '''            m_state = stem_transport_release_wait;\n            m_position_seconds = seconds;\n            m_render_seconds = seconds;\n            m_scrub_audible_until = 0;\n            m_scrub_velocity = 0.0;\n            m_scrub_motion_tick = 0;\n'''),
('''        m_state = stem_transport_normal;\n        m_scrub_audible_until = 0;\n''',
 '''        m_state = stem_transport_normal;\n        m_scrub_audible_until = 0;\n        m_scrub_velocity = 0.0;\n        m_scrub_motion_tick = 0;\n''')]:
    if old not in s:
        raise SystemExit('transport reset block not found')
    s = s.replace(old, new, 1)

old = '''        return transport_snapshot{\n            m_state, m_position_seconds, m_render_seconds, m_scrub_audible_until};\n'''
new = '''        return transport_snapshot{\n            m_state, m_position_seconds, m_render_seconds,\n            m_scrub_audible_until, m_scrub_velocity, m_scrub_motion_tick};\n'''
if old not in s:
    raise SystemExit('snapshot return not found')
s = s.replace(old, new, 1)

old = '''    double m_render_seconds = 0.0;\n    ULONGLONG m_scrub_audible_until = 0;\n};\n'''
new = '''    double m_render_seconds = 0.0;\n    ULONGLONG m_scrub_audible_until = 0;\n    double m_scrub_velocity = 0.0;\n    ULONGLONG m_scrub_motion_tick = 0;\n};\n'''
if old not in s:
    raise SystemExit('controller members not found')
s = s.replace(old, new, 1)

# Replace the sub-block experiment with a real hand-velocity renderer. The DSP
# cursor remains continuous; the latest visual target contributes only a bounded
# phase correction so block size no longer determines scratch pitch.
start = s.index('            if (ts.state == stem_transport_scrub) {')
end = s.index('            if (ts.state == stem_transport_reverse) {', start)
old = s[start:end]
new = '''            if (ts.state == stem_transport_scrub) {\n                const double delta =\n                    ts.position_seconds - ts.render_seconds;\n                const double move_epsilon =\n                    0.5 / static_cast<double>(rate);\n                const bool fresh_motion =\n                    GetTickCount64() <= ts.scrub_audible_until;\n\n                if (fresh_motion &&\n                    (std::abs(delta) > move_epsilon ||\n                     std::abs(ts.scrub_velocity) > 1.0e-4)) {\n                    const double output_span =\n                        frames > 1\n                            ? static_cast<double>(frames - 1) /\n                                static_cast<double>(rate)\n                            : chunk_seconds;\n\n                    // Primary rate comes from mouse distance / wall-clock time.\n                    // This is the key difference from the previous renderer: an\n                    // identical hand gesture now has the same pitch/speed no matter\n                    // which foobar DSP block size happens to carry it.\n                    double source_rate = ts.scrub_velocity;\n\n                    if (output_span > 0.0 && std::abs(delta) > move_epsilon) {\n                        const double block_align_rate = delta / output_span;\n                        const double correction_limit = (std::max)(\n                            0.75, std::abs(source_rate) * 0.50);\n                        const double correction = std::clamp(\n                            (block_align_rate - source_rate) *\n                                kScrubPhaseCorrectionMix,\n                            -correction_limit, correction_limit);\n                        source_rate += correction;\n\n                        // Never run through the visible hand position in the same\n                        // direction. When we are already close, land exactly on it.\n                        if (delta * source_rate > 0.0 &&\n                            std::abs(source_rate * output_span) >\n                                std::abs(delta)) {\n                            source_rate = block_align_rate;\n                        }\n                    }\n\n                    source_rate = std::clamp(\n                        source_rate, -kScrubMaxSourceRate, kScrubMaxSourceRate);\n\n                    const double next_render = (std::max)(\n                        0.0, ts.render_seconds + source_rate * chunk_seconds);\n                    cache_manager().request_transport(\n                        next_render, source_rate < 0.0);\n\n                    std::vector<float> preview;\n                    if (std::abs(source_rate) > 1.0e-6 &&\n                        cache_manager().render(\n                            mode, ts.render_seconds, rate, frames,\n                            preview, source_rate) &&\n                        preview.size() == frames * kCacheChannels) {\n                        write_preview(preview);\n                    } else {\n                        write_silence();\n                    }\n\n                    // Advance by what was actually rendered, not by snapping to the\n                    // latest mouse target. That keeps the audio trajectory continuous\n                    // across callback boundaries while the small phase correction\n                    // keeps it visually aligned.\n                    transport().complete_scrub(next_render);\n                } else {\n                    write_silence();\n                    if (std::abs(delta) > move_epsilon) {\n                        transport().complete_scrub(ts.position_seconds);\n                    }\n                }\n                m_position_seconds += chunk_seconds;\n                m_using_stem = false;\n                return true;\n            }\n\n'''
s = s[:start] + new + s[end:]

p.write_text(s, encoding='utf-8')
print('patched wall-clock scratch velocity')
