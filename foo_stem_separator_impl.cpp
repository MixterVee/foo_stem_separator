#include <foobar2000/SDK/foobar2000.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commdlg.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "onnx_stem_engine.h"
#include "stem_mode.h"
#include "persistent_stem_cache.h"

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

DECLARE_COMPONENT_VERSION(
    "Stem Separator",
    "2.5.0 Dynamic DirectML GPU backends + benchmark",
    "Native ONNX vocals / instrumental separation.\n"
    "Zero-latency position-cache playback with optional start pre-cache and clean WAV/MP3 export.\n"
    "Live stems use independent read-ahead caching; export uses whole-track Spleeter inference with WAV or 320 kbps MP3 output."
);

VALIDATE_COMPONENT_FILENAME("foo_stem_separator.dll");

namespace stem_precache {

static std::atomic<bool> g_enabled{true};

bool enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

void toggle() {
    const bool current =
        g_enabled.load(std::memory_order_relaxed);

    g_enabled.store(
        !current,
        std::memory_order_relaxed);
}

} // namespace stem_precache

namespace persistent_stem_cache {
namespace {
static const GUID g_cache_enabled_guid =
    {0x8e6f2201,0x3aa6,0x43c8,{0x9d,0x2e,0x81,0x6d,0x61,0xa4,0x77,0x10}};
static const GUID g_cache_max_gb_guid =
    {0x8e6f2202,0x3aa6,0x43c8,{0x9d,0x2e,0x81,0x6d,0x61,0xa4,0x77,0x10}};

cfg_int g_cache_enabled_cfg(g_cache_enabled_guid, 1);
cfg_int g_cache_max_gb_cfg(g_cache_max_gb_guid, 10);
} // namespace

bool enabled() {
    return static_cast<int>(g_cache_enabled_cfg.get()) != 0;
}

void set_enabled(bool value) {
    g_cache_enabled_cfg = value ? 1 : 0;
}

unsigned max_gb() {
    const int configured = static_cast<int>(g_cache_max_gb_cfg.get());
    if (configured < 1) return 10u;
    return static_cast<unsigned>(configured > 200 ? 200 : configured);
}

void set_max_gb(unsigned value) {
    if (value < 1u) value = 1u;
    if (value > 200u) value = 200u;
    g_cache_max_gb_cfg = static_cast<int>(value);
    detail::enforce_limit();
}

} // namespace persistent_stem_cache

namespace {

constexpr unsigned kExportRate = 44100;
constexpr unsigned kExportChannels = 2;
constexpr double kExportWindowSeconds = 12.0;
constexpr double kExportOverlapSeconds = 2.0;

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

std::wstring local_path_from_handle(metadb_handle_ptr handle) {
    std::wstring path = utf8_to_wide(handle->get_path());

    const std::wstring prefix = L"file://";
    if (path.rfind(prefix, 0) == 0) {
        path.erase(0, prefix.size());
    }

    return path;
}

std::wstring default_export_path(
    const std::wstring& source,
    bool vocals,
    bool mp3) {

    fs::path p(source);

    const std::wstring suffix =
        vocals
            ? (mp3 ? L" - Vocals.mp3" : L" - Vocals.wav")
            : (mp3 ? L" - Instrumental.mp3" : L" - Instrumental.wav");

    return (
        p.parent_path() /
        (p.stem().wstring() + suffix)
    ).wstring();
}

bool choose_save_path(
    const std::wstring& initial,
    bool mp3,
    std::wstring& selected) {

    std::vector<wchar_t> filename(32768, L'\0');

    const size_t copy_count =
        (std::min)(initial.size(), filename.size() - 1);

    std::copy_n(
        initial.c_str(),
        copy_count,
        filename.data());

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter =
        mp3
            ? L"MP3 audio (*.mp3)\0*.mp3\0All files (*.*)\0*.*\0\0"
            : L"WAV audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = filename.data();
    ofn.nMaxFile = static_cast<DWORD>(filename.size());
    ofn.lpstrDefExt = mp3 ? L"mp3" : L"wav";
    ofn.Flags =
        OFN_OVERWRITEPROMPT |
        OFN_PATHMUSTEXIST |
        OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&ofn)) {
        return false;
    }

    selected = filename.data();
    return !selected.empty();
}

