#include <foobar2000/SDK/foobar2000.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "onnx_stem_engine.h"
#include "stem_mode.h"

namespace {

constexpr double kWindowSeconds = 2.0;
constexpr double kOverlapSeconds = 0.5;
constexpr unsigned kSupportedRate = 44100;
constexpr unsigned kSupportedChannels = 2;

struct processed_segment {
    std::vector<float> original;
    std::vector<float> vocals;
    std::vector<float> instrumental;
};

class async_separator {
public:
    async_separator() {
        m_thread = std::thread([this]() { worker_main(); });
    }

    ~async_separator() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();

        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    void submit(std::vector<float> window) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // Do not let an accidental decoder burst create an unbounded queue.
            if (m_jobs.size() >= 6) {
                m_jobs.pop_front();
            }
            m_jobs.emplace_back(std::move(window));
        }
        m_cv.notify_one();
    }

    bool try_pop(processed_segment& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_ready.empty()) return false;

        out = std::move(m_ready.front());
        m_ready.pop_front();
        return true;
    }

    bool wait_pop(
        processed_segment& out,
        std::chrono::milliseconds timeout) {

        std::unique_lock<std::mutex> lock(m_mutex);

        if (!m_ready_cv.wait_for(
                lock,
                timeout,
                [this]() {
                    return m_stop || !m_ready.empty();
                })) {
            return false;
        }

        if (m_ready.empty()) return false;

        out = std::move(m_ready.front());
        m_ready.pop_front();
        return true;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.clear();
        m_ready.clear();
        m_have_previous = false;
        m_prev_original_tail.clear();
        m_prev_vocals_tail.clear();
        m_prev_instrumental_tail.clear();
        ++m_generation;
    }

