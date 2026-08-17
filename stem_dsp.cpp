#include <foobar2000/SDK/foobar2000.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <memory>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

#include "onnx_stem_engine.h"
#include "stem_mode.h"
#include "stem_transport_service.h"

namespace stem_precache {
bool enabled();
}

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")

using Microsoft::WRL::ComPtr;

#undef FOOGUIDDECL
#define FOOGUIDDECL
FOOGUIDDECL const GUID stem_transport_service::class_guid =
{ 0x3f42b0c7, 0x8df1, 0x4fb9, { 0xa6, 0x7d, 0x21, 0x55, 0x91, 0xc8, 0x43, 0x6e } };

namespace {

constexpr unsigned kCacheRate = 44100;
constexpr unsigned kCacheChannels = 2;

constexpr double kCacheSeconds = 20.0;
constexpr double kCacheOverlapSeconds = 3.0;
constexpr double kPrefetchSeconds = 20.0;
// Original PCM is cheap to decode, so keep a smaller trigger margin around the
// playhead continuously. This makes the very first platter grab audible instead
// of waiting for a transport request after the mouse is already down.
// Original transport does not need a 20-second analysis block. Publish a
// compact decoder-only window quickly, then keep extending it in the background.
// Keep random Original platter requests tiny and quick. A 1.5-second window
// exactly covers the transport renderer's current 1.25-second directional
// safety margin plus a small overlap, so the worker publishes usable PCM
// without decoding a long block first.
constexpr double kOriginalQuickCacheSeconds = 1.5;
constexpr double kOriginalQuickOverlapSeconds = 0.25;
// Ordinary Original decoding is cheap and must stay well ahead of a platter.
// Keep quick random transport jobs tiny, but let the separate sequential
// background decoder publish a much larger future window that rapid forward
// scratches can read immediately.
constexpr double kOriginalBackgroundCacheSeconds = 30.0;
constexpr double kOriginalBackgroundOverlapSeconds = 3.0;
constexpr double kOriginalBackgroundPrefetchSeconds = 12.0;
constexpr double kSwitchFadeSeconds = 0.050;
constexpr double kCacheHandoffFadeSeconds = 0.080;
constexpr double kDecodeSeekPrerollSeconds = 5.0;
// Stem analysis keeps the conservative 5-second preroll used by the stable
// VBR path. Original scratch PCM only needs timestamp-accurate decoder output,
// so use a short preroll to avoid decoding/discarding five seconds on every
// random hand movement.
constexpr double kOriginalDecodeSeekPrerollSeconds = 0.50;
// Keep already-decoded Original PCM in RAM for platter work. At 44.1 kHz stereo
// float, 45 seconds is only about 15 MB and removes decoder-seek latency from
// normal scratches around the current playhead.
constexpr double kOriginalRollingSeconds = 45.0;
constexpr uint64_t kOriginalRollingJoinToleranceFrames = 8;
constexpr double kFirstBlockFadeSeconds = 0.005;
// Spectral Waveform now explicitly returns scrub transport to HOLD after real
// mouse motion stops. Keep this slightly longer timeout as a safety net only.
constexpr ULONGLONG kScrubAudibleSafetyMs = 320;
constexpr double kScrubKeepaliveToleranceSeconds = 0.002;
constexpr double kScrubSubBlockSeconds = 0.004;
constexpr double kScrubCarrySlopeLimit = 2.0;
constexpr double kScrubGestureDefaultDt = 0.016;
constexpr double kScrubGestureMinDt = 0.001;
constexpr double kScrubGestureMaxDt = 0.250;
constexpr double kScrubMaxSourceRate = 24.0;
// Render a short, fixed-delay window of the real mouse path. This decouples
// scratch audio from arbitrary foobar DSP block boundaries and averages away
// 1-2 ms Windows mouse-message timing spikes without adding a perceptible delay.
constexpr double kScrubTrajectoryLagSeconds = 0.010;
constexpr double kScrubTrajectorySliceSeconds = 0.008;
constexpr double kScrubTrajectoryExtrapolateSeconds = 0.012;
constexpr double kScrubTrajectoryHistorySeconds = 0.750;
// Pitch follows measured hand velocity. Keep only a very short audio-side ramp
// for click-free acceleration, and sample-lock the virtual stylus whenever its
// integrated cursor drifts materially away from the actual hand position.
constexpr double kScrubRateRampSeconds = 0.006;
constexpr double kScrubMaxCursorErrorSeconds = 0.060;
constexpr double kScrubReversalReanchorErrorSeconds = 0.015;

std::atomic<uint64_t> g_dbg_render_attempts{0};
std::atomic<uint64_t> g_dbg_render_successes{0};
std::atomic<uint64_t> g_dbg_live_hits{0};
std::atomic<uint64_t> g_dbg_cache_hits{0};
std::atomic<uint64_t> g_dbg_render_misses{0};
std::atomic<uint64_t> g_dbg_scrub_audio_writes{0};
std::atomic<uint64_t> g_dbg_scrub_silence_writes{0};
std::atomic<int> g_dbg_last_render_source{stem_debug_source_none};
std::atomic<int> g_dbg_last_render_ok{0};
std::atomic<double> g_dbg_last_render_start{0.0};
std::atomic<double> g_dbg_last_source_rate{0.0};

void reset_scratch_debug() {
    g_dbg_render_attempts.store(0, std::memory_order_relaxed);
    g_dbg_render_successes.store(0, std::memory_order_relaxed);
    g_dbg_live_hits.store(0, std::memory_order_relaxed);
    g_dbg_cache_hits.store(0, std::memory_order_relaxed);
    g_dbg_render_misses.store(0, std::memory_order_relaxed);
    g_dbg_scrub_audio_writes.store(0, std::memory_order_relaxed);
    g_dbg_scrub_silence_writes.store(0, std::memory_order_relaxed);
    g_dbg_last_render_source.store(stem_debug_source_none, std::memory_order_relaxed);
    g_dbg_last_render_ok.store(0, std::memory_order_relaxed);
    g_dbg_last_render_start.store(0.0, std::memory_order_relaxed);
    g_dbg_last_source_rate.store(0.0, std::memory_order_relaxed);
}

std::wstring utf8_to_wide_cache(const char* s) {
    if (!s || !*s) return {};

    const int n = MultiByteToWideChar(
        CP_UTF8, 0, s, -1, nullptr, 0);

    if (n <= 1) return {};

    std::vector<wchar_t> temp(
        static_cast<size_t>(n));

    MultiByteToWideChar(
        CP_UTF8,
        0,
        s,
        -1,
        temp.data(),
        n);

    return std::wstring(temp.data());
}

std::wstring local_path_from_metadb(
    metadb_handle_ptr handle) {

    if (handle.is_empty()) return {};

    std::wstring path =
        utf8_to_wide_cache(
            handle->get_path());

    const std::wstring prefix =
        L"file://";

    if (path.rfind(prefix, 0) == 0) {
        path.erase(0, prefix.size());
    }

    return path;
}

std::wstring local_path_from_utf8_cache(const char* raw) {
    std::wstring path = utf8_to_wide_cache(raw);
    const std::wstring prefix = L"file://";
    if (path.rfind(prefix, 0) == 0) path.erase(0, prefix.size());
    return path;
}

bool convert_to_cache_stereo(
    const float* input,
    size_t frames,
    unsigned channels,
    unsigned sample_rate,
    std::vector<float>& out,
    size_t exact_output_frames = 0) {

    out.clear();
    if (input == nullptr || frames == 0 || channels == 0 || sample_rate == 0) return false;

    size_t output_frames = exact_output_frames;
    if (output_frames == 0) {
        output_frames = frames;
        if (sample_rate != kCacheRate) {
            output_frames = (std::max)(
                static_cast<size_t>(1),
                static_cast<size_t>(std::llround(
                    static_cast<double>(frames) *
                    static_cast<double>(kCacheRate) /
                    static_cast<double>(sample_rate))));
        }
    }

    out.assign(output_frames * kCacheChannels, 0.0f);

    auto read_channel = [input, channels, frames](size_t frame, unsigned ch) -> float {
        frame = (std::min)(frame, frames - 1);
        const float* src = input + frame * channels;
        if (channels == 1) return src[0];
        return src[ch == 0 ? 0 : 1];
    };

    if (frames == 1 || output_frames == 1) {
        const float l = read_channel(0, 0);
        const float r = read_channel(0, 1);
        for (size_t i = 0; i < output_frames; ++i) {
            out[i * 2] = l;
            out[i * 2 + 1] = r;
        }
        return true;
    }

    const double scale = static_cast<double>(frames - 1) /
        static_cast<double>(output_frames - 1);

    for (size_t i = 0; i < output_frames; ++i) {
        const double source = static_cast<double>(i) * scale;
        const size_t i0 = static_cast<size_t>(source);
        const size_t i1 = (std::min)(i0 + 1, frames - 1);
        const float frac = static_cast<float>(source - static_cast<double>(i0));
        for (unsigned ch = 0; ch < 2; ++ch) {
            const float a = read_channel(i0, ch);
            const float b = read_channel(i1, ch);
            out[i * 2 + ch] = a + (b - a) * frac;
        }
    }
    return true;
}

struct cache_segment {
    uint64_t generation = 0;
    double start_seconds = 0.0;
    double end_seconds = 0.0;

    // Original is optional for Spectral Waveform-published blocks. Normal
    // playback has the source chunk already, while Original transport can be
    // decoded cheaply without invoking Spleeter.
    std::vector<float> original;
    std::vector<float> vocals;
    std::vector<float> instrumental;

    // True when this PCM was produced by Spectral Waveform's contextual stem
    // analysis. These blocks are preferred for jog/reverse so we never pay a
    // second Spleeter pass for an already processed visible region.
    bool external_waveform = false;
};

struct cache_job {
    uint64_t generation = 0;
    std::wstring path;
    double start_seconds = 0.0;
    bool force_reanchor = false;
    bool transport_preview = false;
    bool need_stems = true;
};

struct sequential_decoder_state {
    ComPtr<IMFSourceReader> reader;
    std::wstring path;

    bool valid = false;

    // Absolute PCM frame index represented by fifo[0].
    uint64_t fifo_start_frame = 0;