class mf_shutdown_guard {
public:
    ~mf_shutdown_guard() {
        if (m_started) MFShutdown();
        if (m_com) CoUninitialize();
    }

    bool start(std::wstring& error) {
        const HRESULT chr =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        if (SUCCEEDED(chr)) {
            m_com = true;
        } else if (chr != RPC_E_CHANGED_MODE) {
            error = L"COM initialization failed.";
            return false;
        }

        const HRESULT hr =
            MFStartup(MF_VERSION, MFSTARTUP_FULL);

        if (FAILED(hr)) {
            error = L"Media Foundation initialization failed.";
            return false;
        }

        m_started = true;
        return true;
    }

private:
    bool m_started = false;
    bool m_com = false;
};

bool configure_float_stereo_44100(
    IMFSourceReader* reader,
    std::wstring& error) {

    ComPtr<IMFMediaType> type;

    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr)) {
        error = L"Could not create Media Foundation audio type.";
        return false;
    }

    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kExportChannels);
    type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kExportRate);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 8);
    type->SetUINT32(
        MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
        kExportRate * 8);

    hr = reader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM,
        nullptr,
        type.Get());

    if (FAILED(hr)) {
        error =
            L"Windows Media Foundation could not convert this track "
            L"to stereo 44.1 kHz float audio.";
        return false;
    }

    reader->SetStreamSelection(
        MF_SOURCE_READER_ALL_STREAMS,
        FALSE);

    reader->SetStreamSelection(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM,
        TRUE);

    return true;
}

bool decode_audio_file(
    const std::wstring& source,
    std::vector<float>& audio,
    std::wstring& error) {

    mf_shutdown_guard guard;
    if (!guard.start(error)) return false;

    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 2);
    if (FAILED(hr)) {
        error = L"Could not create Media Foundation attributes.";
        return false;
    }

    ComPtr<IMFSourceReader> reader;

    hr = MFCreateSourceReaderFromURL(
        source.c_str(),
        attrs.Get(),
        &reader);

    if (FAILED(hr)) {
        error =
            L"Windows could not open the selected audio file for export.";
        return false;
    }

    if (!configure_float_stereo_44100(reader.Get(), error)) {
        return false;
    }

    audio.clear();

    for (;;) {
        DWORD stream_index = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;

        hr = reader->ReadSample(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            0,
            &stream_index,
            &flags,
            &timestamp,
            &sample);

        if (FAILED(hr)) {
            error = L"Error while decoding the source file.";
            return false;
        }

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            break;
        }

        if (!sample) continue;

        ComPtr<IMFMediaBuffer> buffer;

        hr = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(hr)) {
            error = L"Could not read decoded audio samples.";
            return false;
        }

        BYTE* bytes = nullptr;
        DWORD max_length = 0;
        DWORD current_length = 0;

        hr = buffer->Lock(
            &bytes,
            &max_length,
            &current_length);

        if (FAILED(hr)) {
            error = L"Could not lock decoded audio buffer.";
            return false;
        }

        const size_t count =
            current_length / sizeof(float);

        const float* floats =
            reinterpret_cast<const float*>(bytes);

        audio.insert(
            audio.end(),
            floats,
            floats + count);

        buffer->Unlock();
    }

    if (audio.empty()) {
        error = L"No audio samples were decoded.";
        return false;
    }

    return true;
}

