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

Replace-Exact 'Live stem cache window' @'
constexpr double kCacheSeconds = 20.0;
constexpr double kCacheOverlapSeconds = 3.0;
constexpr double kPrefetchSeconds = 20.0;
'@ @'
// Mouse scratching no longer needs long stem transport blocks. Publish small
// live stem blocks quickly; this was the known-stable live configuration before
// the scratch-cache experiments. Spleeter produces both stems in one pass, so
// every completed block makes Vocals and Instrumental immediately switchable.
constexpr double kCacheSeconds = 4.0;
constexpr double kCacheOverlapSeconds = 1.0;
constexpr double kPrefetchSeconds = 20.0;
'@

Replace-Exact 'Proactive track-start stem cache' @'
        if (!m_path.empty()) {
            const stemmode::mode mode = stemmode::get();
            if (mode != stemmode::mode::original) {
                queue_job_locked(0.0, true);
            } else {
                // Pre-decode the first Original transport window immediately.
                // Unlike stem caching this is just Media Foundation decode and is
                // cheap enough to keep ready for platter work all the time.
                m_jobs.emplace_back(cache_job{
                    m_generation, m_path, 0.0, true, false, false});
                m_job_pending = true;
            }
        }
'@ @'
        if (!m_path.empty()) {
            const stemmode::mode mode = stemmode::get();
            const bool warm_stems =
                mode != stemmode::mode::original || stem_precache::enabled();
            if (warm_stems) {
                // Pre-cache now means PRE-cache: start Spleeter as soon as a new
                // track begins even while Original is selected. The generated
                // segment contains Original + Vocals + Instrumental, so no
                // separate Original decoder job is needed for this region.
                queue_job_locked(0.0, true);
            } else {
                // Pre-cache was explicitly disabled: retain the cheap Original
                // decoder-only behavior and leave ONNX lazy.
                m_jobs.emplace_back(cache_job{
                    m_generation, m_path, 0.0, true, false, false});
                m_job_pending = true;
            }
        }
'@

Replace-Exact 'Continuous background stem precache' @'
    void ensure_ahead(double playback_seconds) {
        const stemmode::mode mode = stemmode::get();

        std::lock_guard<std::mutex> lock(
            m_mutex);
'@ @'
    void ensure_ahead(double playback_seconds) {
        const stemmode::mode selected_mode = stemmode::get();
        // When pre-cache is enabled, keep generating stem blocks ahead even if
        // the listener is currently hearing Original. A vocals probe is enough:
        // each Spleeter job always publishes both Vocals and Instrumental.
        const stemmode::mode mode =
            selected_mode == stemmode::mode::original && stem_precache::enabled()
                ? stemmode::mode::vocals
                : selected_mode;

        std::lock_guard<std::mutex> lock(
            m_mutex);
'@

Replace-Exact 'Warm ONNX engine in cache worker' @'
            // Original platter PCM does not need ONNX at all. Construct the
            // separation engine lazily so cheap Original prefetch can begin as
            // soon as Media Foundation is ready instead of waiting for model
            // initialization first. Construction still stays inside the worker
            // exception boundary and is additionally covered by the per-job try.
            std::unique_ptr<onnxstem::engine> engine;

            sequential_decoder_state decoder_state;
'@ @'
            // Pre-cache is enabled by default. Warm the ONNX/Spleeter session on
            // the cache worker immediately so the first user stem request does not
            // also pay DLL/model/session creation. This never blocks foobar's UI or
            // ordinary Original playback. If pre-cache is disabled, preserve lazy
            // initialization exactly as before.
            std::unique_ptr<onnxstem::engine> engine;
            if (stem_precache::enabled()) {
                engine = std::make_unique<onnxstem::engine>();
                engine->ready();
            }

            sequential_decoder_state decoder_state;
'@

[System.IO.File]::WriteAllText(
    $path,
    $source,
    [System.Text.UTF8Encoding]::new($false))

Write-Host 'Applied proactive fast stem-start patch.'
