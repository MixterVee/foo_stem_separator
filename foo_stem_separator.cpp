// foo_stem_separator.cpp
//
// foobar2000 v2 component scaffold.
// Windows/Demucs engine is implemented in stem_engine.cpp.

#include <foobar2000/SDK/foobar2000.h>
#include <windows.h>
#include <shellapi.h>
#include <thread>
#include <string>

#include "stem_engine.h"

DECLARE_COMPONENT_VERSION(
    "Stem Separator",
    "0.1.0",
    "AI vocals / instrumental separation using Demucs.\n"
    "Original files are never modified."
);

VALIDATE_COMPONENT_FILENAME("foo_stem_separator.dll");

namespace {

void show_message(const wchar_t* text, const wchar_t* title = L"Stem Separator") {
    MessageBoxW(nullptr, text, title, MB_OK | MB_ICONINFORMATION);
}

std::wstring utf8_to_wide(const char* s) {
    if (!s) return {};

    int needed = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (needed <= 1) return {};

    std::wstring out(static_cast<size_t>(needed - 1), L'\0');

    // Supply room for the terminating NUL with a temporary buffer.
    std::wstring temp(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, temp.data(), needed);
    temp.resize(static_cast<size_t>(needed - 1));
    return temp;
}

void generate_async(std::wstring source, stemsep::stem_kind kind, bool playAfter) {
    std::thread([source = std::move(source), kind, playAfter]() {
        auto r = stemsep::separate_track(source, kind);

        if (!r.ok) {
            std::wstring msg = L"Stem separation failed.\n\n" + r.error;
            MessageBoxW(nullptr, msg.c_str(), L"Stem Separator", MB_OK | MB_ICONERROR);
            return;
        }

        if (!playAfter) {
            std::wstring msg = L"Stem ready:\n\n" + r.stem_path;
            MessageBoxW(nullptr, msg.c_str(), L"Stem Separator", MB_OK | MB_ICONINFORMATION);
            return;
        }

        // v0.1: reveal the generated stem in Explorer.
        //
        // Next milestone: post back to foobar2000's main thread, create a
        // metadb handle for this WAV and start playback automatically.
        std::wstring args = L"/select,\"" + r.stem_path + L"\"";
        ShellExecuteW(
            nullptr,
            L"open",
            L"explorer.exe",
            args.c_str(),
            nullptr,
            SW_SHOWNORMAL
        );
    }).detach();
}

class stem_context_menu : public contextmenu_item_simple {
public:
    enum command_id : unsigned {
        cmd_play_instrumental = 0,
        cmd_play_vocals,
        cmd_generate,
        cmd_delete_cache,
        cmd_count
    };

    unsigned get_num_items() override {
        return cmd_count;
    }

    void get_item_name(unsigned index, pfc::string_base& out) override {
        switch (index) {
        case cmd_play_instrumental:
            out = "Stem Separator / Play Instrumental";
            break;
        case cmd_play_vocals:
            out = "Stem Separator / Play Vocals";
            break;
        case cmd_generate:
            out = "Stem Separator / Generate Stems";
            break;
        case cmd_delete_cache:
            out = "Stem Separator / Delete Cached Stems";
            break;
        default:
            out = "Stem Separator";
            break;
        }
    }

    GUID get_item_guid(unsigned index) override {
        static const GUID ids[cmd_count] = {
            {0xf0b01001,0x4b8e,0x4ab1,{0x91,0x10,0x7a,0xc1,0x11,0x00,0x00,0x01}},
            {0xf0b01002,0x4b8e,0x4ab1,{0x91,0x10,0x7a,0xc1,0x11,0x00,0x00,0x02}},
            {0xf0b01003,0x4b8e,0x4ab1,{0x91,0x10,0x7a,0xc1,0x11,0x00,0x00,0x03}},
            {0xf0b01004,0x4b8e,0x4ab1,{0x91,0x10,0x7a,0xc1,0x11,0x00,0x00,0x04}}
        };

        return ids[index < cmd_count ? index : 0];
    }

    bool get_item_description(unsigned index, pfc::string_base& out) override {
        switch (index) {
        case cmd_play_instrumental:
            out = "Generate or reuse the accompaniment stem for the selected track.";
            return true;

        case cmd_play_vocals:
            out = "Generate or reuse the isolated vocal stem for the selected track.";
            return true;

        case cmd_generate:
            out = "Generate and cache vocals plus instrumental stems.";
            return true;

        case cmd_delete_cache:
            out = "Delete cached stems for the selected track.";
            return true;
        }

        return false;
    }

    // foobar2000 SDK 2025-03-07 contextmenu_item_simple signature:
    //
    //   context_command(unsigned, metadb_handle_list_cref, const GUID&)
    //
    // There is NO abort_callback parameter here.
    void context_command(
        unsigned index,
        metadb_handle_list_cref data,
        const GUID&
    ) override {

        if (data.get_count() == 0) return;

        const char* raw = data[0]->get_path();
        std::wstring source = utf8_to_wide(raw);

        const std::wstring prefix = L"file://";
        if (source.rfind(prefix, 0) == 0) {
            source.erase(0, prefix.size());
        }

        if (index == cmd_delete_cache) {
            bool ok = stemsep::delete_track_cache(source);

            show_message(
                ok
                    ? L"Cached stems deleted."
                    : L"Could not delete the cached stems."
            );
            return;
        }

        if (index == cmd_play_instrumental) {
            generate_async(
                source,
                stemsep::stem_kind::instrumental,
                true
            );
        }
        else if (index == cmd_play_vocals) {
            generate_async(
                source,
                stemsep::stem_kind::vocals,
                true
            );
        }
        else if (index == cmd_generate) {
            // Demucs two-stem mode creates both output files in one pass.
            generate_async(
                source,
                stemsep::stem_kind::vocals,
                false
            );
        }
    }
};

static contextmenu_item_factory_t<stem_context_menu> g_stem_context_menu;

} // namespace