void append_u16(std::ofstream& f, uint16_t v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

void append_u32(std::ofstream& f, uint32_t v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

bool write_float_wav(
    const std::wstring& path,
    const std::vector<float>& samples,
    std::wstring& error) {

    const uint64_t data_bytes_64 =
        static_cast<uint64_t>(samples.size()) * sizeof(float);

    if (data_bytes_64 > 0xFFFFFFFFull - 64ull) {
        error =
            L"The WAV would exceed the classic RIFF 4 GB limit.";
        return false;
    }

    const uint32_t data_bytes =
        static_cast<uint32_t>(data_bytes_64);

    const uint32_t riff_size =
        36u + data_bytes;

    std::ofstream file(
        fs::path(path),
        std::ios::binary | std::ios::trunc);

    if (!file) {
        error = L"Could not create the destination WAV file.";
        return false;
    }

    file.write("RIFF", 4);
    append_u32(file, riff_size);
    file.write("WAVE", 4);

    file.write("fmt ", 4);
    append_u32(file, 16);
    append_u16(file, 3); // IEEE float
    append_u16(file, kExportChannels);
    append_u32(file, kExportRate);
    append_u32(
        file,
        kExportRate * kExportChannels * sizeof(float));
    append_u16(
        file,
        kExportChannels * sizeof(float));
    append_u16(file, 32);

    file.write("data", 4);
    append_u32(file, data_bytes);

    file.write(
        reinterpret_cast<const char*>(samples.data()),
        data_bytes);

    if (!file) {
        error = L"Error while writing the WAV file.";
        return false;
    }

    return true;
}

bool write_mp3_320(
    const std::wstring& path,
    const std::vector<float>& samples,
    std::wstring& error) {

    if (samples.empty()) {
        error = L"No audio samples to encode.";
        return false;
    }

    mf_shutdown_guard mf;
    if (!mf.start(error)) {
        return false;
    }

    ComPtr<IMFSinkWriter> writer;

    HRESULT hr =
        MFCreateSinkWriterFromURL(
            path.c_str(),
            nullptr,
            nullptr,
            &writer);

    if (FAILED(hr)) {
        error = L"Could not create the MP3 output file.";
        return false;
    }

    ComPtr<IMFMediaType> output_type;
    hr = MFCreateMediaType(&output_type);
    if (FAILED(hr)) {
        error = L"Could not create the MP3 output format.";
        return false;
    }

    output_type->SetGUID(
        MF_MT_MAJOR_TYPE,
        MFMediaType_Audio);

    output_type->SetGUID(
        MF_MT_SUBTYPE,
        MFAudioFormat_MP3);

    output_type->SetUINT32(
        MF_MT_AUDIO_NUM_CHANNELS,
        kExportChannels);

    output_type->SetUINT32(
        MF_MT_AUDIO_SAMPLES_PER_SECOND,
        kExportRate);

    // Media Foundation specifies encoded MP3 bitrate here in BYTES/sec.
    output_type->SetUINT32(
        MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
        320000 / 8);

    DWORD stream_index = 0;

    hr = writer->AddStream(
        output_type.Get(),
        &stream_index);

    if (FAILED(hr)) {
        error = L"The Windows MP3 encoder did not accept 320 kbps stereo output.";
        return false;
    }

    // The built-in Media Foundation MP3 encoder accepts 16-bit integer PCM,
    // not 32-bit floating point, so convert the clean Spleeter float output
    // only at this final encoding stage.
    ComPtr<IMFMediaType> input_type;
    hr = MFCreateMediaType(&input_type);
    if (FAILED(hr)) {
        error = L"Could not create the MP3 encoder input format.";
        return false;
    }

    input_type->SetGUID(
        MF_MT_MAJOR_TYPE,
        MFMediaType_Audio);

    input_type->SetGUID(
        MF_MT_SUBTYPE,
        MFAudioFormat_PCM);

    input_type->SetUINT32(
        MF_MT_AUDIO_NUM_CHANNELS,
        kExportChannels);

    input_type->SetUINT32(
        MF_MT_AUDIO_SAMPLES_PER_SECOND,
        kExportRate);

    input_type->SetUINT32(
        MF_MT_AUDIO_BITS_PER_SAMPLE,
        16);

    input_type->SetUINT32(
        MF_MT_AUDIO_BLOCK_ALIGNMENT,
        kExportChannels * sizeof(int16_t));

    input_type->SetUINT32(
        MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
        kExportRate * kExportChannels * sizeof(int16_t));

    hr = writer->SetInputMediaType(
        stream_index,
        input_type.Get(),
        nullptr);

    if (FAILED(hr)) {
        error = L"Could not configure the Windows MP3 encoder for 16-bit PCM input.";
        return false;
    }

    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        error = L"Could not start MP3 encoding.";
        return false;
    }

    const size_t total_frames =
        samples.size() / kExportChannels;

    const size_t frames_per_block = kExportRate;
    size_t frame_offset = 0;
    LONGLONG sample_time = 0;

    while (frame_offset < total_frames) {
        const size_t remaining =
            total_frames - frame_offset;

        const size_t frame_count =
            remaining < frames_per_block
                ? remaining
                : frames_per_block;

        std::vector<int16_t> pcm(
            frame_count * kExportChannels);

        const size_t sample_offset =
            frame_offset * kExportChannels;

        for (size_t i = 0; i < pcm.size(); ++i) {
            float v = samples[sample_offset + i];

            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;

            const float scaled =
                v >= 0.0f
                    ? v * 32767.0f
                    : v * 32768.0f;

            pcm[i] =
                static_cast<int16_t>(
                    std::lrint(scaled));
        }

        const DWORD byte_count =
            static_cast<DWORD>(
                pcm.size() * sizeof(int16_t));

        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateMemoryBuffer(
            byte_count,
            &buffer);

        if (FAILED(hr)) {
            error = L"Could not allocate an MP3 encoder buffer.";
            return false;
        }

        BYTE* dst = nullptr;
        DWORD max_length = 0;

        hr = buffer->Lock(
            &dst,
            &max_length,
            nullptr);

        if (FAILED(hr)) {
            error = L"Could not lock an MP3 encoder buffer.";
            return false;
        }

        memcpy(dst, pcm.data(), byte_count);
        buffer->Unlock();
        buffer->SetCurrentLength(byte_count);

        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) {
            error = L"Could not create an MP3 input sample.";
            return false;
        }

        sample->AddBuffer(buffer.Get());

        const LONGLONG duration =
            static_cast<LONGLONG>(
                (10000000.0 *
                 static_cast<double>(frame_count)) /
                static_cast<double>(kExportRate));

        sample->SetSampleTime(sample_time);
        sample->SetSampleDuration(duration);

        hr = writer->WriteSample(
            stream_index,
            sample.Get());

        if (FAILED(hr)) {
            error = L"Windows Media Foundation failed while encoding MP3 audio.";
            return false;
        }

        sample_time += duration;
        frame_offset += frame_count;
    }

    hr = writer->Finalize();
    if (FAILED(hr)) {
        error = L"Could not finalize the MP3 file.";
        return false;
    }

    return true;
}

