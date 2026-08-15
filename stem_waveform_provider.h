#pragma once

#include <foobar2000/SDK/foobar2000.h>

class NOVTABLE stem_waveform_provider : public service_base {
    FB2K_MAKE_SERVICE_INTERFACE_ENTRYPOINT(stem_waveform_provider);
public:
    // 0 = Original, 1 = Vocals, 2 = Instrumental.
    virtual int get_mode() = 0;

    // Separates one decoded interleaved PCM block. The provider accepts the
    // source format supplied by foobar2000, performs any preparation needed by
    // the ONNX engine internally, and writes both stems back in the SAME frame
    // count/channel layout as the input. Output buffers are owned by the caller
    // so no allocator/container objects cross the component DLL boundary.
    virtual bool process_both(
        const float* input,
        t_size frames,
        unsigned channels,
        unsigned sample_rate,
        float* vocals_out,
        float* instrumental_out,
        t_size output_samples,
        abort_callback& aborter) = 0;
};
