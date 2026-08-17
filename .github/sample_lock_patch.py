from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')

old = '''constexpr double kScrubGestureDefaultDt = 0.016;\nconstexpr double kScrubGestureMinDt = 0.004;\nconstexpr double kScrubGestureMaxDt = 0.080;\nconstexpr double kScrubMaxSourceRate = 24.0;\n// A real platter follows hand velocity; it does not bend pitch to catch a\n// position target. Smooth only a little mouse timestamp jitter, then ramp each\n// DSP block from the last audible velocity to the latest hand velocity.\nconstexpr double kScrubVelocityCarry = 0.25;\nconstexpr double kScrubRateRampSeconds = 0.012;\nconstexpr double kScrubNearZeroRate = 0.015;\n'''
new = '''constexpr double kScrubGestureDefaultDt = 0.016;\nconstexpr double kScrubGestureMinDt = 0.001;\nconstexpr double kScrubGestureMaxDt = 0.250;\nconstexpr double kScrubMaxSourceRate = 24.0;\n// Pitch follows measured hand velocity. Keep only a very short audio-side ramp\n// for click-free acceleration, and sample-lock the virtual stylus whenever its\n// integrated cursor drifts materially away from the actual hand position.\nconstexpr double kScrubRateRampSeconds = 0.006;\nconstexpr double kScrubMaxCursorErrorSeconds = 0.060;\nconstexpr double kScrubReversalReanchorErrorSeconds = 0.015;\n'''
if old not in s:
    raise SystemExit('constant anchor not found')
s = s.replace(old, new, 1)

old = '''            const ULONGLONG now = GetTickCount64();\n\n            retarget =\n'''
new = '''            const ULONGLONG now = GetTickCount64();\n            const auto now_clock = std::chrono::steady_clock::now();\n\n            retarget =\n'''
if old not in s:
    raise SystemExit('set_scrub clock anchor not found')
s = s.replace(old, new, 1)

old = '''                double dt = kScrubGestureDefaultDt;\n                if (previous_state == stem_transport_scrub &&\n                    m_scrub_motion_tick != 0 && now > m_scrub_motion_tick) {\n                    dt = std::clamp(\n                        static_cast<double>(now - m_scrub_motion_tick) / 1000.0,\n                        kScrubGestureMinDt, kScrubGestureMaxDt);\n                }\n\n                double measured =\n                    dt > 0.0 ? (seconds - previous_position) / dt : 0.0;\n                measured = std::clamp(\n                    measured, -kScrubMaxSourceRate, kScrubMaxSourceRate);\n\n                // Keep reversals immediate, but remove a little timestamp jitter\n                // while the hand continues in the same direction. The audio-side\n                // renderer performs the short acceleration ramp; this estimate is\n                // still fundamentally distance / wall-clock time.\n                if (previous_state == stem_transport_scrub &&\n                    measured * m_scrub_velocity > 0.0) {\n                    measured =\n                        kScrubVelocityCarry * m_scrub_velocity +\n                        (1.0 - kScrubVelocityCarry) * measured;\n                }\n\n                m_scrub_velocity = measured;\n                m_scrub_motion_tick = now;\n                m_scrub_audible_until = now + kScrubAudibleSafetyMs;\n'''
new = '''                double dt = kScrubGestureDefaultDt;\n                if (previous_state == stem_transport_scrub &&\n                    m_scrub_motion_clock.time_since_epoch().count() != 0) {\n                    dt = std::clamp(\n                        std::chrono::duration<double>(\n                            now_clock - m_scrub_motion_clock).count(),\n                        kScrubGestureMinDt, kScrubGestureMaxDt);\n                }\n\n                // Use the actual high-resolution event interval. Do not carry the\n                // previous speed into this measurement; that was making slow mouse\n                // gestures chirp after irregular Windows message spacing.\n                double measured =\n                    dt > 0.0 ? (seconds - previous_position) / dt : 0.0;\n                measured = std::clamp(\n                    measured, -kScrubMaxSourceRate, kScrubMaxSourceRate);\n\n                m_scrub_velocity = measured;\n                m_scrub_motion_tick = now;\n                m_scrub_motion_clock = now_clock;\n                m_scrub_audible_until = now + kScrubAudibleSafetyMs;\n'''
if old not in s:
    raise SystemExit('velocity anchor not found')
s = s.replace(old, new, 1)

old = '''                m_scrub_velocity = 0.0;\n                m_scrub_audible_until = 0;\n                m_scrub_motion_tick = now;\n                m_render_seconds = seconds;\n'''
new = '''                m_scrub_velocity = 0.0;\n                m_scrub_audible_until = 0;\n                m_scrub_motion_tick = now;\n                m_scrub_motion_clock = now_clock;\n                m_render_seconds = seconds;\n'''
if old not in s:
    raise SystemExit('idle clock anchor not found')
s = s.replace(old, new, 1)

# Give HOLD a high-resolution timing anchor too.
old = '''            m_scrub_velocity = 0.0;\n            m_scrub_motion_tick = GetTickCount64();\n        }\n        cache_manager().request_transport(seconds, false);\n'''
new = '''            m_scrub_velocity = 0.0;\n            m_scrub_motion_tick = GetTickCount64();\n            m_scrub_motion_clock = std::chrono::steady_clock::now();\n        }\n        cache_manager().request_transport(seconds, false);\n'''
if old not in s:
    raise SystemExit('hold clock anchor not found')
