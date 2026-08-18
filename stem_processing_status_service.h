#pragma once

#include <foobar2000/SDK/foobar2000.h>

// Read-only cross-component snapshot used by Spectral Waveform.  Keep this
// structure POD/fixed-size so no C++ heap-owned strings cross DLL boundaries.
enum stem_processing_backend_type : int {
    stem_processing_backend_unknown = -1,
    stem_processing_backend_cpu = 0,
    stem_processing_backend_directml = 1,
};

struct stem_processing_status {
    int mode = 0; // 0 Original, 1 Vocals, 2 Instrumental
    int backend_type = stem_processing_backend_unknown;
    unsigned adapter_index = 0;
    int engine_ready = 0;
    int processing = 0;
    int cpu_fallback = 0;
    wchar_t backend_label[128]{};
};

class NOVTABLE stem_processing_status_service : public service_base {
    FB2K_MAKE_SERVICE_INTERFACE_ENTRYPOINT(stem_processing_status_service);
public:
    virtual bool get_status(stem_processing_status& out) = 0;
};