bool separate_for_export(
    const std::vector<float>& input,
    bool want_vocals,
    std::vector<float>& output,
    std::wstring& error) {

    const size_t total_frames =
        input.size() / kExportChannels;

    if (total_frames == 0) {
        error = L"The decoded track is empty.";
        return false;
    }

    // V20: process the COMPLETE decoded track in one Spleeter inference.
    //
    // This deliberately removes all export window boundaries. The earlier
    // exporter used overlapping blocks and could still create occasional
    // discontinuities/clicks in the saved WAV even though live playback was
    // clean. sherpa-onnx source separation is an offline API, so whole-track
    // processing is the most natural export path.
    onnxstem::engine engine;

    std::vector<float> vocals;
    std::vector<float> instrumental;

    if (!engine.process_both(
            input.data(),
            total_frames,
            kExportChannels,
            kExportRate,
            vocals,
            instrumental)) {

        error =
            L"ONNX separation failed: " +
            engine.last_error();

        return false;
    }

    const std::vector<float>& selected =
        want_vocals
            ? vocals
            : instrumental;

    if (selected.size() != input.size()) {
        error =
            L"ONNX returned a stem with an unexpected sample count.";
        return false;
    }

    output = selected;
    return true;
}

void export_thread(
    std::wstring source,
    std::wstring destination,
    bool vocals,
    bool mp3) {

    std::vector<float> decoded;
    std::vector<float> separated;
    std::wstring error;

    if (!decode_audio_file(
            source,
            decoded,
            error)) {

        MessageBoxW(
            nullptr,
            error.c_str(),
            L"Stem Separator - Export failed",
            MB_OK | MB_ICONERROR);

        return;
    }

    if (!separate_for_export(
            decoded,
            vocals,
            separated,
            error)) {

        MessageBoxW(
            nullptr,
            error.c_str(),
            L"Stem Separator - Export failed",
            MB_OK | MB_ICONERROR);

        return;
    }

    const bool wrote_file =
        mp3
            ? write_mp3_320(
                destination,
                separated,
                error)
            : write_float_wav(
                destination,
                separated,
                error);

    if (!wrote_file) {
        MessageBoxW(
            nullptr,
            error.c_str(),
            L"Stem Separator - Export failed",
            MB_OK | MB_ICONERROR);

        return;
    }

    const std::wstring format =
        mp3 ? L"MP3" : L"WAV";

    const std::wstring message =
        std::wstring(
            vocals
                ? L"Vocal "
                : L"Instrumental "
        ) +
        format +
        L" saved successfully:\n\n" +
        destination;

    MessageBoxW(
        nullptr,
        message.c_str(),
        L"Stem Separator",
        MB_OK | MB_ICONINFORMATION);
}

