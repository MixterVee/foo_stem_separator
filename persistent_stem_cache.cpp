#include "persistent_stem_cache.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <compressapi.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <vector>

#pragma comment(lib, "Cabinet.lib")

namespace fs = std::filesystem;

namespace persistent_stem_cache {
namespace {

constexpr uint32_t kMagic = 0x31435353u; // SSC1
constexpr uint32_t kLegacyVersion = 1;
constexpr uint32_t kVersion = 2;
constexpr uint32_t kRate = 44100;
constexpr uint32_t kChannels = 2;
constexpr uint64_t kMaxSegmentFrames = static_cast<uint64_t>(kRate) * 10u;
constexpr uint64_t kBytesPerGiB = 1024ull * 1024ull * 1024ull;
constexpr ULONGLONG kCleanupIntervalMs = 60ull * 1000ull;
constexpr wchar_t kAccessMarkerName[] = L".access";

constexpr uint32_t kCodecRawFloat = 0;
constexpr uint32_t kCodecXpressHuffDeltaFloat = 1;

std::mutex g_cache_mutex;
ULONGLONG g_last_cleanup_tick = 0;

uint64_t configured_max_bytes() {
    return max_bytes();
}

uint64_t configured_trim_bytes() {
    return configured_max_bytes() * 4ull / 5ull;
}

struct source_fingerprint {
    uint64_t path_hash = 0;
    uint64_t size = 0;
    uint64_t write_time = 0;
};

struct track_cache_info {
    fs::path path;
    uint64_t bytes = 0;
    fs::file_time_type last_access = fs::file_time_type::min();
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

struct file_header_v2_extra {
    uint32_t codec = kCodecRawFloat;
    uint32_t reserved = 0;
    uint64_t raw_payload_bytes = 0;
    uint64_t stored_payload_bytes = 0;
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
        (h.version == kLegacyVersion || h.version == kVersion) &&
        h.sample_rate == kRate &&
        h.channels == kChannels &&
        h.path_hash == fp.path_hash &&
        h.source_size == fp.size &&
        h.source_write_time == fp.write_time &&
        h.frame_count != 0 &&
        h.frame_count <= kMaxSegmentFrames;
}

void touch_access_marker(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return;

    const fs::path marker = dir / kAccessMarkerName;
    std::ofstream file(marker, std::ios::binary | std::ios::trunc);
    if (!file) return;
    file.put('1');
    file.close();
}

track_cache_info inspect_track_cache(const fs::path& dir) {
    track_cache_info info;
    info.path = dir;

    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            ec.clear();
            continue;
        }

        const uint64_t size = static_cast<uint64_t>(it->file_size(ec));
        if (!ec) info.bytes += size;
        ec.clear();

        const fs::file_time_type stamp = it->last_write_time(ec);
        if (!ec && stamp > info.last_access) info.last_access = stamp;
        ec.clear();
    }

    const fs::path marker = dir / kAccessMarkerName;
    const fs::file_time_type marker_time = fs::last_write_time(marker, ec);
    if (!ec) info.last_access = marker_time;

    return info;
}

void prune_cache_locked(const fs::path& protected_dir) {
    const fs::path root = root_path();
    if (root.empty()) return;

    std::error_code ec;
    if (!fs::exists(root, ec) || ec) return;

    std::vector<track_cache_info> tracks;
    uint64_t total_bytes = 0;

    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) {
            ec.clear();
            continue;
        }

        track_cache_info info = inspect_track_cache(it->path());
        total_bytes += info.bytes;
        tracks.push_back(std::move(info));
    }

    if (total_bytes <= configured_max_bytes()) return;

    std::sort(tracks.begin(), tracks.end(), [](const track_cache_info& a, const track_cache_info& b) {
        return a.last_access < b.last_access;
    });

    const fs::path protected_normal = protected_dir.lexically_normal();

    for (const auto& track : tracks) {
        if (total_bytes <= configured_trim_bytes()) break;
        if (!protected_dir.empty() && track.path.lexically_normal() == protected_normal) continue;

        const uint64_t removed_bytes = track.bytes;
        fs::remove_all(track.path, ec);
        if (!ec) {
            total_bytes = total_bytes > removed_bytes
                ? total_bytes - removed_bytes
                : 0;
        }
        ec.clear();
    }
}

void maybe_prune_cache_locked(const fs::path& protected_dir) {
    const ULONGLONG now = GetTickCount64();
    if (g_last_cleanup_tick != 0 && now - g_last_cleanup_tick < kCleanupIntervalMs) {
        return;
    }
    g_last_cleanup_tick = now;
    prune_cache_locked(protected_dir);
}

