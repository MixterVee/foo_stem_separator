from pathlib import Path
import subprocess

path = Path("stem_dsp.cpp")
s = path.read_text(encoding="utf-8")

BASE = "f5d84e2399651c2b165a32bf63e39dce7bec5dea"
base = subprocess.check_output(
    ["git", "show", f"{BASE}:stem_dsp.cpp"], text=True, encoding="utf-8"
)

region_start_marker = "            auto write_silence = [&]() {\n"
reverse_marker = "            if (ts.state == stem_transport_reverse) {\n"
hold_marker = "            if (ts.state == stem_transport_hold) {\n"
scrub_marker = "            if (ts.state == stem_transport_scrub) {\n"

# Recover the known-good write_silence, write_preview and HOLD code from the
# parent source. Find SCRUB only after HOLD so we cannot accidentally match the
# debug counter inside either helper lambda.
base_start = base.find(region_start_marker)
if base_start < 0:
    raise SystemExit("base transport helper start not found")
base_hold = base.find(hold_marker, base_start)
if base_hold < 0:
    raise SystemExit("base HOLD block not found")
base_scrub = base.find(scrub_marker, base_hold + len(hold_marker))
if base_scrub < 0:
    raise SystemExit("base SCRUB state block not found")
base_prefix = base[base_start:base_scrub]

cur_start = s.find(region_start_marker)
if cur_start < 0:
    raise SystemExit("current transport helper start not found")
cur_reverse = s.find(reverse_marker, cur_start)
if cur_reverse < 0:
    raise SystemExit("current REVERSE state block not found")

new_scrub = r'''            if (ts.state == stem_transport_scrub) {
                // DJ-style platter engine: the hand supplies a target position and
                // velocity, while the audio thread owns one continuous virtual
                // stylus. Controller updates never become decoder seeks or
                // standalone chunks of mouse trajectory.
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

                const double error = ts.position_seconds - m_scrubRenderPosition;
                const double half_sample = 0.5 / static_cast<double>(rate);

                // A stopped record is silence, not a repeated sample. Keep the
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

                // Mixxx-style target servo: hand velocity is primary; bounded
                // position error correction prevents the virtual stylus drifting
                // away from the platter without turning that target into a seek.
                const double correction = std::clamp(
                    error / kDjScratchPhaseCorrectionSeconds,
                    -kDjScratchMaxCorrectionRate,
                    kDjScratchMaxCorrectionRate);
                const double requested_rate = std::clamp(
                    hand_rate + correction,
                    -kScrubMaxSourceRate,
                    kScrubMaxSourceRate);

                const double previous_rate = m_scrubPreviousRate;
                const bool reversal = previous_rate * requested_rate < 0.0;
                const bool strong_deceleration =
                    std::abs(requested_rate) + kDjScratchStrongDecel <
                    std::abs(previous_rate);

                // Low-pass ordinary acceleration, but do not smear a strong
                // slowdown. This mirrors Mixxx's no-filter hard deceleration
                // behavior so the audio stops with the hand instead of overshooting.
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

                std::vector<float> preview(frames * kCacheChannels, 0.0f);
                const size_t slice_frames = (std::max)(
                    static_cast<size_t>(1),
                    static_cast<size_t>(std::llround(
                        kScrubTrajectorySliceSeconds *
                        static_cast<double>(rate))));

                auto ramp_rate_at = [&](double x) -> double {
                    x = std::clamp(x, 0.0, 1.0);
                    if (reversal) {
                        // Mixxx's linear scratch scaler crosses through zero on
                        // a direction change rather than flipping sign abruptly.
                        if (x < 0.5) {
                            return previous_rate * (1.0 - 2.0 * x);
                        }
                        return next_rate * (2.0 * x - 1.0);
                    }
                    return previous_rate + (next_rate - previous_rate) * x;
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
                    const double local_rate = ramp_rate_at(midpoint);

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
                            // On a cache miss, do not advance the stylus. The
                            // decoder can catch up without causing a silence cascade.
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

s = s[:cur_start] + base_prefix + new_scrub + s[cur_reverse:]

# Structural guardrails before writing.
region = s[cur_start:s.find(reverse_marker, cur_start)]
checks = {
    "write_silence helper": "auto write_silence = [&]()" in region,
    "write_preview helper": "auto write_preview = [&](const std::vector<float>& rendered)" in region,
    "HOLD state": hold_marker.strip() in region,
    "DJ SCRUB state": "DJ-style platter engine" in region,
    "single SCRUB state after HOLD": region.count(scrub_marker.strip()) >= 3,  # 2 helper debug checks + state
    "persistent stylus member": "double m_scrubRenderPosition = 0.0;" in s,
    "Hermite interpolator": "scratch_hermite4" in s,
    "live readiness": "The live Original deque is already resident PCM" in s,
}
for name, ok in checks.items():
    if not ok:
        raise SystemExit(f"repair guard failed: {name}")

path.write_text(s, encoding="utf-8")
print("DJ scratch transport section repaired")
