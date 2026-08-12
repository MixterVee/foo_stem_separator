# foo_stem_separator v0.1

Experimental foobar2000 component project for AI vocal / instrumental separation.

## Goal

Right-click one or more local tracks in foobar2000 and choose:

- Stem Separator > Play Instrumental
- Stem Separator > Play Vocals
- Stem Separator > Generate Stems
- Stem Separator > Delete Cached Stems

The original audio file is never modified.

## Separation engine

v0.1 uses Demucs in two-stem vocal mode:

    python -m demucs --two-stems=vocals -n htdemucs -o "<cache>" "<track>"

Demucs creates:

    vocals.wav
    no_vocals.wav

`no_vocals.wav` is the instrumental stem.

## Cache

Default intended cache root:

    %LOCALAPPDATA%\foo_stem_separator\cache

Each source file gets its own hashed cache directory so files with identical
names in different folders do not collide.

## Why this is not yet a live DSP

Demucs separation is computationally expensive and should not run in the
foobar2000 real-time audio callback. v0.1 separates in a worker process,
caches the result, then opens/plays the cached stem.

A later version can add a Serato-style Original / Vocal / Instrumental switch
that selects already-cached audio instantly.

## Requirements

- Windows 10/11 x64
- foobar2000 v2 x64
- foobar2000 SDK 2025-03-07
- Visual Studio 2022
- Python 3
- Demucs

Install Demucs:

    py -m pip install -U demucs

Test outside foobar first:

    py -m demucs --two-stems=vocals -n htdemucs "C:\Music\test.flac"

## Project status

This package is the v0.1 engineering scaffold. `stem_engine.cpp` contains the
Windows process/cache implementation. `foo_stem_separator.cpp` contains the
foobar2000 component/menu layer and marked integration points for the exact
SDK callback/playback calls used by the final build.

The separation engine can already be tested independently with
`tools\stem_test.py`.
