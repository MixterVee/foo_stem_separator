#include <foobar2000/SDK/foobar2000.h>

#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "onnx_stem_engine.h"
#include "stem_mode.h"

namespace {

constexpr double kWindowSeconds = 4.0;
constexpr double kOverlapSeconds = 1.5;

constexpr unsigned kOnnxRate = 44100;
constexpr unsigned kChannels = 2;

static size_t frames_for(double seconds, unsigned rate) {
    return static_cast<size_t>(
        seconds * static_cast<double>(rate) + 0.5);
}

static std::vector<float> resample_stereo_linear(
    const std::vector<float>& input,
    size_t input_frames,
    unsigned input_rate,
    unsigned output_rate,
    size_t output_frames) {

    std::vector<float> output(
        output_frames * kChannels,
        0.0f);

    if (input_frames == 0 || output_frames == 0) {
        return output;
    }

    if (input_rate == output_rate &&
        input_frames == output_frames) {
        return input;
    }

    if (input_frames == 1) {
        for (size_t f = 0; f < output_frames; ++f) {
            output[f * 2] = input[0];
            output[f * 2 + 1] = input[1];
        }
        return output;
    }

    const double ratio =
        static_cast<double>(input_rate) /
        static_cast<double>(output_rate);

    for (size_t out_f = 0; out_f < output_frames; ++out_f) {
        double source_pos =
            static_cast<double>(out_f) * ratio;

        size_t i0 =
            static_cast<size_t>(source_pos);

        if (i0 >= input_frames - 1) {
            i0 = input_frames - 1;
            output[out_f * 2] =
                input[i0 * 2];
            output[out_f * 2 + 1] =
                input[i0 * 2 + 1];
            continue;
        }

        const size_t i1 = i0 + 1;
        const float frac =
            static_cast<float>(
                source_pos -
                static_cast<double>(i0));

        const float inv = 1.0f - frac;

        output[out_f * 2] =
            input[i0 * 2] * inv +
            input[i1 * 2] * frac;

        output[out_f * 2 + 1] =
            input[i0 * 2 + 1] * inv +
            input[i1 * 2 + 1] * frac;
    }

    return output;
}

struct separated_segment {
    std::vector<float> original;
    std::vector<float> vocals;
    std::vector<float> instrumental;

    std::vector<float> original_tail;
    std::vector<float> vocals_tail;
    std::vector<float> instrumental_tail;
};

class separator_worker {
public:
    separator_worker() {
        m_thread = std::thread([this]() { run(); });
    }

    ~separator_worker() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }

        m_job_cv.notify_all();
        m_ready_cv.notify_all();

        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    void submit(
        std::vector<float> source_window,
        unsigned source_rate) {

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            m_jobs.emplace_back(job{
                m_generation,
                source_rate,
                std::move(source_window)
            });
        }

        m_job_cv.notify_one();
    }

    bool try_pop(separated_segment& out) {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_ready.empty()) {
            return false;
        }

        out = std::move(m_ready.front());
        m_ready.pop_front();
        return true;
    }

    bool wait_pop(separated_segment& out) {
        std::unique_lock<std::mutex> lock(m_mutex);

        m_ready_cv.wait(
            lock,
            [this]() {
                return m_stop || !m_ready.empty();
            });

        if (m_ready.empty()) {
            return false;
        }

        out = std::move(m_ready.front());
        m_ready.pop_front();
        return true;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(m_mutex);

        ++m_generation;

        m_jobs.clear();
        m_ready.clear();

        m_prev_original_tail.clear();
        m_prev_vocals_tail.clear();
        m_prev_instrumental_tail.clear();

        m_have_previous = false;
        m_previous_rate = 0;
    }

