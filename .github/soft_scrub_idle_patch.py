from pathlib import Path

p = Path('stem_dsp.cpp')
s = p.read_text(encoding='utf-8')

old = '''            if (retarget) {
                double dt = kScrubGestureDefaultDt;
                if (previous_state == stem_transport_scrub &&
                    m_scrub_motion_tick != 0 && now > m_scrub_motion_tick) {
                    dt = std::clamp(
                        static_cast<double>(now - m_scrub_motion_tick) / 1000.0,
                        kScrubGestureMinDt, kScrubGestureMaxDt);
                }

                double measured =
                    dt > 0.0 ? (seconds - previous_position) / dt : 0.0;
                measured = std::clamp(
                    measured, -kScrubMaxSourceRate, kScrubMaxSourceRate);

                // Preserve quick direction changes. Only a small amount of
                // same-direction carry removes mouse timestamp jitter; unlike the
                // previous experiment, this is not a low-pass platter filter.
                if (previous_state == stem_transport_scrub &&
                    measured * m_scrub_velocity > 0.0) {
                    measured = 0.20 * m_scrub_velocity + 0.80 * measured;
                }

                m_scrub_velocity = measured;
                m_scrub_motion_tick = now;
                m_scrub_audible_until = now + kScrubAudibleSafetyMs;
            }

            m_state = stem_transport_scrub;
            m_position_seconds = seconds;
            reverse = m_scrub_velocity < 0.0 || seconds < m_render_seconds;
'''
new = '''            if (retarget) {
                double dt = kScrubGestureDefaultDt;
                if (previous_state == stem_transport_scrub &&
                    m_scrub_motion_tick != 0 && now > m_scrub_motion_tick) {
                    dt = std::clamp(
                        static_cast<double>(now - m_scrub_motion_tick) / 1000.0,
                        kScrubGestureMinDt, kScrubGestureMaxDt);
                }

                double measured =
                    dt > 0.0 ? (seconds - previous_position) / dt : 0.0;
                measured = std::clamp(
                    measured, -kScrubMaxSourceRate, kScrubMaxSourceRate);

                // Preserve quick direction changes. Only a small amount of
                // same-direction carry removes mouse timestamp jitter; unlike the
                // previous experiment, this is not a low-pass platter filter.
                if (previous_state == stem_transport_scrub &&
                    measured * m_scrub_velocity > 0.0) {
                    measured = 0.20 * m_scrub_velocity + 0.80 * measured;
                }

                m_scrub_velocity = measured;
                m_scrub_motion_tick = now;
                m_scrub_audible_until = now + kScrubAudibleSafetyMs;
            } else if (previous_state == stem_transport_scrub) {
                // Spectral sends one unchanged SCRUB target after the hand has
                // been motionless for its short gate. Treat that as a soft platter
                // stop: silence the next DSP block and snap the render cursor to
                // the sample under the hand, but DO NOT enter HOLD or seek foobar.
                // The old hard-HOLD seek could flush the short scratch gesture out
                // of the output queue before it ever reached the speakers.
                m_scrub_velocity = 0.0;
                m_scrub_audible_until = 0;
                m_scrub_motion_tick = now;
                m_render_seconds = seconds;
            }

            m_state = stem_transport_scrub;
            m_position_seconds = seconds;
            reverse = m_scrub_velocity < 0.0 || seconds < m_render_seconds;
'''
if old not in s:
    raise SystemExit('set_scrub motion block anchor not found')
s = s.replace(old, new, 1)

p.write_text(s, encoding='utf-8')
print('patched soft scrub idle')
