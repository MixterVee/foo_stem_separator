# foo_stem_separator

Native foobar2000 v2 component for live two-stem vocal / instrumental separation, packaged universally for Win32 and x64.

## Current stable: 2.8.2

### Playback

- Original / Vocals / Instrumental / Blend switching during playback
- seek-safe read-ahead caching and restore barrier to prevent stale stem leakage
- track-start pre-cache option so a selected stem can be ready from the first sample
- 44.1 kHz and 48 kHz source playback support
- clean end-of-track handling
- automatic gain matching option

### Export

- Save Vocals or Instrumental as 32-bit float WAV
- Save Vocals or Instrumental as 320 kbps MP3
- whole-track offline separation for clean exported files

### Persistent stem cache

- lossless compressed on-disk cache
- enabled by default
- default maximum: 10 GB
- selectable limits: 2, 5, 10, 20, 50, and 100 GB
- Current Cache size display
- Clear Stem Cache command
- bounded least-recently-used whole-track cleanup when the limit is exceeded

### Processing backends

- x64: CPU and DirectML GPU processing
- Win32/x86: CPU ONNX processing
- DirectML adapter discovery and benchmark / backend selection on x64
- saved GPU preference by hardware identity
- automatic CPU fallback when the preferred x64 GPU is unavailable

## Important DSP setup

Stem Separator must be present in foobar2000's active DSP chain for live audio replacement.

Open **Preferences → Playback → DSP Manager** and move **Stem Separator (ONNX)** to **Active DSPs**. 32-bit and 64-bit foobar2000 keep separate DSP configurations, so enable it in each installation you use.

## Requirements

- Windows 10/11
- foobar2000 v2, 32-bit or 64-bit
- Visual Studio 2022 for source builds
- foobar2000 SDK compatible with the repository build workflow

The packaged component includes the required ONNX runtime libraries and Spleeter two-stem models; Python and Demucs are not required for normal use.

## Build

The supported build is `.github/workflows/build.yml`. It builds one universal `.fb2k-component`: Win32 at the package root, x64 under `x64/`, with shared Spleeter models.