    // Absolute frame index where the decoder will append next.
    uint64_t decoded_end_frame = 0;

    // Interleaved stereo float PCM.
    std::vector<float> fifo;

    void reset() {
        reader.Reset();
        path.clear();
        valid = false;
        fifo_start_frame = 0;
        decoded_end_frame = 0;
        fifo.clear();
    }
};


class live_cache_manager {
public:
    live_cache_manager() {
        m_thread =
            std::thread(
                [this]() { worker_main(); });
    }

    ~live_cache_manager() {
        {
            std::lock_guard<std::mutex> lock(
                m_mutex);

            m_stop = true;
        }

        m_cv.notify_all();
        m_ready_cv.notify_all();

        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    uint64_t generation() const {
        std::lock_guard<std::mutex> lock(
            m_mutex);

        return m_generation;
    }

    double anchor_time() const {
        std::lock_guard<std::mutex> lock(
            m_mutex);

        return m_anchor_seconds;
    }

    bool debug_live_range(double& start_seconds, double& end_seconds) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_live_original.empty()) {
            start_seconds = -1.0;
            end_seconds = -1.0;
            return false;
        }
        const uint64_t frames = static_cast<uint64_t>(
            m_live_original.size() / kCacheChannels);
        start_seconds = static_cast<double>(m_live_original_start_frame) /
            static_cast<double>(kCacheRate);
        end_seconds = start_seconds + static_cast<double>(frames) /
            static_cast<double>(kCacheRate);
        return true;
    }

    bool is_track_start_generation(
        uint64_t generation) const {

        std::lock_guard<std::mutex> lock(
            m_mutex);

        return
            generation != 0 &&
            generation ==
                m_track_start_generation;
    }

    bool wait_until_ready(
        uint64_t generation,
        double position_seconds,
        unsigned timeout_ms) {

        std::unique_lock<std::mutex> lock(
            m_mutex);

        const auto ready =
            [this,
             generation,
             position_seconds]() {

                if (m_stop ||
                    generation !=
                        m_generation) {
                    return true;
                }

                for (const auto& seg_ptr : m_segments) {
                const cache_segment& seg = *seg_ptr;

                    if (position_seconds >=
                            seg.start_seconds &&
                        position_seconds <
                            seg.end_seconds) {
                        return true;
                    }
                }

                return false;
            };

        if (!m_ready_cv.wait_for(
                lock,
                std::chrono::milliseconds(
                    timeout_ms),
                ready)) {
            return false;
        }

        if (m_stop ||
            generation !=
                m_generation) {
            return false;
        }

        for (const auto& seg_ptr : m_segments) {
                const cache_segment& seg = *seg_ptr;

            if (position_seconds >=
                    seg.start_seconds &&
                position_seconds <
                    seg.end_seconds) {
                return true;
            }
        }

        return false;
    }

    void new_track(
        const std::wstring& path) {

        reset_scratch_debug();

        std::lock_guard<std::mutex> lock(
            m_mutex);

        ++m_generation;

        m_track_start_generation =
            m_generation;

        m_path = path;
        m_anchor_seconds = 0.0;

        m_segments.clear();
        m_live_original.clear();
        m_live_original_start_frame = 0;
        m_jobs.clear();
        m_job_pending = false;

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

        m_cv.notify_one();
    }

    void seek(double seconds) {
        if (seconds < 0.0) {
            seconds = 0.0;
        }

        std::lock_guard<std::mutex> lock(
            m_mutex);

        ++m_generation;

        // A seek keeps V24's immediate-playback behavior.
        // Start pre-cache applies only to a newly started track.
        m_track_start_generation = 0;

        m_anchor_seconds = seconds;

        // Keep completed segments from this track. They are position-indexed
        // and remain valid after a seek; retaining them is what lets a transport
        // release resume directly in Vocals/Instrumental without leaking Original.
        m_jobs.clear();
        m_job_pending = false;

        if (!m_path.empty()) {
            const stemmode::mode mode = stemmode::get();

            if (mode != stemmode::mode::original) {
                queue_job_locked(seconds, true);
            } else {
                // Spectral Waveform arms HOLD and then seeks to the same sample
                // to flush queued output. That seek used to clear the Original
                // transport request made by set_hold(), leaving SCRUB with no PCM
                // until the mouse had already moved. Re-prime a cheap decoder-only
                // transport window here. No Spleeter inference is involved.
                // First publish a compact transport window around the grab point.
                // Then, on the independent sequential decoder timeline, fill a
                // large future region. Rapid scrub retargeting may coalesce the
                // transport-preview jobs but never cancels this background job.
                const double preview_start = (std::max)(
                    0.0, seconds - kOriginalQuickCacheSeconds * 0.5);
                m_jobs.emplace_front(cache_job{
                    m_generation, m_path, preview_start, true, true, false});

                const double background_start = (std::max)(
                    0.0, seconds - kOriginalBackgroundOverlapSeconds);
                m_jobs.emplace_back(cache_job{
                    m_generation, m_path, background_start, true, false, false});
                m_job_pending = true;
            }
        }

        m_cv.notify_one();
    }

    void transport_flush_seek(double seconds) {
        if (seconds < 0.0) seconds = 0.0;
        std::lock_guard<std::mutex> lock(m_mutex);
        // Spectral Waveform uses a seek to the already-armed HOLD/REVERSE/RELEASE
        // position only to flush queued output. The track and all position-indexed
        // PCM remain valid, so preserve generation, jobs, rolling history and the
        // sequential ahead decoder. flush() will make the DSP pick up this anchor.
        m_anchor_seconds = seconds;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(
            m_mutex);

        ++m_generation;
        m_track_start_generation = 0;

        m_path.clear();
        m_anchor_seconds = 0.0;

        m_segments.clear();
        m_live_original.clear();
        m_live_original_start_frame = 0;
        m_jobs.clear();
        m_job_pending = false;
    }

    void ensure_ahead(double playback_seconds) {
        const stemmode::mode mode = stemmode::get();

        std::lock_guard<std::mutex> lock(
            m_mutex);

        if (m_path.empty() ||
            m_job_pending) {
            return;
        }

        if (mode == stemmode::mode::original) {
            // Find Original PCM that actually covers the playhead. Completed
            // windows are retained, so this also gives scratch useful history.
            double covering_end = -1.0;
            for (const auto& seg_ptr : m_segments) {
                const cache_segment& seg = *seg_ptr;
                if (!segment_has_mode(seg, mode)) continue;
                if (playback_seconds >= seg.start_seconds - 1.0e-6 &&
                    playback_seconds < seg.end_seconds) {
                    covering_end = (std::max)(covering_end, seg.end_seconds);
                }
            }

            double next = 0.0;
            if (covering_end < 0.0) {
                // A seek/jump landed outside cached Original PCM. Start a large
                // background window slightly behind the playhead so the platter
                // immediately gains both history and substantial future material.
                next = (std::max)(
                    0.0, playback_seconds - kOriginalBackgroundOverlapSeconds);
            } else if (covering_end - playback_seconds <=
                       kOriginalBackgroundPrefetchSeconds) {
                // Extend the sequential future cache long before the platter can
                // reach its edge. This job is decoder-only and normally finishes
                // far faster than real-time playback.
                next = (std::max)(
                    0.0, covering_end - kOriginalBackgroundOverlapSeconds);
            } else {
                return;
            }

            m_jobs.emplace_back(cache_job{
                m_generation, m_path, next, false, false, false});
            m_job_pending = true;
            m_cv.notify_one();
            return;
        }

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

        if (cache_end -
                playback_seconds <=
            kPrefetchSeconds) {

            double next =
                cache_end -
                kCacheOverlapSeconds;

            if (next < playback_seconds) {
                next = playback_seconds;
            }

            queue_job_locked(next);
            m_cv.notify_one();
        }
    }

    void publish_live_original(
        double start_seconds,
        const audio_sample* input,
        size_t frames,
        unsigned channels,
        unsigned sample_rate) {

        if (input == nullptr || frames == 0 || channels == 0 || sample_rate == 0) return;
        if (start_seconds < 0.0) start_seconds = 0.0;

        // audio_sample is foobar's decoded float PCM. Convert/resample outside the
        // cache lock so the realtime callback only holds the mutex while appending.
        std::vector<float> source(frames * channels);
        for (size_t i = 0; i < source.size(); ++i) {
            source[i] = static_cast<float>(input[i]);
        }

        const uint64_t new_start_frame = static_cast<uint64_t>(
            start_seconds * static_cast<double>(kCacheRate) + 0.5);
        const double source_duration =
            static_cast<double>(frames) / static_cast<double>(sample_rate);
        const uint64_t new_end_frame = static_cast<uint64_t>(
            (start_seconds + source_duration) *
                static_cast<double>(kCacheRate) + 0.5);
        const size_t exact_cache_frames = static_cast<size_t>((std::max)(
            static_cast<uint64_t>(1),
            new_end_frame > new_start_frame
                ? new_end_frame - new_start_frame
                : static_cast<uint64_t>(1)));

        std::vector<float> cache_pcm;
        if (!convert_to_cache_stereo(
                source.data(), frames, channels, sample_rate, cache_pcm,
                exact_cache_frames) ||
            cache_pcm.empty()) {
            return;
        }

        const uint64_t new_frames = static_cast<uint64_t>(
            cache_pcm.size() / kCacheChannels);
        if (new_frames == 0) return;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_path.empty()) return;

        if (m_live_original.empty()) {
            m_live_original_start_frame = new_start_frame;
        } else {
            const uint64_t live_frames = static_cast<uint64_t>(
                m_live_original.size() / kCacheChannels);
            const uint64_t live_end_frame = m_live_original_start_frame + live_frames;

            // A far seek starts a new rolling region. A HOLD self-seek or ordinary
            // overlapping callback stays inside the existing region and preserves
            // the already-heard history behind the platter.
            if (new_start_frame > live_end_frame + kOriginalRollingJoinToleranceFrames ||
                new_start_frame + new_frames + kOriginalRollingJoinToleranceFrames <
                    m_live_original_start_frame) {
                m_live_original.clear();
                m_live_original_start_frame = new_start_frame;
            }
        }

        uint64_t live_frames = static_cast<uint64_t>(
            m_live_original.size() / kCacheChannels);
        uint64_t live_end_frame = m_live_original_start_frame + live_frames;

