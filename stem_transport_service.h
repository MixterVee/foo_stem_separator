#pragma once

#include <foobar2000/SDK/foobar2000.h>

enum stem_transport_state : int {
    stem_transport_normal = 0,
    stem_transport_hold = 1,
    stem_transport_scrub = 2,
    stem_transport_reverse = 3,
    stem_transport_release_wait = 4,
};

// Cross-component transport control used by Spectral Waveform.
// The Stem Separator DSP remains the audio clock: while transport is active
// it replaces the ordinary forward stream with silence, scrub preview, or
// reverse audio, then returns to normal playback after one final seek.
class NOVTABLE stem_transport_service : public service_base {
    FB2K_MAKE_SERVICE_INTERFACE_ENTRYPOINT(stem_transport_service);
public:
    virtual void set_hold(double position_seconds) = 0;
    virtual void set_scrub(double position_seconds) = 0;
    virtual void set_reverse(double position_seconds) = 0;
    virtual void release_transport(double position_seconds) = 0;
    virtual void cancel_transport() = 0;

    virtual int get_state() = 0;
    virtual double get_position_seconds() = 0;
    virtual bool is_position_ready(double position_seconds) = 0;

    // Spectral Waveform already pays the Spleeter cost while progressively
    // building the Vocal/Instrumental waveform. Publish that exact separated
    // PCM into the transport cache so jog/reverse can reuse it immediately
    // instead of launching a second Spleeter pass for the same audio.
    // Buffers are interleaved and remain owned by the caller; this method copies
    // and normalizes them to the transport cache's internal 44.1-kHz stereo format.
    virtual bool publish_cache_block(
        const char* track_path_utf8,
        double start_seconds,
        const float* original,
        const float* vocals,
        const float* instrumental,
        t_size frames,
        unsigned channels,
        unsigned sample_rate) = 0;
};
