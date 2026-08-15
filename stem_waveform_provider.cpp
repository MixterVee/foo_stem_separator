#include <foobar2000/SDK/foobar2000.h>

#include "onnx_stem_engine.h"
#include "stem_mode.h"
#include "stem_waveform_provider.h"

#include <mutex>
#include <vector>

#undef FOOGUIDDECL
#define FOOGUIDDECL
FOOGUIDDECL const GUID stem_waveform_provider::class_guid =
{ 0xd8ae9a42, 0x5f1d, 0x4cba, { 0xa9, 0xb7, 0x3c, 0x2e, 0x61, 0xf4, 0xd8, 0x12 } };

namespace {

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
        pfc::array_t<float>& vocals,
        pfc::array_t<float>& instrumental,
        abort_callback& aborter) override {

        aborter.check();
        if (input == nullptr || frames == 0 || channels == 0 || sample_rate == 0) return false;

        std::lock_guard<std::mutex> lock(m_mutex);
        aborter.check();

        if (!m_engine.ready()) return false;

        std::vector<float> outVocals;
        std::vector<float> outInstrumental;
        if (!m_engine.process_both(
                input,
                static_cast<size_t>(frames),
                channels,
                sample_rate,
                outVocals,
                outInstrumental)) {
            return false;
        }

        aborter.check();
        const size_t expected = static_cast<size_t>(frames) * channels;
        if (outVocals.size() != expected || outInstrumental.size() != expected) return false;

        vocals.set_size(expected);
        instrumental.set_size(expected);
        if (expected > 0) {
            memcpy(vocals.get_ptr(), outVocals.data(), expected * sizeof(float));
            memcpy(instrumental.get_ptr(), outInstrumental.data(), expected * sizeof(float));
        }
        return true;
    }

private:
    std::mutex m_mutex;
    onnxstem::engine m_engine;
};

static service_factory_single_t<stem_waveform_provider_impl> g_stem_waveform_provider_factory;

} // namespace
