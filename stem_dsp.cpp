#include <foobar2000/SDK/foobar2000.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "onnx_stem_engine.h"
#include "stem_mode.h"

namespace {

constexpr double kWindowSeconds = 4.0;
constexpr double kOverlapSeconds = 1.5;
constexpr double kDeclickSeconds = 0.010; // 10 ms
constexpr float kDeclickThreshold = 0.0125f;

constexpr unsigned kRate = 44100;
constexpr unsigned kChannels = 2;

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

    static bool g_have_config_popup() { return false; }

    static void g_show_config_popup(
        const dsp_preset&, HWND, dsp_preset_edit_callback&) {}

    bool on_chunk(audio_chunk* chunk, abort_callback&) override {
        if (!chunk || chunk->is_empty()) return false;

        const unsigned channels = chunk->get_channels();
        const unsigned rate = chunk->get_srate();
        const unsigned chan_config = chunk->get_channel_config();

        if (channels != kChannels || rate != kRate) {
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
            m_input[old + i] = static_cast<float>(src[i]);
        }

        const size_t window_frames =
            static_cast<size_t>(kRate * kWindowSeconds);
        const size_t overlap_frames =
            static_cast<size_t>(kRate * kOverlapSeconds);
        const size_t hop_frames = window_frames - overlap_frames;

        const size_t window_values = window_frames * kChannels;
        const size_t hop_values = hop_frames * kChannels;

        while (m_input.size() >= window_values) {
            std::vector<float> window(
                m_input.begin(),
                m_input.begin() + static_cast<std::ptrdiff_t>(window_values));

            std::vector<float> vocals;
            std::vector<float> instrumental;

            const bool ok = m_engine.process_both(
                window.data(), window_frames, kChannels, kRate,
                vocals, instrumental);

            if (!ok) {
                vocals = window;
                instrumental = window;
            }

            emit_window(
                window, vocals, instrumental,
                window_frames, hop_frames, overlap_frames);

            m_input.erase(
                m_input.begin(),
                m_input.begin() + static_cast<std::ptrdiff_t>(hop_values));
        }

        return false;
    }

    void on_endofplayback(abort_callback&) override { flush_tail(); }
    void on_endoftrack(abort_callback&) override { flush_tail(); }
    void flush() override { reset_state(); }

    double get_latency() override { return kWindowSeconds; }
    bool need_track_change_mark() override { return true; }

private:
    static const std::vector<float>& select_mode(
        const std::vector<float>& original,
        const std::vector<float>& vocals,
        const std::vector<float>& instrumental) {

        switch (stemmode::get()) {
        case stemmode::mode::vocals:
            return vocals;
        case stemmode::mode::instrumental:
            return instrumental;
        case stemmode::mode::original:
        default:
            return original;
        }
    }

    void emit_window(
        const std::vector<float>& original,
        const std::vector<float>& vocals,
        const std::vector<float>& instrumental,
        size_t window_frames,
        size_t hop_frames,
        size_t overlap_frames) {

        const auto& current =
            select_mode(original, vocals, instrumental);

        std::vector<float> segment;
        segment.reserve(hop_frames * kChannels);

        if (!m_have_previous_tail) {
            segment.insert(
                segment.end(),
                current.begin(),
                current.begin() +
                    static_cast<std::ptrdiff_t>(hop_frames * kChannels));
        } else {
            constexpr double kHalfPi =
                1.57079632679489661923;

            for (size_t f = 0; f < overlap_frames; ++f) {
                const double t =
                    overlap_frames > 1
                        ? static_cast<double>(f) /
                            static_cast<double>(overlap_frames - 1)
                        : 1.0;

                const double c = std::cos(kHalfPi * t);
                const double s = std::sin(kHalfPi * t);

                const float gain_prev = static_cast<float>(c * c);
                const float gain_curr = static_cast<float>(s * s);

                for (size_t ch = 0; ch < kChannels; ++ch) {
                    const size_t i = f * kChannels + ch;
                    segment.push_back(
                        m_previous_tail[i] * gain_prev +
                        current[i] * gain_curr);
                }
            }

            const size_t start = overlap_frames * kChannels;
            const size_t end = hop_frames * kChannels;

            segment.insert(
                segment.end(),
                current.begin() + static_cast<std::ptrdiff_t>(start),
                current.begin() + static_cast<std::ptrdiff_t>(end));
        }

        const size_t tail_start = hop_frames * kChannels;
        const size_t tail_end = window_frames * kChannels;

        m_previous_tail.assign(
            current.begin() + static_cast<std::ptrdiff_t>(tail_start),
            current.begin() + static_cast<std::ptrdiff_t>(tail_end));

        m_have_previous_tail = true;

        apply_declick(segment);
        emit_audio(segment);
    }