bool xpress_huff_compress(
    const std::vector<unsigned char>& input,
    std::vector<unsigned char>& output) {

    output.clear();
    if (input.empty()) return false;

    COMPRESSOR_HANDLE compressor = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &compressor)) {
        return false;
    }

    SIZE_T required = 0;
    const BOOL first = Compress(
        compressor,
        const_cast<unsigned char*>(input.data()),
        input.size(),
        nullptr,
        0,
        &required);

    if (!first && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        CloseCompressor(compressor);
        return false;
    }
    if (required == 0) {
        CloseCompressor(compressor);
        return false;
    }

    output.resize(required);
    SIZE_T actual = 0;
    const BOOL ok = Compress(
        compressor,
        const_cast<unsigned char*>(input.data()),
        input.size(),
        output.data(),
        output.size(),
        &actual);
    CloseCompressor(compressor);

    if (!ok || actual == 0 || actual > output.size()) {
        output.clear();
        return false;
    }

    output.resize(actual);
    return true;
}

bool xpress_huff_decompress(
    const std::vector<unsigned char>& input,
    size_t expected_size,
    std::vector<unsigned char>& output) {

    output.clear();
    if (input.empty() || expected_size == 0) return false;

    DECOMPRESSOR_HANDLE decompressor = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &decompressor)) {
        return false;
    }

    output.resize(expected_size);
    SIZE_T actual = 0;
    const BOOL ok = Decompress(
        decompressor,
        const_cast<unsigned char*>(input.data()),
        input.size(),
        output.data(),
        output.size(),
        &actual);
    CloseDecompressor(decompressor);

    if (!ok || actual != expected_size) {
        output.clear();
        return false;
    }
    return true;
}

void append_delta_float_bytes(
    const std::vector<float>& stem,
    std::vector<unsigned char>& output) {

    const size_t base = output.size();
    output.resize(base + stem.size() * sizeof(float));

    uint32_t previous[kChannels] = {};
    for (size_t i = 0; i < stem.size(); ++i) {
        uint32_t bits = 0;
        std::memcpy(&bits, &stem[i], sizeof(bits));

        const size_t channel = i % kChannels;
        const uint32_t delta = bits ^ previous[channel];
        previous[channel] = bits;

        std::memcpy(
            output.data() + base + i * sizeof(delta),
            &delta,
            sizeof(delta));
    }
}

bool decode_delta_float_bytes(
    const unsigned char* data,
    size_t values,
    std::vector<float>& output) {

    if (!data || values == 0) return false;

    output.resize(values);
    uint32_t previous[kChannels] = {};

    for (size_t i = 0; i < values; ++i) {
        uint32_t delta = 0;
        std::memcpy(&delta, data + i * sizeof(delta), sizeof(delta));

        const size_t channel = i % kChannels;
        const uint32_t bits = delta ^ previous[channel];
        previous[channel] = bits;

        std::memcpy(&output[i], &bits, sizeof(bits));
    }
    return true;
}

void build_raw_float_payload(
    const std::vector<float>& vocals,
    const std::vector<float>& instrumental,
    std::vector<unsigned char>& output) {

    const size_t stem_bytes = vocals.size() * sizeof(float);
    output.resize(stem_bytes * 2u);
    std::memcpy(output.data(), vocals.data(), stem_bytes);
    std::memcpy(output.data() + stem_bytes, instrumental.data(), stem_bytes);
}

bool decode_v2_payload(
    const file_header_v2_extra& extra,
    const std::vector<unsigned char>& stored,
    size_t values_per_stem,
    segment& seg) {

    const size_t stem_bytes = values_per_stem * sizeof(float);
    const size_t expected_raw_bytes = stem_bytes * 2u;

    if (extra.raw_payload_bytes != expected_raw_bytes ||
        extra.stored_payload_bytes != stored.size()) {
        return false;
    }

    if (extra.codec == kCodecRawFloat) {
        if (stored.size() != expected_raw_bytes) return false;

        seg.vocals.resize(values_per_stem);
        seg.instrumental.resize(values_per_stem);
        std::memcpy(seg.vocals.data(), stored.data(), stem_bytes);
        std::memcpy(seg.instrumental.data(), stored.data() + stem_bytes, stem_bytes);
        return true;
    }

    if (extra.codec == kCodecXpressHuffDeltaFloat) {
        std::vector<unsigned char> delta_payload;
        if (!xpress_huff_decompress(stored, expected_raw_bytes, delta_payload)) {
            return false;
        }

        return decode_delta_float_bytes(
                   delta_payload.data(), values_per_stem, seg.vocals) &&
            decode_delta_float_bytes(
                delta_payload.data() + stem_bytes,
                values_per_stem,
                seg.instrumental);
    }

    return false;
}

} // namespace

uint64_t max_bytes() {
    return static_cast<uint64_t>(max_gb()) * kBytesPerGiB;
}

namespace detail {
void enforce_limit() {
    std::lock_guard<std::mutex> guard(g_cache_mutex);
    g_last_cleanup_tick = 0;
    prune_cache_locked({});
}
} // namespace detail

uint64_t current_size_bytes() {
    std::lock_guard<std::mutex> guard(g_cache_mutex);
    const fs::path root = root_path();
    if (root.empty()) return 0;

    std::error_code ec;
    if (!fs::exists(root, ec) || ec) return 0;

    uint64_t total = 0;
    for (fs::recursive_directory_iterator it(root, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            ec.clear();
            continue;
        }
        const uint64_t bytes = static_cast<uint64_t>(it->file_size(ec));
        if (!ec) total += bytes;
        ec.clear();
    }
    return total;
}

