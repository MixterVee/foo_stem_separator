#include "stem_engine.h"
#include <ShlObj.h>
#include <bcrypt.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;

namespace stemsep {

static std::wstring quote(const std::wstring& s) {
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    out += L"\"";
    return out;
}

static std::wstring sha256_hex(const std::wstring& text) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objLen = 0, cb = 0, hashLen = 0;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return L"hash-error";

    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cb, 0);
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(&hashLen), sizeof(hashLen), &cb, 0);

    std::vector<UCHAR> obj(objLen), digest(hashLen);

    if (BCryptCreateHash(alg, &hash, obj.data(), objLen, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return L"hash-error";
    }

    const auto* bytes = reinterpret_cast<const UCHAR*>(text.data());
    ULONG byteCount = static_cast<ULONG>(text.size() * sizeof(wchar_t));
    BCryptHashData(hash, const_cast<PUCHAR>(bytes), byteCount, 0);
    BCryptFinishHash(hash, digest.data(), hashLen, 0);

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);

    std::wostringstream ss;
    for (auto b : digest)
        ss << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<int>(b);
    return ss.str();
}

std::wstring cache_root() {
    wchar_t* p = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
        result = p;
        CoTaskMemFree(p);
    } else {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        result = temp;
    }
    result += L"\\foo_stem_separator\\cache";
    fs::create_directories(result);
    return result;
}

std::wstring track_cache_dir(const std::wstring& sourcePath) {
    fs::path src(sourcePath);
    std::wstring identity = fs::absolute(src).wstring();

    std::error_code ec;
    auto size = fs::file_size(src, ec);
    if (!ec) identity += L"|" + std::to_wstring(size);

    auto stamp = fs::last_write_time(src, ec);
    if (!ec) {
        identity += L"|" + std::to_wstring(stamp.time_since_epoch().count());
    }

    return (fs::path(cache_root()) / sha256_hex(identity).substr(0, 24)).wstring();
}

static fs::path demucs_model_dir(const std::wstring& sourcePath) {
    return fs::path(track_cache_dir(sourcePath)) / L"htdemucs";
}

static fs::path demucs_track_dir(const std::wstring& sourcePath) {
    fs::path src(sourcePath);
    // Demucs normally uses the filename without extension as the output folder.
    return demucs_model_dir(sourcePath) / src.stem();
}

std::wstring expected_stem_path(const std::wstring& sourcePath, stem_kind kind) {
    const wchar_t* filename =
        kind == stem_kind::vocals ? L"vocals.wav" : L"no_vocals.wav";
    return (demucs_track_dir(sourcePath) / filename).wstring();
}

bool stem_is_cached(const std::wstring& sourcePath, stem_kind kind) {
    std::error_code ec;
    auto p = expected_stem_path(sourcePath, kind);
    return fs::exists(p, ec) && fs::file_size(p, ec) > 44;
}

static result run_demucs(const std::wstring& sourcePath, stem_kind kind) {
    result r;
    r.source_path = sourcePath;
    r.cache_dir = track_cache_dir(sourcePath);
    r.stem_path = expected_stem_path(sourcePath, kind);

    if (stem_is_cached(sourcePath, kind)) {
        r.ok = true;
        return r;
    }

    fs::create_directories(r.cache_dir);

    // Use the Python command available on this Windows system.
    std::wstring cmd =
        L"python -m demucs --two-stems=vocals -n htdemucs -o " +
        quote(r.cache_dir) + L" " + quote(sourcePath);

    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessW(
        nullptr,
        mutableCmd.data(),
        nullptr, nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi);

    if (!ok) {
        r.error = L"Could not start Python/Demucs. Windows error " +
                  std::to_wstring(GetLastError());
        return r;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &r.exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (r.exit_code != 0) {
        r.error = L"Demucs returned exit code " + std::to_wstring(r.exit_code);
        return r;
    }

    if (!stem_is_cached(sourcePath, kind)) {
        r.error = L"Demucs finished but the expected stem file was not found: " +
                  r.stem_path;
        return r;
    }

    r.ok = true;
    return r;
}

result separate_track(const std::wstring& sourcePath, stem_kind kind) {
    return run_demucs(sourcePath, kind);
}

bool delete_track_cache(const std::wstring& sourcePath) {
    std::error_code ec;
    auto dir = track_cache_dir(sourcePath);
    if (!fs::exists(dir, ec)) return true;
    fs::remove_all(dir, ec);
    return !ec;
}

}
