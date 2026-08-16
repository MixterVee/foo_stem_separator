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
constexpr double kSwitchFadeSeconds = 0.050;
constexpr double kCacheHandoffFadeSeconds = 0.080;
constexpr double kDecodeSeekPrerollSeconds = 5.0;
constexpr double kFirstBlockFadeSeconds = 0.005;

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

struct cache_segment {
    uint64_t generation = 0;
    double start_seconds = 0.0;
    double end_seconds = 0.0;

    // Keep the decoded original beside both stems. Normal playback still uses
    // foobar's incoming chunk; this copy is only for jog/reverse preview.
    std::vector<float> original;
    std::vector<float> vocals;
    std::vector<float> instrumental;
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

                for (const auto& seg :
                     m_segments) {

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

        for (const auto& seg :
             m_segments) {

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

        std::lock_guard<std::mutex> lock(
            m_mutex);

        ++m_generation;

        m_track_start_generation =
            m_generation;

        m_path = path;
        m_anchor_seconds = 0.0;

        m_segments.clear();
        m_jobs.clear();
        m_job_pending = false;

        if (!m_path.empty() &&
            stemmode::get() !=
                stemmode::mode::original) {

            queue_job_locked(0.0, true);
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

        if (!m_path.empty() &&
            stemmode::get() !=
                stemmode::mode::original) {

            queue_job_locked(seconds, true);
        }

        m_cv.notify_one();
    }

    void stop() {
        std::lock_guard<std::mutex> lock(
            m_mutex);

        ++m_generation;
        m_track_start_generation = 0;

        m_path.clear();
        m_anchor_seconds = 0.0;

        m_segments.clear();
        m_jobs.clear();
        m_job_pending = false;
    }

    void ensure_ahead(double playback_seconds) {
        if (stemmode::get() ==
            stemmode::mode::original) {
            return;
        }

        std::lock_guard<std::mutex> lock(
            m_mutex);

        if (m_path.empty() ||
            m_job_pending) {
            return;
        }

        double cache_end = -1.0;

        if (!m_segments.empty()) {
            cache_end =
                m_segments.back().end_seconds;
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

    bool transport_position_ready(double position_seconds) const {
        const stemmode::mode mode = stemmode::get();
        if (mode == stemmode::mode::original) return true;
        if (position_seconds < 0.0) position_seconds = 0.0;

        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& seg : m_segments) {
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
        const double margin = 0.35;
        const double need_start = reverse
            ? std::max(0.0, position_seconds - margin)
            : position_seconds;
        const double need_end = reverse
            ? position_seconds
            : position_seconds + margin;

        for (const auto& seg : m_segments) {
            if (!segment_has_mode(seg, mode)) continue;
            if (seg.start_seconds <= need_start + 1.0e-6 &&
                seg.end_seconds >= need_end - 1.0e-6) {
                return;
            }
        }

        // Coalesce scrub requests: a slow Spleeter job must never build a queue of
        // obsolete mouse positions. The newest transport target goes to the front.
        for (auto it = m_jobs.begin(); it != m_jobs.end();) {
            if (it->transport_preview) it = m_jobs.erase(it);
            else ++it;
        }

        double start = reverse
            ? std::max(0.0, position_seconds - (kCacheSeconds - 0.5))
            : std::max(0.0, position_seconds - 0.5);

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
        bool reverse = false) {

        if (output_rate == 0 ||
            frames == 0) {
            return false;
        }

        std::vector<cache_segment> snapshot;

        {
            std::lock_guard<std::mutex> lock(
                m_mutex);

            if (m_segments.empty()) {
                return false;
            }

            snapshot.assign(
                m_segments.begin(),
                m_segments.end());
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

            const double direction = reverse ? -1.0 : 1.0;
            const double t =
                start_seconds +
                direction * static_cast<double>(f) * dt;

            if (t < 0.0) return false;

            const cache_segment* first =
                nullptr;

            const cache_segment* second =
                nullptr;

            for (const auto& seg :
                 snapshot) {

                if (!segment_has_mode(seg, mode)) continue;

                if (t >= seg.start_seconds &&
                    t < seg.end_seconds) {

                    if (!first) {
                        first = &seg;
                    }
                    else {
                        second = &seg;
                        break;
                    }
                }
            }

            if (!first) {
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

        return true;
    }

private:
    static bool segment_has_mode(const cache_segment& seg, stemmode::mode mode) {
        if (mode == stemmode::mode::original) return !seg.original.empty();
        if (mode == stemmode::mode::vocals) return !seg.vocals.empty();
        return !seg.instrumental.empty();
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
        double target_seconds) {

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
            kDecodeSeekPrerollSeconds;

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
                kCacheSeconds *
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
                    requested_start_seconds)) {
                return false;
            }
            reached_target = false;
        }

        if (block_start_frame <
            state.fifo_start_frame) {

            if (!reanchor_decoder(
                    state,
                    state.path,
                    requested_start_seconds)) {
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
                (kCacheSeconds -
                 kCacheOverlapSeconds) *
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

            // Engine construction itself can throw. Keeping it inside this
            // top-level exception boundary prevents std::terminate().
            onnxstem::engine engine;

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

                    const bool decoded =
                        decode_exact_block(
                            active_decoder,
                            job.start_seconds,
                            job.force_reanchor,
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

                            separated =
                                engine.process_both(
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
                                m_segments.push_back(std::move(seg));
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
    std::deque<cache_segment> m_segments;
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
};

class transport_controller {
public:
    void set_hold(double seconds) {
        seconds = std::max(0.0, seconds);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state = stem_transport_hold;
            m_position_seconds = seconds;
            m_render_seconds = seconds;
            m_scrub_audible_until = 0;
        }
        cache_manager().request_transport(seconds, false);
    }

    void set_scrub(double seconds) {
        seconds = std::max(0.0, seconds);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state = stem_transport_scrub;
            m_position_seconds = seconds;
            m_render_seconds = seconds;
            m_scrub_audible_until = GetTickCount64() + 150;
        }
        cache_manager().request_transport(seconds, false);
    }

    void set_reverse(double seconds) {
        seconds = std::max(0.0, seconds);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state = stem_transport_reverse;
            m_position_seconds = seconds;
            m_render_seconds = seconds;
            m_scrub_audible_until = 0;
        }
        cache_manager().request_transport(seconds, true);
    }

    void release_transport(double seconds) {
        seconds = std::max(0.0, seconds);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state = stem_transport_release_wait;
            m_position_seconds = seconds;
            m_render_seconds = seconds;
            m_scrub_audible_until = 0;
        }
        cache_manager().request_transport(seconds, false);
    }

    void cancel() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = stem_transport_normal;
        m_scrub_audible_until = 0;
    }

    transport_snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return transport_snapshot{
            m_state, m_position_seconds, m_render_seconds, m_scrub_audible_until};
    }

    double visible_position() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_position_seconds;
    }

    int state() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_state;
    }