        // If the new callback begins a few rounding frames after our end, align it
        // rather than inserting silence. Larger gaps were handled as a new region.
        uint64_t effective_start = new_start_frame;
        if (effective_start > live_end_frame &&
            effective_start <= live_end_frame + kOriginalRollingJoinToleranceFrames) {
            effective_start = live_end_frame;
        }

        size_t skip_frames = 0;
        if (effective_start < live_end_frame) {
            const uint64_t overlap = live_end_frame - effective_start;
            if (overlap >= new_frames) {
                return;
            }
            skip_frames = static_cast<size_t>(overlap);
        }

        const size_t first_value = skip_frames * kCacheChannels;
        for (size_t i = first_value; i < cache_pcm.size(); ++i) {
            m_live_original.push_back(cache_pcm[i]);
        }

        const uint64_t max_frames = static_cast<uint64_t>(
            kOriginalRollingSeconds * static_cast<double>(kCacheRate) + 0.5);
        live_frames = static_cast<uint64_t>(m_live_original.size() / kCacheChannels);
        if (live_frames > max_frames) {
            const uint64_t drop_frames = live_frames - max_frames;
            const size_t drop_values = static_cast<size_t>(
                drop_frames * kCacheChannels);
            for (size_t i = 0; i < drop_values; ++i) {
                m_live_original.pop_front();
            }
            m_live_original_start_frame += drop_frames;
        }
    }

    bool publish_external_segment(
        const std::wstring& path,
        double start_seconds,
        const float* original,
        const float* vocals,
        const float* instrumental,
        size_t frames,
        unsigned channels,
        unsigned sample_rate) {

        if (path.empty() || start_seconds < 0.0) return false;

        std::vector<float> cache_original;
        std::vector<float> cache_vocals;
        std::vector<float> cache_instrumental;

        // Spectral Waveform may omit Original for persisted transport blocks.
        // Original is cheap to decode on demand; the separated stems are the
        // expensive data that must survive a restart.
        if (original != nullptr &&
            !convert_to_cache_stereo(original, frames, channels, sample_rate, cache_original)) {
            return false;
        }
        if (!convert_to_cache_stereo(vocals, frames, channels, sample_rate, cache_vocals) ||
            !convert_to_cache_stereo(instrumental, frames, channels, sample_rate, cache_instrumental)) {
            return false;
        }
        if (cache_vocals.empty() || cache_instrumental.size() != cache_vocals.size()) return false;
        if (!cache_original.empty() && cache_original.size() != cache_vocals.size()) return false;

        const size_t cache_frames = cache_vocals.size() / kCacheChannels;
        if (cache_frames == 0) return false;
        const double end_seconds = start_seconds +
            static_cast<double>(cache_frames) / static_cast<double>(kCacheRate);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_path.empty() || _wcsicmp(m_path.c_str(), path.c_str()) != 0) return false;

            // A re-analysis can republish the same tile. Replace the previous
            // external copy instead of growing the in-memory cache indefinitely.
            for (auto it = m_segments.begin(); it != m_segments.end();) {
                if ((*it)->external_waveform &&
                    std::abs((*it)->start_seconds - start_seconds) < 0.001 &&
                    std::abs((*it)->end_seconds - end_seconds) < 0.003) {
                    it = m_segments.erase(it);
                } else {
                    ++it;
                }
            }

            cache_segment seg;
            seg.generation = m_generation;
            seg.start_seconds = start_seconds;
            seg.end_seconds = end_seconds;
            seg.original = std::move(cache_original);
            seg.vocals = std::move(cache_vocals);
            seg.instrumental = std::move(cache_instrumental);
            seg.external_waveform = true;
            m_segments.push_back(std::make_shared<cache_segment>(std::move(seg)));
        }

        m_ready_cv.notify_all();
        return true;
    }

    bool transport_position_ready(double position_seconds) const {
        const stemmode::mode mode = stemmode::get();
        if (mode == stemmode::mode::original) return true;
        if (position_seconds < 0.0) position_seconds = 0.0;

        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& seg_ptr : m_segments) {
                const cache_segment& seg = *seg_ptr;
            if (!segment_has_mode(seg, mode)) continue;
            if (position_seconds >= seg.start_seconds &&
                position_seconds < seg.end_seconds) {
                return true;
            }
        }
        return false;
    }

    void request_transport(double position_seconds, bool reverse) {
        if (position_seconds < 0.0) position_seconds = 0.0;
        const stemmode::mode mode = stemmode::get();

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_path.empty()) return;

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

        m_jobs.emplace_front(cache_job{
            m_generation, m_path, start, true, true,
            mode != stemmode::mode::original});
        m_job_pending = true;
        m_cv.notify_one();
    }

    bool render(
        stemmode::mode mode,
        double start_seconds,
        unsigned output_rate,
        size_t frames,
        std::vector<float>& out,
        double source_rate = 1.0) {

        if (output_rate == 0 ||
            frames == 0) {
            return false;
        }

        g_dbg_render_attempts.fetch_add(1, std::memory_order_relaxed);
        g_dbg_last_render_start.store(start_seconds, std::memory_order_relaxed);
        g_dbg_last_source_rate.store(source_rate, std::memory_order_relaxed);
        g_dbg_last_render_source.store(stem_debug_source_none, std::memory_order_relaxed);
        g_dbg_last_render_ok.store(0, std::memory_order_relaxed);

        std::vector<std::shared_ptr<const cache_segment>> snapshot;

        {
            std::lock_guard<std::mutex> lock(
                m_mutex);

            if (mode == stemmode::mode::original && !m_live_original.empty()) {
                const size_t live_frames = m_live_original.size() / kCacheChannels;
                const double live_start = static_cast<double>(
                    m_live_original_start_frame) / static_cast<double>(kCacheRate);
                const double live_end = live_start +
                    static_cast<double>(live_frames) / static_cast<double>(kCacheRate);
                const double last_t = start_seconds +
                    source_rate * static_cast<double>(frames - 1) /
                    static_cast<double>(output_rate);
                const double need_start = (std::min)(start_seconds, last_t);
                const double need_end = (std::max)(start_seconds, last_t);

                if (need_start >= live_start - 1.0e-9 &&
                    need_end < live_end && live_frames != 0) {
                    out.assign(frames * kCacheChannels, 0.0f);
                    const double dt = 1.0 / static_cast<double>(output_rate);
                    for (size_t f = 0; f < frames; ++f) {
                        const double t = start_seconds +
                            source_rate * static_cast<double>(f) * dt;
                        double source_pos =
                            t * static_cast<double>(kCacheRate) -
                            static_cast<double>(m_live_original_start_frame);
                        if (source_pos < 0.0) source_pos = 0.0;
                        size_t i0 = static_cast<size_t>(source_pos);
                        if (i0 >= live_frames) i0 = live_frames - 1;
                        const size_t i1 = (std::min)(i0 + 1, live_frames - 1);
                        const float frac = static_cast<float>(
                            source_pos - static_cast<double>(i0));
                        for (unsigned ch = 0; ch < kCacheChannels; ++ch) {
                            const float a = m_live_original[i0 * kCacheChannels + ch];
                            const float b = m_live_original[i1 * kCacheChannels + ch];
                            out[f * kCacheChannels + ch] = a + (b - a) * frac;
                        }
                    }
                    g_dbg_render_successes.fetch_add(1, std::memory_order_relaxed);
                    g_dbg_live_hits.fetch_add(1, std::memory_order_relaxed);
                    g_dbg_last_render_source.store(stem_debug_source_live, std::memory_order_relaxed);
                    g_dbg_last_render_ok.store(1, std::memory_order_relaxed);
                    return true;
                }
            }

            if (m_segments.empty()) {
                g_dbg_render_misses.fetch_add(1, std::memory_order_relaxed);
                g_dbg_last_render_source.store(stem_debug_source_miss, std::memory_order_relaxed);
                return false;
            }

            snapshot.reserve(m_segments.size());
            for (const auto& seg : m_segments) {
                snapshot.emplace_back(seg);
            }
        }

        out.assign(
            frames * kCacheChannels,
            0.0f);

        const double dt =
            1.0 /
            static_cast<double>(
                output_rate);

        for (size_t f = 0;
             f < frames;
             ++f) {

            const double t =
                start_seconds +
                source_rate * static_cast<double>(f) * dt;

            if (t < 0.0) {
                g_dbg_render_misses.fetch_add(1, std::memory_order_relaxed);
                g_dbg_last_render_source.store(stem_debug_source_miss, std::memory_order_relaxed);
                return false;
            }

            const cache_segment* first = nullptr;
            const cache_segment* second = nullptr;

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

            if (!first) {
                g_dbg_render_misses.fetch_add(1, std::memory_order_relaxed);
                g_dbg_last_render_source.store(stem_debug_source_miss, std::memory_order_relaxed);
                return false;
            }

            auto sample_from =
                [mode, t](
                    const cache_segment& seg,
                    unsigned ch) -> float {

                const std::vector<float>& data =
                    mode == stemmode::mode::original
                        ? seg.original
                        : (mode == stemmode::mode::vocals
                            ? seg.vocals
                            : seg.instrumental);

                const double rel =
                    t - seg.start_seconds;

                double source_pos =
                    rel *
                    static_cast<double>(
                        kCacheRate);

                if (source_pos < 0.0) {
                    source_pos = 0.0;
                }

                const size_t total_frames =
                    data.size() /
                    kCacheChannels;

                if (total_frames == 0) {
                    return 0.0f;
                }

                size_t i0 =
                    static_cast<size_t>(
                        source_pos);

                if (i0 >=
                    total_frames - 1) {

                    i0 =
                        total_frames - 1;

                    return data[
                        i0 * kCacheChannels +
                        ch];
                }

                const size_t i1 =
                    i0 + 1;

                const float frac =
                    static_cast<float>(
                        source_pos -
                        static_cast<double>(
                            i0));

                return
                    data[
                        i0 * kCacheChannels +
                        ch] *
                        (1.0f - frac) +
                    data[
                        i1 * kCacheChannels +
                        ch] *
                        frac;
            };

            for (unsigned ch = 0;
                 ch < kCacheChannels;
                 ++ch) {

                float value =
                    sample_from(
                        *first,
                        ch);

                if (second) {
                    const double overlap_start =
                        second->start_seconds;

                    const double overlap_end =
                        first->end_seconds;

                    if (overlap_end >
                            overlap_start &&
                        t >= overlap_start &&
                        t < overlap_end) {

                        const double midpoint =
                            0.5 *
                            (overlap_start +
                             overlap_end);

                        const double half_fade =
                            0.5 *
                            kCacheHandoffFadeSeconds;

                        const double fade_start =
                            midpoint - half_fade;

                        const double fade_end =
                            midpoint + half_fade;

                        const float second_value =
                            sample_from(
                                *second,
                                ch);

                        if (t >= fade_end) {
                            value =
                                second_value;
                        }
                        else if (t >
                                 fade_start) {

                            double x =
                                (t - fade_start) /
                                (fade_end -
                                 fade_start);

                            if (x < 0.0) x = 0.0;
                            if (x > 1.0) x = 1.0;

                            constexpr double kHalfPi =
                                1.57079632679489661923;

                            const double c =
                                std::cos(
                                    kHalfPi * x);

                            const double s =
                                std::sin(
                                    kHalfPi * x);

                            const float a =
                                static_cast<float>(
                                    c * c);

                            const float b =
                                static_cast<float>(
                                    s * s);

                            value =
                                value * a +
                                second_value * b;
                        }
                    }
                }

                out[
                    f * kCacheChannels +
                    ch] = value;
            }
        }

        g_dbg_render_successes.fetch_add(1, std::memory_order_relaxed);
        g_dbg_cache_hits.fetch_add(1, std::memory_order_relaxed);
        g_dbg_last_render_source.store(stem_debug_source_cache, std::memory_order_relaxed);
        g_dbg_last_render_ok.store(1, std::memory_order_relaxed);
        return true;
    }

