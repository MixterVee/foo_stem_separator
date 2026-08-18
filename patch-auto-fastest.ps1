$ErrorActionPreference = 'Stop'
$nl = [Environment]::NewLine

function Replace-Once([string]$text, [string]$old, [string]$new, [string]$label) {
    if (-not $text.Contains($old)) { throw "Patch marker not found: $label" }
    return $text.Replace($old, $new)
}

# -----------------------------------------------------------------------------
# Engine public API
# -----------------------------------------------------------------------------
$headerPath = Join-Path $env:GITHUB_WORKSPACE 'onnx_stem_engine.h'
$header = Get-Content $headerPath -Raw
$oldHeader = @'
backend selected_backend();
void select_backend(backend value);
std::wstring backend_name(backend value);
'@
$newHeader = @'
backend selected_backend();
void select_backend(backend value);
void remember_auto_backend(backend fastest);
void select_auto_backend();
bool selected_backend_preference_is_auto();
backend auto_backend();
std::wstring auto_backend_name();
std::wstring backend_name(backend value);
'@
$header = Replace-Once $header $oldHeader $newHeader 'engine auto API declarations'
Set-Content $headerPath $header -Encoding UTF8

# -----------------------------------------------------------------------------
# Persistent Auto winner + runtime resolution
# -----------------------------------------------------------------------------
$cppPath = Join-Path $env:GITHUB_WORKSPACE 'onnx_stem_engine.cpp'
$cpp = Get-Content $cppPath -Raw

$cfgMarker = @'
cfg_int g_backend_ordinal_cfg(g_backend_ordinal_guid, 0);

std::mutex g_runtime_status_mutex;
'@
$cfgBlock = @'
cfg_int g_backend_ordinal_cfg(g_backend_ordinal_guid, 0);

// Auto / Fastest stores the latest successful benchmark winner separately from
// the user's manual CPU/GPU preference. This lets a benchmark refresh Auto's
// winner without changing a manually selected backend.
static const GUID g_auto_kind_guid =
    {0x676c1ff0,0x3772,0x4cad,{0xb1,0x10,0xf7,0x6b,0x44,0xe8,0xf0,0x11}};
static const GUID g_auto_vendor_guid =
    {0x676c1ff1,0x3772,0x4cad,{0xb1,0x10,0xf7,0x6b,0x44,0xe8,0xf0,0x11}};
static const GUID g_auto_device_guid =
    {0x676c1ff2,0x3772,0x4cad,{0xb1,0x10,0xf7,0x6b,0x44,0xe8,0xf0,0x11}};
static const GUID g_auto_subsys_guid =
    {0x676c1ff3,0x3772,0x4cad,{0xb1,0x10,0xf7,0x6b,0x44,0xe8,0xf0,0x11}};
static const GUID g_auto_revision_guid =
    {0x676c1ff4,0x3772,0x4cad,{0xb1,0x10,0xf7,0x6b,0x44,0xe8,0xf0,0x11}};
static const GUID g_auto_ordinal_guid =
    {0x676c1ff5,0x3772,0x4cad,{0xb1,0x10,0xf7,0x6b,0x44,0xe8,0xf0,0x11}};

cfg_int g_auto_kind_cfg(g_auto_kind_guid, -1); // -1 none, 0 CPU, 1 DirectML
cfg_int g_auto_vendor_cfg(g_auto_vendor_guid, 0);
cfg_int g_auto_device_cfg(g_auto_device_guid, 0);
cfg_int g_auto_subsys_cfg(g_auto_subsys_guid, 0);
cfg_int g_auto_revision_cfg(g_auto_revision_guid, 0);
cfg_int g_auto_ordinal_cfg(g_auto_ordinal_guid, 0);

std::mutex g_runtime_status_mutex;
'@
$cpp = Replace-Once $cpp $cfgMarker $cfgBlock 'Auto config block'

$saveMarker = 'void save_cpu_selection() {'
$helperBlock = @'
bool matches_saved_auto_hardware(const onnxstem::directml_adapter_info& a) {
    return a.vendor_id == static_cast<uint32_t>(g_auto_vendor_cfg.get()) &&
        a.device_id == static_cast<uint32_t>(g_auto_device_cfg.get()) &&
        a.subsys_id == static_cast<uint32_t>(g_auto_subsys_cfg.get()) &&
        a.revision == static_cast<uint32_t>(g_auto_revision_cfg.get()) &&
        a.duplicate_ordinal == static_cast<uint32_t>(g_auto_ordinal_cfg.get());
}

onnxstem::backend resolve_auto_backend(
    const std::vector<onnxstem::directml_adapter_info>& adapters) {

    const int kind = static_cast<int>(g_auto_kind_cfg.get());
    if (kind == 0) return onnxstem::backend::cpu;

    if (kind == 1) {
        for (const auto& a : adapters) {
            if (matches_saved_auto_hardware(a)) {
                return onnxstem::directml_backend(a.index);
            }
        }
    }

    // No benchmark winner yet, or the benchmark-winning GPU is missing.
    // Auto remains selected but resolves safely to CPU until a valid winner
    // is available again.
    return onnxstem::backend::cpu;
}

