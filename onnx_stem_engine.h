#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "stem_mode.h"

namespace onnxstem {

class engine {
public:
    engine();
    ~engine();

    engine(const engine&) = delete;
    engine& operator=(const engine&) = delete;

    bool ready();
    const std::wstring& last_error() const noexcept { return m_error; }

    bool process_interleaved(
        const float* input,
        size_t frames,
        unsigned channels,
        unsigned sample_rate,
        stemmode::mode mode,
        std::vector<float>& output);

private:
    bool initialize();
    void shutdown();

    std::wstring component_directory() const;
    std::wstring dll_path() const;
    std::wstring vocals_model_path() const;
    std::wstring accompaniment_model_path() const;

    // Keep Windows types out of this header so windows.h is not pulled in
    // before the foobar2000 SDK / Winsock2 headers.
    void* m_module = nullptr;
    const void* m_separator = nullptr;
    bool m_attempted = false;
    std::wstring m_error;

    struct api;
    api* m_api = nullptr;
};

} // namespace onnxstem