private:
    static bool segment_has_mode(const cache_segment& seg, stemmode::mode mode) {
        if (mode == stemmode::mode::original) return !seg.original.empty();
        if (mode == stemmode::mode::vocals) return !seg.vocals.empty();
        return !seg.instrumental.empty();
    }

    bool range_ready_locked(stemmode::mode mode, double start_seconds, double end_seconds) const {
        if (end_seconds <= start_seconds + 1.0e-6) return true;

        double cursor = start_seconds;
        while (cursor < end_seconds - 1.0e-6) {
            double furthest = cursor;
            for (const auto& seg_ptr : m_segments) {
                const cache_segment& seg = *seg_ptr;
                if (!segment_has_mode(seg, mode)) continue;
                if (seg.start_seconds <= cursor + 1.0e-6 &&
                    seg.end_seconds > furthest) {
                    furthest = seg.end_seconds;
                }
            }
            if (furthest <= cursor + 1.0e-6) return false;
            cursor = furthest;
        }
        return true;
    }

    void queue_job_locked(
        double start_seconds,
        bool force_reanchor = false) {

        if (m_path.empty()) {
            return;
        }

        if (start_seconds < 0.0) {
            start_seconds = 0.0;
        }

        m_jobs.emplace_back(
            cache_job{
                m_generation,
                m_path,
                start_seconds,
                force_reanchor,
                false,
                true});

        m_job_pending = true;
    }

    static bool configure_reader(
        IMFSourceReader* reader) {

        ComPtr<IMFMediaType> type;

        HRESULT hr =
            MFCreateMediaType(&type);

        if (FAILED(hr)) {
            return false;
        }

        type->SetGUID(
            MF_MT_MAJOR_TYPE,
            MFMediaType_Audio);

        type->SetGUID(
            MF_MT_SUBTYPE,
            MFAudioFormat_Float);

        type->SetUINT32(
            MF_MT_AUDIO_NUM_CHANNELS,
            kCacheChannels);

        type->SetUINT32(
            MF_MT_AUDIO_SAMPLES_PER_SECOND,
            kCacheRate);

        type->SetUINT32(
            MF_MT_AUDIO_BITS_PER_SAMPLE,
            32);

        type->SetUINT32(
            MF_MT_AUDIO_BLOCK_ALIGNMENT,
            8);

        type->SetUINT32(
            MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
            kCacheRate * 8);

        hr =
            reader->SetCurrentMediaType(
                MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                nullptr,
                type.Get());

        if (FAILED(hr)) {
            return false;
        }

        reader->SetStreamSelection(
            MF_SOURCE_READER_ALL_STREAMS,
            FALSE);

        reader->SetStreamSelection(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            TRUE);

        return true;
    }

    static bool open_decoder(
        sequential_decoder_state& state,
        const std::wstring& path) {

        state.reset();

        HRESULT hr =
            MFCreateSourceReaderFromURL(
                path.c_str(),
                nullptr,
                &state.reader);

        if (FAILED(hr)) {
            return false;
        }

        if (!configure_reader(
                state.reader.Get())) {
            state.reset();
            return false;
        }

        state.path = path;
        state.valid = true;
        state.fifo_start_frame = 0;
        state.decoded_end_frame = 0;
        state.fifo.clear();

        return true;
    }

    static bool reanchor_decoder(
        sequential_decoder_state& state,
        const std::wstring& path,
        double target_seconds,
        double seek_preroll_seconds) {

        if (!state.valid ||
            state.path != path) {

            if (!open_decoder(
                    state,
                    path)) {
                return false;
            }
        }

        double seek_seconds =
            target_seconds -
            (std::max)(0.0, seek_preroll_seconds);

        if (seek_seconds < 0.0) {
            seek_seconds = 0.0;
        }

        PROPVARIANT pos;
        PropVariantInit(&pos);

        pos.vt = VT_I8;
        pos.hVal.QuadPart =
            static_cast<LONGLONG>(
                seek_seconds *
                10000000.0);

        HRESULT hr =
            state.reader->
                SetCurrentPosition(
                    GUID_NULL,
                    pos);

        PropVariantClear(&pos);

        if (FAILED(hr)) {
            state.reset();
            return false;
        }

        state.fifo.clear();

        // Start absolute indexing at the requested target. We decode from the
        // preroll position and discard until the exact target timestamp.
        state.fifo_start_frame =
            static_cast<uint64_t>(
                target_seconds *
                static_cast<double>(
                    kCacheRate) +
                0.5);

        state.decoded_end_frame =
            state.fifo_start_frame;

        return true;
    }

    static bool append_decoded_until(
        sequential_decoder_state& state,
        uint64_t required_end_frame,
        double trim_target_seconds,
        bool& reached_target) {

        while (state.decoded_end_frame <
               required_end_frame) {

            DWORD stream_index = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;

            ComPtr<IMFSample> sample;

            HRESULT hr =
                state.reader->ReadSample(
                    MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                    0,
                    &stream_index,
                    &flags,
                    &timestamp,
                    &sample);

            if (FAILED(hr)) {
                state.reset();
                return false;
            }

            if (flags &
                MF_SOURCE_READERF_ENDOFSTREAM) {
                break;
            }

            if (!sample) {
                continue;
            }

            ComPtr<IMFMediaBuffer> buffer;

            hr =
                sample->ConvertToContiguousBuffer(
                    &buffer);

            if (FAILED(hr)) {
                state.reset();
                return false;
            }

            BYTE* data = nullptr;
            DWORD max_length = 0;
            DWORD current_length = 0;

            hr =
                buffer->Lock(
                    &data,
                    &max_length,
                    &current_length);

            if (FAILED(hr)) {
                state.reset();
                return false;
            }

            const size_t sample_values =
                current_length /
                sizeof(float);

            const size_t sample_frames =
                sample_values /
                kCacheChannels;

            const float* floats =
                reinterpret_cast<
                    const float*>(data);

            size_t first_frame = 0;

            if (!reached_target) {
                const double sample_start =
                    static_cast<double>(
                        timestamp) /
                    10000000.0;

                const double sample_end =
                    sample_start +
                    static_cast<double>(
                        sample_frames) /
                    static_cast<double>(
                        kCacheRate);

                if (sample_end <=
                    trim_target_seconds) {

                    buffer->Unlock();
                    continue;
                }

                if (sample_start <
                    trim_target_seconds) {

                    const double skip_d =
                        (trim_target_seconds -
                         sample_start) *
                        static_cast<double>(
                            kCacheRate);

                    size_t skip =
                        static_cast<size_t>(
                            skip_d + 0.5);

                    if (skip > sample_frames) {
                        skip = sample_frames;
                    }

                    first_frame = skip;
                }

                reached_target = true;
            }

            const size_t available_frames =
                sample_frames >
                    first_frame
                    ? sample_frames -
                        first_frame
                    : 0;

            if (available_frames != 0) {
                const size_t first_value =
                    first_frame *
                    kCacheChannels;

                const size_t values =
                    available_frames *
                    kCacheChannels;

                state.fifo.insert(
                    state.fifo.end(),
                    floats + first_value,
                    floats + first_value +
                        values);

                state.decoded_end_frame +=
                    static_cast<uint64_t>(
                        available_frames);
            }

            buffer->Unlock();
        }

        return
            reached_target &&
            state.decoded_end_frame >
                state.fifo_start_frame;
    }

    static bool decode_exact_block(
        sequential_decoder_state& state,
        double requested_start_seconds,
        bool force_reanchor,
        double window_seconds,
        double overlap_seconds,
        double seek_preroll_seconds,
        std::vector<float>& audio) {

        if (!state.valid) {
            return false;
        }

        const uint64_t block_start_frame =
            static_cast<uint64_t>(
                requested_start_seconds *
                static_cast<double>(
                    kCacheRate) +
                0.5);

        const uint64_t window_frames =
            static_cast<uint64_t>(
                window_seconds *
                static_cast<double>(
                    kCacheRate) +
                0.5);

        const uint64_t desired_block_end_frame =
            block_start_frame +
            window_frames;

        bool reached_target = true;

        if (force_reanchor) {
            if (!reanchor_decoder(
                    state,
                    state.path,
                    requested_start_seconds,
                    seek_preroll_seconds)) {
                return false;
            }
            reached_target = false;
        }

        if (block_start_frame <
            state.fifo_start_frame) {

            if (!reanchor_decoder(
                    state,
                    state.path,
                    requested_start_seconds,
                    seek_preroll_seconds)) {
                return false;
            }
            reached_target = false;
        }

        if (!append_decoded_until(
                state,
                desired_block_end_frame,
                requested_start_seconds,
                reached_target)) {
            return false;
        }

        if (block_start_frame <
            state.fifo_start_frame) {
            return false;
        }

        // V34: allow a short final block at EOF instead of requiring a full
        // 20-second cache window.
        const uint64_t actual_block_end_frame =
            state.decoded_end_frame <
                desired_block_end_frame
                ? state.decoded_end_frame
                : desired_block_end_frame;

        if (actual_block_end_frame <=
            block_start_frame) {
            return false;
        }

        const uint64_t offset_frames =
            block_start_frame -
            state.fifo_start_frame;

        const uint64_t actual_frames =
            actual_block_end_frame -
            block_start_frame;

        const size_t offset_values =
            static_cast<size_t>(
                offset_frames *
                kCacheChannels);

        const size_t actual_values =
            static_cast<size_t>(
                actual_frames *
                kCacheChannels);

        if (offset_values +
                actual_values >
            state.fifo.size()) {
            return false;
        }

        audio.assign(
            state.fifo.begin() +
                static_cast<std::ptrdiff_t>(
                    offset_values),
            state.fifo.begin() +
                static_cast<std::ptrdiff_t>(
                    offset_values +
                    actual_values));

        const uint64_t hop_frames =
            static_cast<uint64_t>(
                ((std::max)(0.001, window_seconds - overlap_seconds)) *
                static_cast<double>(
                    kCacheRate) +
                0.5);

        const uint64_t next_start_frame =
            block_start_frame +
            hop_frames;

        if (next_start_frame >
                state.fifo_start_frame &&
            next_start_frame <=
                state.decoded_end_frame) {

            const uint64_t drop_frames =
                next_start_frame -
                state.fifo_start_frame;

            const size_t drop_values =
                static_cast<size_t>(
                    drop_frames *
                    kCacheChannels);

            if (drop_values <=
                state.fifo.size()) {

                state.fifo.erase(
                    state.fifo.begin(),
                    state.fifo.begin() +
                        static_cast<std::ptrdiff_t>(
                            drop_values));

                state.fifo_start_frame =
                    next_start_frame;
            }
        }

        return true;
    }


    static void apply_first_block_fade(
        std::vector<float>& samples) {

        const size_t frames =
            samples.size() /
            kCacheChannels;

        size_t fade_frames =
            static_cast<size_t>(
                kFirstBlockFadeSeconds *
                static_cast<double>(
                    kCacheRate));

        if (fade_frames > frames) {
            fade_frames = frames;
        }

        if (fade_frames == 0) {
            return;
        }

        for (size_t f = 0;
             f < fade_frames;
             ++f) {

            const float gain =
                fade_frames > 1
                    ? static_cast<float>(f) /
                        static_cast<float>(
                            fade_frames - 1)
                    : 1.0f;

            for (unsigned ch = 0;
                 ch < kCacheChannels;
                 ++ch) {

                samples[
                    f * kCacheChannels +
                    ch] *= gain;
            }
        }
    }


    void worker_main() noexcept {
        HRESULT chr = E_FAIL;
        bool com_initialized = false;
        bool mf_started = false;

        auto release_waiters =
            [this](uint64_t generation) {

                {
                    std::lock_guard<std::mutex>
                        lock(m_mutex);

                    if (generation ==
                        m_generation) {

                        m_job_pending =
                            !m_jobs.empty();
                    }
                }

                // A pre-cache DSP callback may be waiting for this job.
                // Always wake it on success OR failure.
                m_ready_cv.notify_all();
            };

        try {
            chr =
                CoInitializeEx(
                    nullptr,
                    COINIT_MULTITHREADED);

            if (SUCCEEDED(chr)) {
                com_initialized = true;
            }
            else if (chr !=
                     RPC_E_CHANGED_MODE) {
                return;
            }

            const HRESULT mhr =
                MFStartup(
                    MF_VERSION,
                    MFSTARTUP_FULL);

            if (FAILED(mhr)) {
                return;
            }

            mf_started = true;

            // Original platter PCM does not need ONNX at all. Construct the
            // separation engine lazily so cheap Original prefetch can begin as
            // soon as Media Foundation is ready instead of waiting for model
            // initialization first. Construction still stays inside the worker
            // exception boundary and is additionally covered by the per-job try.
            std::unique_ptr<onnxstem::engine> engine;

            sequential_decoder_state decoder_state;
            sequential_decoder_state preview_decoder_state;

            for (;;) {
                cache_job job;

                {
                    std::unique_lock<std::mutex>
                        lock(m_mutex);

                    m_cv.wait(
                        lock,
                        [this]() {
                            return
                                m_stop ||
                                !m_jobs.empty();
                        });

                    if (m_stop) {
                        break;
                    }

                    job =
                        std::move(
                            m_jobs.front());

                    m_jobs.pop_front();
                }

                try {
                    std::vector<float> input;

                    // Transport preview gets a separate decoder timeline. A random
                    // jog request therefore cannot make the ordinary forward cache
                    // decode a huge gap before its next continuation block.
                    sequential_decoder_state& active_decoder =
                        job.transport_preview ? preview_decoder_state : decoder_state;

                    if (!active_decoder.valid ||
                        active_decoder.path != job.path) {

                        if (!open_decoder(
                                active_decoder,
                                job.path)) {

                            throw std::runtime_error(
                                "Could not open live decoder");
                        }
                    }

                    const double decode_window = job.need_stems
                        ? kCacheSeconds
                        : (job.transport_preview
                            ? kOriginalQuickCacheSeconds
                            : kOriginalBackgroundCacheSeconds);
                    const double decode_overlap = job.need_stems
                        ? kCacheOverlapSeconds
                        : (job.transport_preview
                            ? kOriginalQuickOverlapSeconds
                            : kOriginalBackgroundOverlapSeconds);
                    const double decode_preroll = job.need_stems
                        ? kDecodeSeekPrerollSeconds
                        : kOriginalDecodeSeekPrerollSeconds;

                    const bool decoded =
                        decode_exact_block(
                            active_decoder,
                            job.start_seconds,
                            job.force_reanchor,
                            decode_window,
                            decode_overlap,
                            decode_preroll,
                            input);

                    std::vector<float> vocals;
                    std::vector<float> instrumental;

                    bool separated = !job.need_stems;
                    size_t decoded_frames = 0;

                    if (decoded) {
                        decoded_frames =
                            input.size() /
                            kCacheChannels;

                        if (decoded_frames != 0 && job.need_stems) {
                            const size_t full_window_frames =
                                static_cast<size_t>(
                                    kCacheSeconds *
                                    static_cast<double>(
                                        kCacheRate) +
                                    0.5);

                            std::vector<float> analysis_input = input;

                            if (decoded_frames < full_window_frames) {
                                analysis_input.resize(
                                    full_window_frames * kCacheChannels, 0.0f);
                            }

                            if (!engine) {
                                engine = std::make_unique<onnxstem::engine>();
                            }

                            separated =
                                engine->process_both(
                                    analysis_input.data(),
                                    analysis_input.size() / kCacheChannels,
                                    kCacheChannels,
                                    kCacheRate,
                                    vocals,
                                    instrumental);

                            if (separated) {
                                const size_t true_values =
                                    decoded_frames * kCacheChannels;

                                if (vocals.size() >= true_values &&
                                    instrumental.size() >= true_values) {
                                    vocals.resize(true_values);
                                    instrumental.resize(true_values);
                                } else {
                                    separated = false;
                                }
                            }
                        }
                    }

                    {
                        std::lock_guard<std::mutex>
                            lock(m_mutex);

                        // Seek/new-track happened while this job ran.
                        if (job.generation !=
                            m_generation) {

                            m_job_pending =
                                !m_jobs.empty();
                        }
                        else {
                            const bool stem_payload_ok =
                                !job.need_stems ||
                                (separated &&
                                 vocals.size() == input.size() &&
                                 instrumental.size() == input.size());

                            if (decoded && !input.empty() && stem_payload_ok) {
                                // Only the first separated live cache block gets this
                                // tiny fade. Original preview data is never altered.
                                if (job.need_stems && job.start_seconds <= 0.000001) {
                                    apply_first_block_fade(vocals);
                                    apply_first_block_fade(instrumental);
                                }

                                const size_t frames = input.size() / kCacheChannels;
                                const double duration =
                                    static_cast<double>(frames) /
                                    static_cast<double>(kCacheRate);

                                cache_segment seg;
                                seg.generation = job.generation;
                                seg.start_seconds = job.start_seconds;
                                seg.end_seconds = job.start_seconds + duration;
                                seg.original = input;
                                if (job.need_stems) {
                                    seg.vocals = std::move(vocals);
                                    seg.instrumental = std::move(instrumental);
                                }

                                // Completed segments remain valid for the whole track.
                                // This also makes short reverse moves instant instead of
                                // re-running Spleeter after every release/seek.
                                m_segments.push_back(std::make_shared<cache_segment>(std::move(seg)));
                            }

                            m_job_pending =
                                !m_jobs.empty();
                        }
                    }

                    m_ready_cv.notify_all();
                }
                catch (const std::exception&) {
                    // A failed cache job must never take foobar down.
                    // Drop this job, release any pre-cache waiter, and allow
                    // playback to fall back to original audio.
                    release_waiters(
                        job.generation);
                }
                catch (...) {
                    // sherpa-onnx / Media Foundation can surface non-standard
                    // exceptions as well. Treat them exactly like a failed
                    // cache job.
                    release_waiters(
                        job.generation);
                }
            }
        }
        catch (const std::exception&) {
            // Protect the entire worker lifetime, including ONNX engine
            // construction. No exception may cross std::thread's entry point.
            {
                std::lock_guard<std::mutex>
                    lock(m_mutex);

                m_jobs.clear();
                m_job_pending = false;
            }

            m_ready_cv.notify_all();
        }
        catch (...) {
            {
                std::lock_guard<std::mutex>
                    lock(m_mutex);

                m_jobs.clear();
                m_job_pending = false;
            }

            m_ready_cv.notify_all();
        }

        if (mf_started) {
            MFShutdown();
        }

        if (com_initialized) {
            CoUninitialize();
        }
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_ready_cv;

    std::thread m_thread;
    bool m_stop = false;

    uint64_t m_generation = 1;
    uint64_t m_track_start_generation = 0;

    std::wstring m_path;
    double m_anchor_seconds = 0.0;

    bool m_job_pending = false;

    std::deque<cache_job> m_jobs;
    std::deque<std::shared_ptr<cache_segment>> m_segments;

    // Directly harvested from foobar's decoded Original stream. Unlike cache
    // jobs, this region is already in RAM and cannot be invalidated by rapid
    // platter retargeting.
    std::deque<float> m_live_original;
    uint64_t m_live_original_start_frame = 0;
};

