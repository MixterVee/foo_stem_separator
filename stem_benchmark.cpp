#include <foobar2000/SDK/foobar2000.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commctrl.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "onnx_stem_engine.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace {

constexpr unsigned kRate = 44100;
constexpr unsigned kChannels = 2;
constexpr size_t kBenchmarkFrames = static_cast<size_t>(kRate) * 4;

static const GUID g_stem_separator_context_group =
    {0x72a4f1c1,0x4ad3,0x4bb6,{0x98,0x2d,0x7f,0x42,0x31,0x90,0x45,0x10}};

std::wstring utf8_to_wide(const char* s) {
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::vector<wchar_t> temp(static_cast<size_t>(n));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, temp.data(), n);
    return std::wstring(temp.data());
}

std::wstring local_path_from_handle(metadb_handle_ptr handle) {
    if (handle.is_empty()) return {};
    std::wstring path = utf8_to_wide(handle->get_path());
    const std::wstring prefix = L"file://";
    if (path.rfind(prefix, 0) == 0) path.erase(0, prefix.size());
    return path;
}

class mf_guard {
public:
    ~mf_guard() {
        if (m_mf) MFShutdown();
        if (m_com) CoUninitialize();
    }

    bool start(std::wstring& error) {
        const HRESULT chr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(chr)) {
            m_com = true;
        } else if (chr != RPC_E_CHANGED_MODE) {
            error = L"COM initialization failed.";
            return false;
        }

        const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(hr)) {
            error = L"Media Foundation initialization failed.";
            return false;
        }
        m_mf = true;
        return true;
    }

private:
    bool m_com = false;
    bool m_mf = false;
};

bool decode_benchmark_clip(
    const std::wstring& source,
    std::vector<float>& audio,
    std::wstring& error) {

    mf_guard guard;
    if (!guard.start(error)) return false;

    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(source.c_str(), nullptr, &reader);
    if (FAILED(hr)) {
        error = L"Windows could not open the selected audio file.";
        return false;
    }

    ComPtr<IMFMediaType> type;
    hr = MFCreateMediaType(&type);
    if (FAILED(hr)) {
        error = L"Could not create the benchmark audio format.";
        return false;
    }

    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 8);
    type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * 8);

    hr = reader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type.Get());
    if (FAILED(hr)) {
        error = L"Windows could not convert this track to stereo 44.1 kHz float audio.";
        return false;
    }

    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    const size_t wanted_samples = kBenchmarkFrames * kChannels;
    audio.clear();
    audio.reserve(wanted_samples);

    while (audio.size() < wanted_samples) {
        DWORD stream_index = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;

        hr = reader->ReadSample(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            0, &stream_index, &flags, &timestamp, &sample);

        if (FAILED(hr)) {
            error = L"Error while decoding the benchmark clip.";
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;

        BYTE* bytes = nullptr;
        DWORD max_length = 0;
        DWORD current_length = 0;
        if (FAILED(buffer->Lock(&bytes, &max_length, &current_length))) continue;

        const float* src = reinterpret_cast<const float*>(bytes);
        const size_t count = current_length / sizeof(float);
        const size_t remaining = wanted_samples - audio.size();
        const size_t take = (std::min)(count, remaining);
        audio.insert(audio.end(), src, src + take);
        buffer->Unlock();
    }

    if (audio.size() & 1u) audio.pop_back();
    if (audio.size() < static_cast<size_t>(kRate) * kChannels) {
        error = L"The selected track is too short for a useful benchmark.";
        return false;
    }
    return true;
}

struct benchmark_result {
    onnxstem::backend value = onnxstem::backend::cpu;
    std::wstring label;
    bool ok = false;
    double first_use_ms = 0.0;
    double repeat_ms = 0.0;
    std::wstring error;
};

