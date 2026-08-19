#include <foobar2000/SDK/foobar2000.h>

// Keep the tested 2.5 implementation byte-for-byte intact while publishing
// current release metadata from this thin compilation wrapper.
#pragma push_macro("DECLARE_COMPONENT_VERSION")
#undef DECLARE_COMPONENT_VERSION
#define DECLARE_COMPONENT_VERSION(name, version, about)
#include "foo_stem_separator_impl.cpp"
#pragma pop_macro("DECLARE_COMPONENT_VERSION")

DECLARE_COMPONENT_VERSION(
    "Stem Separator",
    "2.7.0 Stable Stem Blend",
    "Native ONNX vocals / instrumental separation.\n"
    "Live Original / Vocals / Instrumental switching with seek-safe cached playback and clean WAV/MP3 export.\n"
    "Lossless compressed persistent stem cache with configurable size, cache status/clear controls, start pre-cache, and dynamic DirectML/CPU backend benchmarking."
);
