#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace onnxstem {

// Backend values are intentionally encoded so any DirectML adapter index can be
// represented without hard-coding adapter 0 / adapter 1.
enum class backend : int {
    selected = -1,
    cpu = 0
};

struct directml_adapter_info {
    unsigned index = 0;
    std::wstring name;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t subsys_id = 0;
    uint32_t revision = 0;
    uint32_t duplicate_ordinal = 0;
};

backend directml_backend(unsigned adapter_index);
bool is_directml_backend(backend value);
unsigned directml_adapter_index(backend value);

std::vector<directml_adapter_info> enumerate_directml_adapters();
backend selected_backend();
void select_backend(backend value);
std::wstring backend_name(backend value);

struct runtime_status {
    backend active_backend = backend::selected;
    bool engine_ready = false;
    bool using_fallback = false;
    bool processing = false;
    std::wstring backend_label;
};

runtime_status current_runtime_status();
bool selected_backend_preference_is_gpu();

class engine {
public:
    explicit engine(backend requested = backend::selected);
    ~engine();

    engine(const engine&) = delete;
    engine& operator=(const engine&) = delete;

    bool ready();
    const std::wstring& last_error() const noexcept { return m_error; }
    backend active_backend() const noexcept { return m_active_backend; }
    bool using_fallback() const noexcept {
        return m_requested_backend == backend::selected &&
            m_separator != nullptr && m_active_backend == backend::cpu &&
            (is_directml_backend(m_desired_backend) || selected_backend_preference_is_gpu());
    }

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
    bool initialize_backend(backend value);
    void shutdown();

    backend desired_backend() const;

    std::wstring component_directory() const;
    std::wstring dll_path() const;
    std::wstring vocals_model_path() const;
    std::wstring accompaniment_model_path() const;
    bool write_directml_config(
        unsigned adapter_index,
        std::wstring& path,
        std::wstring& error) const;

    backend m_requested_backend = backend::selected;
    backend m_desired_backend = backend::selected;
    backend m_active_backend = backend::selected;

    void* m_module = nullptr;
    const void* m_separator = nullptr;
    bool m_attempted = false;
    std::wstring m_error;

    struct api;
    api* m_api = nullptr;
};

} // namespace onnxstem

