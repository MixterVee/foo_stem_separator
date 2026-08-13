#include <foobar2000/SDK/foobar2000.h>

#include <algorithm>
#include <vector>

#include "onnx_stem_engine.h"
#include "stem_mode.h"

namespace {

constexpr double kBlockSeconds = 1.0;

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

        if (channels != 2 || rate != 44100) {
            reset_buffer();
            return true;
        }

        if (m_rate != 0 &&
            (m_rate != rate ||
             m_channels != channels ||
             m_channel_config != chan_config)) {
            reset_buffer();
        }

        m_rate = rate;
        m_channels = channels;
        m_channel_config = chan_config;

        const size_t values = chunk->get_sample_count() * channels;
        const audio_sample* src = chunk->get_data();

        const size_t old_size = m_buffer.size();
        m_buffer.resize(old_size + values);

        for (size_t i = 0; i < values; ++i) {
            m_buffer[old_size + i] = static_cast<float>(src[i]);
        }

        const size_t target_frames =
            static_cast<size_t>(rate * kBlockSeconds);
        const size_t target_values = target_frames * channels;

        while (m_buffer.size() >= target_values) {
            std::vector<float> block(
                m_buffer.begin(),
                m_buffer.begin() + target_values);

            m_buffer.erase(
                m_buffer.begin(),
                m_buffer.begin() + target_values);

            emit_block(block, target_frames);
        }

        return false;
    }

    void on_endofplayback(abort_callback&) override {
        drain_tail();
    }

    void on_endoftrack(abort_callback&) override {
        drain_tail();
    }

    void flush() override {
        reset_buffer();
    }

    double get_latency() override {
        if (m_rate == 0 || m_channels == 0) return 0.0;

        const double frames =
            static_cast<double>(m_buffer.size()) /
            static_cast<double>(m_channels);

        return frames / static_cast<double>(m_rate);
    }

    bool need_track_change_mark() override {
        return true;
    }

private:
    void reset_buffer() {
        m_buffer.clear();
        m_rate = 0;
        m_channels = 0;
        m_channel_config = 0;
    }

    void emit_block(
        const std::vector<float>& block,
        size_t frames) {

        std::vector<float> processed;
        const auto mode = stemmode::get();

        if (!m_engine.process_interleaved(
                block.data(),
                frames,
                m_channels,
                m_rate,
                mode,
                processed)) {
            processed = block;
        }

        std::vector<audio_sample> fb_samples(processed.size());

        for (size_t i = 0; i < processed.size(); ++i) {
            fb_samples[i] = static_cast<audio_sample>(processed[i]);
        }

        audio_chunk* out = insert_chunk(fb_samples.size());

        out->set_data(
            fb_samples.data(),
            frames,
            m_channels,
            m_rate,
            m_channel_config);
    }

    void drain_tail() {
        if (m_buffer.empty() ||
            m_rate == 0 ||
            m_channels == 0) {
            reset_buffer();
            return;
        }

        const size_t frames = m_buffer.size() / m_channels;

        if (frames > 0) {
            emit_block(m_buffer, frames);
        }

        reset_buffer();
    }

    std::vector<float> m_buffer;
    unsigned m_rate = 0;
    unsigned m_channels = 0;
    unsigned m_channel_config = 0;

    onnxstem::engine m_engine;
};

static dsp_factory_t<stem_dsp> g_stem_dsp_factory;

} // namespace