    void advance_scrub(double seconds) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state == stem_transport_scrub) {
            m_render_seconds = std::max(0.0, m_render_seconds + seconds);
        }
    }

    void advance_reverse(double seconds) {
        double next = 0.0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_state != stem_transport_reverse) return;
            m_render_seconds = std::max(0.0, m_render_seconds - seconds);
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

        // Transport preview keeps foobar's audio clock running while replacing
        // what is heard. That gives us a real stationary HOLD, audible jog, and
        // 1x reverse without hammering playback_seek on every mouse/key event.
        const transport_snapshot ts = transport().snapshot();
        if (ts.state != stem_transport_normal) {
            const double chunk_seconds =
                static_cast<double>(frames) / static_cast<double>(rate);

            auto write_silence = [&]() {
                std::vector<audio_sample> zeros(frames * kCacheChannels, 0);
                chunk->set_data(
                    zeros.data(), frames, channels, rate, chunk->get_channel_config());
            };

            auto write_preview = [&](const std::vector<float>& rendered) {
                std::vector<audio_sample> output(rendered.size());
                for (size_t i = 0; i < rendered.size(); ++i) {
                    output[i] = static_cast<audio_sample>(rendered[i]);
                }
                chunk->set_data(
                    output.data(), frames, channels, rate, chunk->get_channel_config());
            };

            if (ts.state == stem_transport_hold) {
                write_silence();
                m_position_seconds += chunk_seconds;
                m_using_stem = false;
                return true;
            }

            if (ts.state == stem_transport_scrub) {
                if (GetTickCount64() <= ts.scrub_audible_until) {
                    cache_manager().request_transport(ts.render_seconds, false);
                    std::vector<float> preview;
                    if (cache_manager().render(
                            mode, ts.render_seconds, rate, frames, preview, false) &&
                        preview.size() == frames * kCacheChannels) {
                        write_preview(preview);
                        transport().advance_scrub(chunk_seconds);
                    } else {
                        // Never substitute Original when a selected stem is missing.
                        write_silence();
                    }
                } else {
                    write_silence();
                }
                m_position_seconds += chunk_seconds;
                m_using_stem = false;
                return true;
            }

            if (ts.state == stem_transport_reverse) {
                cache_manager().request_transport(ts.render_seconds, true);
                std::vector<float> preview;
                if (cache_manager().render(
                        mode, ts.render_seconds, rate, frames, preview, true) &&
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
                if (mode == stemmode::mode::original) {
                    transport().finish_release();
                    m_position_seconds += chunk_seconds;
                    m_using_stem = false;
                    return true;
                }

                cache_manager().request_transport(m_position_seconds, false);
                std::vector<float> preview;
                if (cache_manager().render(
                        mode, m_position_seconds, rate, frames, preview, false) &&
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
    }

    bool m_have_position = false;
    bool m_using_stem = false;
    bool m_precache_handled = false;

    uint64_t m_generation = 0;
    double m_position_seconds = 0.0;
};

static dsp_factory_t<stem_dsp>
    g_stem_dsp_factory;

} // namespace
