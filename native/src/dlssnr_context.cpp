// Ported from Magpie experimental DLSSNRFilter.cpp / NgxD3D12Core.cpp (see header).
#include "dlssnr_context.h"
#include "ngx_runtime_guard.h"
#include "panel_ipc.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <share.h> // _SH_DENYNO(timing log 显式共享读)

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

void DbgLine(const char *msg) noexcept {
    OutputDebugStringA("vs_dlssnr: ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
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
    // Thin wrappers over the shared builder (d3d12_context.h); the barrier
    // struct fill itself lives in exactly one place.
    return Transition(resource, D3D12_RESOURCE_STATE_COMMON, after);
}

D3D12_RESOURCE_BARRIER TransitionFromTo(
    ID3D12Resource *resource,
    D3D12_RESOURCE_STATES from,
    D3D12_RESOURCE_STATES to) noexcept {
    return Transition(resource, from, to);
}

} // namespace (Transition helpers)

// diagnostics: append timing lines next to the host exe.
// Controlled by the panel ("性能日志" toggle; wired up by the panel-IPC step).
std::atomic<bool> g_timingLogEnabled{ true };
// fmParallel: several frame threads push timing samples / flush the log
std::mutex g_timingMutex;

namespace {

// Path + append handle, resolved once under g_timingMutex. A persistent
// handle avoids a full CreateFile/close cycle (through the filesystem filter
// stack) on every 60th frame; disabling the toggle closes it again so the
// log file can be deleted/moved while logging is off.
wchar_t g_timingLogPath[MAX_PATH] = L"";
FILE *g_timingLogFile = nullptr;

// 前置声明:SetTimingLogEnabled 的翻转留痕要在定义之前调用 TimingLog。
void TimingLog(const char *line) noexcept;

} // namespace

void SetTimingLogEnabled(bool enabled) noexcept {
    const bool prev = g_timingLogEnabled.load(std::memory_order_relaxed);
    if (prev == enabled) return;
    // 翻转本身留痕:日志中段的"空白期"从此可以定责(面板关了开关),
    // 而不是被误读为"插件没写日志"。OFF 必须在翻标志之前写(标志关了
    // TimingLog 直接 no-op)。
    if (!enabled) TimingLog("DLSSNR STATUS: timing log disabled (panel toggle)");
    g_timingLogEnabled.store(enabled, std::memory_order_relaxed);
    if (enabled) TimingLog("DLSSNR STATUS: timing log enabled (panel toggle)");
    if (!enabled) {
        std::lock_guard<std::mutex> lock(g_timingMutex);
        if (g_timingLogFile) {
            fclose(g_timingLogFile);
            g_timingLogFile = nullptr;
        }
    }
}

