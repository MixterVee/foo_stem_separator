# foo_stem_separator

Native foobar2000 v2 x64 component for live two-stem vocal / instrumental separation.

## Current stable: 2.6.0

### Playback

- Original / Vocals / Instrumental switching during playback
- seek-safe read-ahead caching and restore barrier to prevent stale stem leakage
- track-start pre-cache option so a selected stem can be ready from the first sample
- 44.1 kHz and 48 kHz source playback support
- clean end-of-track handling

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

- CPU processing
- DirectML GPU adapter discovery
- benchmark / backend selection
- saved GPU preference by hardware identity
- automatic CPU fallback when the preferred GPU is unavailable

## Requirements

- Windows 10/11 x64
- foobar2000 v2 x64
- Visual Studio 2022 for source builds
- foobar2000 SDK compatible with the repository build workflow

The packaged component includes the required ONNX runtime libraries and Spleeter two-stem models; Python and Demucs are not required for normal use.

## Build

The supported build is `.github/workflows/build.yml`. It uses `build-ci-v12.ps1`, builds the component and DirectML runtime, downloads the Spleeter models, and packages `foo_stem_separator.fb2k-component`.
