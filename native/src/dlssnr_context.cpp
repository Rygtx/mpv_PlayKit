// Ported from Magpie experimental DLSSNRFilter.cpp / NgxD3D12Core.cpp (see header).
#include "dlssnr_context.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace vsdlssnr {

namespace {

constexpr NVSDK_NGX_Feature FEATURE_DLSSNR = static_cast<NVSDK_NGX_Feature>(18);
// Signed snippet application id + Magpie project id: the NGX driver-side
// configuration grants the Feature 18 model mapping per project id, so a
// self-invented id would silently fall back (black output).
constexpr unsigned long long DLSSNR_SIGNED_SNIPPET_APPLICATION_ID = 0x0876232Cull;
constexpr char PROJECT_ID[] = "7c134ab9-9677-4af5-a2b2-bca943350861";
constexpr char ENGINE_NAME[] = "vs_dlssnr-0.1";

constexpr char PARAM_WIDTH[] = "DLSSNR.Width";
constexpr char PARAM_HEIGHT[] = "DLSSNR.Height";
constexpr char PARAM_INPUT_WIDTH[] = "DLSSNR.InputWidth";
constexpr char PARAM_INPUT_HEIGHT[] = "DLSSNR.InputHeight";
constexpr char PARAM_OUTPUT_WIDTH[] = "DLSSNR.OutputWidth";
constexpr char PARAM_OUTPUT_HEIGHT[] = "DLSSNR.OutputHeight";
constexpr char PARAM_OUTPUT_DOT_WIDTH[] = "DLSSNR.Output.Width";
constexpr char PARAM_OUTPUT_DOT_HEIGHT[] = "DLSSNR.Output.Height";
constexpr char PARAM_UPSCALING[] = "DLSSNR.Upscaling";
constexpr char PARAM_SCALE[] = "DLSSNR.Scale";
constexpr char PARAM_SCALING_RATIO[] = "DLSSNR.ScalingRatio";
constexpr char PARAM_SCALING_RATIO_CALLBACK[] = "DLSSNRComputeScalingRatioCallback";
constexpr char PARAM_PRESET[] = "DLSSNR.Hint.Render.Preset";
constexpr char PARAM_INDICATOR_INVERT_X[] = "DLSS.Indicator.Invert.X.Axis";
constexpr char PARAM_INDICATOR_INVERT_Y[] = "DLSS.Indicator.Invert.Y.Axis";
constexpr char PARAM_COLOR[] = "DLSSNR.Color";
constexpr char PARAM_OUTPUT[] = "DLSSNR.Output";
constexpr char PARAM_MVEC[] = "DLSSNR.MVec";
constexpr char PARAM_DEPTH[] = "DLSSNR.Depth";
constexpr char PARAM_MVEC_SCALE_X[] = "DLSSNR.MVecScaleX";
constexpr char PARAM_MVEC_SCALE_Y[] = "DLSSNR.MVecScaleY";
constexpr char PARAM_DEPTH_INVERTED[] = "DLSSNR.DepthInverted";
constexpr char PARAM_ENABLED[] = "DLSSNR.Enabled";
constexpr char PARAM_RESET[] = "DLSSNR.Reset";
constexpr char PARAM_STYLE[] = "DLSSNR.Style";
constexpr char PARAM_INTENSITY[] = "DLSSNR.Intensity";
constexpr char PARAM_LOCAL_TONE[] = "DLSSNR.LocalToneStrength";
constexpr char PARAM_LOCAL_STRUCTURE[] = "DLSSNR.LocalStructureStrength";
constexpr char PARAM_SKIN_STRUCTURE[] = "DLSSNR.SkinStructureStrength";
constexpr char PARAM_AUTO_MASK[] = "DLSSNR.UseAutoMask";
constexpr char PARAM_UI_CORRECTION[] = "DLSSNR.UICorrection";

struct ResourceParameters {
    const char *baseX;
    const char *baseY;
    const char *width;
    const char *height;
};

// Color / Output / MVec / Depth subrect key groups (Magpie DLSSNRFilter.cpp:76-85)
constexpr ResourceParameters RESOURCE_PARAMETERS[]{
    { "DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY",
        "DLSSNR.ColorSubrectWidth", "DLSSNR.ColorSubrectHeight" },
    { "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY",
        "DLSSNR.OutputSubrectWidth", "DLSSNR.OutputSubrectHeight" },
    { "DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY",
        "DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight" },
    { "DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY",
        "DLSSNR.DepthSubrectWidth", "DLSSNR.DepthSubrectHeight" }
};

LONG CaptureNgxException(DWORD code, DWORD *sehCode) noexcept {
    *sehCode = code;
    return EXCEPTION_EXECUTE_HANDLER;
}

void DbgLine(const char *msg) noexcept {
    OutputDebugStringA("vs_dlssnr: ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

template <typename T>
void *FunctionAddress(T function) noexcept {
    void *result = nullptr;
    static_assert(sizeof(function) == sizeof(result));
    std::memcpy(&result, &function, sizeof(result));
    return result;
}

void SetSubrect(NVSDK_NGX_Parameter *parameters, const ResourceParameters &resource, int width, int height) noexcept {
    parameters->Set(resource.baseX, 0);
    parameters->Set(resource.baseY, 0);
    parameters->Set(resource.width, width);
    parameters->Set(resource.height, height);
}

// Magpie DLSSNRFilter.cpp:1092-1102; the snippet calls back into this during
// evaluate to re-assert the scaling ratio.
NVSDK_NGX_Result NVSDK_CONV SetScalingRatioCallback(NVSDK_NGX_Parameter *parameters) noexcept {
    __try {
        if (!parameters) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        parameters->Set(PARAM_SCALING_RATIO, 1.0f);
        return NVSDK_NGX_Result_Success;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

// NGX internal log sink (VSDLSSNR_NGX_LOG=1), mirrors Magpie's LoggingInfo hook.
void NVSDK_CONV NgxLogCallback(const char *message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) noexcept {
    if (!message) return;
    OutputDebugStringA("vs_dlssnr NGX: ");
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
}

D3D12_RESOURCE_BARRIER TransitionTo(
    ID3D12Resource *resource,
    D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

D3D12_RESOURCE_BARRIER TransitionFromTo(
    ID3D12Resource *resource,
    D3D12_RESOURCE_STATES from,
    D3D12_RESOURCE_STATES to) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

} // namespace (Transition helpers)

// diagnostics: append timing lines next to the host exe.
// Controlled by the panel ("性能日志" toggle; wired up by the panel-IPC step).
std::atomic<bool> g_timingLogEnabled{ true };

void SetTimingLogEnabled(bool enabled) noexcept {
    g_timingLogEnabled.store(enabled, std::memory_order_relaxed);
}

namespace {

void TimingLog(const char *line) noexcept {
    if (!g_timingLogEnabled.load(std::memory_order_relaxed)) return;
    wchar_t dir[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, dir, MAX_PATH)) return;
    std::filesystem::path base = std::filesystem::path(dir).parent_path();
    FILE *f = nullptr;
    if (_wfopen_s(&f, (base / L"dlssnr_timing.log").c_str(), L"a") != 0 || !f) return;
    fwrite(line, 1, strlen(line), f);
    if (!strlen(line) || line[strlen(line) - 1] != '\n') fputc('\n', f);
    fclose(f);
}

} // namespace (TimingLog helpers)

// Internal NGX processing size for a resolution percent (25-100; aligned to even)
// Internal NGX processing size (aligned to even). scalingEnabled == 0 forces
// the full source size (internal-resolution scaling off).
static void InternalSize(int w, int h, int pct, int &iw, int &ih) noexcept {
    const int p = std::clamp(pct, 25, 100);
    if (p >= 100) {
        iw = w;
        ih = h;
        return;
    }
    iw = (std::max)(1, (w * p / 100) & ~1);
    ih = (std::max)(1, (h * p / 100) & ~1);
}

// Magpie-style rolling timing window: last / EMA / p99 over 120 samples
struct TimingWindow {
    double gpu[120]{};
    double pack[120]{};
    double evalCpu[120]{};
    double unpack[120]{};
    int count = 0;
    int idx = 0;

    void Push(double g, double p, double e, double u) noexcept {
        gpu[idx] = g; pack[idx] = p; evalCpu[idx] = e; unpack[idx] = u;
        idx = (idx + 1) % 120;
        if (count < 120) ++count;
    }
    static double Ema(const double *arr, int count) noexcept {
        if (!count) return 0.0;
        double e = arr[0];
        for (int i = 1; i < count; ++i) e = e * 0.9 + arr[i] * 0.1;
        return e;
    }
    static double P99(const double *arr, int count) noexcept {
        if (!count) return 0.0;
        double sorted[120];
        for (int i = 0; i < count; ++i) sorted[i] = arr[i];
        std::sort(sorted, sorted + count);
        const int i99 = (std::min)(count - 1, static_cast<int>(count * 0.99));
        return sorted[i99];
    }
};

// rolling window lives at file scope (single filter instance)
TimingWindow g_timing;

DlssnrContext::~DlssnrContext() { Shutdown(); }

// ---- SEH-wrapped core calls (NgxD3D12Core.cpp:22-89) ----

NVSDK_NGX_Result DlssnrContext::CoreInitSafely(
    const wchar_t *appDir, ID3D12Device *device,
    const NVSDK_NGX_FeatureCommonInfo *info, DWORD *sehCode) noexcept {
    *sehCode = 0;
    __try {
        return NVSDK_NGX_D3D12_Init_with_ProjectID(
            PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, ENGINE_NAME,
            appDir, device, info, NVSDK_NGX_Version_API);
    } __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

NVSDK_NGX_Result DlssnrContext::CoreAllocateParametersSafely(NVSDK_NGX_Parameter **out, DWORD *sehCode) noexcept {
    *sehCode = 0;
    __try {
        return NVSDK_NGX_D3D12_AllocateParameters(out);
    } __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

NVSDK_NGX_Result DlssnrContext::CoreDestroyParametersSafely(NVSDK_NGX_Parameter *p, DWORD *sehCode) noexcept {
    *sehCode = 0;
    __try {
        return NVSDK_NGX_D3D12_DestroyParameters(p);
    } __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

NVSDK_NGX_Result DlssnrContext::CoreShutdownSafely(ID3D12Device *device, DWORD *sehCode) noexcept {
    *sehCode = 0;
    __try {
        return NVSDK_NGX_D3D12_Shutdown1(device);
    } __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

NVSDK_NGX_Result DlssnrContext::SnippetInitSafely(const wchar_t *appDataPath, ID3D12Device *device, DWORD *sehCode) noexcept {
    *sehCode = 0;
    __try {
        return _snippetInitExt(
            DLSSNR_SIGNED_SNIPPET_APPLICATION_ID, appDataPath,
            device, NVSDK_NGX_Version_API, nullptr);
    } __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

NVSDK_NGX_Result DlssnrContext::SnippetCreateFeatureSafely(ID3D12GraphicsCommandList *cl, NVSDK_NGX_Parameter *params, DWORD *sehCode) noexcept {
    *sehCode = 0;
    __try {
        return _snippetCreateFeature(cl, FEATURE_DLSSNR, params, &_feature);
    } __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

NVSDK_NGX_Result DlssnrContext::SnippetEvaluateSafely(ID3D12GraphicsCommandList *cl, NVSDK_NGX_Parameter *params, DWORD *sehCode) noexcept {
    *sehCode = 0;
    __try {
        return _snippetEvaluateFeature(cl, _feature, params, nullptr);
    } __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

NVSDK_NGX_Result DlssnrContext::SnippetReleaseSafely(DWORD *sehCode) noexcept {
    *sehCode = 0;
    __try {
        return _snippetReleaseFeature(_feature);
    } __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

NVSDK_NGX_Result DlssnrContext::SnippetShutdownSafely(DWORD *sehCode) noexcept {
    *sehCode = 0;
    __try {
        return _snippetShutdown(_d3d12->Device());
    } __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

// ---- Parameter assembly (Magpie DLSSNRFilter.cpp:1104-1130 / 1197-1230) ----

void DlssnrContext::SetCreateParametersUnsafe() noexcept {
    NVSDK_NGX_Parameter *p = _parameters;
    const DlssnrParams createParams = _shared->Snapshot();
    // Internal processing size: source * inputResolutionPercent (25-100) when
    // scaling is enabled; at 100% or with scaling disabled all NGX size keys
    // refer to the full source size.
    int iw = _width, ih = _height;
    if (createParams.scalingEnabled) {
        InternalSize(_width, _height, createParams.inputResolutionPercent, iw, ih);
    }
    p->Set(PARAM_WIDTH, iw);
    p->Set(PARAM_HEIGHT, ih);
    p->Set(PARAM_INPUT_WIDTH, iw);
    p->Set(PARAM_INPUT_HEIGHT, ih);
    p->Set(PARAM_OUTPUT_WIDTH, iw);
    p->Set(PARAM_OUTPUT_HEIGHT, ih);
    p->Set(PARAM_OUTPUT_DOT_WIDTH, iw);
    p->Set(PARAM_OUTPUT_DOT_HEIGHT, ih);
    p->Set(PARAM_UPSCALING, 0u);
    p->Set(PARAM_SCALE, 1.0f);
    p->Set(PARAM_SCALING_RATIO, 1.0f);
    p->Set(PARAM_SCALING_RATIO_CALLBACK, FunctionAddress(&SetScalingRatioCallback));
    p->Set(PARAM_PRESET, createParams.preset);
    p->Set(NVSDK_NGX_Parameter_Width, iw);
    p->Set(NVSDK_NGX_Parameter_Height, ih);
    p->Set(NVSDK_NGX_Parameter_PerfQualityValue,
           static_cast<int>(NVSDK_NGX_PerfQuality_Value_Balanced));
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set(PARAM_INDICATOR_INVERT_X, 0);
    p->Set(PARAM_INDICATOR_INVERT_Y, 0);
}

bool DlssnrContext::SetCreateParametersSafely(DWORD *sehCode) noexcept {
    *sehCode = 0;
    __try {
        SetCreateParametersUnsafe();
        return true;
    } __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
        return false;
    }
}

void DlssnrContext::SetEvaluateParametersUnsafe(bool resetHistory) noexcept {
    NVSDK_NGX_Parameter *p = _parameters;
    const DlssnrParams params = _shared->Snapshot();
    // With internal-resolution scaling, NGX consumes/produces the reduced
    // textures; the residual composite then reconstructs the full-size output.
    const bool scaling = _d3d12->HasScaling();
    const int ew = scaling ? _d3d12->InternalWidth() : _width;
    const int eh = scaling ? _d3d12->InternalHeight() : _height;
    p->Set(PARAM_COLOR, scaling ? _d3d12->ReducedColor() : _d3d12->InputColor());
    p->Set(PARAM_OUTPUT, scaling ? _d3d12->ReducedDenoised() : _d3d12->OutputColor());
    p->Set(PARAM_MVEC, _d3d12->Motion());
    p->Set(PARAM_DEPTH, _d3d12->Depth());
    SetSubrect(p, RESOURCE_PARAMETERS[0], ew, eh);
    SetSubrect(p, RESOURCE_PARAMETERS[1], ew, eh);
    SetSubrect(p, RESOURCE_PARAMETERS[2], ew, eh);
    SetSubrect(p, RESOURCE_PARAMETERS[3], ew, eh);
    p->Set(PARAM_MVEC_SCALE_X, 1.0f);
    p->Set(PARAM_MVEC_SCALE_Y, 1.0f);
    p->Set(PARAM_DEPTH_INVERTED, 1);
    p->Set(PARAM_ENABLED, 1);
    p->Set(PARAM_RESET, resetHistory ? 1 : 0);
    p->Set(PARAM_STYLE, params.style);
    p->Set(PARAM_INTENSITY, params.intensity);
    p->Set(PARAM_LOCAL_TONE, params.localToneStrength);
    p->Set(PARAM_LOCAL_STRUCTURE, params.localStructureStrength);
    p->Set(PARAM_SKIN_STRUCTURE, params.skinStructureStrength);
    p->Set(PARAM_AUTO_MASK, params.useAutoMask ? 1 : 0);
    p->Set(PARAM_UI_CORRECTION, params.uiCorrection ? 1 : 0);
}

bool DlssnrContext::SetEvaluateParametersSafely(bool resetHistory, DWORD *sehCode) noexcept {
    *sehCode = 0;
    __try {
        SetEvaluateParametersUnsafe(resetHistory);
        return true;
    } __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
        return false;
    }
}

// ---- Lifecycle ----

bool DlssnrContext::Initialize(
    D3D12Context &d3d12, const wchar_t *ngxDllPath,
    int width, int height, SharedParams *shared,
    char *err, size_t errLen) noexcept {
    auto fail = [&](const char *what) {
        if (err && errLen) std::snprintf(err, errLen, "%s", what);
        DbgLine(what);
        return false;
    };
    (void)fail;

    _d3d12 = &d3d12;
    _width = width;
    _height = height;
    _shared = shared;

    // Application data path = snippet directory (mirrors Magpie using its exe dir;
    // the directory must be writable for NGX caches).
    {
        std::filesystem::path dll(ngxDllPath);
        std::error_code ec;
        std::filesystem::path dir = dll.parent_path();
        std::filesystem::create_directories(dir, ec);
        const std::wstring dirStr = dir.wstring();
        if (dirStr.size() >= MAX_PATH) return false;
        std::memcpy(_appDataPath, dirStr.c_str(), (dirStr.size() + 1) * sizeof(wchar_t));
    }

    // 1) NGX static core (NgxD3D12Core.cpp:118-151)
    {
        const wchar_t *featurePaths[]{ _appDataPath };
        NVSDK_NGX_FeatureCommonInfo featureInfo{};
        featureInfo.PathListInfo.Path = featurePaths;
        featureInfo.PathListInfo.Length = 1;
        if (GetEnvironmentVariableA("VSDLSSNR_NGX_LOG", nullptr, 0) != 0) {
            featureInfo.LoggingInfo.LoggingCallback = &NgxLogCallback;
            featureInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_Logging_Level::NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
            featureInfo.LoggingInfo.DisableOtherLoggingSinks = false;
        }
        DWORD sehCode = 0;
        const NVSDK_NGX_Result r = CoreInitSafely(_appDataPath, _d3d12->Device(), &featureInfo, &sehCode);
        if (sehCode) return false;
        if (!NVSDK_NGX_SUCCEED(r)) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "NGX core Init_with_ProjectID failed (0x%x)", static_cast<unsigned>(r));
            return fail(msg);
        }
        _coreInitialized = true;
    }

    // 2) Allocate the parameter block from the core; the snippet receives the
    // same block for CreateFeature/EvaluateFeature (Magpie DLSSNRFilter.cpp:1717).
    {
        DWORD sehCode = 0;
        const NVSDK_NGX_Result r = CoreAllocateParametersSafely(&_parameters, &sehCode);
        if (sehCode || !NVSDK_NGX_SUCCEED(r) || !_parameters) return false;
    }

    // 3) Signed snippet load (Magpie InitializeSignedSnippet, cpp:1146-1195).
    //    The caller-compatibility IAT hook must be installed before Init runs.
    _snippetModule = LoadLibraryExW(
        ngxDllPath, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!_snippetModule) return fail("LoadLibraryExW(nvngx_dlssnr.dll) failed");

    _snippetInitExt = reinterpret_cast<SnippetInitExtFn>(
        GetProcAddress(_snippetModule, "NVSDK_NGX_D3D12_Init_Ext"));
    _snippetCreateFeature = reinterpret_cast<CreateFeatureFn>(
        GetProcAddress(_snippetModule, "NVSDK_NGX_D3D12_CreateFeature"));
    _snippetEvaluateFeature = reinterpret_cast<EvaluateFeatureFn>(
        GetProcAddress(_snippetModule, "NVSDK_NGX_D3D12_EvaluateFeature"));
    _snippetReleaseFeature = reinterpret_cast<ReleaseFeatureFn>(
        GetProcAddress(_snippetModule, "NVSDK_NGX_D3D12_ReleaseFeature"));
    _snippetShutdown = reinterpret_cast<ShutdownFn>(
        GetProcAddress(_snippetModule, "NVSDK_NGX_D3D12_Shutdown1"));
    if (!_snippetInitExt || !_snippetCreateFeature || !_snippetEvaluateFeature ||
        !_snippetReleaseFeature || !_snippetShutdown) {
        return fail("DLSSNR signed snippet exports are incomplete");
    }

    if (!InstallSnippetCallerHook(_snippetModule, _hook)) {
        return fail("IAT hook install failed (no GetModuleFileNameW import slot)");
    }

    {
        DWORD sehCode = 0;
        const NVSDK_NGX_Result r = SnippetInitSafely(_appDataPath, _d3d12->Device(), &sehCode);
        if (sehCode) return fail("Snippet Init_Ext raised SEH");
        if (!NVSDK_NGX_SUCCEED(r)) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "Snippet Init_Ext failed (0x%x)", static_cast<unsigned>(r));
            return fail(msg);
        }
        _snippetInitialized = true;
    }

    // 4) Frame resources incl. zero-guidance textures + residual scaling
    //    textures/compute at the internal resolution (skipped entirely when
    //    internal-resolution scaling is disabled)
    if (!_d3d12->CreateFrameResources(_width, _height, err, errLen)) return false;
    if (_shared->Snapshot().scalingEnabled) {
        const int pct = std::clamp(_shared->Snapshot().inputResolutionPercent, 25, 100);
        int iw = _width, ih = _height;
        InternalSize(_width, _height, pct, iw, ih);
        if (!_d3d12->CreateScalingResources(iw, ih, err, errLen)) return false;
    }

    // 5) CreateFeature on an open command list, then close+execute (Magpie cpp:1720-1758)
    if (!_d3d12->BeginRecording()) return fail("BeginRecording(create) failed");
    {
        DWORD sehCode = 0;
        if (!SetCreateParametersSafely(&sehCode)) return fail("Create parameter setup raised SEH");
    }
    {
        DWORD sehCode = 0;
        const NVSDK_NGX_Result r = SnippetCreateFeatureSafely(_d3d12->CommandList(), _parameters, &sehCode);
        if (sehCode) return fail("Snippet CreateFeature raised SEH");
        if (!NVSDK_NGX_SUCCEED(r) || !_feature) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "Feature 18 creation failed (0x%x)", static_cast<unsigned>(r));
            return fail(msg);
        }
    }
    if (!_d3d12->ExecuteAndWait()) return fail("Execute(create) failed");

    _ready = true;
    {
        char msg[160];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: Feature=18 created=true path=signed-snippet %dx%d disabled=false", _width, _height);
        DbgLine(msg);
    }
    return true;
}

bool DlssnrContext::RecreateFeature(int preset, int resPercent, int scalingEnabled, char *err, size_t errLen) noexcept {
    if (!_ready || !_snippetReleaseFeature) {
        if (err && errLen) std::snprintf(err, errLen, "RecreateFeature: context not ready");
        return false;
    }
    // Preset / internal-resolution / scaling-toggle are create-time keys:
    // release the feature, rebuild scaling textures when needed, then
    // CreateFeature again (device/queues untouched). Scaling disabled means
    // the residual pipeline is dropped from the frame flow entirely.
    {
        DWORD sehCode = 0;
        SnippetReleaseSafely(&sehCode);
        _feature = nullptr;
        sehCode = 0;
        CoreDestroyParametersSafely(_parameters, &sehCode);
        _parameters = nullptr;
        sehCode = 0;
        const NVSDK_NGX_Result r = CoreAllocateParametersSafely(&_parameters, &sehCode);
        if (sehCode || !NVSDK_NGX_SUCCEED(r) || !_parameters) {
            if (err && errLen) std::snprintf(err, errLen, "RecreateFeature: AllocateParameters failed");
            _ready = false;
            return false;
        }
        if (scalingEnabled) {
            int iw = _width, ih = _height;
            InternalSize(_width, _height, resPercent, iw, ih);
            if (!_d3d12->CreateScalingResources(iw, ih, err, errLen)) {
                _ready = false;
                return false;
            }
        } else {
            _d3d12->ClearScalingResources();
        }
    }
    if (!_d3d12->BeginRecording()) {
        if (err && errLen) std::snprintf(err, errLen, "RecreateFeature: BeginRecording failed");
        return false;
    }
    {
        DWORD sehCode = 0;
        if (!SetCreateParametersSafely(&sehCode)) {
            if (err && errLen) std::snprintf(err, errLen, "RecreateFeature: parameter setup raised SEH");
            return false;
        }
        const NVSDK_NGX_Result r = SnippetCreateFeatureSafely(_d3d12->CommandList(), _parameters, &sehCode);
        if (sehCode || !NVSDK_NGX_SUCCEED(r) || !_feature) {
            if (err && errLen) std::snprintf(err, errLen, "RecreateFeature: CreateFeature failed (0x%x)",
                static_cast<unsigned>(sehCode ? 0xFFFFFFFFu : r));
            return false;
        }
    }
    if (!_d3d12->ExecuteAndWait()) {
        if (err && errLen) std::snprintf(err, errLen, "RecreateFeature: Execute failed");
        return false;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "DLSSNR STATUS: preset=%d res=%d%% scaling=%d feature recreated",
             preset, scalingEnabled ? resPercent : 100, scalingEnabled);
    DbgLine(msg);
    TimingLog(msg);
    return true;
}

bool DlssnrContext::ProcessFrame(
    const uint8_t *const *srcPlanes, const int64_t *srcStrides,
    uint8_t **dstPlanes, int64_t *dstStrides,
    int width, int height, bool resetHistory,
    char *err, size_t errLen,
    char *timingOut, size_t timingLen) noexcept {
    if (!_ready) {
        if (err && errLen) std::snprintf(err, errLen, "context not ready");
        return false;
    }
    // Panel preset / internal-resolution / scaling-toggle changes require a
    // feature rebuild; consume before packing.
    if (int newPreset = -1, newRes = -1, newScaling = -1; _shared->ConsumeRebuild(newPreset, newRes, newScaling)) {
        if (!RecreateFeature(newPreset, newRes, newScaling, err, errLen)) return false;
    }
    const DlssnrParams frameParams = _shared->Snapshot();
    const float residualMultiplier = std::clamp(frameParams.residualMultiplier, 1.0f, 2.0f);
    // Telemetry is always on (QPC reads cost ~ns); timingOut additionally
    // receives a per-frame segment string for the VS-log channel.
    const bool vsTiming = timingOut && timingLen > 0;
    LARGE_INTEGER qpcFreq{}, t0{}, t1{}, t2{}, t3{}, t4{};
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&t0);
    // Diagnostic: VSDLSSNR_SKIP_EVAL=1 measures the pipe without NGX evaluate
    static const bool skipEval = GetEnvironmentVariableA("VSDLSSNR_SKIP_EVAL", nullptr, 0) != 0;
    if (!_d3d12->PackInput(srcPlanes, srcStrides, width, height, err, errLen)) return false;
    QueryPerformanceCounter(&t1);

    if (!_d3d12->BeginRecording()) {
        if (err && errLen) std::snprintf(err, errLen, "BeginRecording(frame) failed");
        return false;
    }
    // NOTE: D3D12 timestamp queries crash NGX snippet evaluate (SEH) when on the same command list - do not re-enable (verified 2026-09-05)

    if (skipEval) {
        // Diagnostic path: route input straight to output for pipe-cost measurement
        auto *cl = _d3d12->CommandList();
        D3D12_RESOURCE_BARRIER bar[2]{
            TransitionTo(_d3d12->InputColor(), D3D12_RESOURCE_STATE_COPY_SOURCE),
            TransitionTo(_d3d12->OutputColor(), D3D12_RESOURCE_STATE_COPY_DEST),
        };
        cl->ResourceBarrier(2, bar);
        cl->CopyResource(_d3d12->OutputColor(), _d3d12->InputColor());
        for (auto &b : bar) {
            D3D12_RESOURCE_STATES tmp = b.Transition.StateBefore;
            b.Transition.StateBefore = b.Transition.StateAfter;
            b.Transition.StateAfter = tmp;
        }
        cl->ResourceBarrier(2, bar);
    } else {
        if (!_d3d12->RecordUploadCopy(err, errLen)) return false;

        auto *cl = _d3d12->CommandList();
        const bool scaling = _d3d12->HasScaling();

        // Pre-evaluate barriers (executed on the GPU before the NGX dispatch).
        D3D12_RESOURCE_BARRIER pre[4];
        UINT preCount = 0;
        if (scaling) {
            // downsample source -> reducedColor (area average, Magpie HLSL)
            D3D12_RESOURCE_BARRIER b1[2]{
                TransitionTo(_d3d12->InputColor(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                TransitionTo(_d3d12->ReducedColor(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            cl->ResourceBarrier(2, b1);
            _d3d12->RecordDownsample(residualMultiplier);
            // reducedColor: UAV (write) -> NSR (NGX input)
            D3D12_RESOURCE_BARRIER b2[1]{
                TransitionFromTo(_d3d12->ReducedColor(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            };
            cl->ResourceBarrier(1, b2);
            // NGX evaluate: color=ReducedColor (NSR), output=ReducedDenoised (UAV)
            pre[0] = TransitionTo(_d3d12->Motion(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            pre[1] = TransitionTo(_d3d12->Depth(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            pre[2] = TransitionTo(_d3d12->ReducedDenoised(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            preCount = 3;
        } else {
            // NGX evaluate at full source size: color=InputColor, output=OutputColor
            // (barrier set mirrors the pre-residual pipeline / Magpie Draw())
            pre[0] = TransitionTo(_d3d12->InputColor(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            pre[1] = TransitionTo(_d3d12->Motion(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            pre[2] = TransitionTo(_d3d12->Depth(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            pre[3] = TransitionTo(_d3d12->OutputColor(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            preCount = 4;
        }
        cl->ResourceBarrier(preCount, pre);

        DWORD sehCode = 0;
        if (!SetEvaluateParametersSafely(resetHistory, &sehCode)) {
            if (err && errLen) std::snprintf(err, errLen, "Evaluate parameter setup raised SEH");
            return false;
        }
        const NVSDK_NGX_Result r = SnippetEvaluateSafely(cl, _parameters, &sehCode);
        // split timing: NGX's own CPU cost inside EvaluateFeature vs GPU wait
        // NOTE: timestamp disabled, see note above
        QueryPerformanceCounter(&t2);
        if (sehCode) {
            char buf[96];
            snprintf(buf, sizeof(buf), "EvaluateFeature raised SEH 0x%x (scaling=%d)", sehCode, scaling ? 1 : 0);
            DbgLine(buf);
            if (err && errLen) snprintf(err, errLen, "EvaluateFeature raised SEH 0x%x", sehCode);
            return false;
        }
        if (!NVSDK_NGX_SUCCEED(r)) {
            if (err && errLen) std::snprintf(err, errLen, "EvaluateFeature failed (0x%x)", static_cast<unsigned>(r));
            return false;
        }

        if (scaling) {
            // reducedDenoised: UAV (NGX write) -> NSR (residual read)
            D3D12_RESOURCE_BARRIER b4[1]{
                TransitionFromTo(_d3d12->ReducedDenoised(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            };
            cl->ResourceBarrier(1, b4);
            // Lanczos3 horizontal residual upsample (reducedColor still NSR)
            D3D12_RESOURCE_BARRIER b5[1]{
                TransitionTo(_d3d12->HorizontalRes(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            cl->ResourceBarrier(1, b5);
            _d3d12->RecordResidualHorizontal(residualMultiplier);
            D3D12_RESOURCE_BARRIER b6[3]{
                TransitionFromTo(_d3d12->HorizontalRes(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                TransitionFromTo(_d3d12->ReducedColor(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                TransitionFromTo(_d3d12->ReducedDenoised(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            };
            cl->ResourceBarrier(3, b6);
            // vertical composite onto full-size output (input = original color)
            D3D12_RESOURCE_BARRIER b7[1]{
                TransitionTo(_d3d12->OutputColor(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            cl->ResourceBarrier(1, b7);
            // move input back to COMMON only after vertical dispatch consumed it
            _d3d12->RecordResidualVertical(residualMultiplier);
            D3D12_RESOURCE_BARRIER b8[5]{
                TransitionFromTo(_d3d12->OutputColor(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
                TransitionFromTo(_d3d12->InputColor(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                TransitionFromTo(_d3d12->HorizontalRes(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                TransitionFromTo(_d3d12->Motion(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                TransitionFromTo(_d3d12->Depth(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            };
            cl->ResourceBarrier(5, b8);
        } else {
            // no scaling: NGX wrote OutputColor directly; undo the pre-evaluate set
            for (UINT i = 0; i < preCount; ++i) {
                D3D12_RESOURCE_BARRIER back = pre[i];
                D3D12_RESOURCE_STATES tmp = back.Transition.StateBefore;
                back.Transition.StateBefore = back.Transition.StateAfter;
                back.Transition.StateAfter = tmp;
                cl->ResourceBarrier(1, &back);
            }
        }
    }

    if (!_d3d12->RecordReadbackCopy(err, errLen)) return false;
    // NOTE: timestamp disabled, see note above
    if (!_d3d12->ExecuteAndWait()) {
        if (err && errLen) std::snprintf(err, errLen, "Execute(frame) failed");
        return false;
    }
    QueryPerformanceCounter(&t3);
    const bool rb = _d3d12->UnpackOutput(dstPlanes, dstStrides, width, height, err, errLen);
    {
        static bool dumped = false;
        if (!dumped && GetEnvironmentVariableA("VSDLSSNR_DUMP", nullptr, 0) != 0) {
            dumped = true;
            wchar_t dir[MAX_PATH];
            if (GetModuleFileNameW(nullptr, dir, MAX_PATH)) {
                std::filesystem::path base = std::filesystem::path(dir).parent_path();
                const bool scaling = _d3d12->HasScaling();
                _d3d12->DumpTextureToFile(_d3d12->InputColor(), width, height,
                                          (base / L"dump_input.bin").c_str());
                if (scaling) {
                    _d3d12->DumpTextureToFile(_d3d12->ReducedColor(), _d3d12->InternalWidth(),
                                              _d3d12->InternalHeight(), (base / L"dump_reduced_color.bin").c_str());
                    _d3d12->DumpTextureToFile(_d3d12->ReducedDenoised(), _d3d12->InternalWidth(),
                                              _d3d12->InternalHeight(), (base / L"dump_reduced_denoised.bin").c_str());
                    _d3d12->DumpTextureToFile(_d3d12->HorizontalRes(), width,
                                              _d3d12->InternalHeight(), (base / L"dump_horizontal.bin").c_str());
                }
                _d3d12->DumpTextureToFile(_d3d12->OutputColor(), width, height,
                                          (base / L"dump_output.bin").c_str());
            }
        }
    }
    {
        QueryPerformanceCounter(&t4);
        const auto ms = [](LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER f) {
            return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
        };
        const double packMs = ms(t0, t1, qpcFreq);
        const double evalCpuMs = ms(t1, t2, qpcFreq);
        // Pure-GPU timing is unavailable (timestamp query + NGX evaluate = SEH,
        // see NOTE above); the submit+wait wall clock stands in for it.
        const double gpuWaitMs = ms(t2, t3, qpcFreq);
        const double unpackMs = ms(t3, t4, qpcFreq);
        g_timing.Push(gpuWaitMs, packMs, evalCpuMs, unpackMs);

        // Magpie-style periodic stats (every 60 frames) into dlssnr_timing.log
        static int statFrames = 0;
        if (++statFrames % 60 == 1) {
            const int lastIdx = g_timing.idx - 1 < 0 ? g_timing.count - 1 : g_timing.idx - 1;
            char line[256];
            snprintf(line, sizeof(line),
                     "DLSSNR perf: gpu=%.1f ema=%.1f p99=%.1f | pack=%.1f eval_cpu=%.1f unpack=%.1f | res=%d%% %dx%d",
                     g_timing.gpu[lastIdx],
                     TimingWindow::Ema(g_timing.gpu, g_timing.count),
                     TimingWindow::P99(g_timing.gpu, g_timing.count),
                     TimingWindow::Ema(g_timing.pack, g_timing.count),
                     TimingWindow::Ema(g_timing.evalCpu, g_timing.count),
                     TimingWindow::Ema(g_timing.unpack, g_timing.count),
                     std::clamp(_shared->Snapshot().inputResolutionPercent, 25, 100),
                     _width, _height);
            TimingLog(line);
        }

        if (vsTiming) {
            std::snprintf(timingOut, timingLen, "pack=%.1f,eval_cpu=%.1f,gpu=%.1f,unpack=%.1f",
                          packMs, evalCpuMs, gpuWaitMs, unpackMs);
        }
    }
    return rb;
}

void DlssnrContext::Shutdown() noexcept {
    if (_feature && _snippetReleaseFeature) {
        DWORD sehCode = 0;
        SnippetReleaseSafely(&sehCode);
        _feature = nullptr;
    }
    if (_snippetInitialized && _snippetShutdown && _d3d12) {
        DWORD sehCode = 0;
        SnippetShutdownSafely(&sehCode);
        _snippetInitialized = false;
    }
    if (_coreInitialized && _d3d12) {
        DWORD sehCode = 0;
        CoreShutdownSafely(_d3d12->Device(), &sehCode);
        _coreInitialized = false;
    }
    if (_parameters) {
        DWORD sehCode = 0;
        CoreDestroyParametersSafely(_parameters, &sehCode);
        _parameters = nullptr;
    }
    RestoreSnippetCallerHook(_hook);
    if (_snippetModule) {
        // Magpie intentionally skips FreeLibrary when the hook cannot be
        // restored (DLSSNRFilter.cpp:859-872); same here.
        if (!_hook.installed) FreeLibrary(_snippetModule);
        _snippetModule = nullptr;
    }
    _ready = false;
    _d3d12 = nullptr;
}

} // namespace vsdlssnr