live_cache_manager& cache_manager() {
    static live_cache_manager instance;
    return instance;
}

struct transport_snapshot {
    int state = stem_transport_normal;
    double position_seconds = 0.0;
    double render_seconds = 0.0;
    ULONGLONG scrub_audible_until = 0;
    double scrub_velocity = 0.0;
    ULONGLONG scrub_motion_tick = 0;
};

struct scrub_motion_event {
    double position_seconds = 0.0;
    std::chrono::steady_clock::time_point when{};
};

class transport_controller {
public:
    void set_hold(double seconds) {
        seconds = (std::max)(0.0, seconds);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state = stem_transport_hold;
            m_position_seconds = seconds;
            m_render_seconds = seconds;
            m_scrub_audible_until = 0;
            m_scrub_velocity = 0.0;
            m_scrub_motion_tick = GetTickCount64();
            m_scrub_motion_clock = std::chrono::steady_clock::now();
            m_scrub_motion_events.clear();
            m_scrub_motion_events.push_back(
                scrub_motion_event{seconds, m_scrub_motion_clock});
        }
        cache_manager().request_transport(seconds, false);
    }

    void set_scrub(double seconds) {
        seconds = (std::max)(0.0, seconds);
        bool retarget = true;
        bool reverse = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const int previous_state = m_state;
            const double previous_position = m_position_seconds;
            const ULONGLONG now = GetTickCount64();
            const auto now_clock = std::chrono::steady_clock::now();

            retarget =
                previous_state != stem_transport_scrub ||
                std::abs(seconds - previous_position) >
                    kScrubKeepaliveToleranceSeconds;

            // A centered grab enters from HOLD, whose render cursor is the exact
            // sample that was under the playhead when the platter was grabbed.
            if (previous_state != stem_transport_scrub &&
                previous_state != stem_transport_hold) {
                m_render_seconds = seconds;
            }

            if (retarget) {
                double dt = kScrubGestureDefaultDt;
                if (previous_state == stem_transport_scrub &&
                    m_scrub_motion_clock.time_since_epoch().count() != 0) {
                    dt = std::clamp(
                        std::chrono::duration<double>(
                            now_clock - m_scrub_motion_clock).count(),
                        kScrubGestureMinDt, kScrubGestureMaxDt);
                }

                // Use the actual high-resolution event interval. Do not carry the
                // previous speed into this measurement; that was making slow mouse
                // gestures chirp after irregular Windows message spacing.
                double measured =
                    dt > 0.0 ? (seconds - previous_position) / dt : 0.0;
                measured = std::clamp(
                    measured, -kScrubMaxSourceRate, kScrubMaxSourceRate);

                m_scrub_velocity = measured;
                m_scrub_motion_tick = now;
                m_scrub_motion_clock = now_clock;
                m_scrub_audible_until = now + kScrubAudibleSafetyMs;

                // Preserve the real mouse trajectory instead of extrapolating one
                // velocity across an arbitrary foobar DSP block. HOLD already seeds
                // the queue with the grab position. For any other entry path, create
                // a synthetic predecessor at the measured event interval.
                if (previous_state != stem_transport_scrub &&
                    previous_state != stem_transport_hold) {
                    m_scrub_motion_events.clear();
                    m_scrub_motion_events.push_back(scrub_motion_event{
                        previous_position,
                        now_clock - std::chrono::duration_cast<
                            std::chrono::steady_clock::duration>(
                                std::chrono::duration<double>(dt))});
                }
                if (m_scrub_motion_events.empty()) {
                    m_scrub_motion_events.push_back(scrub_motion_event{
                        previous_position,
                        now_clock - std::chrono::duration_cast<
                            std::chrono::steady_clock::duration>(
                                std::chrono::duration<double>(dt))});
                }
                m_scrub_motion_events.push_back(
                    scrub_motion_event{seconds, now_clock});
                while (m_scrub_motion_events.size() > 96) {
                    m_scrub_motion_events.pop_front();
                }
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
                m_scrub_motion_clock = now_clock;
                m_render_seconds = seconds;
                // Soft idle is a real stationary platter point. Keep the recent
                // trajectory and append a stationary endpoint; the DSP renders a
                // fixed wall-clock window, so preserving the tail cannot replay old
                // motion but does let the last few milliseconds drain naturally.
                if (m_scrub_motion_events.empty() ||
                    m_scrub_motion_events.back().when != now_clock) {
                    m_scrub_motion_events.push_back(
                        scrub_motion_event{seconds, now_clock});
                }
                while (m_scrub_motion_events.size() > 128) {
                    m_scrub_motion_events.pop_front();
                }
            }

            m_state = stem_transport_scrub;
            m_position_seconds = seconds;
            reverse = m_scrub_velocity < 0.0 || seconds < m_render_seconds;
        }

        if (retarget) {
            cache_manager().request_transport(seconds, reverse);
        }
    }

    void set_reverse(double seconds) {
        seconds = (std::max)(0.0, seconds);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state = stem_transport_reverse;
            m_position_seconds = seconds;
            m_render_seconds = seconds;
            m_scrub_audible_until = 0;
            m_scrub_velocity = 0.0;
            m_scrub_motion_tick = 0;
            m_scrub_motion_clock = {};
            m_scrub_motion_events.clear();
        }
        cache_manager().request_transport(seconds, true);
    }

    void release_transport(double seconds) {
        seconds = (std::max)(0.0, seconds);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state = stem_transport_release_wait;
            m_position_seconds = seconds;
            m_render_seconds = seconds;
            m_scrub_audible_until = 0;
            m_scrub_velocity = 0.0;
            m_scrub_motion_tick = 0;
            m_scrub_motion_clock = {};
            m_scrub_motion_events.clear();
        }
        cache_manager().request_transport(seconds, false);
    }

    void cancel() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = stem_transport_normal;
        m_scrub_audible_until = 0;
        m_scrub_velocity = 0.0;
        m_scrub_motion_tick = 0;
        m_scrub_motion_clock = {};
        m_scrub_motion_events.clear();
    }

    transport_snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return transport_snapshot{
            m_state, m_position_seconds, m_render_seconds,
            m_scrub_audible_until, m_scrub_velocity, m_scrub_motion_tick};
    }

    bool snapshot_scrub_motion(std::vector<scrub_motion_event>& out) {
        out.clear();
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state != stem_transport_scrub || m_scrub_motion_events.empty()) {
            return false;
        }

        const auto cutoff = std::chrono::steady_clock::now() -
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(kScrubTrajectoryHistorySeconds));

        // Retain one predecessor before the cutoff so interpolation at the start
        // of the requested wall-clock window remains continuous.
        while (m_scrub_motion_events.size() > 2 &&
               m_scrub_motion_events[1].when < cutoff) {
            m_scrub_motion_events.pop_front();
        }

        out.assign(
            m_scrub_motion_events.begin(),
            m_scrub_motion_events.end());
        return !out.empty();
    }

    double visible_position() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_position_seconds;
    }

    int state() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_state;
    }

    void complete_scrub(double seconds) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state == stem_transport_scrub) {
            m_render_seconds = (std::max)(0.0, seconds);
        }
    }

    void advance_reverse(double seconds) {
        double next = 0.0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_state != stem_transport_reverse) return;
            m_render_seconds = (std::max)(0.0, m_render_seconds - seconds);
            m_position_seconds = m_render_seconds;
            next = m_render_seconds;
        }
        cache_manager().request_transport(next, true);
    }

    void finish_release() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state == stem_transport_release_wait) {
            m_state = stem_transport_normal;
        }
    }