void save_auto_cpu_winner() {
    g_auto_kind_cfg = 0;
}

void save_auto_gpu_winner(const onnxstem::directml_adapter_info& a) {
    g_auto_kind_cfg = 1;
    g_auto_vendor_cfg = static_cast<int32_t>(a.vendor_id);
    g_auto_device_cfg = static_cast<int32_t>(a.device_id);
    g_auto_subsys_cfg = static_cast<int32_t>(a.subsys_id);
    g_auto_revision_cfg = static_cast<int32_t>(a.revision);
    g_auto_ordinal_cfg = static_cast<int32_t>(a.duplicate_ordinal);
}

'@
$cpp = Replace-Once $cpp $saveMarker ($helperBlock + $saveMarker) 'Auto helper functions'

$migrationMarker = @'
    // One-time migration from the original index-based selector.
'@
$autoSelectedBlock = @'
    if (kind == 2) {
        return resolve_auto_backend(adapters);
    }

    // One-time migration from the original index-based selector.
'@
$cpp = Replace-Once $cpp $migrationMarker $autoSelectedBlock 'selected_backend Auto resolver'

$backendNameMarker = 'std::wstring backend_name(backend value) {'
$publicAutoFunctions = @'
void remember_auto_backend(backend fastest) {
    if (fastest == backend::cpu) {
        save_auto_cpu_winner();
        return;
    }

    if (!is_directml_backend(fastest)) return;
    const unsigned wanted = directml_adapter_index(fastest);
    for (const auto& a : enumerate_directml_adapters()) {
        if (a.index == wanted) {
            save_auto_gpu_winner(a);
            return;
        }
    }
}

void select_auto_backend() {
    // Kind 2 means resolve the separately stored benchmark winner each time.
    // If no winner exists yet, Auto safely resolves to CPU.
    g_backend_kind_cfg = 2;
}

bool selected_backend_preference_is_auto() {
    return static_cast<int>(g_backend_kind_cfg.get()) == 2;
}

backend auto_backend() {
    return resolve_auto_backend(enumerate_directml_adapters());
}

std::wstring auto_backend_name() {
    return backend_name(auto_backend());
}

'@
$cpp = Replace-Once $cpp $backendNameMarker ($publicAutoFunctions + $backendNameMarker) 'Auto public functions'

$oldGpuPreference = @'
bool selected_backend_preference_is_gpu() {
    return static_cast<int>(g_backend_kind_cfg.get()) == 1;
}
'@
$newGpuPreference = @'
bool selected_backend_preference_is_gpu() {
    const int kind = static_cast<int>(g_backend_kind_cfg.get());
    return kind == 1 || (kind == 2 && static_cast<int>(g_auto_kind_cfg.get()) == 1);
}
'@
$cpp = Replace-Once $cpp $oldGpuPreference $newGpuPreference 'Auto GPU fallback detection'
Set-Content $cppPath $cpp -Encoding UTF8

# -----------------------------------------------------------------------------
# Benchmark UI: remember fastest winner and expose Auto / Fastest button
# -----------------------------------------------------------------------------
$benchmarkPath = Join-Path $env:GITHUB_WORKSPACE 'stem_benchmark.cpp'
$benchmark = Get-Content $benchmarkPath -Raw
$start = $benchmark.IndexOf('void show_results_and_select(std::vector<benchmark_result>& results) {')
$end = $benchmark.IndexOf('void benchmark_thread(std::wstring source) {')
if ($start -lt 0 -or $end -le $start) { throw 'Benchmark selection function boundaries not found.' }

