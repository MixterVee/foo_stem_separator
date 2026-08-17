from pathlib import Path

path = Path("stem_dsp.cpp")
s = path.read_text(encoding="utf-8")

old = '''                // A stopped record is silence, not a repeated sample. Keep the
                // stylus exactly under the hand without advancing the source.
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
'''

new = '''                // Once real mouse motion has stopped, the platter is physically
                // stationary. Snap the virtual stylus to the held hand position and
                // silence it immediately instead of allowing the phase-correction
                // servo to keep crawling through/repeating audio. write_silence()
                // already applies the short transport-tail de-click fade.
                const bool no_recent_motion =
                    motion_age > kDjScratchMotionGraceSeconds;

                if (no_recent_motion ||
                    (std::abs(hand_rate) <= kDjScratchStoppedRate &&
                     std::abs(error) <= half_sample)) {
                    m_scrubRenderPosition = (std::max)(0.0, ts.position_seconds);
                    m_scrubPreviousRate = 0.0;
                    g_dbg_last_source_rate.store(
                        0.0, std::memory_order_relaxed);
                    transport().complete_scrub(m_scrubRenderPosition);
                    write_silence();
                    m_position_seconds += chunk_seconds;
                    m_using_stem = false;
                    return true;
                }
'''

if old not in s:
    raise SystemExit("stationary hold target block not found or already changed")

s = s.replace(old, new, 1)

# Guardrails: keep the DJ renderer intact while ensuring the new stop override
# sits before the phase-correction servo.
if s.count("const bool no_recent_motion") != 1:
    raise SystemExit("stationary hold guard failed: marker count")
if s.find("const bool no_recent_motion") > s.find("const double correction = std::clamp", s.find("const bool no_recent_motion")):
    raise SystemExit("stationary hold guard failed: stop override ordering")
if "DJ-style platter engine" not in s or "scratch_hermite4" not in s:
    raise SystemExit("stationary hold guard failed: scratch engine missing")

path.write_text(s, encoding="utf-8")
print("Stationary scratch hold fix applied")
