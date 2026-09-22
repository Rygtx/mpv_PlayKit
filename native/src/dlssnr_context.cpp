// Ported from Magpie experimental DLSSNRFilter.cpp / NgxD3D12Core.cpp (see header).
#include "dlssnr_context.h"
#include "dlssfg_gate.h"
#include "ffxof_context.h"
#include "ngx_runtime_guard.h"
#include "nvof_context.h"
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

// stats 通道自身建立失败的留痕钩子(panel_ipc.h):映射创建失败曾经只有
// OutputDebugString 一个出口 —— GUI mpv 没人看,面板从此空白而 timing log
// 零痕迹,与"插件没加载"无法区分。注册后失败行进同一日志文件。
namespace {
const bool s_registerStatsFailLog = [] {
    vsdlssnr::g_statsChannelFailLog = TimingLog;
    return true;
}();
} // namespace

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
    // OptiScaler DLSSNR fork 对齐:模型建 feature 时读一次 tuning("set
    // before create"),create 侧同键双写;evaluate 侧逐帧写入是 mpv 路径
    // 实测生效的机制(A/B DIFF),双写无害。UICorrection 依赖引擎提供的
    // UI/UIAlpha 图层资源 —— 视频管线无 UI 图层,结构性 no-op,恒写模型
    // 默认 1(与 OptiScaler 的处理一致,不复露为用户选项)。
    p->Set(PARAM_AUTO_MASK, createParams.useAutoMask ? 1 : 0);
    p->Set(PARAM_UI_CORRECTION, 1);
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
    // UICorrection 退役(v13):视频管线无 UI 图层,结构性 no-op。恒写模型
    // 默认 1,防参数块复用时残留旧值静默生效(OptiScaler 显式写入策略)。
    p->Set(PARAM_UI_CORRECTION, 1);
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
    const RtxVideoParams &rtx,
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
    _rtx = rtx;

    // RTX Video 几何换算(创建时定格;capability 预检在资源创建之前 ——
    // 不过 = 资源就不按 RTX 形态建,直通尺寸零浪费):
    //   ratio = 目标高 / 源高;≤ 1.001 → VSR 旁路(只支持放大,对齐浏览器
    //   端官方语义;1:1 与缩小场景本就不该付 VSR 的 GPU 价)。
    //   pipe  = min(目标, 源×4) —— 官方单 pass 放大上限,超出部分由
    //   convert-out 双线性从 4x 中间位补完。
    //   hdr   = TrueHDR 在管线(输出几何切换 P10,与 VSR 无关,pipe=src)。
    {
        int dstW = width, dstH = height;
        if (_rtx.vsrMode == 1 || _rtx.vsrMode == 2) {
            if (_rtx.vsrMode == 2) {
                // 手动倍率:输出 = 源 × scale(宽度按源宽高比推)。
                const double sc = std::clamp(_rtx.vsrScale, kVsrScaleMin, kVsrScaleMax);
                dstH = static_cast<int>(std::lround(static_cast<double>(height) * sc));
                dstW = static_cast<int>(std::lround(static_cast<double>(width) * sc));
            } else {
                // vsrAutoHeight <= 0 = 探测失败(无可见窗口):目标=源,
                // ratio=1 走旁路 —— 窗口出现后的链重建重新探测并启用。
                dstH = _rtx.vsrAutoHeight > 0
                           ? std::clamp(_rtx.vsrAutoHeight, 144, 8192)
                           : height;
                const double scale = static_cast<double>(dstH) / static_cast<double>(height);
                dstW = static_cast<int>(std::lround(static_cast<double>(width) * scale));
            }
            dstH = (std::max)(1, dstH);
            dstW = (std::max)(1, dstW);
            // 420 输出契约:目标尺寸必须取偶。VS 按算术右移分配色度面
            // (奇高 → floor 行数),拷贝侧按 ceil 行数写会越界一格 ——
            // 静默堆腐蚀,落进未映射页时 c0000005(2026-09-22 真机实锤:
            // 窗口客户区 2290x959 奇高,15 帧后崩)。偏差 ≤1px。
            dstH &= ~1;
            dstW &= ~1;
        }
        const double ratio = static_cast<double>(dstH) / static_cast<double>(height);
        _vsrRequested = _rtx.vsrMode > 0 && ratio > 1.001;
        _hdrActive = _rtx.hdrEnabled != 0;
        if (_vsrRequested) {
            const double cap = static_cast<double>(kVsrMaxScale);
            _pipeH = static_cast<int>(std::lround(static_cast<double>(height) *
                                                  (std::min)(ratio, cap)));
            _pipeW = static_cast<int>(std::lround(static_cast<double>(width) *
                                                  (std::min)(ratio, cap)));
            _pipeH = (std::max)(1, _pipeH);
            _pipeW = (std::max)(1, _pipeW);
            _outW = dstW;
            _outH = dstH;
        } else {
            _pipeW = width;
            _pipeH = height;
            _outW = width;
            _outH = height;
        }
        // 同上:直通/HDR-only 路径的输出也强制偶尺寸(源本身奇尺寸时
        // newVideoFrame 的色度面行数是 floor,拷贝侧必须与其一致)。
        _outW &= ~1;
        _outH &= ~1;
        _pipeW &= ~1;
        _pipeH &= ~1;
        _rtxActive = _vsrRequested || _hdrActive;
    }

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

    // 0) FG hook 代理预载(先于 NGX 核心存在)。0.3.x 的设计路径 = 代理
    // DllMain 挂 LoadLibrary 监视、等核心出现再装钩;"核心先在、代理后进"
    // 的非设计路径下钩子生效极慢(2026-09-22 实测:预载当拍查询 + 250ms×20
    // 轮询全败 0xBAD0000B,+17s seek 后同进程重查才过)。挪到核心初始化前
    // 即走设计路径。纯官方档/FG 未请求不预载。
    // GPU 族分流:SM86 代理只适用 Turing/Ampere;Ada 及更新走官方链,由
    // dlssfg_context 的 mfg gate 解锁拿多帧(RTX 40 6x)。探测失败 fail-open
    // 维持预载(30 系无损)。
    if (_shared->Snapshot().fgEnabled &&
        std::clamp(_shared->Snapshot().fgRoute, kFgRouteMin, kFgRouteMax) == kFgRouteAuto &&
        fgDllPath && fgDllPath[0] &&
        dlssfg_gate::GpuFamilyPrefersProxy()) {
        // 预载失败曾经裸 return false 零痕迹:下游只会报 "official NGX
        // reports DLSSG unavailable",把用户引向驱动/官方 DLL,真因
        // (version.dll 缺失/被杀软隔离)无一行日志。原因串进 _fgProxyNote,
        // 官方链失败时并入 fg_detail(面板直读)。
        char proxyErr[96]{};
        if (DlssfgContext::PreloadProxyModule(fgDllPath, proxyErr, sizeof(proxyErr))) {
            TimingStatusLine(
                "DLSSNR STATUS: dlssfg proxy preloaded before NGX core (load-monitor hook attach path)");
        } else {
            std::snprintf(_fgProxyNote, sizeof(_fgProxyNote), "%s", proxyErr);
            char msg[224];
            std::snprintf(msg, sizeof(msg),
                          "DLSSNR STATUS: dlssfg proxy preload FAILED (%s); falling through to official chain",
                          proxyErr);
            TimingStatusLine(msg);
        }
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

    // 2b) RTX Video capability 预检(核心在、参数块在之后立刻查;VSR/
    //     TrueHDR 不可用 = 对应功能静默退出,几何按直通尺寸回收 —— 资源
    //     不按 RTX 形态建,零浪费。可用性 = capability 键 + snippet DLL
    //     部署双条件:键由核心解析 nvngx_vsr.dll/nvngx_truehdr.dll 时回填,
    //     DLL 缺失时键缺失,同样走退出)。
    {
        auto vsrCapOk = [&]() -> bool {
            if (!_vsrRequested) return false;
            DWORD sehCode = 0;
            NVSDK_NGX_Parameter *cap = nullptr;
            const NVSDK_NGX_Result r = CoreGetCapabilityParametersSafely(&cap, &sehCode);
            if (sehCode || !NVSDK_NGX_SUCCEED(r) || !cap) return false;
            int available = 0;
            const bool have = cap->Get(NVSDK_NGX_Parameter_VSR_Available, &available) ==
                              NVSDK_NGX_Result_Success;
            _vsrParams = cap; // 可用性定案后由 4c 段消费(不可用也留着,Shutdown 不触碰)
            return have && available;
        };
        auto hdrCapOk = [&]() -> bool {
            if (!_hdrActive) return false;
            DWORD sehCode = 0;
            NVSDK_NGX_Parameter *cap = nullptr;
            const NVSDK_NGX_Result r = CoreGetCapabilityParametersSafely(&cap, &sehCode);
            if (sehCode || !NVSDK_NGX_SUCCEED(r) || !cap) return false;
            int available = 0;
            const bool have = cap->Get(NVSDK_NGX_Parameter_TrueHDR_Available, &available) ==
                              NVSDK_NGX_Result_Success;
            _hdrParams = cap;
            return have && available;
        };
        if (!vsrCapOk()) {
            if (_vsrRequested) {
                TimingStatusLine("DLSSNR STATUS: rtx vsr unavailable (capability); passthrough size");
                std::snprintf(_rtxDetail, sizeof(_rtxDetail),
                              "VSR 不可用(capability;驱动/RTX 显卡/nvngx_vsr.dll)");
            }
            _vsrRequested = false;
            _pipeW = width;
            _pipeH = height;
            _outW = width;
            _outH = height;
        }
        if (!hdrCapOk()) {
            if (_hdrActive) {
                TimingStatusLine("DLSSNR STATUS: rtx hdr unavailable (capability); SDR output");
                std::snprintf(_rtxDetail, sizeof(_rtxDetail),
                              "TrueHDR 不可用(capability;驱动/RTX 显卡/nvngx_truehdr.dll)");
            }
            _hdrActive = false;
        }
        _rtxActive = _vsrRequested || _hdrActive;
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

    {
        // 真因透传:五种失败(缺槽位/owner 被占/句柄失败/VirtualProtect/null
        // 导入)曾统一报成静态文案,日志误导排查方向。
        char hookErr[96]{};
        if (!InstallSnippetCallerHook(_snippetModule, _hook, hookErr, sizeof(hookErr))) {
            char msg[160];
            std::snprintf(msg, sizeof(msg), "IAT hook install failed (%s)",
                          hookErr[0] ? hookErr : "unknown");
            return fail(msg);
        }
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
    // FG 会话级事实初值:请求了 = 暂记 copy(初始化成功会被下文改写成
    // 实际路由);没请求 = off。失败原因串在此段内逐路径覆写。
    _fgCreateMult = _fgRequested
                        ? std::clamp(_shared->Snapshot().fgMultiplier, kFgMultMin, kFgMultMax)
                        : 0;
    std::snprintf(_fgRouteEff, sizeof(_fgRouteEff), "%s", _fgRequested ? "copy" : "off");
    _fgDetail[0] = '\0';
    // 预载失败归因并入:官方链失败时 _fgDetail 只描述官方链自身,真因
    // (proxy 没进托)在 _fgProxyNote —— 拼接后面板一行直读完整因果,
    // 不必再翻 timing log 反推。
    auto appendProxyNote = [&]() noexcept {
        if (!_fgProxyNote[0] || !_fgDetail[0]) return;
        const size_t len = std::strlen(_fgDetail);
        std::snprintf(_fgDetail + len, sizeof(_fgDetail) - len, "; %s", _fgProxyNote);
    };
    if (!_d3d12->CreateFrameResources(_width, _height, _depth, _fgRequested,
                                      _pipeW, _pipeH, _outW, _outH,
                                      _vsrRequested, _hdrActive, err, errLen)) return failWithExistingErr();
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
        // FG 路由选择(0=自动:预载 0.3.x hook 代理后走官方链;1=纯官方:
        // 不预载代理直连官方运行时)。钉档失败不回落(选错档 = FG 关,
        // 输出 1:1)。
        const int fgRoute =
            std::clamp(_shared->Snapshot().fgRoute, kFgRouteMin, kFgRouteMax);
        // 官方 NGX 链(PORTING #8):官方签名 nvngx_dlssg.dll 与模型 DLL
        // 同目录(ngx\)部署,经共享 NGX core 走官方签名链 —— 免自签
        // proxy 与杀软误报面。参数块 = GetCapability(官方 DLSSG 的 Magpie
        // 同款 create/eval 块)。RTX 30/20 的 DLSS-G 由 dlssg_for_sm86
        // 0.3.x hook 代理接管交付(见下方预载段),链路形态不变。
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
        // (自动档预载已前移至 NGX 核心初始化之前 —— 见 Initialize 第 0 步:
        // 0.3.x 钩子的设计路径是代理先在、监视核心加载后再装;0.3.x 代理
        // 拦截 nvngx_dlssg.dll 加载替换为内嵌运行库 + fg_gate 钩子接答核心
        // 能力查询/CreateFeature。)
        bool fgUp = false;
        if (officialDll[0]) {
            DWORD sehCode = 0;
            const NVSDK_NGX_Result pr = CoreGetCapabilityParametersSafely(&_fgParams, &sehCode);
            if (sehCode || !NVSDK_NGX_SUCCEED(pr) || !_fgParams) {
                TimingStatusLine(fgRoute == kFgRouteOfficial
                                     ? "DLSSNR STATUS: dlssfg official capability block FAILED; FG off (route pinned)"
                                     : "DLSSNR STATUS: dlssfg official capability block FAILED; FG off");
                std::snprintf(_fgDetail, sizeof(_fgDetail), "official capability block failed");
                appendProxyNote();
            } else {
                _fg = std::make_unique<DlssfgContext>();
                char fgErr[256]{};
                // FG backbuffer = 管线色(_hdrPipe → FP16 scRGB;
                // SDR = BGRA8),尺寸 = PIPE(VSR 放大后= 中间位)。
                if (_fg->Initialize(*_d3d12, officialDll, _fgParams,
                                    _pipeW, _pipeH,
                                    _hdrActive ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                               : DXGI_FORMAT_B8G8R8A8_UNORM,
                                    fgErr, sizeof(fgErr))) {
                    fgUp = true;
                    // 钩子进程级、装上不可拆:只要缓存模块是 hook 型就标
                    // official-hook(与本次是否预载解耦 —— 上会话自动档
                    // 预载后,本会话纯官方在 30/20 系实际仍是 hook 在托)。
                    std::snprintf(_fgRouteEff, sizeof(_fgRouteEff), "%s",
                                  DlssfgContext::CachedProxyIsHookStyle() ? "official-hook" : "official");
                } else {
                    char msg[352];
                    std::snprintf(msg, sizeof(msg),
                                  "DLSSNR STATUS: dlssfg official init failed (%s); %s",
                                  fgErr,
                                  fgRoute == kFgRouteOfficial
                                      ? "FG off (route pinned)"
                                      : "FG off (dlssg_for_sm86 >= 0.3.0 required for RTX 30/20)");
                    DbgLine(msg);
                    TimingStatusLine(msg);
                    SanitizeJsonDetail(fgErr, _fgDetail, sizeof(_fgDetail));
                    appendProxyNote();
                    _fg.reset();
                    // 参数块:FG 死了块也作废(core 参数块无成本,留着会话内
                    // 复用反而要考虑并发;直接随进程回收,Shutdown 不再触碰)。
                    _fgParams = nullptr;
                }
            }
        }
        if (!fgUp) {
            // 部署缺失时尝试路径没跑,失败原因没有其它出口,这里补一行;
            // 尝试路径自身失败已有带因状态行。
            if (!officialDll[0]) {
                TimingStatusLine(
                    "DLSSNR STATUS: dlssfg nvngx_dlssg.dll missing; 1:1 output");
                std::snprintf(_fgDetail, sizeof(_fgDetail), "nvngx_dlssg.dll missing");
                appendProxyNote();
            }
            _fgRequested = false; // 槽资源已带 FG 纹理,无害留用
            _fgCreateMult = 0;    // 未激活:面板倍数 mismatch 判定归零
        }
    }

    // 4a-bis) RTX Video features(VSR → TrueHDR;capability 已在 2b 预检,
    // 这里只做 CreateFeature。失败 = capability 过后仍建不起来(驱动/核心
    // 版本错配等异常)→ 整体初始化失败:管线几何已按 RTX 形态建,半套
    // 降级会写出未定义内容 —— 插件层回落纯直通比静默花屏诚实)。
    if (_vsrRequested) {
        _vsr = std::make_unique<RtxVsrContext>();
        char rtxErr[192]{};
        if (!_vsr->Initialize(*_d3d12, _vsrParams, _width, _height, rtxErr, sizeof(rtxErr))) {
            SanitizeJsonDetail(rtxErr, _rtxDetail, sizeof(_rtxDetail));
            char msg[256];
            std::snprintf(msg, sizeof(msg), "rtx vsr CreateFeature failed: %.180s", rtxErr);
            return fail(msg);
        }
    }
    if (_hdrActive) {
        _hdr = std::make_unique<RtxHdrContext>();
        char rtxErr[192]{};
        if (!_hdr->Initialize(*_d3d12, _hdrParams, _pipeW, _pipeH, rtxErr, sizeof(rtxErr))) {
            SanitizeJsonDetail(rtxErr, _rtxDetail, sizeof(_rtxDetail));
            char msg[256];
            std::snprintf(msg, sizeof(msg), "rtx hdr CreateFeature failed: %.180s", rtxErr);
            return fail(msg);
        }
    }
    _rtxDetail[0] = '\0'; // VSR/HDR 全部成功:清空失败原因(diagnostic 红显解除)

    // 4b) 光流会话(of_backend 单一后端,默认 FFX):quality > 0 时建立;
    // 失败优雅回退零 guidance(记 _nvofFailed,不拖垮整个滤镜)。冷初始化
    // 单线程、槽池空闲,满足会话的 PoolHold 约束。档位按后端取对应字段
    // (NVOF→motionVectorQuality,FFX→ffxQuality)。
    {
        const DlssnrParams snap = _shared->Snapshot();
        const int backendReq =
            std::clamp(snap.ofBackend, kOfBackendMin, kOfBackendMax);
        const int ofq = ResolveOfQuality(snap); // clamp 在 helper 内(按后端值域)
        _curOfQuality = ofq;
        _nvofFailed = false;
        {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: init of=%d backend=%d",
                          ofq, backendReq);
            TimingStatusLine(msg);
        }
        if (ofq > 0) {
            char ofErr[160]{};
            _ofBackend = CreateOfBackend(ofq, _width, _height, ofErr, sizeof(ofErr));
            if (_ofBackend) {
                _curOfBackend = backendReq;
                _ofDetail[0] = '\0';
                char msg[160];
                std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: of session created backend=%d quality=%d %dx%d",
                              _curOfBackend, ofq, _width, _height);
                DbgLine(msg);
                TimingLog(msg);
            } else {
                _nvofFailed = true;
                // 原因串入 _ofDetail(of_detail 键):诊断页"请求 X | 实际
                // zero"只说降级事实,"为什么"(SM6.2 不支持/驱动拒双向/
                // dll 缺失)从这里直达面板 —— fg_detail 同款三件套。
                SanitizeJsonDetail(ofErr, _ofDetail, sizeof(_ofDetail));
                char msg[288];
                std::snprintf(msg, sizeof(msg),
                              "DLSSNR STATUS: of init failed (%s); zero guidance", ofErr);
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
        // 960:gpu_name(≤128)+ fg_detail(≤128)+ of_detail(≤96)全满时
        // 512 会截断(PublishStatsJson 超长静默截断 = 尾键丢失,面板读不到
        // 还不报错 —— v19 扩容 tick body 的同一教训)。
        char rtxState[96];
        if (_vsrRequested && _hdrActive) {
            std::snprintf(rtxState, sizeof(rtxState), "vsr+hdr %dx%d", _outW, _outH);
        } else if (_vsrRequested) {
            std::snprintf(rtxState, sizeof(rtxState), "vsr %dx%d", _outW, _outH);
        } else if (_hdrActive) {
            std::snprintf(rtxState, sizeof(rtxState), "hdr %dx%d", _outW, _outH);
        } else {
            std::snprintf(rtxState, sizeof(rtxState), "off");
        }
        // 缓存进成员:每帧 stats 体(_rtx 键)复用 —— 每帧体覆盖 init 体后
        // 若缺 rtx 键,面板诊断恒 "(未加载)"(2026-09-22 实锤)。
        std::snprintf(_rtxStateStr, sizeof(_rtxStateStr), "%s", rtxState);
        char body[1024];
        std::snprintf(body, sizeof(body),
                      "{\"gpu_name\":\"%s\",\"width\":%d,\"height\":%d,"
                      "\"rtx\":\"%s\",\"rtx_detail\":\"%s\","
                      "\"%s\":\"%s\",\"%s\":\"%s\","
                      "\"%s\":\"%s\",\"%s\":%d,\"%s\":\"%s\","
                      "\"%s\":%d,\"%s\":\"%s\"}",
                      _gpuNameUtf8, _width, _height,
                      rtxState, _rtxDetail,
                      SK_FILTER_STATE, (_nvofFailed && _curOfQuality > 0) ? "nvof_zero" : "ok",
                      SK_OF_MODE, OfModeString(),
                      SK_FG_ROUTE_EFFECTIVE, _fgRouteEff,
                      SK_FG_MULT_CREATE, _fgCreateMult,
                      SK_FG_DETAIL, _fgDetail,
                      SK_FG_MULT_MAX, _fg ? _fg->MaxGen() : 0,
                      SK_OF_DETAIL, _ofDetail);
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
        char msg[384];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: Feature=18 created=true path=signed-snippet %dx%dd%d disabled=false gpu=%.60s rtx=%s pipe=%dx%d out=%dx%d hdr=%d fg=%d",
                      _width, _height, _depth, _gpuNameUtf8,
                      _vsrRequested ? (_hdrActive ? "vsr+hdr" : "vsr") : (_hdrActive ? "hdr" : "off"),
                      _pipeW, _pipeH, _outW, _outH,
                      _hdrActive ? 1 : 0, _fg && _fg->Enabled() ? 1 : 0);
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
    if (resize) {
        // RTX 几何随源尺寸换算(必须先于 CreateFrameResources —— 资源按
        // 新几何建)。vsr 开:输出目标 _outH 绝对(显示器适配/手动高度),
        // 宽按新源宽高比重推,pipe = min(目标, 新源×4);仅 hdr:pipe/out
        // 跟随新源(TrueHDR 不缩放)。
        if (_vsrRequested) {
            const double ratio = static_cast<double>(_outH) / static_cast<double>(newHeight);
            const double cap = static_cast<double>(kVsrMaxScale);
            _outW = (std::max)(1, static_cast<int>(std::lround(
                                      static_cast<double>(newWidth) * ratio)));
            _pipeH = static_cast<int>(std::lround(
                static_cast<double>(newHeight) * (std::min)(ratio, cap)));
            _pipeW = static_cast<int>(std::lround(
                static_cast<double>(newWidth) * (std::min)(ratio, cap)));
            _pipeW = (std::max)(1, _pipeW);
            _pipeH = (std::max)(1, _pipeH);
        } else {
            _pipeW = newWidth;
            _pipeH = newHeight;
            if (_hdrActive) {
                _outW = newWidth;
                _outH = newHeight;
            }
        }
    }
    if (resize && !_d3d12->CreateFrameResources(newWidth, newHeight, newDepth, _fgRequested,
                                                _pipeW, _pipeH, _outW, _outH,
                                                _vsrRequested, _hdrActive, err, errLen)) {
        _ready.store(false, std::memory_order_release);
        return false;
    }
    if (resize) {
        _width = newWidth;
        _height = newHeight;
        _depth = newDepth;
        // FG feature 随尺寸重建(proxy Release + CreateFeature;失败 = FG
        // 闩停,降级复制帧,NR 不受影响)。历史在重建时作废。backbuffer
        // 尺寸/格式 = PIPE/RTX 形态(与冷初始化同判据)。
        if (_fg && _fg->Enabled()) {
            char fgErr[192]{};
            if (!_fg->Rebuild(_pipeW, _pipeH,
                              _hdrActive ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                         : DXGI_FORMAT_B8G8R8A8_UNORM,
                              fgErr, sizeof(fgErr))) {
                char msg[288];
                std::snprintf(msg, sizeof(msg),
                              "DLSSNR STATUS: dlssfg rebuild failed (%s); FG off (dup)",
                              fgErr);
                DbgLine(msg);
                TimingStatusLine(msg);
            }
        }
        // 光流会话随尺寸重建(已在 PoolHold 内,内联处理,勿调 RebuildOf
        // —— 那会二次取 PoolHold 死锁)。退役旧会话不销毁(见 _retiredOf
        // 注释),失败降级零 guidance,不致命。会话输入尺寸遵循 follow 语义
        // (scalingEnabled/resPercent 是本次重建的目标态,内部尺寸由参数
        // 直推,不依赖尚未执行的 RebuildScaling)。
        if (_ofBackend && _curOfQuality > 0 && !_nvofFailed) {
            const DlssnrParams sp = _shared->Snapshot();
            // FG 激活时 MVecs 契约要求源尺寸稠密运动 —— follow 被忽略。
            const bool foll = sp.nvofFollowScaling != 0 && scalingEnabled && !(_fg && _fg->Enabled());
            int iw = _width, ih = _height;
            if (foll) InternalSize(_width, _height, resPercent, iw, ih);
            char ofErr[160]{};
            auto next = CreateOfBackend(_curOfQuality, iw, ih, ofErr, sizeof(ofErr));
            if (next) {
                _retiredOf.push_back(std::move(_ofBackend));
                _ofBackend = std::move(next);
                _curOfBackend = std::clamp(sp.ofBackend, kOfBackendMin, kOfBackendMax);
            } else {
                _nvofFailed = true;
                char msg[288];
                std::snprintf(msg, sizeof(msg),
                              "DLSSNR STATUS: of resize failed (%s); zero guidance", ofErr);
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

std::unique_ptr<IOpticalFlowBackend> DlssnrContext::CreateOfBackend(
    int q, int dstW, int dstH, char *err, size_t errLen) noexcept {
    // of_backend = 单一后端(用户裁定 2026-09-21:默认 FFX,跨厂商通用;
    // 上游 Magpie 同款 RTX 5070 Ti 上 AMD OF 实测有流)。仅 1 = nvof 时
    // 走 NVOF(NVOF 引擎本身 NVIDIA 专属,内部厂商门会拦截)。失败 =
    // 零 guidance,不跨后端回落(对齐 fg_route 先例:行为可预测)。
    const int backendReq =
        std::clamp(_shared->Snapshot().ofBackend, kOfBackendMin, kOfBackendMax);
    if (backendReq == kOfBackendNvof) {
        auto b = std::make_unique<NvofContext>();
        if (!b->Initialize(*_d3d12, dstW, dstH, q, err, errLen)) return nullptr;
        return b;
    }
    auto b = std::make_unique<FxofContext>();
    if (!b->Initialize(*_d3d12, dstW, dstH, q, err, errLen)) return nullptr;
    return b;
}

bool DlssnrContext::RebuildOf(int quality, int dstW, int dstH, char *err, size_t errLen) noexcept {
    // 光流会话重建(quality / of_backend / 会话输入尺寸变化;follow 模式下
    // 会话输入 = 内部尺寸,由调用方传入 dstW/dstH)。只重建光流会话,NGX
    // feature 不动。内部自取 PoolHold(槽池封死满足会话的调用约束)——
    // 因此绝不能在已持有 PoolHold 的路径上调用(RecreateFeature 的尺寸重建
    // 内联处理,不走这里)。调用方必须尚未持有槽位(ProcessFrame 在
    // AcquireSlot 之前消费本请求,与 ConsumeRebuild 同款)。
    //
    // _nvofMutex:fmParallel 下多个帧线程会同时看到同一档位变化并并发进入
    // (实测并发重建互相踩踏挂死);锁内复查 _curOfQuality/_curOfBackend 与
    // 尺寸,后来者直接跳过。
    std::lock_guard<std::mutex> switchLock(_nvofMutex);
    // 调用方传入的档位已经 ResolveOfQuality 按后端值域 clamp;此处宽 clamp
    // 仅作双保险(FFX 的 2 也在 0-5 内,不受影响)。
    const int q = std::clamp(quality, kOfQualityMin, kOfQualityMax);
    const int backendReq =
        std::clamp(_shared->Snapshot().ofBackend, kOfBackendMin, kOfBackendMax);
    if (q == _curOfQuality && backendReq == _curOfBackend && !_nvofFailed && _ofBackend &&
        _ofBackend->Width() == dstW && _ofBackend->Height() == dstH) return true;
    {
        D3D12Context::PoolHold pool(*_d3d12);
        if (q == 0) {
            // 保留会话仅停用:后端销毁不可靠(NVOF 引擎 destroy 实测崩溃,
            // FFX 首期同策略走 _retiredOf);空闲会话无 GPU 开销,与热上下文
            // 同哲学,进程退出统一回收。
            _nvofFailed = false;
            _curOfQuality = 0;
            TimingStatusLine("DLSSNR STATUS: of disabled (quality=0; session kept)");
        } else if (!_ofBackend || _nvofFailed ||
                   _ofBackend->Quality() != q || _ofBackend->Width() != dstW ||
                   _ofBackend->Height() != dstH || backendReq != _curOfBackend) {
            // 先建新会话再弃旧(旧会话仅弃引用,不销毁 —— 见 _retiredOf 注释)。
            // 弃旧的 GPU 资源随 PoolHold 排空后不再被引用,纹理显存由驱动
            // 按引用回收(对象随进程生存)。
            auto next = CreateOfBackend(q, dstW, dstH, err, errLen);
            if (next) {
                if (_ofBackend) _retiredOf.push_back(std::move(_ofBackend)); // 退役,不销毁
                _ofBackend = std::move(next);
                _nvofFailed = false;
                _curOfQuality = q;
                _curOfBackend = backendReq;
            } else {
                _nvofFailed = true;
                _curOfQuality = q;
                _curOfBackend = backendReq;
                char msg[288];
                std::snprintf(msg, sizeof(msg),
                              "DLSSNR STATUS: of init failed backend=%d quality=%d (%s); zero guidance",
                              backendReq, q, err && errLen ? err : "");
                DbgLine(msg);
                TimingLog(msg);
                return false; // 调用方决定是否视为致命(帧路径上不致命)
            }
        } else {
            // 复用保留的会话(q=0 期间停用):帧序门与历史都已在停用期冻结,
            // 重置让下一帧重新播种,避免旧参考帧产生一次错误流。停用期的
            // 尺寸/档位/后端变化由上方的条件分支兜住。
            _ofBackend->ResetHistory();
            _nvofFailed = false;
            _curOfQuality = q;
        }
    }
    if (_ofBackend && _ofBackend->Enabled()) {
        char msg[160];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: of rebuilt backend=%d quality=%d %dx%d",
                      _curOfBackend, _curOfQuality, dstW, dstH);
        DbgLine(msg);
        TimingLog(msg);
    }
    return _ofBackend && _ofBackend->Enabled();
}

void DlssnrContext::ResetNvofHistory() noexcept {
    // seek = 新时间线:流历史作废,下一帧重新播种(清零发布 + NGX PARAM_RESET;
    // FG 下一帧 eval 带 DLSSG.Reset,该帧插值输出降级复制)。会话本身保留
    // (热上下文跨 seek 存活)。
    if (_ofBackend) _ofBackend->ResetHistory();
    if (_fg) _fg->ResetHistory();
}

bool DlssnrContext::Rebind(SharedParams *shared, int width, int height, int depth,
                           const RtxVideoParams &rtx, char *err, size_t errLen) noexcept {
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
        // 下方重建分支)。档位按后端取对应字段。
        const bool fgHot = _fg && _fg->Enabled();
        const bool rbFollow = p.nvofFollowScaling != 0 && _d3d12->HasScaling() && !fgHot;
        const int nvW = rbFollow ? _d3d12->InternalWidth() : _width;
        const int nvH = rbFollow ? _d3d12->InternalHeight() : _height;
        const int backendReq =
            std::clamp(p.ofBackend, kOfBackendMin, kOfBackendMax);
        const int ofq = ResolveOfQuality(p); // clamp 在 helper 内(按后端值域)
        if (ofq != _curOfQuality || backendReq != _curOfBackend ||
            (ofq > 0 && _ofBackend &&
                (_ofBackend->Width() != nvW || _ofBackend->Height() != nvH)) ||
            (ofq > 0 && _nvofFailed)) {
            RebuildOf(ofq, nvW, nvH, err, errLen); // 失败仅降级零 guidance,热复用不受影响
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
        const int backendReq =
            std::clamp(p.ofBackend, kOfBackendMin, kOfBackendMax);
        const int ofq = ResolveOfQuality(p); // clamp 在 helper 内(按后端值域)
        if (ofq != _curOfQuality || backendReq != _curOfBackend ||
            (ofq > 0 && _ofBackend &&
                (_ofBackend->Width() != nvW || _ofBackend->Height() != nvH)) || (ofq > 0 && _nvofFailed)) {
            RebuildOf(ofq, nvW, nvH, err, errLen);
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
    // 光流档位/后端/会话输入尺寸同步(只重建光流会话,不动 NGX feature)。
    // 在 AcquireSlot 之前消费:RebuildOf 要封池排空。失败只降级零 guidance,
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
    // 档位按后端取对应字段(NVOF→motionVectorQuality,FFX→ffxQuality)。
    const int ofBackendReq =
        std::clamp(frameParams.ofBackend, kOfBackendMin, kOfBackendMax);
    const int ofq = ResolveOfQuality(frameParams); // clamp 在 helper 内(按后端值域)
    if (ofq != _curOfQuality ||
        ofBackendReq != _curOfBackend ||
        (ofq > 0 && _ofBackend && (_ofBackend->Width() != nvDstW || _ofBackend->Height() != nvDstH))) {
        char nvofErr[160]{};
        RebuildOf(ofq, nvDstW, nvDstH, nvofErr, sizeof(nvofErr));
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
    LARGE_INTEGER qpcFreq{}, t0{}, t1{}, t2{}, t3a{}, t3b{}, t4{};
    LARGE_INTEGER tSlot0{}, tSlot1{}; // AcquireSlot 等待(perf 行 slot=)
    LARGE_INTEGER tLock0{}, tLock1{}; // evaluate 互斥等待(perf 行 lock=)
    // t3a/t3b = base/fg 两段栅栏完成点(gpu 段 = t2→t3a 基础管线 GPU;
    // fg 段 = t3a→t3b 插帧 GPU。timestamp query 与 NGX 同 CL 会 SEH,
    // 分段提交 + 栅栏差分是唯一无损精确拆分法)。
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
    // 零光流/面板关/eval 降级)。函数级作用域 —— 统计段与 dump 都要读。
    // fgRan = 本帧向 proxy 提交过 eval(含播种;dump 判定用)。
    int fgEvaluatedCount = 0;
    bool fgRan = false;
    if (!skipEval && _ofBackend && _ofBackend->Enabled() && _curOfQuality > 0) {
        // 会话输入尺寸(follow 模式 = 内部尺寸);densify 的向量换算把流
        // 向量从会话输入像素单位换算回源像素单位(NVOF MotionScale /
        // FFX VectorScale,语义见各分支)。
        const uint32_t nvW = static_cast<uint32_t>(_ofBackend->Width());
        const uint32_t nvH = static_cast<uint32_t>(_ofBackend->Height());
        const float msX = nvW != static_cast<uint32_t>(width)
                              ? static_cast<float>(width) / static_cast<float>(nvW) : 1.0f;
        const float msY = nvH != static_cast<uint32_t>(height)
                              ? static_cast<float>(height) / static_cast<float>(nvH) : 1.0f;
        // follow 内部管线(会话输入 = 内部尺寸 ≠ 源):densify 直接产出
        // NGX 缩放消费纹理 reducedMotion/reducedConfidence,跳过“densify
        // 到源尺寸 → guidance 降采样缩回内部”的放大-缩小 pass。
        densifyInternal =
            nvW != static_cast<uint32_t>(width) || nvH != static_cast<uint32_t>(height);
        const int ofKind = _ofBackend->Kind();
        OfPostExecuteFn post;
        OfPostCopyFn postCopy;
        if (ofKind == kOfBackendNvof) {
            // ---- NVOF:网格流(1/32 像素定点)densify(原 PORTING #6 路径)----
            NvofContext *nv = static_cast<NvofContext *>(_ofBackend.get());
            const uint32_t gs = nv->GridSize();
            const uint32_t flowW = (nvW + gs - 1) / gs;
            const uint32_t flowH = (nvH + gs - 1) / gs;
            const bool hasBwd = nv->Bidirectional();
            const bool hasCost = nv->CostEnabled();
            post = [this, &slot, flowW, flowH, gs, hasCost, hasBwd, msX, msY, densifyInternal,
                    nvW, nvH, width, height](ID3D12GraphicsCommandList *cl, int inputIndex) {
                (void)inputIndex;
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
            // YUV→RGB 转换,follow 时追加 RecordNvofDownsample 直写注册输入
            // 纹理;非 follow 由 StageFrame 随后做整帧纹理拷贝。
            postCopy = [this, &slot, nvW, nvH, densifyInternal, matrix, range](ID3D12GraphicsCommandList *cl, int inputIndex) {
                _d3d12->RecordConvertInput(*cl, *slot, matrix, range,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (densifyInternal) {
                    _d3d12->RecordNvofDownsample(*cl, *slot,
                                                 static_cast<int>(nvW), static_cast<int>(nvH),
                                                 inputIndex);
                }
            };
        } else { // kOfBackendFfx(Kind() 只产出 nvof/ffx 两种)
            // ---- FFX(AMD FidelityFX OF):Prepare 在 postCopy 内紧随转换,
            // densify 读稀疏流(1/8 OF extent,R16G16_SINT,单位 = OF extent
            // 像素),VectorScale = dense/OF。----
            FxofContext *fx = static_cast<FxofContext *>(_ofBackend.get());
            const uint32_t ofW = fx->OfWidth(), ofH = fx->OfHeight();
            const uint32_t spW = fx->SparseWidth(), spH = fx->SparseHeight();
            post = [this, &slot, ofW, ofH, spW, spH, densifyInternal,
                    nvW, nvH, width, height](ID3D12GraphicsCommandList *cl, int inputIndex) {
                (void)inputIndex;
                ID3D12Resource *dstMotion = densifyInternal ? slot->reducedMotion.Get()
                                                            : slot->motion.Get();
                ID3D12Resource *dstConf = densifyInternal ? slot->reducedConfidence.Get()
                                                          : slot->confidence.Get();
                D3D12_RESOURCE_BARRIER g1[2]{
                    Transition(dstMotion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                    Transition(dstConf, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                };
                cl->ResourceBarrier(2, g1);
                const uint32_t denseW = densifyInternal ? nvW : static_cast<uint32_t>(width);
                const uint32_t denseH = densifyInternal ? nvH : static_cast<uint32_t>(height);
                _d3d12->RecordFfxDensify(*cl, *slot, denseW, denseH, ofW, ofH, spW, spH,
                                         static_cast<float>(denseW) / static_cast<float>(ofW),
                                         static_cast<float>(denseH) / static_cast<float>(ofH),
                                         densifyInternal ? 20u : 12u,
                                         densifyInternal ? 21u : 13u);
                D3D12_RESOURCE_BARRIER g2[2]{
                    Transition(dstMotion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                    Transition(dstConf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                };
                cl->ResourceBarrier(2, g2);
            };
            postCopy = [this, &slot, ofW, ofH, width, height, matrix, range](ID3D12GraphicsCommandList *cl, int inputIndex) {
                (void)inputIndex;
                _d3d12->RecordConvertInput(*cl, *slot, matrix, range,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                // Prepare 紧随转换(box 平均下采样写 ffxInput;FFX 无
                // ping-pong 输入,Quality/Performance 的 OF extent 由会话定)。
                _d3d12->RecordFfxPrepare(*cl, *slot, static_cast<uint32_t>(width),
                                         static_cast<uint32_t>(height), ofW, ofH);
            };
        }
        const OfStageResult st =
            _ofBackend->StageFrame(n, _d3d12->InputColor(*slot), post, postCopy, densifyInternal);
        nvofHistoryReset = st.historyReset;
        nvofMs = _ofBackend->LastStageMs();
        realMotion = st.waitFenceValue != 0;
        nvofInputIndex = st.inputIndex;
    }
    // 3 连败停用(会话内部闩锁)在帧线程侧只发现不重试,防风暴;
    // Rebind/换档时经 _nvofFailed 条目重试一次。
    if (_curOfQuality > 0 && _ofBackend && !_ofBackend->Enabled() && !_nvofFailed) {
        _nvofFailed = true;
        TimingStatusLine("DLSSNR STATUS: of disabled (exhausted); zero guidance until rebind");
    }
    // early-return 路径释放槽位前排空在途拷贝(宿主复用 upload 缓冲;
    // 正常路径已被 execute/完成栅栏覆盖,no-op)。
    struct CopyDrainGuard {
        IOpticalFlowBackend *b;
        bool on;
        ~CopyDrainGuard() { if (b && on) b->WaitCopyIdle(); }
    } drainGuard{ _ofBackend.get(), _ofBackend && _curOfQuality > 0 };

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
        // t2 与正常路径 eval 后同位:此分支不赋值的话 gpuWaitMs = ms(t2=0, t3a)
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

    // ---- RTX Video 段(NR 输出 → VSR 放大 → TrueHDR)----
    // 官方契约(Programming Guide 3.3.5):TrueHDR 必须在 VSR 之后(VSR 不
    // 吃 HDR 输入)。执行模型 = 专用命令队列(SDK D3D12 样例 CDx12NGXVSR
    // 同款;真机实测 nvngx_vsr 不接受宿主 CL —— 录上去 Close 必报
    // E_INVALIDARG,2026-09-22):base CL 提交并等待后,VSR/HDR eval 在各自
    // 的专用队列执行(fence 链:base → vsr → hdr),post 段(fg CL)提交时
    // 主队列 Wait 最后一个 RTX fence 再消费管线色。
    // 资源状态契约:输入 outputColor 由 base CL 转 NSR;输出 vsrColor/
    // hdrColor 由 NGX 自行屏障做 UAV 写,Execute 完成后隐式衰减回 COMMON
    // (D3D12 规则)→ post CL 显式 COMMON→NSR 消费、收尾归 COMMON。
    // 管线色 pipeColor(真实尺寸 pipeW/H)是下游(输出转换/FG backbuffer)
    // 的单一事实:hdrRun → hdrColor(FP16 @PIPE);vsrRun → vsrColor
    // (BGRA8 @PIPE);皆无 → outputColor(@src;skipEval/NOEVAL 探针/降级)。
    const bool rtxSessOk = (!_vsrRequested || (_vsr && _vsr->Enabled())) &&
                           (!_hdrActive || (_hdr && _hdr->Enabled()));
    if (!rtxSessOk) {
        // 会话闩停(eval SEH/失败后 Enabled()=false):管线几何按 RTX 形态
        // 建,半套降级会消费未定义内容 —— 与 NGX fault 同语义整体失败,
        // 首帧进 timing log + 面板死亡态,后续帧早退(调方 CopyPlanes 兜底)。
        if (err && errLen) std::snprintf(err, errLen, "rtx video session latched off");
        PublishDeadState("passthrough", err);
        return false;
    }
    const bool vsrLive = _vsrRequested && !skipEval;
    const bool hdrLive = _hdrActive && !skipEval;
    // 探针(VSDLSSNR_RTX_NOEVAL=1):只跑本地管线不调 RTX NGX —— eval 期
    // 问题二分定位(NGX eval 段 vs 本地录制段)。
    static const bool rtxNoEval = GetEnvironmentVariableA("VSDLSSNR_RTX_NOEVAL", nullptr, 0) != 0;
    const bool vsrRun = vsrLive && !rtxNoEval;
    const bool hdrRun = hdrLive && !rtxNoEval;
    const bool rtxIn = vsrRun || hdrRun; // 本帧有 RTX eval:base 尾转 NSR、
                                         // 真实帧输出移 post 段
    ID3D12Resource *pipeColor = _d3d12->OutputColor(*slot);
    int pipeW = width, pipeH = height;
    if (vsrRun) {
        pipeColor = slot->vsrColor.Get();
        pipeW = _pipeW;
        pipeH = _pipeH;
    }
    if (hdrRun) {
        pipeColor = slot->hdrColor.Get();
        pipeW = _pipeW;
        pipeH = _pipeH;
    }
    // FG backbuffer 形态门:backbuffer = pipeColor,仅当它在 PIPE 几何
    // (vsr/hdr 实跑了,或本就无 RTX)。skipEval/NOEVAL 帧整体无 FG。
    const bool fgPipeOk = vsrRun || hdrRun || !_rtxActive;

    // ---- base/fg 分段提交(处理用时拆账)----
    // gpu 段与 fg 段的精确拆分靠两次提交 + 栅栏完成点差分(timestamp query
    // 与 NGX 同 CL 会 SEH,2026-09-05 实锤,栅栏差分是无损精确拆分):
    //   base CL = 基础管线(NR 推理/残差/直通 + 真实帧 YUV/回读)
    //   fg CL   = 插帧链(FG 推理 + 插值 YUV/回读)
    // Begin 在 base 提交前执行:失败即整体降级为门关帧(guidance 归位屏障
    // 落 base 尾,motion 不滞留 NSR —— 帧末全资源 COMMON 不变量跨两段保持)。
    const bool fgGateOpen = fgM > 0 && !skipEval && realMotion && !nvofHistoryReset &&
                            _fg && _fg->Enabled() && fgPipeOk;
    // post 段(fg CL)= FG 门开帧 + RTX 帧(真实帧输出转换在 eval 之后)。
    // Begin 失败:FG 帧降级门关;RTX 帧无处落管线色转换 —— 整帧失败。
    const bool postBeginReq = fgGateOpen || rtxIn;
    bool fgBeginOk = false;
    if (postBeginReq) {
        fgBeginOk = _d3d12->BeginFgRecording(*slot);
        if (!fgBeginOk) {
            if (rtxIn) {
                if (err && errLen) std::snprintf(err, errLen, "post CL begin failed (rtx frame)");
                TimingStatusLine("DLSSNR STATUS: post CL begin FAILED on rtx frame");
                return false;
            }
            TimingStatusLine("DLSSNR STATUS: fg CL begin FAILED; interpolation degraded to dup");
        }
    }
    const bool fgOnFgCl = fgGateOpen && fgBeginOk;

    // 调试视图(面板"调试视图"下拉):1 = |输出−输入|×20 灰度;2 = 光流场
    // (方向→色相、幅值→亮度)。此处是三条路径(skipEval/nrOff/eval)帧末
    // outputColor=UAV 的唯一公共插入点;FG 激活时插帧链会拿到调试图当
    // backbuffer(调试态可接受)。光流场取材:真运动帧按管线取 slot 场
    // (follow 内部管线 = reducedMotion,否则源尺寸 motion;均已 NSR),
    // 播种/OF 关帧绑静态零纹理 —— 全黑 = 无光流数据,与差异视图
    // "一片灰 = 没动"同款语义。
    if (frameParams.debugView == 2) {
        _d3d12->RecordFlowView(*slot, realMotion && densifyInternal, realMotion);
    } else if (frameParams.debugView == 1) {
        _d3d12->RecordDebugDiff(*slot);
    }
    // base CL 收尾:RTX 本帧有 eval → outputColor 转 NSR 作 VSR/HDR 输入,
    // 真实帧输出转换移 post 段(eval 之后才有管线色);无 eval(skipEval/
    // NOEVAL 探针)→ 现场做输出转换(源 = outputColor @src,状态契约同
    // 旧路径);无 RTX 会话 → 旧 RecordYuvOutput,行为逐字节不变。
    auto *baseCl = slot->commandList.Get();
    if (rtxIn) {
        D3D12_RESOURCE_BARRIER inPrep[1]{
            Transition(_d3d12->OutputColor(*slot),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        baseCl->ResourceBarrier(1, inPrep);
    } else if (_rtxActive) {
        _d3d12->RecordColorOutput(*baseCl, *slot,
                                  _d3d12->OutputColor(*slot), D3D12Context::kSrvOutputColor,
                                  false, width, height, matrix, range,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!_d3d12->RecordReadbackCopy(*baseCl, *slot, err, errLen)) {
            return false;
        }
    } else {
        _d3d12->RecordYuvOutput(*baseCl, *slot, matrix, range,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!_d3d12->RecordReadbackCopy(*baseCl, *slot, err, errLen)) {
            return false;
        }
    }
    // guidance 纹理归位 COMMON(fg/post 段才消费:门开帧归位录 post 尾,门关
    // /降级帧录 base 尾。仅 realMotion 帧动过:播种/失败帧的发布走静态零
    // 纹理,per-slot motion 全程 COMMON 不被触碰。follow 内部管线只动过
    // reducedMotion/reducedConfidence —— 源尺寸对全程 COMMON,对它做
    // NSR→COMMON 是非法屏障)。
    auto recordGuidancePark = [&](ID3D12GraphicsCommandList &gcl) {
        if (!realMotion) return;
        if (densifyInternal) {
            D3D12_RESOURCE_BARRIER gBackR[2]{
                TransitionFromTo(slot->reducedMotion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                TransitionFromTo(slot->reducedConfidence.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            };
            gcl.ResourceBarrier(2, gBackR);
        } else {
            D3D12_RESOURCE_BARRIER gBack[2]{
                TransitionFromTo(slot->motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                TransitionFromTo(slot->confidence.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            };
            gcl.ResourceBarrier(2, gBack);
            if (guidanceDown) {
                D3D12_RESOURCE_BARRIER gBackR[2]{
                    TransitionFromTo(slot->reducedMotion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                    TransitionFromTo(slot->reducedConfidence.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                };
                gcl.ResourceBarrier(2, gBackR);
            }
        }
    };
    if (!fgOnFgCl && !rtxIn) recordGuidancePark(*baseCl);
    // NOTE: timestamp disabled, see note above
    // NVOF flow 已在 StageFrame 里 CPU 等待完成(execute 后的输出栅栏),
    // 槽 CL 提交时 GPU 侧 flow 已就绪 —— 不再需要队列级 Wait(实测该栅栏
    // 在队列 Wait 语义下可能永不满足,见 NvofContext::StageFrame 注释)。
    if (!_d3d12->SubmitBaseFrame(*slot, nullptr, 0, err, errLen)) {
        if (err && errLen) {
            TimingStatusLine(err); // 临时探针
            std::snprintf(err, errLen, "Submit(base) failed");
        }
        return false;
    }
    if (ProbeEnabled()) TimingStatusLine("PROBE: base submitted"); // 临时探针(VSDLSSNR_PROBE=1)

    // ---- RTX eval(专用队列;fence 链 base → vsr → hdr)----
    // CPU 端不等 eval 完成:post 段提交时主队列 Wait(rtx fence)保证消费
    // 顺序,Unpack 由 post 栅栏覆盖。录制经 _evaluateMutex 与 NR/DLSSG 串行
    //(NGX 非线程安全;两队列的 CL/allocator 均为单例,锁内顺序录制)。
    uint64_t rtxFenceVal = 0;
    ID3D12Fence *rtxFence = nullptr;
    // per-eval 参数走本帧快照(面板质量/HDR 滑块逐帧生效;几何/形态仍按
    // 创建时的 _rtx)。clamp 在 helper 内的常量引用,越界面板值安全。
    const DlssnrParams rtxLive = _shared->Snapshot();
    if (vsrRun) {
        std::lock_guard<std::mutex> rtxLock(_evaluateMutex);
        char rtxErr[160]{};
        uint64_t fv = 0;
        if (!_vsr->Evaluate(_d3d12->OutputColor(*slot), width, height,
                            slot->vsrColor.Get(), _pipeW, _pipeH,
                            std::clamp(rtxLive.rtxVsrStrength, kVsrStrengthMin, kVsrStrengthMax),
                            _d3d12->Fence(), slot->baseFenceValue, &fv,
                            rtxErr, sizeof(rtxErr))) {
            if (err && errLen) std::snprintf(err, errLen, "%.180s", rtxErr);
            return false;
        }
        rtxFence = _vsr->Queue().Fence();
        rtxFenceVal = fv;
    }
    if (hdrRun) {
        std::lock_guard<std::mutex> rtxLock(_evaluateMutex);
        char rtxErr[160]{};
        uint64_t fv = 0;
        // TrueHDR 输入 = VSR 输出(vsrRun 链)或 NR 输出(仅 HDR 会话)。
        if (!_hdr->Evaluate(vsrRun ? slot->vsrColor.Get()
                                   : _d3d12->OutputColor(*slot),
                            pipeW, pipeH, slot->hdrColor.Get(),
                            rtxLive.rtxHdrContrast, rtxLive.rtxHdrSaturation,
                            rtxLive.rtxHdrMiddleGray, rtxLive.rtxHdrMaxLuminance,
                            vsrRun ? _vsr->Queue().Fence() : _d3d12->Fence(),
                            vsrRun ? rtxFenceVal : slot->baseFenceValue, &fv,
                            rtxErr, sizeof(rtxErr))) {
            if (err && errLen) std::snprintf(err, errLen, "%.180s", rtxErr);
            return false;
        }
        rtxFence = _hdr->Queue().Fence();
        rtxFenceVal = fv;
    }

    // ---- post 段(fg CL;主队列以 RTX fence 排序,消费管线色)----
    // 内容:RTX 帧的 outputColor 归位 + FG 插帧链(backbuffer = 管线色)
    // + 真实帧输出转换/回读(RTX eval 之后才有管线色,落在本段)。
    // 无 RTX 的帧 = 旧"fg CL"语义(仅 FG 门开帧录制)。插值输出就绪由本槽
    // WaitFrame(fg 栅栏)覆盖(主队列 Wait(RTX fence) 已排在执行前)。
    // backbuffer = 管线色 pipeColor(vsrRun → vsrColor BGRA8 / hdrRun →
    // hdrColor FP16;NGX 写后已衰减 COMMON —— fgBar 显式 COMMON→NSR),
    // MVecs = PIPE 尺寸稠密运动(motionDense;FG 激活时 follow 被忽略),
    // Depth = 静态零纹理(PIPE≠src 用 PIPE 版)。倍数 M:按序 eval 插值槽
    // 1..M-1(官方 MFG 契约),每槽紧随转换 + 独立回读。播种帧只提交首个
    // eval(DLSSG.Reset=1,输出不消费);零光流帧整块跳过。
    const bool postNeeded = rtxIn || fgOnFgCl;
    ID3D12GraphicsCommandList *fgCl = slot->fgCommandList.Get();
    if (rtxIn) {
        // base 尾 outputColor 已 NSR 化作 RTX 输入;NSR 读不衰减,归位 COMMON。
        D3D12_RESOURCE_BARRIER inBack[1]{
            TransitionFromTo(_d3d12->OutputColor(*slot),
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_COMMON),
        };
        fgCl->ResourceBarrier(1, inBack);
    }
    if (fgOnFgCl) {
        const bool fgResetEval = _fg->NeedsReset();
        D3D12_RESOURCE_BARRIER fgBar[1]{
            TransitionFromTo(pipeColor,
                             D3D12_RESOURCE_STATE_COMMON,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        fgCl->ResourceBarrier(1, fgBar);
        // MVecs = PIPE 尺寸稠密运动场(FG 契约 = backbuffer 同尺寸、像素
        // 单位):PIPE≠src 时源尺寸 motion 双线性放大进 motionDense。
        // motion 已 NSR(densify 收尾),SRV 直读;放大后 UAV→NSR 供 eval。
        ID3D12Resource *fgMvec = slot->motion.Get();
        if (slot->motionDense) {
            D3D12_RESOURCE_BARRIER msBar[1]{
                Transition(slot->motionDense.Get(),
                           D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            fgCl->ResourceBarrier(1, msBar);
            _d3d12->RecordMotionScale(*fgCl, *slot, width, height);
            D3D12_RESOURCE_BARRIER msNsr[1]{
                Transition(slot->motionDense.Get(),
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            };
            fgCl->ResourceBarrier(1, msNsr);
            fgMvec = slot->motionDense.Get();
        }
        // Depth = 静态零纹理,尺寸随 backbuffer(PIPE≠src 用 PIPE 版)。
        ID3D12Resource *fgDepth = slot->motionDense ? _d3d12->DepthPipe() : _d3d12->Depth();
        char fgErr[160]{};
        for (int g = 0; g < fgM - 1; ++g) {
            if (fgResetEval && g > 0) break; // 播种帧只建历史,不产插值
            D3D12_RESOURCE_BARRIER toUav[1]{
                Transition(slot->fgInterp.Get(),
                           D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            fgCl->ResourceBarrier(1, toUav);
            if (_fg->Evaluate(fgCl, pipeColor, fgMvec,
                              fgDepth, slot->fgInterp.Get(), pipeW, pipeH,
                              fgM, g + 1, false, fgErr, sizeof(fgErr))) {
                fgRan = true;
                if (fgResetEval) {
                    // 播种帧:插值输出不消费,fgInterp 归 COMMON。
                    D3D12_RESOURCE_BARRIER fgSeed[1]{
                        Transition(slot->fgInterp.Get(),
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COMMON),
                    };
                    fgCl->ResourceBarrier(1, fgSeed);
                    break;
                }
                // 有效插值:fgInterp UAV→NSR 进转换(HDR = FP16→PQ),转换
                // 收尾归 COMMON;回读落第 g 组缓冲(各槽独立,真实帧回读
                // 不被覆写)。
                D3D12_RESOURCE_BARRIER toNsr[1]{
                    Transition(slot->fgInterp.Get(),
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                };
                fgCl->ResourceBarrier(1, toNsr);
                if (_rtxActive) {
                    _d3d12->RecordColorOutput(*fgCl, *slot, slot->fgInterp.Get(),
                                              D3D12Context::kSrvFgInterp, _hdrActive,
                                              pipeW, pipeH, matrix, range,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                } else {
                    _d3d12->RecordYuvOutput(*fgCl, *slot, matrix, range,
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                            slot->fgInterp.Get(), D3D12Context::kSrvFgInterp);
                }
                if (!_d3d12->RecordReadbackCopy(*fgCl, *slot, err, errLen, g)) {
                    return false;
                }
                if (fgGenOk) fgGenOk[g] = true;
                ++fgEvaluatedCount;
                // eval 恢复:解除降级日志闩锁,下次新故障重新记一条。
                _fgDupLogged.store(false, std::memory_order_release);
            } else {
                // eval 失败(会话已被闩停):fgInterp 回 COMMON,本帧全部
                // 插值槽降级复制。
                D3D12_RESOURCE_BARRIER undo[1]{
                    Transition(slot->fgInterp.Get(),
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COMMON),
                };
                fgCl->ResourceBarrier(1, undo);
                // 失败必须进 timing log(dlssfg 内部已留痕;此处补帧号锚点)。
                // 闩锁一次:gate 回落 2x 等场景下每源帧都有槽失败,不闩 =
                // 24fps 源每秒 24 行,根因行被淹没(eval 恢复即解除)。
                if (!_fgDupLogged.exchange(true, std::memory_order_acq_rel)) {
                    char msg[256];
                    std::snprintf(msg, sizeof(msg),
                                  "DLSSNR STATUS: dlssfg frame %d slot %d degraded to dup: %.140s"
                                  " (further dup logs suppressed until recovery)",
                                  n, g + 1, fgErr);
                    TimingStatusLine(msg);
                }
                break;
            }
        }
        // fg CL 尾归位:无 RTX 的 legacy 帧 = 管线色(outputColor)NSR→COMMON
        // (eval 消费过;全败/播种帧同样 NSR 过 —— 统一归位);RTX 帧的
        // 管线色归位由下方真实帧输出转换(RecordColorOutput 收尾)承担。
        if (!rtxIn) {
            D3D12_RESOURCE_BARRIER fgBack[1]{
                TransitionFromTo(pipeColor,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_COMMON),
            };
            fgCl->ResourceBarrier(1, fgBack);
        }
        if (slot->motionDense) {
            D3D12_RESOURCE_BARRIER msBack[1]{
                TransitionFromTo(slot->motionDense.Get(),
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_COMMON),
            };
            fgCl->ResourceBarrier(1, msBack);
        }
        recordGuidancePark(*fgCl);
    }
    if (rtxIn) {
        // 真实帧输出转换(post 段):源 = 管线色(fgOnFgCl 时已被 fgBar 转
        // NSR;否则 NGX 写后衰减 COMMON —— 两种 stateBefore 都由
        // RecordColorOutput 统一 NSR 化),收尾归 COMMON。
        const UINT rtxSrv = pipeColor == slot->hdrColor.Get() ? D3D12Context::kSrvHdrColor
                                                              : D3D12Context::kSrvVsrColor;
        _d3d12->RecordColorOutput(*fgCl, *slot, pipeColor, rtxSrv,
                                  pipeColor == slot->hdrColor.Get(),
                                  pipeW, pipeH, matrix, range,
                                  fgOnFgCl ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                           : D3D12_RESOURCE_STATE_COMMON);
        if (!_d3d12->RecordReadbackCopy(*fgCl, *slot, err, errLen)) {
            return false;
        }
    }
    if (postNeeded) {
        // 主队列 Wait(RTX fence)排在 post CL 执行前 —— 跨队列生产者-消费
        // 者顺序(管线色由专用队列写入后,本段才能读)。
        if (!_d3d12->SubmitFgFrame(*slot, rtxFence, rtxFenceVal, err, errLen)) {
            if (err && errLen) {
                TimingStatusLine(err); // 临时探针
                std::snprintf(err, errLen, "Submit(fg) failed");
            }
            return false;
        }
    } else {
        // 门关帧(面板关/诊断跳过/播种/零光流/fg CL 起点失败)不向 proxy
        // 提交 eval:其内部 backbuffer 历史滞留在跳过前 —— 置位重置让恢复
        // 帧重新播种(播种帧插值输出照常降级复制),否则"关→开"/播种后的
        // 首个 eval 用陈旧历史插出鬼影。逐帧置位无害(布尔位;会话已死时
        // Enabled() 为 false 不进此分支)。无 fg CL 提交:WaitFrame 的目标
        // 栅栏停在 base 值(fg 段 = 0)。
        if (_fg && _fg->Enabled()) _fg->ResetHistory();
        slot->fenceValue = slot->baseFenceValue;
    }
    if (!_d3d12->WaitBaseFrame(*slot, err, errLen)) {
        // A timed-out fence means GPU hang / device removal: stop evaluating,
        // otherwise every later frame stalls the full 10s fence wait again.
        if (_d3d12->IsDeviceLost()) _ready.store(false, std::memory_order_release);
        if (err && errLen) {
            TimingStatusLine(err); // 临时探针
            std::snprintf(err, errLen, "Wait(base) failed");
        }
        return false;
    }
    if (ProbeEnabled()) TimingStatusLine("PROBE: base waited"); // 临时探针(VSDLSSNR_PROBE=1)
    QueryPerformanceCounter(&t3a);
    if (!_d3d12->WaitFrame(*slot, err, errLen)) {
        if (_d3d12->IsDeviceLost()) _ready.store(false, std::memory_order_release);
        if (err && errLen) {
            TimingStatusLine(err); // 临时探针
            std::snprintf(err, errLen, "Wait(fg) failed");
        }
        return false;
    }
    if (ProbeEnabled()) TimingStatusLine("PROBE: waited"); // 临时探针(VSDLSSNR_PROBE=1)
    QueryPerformanceCounter(&t3b);
    const bool rb = _d3d12->UnpackOutput(*slot, dstPlanes, dstStrides, _outW, _outH, err, errLen);
    // FG 插值帧回读(各组独立缓冲;eval 成功的槽才有内容)。失败按整体
    // 失败处理:调用方会对全部输出做源帧复制降级。
    if (rb && fgDstPlanes) {
        for (int g = 0; g < fgM - 1; ++g) {
            if (fgGenOk && fgGenOk[g] &&
                !_d3d12->UnpackOutput(*slot, &fgDstPlanes[g * 3], &fgDstStrides[g * 3],
                                      _outW, _outH, err, errLen, g)) {
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
        // VSDLSSNR_DUMP_SKIP=N:跳过前 N 个真运动帧再 dump(warmup 帧
        // 的流场未成熟,FFX/NVOF A/B 对照用)。
        static std::atomic<int> realCount{ 0 };
        const int myIdx = realMotion ? realCount.fetch_add(1, std::memory_order_relaxed) : -1;
        static const int dumpSkip = [] {
            char b[16]{};
            GetEnvironmentVariableA("VSDLSSNR_DUMP_SKIP", b, sizeof(b) - 1);
            return b[0] ? std::atoi(b) : 0;
        }();
        if (dumpEnabled && realMotion && myIdx >= dumpSkip &&
            !dumpedMotion.load(std::memory_order_relaxed)) {
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
                    if (_ofBackend && _ofBackend->Kind() == kOfBackendNvof &&
                        static_cast<NvofContext *>(_ofBackend.get())->FlowForward()) {
                        NvofContext *nv = static_cast<NvofContext *>(_ofBackend.get());
                        const uint32_t gs = nv->GridSize();
                        // flow 网格按会话输入尺寸(follow 模式 = 内部尺寸,
                        // 与 StageFrame 调用点的 flowW/H 同款公式),不是源
                        // 尺寸 —— 曾按源宽算 dump 维度,与纹理不符。
                        const uint32_t nw = static_cast<uint32_t>(nv->Width());
                        const uint32_t nh = static_cast<uint32_t>(nv->Height());
                        const uint32_t fw = (nw + gs - 1) / gs;
                        const uint32_t fh = (nh + gs - 1) / gs;
                        const bool flowOk = _d3d12->DumpTextureToFile(
                            nv->FlowForward(), static_cast<int>(fw),
                            static_cast<int>(fh), (base / L"dump_flow.bin").c_str(),
                            DXGI_FORMAT_R16G16_SINT);
                        if (!flowOk) TimingStatusLine("DLSSNR STATUS: flow dump FAILED");
                    } else if (_ofBackend && _ofBackend->Kind() == kOfBackendFfx) {
                        // FFX 中间场:_ffxInput(R8G8B8A8 逻辑 RGBA 直写) +
                        // 稀疏流(R16G16_SINT,1/8 OF extent)。
                        auto *fx = static_cast<FxofContext *>(_ofBackend.get());
                        if (!_d3d12->DumpTextureToFile(
                                fx->FfxInput(), static_cast<int>(fx->OfWidth()),
                                static_cast<int>(fx->OfHeight()),
                                (base / L"dump_fxof_input.bin").c_str(),
                                DXGI_FORMAT_R8G8B8A8_UNORM)) {
                            TimingStatusLine("DLSSNR STATUS: fxof input dump FAILED");
                        }
                        if (!_d3d12->DumpTextureToFile(
                                fx->SparseFlow(), static_cast<int>(fx->SparseWidth()),
                                static_cast<int>(fx->SparseHeight()),
                                (base / L"dump_fxof_sparse.bin").c_str(),
                                DXGI_FORMAT_R16G16_SINT)) {
                            TimingStatusLine("DLSSNR STATUS: fxof sparse dump FAILED");
                        }
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
                    if (_ofBackend && nvofInputIndex >= 0) {
                        dumpOrLog(_ofBackend->InputTexture(nvofInputIndex),
                                  _ofBackend->Width(), _ofBackend->Height(),
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
        // 分段 GPU 时间 = base/fg 两次提交的栅栏完成点差(timestamp query
        // 与 NGX 同 CL 会 SEH,见 NOTE;栅栏差分是无损精确拆分):
        // gpu 段 = base CL(NR 推理/残差/直通 + 真实帧 YUV/回读),
        // fg 段 = fg CL(FG 推理 + 插值 YUV/回读);门关帧 fg 段 ≈ 0
        // (WaitFrame 等的就是 base 值,立即满足)。
        const double gpuWaitMs = ms(t2, t3a, qpcFreq);
        const double unpackMs = ms(t3b, t4, qpcFreq);
        const double fgMs = ms(t3a, t3b, qpcFreq);
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
                // 门细分探针为 NvofContext 专属(其它后端无引擎等待)。
                NvofContext *nvProbe =
                    (_ofBackend && _ofBackend->Kind() == kOfBackendNvof)
                        ? static_cast<NvofContext *>(_ofBackend.get()) : nullptr;
                snprintf(line, sizeof(line),
                         "DLSSNR perf: gpu=%.1f ema=%.1f p99=%.1f | pack=%.1f nvof=%.1f/%.1f g%.1f c%.1f e%.1f s%u x%u r%u | eval_cpu=%.1f fg=%.1f unpack=%.1f | slot=%.1f/%.1f lock=%.1f/%.1f | res=%d%% of=%d %dx%d f=%d fps=%.0f",
                         gpuLast, gpuEma, gpuP99, packEma, nvofEma, g_timing.nvof[lastIdx],
                         nvProbe ? nvProbe->LastGateWaitMs() : 0.0,
                         nvProbe ? nvProbe->LastCpyWaitMs() : 0.0,
                         nvProbe ? nvProbe->LastExeWaitMs() : 0.0,
                         nvProbe ? nvProbe->GateSkips() : 0u,
                         nvProbe ? nvProbe->GateExpired() : 0u,
                         nvProbe ? nvProbe->ResetCount() : 0u,
                         evalCpuEma, fgLast, unpackEma,
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
        // stats 每帧发布(六段 last + 共享内存写,开销可忽略):面板
        // "处理用时"随帧呼吸,不再按日志节流跳变。perf 行(磁盘 IO)按时间
        // 门 ≥1s 一行(与帧率无关)。Snapshot/FrameRateWindow 在锁外取
        // (各自持独立互斥,勿在 g_timingMutex 内叠锁)。
        {
            // 排队细分 last(slot/lock 等待)与门累计:诊断页数据源。nvof
            // 探针仅 NVOF 后端存在(FFX 无引擎等待,恒 0);指针读法与上方
            // perf 行同款(仅读,换会话在 PoolHold 内,帧线程读不撕裂)。
            const double slotWaitLast = ms(tSlot0, tSlot1, qpcFreq);
            const double lockWaitLast = ms(tLock0, tLock1, qpcFreq);
            NvofContext *nvStats =
                (_ofBackend && _ofBackend->Kind() == kOfBackendNvof)
                    ? static_cast<NvofContext *>(_ofBackend.get()) : nullptr;
            char body[1016]; // 上限 = StatsPayload.json(1024-8);rtx 键入体后余量收紧
            // FG 状态:on = 本帧有真插值(附当前倍数);dup = 复制真实帧
            // (复位/零光流/面板关/降级);off = 本会话未激活;unavailable =
            // official 初始化或 eval 失败闩停(帧率仍 ×M,内容为复制帧)。
            // fg_mult = 当前倍数(面板显示 "3x" 用;未激活 = 0)。
            // fg_route_eff = 实际生效路由(off/official-hook/official/
            // copy),fg_mult_create = 会话创建倍数 —— "auto 档这次到底
            // 走没走 hook 代理""4x 为什么只跑 2x"面板直读,不翻 timing log。
            const char *fgState = !_fg ? "off"
                : (!_fg->Enabled() ? "unavailable"
                                   : (fgEvaluatedCount > 0 ? "on" : "dup"));
            // fg_mult_max = 运行库插值帧上限(gate 解锁结果定格值):40 系
            // 解锁失败回落 2x 时创建/面板仍报 6,没有它面板无从知道实际
            // 密度只有 (max+1)x。
            const int fgMultMax = _fg ? _fg->MaxGen() : 0;
            snprintf(body, sizeof(body),
                     "{\"%s\":%.1f,\"%s\":%.1f,"
                     "\"%s\":%.1f,\"%s\":%.1f,\"%s\":%.1f,\"%s\":%.1f,"
                     "\"%s\":%d,\"%s\":%d,\"%s\":%d,\"%s\":%d,"
                     "\"%s\":%d,\"%s\":%.1f,\"%s\":\"%s\","
                     "\"%s\":\"%s\",\"%s\":\"%s\",\"%s\":\"%s\",\"%s\":%d,"
                     "\"%s\":\"%s\",\"%s\":%d,\"%s\":\"%s\","
                     "\"%s\":%d,\"%s\":\"%s\","
                     "\"%s\":\"%s\",\"%s\":\"%s\","
                     "\"%s\":%.2f,\"%s\":%.2f,"
                     "\"%s\":%u,\"%s\":%u,\"%s\":%u}",
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
                     SK_FG_MULT, fgM,
                     SK_FG_ROUTE_EFFECTIVE, _fgRouteEff,
                     SK_FG_MULT_CREATE, _fgCreateMult,
                     SK_FG_DETAIL, _fgDetail,
                     SK_FG_MULT_MAX, fgMultMax,
                     SK_OF_DETAIL, _ofDetail,
                     SK_RTX, _rtxStateStr,
                     SK_RTX_DETAIL, _rtxDetail,
                     SK_SLOT_WAIT, slotWaitLast,
                     SK_LOCK_WAIT, lockWaitLast,
                     SK_GATE_SKIPS, nvStats ? nvStats->GateSkips() : 0u,
                     SK_GATE_EXPIRED, nvStats ? nvStats->GateExpired() : 0u,
                     SK_GATE_RESETS, nvStats ? nvStats->ResetCount() : 0u);
            PublishStatsJson(body);
        }
        if (line[0]) TimingLog(line); // outside g_timingMutex (TimingLog locks it)

        if (vsTiming) {
            std::snprintf(timingOut, timingLen, "pack=%.1f,nvof=%.1f,eval_cpu=%.1f,gpu=%.1f,fg=%.1f,unpack=%.1f",
                          packMs, nvofMs, evalOnlyMs, gpuWaitMs, fgMs, unpackMs);
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
    // 会触发驱动访问违例,见 _retiredOf 注释)—— 弃引用,随进程退出
    // 由 OS 回收。宿主重启才是真实的生命周期终点。
    if (_ofBackend) {
        _ofBackend.release();
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