void begin_export(
    metadb_handle_list_cref data,
    bool vocals,
    bool mp3) {

    if (data.get_count() == 0) return;

    const std::wstring source =
        local_path_from_handle(data[0]);

    if (source.empty() ||
        !fs::exists(fs::path(source))) {

        MessageBoxW(
            nullptr,
            L"Save is currently available only for local audio files.",
            L"Stem Separator",
            MB_OK | MB_ICONWARNING);

        return;
    }

    std::wstring destination;

    if (!choose_save_path(
            default_export_path(
                source,
                vocals,
                mp3),
            mp3,
            destination)) {
        return;
    }

    MessageBoxW(
        nullptr,
        vocals
            ? L"Vocal export has started in the background.\n"
              L"You can continue using foobar2000."
            : L"Instrumental export has started in the background.\n"
              L"You can continue using foobar2000.",
        L"Stem Separator",
        MB_OK | MB_ICONINFORMATION);

    std::thread(
        export_thread,
        source,
        destination,
        vocals,
        mp3
    ).detach();
}

static const GUID g_stem_separator_context_group =
    {0x72a4f1c1,0x4ad3,0x4bb6,{0x98,0x2d,0x7f,0x42,0x31,0x90,0x45,0x10}};

static contextmenu_group_popup_factory
    g_stem_separator_context_group_factory(
        g_stem_separator_context_group,
        contextmenu_groups::root,
        "Stem Separator",
        0);

static const GUID g_stem_cache_context_group =
    {0x72a4f1d1,0x4ad3,0x4bb6,{0x98,0x2d,0x7f,0x42,0x31,0x90,0x45,0x11}};
static contextmenu_group_popup_factory g_stem_cache_context_group_factory(
    g_stem_cache_context_group,
    g_stem_separator_context_group,
    "Cache Settings",
    50);

static const GUID g_stem_cache_size_context_group =
    {0x72a4f1d2,0x4ad3,0x4bb6,{0x98,0x2d,0x7f,0x42,0x31,0x90,0x45,0x12}};
static contextmenu_group_popup_factory g_stem_cache_size_context_group_factory(
    g_stem_cache_size_context_group,
    g_stem_cache_context_group,
    "Maximum Cache Size",
    20);

static const GUID g_stem_blend_context_group =
    {0x72a4f1e1,0x4ad3,0x4bb6,{0x98,0x2d,0x7f,0x42,0x31,0x90,0x45,0x21}};
static contextmenu_group_popup_factory g_stem_blend_context_group_factory(
    g_stem_blend_context_group,
    g_stem_separator_context_group,
    "Stem Blend",
    20);

static const GUID g_stem_blend_vocal_context_group =
    {0x72a4f1e2,0x4ad3,0x4bb6,{0x98,0x2d,0x7f,0x42,0x31,0x90,0x45,0x22}};
static contextmenu_group_popup_factory g_stem_blend_vocal_context_group_factory(
    g_stem_blend_vocal_context_group,
    g_stem_blend_context_group,
    "Vocals",
    0);

static const GUID g_stem_blend_instrumental_context_group =
    {0x72a4f1e3,0x4ad3,0x4bb6,{0x98,0x2d,0x7f,0x42,0x31,0x90,0x45,0x23}};
static contextmenu_group_popup_factory g_stem_blend_instrumental_context_group_factory(
    g_stem_blend_instrumental_context_group,
    g_stem_blend_context_group,
    "Instrumental",
    10);

class stem_cache_context_menu : public contextmenu_item_simple {
public:
    GUID get_parent() override { return g_stem_cache_context_group; }

    enum command_id : unsigned {
        cmd_enabled = 0,
        cmd_status,
        cmd_clear,
        cmd_count
    };

    unsigned get_num_items() override { return cmd_count; }