bool clear() {
    std::lock_guard<std::mutex> guard(g_cache_mutex);
    const fs::path root = root_path();
    if (root.empty()) return false;

    std::error_code ec;
    if (fs::exists(root, ec) && !ec) {
        fs::remove_all(root, ec);
        if (ec) return false;
    }
    g_last_cleanup_tick = 0;
    return true;
}

std::vector<segment> load(const std::wstring& source_path) {
    std::lock_guard<std::mutex> guard(g_cache_mutex);
    std::vector<segment> out;

    if (!enabled()) return out;

    source_fingerprint fp;
    if (!fingerprint(source_path, fp)) return out;

    const fs::path dir = track_dir(fp);
    if (dir.empty()) return out;

    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) {
        maybe_prune_cache_locked({});
        return out;
    }

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

        const uint64_t values_per_stem_u64 = h.frame_count * kChannels;
        const uint64_t raw_payload_bytes =
            values_per_stem_u64 * sizeof(float) * 2ull;
        const uint64_t actual_bytes = static_cast<uint64_t>(fs::file_size(path, ec));
        if (ec) {
            ec.clear();
            continue;
        }

        segment seg;
        seg.start_frame = h.start_frame;
        const size_t values_per_stem = static_cast<size_t>(values_per_stem_u64);

        if (h.version == kLegacyVersion) {
            const uint64_t expected_bytes = sizeof(file_header) + raw_payload_bytes;
            if (actual_bytes != expected_bytes) continue;

            seg.vocals.resize(values_per_stem);
            seg.instrumental.resize(values_per_stem);

            file.read(
                reinterpret_cast<char*>(seg.vocals.data()),
                static_cast<std::streamsize>(seg.vocals.size() * sizeof(float)));
            file.read(
                reinterpret_cast<char*>(seg.instrumental.data()),
                static_cast<std::streamsize>(seg.instrumental.size() * sizeof(float)));
            if (!file) continue;
        }
        else {
            file_header_v2_extra extra{};
            file.read(reinterpret_cast<char*>(&extra), sizeof(extra));
            if (!file || extra.raw_payload_bytes != raw_payload_bytes ||
                extra.stored_payload_bytes == 0 ||
                extra.stored_payload_bytes > raw_payload_bytes) {
                continue;
            }

            const uint64_t expected_bytes = sizeof(file_header) + sizeof(extra) +
                extra.stored_payload_bytes;
            if (actual_bytes != expected_bytes) continue;

            std::vector<unsigned char> stored(
                static_cast<size_t>(extra.stored_payload_bytes));
            file.read(
                reinterpret_cast<char*>(stored.data()),
                static_cast<std::streamsize>(stored.size()));
            if (!file || !decode_v2_payload(extra, stored, values_per_stem, seg)) {
                continue;
            }
        }

        out.push_back(std::move(seg));
    }

    std::sort(out.begin(), out.end(), [](const segment& a, const segment& b) {
        return a.start_frame < b.start_frame;
    });

    if (!out.empty()) touch_access_marker(dir);
    maybe_prune_cache_locked(dir);
    return out;
}

bool save(
    const std::wstring& source_path,
    uint64_t start_frame,
    const std::vector<float>& vocals,
    const std::vector<float>& instrumental) {

    std::lock_guard<std::mutex> guard(g_cache_mutex);

    if (!enabled()) return false;

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

    std::vector<unsigned char> delta_payload;
    delta_payload.reserve(vocals.size() * sizeof(float) * 2u);
    append_delta_float_bytes(vocals, delta_payload);
    append_delta_float_bytes(instrumental, delta_payload);

    std::vector<unsigned char> compressed;
    const bool compressed_ok = xpress_huff_compress(delta_payload, compressed) &&
        compressed.size() < delta_payload.size();

    std::vector<unsigned char> raw_payload;
    const std::vector<unsigned char>* stored_payload = &compressed;

    file_header_v2_extra extra;
    extra.raw_payload_bytes = static_cast<uint64_t>(delta_payload.size());

    if (compressed_ok) {
        extra.codec = kCodecXpressHuffDeltaFloat;
        extra.stored_payload_bytes = static_cast<uint64_t>(compressed.size());
    }
    else {
        build_raw_float_payload(vocals, instrumental, raw_payload);
        stored_payload = &raw_payload;
        extra.codec = kCodecRawFloat;
        extra.stored_payload_bytes = static_cast<uint64_t>(raw_payload.size());
    }

    std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
    if (!file) return false;

    file.write(reinterpret_cast<const char*>(&h), sizeof(h));
    file.write(reinterpret_cast<const char*>(&extra), sizeof(extra));
    file.write(
        reinterpret_cast<const char*>(stored_payload->data()),
        static_cast<std::streamsize>(stored_payload->size()));
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

    touch_access_marker(dir);
    maybe_prune_cache_locked(dir);
    return true;
}

} // namespace persistent_stem_cache
