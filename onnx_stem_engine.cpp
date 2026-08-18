#include <foobar2000/SDK/foobar2000.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "onnx_stem_engine.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

#pragma comment(lib, "dxgi.lib")

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

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

// Legacy index-based setting kept only to migrate users of the first selectable
// backend build. A fresh install now defaults to CPU until the user benchmarks.
static const GUID g_backend_cfg_guid =
    {0x5c882f31,0x937b,0x4e51,{0xb0,0xc1,0x68,0x55,0x4a,0x6d,0x91,0x73}};
cfg_int g_backend_cfg(g_backend_cfg_guid, 0);

// Stable hardware identity for the preferred GPU. -1 means not migrated yet.
static const GUID g_backend_kind_guid =
    {0x676c1fe0,0x3772,0x4cad,{0xb1,0x10,0xf7,0x6b,0x44,0xe8,0xf0,0x11}};
static const GUID g_backend_vendor_guid =
    {0x676c1fe1,0x3772,0x4cad,{0xb1,0x10,0xf7,0x6b,0x44,0xe8,0xf0,0x11}};
static const GUID g_backend_device_guid =
    {0x676c1fe2,0x3772,0x4cad,{0xb1,0x10,0xf7,0x6b,0x44,0xe8,0xf0,0x11}};
static const GUID g_backend_subsys_guid =
    {0x676c1fe3,0x3772,0x4cad,{0xb1,0x10,0xf7,0x6b,0x44,0xe8,0xf0,0x11}};
static const GUID g_backend_revision_guid =
    {0x676c1fe4,0x3772,0x4cad,{0xb1,0x10,0xf7,0x6b,0x44,0xe8,0xf0,0x11}};
static const GUID g_backend_ordinal_guid =
    {0x676c1fe5,0x3772,0x4cad,{0xb1,0x10,0xf7,0x6b,0x44,0xe8,0xf0,0x11}};

cfg_int g_backend_kind_cfg(g_backend_kind_guid, -1); // 0 CPU, 1 DirectML
cfg_int g_backend_vendor_cfg(g_backend_vendor_guid, 0);
cfg_int g_backend_device_cfg(g_backend_device_guid, 0);
cfg_int g_backend_subsys_cfg(g_backend_subsys_guid, 0);
cfg_int g_backend_revision_cfg(g_backend_revision_guid, 0);
cfg_int g_backend_ordinal_cfg(g_backend_ordinal_guid, 0);

std::wstring utf8_to_wide(const char* s) {
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::vector<wchar_t> temp(static_cast<size_t>(n));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, temp.data(), n);
    return std::wstring(temp.data());
}

std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(
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

void copy_stem_interleaved(
    const Stem& stem,
    size_t requested_frames,
    std::vector<float>& out) {

    const size_t n = std::min<size_t>(
        requested_frames,
        stem.n > 0 ? static_cast<size_t>(stem.n) : 0);

    out.assign(requested_frames * 2, 0.0f);
    if (stem.num_channels < 2 || !stem.samples) return;

    for (size_t i = 0; i < n; ++i) {
        out[i * 2] = stem.samples[0][i];
        out[i * 2 + 1] = stem.samples[1][i];
    }
}

bool same_hardware(
    const onnxstem::directml_adapter_info& a,
    const onnxstem::directml_adapter_info& b) {
    return a.vendor_id == b.vendor_id &&
        a.device_id == b.device_id &&
        a.subsys_id == b.subsys_id &&
        a.revision == b.revision;
}

bool matches_saved_hardware(const onnxstem::directml_adapter_info& a) {
    return a.vendor_id == static_cast<uint32_t>(g_backend_vendor_cfg.get()) &&
        a.device_id == static_cast<uint32_t>(g_backend_device_cfg.get()) &&
        a.subsys_id == static_cast<uint32_t>(g_backend_subsys_cfg.get()) &&
        a.revision == static_cast<uint32_t>(g_backend_revision_cfg.get()) &&
        a.duplicate_ordinal == static_cast<uint32_t>(g_backend_ordinal_cfg.get());
}

void save_cpu_selection() {
    g_backend_kind_cfg = 0;
    g_backend_cfg = 0;
}

void save_gpu_selection(const onnxstem::directml_adapter_info& a) {
    g_backend_kind_cfg = 1;
    g_backend_vendor_cfg = static_cast<int32_t>(a.vendor_id);
    g_backend_device_cfg = static_cast<int32_t>(a.device_id);
    g_backend_subsys_cfg = static_cast<int32_t>(a.subsys_id);
    g_backend_revision_cfg = static_cast<int32_t>(a.revision);
    g_backend_ordinal_cfg = static_cast<int32_t>(a.duplicate_ordinal);

    // Keep the old setting synchronized for downgrade compatibility.
    g_backend_cfg = static_cast<int32_t>(a.index + 1u);
}

} // namespace