    void get_item_name(unsigned index, pfc::string_base& out) override {
        if (index == cmd_enabled) {
            out = persistent_stem_cache::enabled()
                ? "Persistent Cache: ON"
                : "Persistent Cache: OFF";
            return;
        }
        if (index == cmd_status) {
            const uint64_t bytes = persistent_stem_cache::current_size_bytes();
            char text[96] = {};
            const double gib = static_cast<double>(bytes) /
                (1024.0 * 1024.0 * 1024.0);
            if (gib >= 1.0) {
                _snprintf_s(text, sizeof(text), _TRUNCATE,
                    "Current Cache: %.2f GB", gib);
            } else {
                const double mib = static_cast<double>(bytes) /
                    (1024.0 * 1024.0);
                _snprintf_s(text, sizeof(text), _TRUNCATE,
                    "Current Cache: %.1f MB", mib);
            }
            out = text;
            return;
        }
        if (index == cmd_clear) {
            out = "Clear Stem Cache...";
            return;
        }
        out = "Cache Settings";
    }

    GUID get_item_guid(unsigned index) override {
        static const GUID ids[cmd_count] = {
            {0xa92a1011,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x11}},
            {0xa92a1012,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x12}},
            {0xa92a1013,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x13}}
        };
        return ids[index < cmd_count ? index : 0];
    }

    bool get_item_description(unsigned index, pfc::string_base& out) override {
        if (index == cmd_enabled) {
            out = "Enable or disable loading and saving the persistent disk stem cache.";
            return true;
        }
        if (index == cmd_status) {
            out = "Show the current persistent stem cache size and configured maximum.";
            return true;
        }
        if (index == cmd_clear) {
            out = "Delete all persistent stem cache files from disk.";
            return true;
        }
        return false;
    }

    void context_command(unsigned index, metadb_handle_list_cref, const GUID&) override {
        if (index == cmd_enabled) {
            const bool next = !persistent_stem_cache::enabled();
            persistent_stem_cache::set_enabled(next);
            console::print(next
                ? "Stem Separator: persistent cache ON"
                : "Stem Separator: persistent cache OFF");
            return;
        }

        if (index == cmd_status) {
            const uint64_t bytes = persistent_stem_cache::current_size_bytes();
            const double gib = static_cast<double>(bytes) /
                (1024.0 * 1024.0 * 1024.0);
            wchar_t message[512] = {};
            _snwprintf_s(
                message, _countof(message), _TRUNCATE,
                L"Persistent Cache: %s\nCurrent size: %.2f GB\nMaximum size: %u GB\n\n"
                L"When the cache exceeds the maximum, least-recently-used whole-track "
                L"caches are removed until usage falls to about 80%% of the limit.",
                persistent_stem_cache::enabled() ? L"ON" : L"OFF",
                gib,
                persistent_stem_cache::max_gb());
            MessageBoxW(nullptr, message, L"Stem Separator - Cache Status",
                MB_OK | MB_ICONINFORMATION);
            return;
        }

        if (index == cmd_clear) {
            const int answer = MessageBoxW(
                nullptr,
                L"Delete all persistent stem cache files?\n\n"
                L"The current track can continue from its in-memory cache. "
                L"Newly processed stems may begin filling the disk cache again.",
                L"Stem Separator - Clear Cache",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer != IDYES) return;

            const bool ok = persistent_stem_cache::clear();
            MessageBoxW(
                nullptr,
                ok ? L"Persistent stem cache cleared."
                   : L"The persistent stem cache could not be completely cleared.",
                L"Stem Separator",
                MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
            return;
        }
    }
};

static contextmenu_item_factory_t<stem_cache_context_menu>
    g_stem_cache_context_menu;

class stem_cache_size_context_menu : public contextmenu_item_simple {
public:
    GUID get_parent() override { return g_stem_cache_size_context_group; }

    static unsigned size_for(unsigned index) {
        static const unsigned sizes[] = {2, 5, 10, 20, 50, 100};
        return sizes[index < 6 ? index : 2];
    }

    unsigned get_num_items() override { return 6; }

    void get_item_name(unsigned index, pfc::string_base& out) override {
        const unsigned value = size_for(index);
        pfc::string_formatter text;
        text << value << " GB";
        if (persistent_stem_cache::max_gb() == value) text << " (current)";
        out = text;
    }

    GUID get_item_guid(unsigned index) override {
        static const GUID ids[6] = {
            {0xa92a1021,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x21}},
            {0xa92a1022,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x22}},
            {0xa92a1023,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x23}},
            {0xa92a1024,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x24}},
            {0xa92a1025,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x25}},
            {0xa92a1026,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x26}}
        };
        return ids[index < 6 ? index : 2];
    }

    bool get_item_description(unsigned, pfc::string_base& out) override {
        out = "Set the maximum size of the persistent stem cache. "
              "If necessary, old track caches are removed immediately.";
        return true;
    }

    void context_command(unsigned index, metadb_handle_list_cref, const GUID&) override {
        const unsigned value = size_for(index);
        persistent_stem_cache::set_max_gb(value);
        pfc::string_formatter msg;
        msg << "Stem Separator: persistent cache maximum set to "
            << value << " GB";
        console::print(msg);
    }
};

