FOO STEM SEPARATOR - ONNX V12 PROTOTYPE

This is the first native ONNX foobar2000 prototype.

WHAT CHANGES
------------
- No Python or Demucs is used during foobar playback.
- Native sherpa-onnx C API is loaded dynamically.
- Spleeter 2-stem model produces:
    vocals
    accompaniment/instrumental
- A foobar DSP named:
    Stem Separator (ONNX)
  is registered.
- Right-click menu commands change mode:
    Stem Separator / Original
    Stem Separator / Vocals
    Stem Separator / Instrumental

CURRENT LIMITS
--------------
- Stereo 44.1 kHz only in this prototype.
- Uses 1-second synchronous processing blocks.
- No overlap/crossfade yet.
- This is the architecture proof before the background-worker version.

INSTALL AFTER BUILD
-------------------
The GitHub artifact will include:
    foo_stem_separator.fb2k-component

Install that through:
    foobar2000 > File > Preferences > Components > Install

Then enable the DSP:
    Preferences > Playback > DSP Manager
    Add "Stem Separator (ONNX)" to Active DSPs.

Play a 44.1 kHz stereo song.

Right-click a track and select:
    Stem Separator / Vocals
or:
    Stem Separator / Instrumental

The mode is global and affects the active ONNX DSP.

NEXT MILESTONE
--------------
If this works and sounds acceptable:
- move separation to a background worker thread
- overlap/crossfade processing windows
- reduce switching latency
- add toolbar/hotkey mode controls
