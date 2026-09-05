// Ported from Magpie experimental DLSSNRFilter.cpp / NgxD3D12Core.cpp (see header).
#include "dlssnr_context.h"

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

} // namespace

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
    p->Set(PARAM_WIDTH, _width);
    p->Set(PARAM_HEIGHT, _height);
    p->Set(PARAM_INPUT_WIDTH, _width);
    p->Set(PARAM_INPUT_HEIGHT, _height);
    p->Set(PARAM_OUTPUT_WIDTH, _width);
    p->Set(PARAM_OUTPUT_HEIGHT, _height);
    p->Set(PARAM_OUTPUT_DOT_WIDTH, _width);
    p->Set(PARAM_OUTPUT_DOT_HEIGHT, _height);
    p->Set(PARAM_UPSCALING, 0u);
    p->Set(PARAM_SCALE, 1.0f);
    p->Set(PARAM_SCALING_RATIO, 1.0f);
    p->Set(PARAM_SCALING_RATIO_CALLBACK, FunctionAddress(&SetScalingRatioCallback));
    p->Set(PARAM_PRESET, createParams.preset);
    p->Set(NVSDK_NGX_Parameter_Width, _width);
    p->Set(NVSDK_NGX_Parameter_Height, _height);
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
    p->Set(PARAM_COLOR, _d3d12->InputColor());
    p->Set(PARAM_OUTPUT, _d3d12->OutputColor());
    p->Set(PARAM_MVEC, _d3d12->Motion());
    p->Set(PARAM_DEPTH, _d3d12->Depth());
    SetSubrect(p, RESOURCE_PARAMETERS[0], _width, _height);
    SetSubrect(p, RESOURCE_PARAMETERS[1], _width, _height);
    SetSubrect(p, RESOURCE_PARAMETERS[2], _width, _height);
    SetSubrect(p, RESOURCE_PARAMETERS[3], _width, _height);
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

    // 4) Frame resources incl. zero-guidance textures
    if (!_d3d12->CreateFrameResources(_width, _height, err, errLen)) return false;

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

bool DlssnrContext::RecreateFeature(int preset, char *err, size_t errLen) noexcept {
    if (!_ready || !_snippetReleaseFeature) {
        if (err && errLen) std::snprintf(err, errLen, "RecreateFeature: context not ready");
        return false;
    }
    // Preset is a create-time key: release + re-allocate the parameter block,
    // then CreateFeature again (device/queues/textures untouched).
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
    std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: preset=%d feature recreated", preset);
    DbgLine(msg);
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
    // Panel preset changes require a feature rebuild; consume before packing.
    if (int newPreset = -1; _shared->ConsumePresetChange(newPreset)) {
        if (!RecreateFeature(newPreset, err, errLen)) return false;
    }
    const bool timing = timingOut && timingLen > 0;
    LARGE_INTEGER qpcFreq{}, t0{}, t1{}, t2{}, t3{};
    if (timing) {
        QueryPerformanceFrequency(&qpcFreq);
        QueryPerformanceCounter(&t0);
    }
    // Diagnostic: VSDLSSNR_SKIP_EVAL=1 measures the pipe without NGX evaluate
    static const bool skipEval = GetEnvironmentVariableA("VSDLSSNR_SKIP_EVAL", nullptr, 0) != 0;
    if (!_d3d12->PackInput(srcPlanes, srcStrides, width, height, err, errLen)) return false;
    if (timing) QueryPerformanceCounter(&t1);

    if (!_d3d12->BeginRecording()) {
        if (err && errLen) std::snprintf(err, errLen, "BeginRecording(frame) failed");
        return false;
    }

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
        // Barrier set mirrors Magpie Draw() (cpp:1932-1945 / 2006-2012):
        // inputs COMMON -> NON_PIXEL_SHADER_RESOURCE, output COMMON -> UNORDERED_ACCESS.
        D3D12_RESOURCE_BARRIER barriers[4]{
            TransitionTo(_d3d12->InputColor(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            TransitionTo(_d3d12->Motion(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            TransitionTo(_d3d12->Depth(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            TransitionTo(_d3d12->OutputColor(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        cl->ResourceBarrier(4, barriers);

        DWORD sehCode = 0;
        if (!SetEvaluateParametersSafely(resetHistory, &sehCode)) {
            if (err && errLen) std::snprintf(err, errLen, "Evaluate parameter setup raised SEH");
            return false;
        }
        const NVSDK_NGX_Result r = SnippetEvaluateSafely(cl, _parameters, &sehCode);
        if (sehCode) {
            if (err && errLen) std::snprintf(err, errLen, "EvaluateFeature raised SEH");
            return false;
        }
        if (!NVSDK_NGX_SUCCEED(r)) {
            if (err && errLen) std::snprintf(err, errLen, "EvaluateFeature failed (0x%x)", static_cast<unsigned>(r));
            return false;
        }

        for (auto &b : barriers) {
            D3D12_RESOURCE_BARRIER back = b;
            D3D12_RESOURCE_STATES tmp = back.Transition.StateBefore;
            back.Transition.StateBefore = back.Transition.StateAfter;
            back.Transition.StateAfter = tmp;
            cl->ResourceBarrier(1, &back);
        }
    }

    if (!_d3d12->RecordReadbackCopy(err, errLen)) return false;
    if (!_d3d12->ExecuteAndWait()) {
        if (err && errLen) std::snprintf(err, errLen, "Execute(frame) failed");
        return false;
    }
    if (timing) QueryPerformanceCounter(&t2);
    const bool rb = _d3d12->UnpackOutput(dstPlanes, dstStrides, width, height, err, errLen);
    if (timing) {
        LARGE_INTEGER t3{};
        QueryPerformanceCounter(&t3);
        const auto ms = [](LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER f) {
            return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
        };
        std::snprintf(timingOut, timingLen, "pack=%.1f,submit+gpu=%.1f,unpack=%.1f",
                      ms(t0, t1, qpcFreq), ms(t1, t2, qpcFreq), ms(t2, t3, qpcFreq));
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