static contextmenu_item_factory_t<stem_cache_size_context_menu>
    g_stem_cache_size_context_menu;


class stem_blend_vocal_context_menu : public contextmenu_item_simple {
public:
    GUID get_parent() override { return g_stem_blend_vocal_context_group; }

    static int level_for(unsigned index) {
        static const int levels[] = {0, 25, 50, 75, 100};
        return levels[index < 5 ? index : 4];
    }

    unsigned get_num_items() override { return 5; }

    void get_item_name(unsigned index, pfc::string_base& out) override {
        const int value = level_for(index);
        pfc::string_formatter text;
        text << value << "%";
        if (stemmode::vocal_percent() == value) text << " (current)";
        out = text;
    }

    GUID get_item_guid(unsigned index) override {
        static const GUID ids[5] = {
            {0xa92a1030,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x30}},
            {0xa92a1031,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x31}},
            {0xa92a1032,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x32}},
            {0xa92a1033,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x33}},
            {0xa92a1034,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x34}}
        };
        return ids[index < 5 ? index : 4];
    }

    bool get_item_description(unsigned, pfc::string_base& out) override {
        out = "Set the vocal level for Stem Blend. Selecting a level activates Blend mode.";
        return true;
    }

    void context_command(unsigned index, metadb_handle_list_cref, const GUID&) override {
        const int value = level_for(index);
        stemmode::set_vocal_percent(value);
        stemmode::set(stemmode::mode::blend);
        pfc::string_formatter msg;
        msg << "Stem Blend: vocals " << value << "% / instrumental "
            << stemmode::instrumental_percent() << "%";
        console::print(msg);
    }
};

static contextmenu_item_factory_t<stem_blend_vocal_context_menu>
    g_stem_blend_vocal_context_menu;

class stem_blend_instrumental_context_menu : public contextmenu_item_simple {
public:
    GUID get_parent() override { return g_stem_blend_instrumental_context_group; }

    static int level_for(unsigned index) {
        static const int levels[] = {0, 25, 50, 75, 100};
        return levels[index < 5 ? index : 4];
    }

    unsigned get_num_items() override { return 5; }

    void get_item_name(unsigned index, pfc::string_base& out) override {
        const int value = level_for(index);
        pfc::string_formatter text;
        text << value << "%";
        if (stemmode::instrumental_percent() == value) text << " (current)";
        out = text;
    }

    GUID get_item_guid(unsigned index) override {
        static const GUID ids[5] = {
            {0xa92a1040,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x40}},
            {0xa92a1041,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x41}},
            {0xa92a1042,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x42}},
            {0xa92a1043,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x43}},
            {0xa92a1044,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x44}}
        };
        return ids[index < 5 ? index : 4];
    }

    bool get_item_description(unsigned, pfc::string_base& out) override {
        out = "Set the instrumental level for Stem Blend. Selecting a level activates Blend mode.";
        return true;
    }

    void context_command(unsigned index, metadb_handle_list_cref, const GUID&) override {
        const int value = level_for(index);
        stemmode::set_instrumental_percent(value);
        stemmode::set(stemmode::mode::blend);
        pfc::string_formatter msg;
        msg << "Stem Blend: vocals " << stemmode::vocal_percent()
            << "% / instrumental " << value << "%";
        console::print(msg);
    }
};

static contextmenu_item_factory_t<stem_blend_instrumental_context_menu>
    g_stem_blend_instrumental_context_menu;