private:
    mutable std::mutex m_mutex;
    int m_state = stem_transport_normal;
    double m_position_seconds = 0.0;
    double m_render_seconds = 0.0;
    ULONGLONG m_scrub_audible_until = 0;
    double m_scrub_velocity = 0.0;
    ULONGLONG m_scrub_motion_tick = 0;
    std::chrono::steady_clock::time_point m_scrub_motion_clock{};
    std::deque<scrub_motion_event> m_scrub_motion_events;
};

transport_controller& transport() {
    static transport_controller instance;
    return instance;
}

class stem_transport_service_impl : public stem_transport_service {
public:
    void set_hold(double seconds) override { transport().set_hold(seconds); }
    void set_scrub(double seconds) override { transport().set_scrub(seconds); }
    void set_reverse(double seconds) override { transport().set_reverse(seconds); }
    void release_transport(double seconds) override { transport().release_transport(seconds); }
    void cancel_transport() override { transport().cancel(); }
    int get_state() override { return transport().state(); }
    double get_position_seconds() override { return transport().visible_position(); }
    bool is_position_ready(double seconds) override {
        return cache_manager().transport_position_ready(seconds);
    }
    bool get_debug_status(stem_transport_debug_status& out) override {
        try {
            const transport_snapshot ts = transport().snapshot();
            out = stem_transport_debug_status{};
            out.state = ts.state;
            out.mode = static_cast<int>(stemmode::get());
            out.position_seconds = ts.position_seconds;
            out.render_seconds = ts.render_seconds;
            out.scrub_velocity = ts.scrub_velocity;
            out.last_render_source = g_dbg_last_render_source.load(std::memory_order_relaxed);
            out.last_render_ok = g_dbg_last_render_ok.load(std::memory_order_relaxed);
            out.last_render_start_seconds = g_dbg_last_render_start.load(std::memory_order_relaxed);
            out.last_source_rate = g_dbg_last_source_rate.load(std::memory_order_relaxed);
            cache_manager().debug_live_range(out.live_start_seconds, out.live_end_seconds);
            out.render_attempts = g_dbg_render_attempts.load(std::memory_order_relaxed);
            out.render_successes = g_dbg_render_successes.load(std::memory_order_relaxed);
            out.live_hits = g_dbg_live_hits.load(std::memory_order_relaxed);
            out.cache_hits = g_dbg_cache_hits.load(std::memory_order_relaxed);
            out.render_misses = g_dbg_render_misses.load(std::memory_order_relaxed);
            out.scrub_audio_writes = g_dbg_scrub_audio_writes.load(std::memory_order_relaxed);
            out.scrub_silence_writes = g_dbg_scrub_silence_writes.load(std::memory_order_relaxed);
            return true;
        } catch (...) {
            return false;
        }
    }
    bool publish_cache_block(
        const char* track_path_utf8,
        double start_seconds,
        const float* original,
        const float* vocals,
        const float* instrumental,
        t_size frames,
        unsigned channels,
        unsigned sample_rate) override {
        return cache_manager().publish_external_segment(
            local_path_from_utf8_cache(track_path_utf8),
            start_seconds,
            original, vocals, instrumental,
            static_cast<size_t>(frames), channels, sample_rate);
    }
};

