#include <foobar2000/SDK/foobar2000.h>

#include "stem_mode.h"

DECLARE_COMPONENT_VERSION(
    "Stem Separator",
    "0.3.0 async ONNX prototype",
    "Native ONNX vocals / instrumental separation.\n"
    "V13: background worker, 2-second windows, 0.5-second overlap/crossfade.\n"
    "Prototype currently supports stereo 44.1 kHz."
);

VALIDATE_COMPONENT_FILENAME("foo_stem_separator.dll");

namespace {

class stem_mode_context_menu : public contextmenu_item_simple {
public:
    enum command_id : unsigned {
        cmd_original = 0,
        cmd_vocals,
        cmd_instrumental,
        cmd_count
    };

    unsigned get_num_items() override {
        return cmd_count;
    }

    void get_item_name(unsigned index, pfc::string_base& out) override {
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
        default:
            out = "Stem Separator";
            break;
        }
    }

    GUID get_item_guid(unsigned index) override {
        static const GUID ids[cmd_count] = {
            {0xa92a1001,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x01}},
            {0xa92a1002,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x02}},
            {0xa92a1003,0xd1f0,0x4ae1,{0xa0,0x11,0x31,0x10,0x42,0x00,0x00,0x03}}
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
        }

        return false;
    }

    void context_command(
        unsigned index,
        metadb_handle_list_cref,
        const GUID&) override {

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

static contextmenu_item_factory_t<stem_mode_context_menu>
    g_stem_mode_context_menu;

} // namespace