namespace onnxstem {

backend directml_backend(unsigned adapter_index) {
    return static_cast<backend>(static_cast<int>(adapter_index) + 1);
}

bool is_directml_backend(backend value) {
    return static_cast<int>(value) > 0;
}

unsigned directml_adapter_index(backend value) {
    return static_cast<unsigned>(static_cast<int>(value) - 1);
}

std::vector<directml_adapter_info> enumerate_directml_adapters() {
    std::vector<directml_adapter_info> out;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;

    for (unsigned index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory->EnumAdapters1(index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;

        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;

        directml_adapter_info info;
        info.index = index;
        info.name = desc.Description;
        info.vendor_id = desc.VendorId;
        info.device_id = desc.DeviceId;
        info.subsys_id = desc.SubSysId;
        info.revision = desc.Revision;

        uint32_t ordinal = 0;
        for (const auto& prior : out) {
            if (same_hardware(prior, info)) ++ordinal;
        }
        info.duplicate_ordinal = ordinal;
        out.push_back(std::move(info));
    }

    return out;
}

backend selected_backend() {
    const auto adapters = enumerate_directml_adapters();
    const int kind = static_cast<int>(g_backend_kind_cfg.get());

    if (kind == 0) return backend::cpu;

    if (kind == 1) {
        for (const auto& a : adapters) {
            if (matches_saved_hardware(a)) return directml_backend(a.index);
        }

        // Preferred GPU is currently absent. Keep the saved identity so it will
        // be used again if the adapter returns, but run on CPU in the meantime.
        return backend::cpu;
    }

    // One-time migration from the original index-based selector.
    const int old = static_cast<int>(g_backend_cfg.get());
    if (old <= 0) {
        save_cpu_selection();
        return backend::cpu;
    }

    const unsigned old_index = static_cast<unsigned>(old - 1);
    for (const auto& a : adapters) {
        if (a.index == old_index) {
            save_gpu_selection(a);
            return directml_backend(a.index);
        }
    }

    // We cannot safely guess which missing GPU an old numeric index referred to.
    save_cpu_selection();
    return backend::cpu;
}

void select_backend(backend value) {
    if (value == backend::selected) return;

    if (value == backend::cpu) {
        save_cpu_selection();
        return;
    }

    if (!is_directml_backend(value)) return;
    const unsigned wanted = directml_adapter_index(value);
    for (const auto& a : enumerate_directml_adapters()) {
        if (a.index == wanted) {
            save_gpu_selection(a);
            return;
        }
    }
}

std::wstring backend_name(backend value) {
    if (value == backend::selected) value = selected_backend();
    if (value == backend::cpu) return L"CPU";
    if (!is_directml_backend(value)) return L"Unknown";

    const unsigned wanted = directml_adapter_index(value);
    for (const auto& a : enumerate_directml_adapters()) {
        if (a.index == wanted) {
            std::wostringstream s;
            s << L"DirectML adapter " << a.index;
            if (!a.name.empty()) s << L" - " << a.name;
            return s.str();
        }
    }

    std::wostringstream s;
    s << L"DirectML adapter " << wanted;
    return s.str();
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
    std::wstring path = strip_file_scheme(utf8_to_wide(core_api::get_my_full_path()));
    if (path.empty()) return {};
    return fs::path(path).parent_path().wstring();
}

std::wstring engine::dll_path() const {
    return (fs::path(component_directory()) / L"sherpa-onnx-c-api.dll").wstring();
}

std::wstring engine::vocals_model_path() const {
    return (fs::path(component_directory()) / L"models" / L"spleeter" /
        L"vocals.fp16.onnx").wstring();
}

std::wstring engine::accompaniment_model_path() const {
    return (fs::path(component_directory()) / L"models" / L"spleeter" /
        L"accompaniment.fp16.onnx").wstring();
}

bool engine::write_directml_config(
    unsigned adapter_index,
    std::wstring& path,
    std::wstring& error) const {

    wchar_t temp_dir[MAX_PATH + 2]{};
    const DWORD n = GetTempPathW(MAX_PATH, temp_dir);
    if (n == 0 || n > MAX_PATH) {
        error = L"Windows could not provide a temporary directory for DirectML configuration.";
        return false;
    }

    std::wostringstream name;
    name << L"foo_stem_separator_dml_" << GetCurrentProcessId()
         << L"_" << adapter_index << L".config";
    path = (fs::path(temp_dir) / name.str()).wstring();

    std::ofstream file(fs::path(path), std::ios::binary | std::ios::trunc);
    if (!file) {
        error = L"Could not create temporary DirectML adapter configuration.";
        return false;
    }
    file << "device_id=" << adapter_index << "\n";
    file.close();

    if (!file) {
        error = L"Could not write temporary DirectML adapter configuration.";
        return false;
    }
    return true;
}

bool engine::ready() {
    return initialize();
}

bool engine::initialize() {
    const backend wanted = desired_backend();

    // If the desired selection has not changed, an existing session is valid.
    // This also intentionally keeps a CPU fallback session alive after a failed
    // DirectML initialization instead of retrying the GPU every audio block.
    if (m_separator && m_desired_backend == wanted) return true;

    if (m_desired_backend != wanted) {
        shutdown();
        m_desired_backend = wanted;
        m_attempted = false;
        m_error.clear();
    }

    if (m_separator) return true;
    if (m_attempted) return false;
    m_attempted = true;

    if (initialize_backend(wanted)) return true;

    if (m_requested_backend == backend::selected && is_directml_backend(wanted)) {
        const std::wstring gpu_error = m_error;
        shutdown();
        m_error.clear();

        if (initialize_backend(backend::cpu)) {
            pfc::string_formatter msg;
            msg << "Stem Separator: preferred DirectML backend failed; using CPU fallback for this session.";
            console::print(msg);
            return true;
        }

        const std::wstring cpu_error = m_error;
        m_error = L"Preferred DirectML backend failed: " + gpu_error +
            L" CPU fallback also failed: " + cpu_error;
    }

    return false;
}

bool engine::initialize_backend(backend value) {
    m_active_backend = value;
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
    std::wstring temp_config;

    if (is_directml_backend(value)) {
        std::wstring config_error;
        const unsigned index = directml_adapter_index(value);
        if (!write_directml_config(index, temp_config, config_error)) {
            m_error = config_error;
            return false;
        }
        provider8 = "directml:" + wide_to_utf8(temp_config);
    }

    HMODULE module = LoadLibraryExW(
        dll.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

    if (!module) {
        if (!temp_config.empty()) {
            std::error_code ec;
            fs::remove(fs::path(temp_config), ec);
        }
        m_error = L"Could not load sherpa-onnx-c-api.dll. Windows error " +
            std::to_wstring(GetLastError());
        return false;
    }

    auto api_ptr = std::make_unique<engine::api>();
    api_ptr->create = reinterpret_cast<CreateFn>(GetProcAddress(
        module, "SherpaOnnxCreateOfflineSourceSeparation"));
    api_ptr->destroy = reinterpret_cast<DestroyFn>(GetProcAddress(
        module, "SherpaOnnxDestroyOfflineSourceSeparation"));
    api_ptr->process = reinterpret_cast<ProcessFn>(GetProcAddress(
        module, "SherpaOnnxOfflineSourceSeparationProcess"));
    api_ptr->destroy_output = reinterpret_cast<DestroyOutputFn>(GetProcAddress(
        module, "SherpaOnnxDestroySourceSeparationOutput"));

    if (!api_ptr->create || !api_ptr->destroy ||
        !api_ptr->process || !api_ptr->destroy_output) {
        if (!temp_config.empty()) {
            std::error_code ec;
            fs::remove(fs::path(temp_config), ec);
        }
        m_error = L"The installed sherpa-onnx C API does not expose source separation.";
        FreeLibrary(module);
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

    const void* separator = api_ptr->create(&config);

    if (!temp_config.empty()) {
        std::error_code ec;
        fs::remove(fs::path(temp_config), ec);
    }

    if (!separator) {
        m_error = L"Sherpa-onnx could not create the Spleeter separation engine for " +
            backend_name(value) + L".";
        FreeLibrary(module);
        return false;
    }

    m_module = module;
    m_separator = separator;
    m_api = api_ptr.release();
    return true;
}

void engine::shutdown() {
    if (m_api && m_separator) m_api->destroy(m_separator);
    m_separator = nullptr;

    delete m_api;
    m_api = nullptr;

    if (m_module) {
        FreeLibrary(static_cast<HMODULE>(m_module));
        m_module = nullptr;
    }
    m_active_backend = backend::selected;
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
        m_error = L"ONNX prototype currently supports stereo 44.1 kHz audio only.";
        return false;
    }

    if (!initialize()) return false;

    std::vector<float> left(frames);
    std::vector<float> right(frames);
    for (size_t i = 0; i < frames; ++i) {
        left[i] = input[i * 2];
        right[i] = input[i * 2 + 1];
    }

    const float* planes[2] = { left.data(), right.data() };
    const SeparationOutput* result = m_api->process(
        m_separator, planes, 2,
        static_cast<int32_t>(frames), static_cast<int32_t>(sample_rate));

    if (!result) {
        m_error = L"Sherpa-onnx source separation returned no output.";
        return false;
    }

    bool ok = false;
    if (result->num_stems >= 2 &&
        result->sample_rate == static_cast<int32_t>(sample_rate)) {
        copy_stem_interleaved(result->stems[0], frames, vocals);
        copy_stem_interleaved(result->stems[1], frames, instrumental);
        ok = vocals.size() == frames * 2 && instrumental.size() == frames * 2;
    }

    m_api->destroy_output(result);
    if (!ok) {
        m_error = L"Sherpa-onnx returned an unexpected Spleeter output format.";
    }
    return ok;
}

} // namespace onnxstem
