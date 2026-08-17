$ErrorActionPreference = 'Stop'

$path = Join-Path $PSScriptRoot '..\stem_dsp.cpp'
$source = [System.IO.File]::ReadAllText($path)
$source = $source -replace "`r`n", "`n"

function Replace-Exact([string]$label, [string]$old, [string]$new) {
    $script:source = $script:source.Replace("`r`n", "`n")
    $old = $old.Replace("`r`n", "`n")
    $new = $new.Replace("`r`n", "`n")
    if (-not $script:source.Contains($old)) {
        throw "$label: expected source block not found"
    }
    $script:source = $script:source.Replace($old, $new)
}

Replace-Exact 'Original quick-cache size' @'
constexpr double kOriginalQuickCacheSeconds = 1.5;
constexpr double kOriginalQuickOverlapSeconds = 0.25;
'@ @'
// A platter can reverse without warning, so a transport preview must cover
// both sides of the stylus rather than only the current direction. Three
// seconds gives the existing 1.25-second safety margin on each side plus
// interpolation/decoder-edge headroom.
constexpr double kOriginalQuickCacheSeconds = 3.0;
constexpr double kOriginalQuickOverlapSeconds = 0.50;
'@

Replace-Exact 'DJ scratch constants' @'
constexpr double kDjScratchPhaseCorrectionSeconds = 0.050;
constexpr double kDjScratchMaxCorrectionRate = 6.0;
constexpr double kDjScratchRateFilter = 0.40;
'@ @'
// xwax-style virtual platter control: signed hand velocity is the primary
// transport. Absolute position only removes small accumulated drift over a
// relatively slow sync interval. A large hand/render delta is normal while
// foobar has queued output and must not become a multi-x corrective speed jump.
constexpr double kDjScratchSyncTimeSeconds = 0.500;
constexpr double kDjScratchCorrectionWindowSeconds = 0.125;
constexpr double kDjScratchMaxCorrectionRate = 0.35;
constexpr double kDjScratchRateFilter = 0.40;
'@

Replace-Exact 'Bidirectional Original transport cache' @'
        // Require a little playable material on the side in which transport will
        // travel. If it is already cached, no worker job is necessary.
        const double margin = 1.25;
        const double need_start = reverse
            ? (std::max)(0.0, position_seconds - margin)
            : position_seconds;
        const double need_end = reverse
            ? position_seconds
            : position_seconds + margin;

        if (range_ready_locked(mode, need_start, need_end)) return;

        // Coalesce scrub requests: a slow Spleeter job must never build a queue of
        // obsolete mouse positions. The newest transport target goes to the front.
        for (auto it = m_jobs.begin(); it != m_jobs.end();) {
            if (it->transport_preview) it = m_jobs.erase(it);
            else ++it;
        }

        const bool original_preview = mode == stemmode::mode::original;
        const double transport_window = original_preview
            ? kOriginalQuickCacheSeconds
            : kCacheSeconds;
        const double edge_preroll = original_preview ? 0.25 : 0.5;

        double start = reverse
            ? (std::max)(0.0, position_seconds -
                (transport_window - edge_preroll))
            : (std::max)(0.0, position_seconds - edge_preroll);
'@ @'
        // Original scratching is intrinsically bidirectional: the next mouse
        // event may reverse instantly. Require PCM on both sides of the stylus.
        // Separated-stem previews retain their directional policy because those
        // jobs are much more expensive than decoder-only Original previews.
        const bool original_preview = mode == stemmode::mode::original;
        const double margin = 1.25;
        const double need_start = original_preview
            ? (std::max)(0.0, position_seconds - margin)
            : (reverse
                ? (std::max)(0.0, position_seconds - margin)
                : position_seconds);
        const double need_end = original_preview
            ? position_seconds + margin
            : (reverse
                ? position_seconds
                : position_seconds + margin);

        if (range_ready_locked(mode, need_start, need_end)) return;

        // Coalesce scrub requests: a slow Spleeter job must never build a queue of
        // obsolete mouse positions. The newest transport target goes to the front.
        for (auto it = m_jobs.begin(); it != m_jobs.end();) {
            if (it->transport_preview) it = m_jobs.erase(it);
            else ++it;
        }

        const double transport_window = original_preview
            ? kOriginalQuickCacheSeconds
            : kCacheSeconds;
        const double edge_preroll = original_preview ? 0.25 : 0.5;

        // Center cheap Original PCM around the stylus so a reversal never needs
        // a decoder retarget first. Stem previews keep their old directional
        // placement to avoid unnecessary Spleeter work.
        double start = original_preview
            ? (std::max)(0.0, position_seconds - transport_window * 0.5)
            : (reverse
                ? (std::max)(0.0, position_seconds -
                    (transport_window - edge_preroll))
                : (std::max)(0.0, position_seconds - edge_preroll));
'@

Replace-Exact 'Gentle platter drift correction' @'
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
'@ @'
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
'@

[System.IO.File]::WriteAllText(
    $path,
    $source,
    [System.Text.UTF8Encoding]::new($false))

Write-Host 'Applied xwax-style virtual platter scratch patch.'