static service_factory_single_t<stem_transport_service_impl>
    g_stem_transport_service_factory;

class stem_playback_observer :
    public play_callback_static {
public:
    unsigned get_flags() override {
        return
            flag_on_playback_new_track |
            flag_on_playback_stop |
            flag_on_playback_seek |
            flag_on_playback_time;
    }

    void on_playback_starting(
        play_control::t_track_command,
        bool) override {}

    void on_playback_new_track(
        metadb_handle_ptr track) override {

        transport().cancel();
        cache_manager().new_track(
            local_path_from_metadb(
                track));
    }

    void on_playback_stop(
        play_control::t_stop_reason) override {

        transport().cancel();
        cache_manager().stop();
    }

    void on_playback_seek(
        double time) override {

        // HOLD, SCRUB, REVERSE and RELEASE all arm transport first and then seek
        // to that same sample solely to flush foobar's queued output. Treat that
        // as a timeline re-anchor, not as a real user seek; otherwise every grab
        // cancels the platter prefetch job we just started.
        const int state = transport().state();
        if (state != stem_transport_normal) {
            const double transport_position = transport().visible_position();
            if (std::abs(time - transport_position) <= 0.050) {
                cache_manager().transport_flush_seek(time);
                return;
            }
        }

        cache_manager().seek(time);
    }

    void on_playback_pause(
        bool) override {}

    void on_playback_edited(
        metadb_handle_ptr) override {}

    void on_playback_dynamic_info(
        const file_info&) override {}

    void on_playback_dynamic_info_track(
        const file_info&) override {}

    void on_playback_time(
        double time) override {

        cache_manager().ensure_ahead(time);
    }

    void on_volume_change(
        float) override {}
};

static play_callback_static_factory_t<
    stem_playback_observer>
    g_stem_playback_observer_factory;

