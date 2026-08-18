#include "persistent_stem_cache.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace persistent_stem_cache {
namespace {

constexpr uint32_t kMagic = 0x31435353u; // SSC1
constexpr uint32_t kVersion = 1;
constexpr uint32_t kRate = 44100;
constexpr uint32_t kChannels = 2;
constexpr uint64_t kMaxSegmentFrames = static_cast<uint64_t>(kRate) * 10u;

struct source_fingerprint {
    uint64_t path_hash = 0;
    uint64_t size = 0;
    uint64_t write_time = 0;
};

#pragma pack(push, 1)
struct file_header {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    uint32_t sample_rate = kRate;
    uint32_t channels = kChannels;
    uint64_t path_hash = 0;
    uint64_t source_size = 0;
    uint64_t source_write_time = 0;
    uint64_t start_frame = 0;
    uint64_t frame_count = 0;
};
#pragma pack(pop)

uint64_t hash_path(const std::wstring& path) {
    uint64_t h = 1469598103934665603ull;
    for (wchar_t ch : path) {
        const uint32_t v = static_cast<uint32_t>(std::towlower(ch));
        h ^= static_cast<uint64_t>(v & 0xffffu);
        h *= 1099511628211ull;
        h ^= static_cast<uint64_t>((v >> 16) & 0xffffu);
        h *= 1099511628211ull;
    }
    return h;
}

bool fingerprint(const std::wstring& path, source_fingerprint& out) {
    out = {};
    if (path.empty()) return false;

    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;

    ULARGE_INTEGER size{};
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;

    ULARGE_INTEGER stamp{};
    stamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
    stamp.LowPart = data.ftLastWriteTime.dwLowDateTime;

    out.path_hash = hash_path(path);
    out.size = size.QuadPart;
    out.write_time = stamp.QuadPart;
    return true;
}

fs::path root_path() {
    const DWORD needed = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (needed == 0) return {};

    std::vector<wchar_t> value(static_cast<size_t>(needed) + 1u, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        L"LOCALAPPDATA", value.data(), static_cast<DWORD>(value.size()));
    if (written == 0 || written >= value.size()) return {};

    return fs::path(value.data()) / L"foobar2000" / L"StemSeparatorCache" / L"v1";
}

std::wstring hex64(uint64_t value) {
    std::wostringstream text;
    text << std::hex << std::setw(16) << std::setfill(L'0') << value;
    return text.str();
}

fs::path track_dir(const source_fingerprint& fp) {
    const fs::path root = root_path();
    if (root.empty()) return {};
    return root / hex64(fp.path_hash);
}

bool header_matches(const file_header& h, const source_fingerprint& fp) {
    return h.magic == kMagic &&
        h.version == kVersion &&
        h.sample_rate == kRate &&
        h.channels == kChannels &&
        h.path_hash == fp.path_hash &&
        h.source_size == fp.size &&
        h.source_write_time == fp.write_time &&
        h.frame_count != 0 &&
        h.frame_count <= kMaxSegmentFrames;
}

} // namespace

std::vector<segment> load(const std::wstring& source_path) {
    std::vector<segment> out;

    source_fingerprint fp;
    if (!fingerprint(source_path, fp)) return out;

    const fs::path dir = track_dir(fp);
    if (dir.empty()) return out;

    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) return out;

    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto path = it->path();
        if (!it->is_regular_file(ec)) {
            ec.clear();
            continue;
        }
        if (_wcsicmp(path.extension().c_str(), L".ssc") != 0) continue;

        std::ifstream file(path, std::ios::binary);
        if (!file) continue;

        file_header h{};
        file.read(reinterpret_cast<char*>(&h), sizeof(h));
        if (!file || !header_matches(h, fp)) continue;

        const uint64_t values_per_stem = h.frame_count * kChannels;
        const uint64_t expected_bytes = sizeof(file_header) +
            values_per_stem * sizeof(float) * 2ull;
        const uint64_t actual_bytes = static_cast<uint64_t>(fs::file_size(path, ec));
        if (ec || actual_bytes != expected_bytes) {
            ec.clear();
            continue;
        }

        segment seg;
        seg.start_frame = h.start_frame;
        seg.vocals.resize(static_cast<size_t>(values_per_stem));
        seg.instrumental.resize(static_cast<size_t>(values_per_stem));

        file.read(
            reinterpret_cast<char*>(seg.vocals.data()),
            static_cast<std::streamsize>(seg.vocals.size() * sizeof(float)));
        file.read(
            reinterpret_cast<char*>(seg.instrumental.data()),
            static_cast<std::streamsize>(seg.instrumental.size() * sizeof(float)));
        if (!file) continue;

        out.push_back(std::move(seg));
    }

    std::sort(out.begin(), out.end(), [](const segment& a, const segment& b) {
        return a.start_frame < b.start_frame;
    });
    return out;
}

bool save(
    const std::wstring& source_path,
    uint64_t start_frame,
    const std::vector<float>& vocals,
    const std::vector<float>& instrumental) {

    if (source_path.empty() || vocals.empty() || vocals.size() != instrumental.size()) {
        return false;
    }
    if ((vocals.size() % kChannels) != 0) return false;

    const uint64_t frames = static_cast<uint64_t>(vocals.size() / kChannels);
    if (frames == 0 || frames > kMaxSegmentFrames) return false;

    source_fingerprint fp;
    if (!fingerprint(source_path, fp)) return false;

    const fs::path dir = track_dir(fp);
    if (dir.empty()) return false;

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return false;

    const fs::path final_path = dir / (std::to_wstring(start_frame) + L".ssc");
    const fs::path temp_path = dir /
        (std::to_wstring(start_frame) + L".tmp-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));

    file_header h;
    h.path_hash = fp.path_hash;
    h.source_size = fp.size;
    h.source_write_time = fp.write_time;
    h.start_frame = start_frame;
    h.frame_count = frames;

    std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
    if (!file) return false;

    file.write(reinterpret_cast<const char*>(&h), sizeof(h));
    file.write(
        reinterpret_cast<const char*>(vocals.data()),
        static_cast<std::streamsize>(vocals.size() * sizeof(float)));
    file.write(
        reinterpret_cast<const char*>(instrumental.data()),
        static_cast<std::streamsize>(instrumental.size() * sizeof(float)));
    file.flush();
    file.close();

    if (!file) {
        fs::remove(temp_path, ec);
        return false;
    }

    if (!MoveFileExW(
            temp_path.c_str(), final_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(temp_path, ec);
        return false;
    }

    return true;
}

} // namespace persistent_stem_cache