namespace {

void TimingLog(const char *line) noexcept {
    if (!g_timingLogEnabled.load(std::memory_order_relaxed)) return;
    // concurrent frame threads: serialize appends so lines never interleave;
    // the lazy path/handle init below lives inside the lock (it used to race
    // a partially-written path between the first concurrent threads).
    std::lock_guard<std::mutex> lock(g_timingMutex);
    if (!g_timingLogPath[0]) {
        wchar_t dir[MAX_PATH];
        if (!GetModuleFileNameW(nullptr, dir, MAX_PATH)) return;
        swprintf_s(g_timingLogPath, MAX_PATH, L"%s\\dlssnr_timing.log",
                   std::filesystem::path(dir).parent_path().c_str());
    }
    if (!g_timingLogFile) {
        // 显式 DENYNO 共享(_wfopen_s 的共享语义不可靠:实测同进程 python
        // 读被拒 PermissionError)—— 常驻句柄必须允许外部读者(timing log
        // 的全部价值就在跨进程可读)。
        g_timingLogFile = _wfsopen(g_timingLogPath, L"a", _SH_DENYNO);
        if (!g_timingLogFile) {
            // 探针:日志自身打不开(磁盘满/文件被锁)只报一次 —— 否则每行
            // 静默丢失,"日志里怎么没有"无从查起。
            static bool warnedOpen = false;
            if (!warnedOpen) {
                warnedOpen = true;
                OutputDebugStringA("vs_dlssnr: timing log open FAILED (disk full? file locked?)\n");
            }
            return;
        }
        // 会话头:标记这份日志来自哪个二进制。#35(部署错位置/旧时间戳
        // 误判)与 #44(增量编译陈旧造成 ABI 撕裂)两起事故的定位成本都
        // 卡在"这份日志是不是新代码写的" —— build 戳 + pid + 协议 magic
        // 一行定案。flags 回显诊断环境变量现场:忘关的 SKIP_EVAL/DUMP
        // 会把测量结论整个带偏,这里直接留痕。重新打开(面板开关 OFF→ON)
        // 会再写一条,顺带标记续写点。
        const auto envOn = [](const char *name) {
            return GetEnvironmentVariableA(name, nullptr, 0) != 0;
        };
        char header[256];
        snprintf(header, sizeof(header),
                 "DLSSNR STATUS: session start pid=%lu build=\"%s %s\" payload_magic=0x%08X flags=[%s%s%s%s%s]",
                 GetCurrentProcessId(), __DATE__, __TIME__,
                 static_cast<unsigned>(PAYLOAD_MAGIC),
                 envOn("VSDLSSNR_DUMP") ? " dump" : "",
                 envOn("VSDLSSNR_SKIP_EVAL") ? " skip_eval" : "",
                 envOn("VSDLSSNR_D3D12_DEBUG") ? " d3d12_debug" : "",
                 envOn("VSDLSSNR_NGX_LOG") ? " ngx_log" : "",
                 envOn("VSDLSSNR_PROBE") ? " probe" : "");
        fwrite(header, 1, strlen(header), g_timingLogFile);
        fputc('\n', g_timingLogFile);
        fflush(g_timingLogFile);
    }
    // Timestamp every line: the log is append-only across sessions and hosts,
    // and without timestamps a recreate storm cannot be attributed to the
    // session that caused it.
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamped[512];
    snprintf(stamped, sizeof(stamped), "[%02u:%02u:%02u.%03u] %s",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
    fwrite(stamped, 1, strlen(stamped), g_timingLogFile);
    if (!strlen(stamped) || stamped[strlen(stamped) - 1] != '\n') fputc('\n', g_timingLogFile);
    // keep the line on disk even if the session dies before the next flush
    fflush(g_timingLogFile);
}

} // namespace (TimingLog helpers)

// TimingLog 本体在匿名命名空间;nvof_context 等外部翻译单元经由此包装写
// STATUS 行(失败必须进 timing log —— 跨进程观测只认它)。
void TimingStatusLine(const char *line) noexcept { TimingLog(line); }

// 逐帧级探针开关(VSDLSSNR_PROBE=1 启用;逐帧行会淹没 perf 行,平时关)。
// 导出到头文件供 d3d12_context 的 init 期细分探针共用。
bool ProbeEnabled() noexcept {
    static const bool on = GetEnvironmentVariableA("VSDLSSNR_PROBE", nullptr, 0) != 0;
    return on;
}

// Internal NGX processing size for a resolution percent (25-100; aligned to even)
// Internal NGX processing size (aligned to even). scalingEnabled == 0 forces
// the full source size (internal-resolution scaling off).
static void InternalSize(int w, int h, int pct, int &iw, int &ih) noexcept {
    const int p = std::clamp(pct, kResPctMin, kResPctMax);
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
    double nvof[120]{};
    double evalCpu[120]{};
    double unpack[120]{};
    double slotW[120]{}; // AcquireSlot 等待(池耗尽 = 吞吐瓶颈第一现场)
    double lockW[120]{}; // _evaluateMutex 等待(NGX 单例串行化的排队)
    int count = 0;
    int idx = 0;

    void Push(double g, double p, double n, double e, double u, double sw, double lw) noexcept {
        gpu[idx] = g; pack[idx] = p; nvof[idx] = n; evalCpu[idx] = e; unpack[idx] = u;
        slotW[idx] = sw; lockW[idx] = lw;
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
// All SDK entries go through NgxRuntimeGuard::Invoke: after the first SEH the
// process-level latch fails every further SDK call fast (the SEH may have
// left NGX's internal critical section held — re-entering would deadlock).

NVSDK_NGX_Result DlssnrContext::CoreInitSafely(
    const wchar_t *appDir, ID3D12Device *device,
    const NVSDK_NGX_FeatureCommonInfo *info, DWORD *sehCode) noexcept {
    return NgxRuntimeGuard::Invoke([&] {
        return NVSDK_NGX_D3D12_Init_with_ProjectID(
            PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, ENGINE_NAME,
            appDir, device, info, NVSDK_NGX_Version_API);
    }, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

NVSDK_NGX_Result DlssnrContext::CoreAllocateParametersSafely(NVSDK_NGX_Parameter **out, DWORD *sehCode) noexcept {
    return NgxRuntimeGuard::Invoke([&] {
        return NVSDK_NGX_D3D12_AllocateParameters(out);
    }, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

NVSDK_NGX_Result DlssnrContext::CoreGetCapabilityParametersSafely(NVSDK_NGX_Parameter **out, DWORD *sehCode) noexcept {
    return NgxRuntimeGuard::Invoke([&] {
        return NVSDK_NGX_D3D12_GetCapabilityParameters(out);
    }, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

NVSDK_NGX_Result DlssnrContext::CoreDestroyParametersSafely(NVSDK_NGX_Parameter *p, DWORD *sehCode) noexcept {
    return NgxRuntimeGuard::Invoke([&] {
        return NVSDK_NGX_D3D12_DestroyParameters(p);
    }, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

NVSDK_NGX_Result DlssnrContext::CoreShutdownSafely(ID3D12Device *device, DWORD *sehCode) noexcept {
    return NgxRuntimeGuard::Invoke([&] {
        return NVSDK_NGX_D3D12_Shutdown1(device);
    }, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

NVSDK_NGX_Result DlssnrContext::SnippetInitSafely(const wchar_t *appDataPath, ID3D12Device *device, DWORD *sehCode) noexcept {
    return NgxRuntimeGuard::Invoke([&] {
        return _snippetInitExt(
            DLSSNR_SIGNED_SNIPPET_APPLICATION_ID, appDataPath,
            device, NVSDK_NGX_Version_API, nullptr);
    }, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

NVSDK_NGX_Result DlssnrContext::SnippetCreateFeatureSafely(ID3D12GraphicsCommandList *cl, NVSDK_NGX_Parameter *params, DWORD *sehCode) noexcept {
    return NgxRuntimeGuard::Invoke([&] {
        return _snippetCreateFeature(cl, FEATURE_DLSSNR, params, &_feature);
    }, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

NVSDK_NGX_Result DlssnrContext::SnippetEvaluateSafely(ID3D12GraphicsCommandList *cl, NVSDK_NGX_Parameter *params, DWORD *sehCode) noexcept {
    return NgxRuntimeGuard::Invoke([&] {
        return _snippetEvaluateFeature(cl, _feature, params, nullptr);
    }, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

NVSDK_NGX_Result DlssnrContext::SnippetReleaseSafely(DWORD *sehCode) noexcept {
    return NgxRuntimeGuard::Invoke([&] {
        return _snippetReleaseFeature(_feature);
    }, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

NVSDK_NGX_Result DlssnrContext::SnippetShutdownSafely(DWORD *sehCode) noexcept {
    return NgxRuntimeGuard::Invoke([&] {
        return _snippetShutdown(_d3d12->Device());
    }, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
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
    return NgxRuntimeGuard::Invoke([&] {
        SetCreateParametersUnsafe();
        return true;
    }, false, sehCode);
}

void DlssnrContext::SetEvaluateParametersUnsafe(FrameSlot &slot, bool resetHistory, bool realMotion) noexcept {
    NVSDK_NGX_Parameter *p = _parameters;
    const DlssnrParams params = _shared->Snapshot();
    // With internal-resolution scaling, NGX consumes/produces the reduced
    // textures; the residual composite then reconstructs the full-size output.
    const bool scaling = _d3d12->HasScaling();
    const int ew = scaling ? _d3d12->InternalWidth() : _width;
    const int eh = scaling ? _d3d12->InternalHeight() : _height;
    p->Set(PARAM_COLOR, scaling ? _d3d12->ReducedColor(slot) : _d3d12->InputColor(slot));
    p->Set(PARAM_OUTPUT, scaling ? _d3d12->ReducedDenoised(slot) : _d3d12->OutputColor(slot));
    // 真光流:缩放启用时消费降采样后的运动(内部尺寸,向量已换算到内部
    // 像素单位,MVecScale 保持 1);否则直接消费 densify 输出(源尺寸)。
    // realMotion=false → 静态零纹理(零 guidance 路径)。
    p->Set(PARAM_MVEC, _d3d12->MotionResource(slot, realMotion, scaling));
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

bool DlssnrContext::SetEvaluateParametersSafely(FrameSlot &slot, bool resetHistory, bool realMotion, DWORD *sehCode) noexcept {
    return NgxRuntimeGuard::Invoke([&] {
        SetEvaluateParametersUnsafe(slot, resetHistory, realMotion);
        return true;
    }, false, sehCode);
}

// ---- Lifecycle ----

bool DlssnrContext::Initialize(
    D3D12Context &d3d12, const wchar_t *ngxDllPath, const wchar_t *fgDllPath,
    int width, int height, int depth, SharedParams *shared,
    char *err, size_t errLen) noexcept {
    auto fail = [&](const char *what) {
        if (err && errLen) std::snprintf(err, errLen, "%s", what);
        DbgLine(what);
        // 初始化失败 = 本实例整体直通:原因串同时发进 stats,面板可见
        // (mpv 日志在 GUI 下不可见,只留这里是又一次静默降级)。
        PublishDeadState("passthrough", what);
        return false;
    };
    // err 已由被调方填好时的失败出口(CreateFrameResources / RebuildScaling)
    auto failWithExistingErr = [&]() {
        DbgLine(err);
        PublishDeadState("passthrough", err);
        return false;
    };

    _d3d12 = &d3d12;
    _width = width;
    _height = height;
    _depth = depth;
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
        if (sehCode) return fail("NGX core Init_with_ProjectID raised SEH");
        if (!NVSDK_NGX_SUCCEED(r)) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "NGX core Init_with_ProjectID failed (0x%x)", static_cast<unsigned>(r));
            return fail(msg);
        }
        _coreInitialized = true;
        if (ProbeEnabled()) TimingStatusLine("PROBE: ngx core ok"); // init 细分(DEVICE_HUNG 时序定位)
    }

    // 2) Allocate the parameter block from the core; the snippet receives the
    // same block for CreateFeature/EvaluateFeature (Magpie DLSSNRFilter.cpp:1717).
    {
        DWORD sehCode = 0;
        const NVSDK_NGX_Result r = CoreAllocateParametersSafely(&_parameters, &sehCode);
        if (sehCode) return fail("CoreAllocateParameters raised SEH");
        if (!NVSDK_NGX_SUCCEED(r) || !_parameters) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "CoreAllocateParameters failed (0x%x)",
                          static_cast<unsigned>(r));
            return fail(msg);
        }
    }

    // 3) Signed snippet load (Magpie InitializeSignedSnippet, cpp:1146-1195).
    //    The caller-compatibility IAT hook must be installed before Init runs.
    _snippetModule = LoadLibraryExW(
        ngxDllPath, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!_snippetModule) return fail("LoadLibraryExW(nvngx_dlssnr.dll) failed");
    // 探针:snippet 模型文件的部署指纹(大小 + 修改时间)。模型 DLL 与
    // 插件/面板是三件套分离部署,#11 模型 fallback、装错/装旧模型这类
    // 问题从这一行直接核对。
    {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(ngxDllPath, GetFileExInfoStandard, &fa)) {
            SYSTEMTIME stUtc{}, stLoc{};
            FileTimeToSystemTime(&fa.ftLastWriteTime, &stUtc);
            SystemTimeToTzSpecificLocalTime(nullptr, &stUtc, &stLoc);
            ULARGE_INTEGER sz{};
            sz.HighPart = fa.nFileSizeHigh;
            sz.LowPart = fa.nFileSizeLow;
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "DLSSNR STATUS: snippet dll %llu bytes mtime=%04u-%02u-%02u %02u:%02u:%02u",
                          static_cast<unsigned long long>(sz.QuadPart),
                          stLoc.wYear, stLoc.wMonth, stLoc.wDay,
                          stLoc.wHour, stLoc.wMinute, stLoc.wSecond);
            TimingLog(msg);
        }
    }

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
    if (ProbeEnabled()) TimingStatusLine("PROBE: pre frame-res"); // init 细分(DEVICE_HUNG 时序定位)
    // FG 请求 = 创建时参数快照的 fgEnabled(桥接 ini/payload 采纳已在此前
    // 完成);sticky —— 尺寸重建(RecreateFeature)沿用本旗标,面板运行中
    // 改变只影响逐帧 eval 门,不重建槽资源。
    _fgRequested = _shared->Snapshot().fgEnabled != 0;
    if (!_d3d12->CreateFrameResources(_width, _height, _depth, _fgRequested, err, errLen)) return failWithExistingErr();
    if (_shared->Snapshot().scalingEnabled) {
        const int pct = std::clamp(_shared->Snapshot().inputResolutionPercent, kResPctMin, kResPctMax);
        int iw = _width, ih = _height;
        InternalSize(_width, _height, pct, iw, ih);
        if (!_d3d12->RebuildScaling(iw, ih, err, errLen)) return failWithExistingErr();
    }

    // 4a) DLSS FG 会话(挂 NR 之后;先于 NVOF 建立,使光流 follow 的会话
    // 输入尺寸决策能感知 FG)。失败优雅降级:滤镜回退 1:1 输出(不翻倍
    // 帧率),NR 不受影响 —— FG 的 SEH 走本地闩锁,不进全局 NgxRuntimeGuard。
    if (_fgRequested) {
        // FG 路由选择(0=自动:官方优先、不可用回落 proxy(SM86);1=SM86;
        // 2=SM75;3=仅官方 NGX)。钉档失败不跨后端回退(选错档 = FG 关,
        // 输出 1:1),免去自动档在能力外硬件上每次创建的官方双探开销
        // (3080 实测:先 capability 拒载才落 proxy)。
        const int fgRoute =
            std::clamp(_shared->Snapshot().fgRoute, kFgRouteMin, kFgRouteMax);
        // 官方 NGX 分支(PORTING #8):官方签名 nvngx_dlssg.dll 与模型
        // DLL 同目录(ngx\)部署且驱动报告 FG 能力(RTX 40/50)时,经共享
        // NGX core 走官方签名链 —— 免自签 proxy 与杀软误报面,cubin 由
        // NVIDIA 预编译(SM89/SM120,无 PTX JIT)。参数块 = GetCapability
        // (官方 DLSSG 的 Magpie 同款 create/eval 块)。自动档不可用时回落
        // dlssg_for_sm86 proxy —— RTX 30/20 官方拒载(FrameGeneration
        // .Available=0),proxy 是该区间唯一路径。
        wchar_t officialDll[MAX_PATH]{};
        {
            const std::filesystem::path p =
                std::filesystem::path(ngxDllPath).parent_path() / L"nvngx_dlssg.dll";
            const std::wstring ws = p.wstring();
            if (ws.size() < MAX_PATH) {
                const DWORD attr = GetFileAttributesW(ws.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                    std::memcpy(officialDll, ws.c_str(), (ws.size() + 1) * sizeof(wchar_t));
                }
            }
        }
        bool fgUp = false;
        if ((fgRoute == kFgRouteAuto || fgRoute == kFgRouteOfficial) && officialDll[0]) {
            DWORD sehCode = 0;
            const NVSDK_NGX_Result pr = CoreGetCapabilityParametersSafely(&_fgParams, &sehCode);
            if (!sehCode && NVSDK_NGX_SUCCEED(pr) && _fgParams) {
                _fg = std::make_unique<DlssfgContext>();
                char fgErr[256]{};
                if (_fg->Initialize(*_d3d12, officialDll, _appDataPath, _fgParams,
                                    _width, _height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                    DlssfgContext::FgBackend::OfficialNgx,
                                    fgErr, sizeof(fgErr))) {
                    fgUp = true;
                } else {
                    char msg[352];
                    std::snprintf(msg, sizeof(msg),
                                  "DLSSNR STATUS: dlssfg official init failed (%s); %s",
                                  fgErr,
                                  fgRoute == kFgRouteOfficial ? "FG off (route pinned)"
                                                              : "trying proxy");
                    DbgLine(msg);
                    TimingStatusLine(msg);
                    _fg.reset();
                    // 参数块:FG 死了块也作废(core 参数块无成本,留着会话内
                    // 复用反而要考虑并发;直接随进程回收,Shutdown 不再触碰)。
                    _fgParams = nullptr;
                }
            } else {
                TimingStatusLine(fgRoute == kFgRouteOfficial
                                     ? "DLSSNR STATUS: dlssfg official capability block FAILED; FG off (route pinned)"
                                     : "DLSSNR STATUS: dlssfg official capability block FAILED; trying proxy");
            }
        }
        if (!fgUp && fgRoute != kFgRouteOfficial && fgDllPath && fgDllPath[0]) {
            DWORD sehCode = 0;
            const NVSDK_NGX_Result pr = CoreAllocateParametersSafely(&_fgParams, &sehCode);
            if (!sehCode && NVSDK_NGX_SUCCEED(pr) && _fgParams) {
                _fg = std::make_unique<DlssfgContext>();
                char fgErr[256]{};
                if (_fg->Initialize(*_d3d12, fgDllPath, _appDataPath, _fgParams,
                                    _width, _height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                    DlssfgContext::FgBackend::Proxy,
                                    fgErr, sizeof(fgErr))) {
                    fgUp = true;
                } else {
                    char msg[352];
                    std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: dlssfg init failed (%s); 1:1 output",
                                  fgErr);
                    DbgLine(msg);
                    TimingStatusLine(msg);
                    _fg.reset();
                    _fgParams = nullptr;
                }
            } else {
                TimingStatusLine("DLSSNR STATUS: dlssfg core parameter block FAILED; 1:1 output");
            }
        }
        if (!fgUp) {
            // 钉档连尝试都没发生(部署缺失)时,失败原因没有其它出口,
            // 这里补一行;各尝试路径自身失败已有带因状态行。
            if (fgRoute == kFgRouteOfficial && !officialDll[0]) {
                TimingStatusLine(
                    "DLSSNR STATUS: dlssfg route pinned official but nvngx_dlssg.dll missing; 1:1 output");
            } else if ((fgRoute == kFgRouteProxySm86 || fgRoute == kFgRouteProxySm75) &&
                       !(fgDllPath && fgDllPath[0])) {
                TimingStatusLine(
                    "DLSSNR STATUS: dlssfg route pinned proxy but version.dll missing; 1:1 output");
            }
            _fgRequested = false; // 槽资源已带 FG 纹理,无害留用
        }
    }

    // 4b) NVOF 光流会话(PORTING #6):quality > 0 时建立;失败优雅回退零
    // guidance(记 _nvofFailed,不拖垮整个滤镜)。冷初始化单线程、槽池空闲,
    // 满足 NvofContext::Initialize 的 PoolHold 约束。
    {
        const int ofq = std::clamp(_shared->Snapshot().motionVectorQuality, kOfQualityMin, kOfQualityMax);
        _curOfQuality = ofq;
        _nvofFailed = false;
        {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: init of=%d", ofq);
            TimingStatusLine(msg);
        }
        if (ofq > 0) {
            char nvofErr[160]{};
            _nvof = std::make_unique<NvofContext>();
            if (_nvof->Initialize(*_d3d12, _width, _height, ofq, nvofErr, sizeof(nvofErr))) {
                char msg[160];
                std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: nvof session created quality=%d %dx%d",
                              ofq, _width, _height);
                DbgLine(msg);
                TimingLog(msg);
            } else {
                _nvofFailed = true;
                char msg[288];
                std::snprintf(msg, sizeof(msg),
                              "DLSSNR STATUS: nvof init failed (%s); zero guidance", nvofErr);
                DbgLine(msg);
                TimingLog(msg);
            }
        }
    }

    // Publish the render GPU's name so the panel shows it before the first
    // frame lands (queried once from the adapter the device was created on;
    // the per-frame stats publish reuses the cached string). Body also carries
    // the initial filter state + actual NVOF mode: a failed NVOF init (zero
    // guidance) is a degradation, and the panel must not wait for the first
    // stats publish to learn it.
    {
        DXGI_ADAPTER_DESC desc{};
        if (_d3d12->Adapter() && SUCCEEDED(_d3d12->Adapter()->GetDesc(&desc))) {
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                _gpuNameUtf8, sizeof(_gpuNameUtf8), nullptr, nullptr);
        }
        char body[288];
        std::snprintf(body, sizeof(body),
                      "{\"gpu_name\":\"%s\",\"width\":%d,\"height\":%d,"
                      "\"%s\":\"%s\",\"%s\":\"%s\"}",
                      _gpuNameUtf8, _width, _height,
                      SK_FILTER_STATE, (_nvofFailed && _curOfQuality > 0) ? "nvof_zero" : "ok",
                      SK_OF_MODE, OfModeString());
        PublishStatsJson(body);
    }

    // 5) CreateFeature on the control-path command list, then close+execute
    // (Magpie cpp:1720-1758). Initialization is single-threaded (no frame
    // threads yet), the ctl mutex is taken for symmetry with RecreateFeature.
    {
        std::lock_guard<std::mutex> ctlLock(_d3d12->CtlMutex());
        if (!_d3d12->BeginCtlRecording()) return fail("BeginCtlRecording(create) failed");
        {
            DWORD sehCode = 0;
            if (!SetCreateParametersSafely(&sehCode)) return fail("Create parameter setup raised SEH");
        }
        {
            DWORD sehCode = 0;
            const NVSDK_NGX_Result r = SnippetCreateFeatureSafely(_d3d12->CtlCommandList(), _parameters, &sehCode);
            if (sehCode) return fail("Snippet CreateFeature raised SEH");
            if (!NVSDK_NGX_SUCCEED(r) || !_feature) {
                char msg[96];
                std::snprintf(msg, sizeof(msg), "Feature 18 creation failed (0x%x)", static_cast<unsigned>(r));
                return fail(msg);
            }
        }
        if (!_d3d12->ExecuteCtlAndWait()) return fail("Execute(create) failed");
    }

    _ready = true;
    {
        const DlssnrParams p = _shared->Snapshot();
        _curPreset = p.preset;
        _curRes = p.inputResolutionPercent;
        _curScaling = p.scalingEnabled;
        char msg[288];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: Feature=18 created=true path=signed-snippet %dx%dd%d disabled=false gpu=%.60s",
                      _width, _height, _depth, _gpuNameUtf8);
        DbgLine(msg);
        // Rebind/RecreateFeature already log through TimingLog; log the cold
        // path too so the three lifecycle outcomes are distinguishable in
        // dlssnr_timing.log alone (logMessage does not reach mpv's log).
        TimingLog(msg);
    }
    return true;
}

bool DlssnrContext::RecreateFeature(int preset, int resPercent, int scalingEnabled, char *err, size_t errLen,
                                    int newWidth, int newHeight, int newDepth) noexcept {
    if (!_ready.load(std::memory_order_acquire) || !_snippetReleaseFeature) {
        if (err && errLen) std::snprintf(err, errLen, "RecreateFeature: context not ready");
        return false;
    }
    // Preset / internal-resolution / scaling-toggle are create-time keys:
    // release the feature, rebuild scaling textures when needed, then
    // CreateFeature again (device/queues untouched). Scaling disabled means
    // the residual pipeline is dropped from the frame flow entirely.
    //
    // Seal the slot pool BEFORE any NGX call: release racing a concurrent
    // evaluate on another frame thread corrupts the snippet and wedges the
    // pipeline (observed as a hard hang on preset/resolution changes), and
    // the per-slot scaling textures are replaced wholesale. With the pool
    // sealed there is by definition no concurrent evaluate — no NGX-side
    // lock is needed. ConsumeRebuild is consumed before any slot is acquired
    // on this thread, so this thread holds no slot and the drain completes.
    D3D12Context::PoolHold pool(*_d3d12);
    // A video-size change replaces every slot's frame resources first —
    // outside CtlMutex because the guidance clear inside takes it itself.
    // All GPU work is complete (slots are only released after WaitFrame).
    const bool resize = newWidth > 0 &&
                        (newWidth != _width || newHeight != _height || newDepth != _depth);
    if (resize && !_d3d12->CreateFrameResources(newWidth, newHeight, newDepth, _fgRequested, err, errLen)) {
        _ready.store(false, std::memory_order_release);
        return false;
    }
    if (resize) {
        _width = newWidth;
        _height = newHeight;
        _depth = newDepth;
        // FG feature 随尺寸重建(proxy Release + CreateFeature;失败 = FG
        // 闩停,降级复制帧,NR 不受影响)。历史在重建时作废。
        if (_fg && _fg->Enabled()) {
            char fgErr[192]{};
            if (!_fg->Rebuild(_width, _height, DXGI_FORMAT_B8G8R8A8_UNORM, fgErr, sizeof(fgErr))) {
                char msg[288];
                std::snprintf(msg, sizeof(msg),
                              "DLSSNR STATUS: dlssfg rebuild failed (%s); FG off (dup)",
                              fgErr);
                DbgLine(msg);
                TimingStatusLine(msg);
            }
        }
        // NVOF 会话随尺寸重建(已在 PoolHold 内,内联处理,勿调 RebuildNvof
        // —— 那会二次取 PoolHold 死锁)。退役旧会话不销毁(见 _retiredNvof
        // 注释),失败降级零 guidance,不致命。会话输入尺寸遵循 follow 语义
        // (scalingEnabled/resPercent 是本次重建的目标态,内部尺寸由参数
        // 直推,不依赖尚未执行的 RebuildScaling)。
        if (_nvof && _curOfQuality > 0 && !_nvofFailed) {
            const DlssnrParams sp = _shared->Snapshot();
            // FG 激活时 MVecs 契约要求源尺寸稠密运动 —— follow 被忽略。
            const bool foll = sp.nvofFollowScaling != 0 && scalingEnabled && !(_fg && _fg->Enabled());
            int iw = _width, ih = _height;
            if (foll) InternalSize(_width, _height, resPercent, iw, ih);
            auto next = std::make_unique<NvofContext>();
            char nvofErr[160]{};
            if (next->Initialize(*_d3d12, iw, ih, _curOfQuality,
                                 nvofErr, sizeof(nvofErr))) {
                _retiredNvof.push_back(std::move(_nvof));
                _nvof = std::move(next);
            } else {
                _nvofFailed = true;
                char msg[288];
                std::snprintf(msg, sizeof(msg),
                              "DLSSNR STATUS: nvof resize failed (%s); zero guidance", nvofErr);
                DbgLine(msg);
                TimingLog(msg);
            }
        }
    }
    std::lock_guard<std::mutex> ctlLock(_d3d12->CtlMutex());
    {
        DWORD sehCode = 0;
        SnippetReleaseSafely(&sehCode);
        _feature = nullptr;
        // From here on the old feature is gone: every failure path below must
        // drop _ready so ProcessFrame stops evaluating — the rebuild request
        // is consumed by ConsumeRebuild and will never re-fire on its own.
        sehCode = 0;
        CoreDestroyParametersSafely(_parameters, &sehCode);
        _parameters = nullptr;
        sehCode = 0;
        const NVSDK_NGX_Result r = CoreAllocateParametersSafely(&_parameters, &sehCode);
        if (sehCode || !NVSDK_NGX_SUCCEED(r) || !_parameters) {
            if (err && errLen) std::snprintf(err, errLen, "RecreateFeature: AllocateParameters failed");
            _ready.store(false, std::memory_order_release);
            return false;
        }
        if (scalingEnabled) {
            int iw = _width, ih = _height;
            InternalSize(_width, _height, resPercent, iw, ih);
            if (!_d3d12->RebuildScaling(iw, ih, err, errLen)) {
                _ready.store(false, std::memory_order_release);
                return false;
            }
        } else {
            _d3d12->ClearScalingResources();
        }
    }
    if (!_d3d12->BeginCtlRecording()) {
        if (err && errLen) std::snprintf(err, errLen, "RecreateFeature: BeginCtlRecording failed");
        _ready.store(false, std::memory_order_release);
        return false;
    }
    {
        DWORD sehCode = 0;
        if (!SetCreateParametersSafely(&sehCode)) {
            if (err && errLen) std::snprintf(err, errLen, "RecreateFeature: parameter setup raised SEH");
            _ready.store(false, std::memory_order_release);
            return false;
        }
        const NVSDK_NGX_Result r = SnippetCreateFeatureSafely(_d3d12->CtlCommandList(), _parameters, &sehCode);
        if (sehCode || !NVSDK_NGX_SUCCEED(r) || !_feature) {
            if (err && errLen) std::snprintf(err, errLen, "RecreateFeature: CreateFeature failed (0x%x)",
                static_cast<unsigned>(sehCode ? 0xFFFFFFFFu : r));
            _ready.store(false, std::memory_order_release);
            return false;
        }
    }
    if (!_d3d12->ExecuteCtlAndWait()) {
        if (err && errLen) std::snprintf(err, errLen, "RecreateFeature: Execute failed");
        _ready.store(false, std::memory_order_release);
        return false;
    }
    char msg[160];
    snprintf(msg, sizeof(msg),
             "DLSSNR STATUS: preset=%d res=%d%% scaling=%d internal=%dx%d d=%d feature recreated%s frame=%d",
             preset, scalingEnabled ? resPercent : 100, scalingEnabled,
             _d3d12->InternalWidth(), _d3d12->InternalHeight(), _depth,
             resize ? " (size changed)" : "", _lastFrameN.load(std::memory_order_relaxed));
    DbgLine(msg);
    TimingLog(msg);
    _curPreset = preset;
    _curRes = resPercent;
    _curScaling = scalingEnabled != 0;
    return true;
}

bool DlssnrContext::RebuildNvof(int quality, int dstW, int dstH, char *err, size_t errLen) noexcept {
    // NVOF 会话重建(quality 或会话输入尺寸变化;follow 模式下会话输入 =
    // 内部尺寸,由调用方传入 dstW/dstH)。只重建光流会话,NGX feature 不动。
    // 内部自取 PoolHold(槽池封死满足 NvofContext 的调用约束)——因此
    // 绝不能在已持有 PoolHold 的路径上调用(RecreateFeature 的尺寸重建
    // 内联处理,不走这里)。调用方必须尚未持有槽位(ProcessFrame 在
    // AcquireSlot 之前消费本请求,与 ConsumeRebuild 同款)。
    //
    // _nvofMutex:fmParallel 下多个帧线程会同时看到同一档位变化并并发进入
    // (实测并发重建互相踩踏挂死);锁内复查 _curOfQuality 与尺寸,后来者
    // 直接跳过。
    std::lock_guard<std::mutex> switchLock(_nvofMutex);
    const int q = std::clamp(quality, kOfQualityMin, kOfQualityMax);
    if (q == _curOfQuality && !_nvofFailed && _nvof &&
        _nvof->Width() == dstW && _nvof->Height() == dstH) return true;
    {
        D3D12Context::PoolHold pool(*_d3d12);
        if (q == 0) {
            // 保留会话仅停用:实测 nvOFDestroy 后进程内继续 GPU 工作会触发
            // 驱动内部访问违例(nvwgf2umx,2026-09-07);空闲会话无 GPU 开销,
            // 与热上下文同哲学,进程退出统一回收。
            _nvofFailed = false;
            _curOfQuality = 0;
            TimingStatusLine("DLSSNR STATUS: nvof disabled (quality=0; session kept)");
        } else if (!_nvof || _nvofFailed ||
                   _nvof->Quality() != q || _nvof->Width() != dstW || _nvof->Height() != dstH) {
            // 先建新会话再弃旧(旧会话仅弃引用,不调用 nvOFDestroy —— 同上,
            // 销毁后继续 GPU 工作不可靠)。弃旧的 GPU 资源随 PoolHold 排空
            // 后不再被引用,纹理显存由驱动按引用回收(对象随进程生存)。
            auto next = std::make_unique<NvofContext>();
            char nvofErr[160]{};
            if (next->Initialize(*_d3d12, dstW, dstH, q, nvofErr, sizeof(nvofErr))) {
                if (_nvof) _retiredNvof.push_back(std::move(_nvof)); // 退役,不销毁
                _nvof = std::move(next);
                _nvofFailed = false;
                _curOfQuality = q;
            } else {
                _nvofFailed = true;
                _curOfQuality = q;
                char msg[288];
                std::snprintf(msg, sizeof(msg),
                              "DLSSNR STATUS: nvof init failed quality=%d (%s); zero guidance",
                              q, nvofErr);
                DbgLine(msg);
                TimingLog(msg);
                if (err && errLen) std::snprintf(err, errLen, "%s", nvofErr);
                return false; // 调用方决定是否视为致命(帧路径上不致命)
            }
        } else {
            // 复用保留的会话(q=0 期间停用):帧序门与历史都已在停用期冻结,
            // 重置让下一帧重新播种,避免旧参考帧产生一次错误流。停用期的
            // 尺寸/档位变化由上方的条件分支兜住(尺寸不符走重建)。
            _nvof->ResetHistory();
            _nvofFailed = false;
            _curOfQuality = q;
        }
    }
    if (_nvof && _nvof->Enabled()) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: nvof rebuilt quality=%d %dx%d",
                      _curOfQuality, dstW, dstH);
        DbgLine(msg);
        TimingLog(msg);
    }
    return _nvof && _nvof->Enabled();
}

void DlssnrContext::ResetNvofHistory() noexcept {
    // seek = 新时间线:流历史作废,下一帧重新播种(清零发布 + NGX PARAM_RESET;
    // FG 下一帧 eval 带 DLSSG.Reset,该帧插值输出降级复制)。会话本身保留
    // (热上下文跨 seek 存活)。
    if (_nvof) _nvof->ResetHistory();
    if (_fg) _fg->ResetHistory();
}

bool DlssnrContext::Rebind(SharedParams *shared, int width, int height, int depth,
                           char *err, size_t errLen) noexcept {
    if (!_ready.load(std::memory_order_acquire) || !_snippetReleaseFeature) {
        if (err && errLen) std::snprintf(err, errLen, "Rebind: context not ready");
        return false;
    }
    _shared = shared;
    const DlssnrParams p = _shared->Snapshot();
    // Snapshot already carries the ini overrides and the panel-payload adopt
    // the new filter instance loaded (BridgeLoadIni + BridgeAdoptPanelPayload
    // run in DlssnrCreate before this). Only a real create-time change needs
    // the feature rebuilt; a matching hot context keeps the NGX feature
    // completely warm across mpv's seek-triggered script re-initialization.
    // A different video size rebuilds frame resources + feature inside the
    // same pool-sealed RecreateFeature pass, so the ~1s bring-up survives
    // resolution changes too.
    DlssnrParams cur{};
    cur.preset = _curPreset;
    cur.inputResolutionPercent = _curRes;
    cur.scalingEnabled = _curScaling;
    const bool dimsChanged = width != _width || height != _height || depth != _depth;
    if (!dimsChanged && !CreateParamsChanged(p, cur)) {
        // 热复用:NGX feature 保持,但 seek 是新时间线 —— 光流历史必须
        // 作废(下一帧播种),光流档位同步到新实例的参数快照;此前建立
        // 失败的会话在热复用时重试一次。FG 同理(下一帧 eval 带 Reset,
        // 该帧插值输出降级复制)。
        ResetNvofHistory();
        // 会话输入尺寸同步(follow = 开关 + scaling 状态 + FG 未激活;热路
        // 径下内部尺寸未变,除非 ini/payload 同时带了 res% 变化 —— 那会走
        // 下方重建分支)。
        const bool fgHot = _fg && _fg->Enabled();
        const bool rbFollow = p.nvofFollowScaling != 0 && _d3d12->HasScaling() && !fgHot;
        const int nvW = rbFollow ? _d3d12->InternalWidth() : _width;
        const int nvH = rbFollow ? _d3d12->InternalHeight() : _height;
        if (int ofq = std::clamp(p.motionVectorQuality, kOfQualityMin, kOfQualityMax);
            ofq != _curOfQuality || (ofq > 0 && _nvof &&
                (_nvof->Width() != nvW || _nvof->Height() != nvH)) ||
            (ofq > 0 && _nvofFailed)) {
            RebuildNvof(ofq, nvW, nvH, err, errLen); // 失败仅降级零 guidance,热复用不受影响
        }
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: hot rebind kept feature (preset=%d res=%d%% scaling=%d of=%d %dx%dd%d internal=%dx%d)",
                      _curPreset, _curScaling ? _curRes : 100, _curScaling, _curOfQuality,
                      _width, _height, _depth, _d3d12->InternalWidth(), _d3d12->InternalHeight());
        DbgLine(msg);
        TimingLog(msg);
        return true;
    }
    const bool nvofOk = RecreateFeature(p.preset, p.inputResolutionPercent, p.scalingEnabled, err, errLen,
                                        dimsChanged ? width : -1, dimsChanged ? height : -1,
                                        dimsChanged ? depth : -1);
    // 新实例的参数快照可能换了光流档位(面板 seek 前调过):RecreateFeature
    // 只处理尺寸,档位变化在这里补齐。历史已在会话(重)建时作废。会话输
    // 入尺寸同样在此对齐(follow 开关/res% 变化)。
    if (nvofOk) {
        const bool fgHot = _fg && _fg->Enabled();
        const bool rbFollow = p.nvofFollowScaling != 0 && _d3d12->HasScaling() && !fgHot;
        const int nvW = rbFollow ? _d3d12->InternalWidth() : _width;
        const int nvH = rbFollow ? _d3d12->InternalHeight() : _height;
        if (int ofq = std::clamp(p.motionVectorQuality, kOfQualityMin, kOfQualityMax);
            ofq != _curOfQuality || (ofq > 0 && _nvof &&
                (_nvof->Width() != nvW || _nvof->Height() != nvH)) || (ofq > 0 && _nvofFailed)) {
            RebuildNvof(ofq, nvW, nvH, err, errLen);
        }
    }
    return nvofOk;
}

bool DlssnrContext::ProcessFrame(
    const uint8_t *const *srcPlanes, const int64_t *srcStrides,
    uint8_t **dstPlanes, int64_t *dstStrides,
    int fgMultiplier,
    uint8_t **fgDstPlanes, int64_t *fgDstStrides, bool *fgGenOk,
    int width, int height, int n,
    ColorMatrix matrix, ColorRange range,
    char *err, size_t errLen,
    char *timingOut, size_t timingLen) noexcept {
    if (fgGenOk) {
        for (int g = 0; g < kFgGenSlots; ++g) fgGenOk[g] = false;
    }
    // Faulted latch: every NGX entry returns instantly, so bail before
    // paying AcquireSlot + the full-frame CPU pack for a frame that can
    // never succeed.
    if (!_ready.load(std::memory_order_relaxed) ||
        NgxRuntimeGuard::IsFaulted()) {
        if (err && errLen) {
            if (NgxRuntimeGuard::IsFaulted()) {
                std::snprintf(err, errLen,
                              "NGX faulted (SEH 0x%x); SDK disabled until host restart",
                              NgxRuntimeGuard::FaultCode());
            } else {
                std::snprintf(err, errLen, "context not ready");
            }
        }
        // 面板可见的死亡状态(边缘发布一次):NGX fault 与重建失败在这里;
        // 设备丢失不走 —— WaitFenceValue 已发布更具体的 gpu_hang body,
        // 这里的 passthrough 会把它覆盖掉。
        if (NgxRuntimeGuard::IsFaulted()) {
            PublishDeadState("ngx_faulted", err);
        } else if (!_d3d12->IsDeviceLost()) {
            PublishDeadState("passthrough", err);
        }
        return false;
    }
    // 帧号打点(本帧后续 recreate/失败 STATUS 行的关联锚点)。
    _lastFrameN.store(n, std::memory_order_relaxed);
    // 帧序不连续性(乱序/回退/大跳)全部由 NvofContext 的帧序门自愈:到达帧
    // 与 _nextSeq(上一完成帧+1)不匹配即播种(零 guidance),链下一帧立即
    // 恢复 —— 曾在此按 lastN 差值触发 ResetHistory,但全量重置会连带清掉
    // 门状态,与门自身的消化路径重复且有 churn 副作用,已删。
    // Panel preset / internal-resolution / scaling-toggle changes require a
    // feature rebuild; consume before packing.
    if (int newPreset = -1, newRes = -1, newScaling = -1; _shared->ConsumeRebuild(newPreset, newRes, newScaling)) {
        if (!RecreateFeature(newPreset, newRes, newScaling, err, errLen)) {
            // 面板可见:重建失败 = 本会话整体直通,具体原因进 stats。后续帧
            // 顶部 !ready 早退的发布被边缘去重,不会覆盖这条更具体的原因。
            PublishDeadState("passthrough", err);
            return false;
        }
    }
    const DlssnrParams frameParams = _shared->Snapshot();
    // NVOF 档位/会话输入尺寸同步(只重建光流会话,不动 NGX feature)。在
    // AcquireSlot 之前消费:RebuildNvof 要封池排空。失败只降级零 guidance,
    // 帧继续。会话输入尺寸 = follow(开关 + scaling 启用 + FG 未激活)?
    // 内部尺寸 : 源尺寸 —— FG 的 MVecs 契约要求与 backbuffer 同尺寸稠密
    // 运动场,FG 激活时 follow 被强制忽略。ConsumeRebuild 已在上文跑过,
    // 内部尺寸此处是新鲜的。
    const bool fgGateLive = _fg && _fg->Enabled() && frameParams.fgEnabled;
    // 本帧倍数(调用方从参数快照定格 —— 与输出帧数契约绑定;live 变化在
    // 源帧边界生效,由调用方逐帧传入)。0 = FG 未激活。
    const int fgM = fgGateLive && fgDstPlanes && _d3d12->FgSlots()
                        ? std::clamp(fgMultiplier, kFgMultMin, kFgMultMax)
                        : 0;
    const bool nvofFollow = frameParams.nvofFollowScaling != 0 && _d3d12->HasScaling() && !fgGateLive;
    const int nvDstW = nvofFollow ? _d3d12->InternalWidth() : width;
    const int nvDstH = nvofFollow ? _d3d12->InternalHeight() : height;
    if (const int ofq = std::clamp(frameParams.motionVectorQuality, kOfQualityMin, kOfQualityMax);
        ofq != _curOfQuality ||
        (ofq > 0 && _nvof && (_nvof->Width() != nvDstW || _nvof->Height() != nvDstH))) {
        char nvofErr[160]{};
        RebuildNvof(ofq, nvDstW, nvDstH, nvofErr, sizeof(nvofErr));
    }
    const ResidualControls residual{
        std::clamp(frameParams.residualMultiplier, kResidualMultMin, kResidualMultMax),
        std::clamp(frameParams.residualSaturation, kResidualFineMin, kResidualFineMax),
        std::clamp(frameParams.residualLightness, kResidualFineMin, kResidualFineMax),
        std::clamp(frameParams.shadowStructureMultiplier, kResidualFineMin, kResidualFineMax),
        std::clamp(frameParams.reflectionGlowMultiplier, kResidualFineMin, kResidualFineMax),
    };
    // Telemetry is always on (QPC reads cost ~ns); timingOut additionally
    // receives a per-frame segment string for the VS-log channel.
    const bool vsTiming = timingOut && timingLen > 0;
    LARGE_INTEGER qpcFreq{}, t0{}, t1{}, t2{}, t3{}, t4{};
    LARGE_INTEGER tFg0{}, tFg1{};    // FG 段 CPU 墙钟(stats fg_last)
    LARGE_INTEGER tSlot0{}, tSlot1{}; // AcquireSlot 等待(perf 行 slot=)
    LARGE_INTEGER tLock0{}, tLock1{}; // evaluate 互斥等待(perf 行 lock=)
    QueryPerformanceFrequency(&qpcFreq);
    {
        // Frame-cadence tick at the real frame entry (before a possible
        // RecreateFeature wait below would skew the interval).
        LARGE_INTEGER entry{};
        QueryPerformanceCounter(&entry);
        _d3d12->NotifyFrameTick(static_cast<double>(entry.QuadPart) / static_cast<double>(qpcFreq.QuadPart));
    }
    // Diagnostic: VSDLSSNR_SKIP_EVAL=1 measures the pipe without NGX evaluate
    static const bool skipEval = GetEnvironmentVariableA("VSDLSSNR_SKIP_EVAL", nullptr, 0) != 0;
    static const bool dumpEnabled = GetEnvironmentVariableA("VSDLSSNR_DUMP", nullptr, 0) != 0;
    // NR 总开关(live,shared_lock 快照):关 = 跳过 NGX 降噪评估(Input→Output
    // 直拷),NVOF/补帧照常 —— 补帧以直通帧为 backbuffer 继续插值。与诊断
    // 的 skipEval 互不相同:skipEval 连 NVOF/补帧一起跳(测管线底价)。
    const bool nrOff = _shared->Snapshot().nrEnabled == 0;
    // fmParallel: VS feeds several frames concurrently, each on its own slot
    // (command list, staging, textures). The queue serializes the GPU work in
    // submit order, so slots overlap CPU pack/unpack with other slots' GPU
    // time instead of idling it away between frames.
    // 探针:AcquireSlot 等待 = 槽池耗尽时长(3 槽全在飞)。吞吐类问题
    // (启动低帧率/丢帧,#31 类)的第一现场:gpu 段正常而 slot 等待大 =
    // GPU 超容量;两者都小而 fps 低 = 宿主侧问题。
    QueryPerformanceCounter(&tSlot0);
    FrameSlot *slot = _d3d12->AcquireSlot();
    QueryPerformanceCounter(&tSlot1);
    struct SlotGuard {
        D3D12Context *ctx;
        FrameSlot *s;
        ~SlotGuard() { if (s) ctx->ReleaseSlot(s); }
    } guard{ _d3d12, slot };
    // The pool seal only serializes; it does not refresh readiness. A
    // concurrent RecreateFeature may have failed (leaving _parameters null)
    // or a device loss may have latched while this thread waited for a slot
    // — without this re-check the frame would evaluate against a dead/null
    // parameter block and fault the latch.
    if (!_ready.load(std::memory_order_acquire) || !_parameters) {
        if (err && errLen) std::snprintf(err, errLen, "context not ready (rebuild failed or device lost)");
        return false;
    }
    // pack-segment clock starts here: PackInput is a pure-CPU RGBS→RGBA8
    // write into the slot's upload heap. RecreateFeature (above, only on the
    // switch frame: PoolHold drain + NGX rebuild) and AcquireSlot waits are
    // scheduling events, not pack work — counting them made the switch frame
    // report a bogus pack=70-160ms.
    QueryPerformanceCounter(&t0);
    if (!_d3d12->PackInput(*slot, srcPlanes, srcStrides, width, height, err, errLen)) return false;
    QueryPerformanceCounter(&t1);

    // NVOF 光流阶段:帧序门 + 拷贝提交 + execute + densify(独立 nvof CL,
    // 不占槽列表)。densify 经 postExecute 回调在门内、execute 完成后录制
    // 并二次提交 —— 栅栏链(copyFence k_{n+1} > k'_n)封死“下一帧覆写 flow
    // 而本帧 densify 未读”的窗口。publishZero/历史重置语义见 StageFrame。
    bool realMotion = false;      // 本帧有真光流(densify 录制 + PARAM_MVEC 指向它)
    bool nvofHistoryReset = false;
    bool densifyInternal = false; // densify 直写 reducedMotion(follow 内部管线)
    double nvofMs = 0.0;
    int nvofInputIndex = -1;      // 本帧写入的 NVOF 输入 ping-pong 槽位(诊断 dump 用)
    // DLSS FG:本帧各插值槽是否真插值(fgGenOk;false = 复制真实帧:复位/
    // 零光流/面板关/eval 降级)。函数级作用域 —— 真实帧转换与统计段都要
    // 读。fgRan = 本帧向 proxy 提交过 eval(含播种):outputColor 已在
    // NSR,真实帧转换以 NSR 为 stateBefore。
    int fgEvaluatedCount = 0;
    bool fgRan = false;    if (!skipEval && _nvof && _nvof->Enabled() && _curOfQuality > 0) {
        const uint32_t gs = _nvof->GridSize();
        // 流场网格随会话输入尺寸(follow 模式 = 内部尺寸);densify 的
        // MotionScale 把流向量从会话输入像素单位换算回源像素单位。
        const uint32_t nvW = static_cast<uint32_t>(_nvof->Width());
        const uint32_t nvH = static_cast<uint32_t>(_nvof->Height());
        const uint32_t flowW = (nvW + gs - 1) / gs;
        const uint32_t flowH = (nvH + gs - 1) / gs;
        const float msX = nvW != static_cast<uint32_t>(width)
                              ? static_cast<float>(width) / static_cast<float>(nvW) : 1.0f;
        const float msY = nvH != static_cast<uint32_t>(height)
                              ? static_cast<float>(height) / static_cast<float>(nvH) : 1.0f;
        const bool hasBwd = _nvof->Bidirectional();
        const bool hasCost = _nvof->CostEnabled();
        // follow 内部管线(会话输入 = 内部尺寸 ≠ 源):densify 直接产出
        // NGX 缩放消费纹理 reducedMotion/reducedConfidence,跳过“densify
        // 到源尺寸 → guidance 降采样缩回内部”的放大-缩小 pass。流向量保持
        // 会话像素单位 = 内部像素单位(NGX 契约 MVecScale=1),MotionScale
        // 恒 (1,1)。
        densifyInternal =
            nvW != static_cast<uint32_t>(width) || nvH != static_cast<uint32_t>(height);
        NvofContext::PostExecuteFn post =
            [this, &slot, flowW, flowH, gs, hasCost, hasBwd, msX, msY, densifyInternal,
             nvW, nvH, width, height](ID3D12GraphicsCommandList *cl) {
                ID3D12Resource *dstMotion = densifyInternal ? slot->reducedMotion.Get()
                                                            : slot->motion.Get();
                ID3D12Resource *dstConf = densifyInternal ? slot->reducedConfidence.Get()
                                                          : slot->confidence.Get();
                D3D12_RESOURCE_BARRIER g1[2]{
                    Transition(dstMotion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                    Transition(dstConf, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                };
                cl->ResourceBarrier(2, g1);
                _d3d12->RecordDensify(*cl, *slot,
                                      densifyInternal ? nvW : static_cast<uint32_t>(width),
                                      densifyInternal ? nvH : static_cast<uint32_t>(height),
                                      flowW, flowH, gs,
                                      hasCost, hasBwd, hasBwd && hasCost,
                                      densifyInternal ? 1.0f : msX,
                                      densifyInternal ? 1.0f : msY,
                                      densifyInternal ? 20u : 12u,
                                      densifyInternal ? 21u : 13u);
                D3D12_RESOURCE_BARRIER g2[2]{
                    Transition(dstMotion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                    Transition(dstConf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                };
                cl->ResourceBarrier(2, g2);
            };
        // YUV 原生:postCopy 恒设 —— 回调在 nvof CL 第一次提交上记录
        // YUV→RGB 转换(yuvUpload→yuvIn 拷贝 + dispatch → inputColor NSR),
        // follow(densifyInternal)时追加 RecordNvofDownsample 直写注册输入
        // 纹理;非 follow 由 StageFrame 随后做 inputColor→_input[cur] 纹理
        // 拷贝。原 CPU 双线性(~2-4ms@4K)与 WC 内存读坑、WaitCopyIdle 覆写
        // 排空负担一并成为历史;CopyDrainGuard 仍守护 early-return 撕裂
        // (yuvUpload 是 per-slot 资源,nvof CL 在读它)。
        NvofContext::PostCopyFn postCopy =
            [this, &slot, nvW, nvH, densifyInternal, matrix, range](ID3D12GraphicsCommandList *cl, int inputIndex) {
                _d3d12->RecordConvertInput(*cl, *slot, matrix, range,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (densifyInternal) {
                    _d3d12->RecordNvofDownsample(*cl, *slot,
                                                 static_cast<int>(nvW), static_cast<int>(nvH),
                                                 inputIndex);
                }
            };
        const NvofContext::StageResult st =
            _nvof->StageFrame(n, _d3d12->InputColor(*slot), post, postCopy, densifyInternal);
        nvofHistoryReset = st.historyReset;
        nvofMs = _nvof->LastStageMs();
        realMotion = st.waitFenceValue != 0;
        nvofInputIndex = st.inputIndex;
    }
    // 3 连败停用(会话内部闩锁)在帧线程侧只发现不重试,防风暴;
    // Rebind/换档时经 _nvofFailed 条目重试一次。
    if (_curOfQuality > 0 && _nvof && !_nvof->Enabled() && !_nvofFailed) {
        _nvofFailed = true;
        TimingStatusLine("DLSSNR STATUS: nvof disabled (exhausted); zero guidance until rebind");
    }
    // early-return 路径释放槽位前排空在途拷贝(宿主复用 upload 缓冲;
    // 正常路径已被 execute 栅栏覆盖,no-op)。
    struct CopyDrainGuard {
        NvofContext *n;
        bool on;
        ~CopyDrainGuard() { if (n && on) n->WaitCopyIdle(); }
    } drainGuard{ _nvof.get(), _nvof && _curOfQuality > 0 };

    if (ProbeEnabled()) TimingStatusLine("PROBE: post-stage"); // 临时探针(VSDLSSNR_PROBE=1)
    if (!_d3d12->BeginFrameRecording(*slot)) {
        if (err && errLen) std::snprintf(err, errLen, "BeginFrameRecording(frame) failed");
        return false;
    }
    // NOTE: D3D12 timestamp queries crash NGX snippet evaluate (SEH) when on the same command list - do not re-enable (verified 2026-09-05)

    const D3D12_RESOURCE_STATES uploadPost = (skipEval || nrOff)
                                                 ? D3D12_RESOURCE_STATE_COMMON
                                                 : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // Upload/convert: NVOF active 时转换已在 nvof CL 完成(inputIndex>=0);
    // 其余路径(OF 关/skipEval/nvofFailed/迟到帧)在槽 CL 补做,落点语义
    // 与旧 RecordUploadCopy 相同——NSR 供 evaluate,COMMON 供诊断拷贝。
    // The conversion lands directly in the next consumer's state: NSR for
    // the evaluate paths, COMMON for the diagnostic copy — no COMMON detour.
    const bool convertedOnNvof = nvofInputIndex >= 0; // 迟到帧/失败帧 = -1(见 nvof_context.h 迟到帧契约)
    if (!convertedOnNvof) {
        _d3d12->RecordConvertInput(*slot->commandList.Get(), *slot, matrix, range, uploadPost);
    }

    // 槽 CL 与输出状态契约:直通拷贝(skipEval/nrOff)与 eval 路径帧末都把
    // OutputColor 停在 UAV —— 下方 FG 段的 UAV→NSR 屏障与真实帧转换的
    // stateBefore 在三条路径(skipEval / nrOff / eval)上完全一致。
    auto *cl = slot->commandList.Get();
    const bool scaling = _d3d12->HasScaling();
    // guidance 降采样是否本帧执行(eval + 缩放 + 源尺寸运动场);NR 关时
    // 跳过(eval 不消费运动场降采样),reduced 纹理保持 COMMON,帧末归位
    // 相应跳过。
    const bool guidanceDown = realMotion && scaling && !densifyInternal && !nrOff;
    if (skipEval || nrOff) {
        // 直通拷贝:Input → Output(不经 NGX 降噪)。skipEval = 诊断(测管线
        // 底价,NVOF/补帧整体跳过);nrOff = 面板关降噪(NVOF/补帧照常,
        // 补帧以直通帧为 backbuffer)。输入状态:nvof CL 转换落 NSR
        // (convertedOnNvof)/ 槽 CL 补转换落 COMMON,拷贝后统一归 COMMON。
        D3D12_RESOURCE_BARRIER bar[2]{
            Transition(_d3d12->InputColor(*slot),
                       convertedOnNvof ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                       : D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            TransitionTo(_d3d12->OutputColor(*slot), D3D12_RESOURCE_STATE_COPY_DEST),
        };
        cl->ResourceBarrier(2, bar);
        cl->CopyResource(_d3d12->OutputColor(*slot), _d3d12->InputColor(*slot));
        D3D12_RESOURCE_BARRIER back[2]{
            Transition(_d3d12->InputColor(*slot),
                       D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
            Transition(_d3d12->OutputColor(*slot),
                       D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        cl->ResourceBarrier(2, back);
        // t2 与正常路径 eval 后同位:此分支不赋值的话 gpuWaitMs = ms(t2=0, t3)
        // = 开机以来毫秒,面板/perf 日志的 gpu 段被巨值吞没。
        QueryPerformanceCounter(&t2);
    } else {

        // ---- NVOF guidance 段(PORTING #6)----
        // densify/清零已挪进 NVOF 会话的 nvof CL(门内、execute 完成后,
        // 经 postExecute 回调录制)—— 本槽列表只剩缩放启用时的 guidance
        // 降采样(读 nvofCL 转入 NSR 的 motion/confidence,同队列 FIFO
        // 保序)。OF 关闭/跳过时 motion/confidence 全程 COMMON 不动。
        // follow 内部管线 densify 已直写 reducedMotion(NSR),这里整个
        // 跳过;motion/confidence(源尺寸)本帧全程 COMMON 不被触碰。
        if (guidanceDown) {
            // 缩放启用:置信度加权降采样到内部尺寸(运动向量乘
            // MotionScale 换算到内部像素单位,Magpie 同款)。
            D3D12_RESOURCE_BARRIER g3[2]{
                Transition(slot->reducedMotion.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                Transition(slot->reducedConfidence.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            cl->ResourceBarrier(2, g3);
            _d3d12->RecordGuidanceDownsample(*slot);
            D3D12_RESOURCE_BARRIER g4[2]{
                Transition(slot->reducedMotion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                Transition(slot->reducedConfidence.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            };
            cl->ResourceBarrier(2, g4);
        }

        // Pre-evaluate barriers (executed on the GPU before the NGX dispatch).
        // Zero-guidance motion/depth are resident NON_PIXEL_SHADER_RESOURCE;
        // the NVOF guidance textures were transitioned to NSR above.
        D3D12_RESOURCE_BARRIER pre[1];
        UINT preCount = 0;
        if (scaling) {
            // Two-pass Lanczos2 color downsample (upstream 1cde1bae). At equal
            // extents Lanczos2 degenerates to an exact copy, so no skip needed.
            // horizontalRes doubles as the downsample intermediate (upstream
            // reuses resampleIntermediate the same way).
            D3D12_RESOURCE_BARRIER b1[2]{
                TransitionTo(_d3d12->ReducedColor(*slot), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                TransitionTo(_d3d12->HorizontalRes(*slot), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            cl->ResourceBarrier(2, b1);
            _d3d12->RecordDownsampleVertical(*slot, residual);
            // vertical output becomes the horizontal pass's SRV only after the
            // UAV->SRV transition
            D3D12_RESOURCE_BARRIER b1b[1]{
                TransitionFromTo(_d3d12->HorizontalRes(*slot), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            };
            cl->ResourceBarrier(1, b1b);
            _d3d12->RecordDownsampleHorizontal(*slot, residual);
            // reducedColor: UAV (downsample write) -> NSR (NGX input)
            D3D12_RESOURCE_BARRIER b2[1]{
                TransitionFromTo(_d3d12->ReducedColor(*slot), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            };
            cl->ResourceBarrier(1, b2);
            // NGX evaluate: color=ReducedColor (NSR), output=ReducedDenoised (UAV)
            pre[0] = TransitionTo(_d3d12->ReducedDenoised(*slot), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            preCount = 1;
        } else {
            // NGX evaluate at full source size: color=InputColor (already NSR
            // from the upload copy), output=OutputColor
            pre[0] = TransitionTo(_d3d12->OutputColor(*slot), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            preCount = 1;
        }
        cl->ResourceBarrier(preCount, pre);

        // The feature + parameter block are singletons: serialize the CPU-side
        // evaluate across concurrent frame threads. GPU dispatches recorded on
        // this slot's list still run overlapped with other slots' work.
        if (ProbeEnabled()) TimingStatusLine("PROBE: pre-eval-lock"); // 临时探针(VSDLSSNR_PROBE=1)
        // 探针:evaluate 互斥等待。eval_cpu 段包含它(无法从分段里拆出),
        // 这里单独测量:"NGX CPU 变慢"与"被别的槽的 evaluate 排队"由此分家。
        QueryPerformanceCounter(&tLock0);
        std::lock_guard<std::mutex> evalLock(_evaluateMutex);
        QueryPerformanceCounter(&tLock1);
        if (ProbeEnabled()) TimingStatusLine("PROBE: eval-locked"); // 临时探针(VSDLSSNR_PROBE=1)
        DWORD sehCode = 0;
        // NGX PARAM_RESET 只由帧序门的播种帧携带(大跳/回退/缺口的恢复路径)。
        if (!SetEvaluateParametersSafely(*slot, nvofHistoryReset, realMotion, &sehCode)) {
            if (err && errLen) {
                if (NgxRuntimeGuard::IsFaulted() && !sehCode) {
                    std::snprintf(err, errLen,
                                  "NGX faulted (SEH 0x%x); SDK disabled until host restart",
                                  NgxRuntimeGuard::FaultCode());
                } else {
                    std::snprintf(err, errLen, "Evaluate parameter setup raised SEH");
                }
                TimingStatusLine(err); // 临时探针
            }
            return false;
        }
        const NVSDK_NGX_Result r = SnippetEvaluateSafely(cl, _parameters, &sehCode);
        QueryPerformanceCounter(&t2);
        if (sehCode) {
            char buf[160];
            snprintf(buf, sizeof(buf), "EvaluateFeature raised SEH 0x%x (scaling=%d); NGX latched, no further SDK entry",
                     sehCode, scaling ? 1 : 0);
            DbgLine(buf);
            TimingStatusLine(buf); // 临时探针
            if (err && errLen) snprintf(err, errLen, "EvaluateFeature raised SEH 0x%x; NGX disabled until host restart", sehCode);
            return false;
        }
        if (ProbeEnabled()) TimingStatusLine("PROBE: eval-ok"); // 临时探针(VSDLSSNR_PROBE=1)
        if (!NVSDK_NGX_SUCCEED(r)) {
            if (err && errLen) {
                if (NgxRuntimeGuard::IsFaulted()) {
                    std::snprintf(err, errLen,
                                  "NGX faulted (SEH 0x%x); SDK disabled until host restart",
                                  NgxRuntimeGuard::FaultCode());
                } else {
                    snprintf(err, errLen, "EvaluateFeature failed (0x%x)", static_cast<unsigned>(r));
                }
                TimingStatusLine(err); // 临时探针
            }
            return false;
        }

        if (scaling) {
            // reducedDenoised: UAV (NGX write) -> NSR (prepare read)
            D3D12_RESOURCE_BARRIER b4[1]{
                TransitionFromTo(_d3d12->ReducedDenoised(*slot), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            };
            cl->ResourceBarrier(1, b4);
            // PrepareResidual: fine controls per internal-resolution pixel
            // (Magpie 2d37f8c0); reducedColor is still NSR from the NGX input
            D3D12_RESOURCE_BARRIER b5[1]{
                TransitionTo(_d3d12->ControlledRes(*slot), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            cl->ResourceBarrier(1, b5);
            _d3d12->RecordResidualPrepare(*slot, residual);
            D3D12_RESOURCE_BARRIER b6[1]{
                TransitionFromTo(_d3d12->ControlledRes(*slot), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            };
            cl->ResourceBarrier(1, b6);
            // Catmull-Rom horizontal residual upsample; equal internal width
            // skips it and the vertical pass reads controlledRes directly.
            // horizontalRes is in NSR here (downsample intermediate), so the
            // re-use as UAV target needs an explicit NSR->UAV transition.
            const bool equalWidth = _d3d12->InternalWidth() == width;
            if (!equalWidth) {
                D3D12_RESOURCE_BARRIER b7[1]{
                    TransitionFromTo(_d3d12->HorizontalRes(*slot), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                };
                cl->ResourceBarrier(1, b7);
                _d3d12->RecordResidualHorizontal(*slot, residual);
                D3D12_RESOURCE_BARRIER b8[1]{
                    TransitionFromTo(_d3d12->HorizontalRes(*slot), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                };
                cl->ResourceBarrier(1, b8);
            }
            // prepare is done with the reduced textures; park them back in COMMON
            D3D12_RESOURCE_BARRIER b9[2]{
                TransitionFromTo(_d3d12->ReducedColor(*slot), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                TransitionFromTo(_d3d12->ReducedDenoised(*slot), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            };
            cl->ResourceBarrier(2, b9);
            // vertical composite onto full-size output (input = original color)
            D3D12_RESOURCE_BARRIER b10[1]{
                TransitionTo(_d3d12->OutputColor(*slot), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            cl->ResourceBarrier(1, b10);
            // move inputs back to COMMON only after the vertical dispatch
            // consumed them; OutputColor stays UAV — the readback copy takes
            // it from there. horizontalRes always left COMMON this frame
            // (downsample intermediate), so it always transitions back.
            _d3d12->RecordResidualVertical(*slot, residual, equalWidth);
            D3D12_RESOURCE_BARRIER b11[3]{
                TransitionFromTo(_d3d12->InputColor(*slot), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                TransitionFromTo(_d3d12->ControlledRes(*slot), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                TransitionFromTo(_d3d12->HorizontalRes(*slot), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            };
            cl->ResourceBarrier(3, b11);
        } else {
            // no scaling: NGX wrote OutputColor directly. Input returns to
            // COMMON here; OutputColor stays UAV — the readback copy takes
            // it from there.
            D3D12_RESOURCE_BARRIER back[1]{
                TransitionFromTo(_d3d12->InputColor(*slot), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            };
            cl->ResourceBarrier(1, back);
        }
    }

    // ---- DLSS FG 段(挂 NR 之后;eval 与直通拷贝两路汇合):backbuffer =
    // NR 输出(outputColor;NR 关 = 直通帧),MVecs = 源尺寸稠密运动
    // (slot.motion —— FG 激活时 follow 已被忽略,会话必为源尺寸),Depth =
    // 静态零纹理。proxy 契约:输入 NSR / 输出 UAV;CUDA 互操作工作与槽
    // 队列 FIFO 保序,插值输出就绪由本槽 WaitFrame 覆盖(与 NVOF densify
    // 同款论证)。倍数 M:同一条 CL 上按序 eval 插值槽 1..M-1(proxy 契约
    // "MFG indices must be evaluated in order starting at 1"),每槽紧随
    // RGB→YUV 转换 + 独立回读(fgInterp 单纹理逐槽复用 —— 转换/回读后
    // yuvOut 归位、fgInterp 归 COMMON,下一槽重新起步)。播种帧(会话首帧
    // /seek 后)只提交首个 eval(带 DLSSG.Reset=1,只建历史,输出不消费);
    // 零光流帧整块跳过 —— 无运动信息可插,插值槽一律真实帧复制。
    // fgRan = 本帧向 proxy 提交过 eval(含播种):outputColor 已被置 NSR,
    // 真实帧转换以 NSR 为 stateBefore。
    QueryPerformanceCounter(&tFg0);
    if (fgM > 0 && !skipEval && realMotion && !nvofHistoryReset) {
        const bool fgResetEval = _fg->NeedsReset();
        D3D12_RESOURCE_BARRIER fgBar[1]{
            TransitionFromTo(_d3d12->OutputColor(*slot),
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        cl->ResourceBarrier(1, fgBar);
        char fgErr[160]{};
        for (int g = 0; g < fgM - 1; ++g) {
            if (fgResetEval && g > 0) break; // 播种帧只建历史,不产插值
            D3D12_RESOURCE_BARRIER toUav[1]{
                Transition(slot->fgInterp.Get(),
                           D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            cl->ResourceBarrier(1, toUav);
            if (_fg->Evaluate(cl, _d3d12->OutputColor(*slot), slot->motion.Get(),
                              _d3d12->Depth(), slot->fgInterp.Get(), width, height,
                              fgM, g + 1, false, fgErr, sizeof(fgErr))) {
                fgRan = true;
                if (fgResetEval) {
                    // 播种帧:插值输出不消费,fgInterp 归 COMMON。
                    D3D12_RESOURCE_BARRIER fgSeed[1]{
                        Transition(slot->fgInterp.Get(),
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COMMON),
                    };
                    cl->ResourceBarrier(1, fgSeed);
                    break;
                }
                // 有效插值:fgInterp UAV→NSR 进转换,转换收尾归 COMMON;
                // 回读落第 g 组缓冲(各槽独立,真实帧回读不被覆写)。
                D3D12_RESOURCE_BARRIER toNsr[1]{
                    Transition(slot->fgInterp.Get(),
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                };
                cl->ResourceBarrier(1, toNsr);
                _d3d12->RecordYuvOutput(*slot, matrix, range,
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                        slot->fgInterp.Get(), D3D12Context::kSrvFgInterp);
                if (!_d3d12->RecordReadbackCopy(*slot, err, errLen, g)) {
                    return false;
                }
                if (fgGenOk) fgGenOk[g] = true;
                ++fgEvaluatedCount;
            } else {
                // eval 失败(会话已被闩停):fgInterp 回 COMMON,本帧全部
                // 插值槽降级复制;outputColor 按是否提交过 eval 回补屏障。
                D3D12_RESOURCE_BARRIER undo[1]{
                    Transition(slot->fgInterp.Get(),
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COMMON),
                };
                cl->ResourceBarrier(1, undo);
                // 失败必须进 timing log(dlssfg 内部已留痕;此处补帧号锚点)。
                char msg[224];
                std::snprintf(msg, sizeof(msg),
                              "DLSSNR STATUS: dlssfg frame %d slot %d degraded to dup: %.140s",
                              n, g + 1, fgErr);
                TimingStatusLine(msg);
                break;
            }
        }
        if (!fgRan) {
            // 全部槽失败(首次 eval 即败):outputColor 回补 UAV,恢复
            // 帧末不变量(fgRan=false 时真实帧转换以 UAV 为 stateBefore)。
            D3D12_RESOURCE_BARRIER fgUndo[1]{
                TransitionFromTo(_d3d12->OutputColor(*slot),
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            cl->ResourceBarrier(1, fgUndo);
        }
    } else if (_fg && _fg->Enabled()) {
        // 门关帧(面板关/诊断跳过/播种/零光流)不向 proxy 提交 eval:
        // 其内部 backbuffer 历史滞留在跳过前 —— 置位重置让恢复帧重新
        // 播种(播种帧插值输出照常降级复制),否则"关→开"/播种后的
        // 首个 eval 用陈旧历史插出鬼影。逐帧置位无害(布尔位;会话
        // 已死时 Enabled() 为 false 不进此分支)。
        _fg->ResetHistory();
    }
    QueryPerformanceCounter(&tFg1);
    // guidance 纹理归位 COMMON(仅 realMotion 帧动过:播种/失败帧的
    // 发布走静态零纹理,per-slot motion 全程 COMMON 不被触碰)。
    // follow 内部管线只动过 reducedMotion/reducedConfidence —— 源尺寸
    // 对全程 COMMON,对它做 NSR→COMMON 是非法屏障。
    if (realMotion) {
        if (densifyInternal) {
            D3D12_RESOURCE_BARRIER gBackR[2]{
                TransitionFromTo(slot->reducedMotion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                TransitionFromTo(slot->reducedConfidence.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            };
            cl->ResourceBarrier(2, gBackR);
        } else {
            D3D12_RESOURCE_BARRIER gBack[2]{
                TransitionFromTo(slot->motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                TransitionFromTo(slot->confidence.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            };
            cl->ResourceBarrier(2, gBack);
            if (guidanceDown) {
                D3D12_RESOURCE_BARRIER gBackR[2]{
                    TransitionFromTo(slot->reducedMotion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                    TransitionFromTo(slot->reducedConfidence.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                };
                cl->ResourceBarrier(2, gBackR);
            }
        }
    }

    // RGB→YUV(三路汇合处):outputColor 到达时 = UAV(evaluate/垂直合成
    // 写 / 直通拷贝写)/ NSR(FG eval 已消费 backbuffer),RecordYuvOutput
    // 统一 NSR 化后转换、收尾归 COMMON;yuvOut 留 UAV 交 readback。
    _d3d12->RecordYuvOutput(*slot, matrix, range,
                            fgRan ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                  : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!_d3d12->RecordReadbackCopy(*slot, err, errLen)) {
        return false;
    }
    // NOTE: timestamp disabled, see note above
    // NVOF flow 已在 StageFrame 里 CPU 等待完成(execute 后的输出栅栏),
    // 槽 CL 提交时 GPU 侧 flow 已就绪 —— 不再需要队列级 Wait(实测该栅栏
    // 在队列 Wait 语义下可能永不满足,见 NvofContext::StageFrame 注释)。
    if (!_d3d12->SubmitFrame(*slot, nullptr, 0, err, errLen)) {
        if (err && errLen) {
            TimingStatusLine(err); // 临时探针
            std::snprintf(err, errLen, "Submit(frame) failed");
        }
        return false;
    }
    if (ProbeEnabled()) TimingStatusLine("PROBE: submitted"); // 临时探针(VSDLSSNR_PROBE=1)
    if (!_d3d12->WaitFrame(*slot, err, errLen)) {
        // A timed-out fence means GPU hang / device removal: stop evaluating,
        // otherwise every later frame stalls the full 10s fence wait again.
        if (_d3d12->IsDeviceLost()) _ready.store(false, std::memory_order_release);
        if (err && errLen) {
            TimingStatusLine(err); // 临时探针
            std::snprintf(err, errLen, "Wait(frame) failed");
        }
        return false;
    }
    if (ProbeEnabled()) TimingStatusLine("PROBE: waited"); // 临时探针(VSDLSSNR_PROBE=1)
    QueryPerformanceCounter(&t3);
    const bool rb = _d3d12->UnpackOutput(*slot, dstPlanes, dstStrides, width, height, err, errLen);
    // FG 插值帧回读(各组独立缓冲;eval 成功的槽才有内容)。失败按整体
    // 失败处理:调用方会对全部输出做源帧复制降级。
    if (rb && fgDstPlanes) {
        for (int g = 0; g < fgM - 1; ++g) {
            if (fgGenOk && fgGenOk[g] &&
                !_d3d12->UnpackOutput(*slot, &fgDstPlanes[g * 3], &fgDstStrides[g * 3],
                                      width, height, err, errLen, g)) {
                return false;
            }
        }
    }
    {
        // concurrent frame threads: dump at most once, no interleaved writes;
        // the mutex is only taken when a dump is actually pending (the old
        // code locked it every frame even with dumping disabled).
        // 颜色 dump 与 NVOF motion/flow dump 分开锁存:首帧必然是播种帧
        // (publishZero,无真流),motion dump 要等到第一个 densify 帧。
        static std::atomic<bool> dumped{ false };
        static std::atomic<bool> dumpedMotion{ false };
        if (ProbeEnabled()) { // 临时探针
            char probe[96];
            std::snprintf(probe, sizeof(probe), "PROBE: dump-site realMotion=%d ofQ=%d",
                          realMotion ? 1 : 0, _curOfQuality);
            TimingStatusLine(probe);
        }
        if (dumpEnabled && realMotion && !dumpedMotion.load(std::memory_order_relaxed)) {
            static std::mutex dumpMotionMutex;
            std::lock_guard<std::mutex> dumpLock(dumpMotionMutex);
            if (!dumpedMotion.exchange(true)) {
                TimingStatusLine("DLSSNR STATUS: motion latch fired"); // 临时探针
                wchar_t dir[MAX_PATH];
                if (GetModuleFileNameW(nullptr, dir, MAX_PATH)) {
                    std::filesystem::path base = std::filesystem::path(dir).parent_path();
                    const bool scaling = _d3d12->HasScaling();
                    std::lock_guard<std::mutex> ctlLock(_d3d12->CtlMutex());
                    // NGX 实际消费的运动场(缩放时为降采样版,R16G16_FLOAT)+
                    // 原始网格流(S10.5,R16G16_SINT)。
                    bool motionDumped = false;
                    if (scaling) {
                        motionDumped = _d3d12->DumpTextureToFile(
                            slot->reducedMotion.Get(), _d3d12->InternalWidth(),
                            _d3d12->InternalHeight(), (base / L"dump_motion.bin").c_str(),
                            DXGI_FORMAT_R16G16_FLOAT);
                    } else {
                        motionDumped = _d3d12->DumpTextureToFile(
                            slot->motion.Get(), width, height,
                            (base / L"dump_motion.bin").c_str(),
                            DXGI_FORMAT_R16G16_FLOAT);
                    }
                    if (!motionDumped) TimingStatusLine("DLSSNR STATUS: motion dump FAILED");
                    else TimingStatusLine("DLSSNR STATUS: motion dump OK"); // 临时探针
                    if (_nvof && _nvof->FlowForward()) {
                        const uint32_t gs = _nvof->GridSize();
                        // flow 网格按会话输入尺寸(follow 模式 = 内部尺寸,
                        // 与 StageFrame 调用点的 flowW/H 同款公式),不是源
                        // 尺寸 —— 曾按源宽算 dump 维度,与纹理不符。
                        const uint32_t nw = static_cast<uint32_t>(_nvof->Width());
                        const uint32_t nh = static_cast<uint32_t>(_nvof->Height());
                        const uint32_t fw = (nw + gs - 1) / gs;
                        const uint32_t fh = (nh + gs - 1) / gs;
                        const bool flowOk = _d3d12->DumpTextureToFile(
                            _nvof->FlowForward(), static_cast<int>(fw),
                            static_cast<int>(fh), (base / L"dump_flow.bin").c_str(),
                            DXGI_FORMAT_R16G16_SINT);
                        if (!flowOk) TimingStatusLine("DLSSNR STATUS: flow dump FAILED");
                    }
                }
            }
        }
        if (dumpEnabled && !dumped.load(std::memory_order_relaxed)) {
            static std::mutex dumpMutex;
            std::lock_guard<std::mutex> dumpLock(dumpMutex);
            if (!dumped.exchange(true)) {
                wchar_t dir[MAX_PATH];
                if (GetModuleFileNameW(nullptr, dir, MAX_PATH)) {
                    std::filesystem::path base = std::filesystem::path(dir).parent_path();
                    const bool scaling = _d3d12->HasScaling();
                    std::lock_guard<std::mutex> ctlLock(_d3d12->CtlMutex());
                    // 管线色格式为 B8G8R8A8(BGRA 切换):CopyTextureRegion 的
                    // footprint 格式必须与源一致,跨格式会 E_INVALIDARG
                    // (memory #30 同族坑)。颜色 dump 统一显式传 BGRA8。
                    constexpr DXGI_FORMAT kColorDump = DXGI_FORMAT_B8G8R8A8_UNORM;
                    // 探针:dump 失败逐个留痕(footprint/格式不匹配时
                    // CopyTextureRegion 静默 E_INVALIDARG,#41-④ 同族坑;
                    // "dump 文件缺失/尺寸不对"从这行直接定位)。
                    auto dumpOrLog = [&](ID3D12Resource *tex, int w, int h,
                                         const wchar_t *name, DXGI_FORMAT fmt) {
                        if (!_d3d12->DumpTextureToFile(tex, w, h, (base / name).c_str(), fmt)) {
                            char msg[160];
                            std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: dump %ls FAILED", name);
                            TimingStatusLine(msg);
                        }
                    };
                    dumpOrLog(_d3d12->InputColor(*slot), width, height, L"dump_input.bin", kColorDump);
                    // YUV 输入平面(YUV→RGB 转换验收:python 参考按矩阵/范围
                    // 重算 BGRA8 与 dump_input 对比 ≤1-2 LSB)。
                    {
                        const DXGI_FORMAT yuvDumpIn = _d3d12->BitDepth() > 8
                                                          ? DXGI_FORMAT_R16_UNORM
                                                          : DXGI_FORMAT_R8_UNORM;
                        const int cw = (width + 1) >> 1, ch = (height + 1) >> 1;
                        const int pw[3]{ width, cw, cw };
                        const int ph[3]{ height, ch, ch };
                        const wchar_t *names[3]{ L"dump_yuvin_y.bin", L"dump_yuvin_u.bin", L"dump_yuvin_v.bin" };
                        for (int i = 0; i < 3; ++i) {
                            dumpOrLog(_d3d12->YuvInPlane(*slot, i), pw[i], ph[i], names[i], yuvDumpIn);
                        }
                    }
                    // GPU 光流输入降采样结果(注册输入纹理,会话尺寸):
                    // 数值验证用 —— python 参考脚本从 dump_input.bin 重算
                    // 双线性,断言 ≤1 LSB(#46 改 GPU 的验收)。
                    if (_nvof && nvofInputIndex >= 0) {
                        dumpOrLog(_nvof->InputTexture(nvofInputIndex),
                                  _nvof->Width(), _nvof->Height(),
                                  L"dump_nvof_input.bin", kColorDump);
                    }
                    if (scaling) {
                        dumpOrLog(_d3d12->ReducedColor(*slot), _d3d12->InternalWidth(),
                                  _d3d12->InternalHeight(), L"dump_reduced_color.bin", kColorDump);
                        dumpOrLog(_d3d12->ReducedDenoised(*slot), _d3d12->InternalWidth(),
                                  _d3d12->InternalHeight(), L"dump_reduced_denoised.bin", kColorDump);
                        dumpOrLog(_d3d12->HorizontalRes(*slot), width,
                                  _d3d12->InternalHeight(), L"dump_horizontal.bin",
                                  DXGI_FORMAT_R16G16B16A16_FLOAT);
                    }
                    dumpOrLog(_d3d12->OutputColor(*slot), width, height, L"dump_output.bin", kColorDump);
                    // FG 插值输出(仅 eval 过的帧有内容;首帧必为播种,等
                    // 第一个真插值帧才有意义 —— 与 motion dump 同款锁存)。
                    if (fgRan) {
                        dumpOrLog(_d3d12->FgInterp(*slot), width, height,
                                  L"dump_fg_interp.bin", kColorDump);
                    }
                    // YUV 输出平面(RGB→YUV 转换验收:python 参考 script 重算
                    // Y/U/V 与 dump 对比,≤1-2 LSB;P10 = 右对齐 word)。
                    {
                        const DXGI_FORMAT yuvDump = _d3d12->BitDepth() > 8
                                                        ? DXGI_FORMAT_R16_UNORM
                                                        : DXGI_FORMAT_R8_UNORM;
                        const int cw = (width + 1) >> 1, ch = (height + 1) >> 1;
                        const int pw[3]{ width, cw, cw };
                        const int ph[3]{ height, ch, ch };
                        const wchar_t *names[3]{ L"dump_y_plane.bin", L"dump_u_plane.bin", L"dump_v_plane.bin" };
                        for (int i = 0; i < 3; ++i) {
                            dumpOrLog(_d3d12->YuvOutPlane(*slot, i), pw[i], ph[i], names[i], yuvDump);
                        }
                    }
                }
            }
        }
    }
    {
        QueryPerformanceCounter(&t4);
        const auto ms = [](LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER f) {
            return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
        };
        const double packMs = ms(t0, t1, qpcFreq);
        // eval_cpu 的原始窗口包含 NVOF 阶段(t1→t2);nvof 单独上报,这里
        // 扣除保持各段可加。
        const double evalCpuMs = ms(t1, t2, qpcFreq);
        const double evalOnlyMs = evalCpuMs > nvofMs ? evalCpuMs - nvofMs : 0.0;
        // Pure-GPU timing is unavailable (timestamp query + NGX evaluate = SEH,
        // see NOTE above); the submit+wait wall clock stands in for it.
        const double gpuWaitMs = ms(t2, t3, qpcFreq);
        const double unpackMs = ms(t3, t4, qpcFreq);
        const double fgMs = ms(tFg0, tFg1, qpcFreq);
        // Magpie-style perf log line into dlssnr_timing.log (time-gated ≥1s,
        // see perfDue below). TimingLog takes g_timingMutex itself — format
        // the line under the lock, log outside of it, or this thread
        // self-deadlocks on frame 1 and burns one slot forever.
        char line[384] = "";
        // 五段 last(每帧值,面板"处理用时"数据源)+ EMA(perf 日志行专用)。
        // pack/nvof/eval_cpu/unpack 的 last 就是本帧裸值(与日志对齐);gpuLast
        // 取窗口内最近样本。EMA 同锁内算,fmParallel 并发安全。
        double gpuLast = 0.0, gpuEma = 0.0, packEma = 0.0, nvofEma = 0.0,
               evalCpuEma = 0.0, unpackEma = 0.0;
        const double packLast = packMs, nvofLast = nvofMs,
                     evalCpuLast = evalOnlyMs, unpackLast = unpackMs,
                     fgLast = fgMs;
        {
            std::lock_guard<std::mutex> timingLock(g_timingMutex);
            const double slotWaitMs = ms(tSlot0, tSlot1, qpcFreq);
            const double lockWaitMs = ms(tLock0, tLock1, qpcFreq);
            g_timing.Push(gpuWaitMs, packMs, nvofMs, evalOnlyMs, unpackMs, slotWaitMs, lockWaitMs);
            const int lastIdx = g_timing.idx - 1 < 0 ? g_timing.count - 1 : g_timing.idx - 1;
            gpuLast = g_timing.gpu[lastIdx];
            gpuEma = TimingWindow::Ema(g_timing.gpu, g_timing.count);
            packEma = TimingWindow::Ema(g_timing.pack, g_timing.count);
            nvofEma = TimingWindow::Ema(g_timing.nvof, g_timing.count);
            evalCpuEma = TimingWindow::Ema(g_timing.evalCpu, g_timing.count);
            unpackEma = TimingWindow::Ema(g_timing.unpack, g_timing.count);
            // perf 行按时间门(≥1s 一行)而非帧数:诊断日志的样本密度不应
            // 随源帧率漂(60fps=1s 一行而 30fps=2s 一行)。静态量在
            // g_timingMutex 内读写,fmParallel 并发安全;首帧立即落一行。
            static LARGE_INTEGER lastPerfQpc{};
            LARGE_INTEGER nowQpc{};
            QueryPerformanceCounter(&nowQpc);
            const bool perfDue = lastPerfQpc.QuadPart == 0 ||
                                 (nowQpc.QuadPart - lastPerfQpc.QuadPart) >= qpcFreq.QuadPart;
            if (perfDue) lastPerfQpc = nowQpc;
            if (perfDue) {
                const double gpuP99 = TimingWindow::P99(g_timing.gpu, g_timing.count);
                const double slotEma = TimingWindow::Ema(g_timing.slotW, g_timing.count);
                const double lockEma = TimingWindow::Ema(g_timing.lockW, g_timing.count);
                snprintf(line, sizeof(line),
                         "DLSSNR perf: gpu=%.1f ema=%.1f p99=%.1f | pack=%.1f nvof=%.1f/%.1f g%.1f c%.1f e%.1f s%u x%u r%u | eval_cpu=%.1f unpack=%.1f | slot=%.1f/%.1f lock=%.1f/%.1f | res=%d%% of=%d %dx%d f=%d fps=%.0f",
                         gpuLast, gpuEma, gpuP99, packEma, nvofEma, g_timing.nvof[lastIdx],
                         _nvof ? _nvof->LastGateWaitMs() : 0.0,
                         _nvof ? _nvof->LastCpyWaitMs() : 0.0,
                         _nvof ? _nvof->LastExeWaitMs() : 0.0,
                         _nvof ? _nvof->GateSkips() : 0u,
                         _nvof ? _nvof->GateExpired() : 0u,
                         _nvof ? _nvof->ResetCount() : 0u,
                         evalCpuEma, unpackEma,
                         slotEma, g_timing.slotW[lastIdx],
                         lockEma, g_timing.lockW[lastIdx],
                         std::clamp(_shared->Snapshot().inputResolutionPercent, kResPctMin, kResPctMax),
                         _curOfQuality,
                         _width, _height,
                         _lastFrameN.load(std::memory_order_relaxed),
                         _d3d12->FrameRateWindow());
                // nvof=ema/last;后缀 g/c/e = 门等待/前帧拷贝等待/引擎输出等待,
                // s/x/r = 门跳帧/过期帧/历史重置累计(探针保留:时序类问题的
                // 第一手证据)。slot/lock = 槽池等待 / evaluate 互斥等待
                // (ema/last);f = 本行前一帧的帧号(与 STATUS 行对齐用);
                // fps = 1s 窗口帧入口计数,处理帧率 < 源帧率 = 宿主侧没来帧。
            }
        }
        // stats 每帧发布(六段 last + 512B 共享内存写,开销可忽略):面板
        // "处理用时"随帧呼吸,不再按日志节流跳变。perf 行(磁盘 IO)按时间
        // 门 ≥1s 一行(与帧率无关)。Snapshot/FrameRateWindow 在锁外取
        // (各自持独立互斥,勿在 g_timingMutex 内叠锁)。
        {
            char body[512];
            // FG 状态:on = 本帧有真插值(附当前倍数);dup = 复制真实帧
            // (复位/零光流/面板关/降级);off = 本会话未激活;unavailable =
            // proxy 初始化或 eval 失败闩停(帧率仍 ×M,内容为复制帧)。
            // fg_mult = 当前倍数(面板显示 "3x" 用;未激活 = 0)。
            const char *fgState = !_fg ? "off"
                : (!_fg->Enabled() ? "unavailable"
                                   : (fgEvaluatedCount > 0 ? "on" : "dup"));
            snprintf(body, sizeof(body),
                     "{\"%s\":%.1f,\"%s\":%.1f,"
                     "\"%s\":%.1f,\"%s\":%.1f,\"%s\":%.1f,\"%s\":%.1f,"
                     "\"%s\":%d,\"%s\":%d,\"%s\":%d,\"%s\":%d,"
                     "\"%s\":%d,\"%s\":%.1f,\"%s\":\"%s\","
                     "\"%s\":\"%s\",\"%s\":\"%s\",\"%s\":\"%s\",\"%s\":%d}",
                     SK_GPU_LAST, gpuLast, SK_PACK_LAST, packLast,
                     SK_EVAL_CPU_LAST, evalCpuLast, SK_UNPACK_LAST, unpackLast,
                     SK_NVOF_LAST, nvofLast, SK_FG_LAST, fgLast,
                     SK_INTERNAL_W, _d3d12->InternalWidth(), SK_INTERNAL_H, _d3d12->InternalHeight(),
                     SK_WIDTH, _width, SK_HEIGHT, _height,
                     SK_SCALING, _d3d12->HasScaling() ? 1 : 0,
                     SK_FPS, _d3d12->FrameRateWindow(), SK_GPU_NAME, _gpuNameUtf8,
                     SK_FILTER_STATE, (_nvofFailed && _curOfQuality > 0) ? "nvof_zero" : "ok",
                     SK_OF_MODE, OfModeString(),
                     SK_FG, fgState,
                     SK_FG_MULT, fgM);
            PublishStatsJson(body);
        }
        if (line[0]) TimingLog(line); // outside g_timingMutex (TimingLog locks it)

        if (vsTiming) {
            std::snprintf(timingOut, timingLen, "pack=%.1f,nvof=%.1f,eval_cpu=%.1f,gpu=%.1f,unpack=%.1f",
                          packMs, nvofMs, evalOnlyMs, gpuWaitMs, unpackMs);
        }
    }
    return rb;
}

void DlssnrContext::PublishDeadState(const char *state, const char *detail) noexcept {
    // 边缘触发:同一死亡状态只发布一次(多帧线程并发早退时第一个写入者
    // 定内容,后来者跳过)。死亡的 tick 不再运行,这条写入必须替换共享
    // 内存里的旧统计 body,面板才不会一直显示冻结的"NGX 延迟"。
    const int code = std::strcmp(state, "ngx_faulted") == 0 ? 1 : 0;
    if (_lastDeadState.exchange(code) == code) return;
    char safe[288];
    SanitizeJsonDetail(detail, safe, sizeof(safe));
    // 探针:死亡状态边缘进 timing log(边缘触发 = 不刷屏)。此前 init
    // 失败/帧早退的原因串只走 VS logMessage(GUI mpv 不透传)+ stats,
    // timing log 完全空白 —— "为什么回退直通"只能靠猜。
    {
        char msg[384];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: dead state=%s%s%.180s",
                      state, safe[0] ? " detail=" : "", safe);
        TimingLog(msg);
        DbgLine(msg);
    }
    char body[512];
    if (safe[0]) {
        std::snprintf(body, sizeof(body), "{\"%s\":\"%s\",\"%s\":\"%.200s\"}",
                      SK_FILTER_STATE, state, SK_STATE_DETAIL, safe);
    } else {
        std::snprintf(body, sizeof(body), "{\"%s\":\"%s\"}", SK_FILTER_STATE, state);
    }
    PublishStatsJson(body);
}

void DlssnrContext::Shutdown() noexcept {
    // 探针:Shutdown 只在停用路径运行(热上下文刻意泄漏,永不走到这)。
    // 每次出现都是生命周期事件:parked 上下文排干 / 故障隔离 / 直通实例释放。
    {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: ngx shutdown (faulted=%d ready=%d)",
                      NgxRuntimeGuard::IsFaulted() ? 1 : 0, _ready.load(std::memory_order_relaxed) ? 1 : 0);
        TimingLog(msg);
    }
    // NVOF 会话:不调用 nvOFDestroy(销毁后继续进程存活期的 GPU 工作
    // 会触发驱动访问违例,见 _retiredNvof 注释)—— 弃引用,随进程退出
    // 由 OS 回收。宿主重启才是真实的生命周期终点。
    if (_nvof) {
        _nvof.release();
        _curOfQuality = 0;
        _nvofFailed = false;
    }
    // FG:proxy 模块进程级钉住(永不 FreeLibrary),feature 弃引用随进程
    // 回收;参数块在 core 存活期销毁(与 _parameters 同序,Shutdown1 之前)。
    _fg.release(); // 析构不触碰 proxy(见 dlssfg_context 析构注释)
    if (_fgParams) {
        DWORD sehCode = 0;
        CoreDestroyParametersSafely(_fgParams, &sehCode);
        _fgParams = nullptr;
    }
    // A faulted latch refuses further SDK entry (Invoke returns the fallback
    // with *sehCode == 0): skip shutdown entirely and keep the faulted modules
    // parked — matching Magpie's fault isolation, only a host restart clears it.
    if (NgxRuntimeGuard::IsFaulted()) {
        DbgLine("NGX faulted earlier; skipping SDK shutdown (isolation, no re-entry)");
        RestoreSnippetCallerHook(_hook);
        if (_snippetModule) {
            if (!_hook.installed) FreeLibrary(_snippetModule);
            _snippetModule = nullptr;
        }
        _ready = false;
        _d3d12 = nullptr;
        return;
    }
    if (_feature && _snippetReleaseFeature) {
        DWORD sehCode = 0;
        SnippetReleaseSafely(&sehCode);
        _feature = nullptr;
    }
    // Destroy the parameter block while the core is still alive (Magpie
    // DLSSNRFilter.cpp:834-847 order: release -> DestroyParameters ->
    // shutdown); doing it after Shutdown1 operates on freed core state.
    if (_parameters) {
        DWORD sehCode = 0;
        CoreDestroyParametersSafely(_parameters, &sehCode);
        _parameters = nullptr;
    }
    if (_snippetInitialized && _snippetShutdown && _d3d12) {
        DWORD sehCode = 0;
        const NVSDK_NGX_Result r = SnippetShutdownSafely(&sehCode);
        // A final shutdown failure is equally unsafe to retry (upstream marks
        // this state non-recoverable) — latch the process.
        if (sehCode || !NVSDK_NGX_SUCCEED(r)) {
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "DLSSNR STATUS: snippet Shutdown1 FAILED (r=0x%x seh=0x%x); process latched",
                          static_cast<unsigned>(r), static_cast<unsigned>(sehCode));
            TimingLog(msg);
            NgxRuntimeGuard::MarkShutdownFailed();
        }
        _snippetInitialized = false;
    }
    if (_coreInitialized && _d3d12) {
        DWORD sehCode = 0;
        const NVSDK_NGX_Result r = CoreShutdownSafely(_d3d12->Device(), &sehCode);
        if (sehCode || !NVSDK_NGX_SUCCEED(r)) {
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "DLSSNR STATUS: core Shutdown1 FAILED (r=0x%x seh=0x%x); process latched",
                          static_cast<unsigned>(r), static_cast<unsigned>(sehCode));
            TimingLog(msg);
            NgxRuntimeGuard::MarkShutdownFailed();
        }
        _coreInitialized = false;
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
