#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <foobar2000/SDK/foobar2000.h>

#include "onnx_stem_engine.h"
#include "stem_mode.h"
#include "stem_waveform_provider.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#undef FOOGUIDDECL
#define FOOGUIDDECL
FOOGUIDDECL const GUID stem_waveform_provider::class_guid =
{ 0xd8ae9a42, 0x5f1d, 0x4cba, { 0xa9, 0xb7, 0x3c, 0x2e, 0x61, 0xf4, 0xd8, 0x12 } };

namespace {

constexpr unsigned kEngineRate = 44100;
constexpr unsigned kEngineChannels = 2;

void convert_to_stereo(
    const float* input,
    size_t frames,
    unsigned channels,
    std::vector<float>& out) {

    out.assign(frames * kEngineChannels, 0.0f);
    if (input == nullptr || frames == 0 || channels == 0) return;

    if (channels == 1) {
        for (size_t i = 0; i < frames; ++i) {
            const float v = input[i];
            out[i * 2] = v;
            out[i * 2 + 1] = v;
        }
        return;
    }

    for (size_t i = 0; i < frames; ++i) {
        const float* frame = input + i * channels;
        out[i * 2] = frame[0];
        out[i * 2 + 1] = frame[1];
    }
}

void resample_stereo_to_count(
    const std::vector<float>& input,
    size_t input_frames,
    size_t output_frames,
    std::vector<float>& out) {

    out.assign(output_frames * kEngineChannels, 0.0f);
    if (input_frames == 0 || output_frames == 0 || input.size() < input_frames * 2) return;

    if (input_frames == 1 || output_frames == 1) {
        const float l = input[0];
        const float r = input[1];
        for (size_t i = 0; i < output_frames; ++i) {
            out[i * 2] = l;
            out[i * 2 + 1] = r;
        }
        return;
    }

    const double scale = static_cast<double>(input_frames - 1) /
        static_cast<double>(output_frames - 1);

    for (size_t i = 0; i < output_frames; ++i) {
        const double source = static_cast<double>(i) * scale;
        size_t i0 = static_cast<size_t>(source);
        size_t i1 = std::min(i0 + 1, input_frames - 1);
        const float frac = static_cast<float>(source - static_cast<double>(i0));

        for (unsigned ch = 0; ch < 2; ++ch) {
            const float a = input[i0 * 2 + ch];
            const float b = input[i1 * 2 + ch];
            out[i * 2 + ch] = a + (b - a) * frac;
        }
    }
}

void stereo_to_layout(
    const std::vector<float>& stereo,
    size_t frames,
    unsigned channels,
    float* out) {

    if (out == nullptr || frames == 0 || channels == 0 || stereo.size() < frames * 2) return;

    for (size_t i = 0; i < frames; ++i) {
        const float l = stereo[i * 2];
        const float r = stereo[i * 2 + 1];
        const float mono = 0.5f * (l + r);
        float* frame = out + i * channels;

        if (channels == 1) {
            frame[0] = mono;
        } else {
            frame[0] = l;
            frame[1] = r;
            for (unsigned ch = 2; ch < channels; ++ch) frame[ch] = mono;
        }
    }
}

class stem_waveform_provider_impl : public stem_waveform_provider {
public:
    int get_mode() override {
        return static_cast<int>(stemmode::get());
    }

    bool process_both(
        const float* input,
        t_size frames,
        unsigned channels,
        unsigned sample_rate,
        float* vocals_out,
        float* instrumental_out,
        t_size output_samples,
        abort_callback& aborter) override {

        aborter.check();
        if (input == nullptr || frames == 0 || channels == 0 || sample_rate == 0 ||
            vocals_out == nullptr || instrumental_out == nullptr) return false;

        const size_t frameCount = static_cast<size_t>(frames);
        const size_t expected = frameCount * static_cast<size_t>(channels);
        if (output_samples < expected) return false;

        std::vector<float> sourceStereo;
        convert_to_stereo(input, frameCount, channels, sourceStereo);

        size_t engineFrames = frameCount;
        std::vector<float> engineInput;
        if (sample_rate == kEngineRate) {
            engineInput = std::move(sourceStereo);
        } else {
            engineFrames = std::max<size_t>(1,
                static_cast<size_t>(std::llround(
                    static_cast<double>(frameCount) *
                    static_cast<double>(kEngineRate) /
                    static_cast<double>(sample_rate))));
            resample_stereo_to_count(sourceStereo, frameCount, engineFrames, engineInput);
        }

        aborter.check();

        std::vector<float> outVocals;
        std::vector<float> outInstrumental;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            aborter.check();
            if (!m_engine.ready()) return false;
            if (!m_engine.process_both(
                    engineInput.data(),
                    engineFrames,
                    kEngineChannels,
                    kEngineRate,
                    outVocals,
                    outInstrumental)) {
                return false;
            }
        }

        aborter.check();
        if (outVocals.size() != engineFrames * 2 ||
            outInstrumental.size() != engineFrames * 2) return false;

        std::vector<float> vocalsStereo;
        std::vector<float> instrumentalStereo;
        if (engineFrames == frameCount) {
            vocalsStereo = std::move(outVocals);
            instrumentalStereo = std::move(outInstrumental);
        } else {
            resample_stereo_to_count(outVocals, engineFrames, frameCount, vocalsStereo);
            resample_stereo_to_count(outInstrumental, engineFrames, frameCount, instrumentalStereo);
        }

        stereo_to_layout(vocalsStereo, frameCount, channels, vocals_out);
        stereo_to_layout(instrumentalStereo, frameCount, channels, instrumental_out);
        return true;
    }

private:
    std::mutex m_mutex;
    onnxstem::engine m_engine;
};

static service_factory_single_t<stem_waveform_provider_impl> g_stem_waveform_provider_factory;

} // namespace