class stem_dsp :
    public dsp_impl_base {
public:
    explicit stem_dsp(
        dsp_preset const&) {}

    static GUID g_get_guid() {
        static const GUID guid =
            {0x1fd7bdf4,0xa0e1,0x4b3e,
             {0x9b,0x5a,0x45,0xe6,
              0xba,0x88,0x34,0x22}};

        return guid;
    }

    static void g_get_name(
        pfc::string_base& out) {

        out =
            "Stem Separator (ONNX)";
    }

    static bool g_get_default_preset(
        dsp_preset& out) {

        dsp_preset_builder builder;

        builder.finish(
            g_get_guid(),
            out);

        return true;
    }

    static bool g_have_config_popup() {
        return false;
    }

    static void g_show_config_popup(
        const dsp_preset&,
        HWND,
        dsp_preset_edit_callback&) {}

    bool on_chunk(
        audio_chunk* chunk,
        abort_callback&) override {

        if (!chunk ||
            chunk->is_empty()) {

            return false;
        }

        const unsigned channels =
            chunk->get_channels();

        const unsigned rate =
            chunk->get_srate();

        const size_t frames =
            chunk->get_sample_count();

        if (channels !=
                kCacheChannels ||
            rate == 0) {

            reset_local();
            return true;
        }

        const uint64_t gen =
            cache_manager().
                generation();

        if (!m_have_position ||
            gen != m_generation) {

            m_generation = gen;

            m_position_seconds =
                cache_manager().
                    anchor_time();

            m_have_position = true;
            m_using_stem = false;
            m_precache_handled = false;
        }

        const stemmode::mode mode =
            stemmode::get();

        // Harvest foobar's already-decoded Original PCM before HOLD/SCRUB/REVERSE
        // replaces this chunk. During transport the underlying decoder keeps moving,
        // so the RAM platter buffer naturally grows ahead while retaining history.
        if (mode == stemmode::mode::original) {
            cache_manager().publish_live_original(
                m_position_seconds, chunk->get_data(), frames, channels, rate);
        }

        // Transport preview keeps foobar's audio clock running while replacing
        // what is heard. That gives us a real stationary HOLD, audible jog, and
        // 1x reverse without hammering playback_seek on every mouse/key event.
        const transport_snapshot ts = transport().snapshot();
        if (ts.state != stem_transport_normal) {
            const double chunk_seconds =
                static_cast<double>(frames) / static_cast<double>(rate);

            auto write_silence = [&]() {
                if (ts.state == stem_transport_scrub) {
                    g_dbg_scrub_silence_writes.fetch_add(1, std::memory_order_relaxed);
                }
                std::vector<audio_sample> zeros(frames * kCacheChannels, 0);

                if (m_transportTailValid && frames != 0) {
                    const size_t fade_frames = (std::min)(
                        frames,
                        (std::max)(static_cast<size_t>(1),
                            static_cast<size_t>(static_cast<double>(rate) * 0.0025)));

                    for (size_t f = 0; f < fade_frames; ++f) {
                        const double gain =
                            1.0 - static_cast<double>(f + 1) /
                                static_cast<double>(fade_frames);
                        for (unsigned ch = 0; ch < kCacheChannels; ++ch) {
                            zeros[f * kCacheChannels + ch] =
                                static_cast<audio_sample>(
                                    static_cast<double>(m_transportTail[ch]) * gain);
                        }
                    }
                    m_transportTailValid = false;
                }

                chunk->set_data(
                    zeros.data(), frames, channels, rate, chunk->get_channel_config());
            };

            auto write_preview = [&](const std::vector<float>& rendered) {
                if (ts.state == stem_transport_scrub) {
                    g_dbg_scrub_audio_writes.fetch_add(1, std::memory_order_relaxed);
                }
                std::vector<audio_sample> output(rendered.size());
                for (size_t i = 0; i < rendered.size(); ++i) {
                    output[i] = static_cast<audio_sample>(rendered[i]);
                }

                if (m_transportTailValid && frames != 0) {
                    const size_t blend_frames = (std::min)(
                        frames,
                        (std::max)(static_cast<size_t>(1),
                            static_cast<size_t>(static_cast<double>(rate) * 0.0010)));
                    for (size_t f = 0; f < blend_frames; ++f) {
                        const double alpha =
                            static_cast<double>(f + 1) /
                            static_cast<double>(blend_frames);
                        for (unsigned ch = 0; ch < kCacheChannels; ++ch) {
                            const size_t i = f * kCacheChannels + ch;
                            output[i] = static_cast<audio_sample>(
                                static_cast<double>(m_transportTail[ch]) * (1.0 - alpha) +
                                static_cast<double>(output[i]) * alpha);
                        }
                    }
                }

                chunk->set_data(
                    output.data(), frames, channels, rate, chunk->get_channel_config());

                if (frames != 0) {
                    const size_t last = (frames - 1) * kCacheChannels;
                    for (unsigned ch = 0; ch < kCacheChannels; ++ch) {
                        m_transportTail[ch] = output[last + ch];
                    }
                    m_transportTailValid = true;
                }
            };

            if (ts.state == stem_transport_hold) {
                m_scrubRateValid = false;
                m_scrubPreviousRate = 0.0;
                write_silence();
                m_position_seconds += chunk_seconds;
                m_using_stem = false;
                return true;
            }

            if (ts.state == stem_transport_scrub) {
                const double move_epsilon =
                    0.5 / static_cast<double>(rate);

                std::vector<scrub_motion_event> motion;
                const bool have_history =
                    transport().snapshot_scrub_motion(motion);

                if (have_history && !motion.empty()) {
                    using scrub_clock = std::chrono::steady_clock;
                    const auto now_clock = scrub_clock::now();
                    const auto lag = std::chrono::duration_cast<scrub_clock::duration>(
                        std::chrono::duration<double>(kScrubTrajectoryLagSeconds));
                    const auto window_end = now_clock - lag;
                    const auto window_start = window_end -
                        std::chrono::duration_cast<scrub_clock::duration>(
                            std::chrono::duration<double>(chunk_seconds));

                    auto position_at = [&](scrub_clock::time_point when,
                                           double& position,
                                           bool& moving) {
                        moving = false;
                        if (motion.empty()) {
                            position = ts.position_seconds;
                            return;
                        }

                        if (when <= motion.front().when) {
                            position = motion.front().position_seconds;
                            return;
                        }

                        for (size_t i = 1; i < motion.size(); ++i) {
                            if (when <= motion[i].when) {
                                const auto& a = motion[i - 1];
                                const auto& b = motion[i];
                                const double dt = std::chrono::duration<double>(
                                    b.when - a.when).count();
                                if (dt <= 1.0e-9) {
                                    position = b.position_seconds;
                                    moving = std::abs(
                                        b.position_seconds - a.position_seconds) >
                                        move_epsilon;
                                    return;
                                }
                                const double elapsed = std::chrono::duration<double>(
                                    when - a.when).count();
                                const double alpha = std::clamp(
                                    elapsed / dt, 0.0, 1.0);
                                position =
                                    a.position_seconds +
                                    (b.position_seconds - a.position_seconds) * alpha;
                                moving = std::abs(
                                    b.position_seconds - a.position_seconds) >
                                    move_epsilon;
                                return;
                            }
                        }

                        const auto& last = motion.back();
                        position = last.position_seconds;
                        const double age = std::chrono::duration<double>(
                            when - last.when).count();
                        if (age <= 0.0 ||
                            age > kScrubTrajectoryExtrapolateSeconds ||
                            motion.size() < 2) {
                            return;
                        }

                        // Estimate the newest speed over at least ~8 ms whenever
                        // possible. This avoids treating a single 1 ms mouse packet
                        // as a 20x-24x platter impulse.
                        size_t prev = motion.size() - 2;
                        while (prev > 0) {
                            const double span = std::chrono::duration<double>(
                                last.when - motion[prev].when).count();
                            if (span >= kScrubTrajectorySliceSeconds) break;
                            --prev;
                        }
                        const double dt = std::chrono::duration<double>(
                            last.when - motion[prev].when).count();
                        if (dt <= 1.0e-9) return;

                        double velocity =
                            (last.position_seconds -
                             motion[prev].position_seconds) / dt;
                        velocity = std::clamp(
                            velocity,
                            -kScrubMaxSourceRate,
                            kScrubMaxSourceRate);
                        position = (std::max)(
                            0.0,
                            last.position_seconds + velocity * age);
                        moving = std::abs(velocity) > 1.0e-4;
                    };

                    std::vector<float> preview(
                        frames * kCacheChannels, 0.0f);
                    const size_t slice_frames = (std::max)(
                        static_cast<size_t>(1),
                        static_cast<size_t>(std::llround(
                            kScrubTrajectorySliceSeconds *
                            static_cast<double>(rate))));

                    bool any_audio = false;
                    double newest_rate = 0.0;
                    double final_position = ts.position_seconds;
                    size_t rendered_frames = 0;

                    while (rendered_frames < frames) {
                        const size_t count = (std::min)(
                            slice_frames, frames - rendered_frames);
                        const double slice_seconds =
                            static_cast<double>(count) /
                            static_cast<double>(rate);

                        const auto t0 = window_start +
                            std::chrono::duration_cast<scrub_clock::duration>(
                                std::chrono::duration<double>(
                                    static_cast<double>(rendered_frames) /
                                    static_cast<double>(rate)));
                        const auto t1 = t0 +
                            std::chrono::duration_cast<scrub_clock::duration>(
                                std::chrono::duration<double>(slice_seconds));

                        double p0 = ts.position_seconds;
                        double p1 = ts.position_seconds;
                        bool moving0 = false;
                        bool moving1 = false;
                        position_at(t0, p0, moving0);
                        position_at(t1, p1, moving1);
                        p0 = (std::max)(0.0, p0);
                        p1 = (std::max)(0.0, p1);
                        final_position = p1;

                        const double delta = p1 - p0;
                        double local_rate =
                            slice_seconds > 0.0 ? delta / slice_seconds : 0.0;

                        // The 8 ms wall-clock slice itself is the jitter filter.
                        // Keep only a very high safety clamp; ordinary scratching
                        // should no longer hit it just because two mouse messages
                        // happened 1 ms apart.
                        local_rate = std::clamp(
                            local_rate,
                            -kScrubMaxSourceRate,
                            kScrubMaxSourceRate);

                        if ((moving0 || moving1) &&
                            std::abs(local_rate) > 1.0e-4 &&
                            std::abs(delta) > move_epsilon) {
                            cache_manager().request_transport(
                                p0, local_rate < 0.0);

                            std::vector<float> part;
                            if (cache_manager().render(
                                    mode, p0, rate, count,
                                    part, local_rate) &&
                                part.size() == count * kCacheChannels) {
                                std::copy(
                                    part.begin(), part.end(),
                                    preview.begin() +
                                        static_cast<std::ptrdiff_t>(
                                            rendered_frames * kCacheChannels));
                                any_audio = true;
                                newest_rate = local_rate;
                            }
                        }

                        rendered_frames += count;
                    }

                    if (any_audio) {
                        write_preview(preview);
                        g_dbg_last_source_rate.store(
                            newest_rate, std::memory_order_relaxed);
                    } else {
                        write_silence();
                    }

                    // Debug/render position follows the trajectory time that was
                    // actually synthesized, not an arbitrarily old DSP snapshot.
                    transport().complete_scrub(final_position);
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

            if (ts.state == stem_transport_reverse) {
                m_scrubRateValid = false;
                m_scrubPreviousRate = 0.0;
                cache_manager().request_transport(ts.render_seconds, true);
                std::vector<float> preview;
                if (cache_manager().render(
                        mode, ts.render_seconds, rate, frames, preview, -1.0) &&
                    preview.size() == frames * kCacheChannels) {
                    write_preview(preview);
                    transport().advance_reverse(chunk_seconds);
                } else {
                    // Hold the reverse cursor until the selected rendition exists.
                    write_silence();
                }
                m_position_seconds += chunk_seconds;
                m_using_stem = false;
                return true;
            }

            if (ts.state == stem_transport_release_wait) {
                m_scrubRateValid = false;
                m_scrubPreviousRate = 0.0;
                if (mode == stemmode::mode::original) {
                    transport().finish_release();
                    m_position_seconds += chunk_seconds;
                    m_using_stem = false;
                    return true;
                }

                cache_manager().request_transport(m_position_seconds, false);
                std::vector<float> preview;
                if (cache_manager().render(
                        mode, m_position_seconds, rate, frames, preview, 1.0) &&
                    preview.size() == frames * kCacheChannels) {
                    write_preview(preview);
                    transport().finish_release();
                    m_using_stem = true;
                } else {
                    // Stem-safe release: wait silently rather than leaking Original.
                    write_silence();
                    m_using_stem = false;
                }
                m_position_seconds += chunk_seconds;
                return true;
            }
        }

        m_transportTailValid = false;
        m_scrubRateValid = false;
        m_scrubPreviousRate = 0.0;

        // V26: optional track-start pre-cache.
        //
        // Only a brand-new track at 0:00 waits. Seeking deliberately does
        // not enter this path, preserving V24's immediate seek response.
        if (!m_precache_handled &&
            mode !=
                stemmode::mode::original &&
            stem_precache::enabled() &&
            cache_manager().
                is_track_start_generation(
                    m_generation)) {

            cache_manager().ensure_ahead(0.0);

            bool ready = false;

            try {
                ready =
                    cache_manager().
                        wait_until_ready(
                            m_generation,
                            0.0,
                            15000);
            }
            catch (...) {
                // Never let a pre-cache coordination failure propagate into
                // foobar's DSP callback.
                ready = false;
            }

            // If ready, suppress the usual 50 ms original-to-stem fade:
            // the first audible sample should already be the selected stem.
            if (ready) {
                m_using_stem = true;
            }

            m_precache_handled = true;
        }

        if (mode ==
            stemmode::mode::original) {

            m_precache_handled = true;

            m_position_seconds +=
                static_cast<double>(
                    frames) /
                static_cast<double>(
                    rate);

            m_using_stem = false;
            return true;
        }

        // Keep the background reader fed. This call never waits.
        cache_manager().ensure_ahead(
            m_position_seconds);

        std::vector<float> rendered;

        const bool have_cache =
            cache_manager().render(
                mode,
                m_position_seconds,
                rate,
                frames,
                rendered);

        const audio_sample* original =
            chunk->get_data();

        if (!have_cache ||
            rendered.size() !=
                frames *
                kCacheChannels) {

            // Critical V23 behavior:
            // NEVER stall foobar waiting for Spleeter.
            // After a seek we temporarily play the original mix until
            // the position-indexed cache catches up.
            m_position_seconds +=
                static_cast<double>(
                    frames) /
                static_cast<double>(
                    rate);

            m_using_stem = false;
            return true;
        }

        std::vector<audio_sample> output(
            rendered.size());

        const size_t fade_frames =
            static_cast<size_t>(
                kSwitchFadeSeconds *
                static_cast<double>(
                    rate));

        for (size_t f = 0;
             f < frames;
             ++f) {

            float mix = 1.0f;

            if (!m_using_stem &&
                fade_frames > 0 &&
                f < fade_frames) {

                mix =
                    static_cast<float>(
                        f + 1) /
                    static_cast<float>(
                        fade_frames);
            }

            for (unsigned ch = 0;
                 ch < kCacheChannels;
                 ++ch) {

                const size_t i =
                    f *
                        kCacheChannels +
                    ch;

                const float stem_sample =
                    rendered[i];

                const float original_sample =
                    static_cast<float>(
                        original[i]);

                const float v =
                    original_sample *
                        (1.0f - mix) +
                    stem_sample *
                        mix;

                output[i] =
                    static_cast<audio_sample>(
                        v);
            }
        }

        chunk->set_data(
            output.data(),
            frames,
            channels,
            rate,
            chunk->get_channel_config());

        m_using_stem = true;

        m_position_seconds +=
            static_cast<double>(
                frames) /
            static_cast<double>(
                rate);

        return true;
    }

    void on_endofplayback(
        abort_callback&) override {

        reset_local();
    }

    void on_endoftrack(
        abort_callback&) override {

        reset_local();
    }

    void flush() override {
        // Do not stop/join the background cache worker here.
        // A seek callback has already changed its generation and target.
        // We only reset this DSP instance's local timeline.
        m_have_position = false;
        m_using_stem = false;
        m_precache_handled = false;
        m_transportTailValid = false;
        m_scrubRateValid = false;
        m_scrubPreviousRate = 0.0;
    }

    double get_latency() override {
        // V23 deliberately adds no DSP pipeline latency.
        return 0.0;
    }

    bool need_track_change_mark() override {
        return true;
    }

private:
    void reset_local() {
        m_have_position = false;
        m_using_stem = false;
        m_precache_handled = false;
        m_position_seconds = 0.0;
        m_generation = 0;
        m_transportTailValid = false;
        m_transportTail[0] = 0;
        m_transportTail[1] = 0;
        m_scrubRateValid = false;
        m_scrubPreviousRate = 0.0;
    }

    bool m_have_position = false;
    bool m_using_stem = false;
    bool m_precache_handled = false;

    uint64_t m_generation = 0;
    double m_position_seconds = 0.0;

    audio_sample m_transportTail[kCacheChannels] = {};
    bool m_transportTailValid = false;
    bool m_scrubRateValid = false;
    double m_scrubPreviousRate = 0.0;
};

static dsp_factory_t<stem_dsp>
    g_stem_dsp_factory;

} // namespace