double elapsed_ms(
    std::chrono::steady_clock::time_point a,
    std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

std::wstring adapter_label(const onnxstem::directml_adapter_info& a) {
    std::wostringstream s;
    s << L"DirectML adapter " << a.index;
    if (!a.name.empty()) s << L" - " << a.name;
    return s.str();
}

benchmark_result run_one(
    onnxstem::backend value,
    const std::wstring& label,
    const std::vector<float>& clip) {

    benchmark_result out;
    out.value = value;
    out.label = label;

    const size_t frames = clip.size() / kChannels;
    std::vector<float> vocals;
    std::vector<float> instrumental;
    onnxstem::engine engine(value);

    const auto init_begin = std::chrono::steady_clock::now();
    if (!engine.ready()) {
        out.error = engine.last_error();
        return out;
    }
    const auto init_end = std::chrono::steady_clock::now();

    const auto first_begin = std::chrono::steady_clock::now();
    if (!engine.process_both(
            clip.data(), frames, kChannels, kRate, vocals, instrumental)) {
        out.error = engine.last_error();
        return out;
    }
    const auto first_end = std::chrono::steady_clock::now();

    vocals.clear();
    instrumental.clear();

    const auto repeat_begin = std::chrono::steady_clock::now();
    if (!engine.process_both(
            clip.data(), frames, kChannels, kRate, vocals, instrumental)) {
        out.error = engine.last_error();
        return out;
    }
    const auto repeat_end = std::chrono::steady_clock::now();

    out.first_use_ms = elapsed_ms(init_begin, init_end) +
        elapsed_ms(first_begin, first_end);
    out.repeat_ms = elapsed_ms(repeat_begin, repeat_end);
    out.ok = true;
    return out;
}

std::wstring format_seconds(double ms) {
    std::wostringstream s;
    s << std::fixed << std::setprecision(2) << (ms / 1000.0) << L" s";
    return s.str();
}

std::wstring format_speedup(double speedup) {
    std::wostringstream s;
    s << std::fixed << std::setprecision(2) << speedup << L"x";
    return s.str();
}

void show_results_and_select(std::vector<benchmark_result>& results) {
    const benchmark_result* cpu = nullptr;
    const benchmark_result* fastest = nullptr;

    for (const auto& r : results) {
        if (r.value == onnxstem::backend::cpu && r.ok) cpu = &r;
        if (r.ok && (!fastest || r.repeat_ms < fastest->repeat_ms)) fastest = &r;
    }

    // Always refresh Auto's stored winner after a successful benchmark, even if
    // the user chooses a fixed backend below. Auto only becomes active when the
    // dedicated Auto / Fastest button is selected.
    if (fastest) onnxstem::remember_auto_backend(fastest->value);

    std::wostringstream text;
    text << L"Test: the same 4-second stereo 44.1 kHz Spleeter block\n\n";
    text << L"TIME: LOWER is better.\n";
    text << L"SPEED vs CPU: HIGHER is better.\n\n";

    for (const auto& r : results) {
        text << r.label << L"\n";
        if (!r.ok) {
            text << L"  Failed: " << r.error << L"\n\n";
            continue;
        }

        text << L"  First-use: " << format_seconds(r.first_use_ms) << L"\n";
        text << L"  Repeat:    " << format_seconds(r.repeat_ms) << L"\n";
        if (cpu && cpu->repeat_ms > 0.0 && r.repeat_ms > 0.0) {
            text << L"  Speed vs CPU: "
                 << format_speedup(cpu->repeat_ms / r.repeat_ms) << L"\n";
        }
        text << L"\n";
    }

    if (fastest) {
        text << L"Fastest repeat result: " << fastest->label << L"\n";
    }

    text << L"\nAuto / Fastest remembers the fastest successful result from this benchmark.\n"
            L"It does not re-run the benchmark when foobar2000 starts.\n\n"
            L"First-use includes model/session setup plus the first stem pass.\n"
            L"Repeat best represents ongoing stem and seek processing.\n\n"
            L"GPU selections are saved by hardware identity, not by adapter number.\n"
            L"If Auto's winning GPU is missing or DirectML cannot initialize, live stems fall back to CPU.\n\n"
            L"Choose Auto / Fastest or a fixed backend below.";

    std::vector<std::wstring> button_text;
    std::vector<onnxstem::backend> choices;
    button_text.reserve(results.size());
    choices.reserve(results.size());

    for (const auto& r : results) {
        if (!r.ok) continue;
        button_text.push_back(r.label);
        choices.push_back(r.value);
    }

    if (choices.empty()) {
        MessageBoxW(
            core_api::get_main_window(), text.str().c_str(),
            L"Stem Separator Benchmark", MB_OK | MB_ICONWARNING);
        return;
    }

    constexpr int kAutoButton = 900;
    std::wstring auto_button_text;
    std::vector<TASKDIALOG_BUTTON> buttons;
    buttons.reserve(choices.size() + (fastest ? 1u : 0u));

    int default_button = 1000;
    if (fastest) {
        auto_button_text = L"Auto / Fastest - " + fastest->label;
        TASKDIALOG_BUTTON auto_button{};
        auto_button.nButtonID = kAutoButton;
        auto_button.pszButtonText = auto_button_text.c_str();
        buttons.push_back(auto_button);
        default_button = kAutoButton;
    }

    for (size_t i = 0; i < choices.size(); ++i) {
        TASKDIALOG_BUTTON b{};
        b.nButtonID = 1000 + static_cast<int>(i);
        b.pszButtonText = button_text[i].c_str();
        buttons.push_back(b);
    }

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = core_api::get_main_window();
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = L"Stem Separator Benchmark";
    config.pszMainInstruction = L"Benchmark complete - lower processing time is better";
    const std::wstring content = text.str();
    config.pszContent = content.c_str();
    config.cButtons = static_cast<UINT>(buttons.size());
    config.pButtons = buttons.data();
    config.nDefaultButton = default_button;

    int pressed = 0;
    const HRESULT hr = TaskDialogIndirect(&config, &pressed, nullptr, nullptr);
    if (FAILED(hr)) {
        MessageBoxW(
            core_api::get_main_window(), content.c_str(),
            L"Stem Separator Benchmark", MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (pressed == kAutoButton && fastest) {
        onnxstem::select_auto_backend();

        std::wstring confirmation =
            L"Selected processing backend:\n\nAuto / Fastest\n\nCurrent benchmark winner:\n" +
            fastest->label +
            L"\n\nThe winner is stored by hardware identity. Auto will use this backend on future "
            L"separation calls without re-running the benchmark. CPU fallback remains automatic "
            L"if the winning GPU is missing or DirectML cannot initialize.";

        MessageBoxW(
            core_api::get_main_window(), confirmation.c_str(),
            L"Stem Separator", MB_OK | MB_ICONINFORMATION);
        console::print("Stem Separator backend selection updated to Auto / Fastest.");
        return;
    }

    const int choice_index = pressed - 1000;
    if (choice_index < 0 ||
        static_cast<size_t>(choice_index) >= choices.size()) return;

    const onnxstem::backend selected = choices[static_cast<size_t>(choice_index)];
    onnxstem::select_backend(selected);

    std::wstring confirmation =
        L"Selected fixed processing backend:\n\n" + onnxstem::backend_name(selected) +
        L"\n\nThe GPU preference is stored by hardware identity. "
        L"The live cache and exports will use it on their next separation call. "
        L"CPU fallback remains automatic if that GPU is unavailable.";

    MessageBoxW(
        core_api::get_main_window(), confirmation.c_str(),
        L"Stem Separator", MB_OK | MB_ICONINFORMATION);

    console::print("Stem Separator fixed backend selection updated.");
}
void benchmark_thread(std::wstring source) {
    std::vector<float> clip;
    std::wstring error;

    if (!decode_benchmark_clip(source, clip, error)) {
        MessageBoxW(
            core_api::get_main_window(), error.c_str(),
            L"Stem Separator Benchmark", MB_OK | MB_ICONERROR);
        return;
    }

    const auto adapters = onnxstem::enumerate_directml_adapters();
    std::vector<benchmark_result> results;
    results.reserve(1 + adapters.size());

    results.push_back(run_one(onnxstem::backend::cpu, L"CPU", clip));
    for (const auto& a : adapters) {
        results.push_back(run_one(
            onnxstem::directml_backend(a.index), adapter_label(a), clip));
    }

    show_results_and_select(results);
}

void begin_benchmark(metadb_handle_list_cref data) {
    if (data.get_count() == 0) return;

    const std::wstring source = local_path_from_handle(data[0]);
    if (source.empty() || !fs::exists(fs::path(source))) {
        MessageBoxW(
            core_api::get_main_window(),
            L"The benchmark currently requires a local audio file.",
            L"Stem Separator Benchmark", MB_OK | MB_ICONWARNING);
        return;
    }

    const auto adapters = onnxstem::enumerate_directml_adapters();
    std::wostringstream prompt;
    prompt << L"Stem Separator detected " << adapters.size()
           << L" hardware DirectML adapter" << (adapters.size() == 1 ? L"" : L"s")
           << L".\n\nIt will benchmark CPU and every detected GPU using the same 4-second audio clip.\n\n"
              L"For the cleanest comparison, pause playback before running the test.\n"
              L"The test can take several seconds and will heavily use the CPU/GPU while it runs.\n\n"
              L"TIME: lower is better.  SPEED vs CPU: higher is better.\n\n"
              L"Run the benchmark now?";

    const int answer = MessageBoxW(
        core_api::get_main_window(), prompt.str().c_str(),
        L"Stem Separator Benchmark",
        MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON1);

    if (answer != IDYES) return;
    std::thread(benchmark_thread, source).detach();
}

class stem_backend_benchmark_context_menu : public contextmenu_item_simple {
public:
    GUID get_parent() override {
        return g_stem_separator_context_group;
    }

    unsigned get_num_items() override {
        return 1;
    }

    void get_item_name(unsigned, pfc::string_base& out) override {
        out = "Benchmark / Select Processing Backend...";
    }

    GUID get_item_guid(unsigned) override {
        static const GUID id =
            {0xa92a1010,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x10}};
        return id;
    }

    bool get_item_description(unsigned, pfc::string_base& out) override {
        out =
            "Benchmark CPU and every detected DirectML GPU on the same 4-second stem block, "
            "then choose which backend Stem Separator should use. GPU preference is saved by "
            "hardware identity and automatically falls back to CPU if unavailable.";
        return true;
    }

    void context_command(
        unsigned,
        metadb_handle_list_cref data,
        const GUID&) override {
        begin_benchmark(data);
    }
};

static contextmenu_item_factory_t<stem_backend_benchmark_context_menu>
    g_stem_backend_benchmark_context_menu;

} // namespace

