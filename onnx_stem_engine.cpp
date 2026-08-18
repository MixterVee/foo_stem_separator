#include <foobar2000/SDK/foobar2000.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "onnx_stem_engine.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <vector>

namespace fs = std::filesystem;

namespace {

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

static const GUID g_backend_cfg_guid =
    {0x5c882f31,0x937b,0x4e51,{0xb0,0xc1,0x68,0x55,0x4a,0x6d,0x91,0x73}};

// Adapter 1 is the default for this experiment because it is the NVIDIA
// discrete GPU on the test machine and has already been verified to work.
cfg_int g_backend_cfg(g_backend_cfg_guid, 2);

std::wstring utf8_to_wide(const char* s) {
    if (!s || !*s) return {};

    const int n = MultiByteToWideChar(
        CP_UTF8, 0, s, -1, nullptr, 0);

    if (n <= 1) return {};

    std::vector<wchar_t> temp(static_cast<size_t>(n));
    MultiByteToWideChar(
        CP_UTF8, 0, s, -1, temp.data(), n);

    return std::wstring(temp.data());
}

std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) return {};

    const int n = WideCharToMultiByte(
        CP_UTF8, 0, s.c_str(),
        static_cast<int>(s.size()),
        nullptr, 0, nullptr, nullptr);

    std::string out(static_cast<size_t>(n), '\0');

    WideCharToMultiByte(
        CP_UTF8, 0, s.c_str(),
        static_cast<int>(s.size()),
        out.data(), n, nullptr, nullptr);

    return out;
}

std::wstring strip_file_scheme(std::wstring s) {
    const std::wstring prefix = L"file://";
    if (s.rfind(prefix, 0) == 0) {
        s.erase(0, prefix.size());
    }
    return s;
}

void copy_stem_interleaved(
    const Stem& stem,
    size_t requested_frames,
    std::vector<float>& out) {

    const size_t n = std::min<size_t>(
        requested_frames,
        stem.n > 0 ? static_cast<size_t>(stem.n) : 0);

    out.assign(requested_frames * 2, 0.0f);

    if (stem.num_channels < 2 || !stem.samples) {
        return;
    }

    for (size_t i = 0; i < n; ++i) {
        out[i * 2] = stem.samples[0][i];
        out[i * 2 + 1] = stem.samples[1][i];
    }
}

onnxstem::backend sanitize_backend(int64_t raw) {
    switch (raw) {
    case 0:
        return onnxstem::backend::cpu;
    case 1:
        return onnxstem::backend::directml_adapter0;
    case 2:
        return onnxstem::backend::directml_adapter1;
    default:
        return onnxstem::backend::directml_adapter1;
    }
}

} // namespace

