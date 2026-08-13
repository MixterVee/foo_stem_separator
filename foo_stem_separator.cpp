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
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "onnx_stem_engine.h"
#include "stem_mode.h"

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

DECLARE_COMPONENT_VERSION(
    "Stem Separator",
    "1.2.0 seek-safe back-pressure + whole-track WAV export",
    "Native ONNX vocals / instrumental separation.\n"
    "Seek-safe bounded live worker plus whole-track WAV export.\n"
    "Export uses one Spleeter inference for the entire decoded track—no stitched windows."
);

VALIDATE_COMPONENT_FILENAME("foo_stem_separator.dll");

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
    bool vocals) {

    fs::path p(source);
    const std::wstring suffix =
        vocals ? L" - Vocals.wav" : L" - Instrumental.wav";

    return (
        p.parent_path() /
        (p.stem().wstring() + suffix)
    ).wstring();
}

bool choose_save_path(
    const std::wstring& initial,
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
        L"WAV audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = filename.data();
    ofn.nMaxFile = static_cast<DWORD>(filename.size());
    ofn.lpstrDefExt = L"wav";
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
    bool vocals) {

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

    if (!write_float_wav(
            destination,
            separated,
            error)) {

        MessageBoxW(
            nullptr,
            error.c_str(),
            L"Stem Separator - Export failed",
            MB_OK | MB_ICONERROR);

        return;
    }

    const std::wstring message =
        std::wstring(
            vocals
                ? L"Vocal WAV saved successfully:\n\n"
                : L"Instrumental WAV saved successfully:\n\n"
        ) +
        destination;

    MessageBoxW(
        nullptr,
        message.c_str(),
        L"Stem Separator",
        MB_OK | MB_ICONINFORMATION);
}

void begin_export(
    metadb_handle_list_cref data,
    bool vocals) {

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
            default_export_path(source, vocals),
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
        vocals
    ).detach();
}

class stem_mode_context_menu :
    public contextmenu_item_simple {

public:
    enum command_id : unsigned {
        cmd_original = 0,
        cmd_vocals,
        cmd_instrumental,
        cmd_save_vocals,
        cmd_save_instrumental,
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
            out = "Stem Separator / Original";
            break;

        case cmd_vocals:
            out = "Stem Separator / Vocals";
            break;

        case cmd_instrumental:
            out = "Stem Separator / Instrumental";
            break;

        case cmd_save_vocals:
            out = "Stem Separator / Save Vocals as WAV...";
            break;

        case cmd_save_instrumental:
            out = "Stem Separator / Save Instrumental as WAV...";
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
            {0xa92a1004,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x04}},
            {0xa92a1005,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x05}}
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
        }

        return false;
    }

    void context_command(
        unsigned index,
        metadb_handle_list_cref data,
        const GUID&) override {

        if (index == cmd_save_vocals) {
            begin_export(data, true);
            return;
        }

        if (index == cmd_save_instrumental) {
            begin_export(data, false);
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