$newFunction = @'
void show_results_and_select(std::vector<benchmark_result>& results) {
    const benchmark_result* cpu = nullptr;
    const benchmark_result* fastest = nullptr;

    for (const auto& r : results) {
        if (r.value == onnxstem::backend::cpu && r.ok) cpu = &r;
        if (r.ok && (!fastest || r.repeat_ms < fastest->repeat_ms)) fastest = &r;
    }

    // Always refresh Auto's stored winner after a successful benchmark, even if
    // the user chooses a fixed backend below. Auto only becomes active when the
    // dedicated Auto / Fastest button is selected.
    if (fastest) onnxstem::remember_auto_backend(fastest->value);

    std::wostringstream text;
    text << L"Test: the same 4-second stereo 44.1 kHz Spleeter block\n\n";
    text << L"TIME: LOWER is better.\n";
    text << L"SPEED vs CPU: HIGHER is better.\n\n";

    for (const auto& r : results) {
        text << r.label << L"\n";
        if (!r.ok) {
            text << L"  Failed: " << r.error << L"\n\n";
            continue;
        }

        text << L"  First-use: " << format_seconds(r.first_use_ms) << L"\n";
        text << L"  Repeat:    " << format_seconds(r.repeat_ms) << L"\n";
        if (cpu && cpu->repeat_ms > 0.0 && r.repeat_ms > 0.0) {
            text << L"  Speed vs CPU: "
                 << format_speedup(cpu->repeat_ms / r.repeat_ms) << L"\n";
        }
        text << L"\n";
    }

    if (fastest) {
        text << L"Fastest repeat result: " << fastest->label << L"\n";
    }

    text << L"\nAuto / Fastest remembers the fastest successful result from this benchmark.\n"
            L"It does not re-run the benchmark when foobar2000 starts.\n\n"
            L"First-use includes model/session setup plus the first stem pass.\n"
            L"Repeat best represents ongoing stem and seek processing.\n\n"
            L"GPU selections are saved by hardware identity, not by adapter number.\n"
            L"If Auto's winning GPU is missing or DirectML cannot initialize, live stems fall back to CPU.\n\n"
            L"Choose Auto / Fastest or a fixed backend below.";

    std::vector<std::wstring> button_text;
    std::vector<onnxstem::backend> choices;
    button_text.reserve(results.size());
    choices.reserve(results.size());

    for (const auto& r : results) {
        if (!r.ok) continue;
        button_text.push_back(r.label);
        choices.push_back(r.value);
    }

    if (choices.empty()) {
        MessageBoxW(
            core_api::get_main_window(), text.str().c_str(),
            L"Stem Separator Benchmark", MB_OK | MB_ICONWARNING);
        return;
    }

    constexpr int kAutoButton = 900;
    std::wstring auto_button_text;
    std::vector<TASKDIALOG_BUTTON> buttons;
    buttons.reserve(choices.size() + (fastest ? 1u : 0u));

    int default_button = 1000;
    if (fastest) {
        auto_button_text = L"Auto / Fastest - " + fastest->label;
        TASKDIALOG_BUTTON auto_button{};
        auto_button.nButtonID = kAutoButton;
        auto_button.pszButtonText = auto_button_text.c_str();
        buttons.push_back(auto_button);
        default_button = kAutoButton;
    }

    for (size_t i = 0; i < choices.size(); ++i) {
        TASKDIALOG_BUTTON b{};
        b.nButtonID = 1000 + static_cast<int>(i);
        b.pszButtonText = button_text[i].c_str();
        buttons.push_back(b);
    }

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = core_api::get_main_window();
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = L"Stem Separator Benchmark";
    config.pszMainInstruction = L"Benchmark complete - lower processing time is better";
    const std::wstring content = text.str();
    config.pszContent = content.c_str();
    config.cButtons = static_cast<UINT>(buttons.size());
    config.pButtons = buttons.data();
    config.nDefaultButton = default_button;

    int pressed = 0;
    const HRESULT hr = TaskDialogIndirect(&config, &pressed, nullptr, nullptr);
    if (FAILED(hr)) {
        MessageBoxW(
            core_api::get_main_window(), content.c_str(),
            L"Stem Separator Benchmark", MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (pressed == kAutoButton && fastest) {
        onnxstem::select_auto_backend();

        std::wstring confirmation =
            L"Selected processing backend:\n\nAuto / Fastest\n\nCurrent benchmark winner:\n" +
            fastest->label +
            L"\n\nThe winner is stored by hardware identity. Auto will use this backend on future "
            L"separation calls without re-running the benchmark. CPU fallback remains automatic "
            L"if the winning GPU is missing or DirectML cannot initialize.";

        MessageBoxW(
            core_api::get_main_window(), confirmation.c_str(),
            L"Stem Separator", MB_OK | MB_ICONINFORMATION);
        console::print("Stem Separator backend selection updated to Auto / Fastest.");
        return;
    }

    const int choice_index = pressed - 1000;
    if (choice_index < 0 ||
        static_cast<size_t>(choice_index) >= choices.size()) return;

    const onnxstem::backend selected = choices[static_cast<size_t>(choice_index)];
    onnxstem::select_backend(selected);

    std::wstring confirmation =
        L"Selected fixed processing backend:\n\n" + onnxstem::backend_name(selected) +
        L"\n\nThe GPU preference is stored by hardware identity. "
        L"The live cache and exports will use it on their next separation call. "
        L"CPU fallback remains automatic if that GPU is unavailable.";

    MessageBoxW(
        core_api::get_main_window(), confirmation.c_str(),
        L"Stem Separator", MB_OK | MB_ICONINFORMATION);

    console::print("Stem Separator fixed backend selection updated.");
}

'@
$benchmark = $benchmark.Substring(0, $start) + $newFunction + $benchmark.Substring($end)
Set-Content $benchmarkPath $benchmark -Encoding UTF8

Write-Host 'Auto / Fastest patch applied.'
Select-String -Path $cppPath -Pattern 'g_auto_kind_cfg|select_auto_backend|selected_backend_preference_is_auto' -Context 1,1
Select-String -Path $benchmarkPath -Pattern 'Auto / Fastest' -Context 1,1