    void apply_declick(std::vector<float>& samples) {
        if (samples.empty()) return;

        const size_t frames = samples.size() / kChannels;
        if (frames == 0) return;

        if (!m_have_last_output_sample) {
            m_last_left = samples[(frames - 1) * kChannels];
            m_last_right = samples[(frames - 1) * kChannels + 1];
            m_have_last_output_sample = true;
            return;
        }

        const float delta_left = m_last_left - samples[0];
        const float delta_right = m_last_right - samples[1];

        const float abs_left = std::fabs(delta_left);
        const float abs_right = std::fabs(delta_right);

        const float peak_jump =
            abs_left > abs_right
                ? abs_left
                : abs_right;

        // Don't touch clean boundaries. Only correct discontinuities large
        // enough to plausibly create a click.
        if (peak_jump >= kDeclickThreshold) {
            const size_t ramp_frames =
                std::min<size_t>(
                    frames,
                    static_cast<size_t>(kRate * kDeclickSeconds));

            for (size_t f = 0; f < ramp_frames; ++f) {
                const float t =
                    ramp_frames > 1
                        ? static_cast<float>(f) /
                            static_cast<float>(ramp_frames - 1)
                        : 1.0f;

                // Smooth half-cosine correction envelope from 1 to 0.
                const float w =
                    0.5f * (1.0f + std::cos(
                        static_cast<float>(
                            3.14159265358979323846 * t)));

                samples[f * kChannels] += delta_left * w;
                samples[f * kChannels + 1] += delta_right * w;
            }
        }

        m_last_left = samples[(frames - 1) * kChannels];
        m_last_right = samples[(frames - 1) * kChannels + 1];
    }

    void emit_audio(const std::vector<float>& samples) {
        if (samples.empty()) return;

        const size_t frames = samples.size() / kChannels;
        std::vector<audio_sample> fb(samples.size());

        for (size_t i = 0; i < samples.size(); ++i) {
            fb[i] = static_cast<audio_sample>(samples[i]);
        }

        audio_chunk* out = insert_chunk(fb.size());

        out->set_data(
            fb.data(), frames, kChannels, kRate, m_channel_config);
    }

    void flush_tail() {
        const size_t overlap_frames =
            static_cast<size_t>(kRate * kOverlapSeconds);
        const size_t overlap_values = overlap_frames * kChannels;

        if (m_have_previous_tail && !m_previous_tail.empty()) {
            std::vector<float> tail = m_previous_tail;
            apply_declick(tail);
            emit_audio(tail);
        }

        size_t skip = 0;
        if (m_have_previous_tail) {
            skip =
                overlap_values < m_input.size()
                    ? overlap_values
                    : m_input.size();
        }

        if (m_input.size() > skip) {
            std::vector<float> residual(
                m_input.begin() + static_cast<std::ptrdiff_t>(skip),
                m_input.end());

            apply_declick(residual);
            emit_audio(residual);
        }

        reset_state();
    }

    void reset_state() {
        m_input.clear();
        m_previous_tail.clear();
        m_have_previous_tail = false;

        m_have_last_output_sample = false;
        m_last_left = 0.0f;
        m_last_right = 0.0f;

        m_rate = 0;
        m_channels = 0;
        m_channel_config = 0;
    }

    std::vector<float> m_input;
    std::vector<float> m_previous_tail;
    bool m_have_previous_tail = false;

    bool m_have_last_output_sample = false;
    float m_last_left = 0.0f;
    float m_last_right = 0.0f;

    unsigned m_rate = 0;
    unsigned m_channels = 0;
    unsigned m_channel_config = 0;

    onnxstem::engine m_engine;
};

static dsp_factory_t<stem_dsp> g_stem_dsp_factory;

} // namespace