namespace onnxstem {

backend selected_backend() {
    return sanitize_backend(g_backend_cfg.get());
}

void select_backend(backend value) {
    if (value == backend::selected) return;

    switch (value) {
    case backend::cpu:
        g_backend_cfg.set(0);
        break;
    case backend::directml_adapter0:
        g_backend_cfg.set(1);
        break;
    case backend::directml_adapter1:
        g_backend_cfg.set(2);
        break;
    default:
        break;
    }
}

const wchar_t* backend_name(backend value) {
    if (value == backend::selected) {
        value = selected_backend();
    }

    switch (value) {
    case backend::cpu:
        return L"CPU";
    case backend::directml_adapter0:
        return L"DirectML adapter 0";
    case backend::directml_adapter1:
        return L"DirectML adapter 1";
    default:
        return L"Unknown";
    }
}

struct engine::api {
    CreateFn create = nullptr;
    DestroyFn destroy = nullptr;
    ProcessFn process = nullptr;
    DestroyOutputFn destroy_output = nullptr;
};

engine::engine(backend requested)
    : m_requested_backend(requested) {
}

engine::~engine() {
    shutdown();
}

backend engine::desired_backend() const {
    return m_requested_backend == backend::selected
        ? selected_backend()
        : m_requested_backend;
}

std::wstring engine::component_directory() const {
    std::wstring path =
        strip_file_scheme(
            utf8_to_wide(core_api::get_my_full_path()));

    if (path.empty()) return {};
    return fs::path(path).parent_path().wstring();
}

std::wstring engine::dll_path() const {
    return (
        fs::path(component_directory()) /
        L"sherpa-onnx-c-api.dll"
    ).wstring();
}

std::wstring engine::vocals_model_path() const {
    return (
        fs::path(component_directory()) /
        L"models" /
        L"spleeter" /
        L"vocals.fp16.onnx"
    ).wstring();
}

std::wstring engine::accompaniment_model_path() const {
    return (
        fs::path(component_directory()) /
        L"models" /
        L"spleeter" /
        L"accompaniment.fp16.onnx"
    ).wstring();
}

std::wstring engine::directml_config_path(backend value) const {
    const wchar_t* filename =
        value == backend::directml_adapter1
            ? L"directml-device1.config"
            : L"directml-device0.config";

    return (
        fs::path(component_directory()) /
        filename
    ).wstring();
}

bool engine::ready() {
    return initialize();
}

bool engine::initialize() {
    const backend wanted = desired_backend();

    if (m_separator && m_active_backend == wanted) {
        return true;
    }

    // A default-constructed engine follows the user's saved backend selection.
    // If that selection changes while the cache worker is alive, tear down the
    // old session and recreate it on the next separation call without needing
    // a foobar2000 restart.
    if (m_active_backend != wanted) {
        shutdown();
        m_active_backend = wanted;
        m_attempted = false;
        m_error.clear();
    }

    if (m_separator) return true;
    if (m_attempted) return false;

    m_active_backend = wanted;
    m_attempted = true;
    m_error.clear();

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

    std::string provider8 = "cpu";

    if (wanted == backend::directml_adapter0 ||
        wanted == backend::directml_adapter1) {

        const auto config_path = directml_config_path(wanted);
        if (!fs::exists(config_path)) {
            m_error =
                L"Missing DirectML adapter configuration file: " +
                config_path;
            return false;
        }

        provider8 =
            "directml:" +
            wide_to_utf8(config_path);
    }

    HMODULE module = LoadLibraryExW(
        dll.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

    if (!module) {
        m_error =
            L"Could not load sherpa-onnx-c-api.dll. Windows error " +
            std::to_wstring(GetLastError());
        return false;
    }

    m_module = module;

    auto api_ptr = std::make_unique<engine::api>();

    api_ptr->create = reinterpret_cast<CreateFn>(
        GetProcAddress(
            module,
            "SherpaOnnxCreateOfflineSourceSeparation"));

    api_ptr->destroy = reinterpret_cast<DestroyFn>(
        GetProcAddress(
            module,
            "SherpaOnnxDestroyOfflineSourceSeparation"));

    api_ptr->process = reinterpret_cast<ProcessFn>(
        GetProcAddress(
            module,
            "SherpaOnnxOfflineSourceSeparationProcess"));

    api_ptr->destroy_output = reinterpret_cast<DestroyOutputFn>(
        GetProcAddress(
            module,
            "SherpaOnnxDestroySourceSeparationOutput"));

    if (!api_ptr->create ||
        !api_ptr->destroy ||
        !api_ptr->process ||
        !api_ptr->destroy_output) {

        m_error =
            L"The installed sherpa-onnx C API does not expose source separation.";

        FreeLibrary(module);
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
    config.model.provider = provider8.c_str();

    m_separator = api_ptr->create(&config);

    if (!m_separator) {
        m_error =
            L"Sherpa-onnx could not create the Spleeter separation engine for " +
            std::wstring(backend_name(wanted)) + L".";
        FreeLibrary(module);
        m_module = nullptr;
        return false;
    }

    m_api = api_ptr.release();
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
        FreeLibrary(static_cast<HMODULE>(m_module));
        m_module = nullptr;
    }
}

bool engine::process_both(
    const float* input,
    size_t frames,
    unsigned channels,
    unsigned sample_rate,
    std::vector<float>& vocals,
    std::vector<float>& instrumental) {

    vocals.clear();
    instrumental.clear();

    if (!input || frames == 0) return false;

    if (channels != 2 || sample_rate != 44100) {
        m_error =
            L"ONNX prototype currently supports stereo 44.1 kHz audio only.";
        return false;
    }

    if (!initialize()) return false;

    std::vector<float> left(frames);
    std::vector<float> right(frames);

    for (size_t i = 0; i < frames; ++i) {
        left[i] = input[i * 2];
        right[i] = input[i * 2 + 1];
    }

    const float* planes[2] = {
        left.data(),
        right.data()
    };

    const SeparationOutput* result =
        m_api->process(
            m_separator,
            planes,
            2,
            static_cast<int32_t>(frames),
            static_cast<int32_t>(sample_rate));

    if (!result) {
        m_error =
            L"Sherpa-onnx source separation returned no output.";
        return false;
    }

    bool ok = false;

    if (result->num_stems >= 2 &&
        result->sample_rate ==
            static_cast<int32_t>(sample_rate)) {

        copy_stem_interleaved(
            result->stems[0],
            frames,
            vocals);

        copy_stem_interleaved(
            result->stems[1],
            frames,
            instrumental);

        ok =
            vocals.size() == frames * 2 &&
            instrumental.size() == frames * 2;
    }

    m_api->destroy_output(result);

    if (!ok) {
        m_error =
            L"Sherpa-onnx returned an unexpected Spleeter output format.";
    }

    return ok;
}

} // namespace onnxstem
