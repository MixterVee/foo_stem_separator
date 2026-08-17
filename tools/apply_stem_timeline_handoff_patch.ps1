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

Replace-Exact 'Ignore Spectral tiles for internal pre-cache horizon' @'
        double cache_end = -1.0;

        if (!m_segments.empty()) {
            cache_end =
                m_segments.back()->end_seconds;
        }

        // No cache at all: begin at the current playback position.
        if (cache_end < 0.0) {
            queue_job_locked(
                playback_seconds);

            m_cv.notify_one();
            return;
        }
'@ @'
        // Determine how far the Stem worker's OWN sequential cache reaches from
        // the current playhead. Spectral Waveform also publishes separated PCM
        // tiles into m_segments, but those independently-seeked tiles must not
        // convince the live pre-cache worker that its continuous timeline is
        // already filled. Otherwise a far-ahead waveform tile can skip several
        // internal 4-second blocks and ordinary playback later falls back to a
        // differently anchored source at the exact moment a stem is selected.
        double cache_end = playback_seconds;
        bool have_internal_coverage = false;
        for (;;) {
            double furthest = cache_end;
            for (const auto& seg_ptr : m_segments) {
                const cache_segment& seg = *seg_ptr;
                if (seg.external_waveform || !segment_has_mode(seg, mode)) continue;
                if (seg.start_seconds <= cache_end + 1.0e-6 &&
                    seg.end_seconds > furthest) {
                    furthest = seg.end_seconds;
                }
            }
            if (furthest <= cache_end + 1.0e-6) break;
            have_internal_coverage = true;
            cache_end = furthest;
        }

        // No internal cache at the playhead: begin there even if Spectral has an
        // external waveform tile covering the same time.
        if (!have_internal_coverage) {
            queue_job_locked(
                playback_seconds);

            m_cv.notify_one();
            return;
        }
'@

Replace-Exact 'Prefer continuous internal cache over Spectral transport PCM' @'
            // Prefer Spectral Waveform PCM. Widened contextual publishes overlap
            // adjacent 5-second tiles, so retain the two time-adjacent external
            // segments when both cover this sample and crossfade their handoff
            // below. This avoids both duplicate inference and hard tile seams.
            for (const auto& seg_ptr : snapshot) {
                const cache_segment& seg = *seg_ptr;
                if (!seg.external_waveform || !segment_has_mode(seg, mode)) continue;
                if (t < seg.start_seconds || t >= seg.end_seconds) continue;

                if (!first || seg.start_seconds < first->start_seconds) {
                    second = first;
                    first = &seg;
                } else if (!second || seg.start_seconds < second->start_seconds) {
                    second = &seg;
                }
            }

            if (!first) {
                for (const auto& seg_ptr : snapshot) {
                const cache_segment& seg = *seg_ptr;
                    if (seg.external_waveform || !segment_has_mode(seg, mode)) continue;
                    if (t >= seg.start_seconds && t < seg.end_seconds) {
                        if (!first) first = &seg;
                        else { second = &seg; break; }
                    }
                }
            }
'@ @'
            // Ordinary audible stem playback is sample-locked to the Stem
            // worker's sequential decoder timeline. Spectral Waveform PCM comes
            // from independently sought contextual analysis tiles; it remains a
            // useful fallback for a not-yet-built position, but it must never
            // override an internal block that already covers this sample.
            for (const auto& seg_ptr : snapshot) {
                const cache_segment& seg = *seg_ptr;
                if (seg.external_waveform || !segment_has_mode(seg, mode)) continue;
                if (t < seg.start_seconds || t >= seg.end_seconds) continue;

                if (!first || seg.start_seconds < first->start_seconds) {
                    second = first;
                    first = &seg;
                } else if (!second || seg.start_seconds < second->start_seconds) {
                    second = &seg;
                }
            }

            if (!first) {
                // External Spectral PCM is fallback-only. If two contextual tiles
                // overlap, retain both so the existing handoff crossfade still
                // protects the fallback path from a hard tile seam.
                for (const auto& seg_ptr : snapshot) {
                    const cache_segment& seg = *seg_ptr;
                    if (!seg.external_waveform || !segment_has_mode(seg, mode)) continue;
                    if (t < seg.start_seconds || t >= seg.end_seconds) continue;

                    if (!first || seg.start_seconds < first->start_seconds) {
                        second = first;
                        first = &seg;
                    } else if (!second || seg.start_seconds < second->start_seconds) {
                        second = &seg;
                    }
                }
            }
'@

[System.IO.File]::WriteAllText(
    $path,
    $source,
    [System.Text.UTF8Encoding]::new($false))

Write-Host 'Applied stem timeline handoff patch.'