s = s.replace(old, new, 1)

# Reset the high resolution clock on non-scrub transport states.
s = s.replace('''            m_scrub_velocity = 0.0;\n            m_scrub_motion_tick = 0;\n        }\n        cache_manager().request_transport(seconds, true);\n''', '''            m_scrub_velocity = 0.0;\n            m_scrub_motion_tick = 0;\n            m_scrub_motion_clock = {};\n        }\n        cache_manager().request_transport(seconds, true);\n''', 1)
s = s.replace('''            m_scrub_velocity = 0.0;\n            m_scrub_motion_tick = 0;\n        }\n        cache_manager().request_transport(seconds, false);\n''', '''            m_scrub_velocity = 0.0;\n            m_scrub_motion_tick = 0;\n            m_scrub_motion_clock = {};\n        }\n        cache_manager().request_transport(seconds, false);\n''', 1)
s = s.replace('''        m_scrub_velocity = 0.0;\n        m_scrub_motion_tick = 0;\n    }\n\n    transport_snapshot snapshot() const {\n''', '''        m_scrub_velocity = 0.0;\n        m_scrub_motion_tick = 0;\n        m_scrub_motion_clock = {};\n    }\n\n    transport_snapshot snapshot() const {\n''', 1)

old = '''    double m_scrub_velocity = 0.0;\n    ULONGLONG m_scrub_motion_tick = 0;\n};\n'''
new = '''    double m_scrub_velocity = 0.0;\n    ULONGLONG m_scrub_motion_tick = 0;\n    std::chrono::steady_clock::time_point m_scrub_motion_clock{};\n};\n'''
if old not in s:
    raise SystemExit('controller field anchor not found')
s = s.replace(old, new, 1)

old = '''                    const double target_rate = std::clamp(\n                        ts.scrub_velocity,\n                        -kScrubMaxSourceRate,\n                        kScrubMaxSourceRate);\n                    const double start_rate =\n                        m_scrubRateValid ? m_scrubPreviousRate : 0.0;\n                    const double ramp_seconds = (std::min)(\n                        kScrubRateRampSeconds, chunk_seconds);\n\n                    double predicted_move = target_rate * chunk_seconds;\n'''
new = '''                    const double target_rate = std::clamp(\n                        ts.scrub_velocity,\n                        -kScrubMaxSourceRate,\n                        kScrubMaxSourceRate);\n                    const double start_rate =\n                        m_scrubRateValid ? m_scrubPreviousRate : 0.0;\n                    const bool direction_reversal =\n                        m_scrubRateValid &&\n                        target_rate * m_scrubPreviousRate < 0.0;\n                    const bool large_cursor_drift =\n                        std::abs(delta) > kScrubMaxCursorErrorSeconds;\n                    const bool reversal_cursor_drift =\n                        direction_reversal &&\n                        std::abs(delta) > kScrubReversalReanchorErrorSeconds;\n\n                    // Keep the audible stylus attached to the hand. The previous\n                    // velocity-only build could integrate nearly a second away from\n                    // the visible mouse position; that can never resemble a record.\n                    double render_cursor =\n                        (large_cursor_drift || reversal_cursor_drift)\n                            ? ts.position_seconds\n                            : ts.render_seconds;\n\n                    const double ramp_seconds = (std::min)(\n                        kScrubRateRampSeconds, chunk_seconds);\n\n                    double predicted_move = target_rate * chunk_seconds;\n'''
if old not in s:
    raise SystemExit('sample lock anchor not found')
s = s.replace(old, new, 1)

old = '''                    const double desired_next_render = (std::max)(\n                        0.0, ts.render_seconds + predicted_move);\n'''
new = '''                    const double desired_next_render = (std::max)(\n                        0.0, render_cursor + predicted_move);\n'''
if old not in s:
    raise SystemExit('desired cursor anchor not found')
s = s.replace(old, new, 1)

old = '''                    bool rendered_ok = true;\n                    double render_cursor = ts.render_seconds;\n                    size_t rendered_frames = 0;\n'''
new = '''                    bool rendered_ok = true;\n                    size_t rendered_frames = 0;\n'''
if old not in s:
    raise SystemExit('duplicate render cursor anchor not found')
s = s.replace(old, new, 1)

old = '''                        // At the exact reversal point a physical record passes\n                        // through zero speed. Repeating one digital sample creates\n                        // a DC-like click, so make only that tiny near-zero interval\n                        // silent while the velocity crosses through zero.\n                        if (std::abs(local_rate) < kScrubNearZeroRate) {\n                            preview.insert(\n                                preview.end(), count * kCacheChannels, 0.0f);\n                            rendered_frames += count;\n                            continue;\n                        }\n\n'''
new = '''                        // Let the rate pass continuously through zero on a\n                        // reversal. The previous explicit silent slice audibly gated\n                        // every direction change and produced a synthetic chop.\n\n'''
if old not in s:
    raise SystemExit('near-zero silence anchor not found')
s = s.replace(old, new, 1)

p.write_text(s, encoding='utf-8')
print('patched sample-locked platter renderer')
