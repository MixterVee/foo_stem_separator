$ErrorActionPreference = 'Stop'

$path = Join-Path $PSScriptRoot '..\stem_dsp.cpp'
$source = [System.IO.File]::ReadAllText($path)
$source = $source -replace "`r`n", "`n"

function Replace-Exact([string]$label, [string]$old, [string]$new) {
    $script:source = $script:source.Replace("`r`n", "`n")
    $old = $old.Replace("`r`n", "`n")
    $new = $new.Replace("`r`n", "`n")
    if (-not $script:source.Contains($old)) {
        throw "${label}: expected source block not found"
    }
    $script:source = $script:source.Replace($old, $new)
}

Replace-Exact 'Add latency reanchor threshold' @'
constexpr double kDjScratchCorrectionWindowSeconds = 0.125;
constexpr double kDjScratchMaxCorrectionRate = 0.35;
constexpr double kDjScratchRateFilter = 0.40;
'@ @'
constexpr double kDjScratchCorrectionWindowSeconds = 0.125;
constexpr double kDjScratchMaxCorrectionRate = 0.35;
// Foobar's queued output means roughly one configured output-buffer worth of
// hand/render separation is normal. Beyond this larger guard, however, the
// virtual stylus is genuinely stale and should re-anchor at the next DSP chunk
// instead of remaining seconds behind a fast hand movement.
constexpr double kDjScratchReanchorSeconds = 0.250;
constexpr double kDjScratchRateFilter = 0.40;
'@

Replace-Exact 'Latency-aware reanchor correction' @'
                // xwax-style target correction: hand velocity drives the PCM
                // reader. Correct only small accumulated drift, and do it slowly.
                // With foobar's output queue, a larger hand/render delta is normal
                // latency; chasing it would create the multi-second source jumps
                // and cache MISS bursts seen in the diagnostic recordings.
                double correction = 0.0;
                if (std::abs(error) <=
                    kDjScratchCorrectionWindowSeconds) {
                    correction = std::clamp(
                        error / kDjScratchSyncTimeSeconds,
                        -kDjScratchMaxCorrectionRate,
                        kDjScratchMaxCorrectionRate);
                }
                const double requested_rate = std::clamp(
                    hand_rate + correction,
                    -kScrubMaxSourceRate,
                    kScrubMaxSourceRate);

                const double previous_rate = m_scrubPreviousRate;
'@ @'
                // xwax-style target correction: hand velocity drives the PCM
                // reader. Correct only small accumulated drift, and do it slowly.
                // A much larger error means the DSP has fallen more than the
                // normal foobar output queue behind the controller. Re-anchor the
                // *virtual* stylus at this chunk boundary; this is not a foobar
                // seek and does not flush/cache-reset anything.
                const bool hard_reanchor =
                    std::abs(error) > kDjScratchReanchorSeconds;
                const double render_anchor = hard_reanchor
                    ? (std::max)(0.0, ts.position_seconds)
                    : m_scrubRenderPosition;

                double correction = 0.0;
                if (!hard_reanchor &&
                    std::abs(error) <=
                        kDjScratchCorrectionWindowSeconds) {
                    correction = std::clamp(
                        error / kDjScratchSyncTimeSeconds,
                        -kDjScratchMaxCorrectionRate,
                        kDjScratchMaxCorrectionRate);
                }
                const double requested_rate = std::clamp(
                    hand_rate + correction,
                    -kScrubMaxSourceRate,
                    kScrubMaxSourceRate);

                // Do not ramp from a stale old velocity after a hard re-anchor;
                // start this chunk at the currently measured platter rate.
                const double previous_rate = hard_reanchor
                    ? requested_rate
                    : m_scrubPreviousRate;
'@

Replace-Exact 'Request cache at latency anchor' @'
                cache_manager().request_transport(
                    m_scrubRenderPosition, next_rate < 0.0);
'@ @'
                cache_manager().request_transport(
                    render_anchor, next_rate < 0.0);
'@

Replace-Exact 'Render from latency anchor' @'
                double source_position = m_scrubRenderPosition;
'@ @'
                double source_position = render_anchor;
'@

[System.IO.File]::WriteAllText(
    $path,
    $source,
    [System.Text.UTF8Encoding]::new($false))

Write-Host 'Applied latency-aware platter reanchor patch.'
