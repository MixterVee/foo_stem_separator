#include "onnx_stem_engine.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <vector>

#include <foobar2000/SDK/foobar2000.h>

namespace fs = std::filesystem;

namespace {

// Minimal ABI declarations copied from sherpa-onnx's public C API.
// We dynamically load the DLL so the component does not link against it.

struct SpleeterConfig {
    const char* vocals;
    const char* accompaniment;
};

struct UvrConfig {
    const char* model;
};

struct ModelConfig {
    SpleeterConfig spleeter;
    UvrConfig uvr;
    int32_t num_threads;
    int32_t debug;
    const char* provider;
};

struct SeparationConfig {
    ModelConfig model;
};

struct Stem {
    float** samples;
    int32_t num_channels;
    int32_t n;
};

struct SeparationOutput {
    const Stem* stems;
    int32_t num_stems;
    int32_t sample_rate;
};

using CreateFn = const void* (__cdecl*)(const SeparationConfig*);
using DestroyFn = void (__cdecl*)(const void*);
using ProcessFn = const SeparationOutput* (__cdecl*)(
    const void*,
    const float* const*,
    int32_t,
    int32_t,
    int32_t);
using DestroyOutputFn = void (__cdecl*)(const SeparationOutput*);

std::wstring utf8_to_wide(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    std::vector<wchar_t> tmp(static_cast<size_t>(n));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, tmp.data(), n);
    std::copy(tmp.begin(), tmp.end() - 1, out.begin());
    return out;
}

std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(
        CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring strip_file_scheme(std::wstring s) {
    const std::wstring prefix = L"file://";
    if (s.rfind(prefix, 0) == 0) s.erase(0, prefix.size());
    return s;
}

} // namespace

namespace onnxstem {

struct engine::api {
    CreateFn create = nullptr;
    DestroyFn destroy = nullptr;
    ProcessFn process = nullptr;
    DestroyOutputFn destroy_output = nullptr;
};

engine::engine() = default;

engine::~engine() {
    shutdown();
}

std::wstring engine::component_directory() const {
    std::wstring path = strip_file_scheme(utf8_to_wide(core_api::get_my_full_path()));
    if (path.empty()) return {};
    return fs::path(path).parent_path().wstring();
}

std::wstring engine::dll_path() const {
    return (fs::path(component_directory()) / L"sherpa-onnx-c-api.dll").wstring();
}

std::wstring engine::vocals_model_path() const {
    return (fs::path(component_directory()) /
        L"models" / L"spleeter" / L"vocals.fp16.onnx").wstring();
}

std::wstring engine::accompaniment_model_path() const {
    return (fs::path(component_directory()) /
        L"models" / L"spleeter" / L"accompaniment.fp16.onnx").wstring();
}

bool engine::ready() {
    return initialize();
}

bool engine::initialize() {
    if (m_separator) return true;
    if (m_attempted) return false;
    m_attempted = true;

    const auto dll = dll_path();
    const auto vocals = vocals_model_path();
    const auto accomp = accompaniment_model_path();

    if (!fs::exists(dll)) {
        m_error = L"Missing sherpa-onnx-c-api.dll next to the component.";
        return false;
    }
    if (!fs::exists(vocals) || !fs::exists(accomp)) {
        m_error = L"Missing Spleeter ONNX model files under models\\spleeter.";
        return false;
    }

    m_module = LoadLibraryExW(
        dll.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

    if (!m_module) {
        m_error = L"Could not load sherpa-onnx-c-api.dll. Windows error " +
            std::to_wstring(GetLastError());
        return false;
    }

    auto api = std::make_unique<engine::api>();
    api->create = reinterpret_cast<CreateFn>(
        GetProcAddress(m_module, "SherpaOnnxCreateOfflineSourceSeparation"));
    api->destroy = reinterpret_cast<DestroyFn>(
        GetProcAddress(m_module, "SherpaOnnxDestroyOfflineSourceSeparation"));
    api->process = reinterpret_cast<ProcessFn>(
        GetProcAddress(m_module, "SherpaOnnxOfflineSourceSeparationProcess"));
    api->destroy_output = reinterpret_cast<DestroyOutputFn>(
        GetProcAddress(m_module, "SherpaOnnxDestroySourceSeparationOutput"));

    if (!api->create || !api->destroy || !api->process || !api->destroy_output) {
        m_error = L"The installed sherpa-onnx C API does not expose source separation.";
        FreeLibrary(m_module);
        m_module = nullptr;
        return false;
    }

    const std::string vocals8 = wide_to_utf8(vocals);
    const std::string accomp8 = wide_to_utf8(accomp);

    SeparationConfig config{};
    config.model.spleeter.vocals = vocals8.c_str();
    config.model.spleeter.accompaniment = accomp8.c_str();
    config.model.num_threads = 2;
    config.model.debug = 0;
    config.model.provider = "cpu";

    m_separator = api->create(&config);
    if (!m_separator) {
        m_error = L"Sherpa-onnx could not create the Spleeter separation engine.";
        FreeLibrary(m_module);
        m_module = nullptr;
        return false;
    }

    m_api = api.release();
    return true;
}

void engine::shutdown() {
    if (m_api && m_separator) {
        m_api->destroy(m_separator);
    }
    m_separator = nullptr;

    delete m_api;
    m_api = nullptr;

    if (m_module) {
        FreeLibrary(m_module);
        m_module = nullptr;
    }
}

bool engine::process_interleaved(
    const float* input,
    size_t frames,
    unsigned channels,
    unsigned sample_rate,
    stemmode::mode mode,
    std::vector<float>& output) {

    output.clear();

    if (!input || frames == 0 || channels == 0) return false;

    if (mode == stemmode::mode::original) {
        output.assign(input, input + frames * channels);
        return true;
    }

    // First prototype: stereo 44.1 kHz only.
    if (channels != 2 || sample_rate != 44100) {
        m_error = L"ONNX prototype currently supports stereo 44.1 kHz audio only.";
        return false;
    }

    if (!initialize()) return false;

    std::vector<float> left(frames), right(frames);
    for (size_t i = 0; i < frames; ++i) {
        left[i] = input[i * 2];
        right[i] = input[i * 2 + 1];
    }

    const float* planes[2] = { left.data(), right.data() };

    const SeparationOutput* result = m_api->process(
        m_separator,
        planes,
        2,
        static_cast<int32_t>(frames),
        static_cast<int32_t>(sample_rate));

    if (!result) {
        m_error = L"Sherpa-onnx source separation returned no output.";
        return false;
    }

    const int stem_index =
        mode == stemmode::mode::vocals ? 0 : 1;

    bool ok = false;

    if (result->num_stems > stem_index &&
        result->sample_rate == static_cast<int32_t>(sample_rate)) {

        const Stem& stem = result->stems[stem_index];

        if (stem.num_channels >= 2 && stem.n > 0) {
            const size_t out_frames =
                std::min<size_t>(frames, static_cast<size_t>(stem.n));

            output.resize(frames * 2, 0.0f);

            for (size_t i = 0; i < out_frames; ++i) {
                output[i * 2] = stem.samples[0][i];
                output[i * 2 + 1] = stem.samples[1][i];
            }
            ok = true;
        }
    }

    m_api->destroy_output(result);

    if (!ok) {
        m_error = L"Sherpa-onnx returned an unexpected Spleeter output format.";
    }

    return ok;
}

} // namespace onnxstem