class stem_mode_context_menu :
    public contextmenu_item_simple {

public:
    GUID get_parent() override {
        return g_stem_separator_context_group;
    }

    enum command_id : unsigned {
        cmd_original = 0,
        cmd_vocals,
        cmd_instrumental,
        cmd_blend,
        cmd_save_vocals,
        cmd_save_instrumental,
        cmd_save_vocals_mp3,
        cmd_save_instrumental_mp3,
        cmd_precache_start,
        cmd_count
    };

    unsigned get_num_items() override {
        return cmd_count;
    }

    void get_item_name(
        unsigned index,
        pfc::string_base& out) override {

        switch (index) {
        case cmd_original:
            out = "Original";
            break;

        case cmd_vocals:
            out = "Vocals";
            break;

        case cmd_instrumental:
            out = "Instrumental";
            break;

        case cmd_blend: {
            pfc::string_formatter text;
            text << "Blend (V " << stemmode::vocal_percent()
                 << "% / I " << stemmode::instrumental_percent() << "%)";
            out = text;
            break;
        }

        case cmd_save_vocals:
            out = "Save Vocals as WAV...";
            break;

        case cmd_save_instrumental:
            out = "Save Instrumental as WAV...";
            break;

        case cmd_save_vocals_mp3:
            out = "Save Vocals as MP3...";
            break;

        case cmd_save_instrumental_mp3:
            out = "Save Instrumental as MP3...";
            break;

        case cmd_precache_start:
            out =
                stem_precache::enabled()
                    ? "Pre-cache at track start: ON"
                    : "Pre-cache at track start: OFF";
            break;

        default:
            out = "Stem Separator";
            break;
        }
    }

    GUID get_item_guid(unsigned index) override {
        static const GUID ids[cmd_count] = {
            {0xa92a1001,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x01}},
            {0xa92a1002,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x02}},
            {0xa92a1003,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x03}},
            {0xa92a1009,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x09}},
            {0xa92a1004,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x04}},
            {0xa92a1005,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x05}},
            {0xa92a1006,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x06}},
            {0xa92a1007,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x07}},
            {0xa92a1008,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x08}}
        };

        return ids[index < cmd_count ? index : 0];
    }

    bool get_item_description(
        unsigned index,
        pfc::string_base& out) override {

        switch (index) {
        case cmd_original:
            out = "Play the original stereo mix.";
            return true;

        case cmd_vocals:
            out = "Play the Spleeter vocal stem.";
            return true;

        case cmd_instrumental:
            out = "Play the Spleeter accompaniment stem.";
            return true;

        case cmd_blend:
            out = "Play the cached vocals and instrumental together using the current Stem Blend levels.";
            return true;

        case cmd_save_vocals:
            out =
                "Separate the complete local track and save "
                "a vocal-only 32-bit float WAV.";
            return true;

        case cmd_save_instrumental:
            out =
                "Separate the complete local track and save "
                "an instrumental 32-bit float WAV.";
            return true;

        case cmd_save_vocals_mp3:
            out =
                "Separate the complete local track and save "
                "a vocal-only 320 kbps MP3.";
            return true;

        case cmd_save_instrumental_mp3:
            out =
                "Separate the complete local track and save "
                "an instrumental 320 kbps MP3.";
            return true;

        case cmd_precache_start:
            out =
                "When enabled, wait for the first stem cache block "
                "before a new track begins audibly, so Vocal or "
                "Instrumental is present from the first sample.";
            return true;
        }

        return false;
    }

    void context_command(
        unsigned index,
        metadb_handle_list_cref data,
        const GUID&) override {

        if (index == cmd_precache_start) {
            stem_precache::toggle();

            console::print(
                stem_precache::enabled()
                    ? "Stem Separator: pre-cache at track start ON"
                    : "Stem Separator: pre-cache at track start OFF");

            return;
        }

        if (index == cmd_save_vocals) {
            begin_export(data, true, false);
            return;
        }

        if (index == cmd_save_instrumental) {
            begin_export(data, false, false);
            return;
        }

        if (index == cmd_save_vocals_mp3) {
            begin_export(data, true, true);
            return;
        }

        if (index == cmd_save_instrumental_mp3) {
            begin_export(data, false, true);
            return;
        }

        stemmode::mode next =
            stemmode::mode::original;

        if (index == cmd_vocals) {
            next = stemmode::mode::vocals;
        }
        else if (index == cmd_instrumental) {
            next = stemmode::mode::instrumental;
        }
        else if (index == cmd_blend) {
            next = stemmode::mode::blend;
        }

        stemmode::set(next);

        pfc::string_formatter msg;
        msg << "Stem Separator mode: "
            << stemmode::name(next);

        console::print(msg);
    }
};

static contextmenu_item_factory_t<
    stem_mode_context_menu>
    g_stem_mode_context_menu;

} // namespace

