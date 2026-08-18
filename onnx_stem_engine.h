#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace onnxstem {

enum class backend : int {
    selected = -1,
    cpu = 0,
    directml_adapter0 = 1,
    directml_adapter1 = 2
};

backend selected_backend();
void select_backend(backend value);
const wchar_t* backend_name(backend value);

class engine {
public:
    explicit engine(backend requested = backend::selected);
    ~engine();

    engine(const engine&) = delete;
    engine& operator=(const engine&) = delete;

    bool ready();
    const std::wstring& last_error() const noexcept { return m_error; }

    // Separates one stereo 44.1-kHz interleaved block and returns BOTH stems
    // from a single sherpa-onnx inference call.
    bool process_both(
        const float* input,
        size_t frames,
        unsigned channels,
        unsigned sample_rate,
        std::vector<float>& vocals,
        std::vector<float>& instrumental);

private:
    bool initialize();
    void shutdown();

    backend desired_backend() const;

    std::wstring component_directory() const;
    std::wstring dll_path() const;
    std::wstring vocals_model_path() const;
    std::wstring accompaniment_model_path() const;
    std::wstring directml_config_path(backend value) const;

    backend m_requested_backend = backend::selected;
    backend m_active_backend = backend::selected;

    void* m_module = nullptr;
    const void* m_separator = nullptr;
    bool m_attempted = false;
    std::wstring m_error;

    struct api;
    api* m_api = nullptr;
};

} // namespace onnxstem
