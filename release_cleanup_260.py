from pathlib import Path

source_path = Path('foo_stem_separator.cpp')
source = source_path.read_text(encoding='utf-8-sig')
old = '''DECLARE_COMPONENT_VERSION(
    "Stem Separator",
    "2.5.0 Dynamic DirectML GPU backends + benchmark",
    "Native ONNX vocals / instrumental separation.\\n"
    "Zero-latency position-cache playback with optional start pre-cache and clean WAV/MP3 export.\\n"
    "Live stems use independent read-ahead caching; export uses whole-track Spleeter inference with WAV or 320 kbps MP3 output."
);'''
new = '''DECLARE_COMPONENT_VERSION(
    "Stem Separator",
    "2.6.0 Persistent Cache & Cache Settings",
    "Native ONNX vocals / instrumental separation.\\n"
    "Live Original / Vocals / Instrumental switching with seek-safe cached playback and clean WAV/MP3 export.\\n"
    "Lossless compressed persistent stem cache with configurable size, cache status/clear controls, start pre-cache, and dynamic DirectML/CPU backend benchmarking."
);'''
if old not in source:
    raise SystemExit('Stem component version block not found')
source_path.write_text(source.replace(old, new, 1), encoding='utf-8', newline='\n')

build_path = Path('.github/workflows/build.yml')
build = build_path.read_text(encoding='utf-8-sig')
old_backend = 'Stem Separator 2.5.0 dynamic DirectML processing backend'
new_backend = 'Stem Separator 2.6.0 persistent cache + dynamic DirectML processing backend'
if old_backend not in build:
    raise SystemExit('Stable backend metadata line not found')
build_path.write_text(build.replace(old_backend, new_backend, 1), encoding='utf-8', newline='\n')

Path('README.md').write_text('''# foo_stem_separator

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
''', encoding='utf-8', newline='\n')

Path('BUILD_NOTES.md').write_text('''# Build notes

The supported release build is the GitHub Actions workflow:

`.github/workflows/build.yml`

It targets Windows x64 / Visual Studio 2022 and performs the complete package build:

1. obtains the foobar2000 SDK;
2. builds `foo_stem_separator.dll` using `build-ci-v12.ps1`;
3. builds sherpa-onnx 1.13.4 with DirectML support;
4. bundles `sherpa-onnx-c-api.dll`, `onnxruntime.dll`, and `DirectML.dll`;
5. downloads the Spleeter two-stem FP16 models;
6. packages the stable `.fb2k-component` artifact.

`build-ci-v12.ps1` is the current source build script. Earlier numbered build scripts were development snapshots and are intentionally not part of the stable tree.
''', encoding='utf-8', newline='\n')

obsolete = [
    'build-ci.ps1',
    'build-ci-v2.ps1', 'build-ci-v3.ps1', 'build-ci-v4.ps1', 'build-ci-v5.ps1',
    'build-ci-v6.ps1', 'build-ci-v7.ps1', 'build-ci-v8.ps1', 'build-ci-v9.ps1',
    'build-ci-v10.ps1', 'build-ci-v11.ps1',
]
for name in obsolete:
    p = Path(name)
    if p.exists():
        p.unlink()