private:
    struct job {
        uint64_t generation = 0;
        unsigned source_rate = 0;
        std::vector<float> samples;
    };

    static void split_hop_and_tail(
        const std::vector<float>& current,
        unsigned rate,
        std::vector<float>& previous_tail,
        bool have_previous,
        std::vector<float>& hop,
        std::vector<float>& tail) {

        const size_t win_frames =
            frames_for(kWindowSeconds, rate);

        const size_t ov_frames =
            frames_for(kOverlapSeconds, rate);

        const size_t hp_frames =
            win_frames - ov_frames;

        hop.clear();
        hop.reserve(hp_frames * kChannels);

        if (!have_previous) {
            hop.insert(
                hop.end(),
                current.begin(),
                current.begin() +
                    static_cast<std::ptrdiff_t>(
                        hp_frames * kChannels));
        }
        else {
            constexpr double kHalfPi =
                1.57079632679489661923;

            for (size_t f = 0; f < ov_frames; ++f) {
                const double t =
                    ov_frames > 1
                        ? static_cast<double>(f) /
                            static_cast<double>(ov_frames - 1)
                        : 1.0;

                const double c =
                    std::cos(kHalfPi * t);

                const double s =
                    std::sin(kHalfPi * t);

                const float a =
                    static_cast<float>(c * c);

                const float b =
                    static_cast<float>(s * s);

                for (size_t ch = 0; ch < kChannels; ++ch) {
                    const size_t i =
                        f * kChannels + ch;

                    hop.push_back(
                        previous_tail[i] * a +
                        current[i] * b);
                }
            }

            const size_t start =
                ov_frames * kChannels;

            const size_t end =
                hp_frames * kChannels;

            hop.insert(
                hop.end(),
                current.begin() +
                    static_cast<std::ptrdiff_t>(start),
                current.begin() +
                    static_cast<std::ptrdiff_t>(end));
        }

        const size_t tail_start =
            hp_frames * kChannels;

        const size_t tail_end =
            win_frames * kChannels;

        tail.assign(
            current.begin() +
                static_cast<std::ptrdiff_t>(tail_start),
            current.begin() +
                static_cast<std::ptrdiff_t>(tail_end));

        previous_tail = tail;
    }

    void run() {
        onnxstem::engine engine;

        for (;;) {
            job current_job;

            {
                std::unique_lock<std::mutex> lock(m_mutex);

                m_job_cv.wait(
                    lock,
                    [this]() {
                        return m_stop || !m_jobs.empty();
                    });

                if (m_stop) {
                    return;
                }

                current_job =
                    std::move(m_jobs.front());

                m_jobs.pop_front();
            }

            const unsigned source_rate =
                current_job.source_rate;

            const size_t source_window_frames =
                frames_for(
                    kWindowSeconds,
                    source_rate);

            const size_t onnx_window_frames =
                frames_for(
                    kWindowSeconds,
                    kOnnxRate);

            std::vector<float> onnx_input =
                resample_stereo_linear(
                    current_job.samples,
                    source_window_frames,
                    source_rate,
                    kOnnxRate,
                    onnx_window_frames);

            std::vector<float> onnx_vocals;
            std::vector<float> onnx_instrumental;

            const bool ok =
                engine.process_both(
                    onnx_input.data(),
                    onnx_window_frames,
                    kChannels,
                    kOnnxRate,
                    onnx_vocals,
                    onnx_instrumental);

            std::vector<float> source_vocals;
            std::vector<float> source_instrumental;

            if (ok) {
                source_vocals =
                    resample_stereo_linear(
                        onnx_vocals,
                        onnx_window_frames,
                        kOnnxRate,
                        source_rate,
                        source_window_frames);

                source_instrumental =
                    resample_stereo_linear(
                        onnx_instrumental,
                        onnx_window_frames,
                        kOnnxRate,
                        source_rate,
                        source_window_frames);
            }
            else {
                source_vocals =
                    current_job.samples;

                source_instrumental =
                    current_job.samples;
            }

            separated_segment segment;

            {
                std::lock_guard<std::mutex> lock(m_mutex);

                if (current_job.generation !=
                    m_generation) {
                    continue;
                }

                // Sample-rate changes start a fresh overlap chain.
                if (m_previous_rate != 0 &&
                    m_previous_rate != source_rate) {

                    m_prev_original_tail.clear();
                    m_prev_vocals_tail.clear();
                    m_prev_instrumental_tail.clear();
                    m_have_previous = false;
                }

                const bool had_previous =
                    m_have_previous;

                split_hop_and_tail(
                    current_job.samples,
                    source_rate,
                    m_prev_original_tail,
                    had_previous,
                    segment.original,
                    segment.original_tail);

                split_hop_and_tail(
                    source_vocals,
                    source_rate,
                    m_prev_vocals_tail,
                    had_previous,
                    segment.vocals,
                    segment.vocals_tail);

                split_hop_and_tail(
                    source_instrumental,
                    source_rate,
                    m_prev_instrumental_tail,
                    had_previous,
                    segment.instrumental,
                    segment.instrumental_tail);

                m_have_previous = true;
                m_previous_rate = source_rate;

                m_ready.emplace_back(
                    std::move(segment));
            }

            m_ready_cv.notify_one();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_job_cv;
    std::condition_variable m_ready_cv;

    std::deque<job> m_jobs;
    std::deque<separated_segment> m_ready;

    std::thread m_thread;
    bool m_stop = false;

    uint64_t m_generation = 0;

    bool m_have_previous = false;
    unsigned m_previous_rate = 0;

    std::vector<float> m_prev_original_tail;
    std::vector<float> m_prev_vocals_tail;
    std::vector<float> m_prev_instrumental_tail;
};

class stem_dsp : public dsp_impl_base {
public:
    explicit stem_dsp(dsp_preset const&)
        : m_worker(std::make_unique<separator_worker>()) {}

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

    bool on_chunk(
        audio_chunk* chunk,
        abort_callback&) override {

        if (!chunk || chunk->is_empty()) {
            return false;
        }

        const unsigned channels =
            chunk->get_channels();

        const unsigned rate =
            chunk->get_srate();

        const unsigned chan_config =
            chunk->get_channel_config();

        // V19 supports ordinary stereo sample rates. Mono/multichannel is
        // still bypassed unchanged for this prototype.
        if (channels != kChannels ||
            rate < 8000 ||
            rate > 384000) {

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

        const size_t frames =
            chunk->get_sample_count();

        const size_t values =
            frames * channels;

        const audio_sample* src =
            chunk->get_data();

        const size_t old =
            m_input.size();

        m_input.resize(old + values);

        for (size_t i = 0; i < values; ++i) {
            m_input[old + i] =
                static_cast<float>(src[i]);
        }

        queue_available_windows();

        // V21: prime one full analysis window instead of two.
        // This reduces visible playback delay while the worker still has
        // ample CPU headroom on a fast machine.
        if (!m_started &&
            m_submitted >= 1) {

            separated_segment first;

            if (m_worker->wait_pop(first)) {
                emit_segment(first);
                ++m_emitted;
                m_started = true;
            }
        }

        if (m_started) {
            separated_segment ready;

            while (m_worker->try_pop(ready)) {
                emit_segment(ready);
                ++m_emitted;
            }
        }

        return false;
    }

    void on_endofplayback(
        abort_callback&) override {

        drain_submitted_and_tail();
        reset_state();
    }

    void on_endoftrack(
        abort_callback&) override {

        drain_submitted_and_tail();
        reset_state();
    }

    void flush() override {
        reset_state();
    }

    double get_latency() override {
        // One full analysis window is buffered before the first processed
        // samples are emitted.
        return kWindowSeconds;
    }

    bool need_track_change_mark() override {
        return true;
    }

private:
    void queue_available_windows() {
        const size_t win_frames =
            frames_for(kWindowSeconds, m_rate);

        const size_t ov_frames =
            frames_for(kOverlapSeconds, m_rate);

        const size_t hp_frames =
            win_frames - ov_frames;

        const size_t win_values =
            win_frames * kChannels;

        const size_t hop_values =
            hp_frames * kChannels;

        while (m_input.size() >= win_values) {
            std::vector<float> window(
                m_input.begin(),
                m_input.begin() +
                    static_cast<std::ptrdiff_t>(
                        win_values));

            m_worker->submit(
                std::move(window),
                m_rate);

            ++m_submitted;

            m_input.erase(
                m_input.begin(),
                m_input.begin() +
                    static_cast<std::ptrdiff_t>(
                        hop_values));
        }
    }

    const std::vector<float>& choose(
        const separated_segment& segment,
        bool tail) const {

        switch (stemmode::get()) {
        case stemmode::mode::vocals:
            return tail
                ? segment.vocals_tail
                : segment.vocals;

        case stemmode::mode::instrumental:
            return tail
                ? segment.instrumental_tail
                : segment.instrumental;

        case stemmode::mode::original:
        default:
            return tail
                ? segment.original_tail
                : segment.original;
        }
    }

    void emit_segment(
        const separated_segment& segment) {

        emit_float_audio(
            choose(segment, false));

        m_last_segment = segment;
        m_have_last_segment = true;
    }

    void drain_submitted_and_tail() {
        while (m_emitted < m_submitted) {
            separated_segment ready;

            if (!m_worker->wait_pop(ready)) {
                break;
            }

            emit_segment(ready);
            ++m_emitted;
        }

        const size_t overlap_values =
            m_rate != 0
                ? frames_for(
                    kOverlapSeconds,
                    m_rate) * kChannels
                : 0;

        // Emit the final processed overlap exactly once.
        if (m_have_last_segment) {
            emit_float_audio(
                choose(
                    m_last_segment,
                    true));
        }

        // m_input begins with the same overlap already represented by the
        // final processed tail. Emit only true residual audio after it.
        size_t skip = 0;

        if (m_have_last_segment) {
            skip =
                overlap_values < m_input.size()
                    ? overlap_values
                    : m_input.size();
        }

        if (m_input.size() > skip) {
            std::vector<float> residual(
                m_input.begin() +
                    static_cast<std::ptrdiff_t>(skip),
                m_input.end());

            emit_float_audio(residual);
        }
    }

    void emit_float_audio(
        const std::vector<float>& samples) {

        if (samples.empty() ||
            m_rate == 0) {
            return;
        }

        const size_t frames =
            samples.size() / kChannels;

        std::vector<audio_sample> fb(
            samples.size());

        for (size_t i = 0;
             i < samples.size();
             ++i) {

            fb[i] =
                static_cast<audio_sample>(
                    samples[i]);
        }

        audio_chunk* out =
            insert_chunk(fb.size());

        out->set_data(
            fb.data(),
            frames,
            kChannels,
            m_rate,
            m_channel_config);
    }

    void reset_state() {
        // V21 SEEK-SAFETY:
        // Do not merely invalidate queued jobs. A seek/flush must guarantee
        // that no worker from the old timeline can later publish audio.
        //
        // Destroying separator_worker signals stop and joins its thread,
        // including any inference currently in progress. Only after that
        // thread is gone do we create a clean worker for the new timeline.
        m_worker.reset();
        m_worker = std::make_unique<separator_worker>();

        m_input.clear();

        m_submitted = 0;
        m_emitted = 0;
        m_started = false;

        m_have_last_segment = false;
        m_last_segment = separated_segment{};

        m_rate = 0;
        m_channels = 0;
        m_channel_config = 0;
    }

    std::unique_ptr<separator_worker> m_worker;
    std::vector<float> m_input;

    size_t m_submitted = 0;
    size_t m_emitted = 0;

    bool m_started = false;

    bool m_have_last_segment = false;
    separated_segment m_last_segment;

    unsigned m_rate = 0;
    unsigned m_channels = 0;
    unsigned m_channel_config = 0;
};

static dsp_factory_t<stem_dsp>
    g_stem_dsp_factory;

} // namespace