private:
    static std::vector<float> make_segment(
        const std::vector<float>& current,
        std::vector<float>& previous_tail,
        bool& have_previous,
        size_t window_frames,
        size_t hop_frames,
        size_t overlap_frames) {

        const size_t channels = 2;
        std::vector<float> out;
        out.reserve(hop_frames * channels);

        if (!have_previous) {
            out.insert(
                out.end(),
                current.begin(),
                current.begin() +
                    static_cast<std::ptrdiff_t>(
                        hop_frames * channels));
        }
        else {
            // Crossfade the previous window's held tail with this window's
            // first overlap region.
            for (size_t f = 0; f < overlap_frames; ++f) {
                const float t =
                    overlap_frames > 1
                        ? static_cast<float>(f) /
                            static_cast<float>(overlap_frames - 1)
                        : 1.0f;

                for (size_t c = 0; c < channels; ++c) {
                    const size_t i = f * channels + c;
                    const float a = previous_tail[i];
                    const float b = current[i];
                    out.push_back(a * (1.0f - t) + b * t);
                }
            }

            const size_t middle_start =
                overlap_frames * channels;
            const size_t middle_end =
                hop_frames * channels;

            out.insert(
                out.end(),
                current.begin() +
                    static_cast<std::ptrdiff_t>(middle_start),
                current.begin() +
                    static_cast<std::ptrdiff_t>(middle_end));
        }

        const size_t tail_start =
            hop_frames * channels;
        const size_t tail_end =
            window_frames * channels;

        previous_tail.assign(
            current.begin() +
                static_cast<std::ptrdiff_t>(tail_start),
            current.begin() +
                static_cast<std::ptrdiff_t>(tail_end));

        return out;
    }

    void worker_main() {
        onnxstem::engine engine;

        const size_t window_frames =
            static_cast<size_t>(
                kSupportedRate * kWindowSeconds);

        const size_t overlap_frames =
            static_cast<size_t>(
                kSupportedRate * kOverlapSeconds);

        const size_t hop_frames =
            window_frames - overlap_frames;

        for (;;) {
            std::vector<float> job;
            uint64_t generation = 0;

            {
                std::unique_lock<std::mutex> lock(m_mutex);

                m_cv.wait(
                    lock,
                    [this]() {
                        return m_stop || !m_jobs.empty();
                    });

                if (m_stop) return;

                job = std::move(m_jobs.front());
                m_jobs.pop_front();
                generation = m_generation;
            }

            std::vector<float> vocals;
            std::vector<float> instrumental;

            const bool ok = engine.process_both(
                job.data(),
                window_frames,
                kSupportedChannels,
                kSupportedRate,
                vocals,
                instrumental);

            if (!ok) {
                vocals = job;
                instrumental = job;
            }

            std::lock_guard<std::mutex> lock(m_mutex);

            // Discard stale work after seek/flush/track change.
            if (generation != m_generation) {
                continue;
            }

            processed_segment segment;

            segment.original = make_segment(
                job,
                m_prev_original_tail,
                m_have_previous,
                window_frames,
                hop_frames,
                overlap_frames);

            // Use the same "have previous" state for all three streams.
            const bool had_previous = m_have_previous;

            bool vocals_have_previous = had_previous;
            segment.vocals = make_segment(
                vocals,
                m_prev_vocals_tail,
                vocals_have_previous,
                window_frames,
                hop_frames,
                overlap_frames);

            bool instrumental_have_previous = had_previous;
            segment.instrumental = make_segment(
                instrumental,
                m_prev_instrumental_tail,
                instrumental_have_previous,
                window_frames,
                hop_frames,
                overlap_frames);

            m_have_previous = true;

            m_ready.emplace_back(std::move(segment));

            if (m_ready.size() > 8) {
                m_ready.pop_front();
            }

            m_ready_cv.notify_all();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_ready_cv;
    std::deque<std::vector<float>> m_jobs;
    std::deque<processed_segment> m_ready;
    std::thread m_thread;
    bool m_stop = false;

    uint64_t m_generation = 0;
    bool m_have_previous = false;

    std::vector<float> m_prev_original_tail;
    std::vector<float> m_prev_vocals_tail;
    std::vector<float> m_prev_instrumental_tail;
};

class stem_dsp : public dsp_impl_base {
public:
    explicit stem_dsp(dsp_preset const&) {}

    static GUID g_get_guid() {
        static const GUID guid =
            {0x1fd7bdf4,0xa0e1,0x4b3e,{0x9b,0x5a,0x45,0xe6,0xba,0x88,0x34,0x22}};
        return guid;
    }

    static void g_get_name(pfc::string_base& out) {
        out = "Stem Separator (ONNX)";
    }

    static bool g_get_default_preset(dsp_preset& out) {
        dsp_preset_builder builder;
        builder.finish(g_get_guid(), out);
        return true;
    }

    static bool g_have_config_popup() {
        return false;
    }

    static void g_show_config_popup(
        const dsp_preset&,
        HWND,
        dsp_preset_edit_callback&) {}

    bool on_chunk(audio_chunk* chunk, abort_callback&) override {
        if (!chunk || chunk->is_empty()) return false;

        const unsigned channels = chunk->get_channels();
        const unsigned rate = chunk->get_srate();
        const unsigned chan_config = chunk->get_channel_config();

        if (channels != kSupportedChannels ||
            rate != kSupportedRate) {
            reset_state();
            return true;
        }

        if (m_rate != 0 &&
            (m_rate != rate ||
             m_channels != channels ||
             m_channel_config != chan_config)) {
            reset_state();
        }

        m_rate = rate;
        m_channels = channels;
        m_channel_config = chan_config;

        const size_t frames = chunk->get_sample_count();
        const size_t values = frames * channels;
        const audio_sample* src = chunk->get_data();

        const size_t old = m_input.size();
        m_input.resize(old + values);

        for (size_t i = 0; i < values; ++i) {
            m_input[old + i] =
                static_cast<float>(src[i]);
        }

        const size_t window_frames =
            static_cast<size_t>(
                kSupportedRate * kWindowSeconds);

        const size_t overlap_frames =
            static_cast<size_t>(
                kSupportedRate * kOverlapSeconds);

        const size_t hop_frames =
            window_frames - overlap_frames;

        const size_t window_values =
            window_frames * channels;

        const size_t hop_values =
            hop_frames * channels;

        bool submitted = false;

        while (m_input.size() >= window_values) {
            std::vector<float> window(
                m_input.begin(),
                m_input.begin() +
                    static_cast<std::ptrdiff_t>(window_values));

            m_worker.submit(std::move(window));

            // Retain the overlap for the next analysis window.
            m_input.erase(
                m_input.begin(),
                m_input.begin() +
                    static_cast<std::ptrdiff_t>(hop_values));

            submitted = true;
        }

        emit_ready_segments();

        // At startup, if we just submitted the first window and no processed
        // audio is ready yet, wait briefly. On the user's measured machine,
        // 2 seconds of Spleeter audio should process far below this timeout.
        if (submitted && !m_started) {
            processed_segment first;

            if (m_worker.wait_pop(
                    first,
                    std::chrono::milliseconds(1000))) {
                emit_segment(first);
                m_started = true;
            }
        }

        return false;
    }

    void on_endofplayback(abort_callback&) override {
        emit_ready_segments();
        reset_state();
    }

    void on_endoftrack(abort_callback&) override {
        emit_ready_segments();
        reset_state();
    }

    void flush() override {
        reset_state();
    }

    double get_latency() override {
        // Approximate analysis buffering latency.
        return kWindowSeconds;
    }

    bool need_track_change_mark() override {
        return true;
    }

private:
    void reset_state() {
        m_input.clear();
        m_worker.reset();
        m_started = false;
        m_rate = 0;
        m_channels = 0;
        m_channel_config = 0;
    }

    void emit_ready_segments() {
        processed_segment segment;

        while (m_worker.try_pop(segment)) {
            emit_segment(segment);
            m_started = true;
        }
    }

    void emit_segment(const processed_segment& segment) {
        const std::vector<float>* selected = &segment.original;

        switch (stemmode::get()) {
        case stemmode::mode::vocals:
            selected = &segment.vocals;
            break;

        case stemmode::mode::instrumental:
            selected = &segment.instrumental;
            break;

        case stemmode::mode::original:
        default:
            selected = &segment.original;
            break;
        }

        if (!selected || selected->empty()) {
            return;
        }

        const size_t frames =
            selected->size() / kSupportedChannels;

        std::vector<audio_sample> fb(
            selected->size());

        for (size_t i = 0; i < selected->size(); ++i) {
            fb[i] =
                static_cast<audio_sample>((*selected)[i]);
        }

        audio_chunk* out =
            insert_chunk(fb.size());

        out->set_data(
            fb.data(),
            frames,
            kSupportedChannels,
            kSupportedRate,
            m_channel_config);
    }

    std::vector<float> m_input;
    async_separator m_worker;

    bool m_started = false;
    unsigned m_rate = 0;
    unsigned m_channels = 0;
    unsigned m_channel_config = 0;
};

static dsp_factory_t<stem_dsp> g_stem_dsp_factory;

} // namespace
