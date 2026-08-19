# Build notes

The supported release build is the GitHub Actions workflow:

`.github/workflows/build.yml`

It targets Windows x64 / Visual Studio 2022 and performs the complete package build:

1. obtains the foobar2000 SDK;
2. builds `foo_stem_separator.dll` using `build-ci-v12.ps1`;
3. builds sherpa-onnx 1.13.4 with DirectML support;
4. bundles `sherpa-onnx-c-api.dll`, `onnxruntime.dll`, and `DirectML.dll`;
5. downloads the Spleeter two-stem FP16 models;
6. packages the stable `.fb2k-component` artifact.

`foo_stem_separator.cpp` is a thin release-metadata wrapper around the tested `foo_stem_separator_impl.cpp` implementation. Likewise, `build-ci-v12.ps1` prepares the implementation include and delegates the proven build logic to `build-ci-v12-impl.ps1`.

Earlier numbered build scripts were development snapshots and are intentionally not part of the stable tree.
