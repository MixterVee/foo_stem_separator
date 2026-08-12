# Build notes

1. Download foobar2000 SDK 2025-03-07 from the official foobar2000 SDK page.
2. Copy this project's `src` folder beside/into a normal SDK component sample.
3. Use the SDK's x64 Visual Studio 2022 configuration.
4. Add:
   - foo_stem_separator.cpp
   - stem_engine.cpp
   - stem_engine.h
5. Link:
   - bcrypt.lib
   - shell32.lib
6. Build as `foo_stem_separator.dll`.

## One remaining SDK integration point

The Demucs/cache engine is independent of foobar2000 and implemented.

The final build needs the exact current-SDK call for:
- posting the completed worker result to the foobar main thread;
- resolving the generated WAV into a metadb handle;
- adding it to the active playlist;
- starting playback.

That section is marked `TODO FINAL SDK WIRING` in `foo_stem_separator.cpp`.

It should be filled using the current SDK's own sample code rather than using
an old API signature from memory.
