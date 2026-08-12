#pragma once
#include <windows.h>
#include <filesystem>
#include <string>

namespace stemsep {

enum class stem_kind {
    vocals,
    instrumental
};

struct result {
    bool ok = false;
    DWORD exit_code = 0;
    std::wstring source_path;
    std::wstring stem_path;
    std::wstring cache_dir;
    std::wstring error;
};

std::wstring cache_root();
std::wstring track_cache_dir(const std::wstring& sourcePath);
std::wstring expected_stem_path(const std::wstring& sourcePath, stem_kind kind);
bool stem_is_cached(const std::wstring& sourcePath, stem_kind kind);
result separate_track(const std::wstring& sourcePath, stem_kind kind);
bool delete_track_cache(const std::wstring& sourcePath);

}
