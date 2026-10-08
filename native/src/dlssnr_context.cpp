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
constexpr char PARAM_RESET[] = "DLSSNR.Reset";
constexpr char PARAM_STYLE[] = "DLSSNR.Style";
constexpr char PARAM_INTENSITY[] = "DLSSNR.Intensity";
constexpr char PARAM_LOCAL_TONE[] = "DLSSNR.LocalToneStrength";
constexpr char PARAM_LOCAL_STRUCTURE[] = "DLSSNR.LocalStructureStrength";
constexpr char PARAM_SKIN_STRUCTURE[] = "DLSSNR.SkinStructureStrength";
constexpr char PARAM_AUTO_MASK[] = "DLSSNR.UseAutoMask";
constexpr char PARAM_UI_CORRECTION[] = "DLSSNR.UICorrection";

void DbgLine(const char *msg) noexcept {
    OutputDebugStringA("vs_dlssnr: ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
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
    // 关闭竞态收口(2026-10-04):enabled 检查在锁外(上方),而
    // SetTimingLogEnabled(false) 是翻标志后才进锁 fclose —— 已过检查的
    // 日志线程会在锁内看到空句柄,把刚关的文件重新打开(违背"关闭以便
    // 删/移文件"的设计目的)。重开路径补一次锁内复查。
    if (!g_timingLogEnabled.load(std::memory_order_relaxed)) return;
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

// PipeLedger::Expect(实现;断言仅留痕,见头注释)。
void PipeLedger::Expect(ID3D12Resource *res, D3D12_RESOURCE_STATES expected,
                        const char *where) noexcept {
    for (int i = 0; i < count_; ++i) {
        if (entries_[i].res == res) {
            if (entries_[i].state != expected && ProbeEnabled()) {
                char msg[160];
                std::snprintf(msg, sizeof(msg),
                              "DLSSNR LEDGER MISMATCH @%s: res=%p state=%u expected=%u",
                              where, (void *)res,
                              static_cast<unsigned>(entries_[i].state),
                              static_cast<unsigned>(expected));
                TimingStatusLine(msg);
            }
            return;
        }
    }
    Set(res, expected); // 首见登记(此前状态未知,以期望值起账)
}

// TS-INLINE 探针开关(VSDLSSNR_TS_INLINE=1):timestamp 内联 NGX 同 CL 的
// A/B 复核(协议见帧路径 NOTE,2026-10-02),平时关。
static bool TsInlineProbeEnabled() noexcept {
    static const bool on = GetEnvironmentVariableA("VSDLSSNR_TS_INLINE", nullptr, 0) != 0;
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

void DlssnrContext::SetCreateParametersUnsafe(const DlssnrParams &createParams) noexcept {
    NVSDK_NGX_Parameter *p = _parameters;
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
    // 参数块重建(Init/RecreateFeature)后 eval 侧 tuning 缓存失效,强制
    // 首帧重写。此处特征创建期无并发 eval,直接置标志。
    _lastEvalTuningValid = false;
}

bool DlssnrContext::SetCreateParametersSafely(const DlssnrParams &createParams, DWORD *sehCode) noexcept {
    return NgxRuntimeGuard::Invoke([&] {
        SetCreateParametersUnsafe(createParams);
        return true;
    }, false, sehCode);
}

void DlssnrContext::SetEvaluateParametersUnsafe(FrameSlot &slot, bool resetHistory, bool realMotion,
                                                const DlssnrParams &params) noexcept {
    NVSDK_NGX_Parameter *p = _parameters;
    // With internal-resolution scaling, NGX consumes/produces the reduced
    // textures; the residual composite then reconstructs the full-size output.
    const bool scaling = _d3d12->HasScaling();
    p->Set(PARAM_COLOR, scaling ? _d3d12->ReducedColor(slot) : _d3d12->InputColor(slot));
    p->Set(PARAM_OUTPUT, scaling ? _d3d12->ReducedDenoised(slot) : _d3d12->OutputColor(slot));
    // 真光流:缩放启用时消费降采样后的运动(内部尺寸,向量已换算到内部
    // 像素单位,MVecScale 保持 1);否则直接消费 densify 输出(源尺寸)。
    // realMotion=false → 静态零纹理(零 guidance 路径)。
    p->Set(PARAM_MVEC, _d3d12->MotionResource(slot, realMotion, scaling));
    p->Set(PARAM_RESET, resetHistory ? 1 : 0);
    // tuning 键脏检查:params 块在 eval 间持久,值不变不重写(首帧/feature
    // 重建后必写;_evaluateMutex 串行,缓存无竞争)—— evaluate 侧写入是
    // mpv 路径 A/B DIFF 实测生效机制(见 SetCreateParametersUnsafe 注释),
    // 这里只省不变值重写,不改变"逐帧侧写入"的生效语义。
    const EvalTuning tuning{params.style, params.intensity, params.localToneStrength,
                            params.localStructureStrength, params.skinStructureStrength,
                            params.useAutoMask ? 1 : 0};
    if (!_lastEvalTuningValid || tuning != _lastEvalTuning) {
        _lastEvalTuning = tuning;
        _lastEvalTuningValid = true;
        p->Set(PARAM_STYLE, tuning.style);
        p->Set(PARAM_INTENSITY, tuning.intensity);
        p->Set(PARAM_LOCAL_TONE, tuning.localTone);
        p->Set(PARAM_LOCAL_STRUCTURE, tuning.localStructure);
        p->Set(PARAM_SKIN_STRUCTURE, tuning.skin);
        p->Set(PARAM_AUTO_MASK, tuning.autoMask);
    }
}

bool DlssnrContext::SetEvaluateParametersSafely(FrameSlot &slot, bool resetHistory, bool realMotion,
                                                const DlssnrParams &params, DWORD *sehCode) noexcept {
    return NgxRuntimeGuard::Invoke([&] {
        SetEvaluateParametersUnsafe(slot, resetHistory, realMotion, params);
        return true;
    }, false, sehCode);
}

// ---- Lifecycle ----

// RTX feature 降级收口:与 2b capability 失败分支同语义(清旗标 + 几何回落
// + _rtxDetail 记因),CreateFeature 失败共用。解耦关键:RTX 段建不起来只
// 停 RTX,资源尚未建时调用,后续 CreateFrameResources 按降级形态自洽 ——
// NR/FG/OF 不连坐(此前 VSR/HDR CreateFeature 失败 = 整体初始化失败,
// 插件纯直通,是最后一个"建不起就全停"的段)。
void DlssnrContext::DegradeRtxFeature(bool vsr, const char *why) noexcept {
    SanitizeJsonDetail(why, _rtxDetail, sizeof(_rtxDetail));
    if (vsr) {
        _vsrRequested = false;
        _vsr.reset(); // 析构 no-op(不重入 NGX),feature handle 随进程回收
        // 几何回落源尺寸(与 2b capability 失败同款;HDR 不缩放不受影响,
        // 若 HDR 存活则回落到 hdr-only 会话的合法形态 pipe=out=源)。
        _pipeW = _width & ~1;
        _pipeH = _height & ~1;
        _outW = _pipeW;
        _outH = _pipeH;
        TimingStatusLine("DLSSNR STATUS: rtx vsr CreateFeature failed; passthrough size");
    } else {
        _hdrActive = false;
        _hdr.reset();
        TimingStatusLine("DLSSNR STATUS: rtx hdr CreateFeature failed; SDR output");
    }
    _rtxActive = _vsrRequested || _hdrActive;
}

// RTX capability 参数块获取(RecreateFeature 形态段 vsr/hdr 两处同构补查
// 的收敛):取块失败留痕并返回 false;成功落 paramsOut(不可用也留块,
// Shutdown 不触碰 —— 与 2b 预检同语义)。
bool DlssnrContext::FetchRtxCapabilityParams(NVSDK_NGX_Parameter **paramsOut,
                                             const char *what) noexcept {
    DWORD sehCode = 0;
    NVSDK_NGX_Parameter *cap = nullptr;
    const NVSDK_NGX_Result r = CoreGetCapabilityParametersSafely(&cap, &sehCode);
    if (sehCode || !NVSDK_NGX_SUCCEED(r) || !cap) {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: rtx %s capability block unavailable", what);
        TimingStatusLine(msg);
        return false;
    }
    *paramsOut = cap;
    return true;
}

// 抗闪烁时域重建三连调用点(冷初始化 3c / RecreateFeature resize / 帧路径
// live 切换)的收敛:成功 = 建账 _curAntiFlicker=af;失败 = 降级 0 +
// failed 发布(state 3 / route 0 / weight 0)+ 降级日志。why 进日志。
bool DlssnrContext::RebuildTemporalOrDegrade(int af, const char *why) noexcept {
    char afErr[160]{};
    if (_d3d12->RebuildTemporal(af, afErr, sizeof(afErr))) {
        _curAntiFlicker = af;
        return true;
    }
    _curAntiFlicker = 0;
    char amsg[288];
    std::snprintf(amsg, sizeof(amsg),
                  "DLSSNR STATUS: temporal %s failed (%s); anti-flicker off", why, afErr);
    DbgLine(amsg);
    TimingStatusLine(amsg);
    _tPubState.store(3, std::memory_order_relaxed);
    _tPubRoute.store(0, std::memory_order_relaxed);
    _tPubWeight.store(0.0f, std::memory_order_relaxed);
    return false;
}

// RTX 几何单一裁决(Initialize 与 RecreateFeature 形态段共用):
//   ratio = 目标高 / 源高;≤ 1.001 → VSR 旁路(只支持放大,对齐浏览器
//   端官方语义;1:1 与缩小场景本就不该付 VSR 的 GPU 价)。
//   pipe  = min(目标, 源×4) —— 官方单 pass 放大上限,超出部分由
//   convert-out 双线性从 4x 中间位补完。
//   hdr   = TrueHDR 在管线(输出几何切换 P10,与 VSR 无关,pipe=src)。
void DlssnrContext::DecideRtxGeometry(const RtxVideoParams &rtx, int srcW, int srcH) noexcept {
    int dstW = srcW, dstH = srcH;
    if (rtx.vsrMode == 1 || rtx.vsrMode == 2) {
        if (rtx.vsrMode == 2) {
            // 手动倍率:输出 = 源 × scale(宽度按源宽高比推)。
            const double sc = std::clamp(rtx.vsrScale, kVsrScaleMin, kVsrScaleMax);
            dstH = static_cast<int>(std::lround(static_cast<double>(srcH) * sc));
            dstW = static_cast<int>(std::lround(static_cast<double>(srcW) * sc));
        } else {
            // vsrAutoHeight <= 0 = 探测失败(无可见窗口):目标=源,
            // ratio=1 走旁路 —— 窗口出现后的链重建重新探测并启用。
            dstH = rtx.vsrAutoHeight > 0
                       ? std::clamp(rtx.vsrAutoHeight, 144, 8192)
                       : srcH;
            const double scale = static_cast<double>(dstH) / static_cast<double>(srcH);
            dstW = static_cast<int>(std::lround(static_cast<double>(srcW) * scale));
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
    const double ratio = static_cast<double>(dstH) / static_cast<double>(srcH);
    _vsrRequested = rtx.vsrMode > 0 && ratio > 1.001;
    _hdrActive = rtx.hdrEnabled != 0;
    if (_vsrRequested) {
        const double cap = static_cast<double>(kVsrMaxScale);
        _pipeH = static_cast<int>(std::lround(static_cast<double>(srcH) *
                                              (std::min)(ratio, cap)));
        _pipeW = static_cast<int>(std::lround(static_cast<double>(srcW) *
                                              (std::min)(ratio, cap)));
        _pipeH = (std::max)(1, _pipeH);
        _pipeW = (std::max)(1, _pipeW);
        _outW = dstW;
        _outH = dstH;
    } else {
        _pipeW = srcW;
        _pipeH = srcH;
        _outW = srcW;
        _outH = srcH;
    }
    // 同上:直通/HDR-only 路径的输出也强制偶尺寸(源本身奇尺寸时
    // newVideoFrame 的色度面行数是 floor,拷贝侧必须与其一致)。
    _outW &= ~1;
    _outH &= ~1;
    _pipeW &= ~1;
    _pipeH &= ~1;
    _rtxActive = _vsrRequested || _hdrActive;
}

void DlssnrContext::RefreshRtxStateString() noexcept {
    if (_vsrRequested && _hdrActive) {
        std::snprintf(_rtxStateStr, sizeof(_rtxStateStr), "vsr+hdr %dx%d", _outW, _outH);
    } else if (_vsrRequested) {
        std::snprintf(_rtxStateStr, sizeof(_rtxStateStr), "vsr %dx%d", _outW, _outH);
    } else if (_hdrActive) {
        std::snprintf(_rtxStateStr, sizeof(_rtxStateStr), "hdr %dx%d", _outW, _outH);
    } else {
        std::snprintf(_rtxStateStr, sizeof(_rtxStateStr), "off");
    }
}

bool DlssnrContext::Initialize(
    D3D12Context &d3d12, const wchar_t *ngxDllPath, const wchar_t *fgDllPath,
    int width, int height, int depth, SharedParams *shared,
    const RtxVideoParams &rtx,
    int subW, int subH, bool rgb,
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
    // GPU 挂起上报接线(设备层 → 面板 stats 通道;传输归宿主层,设备层
    // 不 include panel_ipc —— 2026-10-04 解耦)。
    d3d12.SetHangNotify([](const char *reasonUtf8) noexcept {
        StatsPayload st{};
        st.gpuHang = 1;
        std::snprintf(st.removedReason, sizeof(st.removedReason), "%s",
                      reasonUtf8 ? reasonUtf8 : "");
        PublishStats(st);
    });
    _width = width;
    _height = height;
    _depth = depth;
    _subW = subW;
    _subH = subH;
    _isRgb = rgb;
    _shared.store(shared, std::memory_order_release);
    _rtx = rtx;
    // 全程单一快照(与帧路径 frameParams 同纪律):初始化是数百毫秒的
    // 多步决策链,散点 Snapshot 会在面板中途推送时建出"半新半旧"的缝合
    // 形态,且组合随时机漂移不可复现。入口取一次,全文只读这一份。
    const DlssnrParams pInit = shared->Snapshot();

    // RTX Video 几何换算(单一裁决 helper;capability 预检在资源创建之前 ——
    // 不过/建不起 = 由 DegradeRtxFeature 收口,资源按降级形态建,直通尺寸
    // 零浪费)。
    DecideRtxGeometry(_rtx, width, height);

    // Application data path = snippet directory (mirrors Magpie using its exe dir;
    // the directory must be writable for NGX caches).
    {
        std::filesystem::path dll(ngxDllPath);
        std::error_code ec;
        std::filesystem::path dir = dll.parent_path();
        std::filesystem::create_directories(dir, ec);
        const std::wstring dirStr = dir.wstring();
        if (dirStr.size() >= MAX_PATH) return fail("NGX snippet dir path too long");
        std::memcpy(_appDataPath, dirStr.c_str(), (dirStr.size() + 1) * sizeof(wchar_t));
    }

    // 0) FG hook 代理预载(先于 NGX 核心存在)。0.3.x 的设计路径 = 代理
    // DllMain 挂 LoadLibrary 监视、等核心出现再装钩;"核心先在、代理后进"
    // 的非设计路径下钩子生效极慢(2026-09-22 实测:预载当拍查询 + 250ms×20
    // 轮询全败 0xBAD0000B,+17s seek 后同进程重查才过)。挪到核心初始化前
    // 即走设计路径。纯官方档/FG 未请求不预载。
    // 代理路径留档:Rebind 形态段 FG off→on 时按此补预载(hotMatch 保证
    // 代理路径跨 seek 恒等;进程缓存命中即秒回)。
    if (fgDllPath && fgDllPath[0]) {
        wcsncpy_s(_fgProxyPath, fgDllPath, _TRUNCATE);
    } else {
        _fgProxyPath[0] = L'\0';
    }
    // GPU 族分流:SM86 代理只适用 Turing/Ampere;Ada 及更新走官方链,由
    // dlssfg_context 的 mfg gate 解锁拿多帧(RTX 40 6x)。探测失败 fail-open
    // 维持预载(30 系无损)。
    if (pInit.fgEnabled &&
        std::clamp(pInit.fgRoute, kFgRouteMin, kFgRouteMax) == kFgRouteAuto &&
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
        // 仅 VSR 已请求时才回收几何:vsrCapOk 对 !_vsrRequested 恒 false,
        // 无条件覆盖会把 DecideRtxGeometry 尾部的取偶结果(本文件 558-590 的
        // 堆腐蚀级不变量)打回原始奇尺寸 —— 每条普通 NR 会话都会踩。
        if (_vsrRequested && !vsrCapOk()) {
            TimingStatusLine("DLSSNR STATUS: rtx vsr unavailable (capability); passthrough size");
            std::snprintf(_rtxDetail, sizeof(_rtxDetail),
                          "VSR 不可用(capability;驱动/RTX 显卡/nvngx_vsr.dll)");
            _vsrRequested = false;
            _pipeW = width & ~1;
            _pipeH = height & ~1;
            _outW = width & ~1;
            _outH = height & ~1;
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
    // 模型文件名(诊断页:实际用的原版还是哪档变体)。
    {
        const wchar_t *base = ngxDllPath;
        for (const wchar_t *p = ngxDllPath; *p; ++p)
            if (*p == L'\\' || *p == L'/') base = p + 1;
        WideCharToMultiByte(CP_UTF8, 0, base, -1,
                            _modelDllUtf8, sizeof(_modelDllUtf8), nullptr, nullptr);
    }
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

    // 3b) RTX Video features(VSR → TrueHDR;capability 已在 2b 预检,这里
    //     只做 CreateFeature)。**先于槽资源创建定案会话形态**:失败走
    //     DegradeRtxFeature 降级(清旗标 + 几何回落),资源按降级形态建,
    //     NR/FG 不连坐 —— 此前排在 FG 之后、失败 = 整体初始化失败,是最后
    //     一个"建不起就全停"的段(2026-09-27 解耦收尾)。Rebind 形态重建
    //     复用同一"先建 feature、后建资源"顺序不变量。上下文与尺寸无关,
    //     跨 seek 热复用。
    //     _rtxDetail 只由降级路径写入(2b capability / 本段 Create),
    //     成功路径不触碰 —— 冷初始化成员全新恒空;热重建由形态段按结果
    //     清/写。
    if (_vsrRequested) {
        _vsr = std::make_unique<RtxVsrContext>();
        char rtxErr[192]{};
        if (!_vsr->Initialize(*_d3d12, _vsrParams, _width, _height, rtxErr, sizeof(rtxErr))) {
            DegradeRtxFeature(/*vsr=*/true, rtxErr);
        }
    }
    if (_hdrActive) {
        _hdr = std::make_unique<RtxHdrContext>();
        char rtxErr[192]{};
        if (!_hdr->Initialize(*_d3d12, _hdrParams, _pipeW, _pipeH, rtxErr, sizeof(rtxErr))) {
            DegradeRtxFeature(/*vsr=*/false, rtxErr);
        }
    }

    // 4) Frame resources incl. zero-guidance textures + residual scaling
    //    textures/compute at the internal resolution (skipped entirely when
    //    internal-resolution scaling is disabled)
    if (ProbeEnabled()) TimingStatusLine("PROBE: pre frame-res"); // init 细分(DEVICE_HUNG 时序定位)
    // FG 请求 = 创建时参数快照的 fgEnabled(桥接 ini/payload 采纳已在此前
    // 完成);sticky —— 尺寸重建(RecreateFeature)沿用本旗标,面板运行中
    // 改变只影响逐帧 eval 门,不重建槽资源。
    _fgRequested = pInit.fgEnabled != 0;
    // 实验性补帧 HDR 域插帧(创建时定格;仅 HDR 会话有意义 —— HDR 关时
    // 强制 0,FG create 格式与槽资源恒 SDR 形态,管线等价)。判据用
    // _hdrActive 实际态而非 rtxHdrEnabled 请求:TrueHDR 在 2b/3b 降级后
    // (capability 不过 / CreateFeature 失败)FG 不再携带 PQ 域形态。
    _fgHdrInterp = (pInit.fgHdrInterp != 0) && _hdrActive;
    // FG 会话级事实初值:请求了 = 暂记 copy(初始化成功会被下文改写成
    // 实际路由);没请求 = off。失败原因串在此段内逐路径覆写。
    // M0 不在此暂记:会话边界(SetupFgSession)单点落账,真实值在 4a 段
    // 就位 —— 此前此处按请求预写,初始化若在会话建立前失败,死态 body
    // 会谎报 FG 契约倍数。
    _fgCreateMult.store(0, std::memory_order_relaxed);
    std::snprintf(_fgRouteEff, sizeof(_fgRouteEff), "%s", _fgRequested ? "copy" : "off");
    _fgDetail[0] = '\0';
    // 预载失败归因并入(appendProxyNote)与 FG 会话建立一起迁入
    // SetupFgSession —— 冷初始化与 Rebind 形态重建共用同一条链。
    if (!_d3d12->CreateFrameResources({_width, _height, _depth, _fgRequested,
                                       _pipeW, _pipeH, _outW, _outH,
                                       _vsrRequested, _hdrActive, _fgHdrInterp,
                                       _subW, _subH, _isRgb},
                                      err, errLen)) return failWithExistingErr();
    // 显形记账:管线色格式(">8bit 无 RTX = FP16;VSR-only 10bit =
    // R10G10B10A2")及其回落原因,防"10bit 源怎么不是 FP16"无头案
    // (排查成本一次一行)。R10G10B10A2 附回退开关提示(NR snippet 侧
    // 探针未过前的可观察锚点)。
    {
        const DXGI_FORMAT fmt = _d3d12->ColorFormat();
        const char *name = fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? "RGBA16F"
                         : fmt == DXGI_FORMAT_R10G10B10A2_UNORM ? "R10G10B10A2"
                                                                : "BGRA8";
        char msg[200];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: color buffer %s (depth=%d rtx=%d)%s",
                      name, _depth,
                      (_vsrRequested || _hdrActive) ? 1 : 0,
                      fmt == DXGI_FORMAT_R10G10B10A2_UNORM
                          ? " [revert: VSDLSSNR_NR_FORMAT=bgra8]" : "");
        TimingStatusLine(msg);
    }
    if (pInit.scalingEnabled) {
        const int pct = std::clamp(pInit.inputResolutionPercent, kResPctMin, kResPctMax);
        int iw = _width, ih = _height;
        InternalSize(_width, _height, pct, iw, ih);
        if (!_d3d12->RebuildScaling(iw, ih, err, errLen)) return failWithExistingErr();
    }
    // 3c) 抗闪烁时域资源(上游 antiFlicker,Magpie v0.6.8):mode > 0 时建
    // 历史/引导纹理。冷初始化单线程,直接 RebuildTemporal(不持槽,PoolHold
    // 约束天然满足);失败降级 antiFlicker=0(模式还能 live 再切)。历史状态
    // 随纹理作废。
    {
        const int af = std::clamp(pInit.antiFlicker, kAntiFlickerMin, kAntiFlickerMax);
        if (af > 0 && RebuildTemporalOrDegrade(af, "init")) {
            _tValid = false;
            _tPubState.store(1, std::memory_order_relaxed);
            _tPubRoute.store(af, std::memory_order_relaxed);
        }
    }

    // 4a) DLSS FG 会话(挂 NR 之后;先于 NVOF 建立,使光流 follow 的会话
    // 输入尺寸决策能感知 FG)。失败优雅降级:滤镜回退 1:1 输出(不翻倍
    // 帧率),NR 不受影响 —— FG 的 SEH 走本地闩锁,不进全局 NgxRuntimeGuard。
    // 建立流程 = SetupFgSession(冷初始化与 Rebind 形态重建共用同一条链,
    // 路由/能力块/失败记因不漂移)。
    if (_fgRequested) {
        SetupFgSession(pInit);
    }

    // 4b) 光流会话(of_backend 单一后端,默认 FFX):quality > 0 时建立;
    // 失败优雅回退零 guidance(记 _nvofFailed,不拖垮整个滤镜)。冷初始化
    // 单线程、槽池空闲,满足会话的 PoolHold 约束。档位按后端取对应字段
    // (NVOF→motionVectorQuality,FFX→ffxQuality)。
    {
        const DlssnrParams &snap = pInit;
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
            _ofBackend = CreateOfBackend(ofq, backendReq, _width, _height, ofErr, sizeof(ofErr));
            if (_ofBackend) {
                _curOfBackend = backendReq;
                _ofDetail[0] = '\0';
                char msg[160];
                std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: of session created backend=%d quality=%d %dx%d",
                              _curOfBackend.load(std::memory_order_relaxed), ofq, _width, _height);
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
        // RTX 实态串(缓存进成员:每帧 stats 体复用 —— 每帧体覆盖 init 体
        // 后若缺 rtx 字段,面板诊断恒 "(未加载)"(2026-09-22 实锤)。
        // 刷新点 = init 与 Rebind 形态重建(RefreshRtxStateString)。
        RefreshRtxStateString();
        StatsPayload st{}; // 未携带字段保持零/空;gpuLast 保持 -1 哨兵
        FillStatsCommon(st);
        st.width = static_cast<uint32_t>(_width);
        st.height = static_cast<uint32_t>(_height);
        PublishStats(st);
    }

    // 5) CreateFeature on the control-path command list, then close+execute
    // (Magpie cpp:1720-1758). Initialization is single-threaded (no frame
    // threads yet), the ctl mutex is taken for symmetry with RecreateFeature.
    {
        std::lock_guard<std::mutex> ctlLock(_d3d12->CtlMutex());
        if (!_d3d12->BeginCtlRecording()) return fail("BeginCtlRecording(create) failed");
        {
            DWORD sehCode = 0;
            if (!SetCreateParametersSafely(pInit, &sehCode)) return fail("Create parameter setup raised SEH");
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
        if (!_d3d12->ExecuteCtlAndWait(err, errLen, "nr create"))
            return fail(err && err[0] ? err : "Execute(create) failed");
    }

    _ready = true;
    {
        _appliedCreate = pInit; // 完整建参数存档(Rebind 比对真身,见成员注释)
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

bool DlssnrContext::RecreateFeature(const RecreateRequest &req, char *err, size_t errLen) noexcept {
    // 请求解包(函数体沿用原局部名;哨兵语义在此一次性翻译):
    const int preset = req.preset;
    const int resPercent = req.resPercent;
    const bool scalingEnabled = req.scalingEnabled;
    const int newWidth = req.dims ? req.newWidth : -1;
    const int newHeight = req.dims ? req.newHeight : -1;
    const int newDepth = req.dims ? req.newDepth : -1;
    const RtxVideoParams *const newRtx = req.shape ? &req.rtx : nullptr;
    const int fgReq = req.shape ? (req.fgRequested ? 1 : 0) : -1;
    const int fgHdrReq = req.shape ? (req.fgHdr ? 1 : 0) : -1;
    if (!_ready.load(std::memory_order_acquire) || !_snippetReleaseFeature) {
        if (err && errLen) std::snprintf(err, errLen, "RecreateFeature: context not ready");
        return false;
    }
    // 全程单一快照(同 Initialize pInit / 帧路径 frameParams 纪律):重建
    // 链数百毫秒,散点 Snapshot 会让形态段各取不同时刻的值。create 键以
    // 入参为准(调用方定格传入),本快照服务形态段其余字段 —— 其 create
    // 键与入参恒等(current 侧在消费与传参之间只被 ConsumeRebuild 改写,
    // 而那正是入参的来源)。
    const DlssnrParams p = _shared.load(std::memory_order_acquire)->Snapshot();
    // 重建耗时分解打点(2026-10-02):VSR 切档"复制帧好几秒"的定位数据 ——
    // 各段耗时随 STATUS 行落 timing log,一次复现即可定位大头(排空/RTX
    // 模型加载/槽纹理/NR 特征)。
    LARGE_INTEGER qpf, tTotal, tSeg;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&tTotal);
    tSeg = tTotal;
    auto segMs = [&qpf, &tSeg](void) -> int {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const int ms = static_cast<int>((now.QuadPart - tSeg.QuadPart) * 1000 / qpf.QuadPart);
        tSeg = now;
        return ms;
    };
    // NR 特征跳过判定的格式基线:评估域格式随 RTX 组合变(10bit 消量化,
    // NrColorFormat),格式变了 NGX 按创建格式理解数据,特征必须重建。
    const DXGI_FORMAT nrFmtBefore = _d3d12->ColorFormat();
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
    // Finish 半段在 VS 线程池异步消费裸槽指针(读回映射/fenceEvent/时间戳)。
    // PoolHold 的槽排空只保证"槽已归还",不覆盖归还前的消费窗口:票据未
    // 清零前替换槽资源 = 迟到 Finish 持已释放的读回映射 memcpy =
    // use-after-free(2026-09-26 闪退族:memcpy INVALID_POINTER_WRITE,
    // 复现 = VSR 切换)。事件驱动排空,上限仅防卡死保险 —— 超时响亮
    // 失败,不静默踩堆。
    if (!_d3d12->WaitFinishTicketsDrained(30000)) {
        if (err && errLen) {
            std::snprintf(err, errLen,
                          "RecreateFeature: frame finish tickets not drained (pipeline wedged)");
        }
        TimingStatusLine("DLSSNR STATUS: recreate ABORTED (finish tickets not drained)");
        // 发布死态:重建请求已被 ConsumeRebuild 消费,此路径若只 return
        // false,会话表面存活(面板无感知)而票据已 wedged —— 与
        // RebuildScaling 失败同款收口(2026-10-05 评审修)。
        _ready.store(false, std::memory_order_release);
        return false;
    }
    const int drainMs = segMs();
    // A video-size/shape change replaces every slot's frame resources first —
    // outside CtlMutex because the guidance clear inside takes it itself.
    // All GPU work is complete (slots are only released after WaitFrame).
    // 尺寸变化:bridge preset 路径(传 -1)不触发;Rebind 跨分辨率触发。
    // 形态变化(newRtx/fgReq 提供):Rebind 形态热化(2026-09-27)——
    // hotMatch 放宽后 RTX/FG 档位变化落到这里,不再付冷启动。
    const bool dimsChange = newWidth > 0 &&
                            (newWidth != _width || newHeight != _height || newDepth != _depth);
    const bool shapeChange = newRtx != nullptr || fgReq >= 0;
    const bool resize = dimsChange || shapeChange;
    if (dimsChange) {
        // 先行更新成员尺寸:形态段的几何换算/降级回落、OF 会话内部尺寸
        // 推导都要按新源尺寸。
        _width = newWidth;
        _height = newHeight;
        _depth = newDepth;
    }
    if (newRtx) {
        // RTX 几何随新参数 + (可能变化的)源尺寸一次换算(单一裁决
        // helper,必须先于 CreateFrameResources —— 资源按新几何建)。
        _rtx = *newRtx;
        DecideRtxGeometry(*newRtx, _width, _height);
    } else if (dimsChange) {
        // RTX 几何随源尺寸换算(rtx 未变)。vsr 开:输出目标 _outH 绝对
        // (显示器适配/手动高度),宽按新源宽高比重推,pipe = min(目标,
        // 新源×4);否则 pipe/out 跟随新源(TrueHDR 不缩放;plain-NR 同样
        // 跟随 —— plain-NR 会话换尺寸后 UnpackOutput 按旧 _outW/_outH
        // 回读,潜伏写穿/裁切)。
        if (_vsrRequested) {
            const double ratio = static_cast<double>(_outH) / static_cast<double>(newHeight);
            if (ratio <= 1.001) {
                // 请求门复查(2026-10-05 评审修):此前沿用创建期 _vsrRequested,
                // 新源高超过绝对目标(auto-height 会话 seek 到更高源)时
                // ratio<1 → pipe<src,VSR 按降采样评估(官方只支持放大,
                // RtxVsrContext 内部无钳制)。与 571 行请求门同判据落旁路。
                _vsrRequested = false;
            }
        }
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
            _outW = newWidth;
            _outH = newHeight;
        }
        // 420 输出契约:全形态输出/管线尺寸强制偶(奇高色度面 floor/ceil
        // 错位 = 静默堆腐蚀,2026-09-22 真机实锤)。
        _outW &= ~1;
        _outH &= ~1;
        _pipeW &= ~1;
        _pipeH &= ~1;
    }
    // ---- 形态段(RTX/FG 会话形态热重建)----
    // 顺序不变量与冷初始化一致(2b/3b/4a):几何定案 → capability 补查 →
    // VSR/HDR 上下文建/退(降级不整体失败)→ FG 旗标定格 → 资源 → FG 会话。
    // 全程在 PoolHold + 票据排空之后:fmParallel 下无并发帧触碰这些成员,
    // NGX create 的 ctl 互斥自取。
    if (shapeChange) {
        // capability 补查:2b 只在请求时查询,关→开场景参数块可能从未取过
        // (不可用时也留块,与 2b 同语义)。失败 = 降级收口。
        if (_vsrRequested && !_vsrParams &&
            !FetchRtxCapabilityParams(&_vsrParams, "vsr")) {
            DegradeRtxFeature(/*vsr=*/true, "vsr capability block unavailable");
        }
        if (_hdrActive && !_hdrParams &&
            !FetchRtxCapabilityParams(&_hdrParams, "hdr")) {
            DegradeRtxFeature(/*vsr=*/false, "hdr capability block unavailable");
        }
        // VSR/HDR 上下文建/退:feature 与尺寸无关(热复用键 = 请求态)。
        // SEH 本地闩锁;CreateFeature 失败走 DegradeRtxFeature(清旗标 +
        // 几何回落),NR/FG 不连坐。撤销请求 = 退役(reset 即弃引用:析构
        // no-op 不重入 NGX,feature handle 随进程回收)。
        char rtxErr[192]{};
        if (_vsrRequested && !_vsr) {
            _vsr = std::make_unique<RtxVsrContext>();
            if (!_vsr->Initialize(*_d3d12, _vsrParams, _width, _height, rtxErr, sizeof(rtxErr))) {
                DegradeRtxFeature(/*vsr=*/true, rtxErr);
            }
        } else if (!_vsrRequested && _vsr) {
            _vsr.reset();
        }
        if (_hdrActive && !_hdr) {
            _hdr = std::make_unique<RtxHdrContext>();
            if (!_hdr->Initialize(*_d3d12, _hdrParams, _pipeW, _pipeH, rtxErr, sizeof(rtxErr))) {
                DegradeRtxFeature(/*vsr=*/false, rtxErr);
            }
        } else if (!_hdrActive && _hdr) {
            _hdr.reset();
        }
        // FG 旗标定格(折叠用 RTX 降级后的实际态)。
        if (fgReq >= 0) {
            _fgRequested = fgReq != 0;
        }
        if (fgHdrReq >= 0) {
            _fgHdrInterp = (fgHdrReq != 0) && _hdrActive;
        }
        // FG 会话级事实重算(路由;M0 归 SetupFgSession 会话边界单点,
        // stats tick 复用成员即面板可见)。
        std::snprintf(_fgRouteEff, sizeof(_fgRouteEff), "%s",
                      !_fgRequested ? "off"
                      : (_fg && _fg->Enabled())
                          ? (DlssfgContext::CachedProxyIsHookStyle() ? "official-hook" : "official")
                          : "copy");
        RefreshRtxStateString();
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: hot rebind shape (vsr=%d hdr=%d fg=%d pipe=%dx%d out=%dx%d)",
                      _vsrRequested ? 1 : 0, _hdrActive ? 1 : 0, FgActive() ? 1 : 0,
                      _pipeW, _pipeH, _outW, _outH);
        TimingLog(msg);
    }
    const int shapeMs = segMs();
    if (resize && !_d3d12->CreateFrameResources({_width, _height, _depth, _fgRequested,
                                                 _pipeW, _pipeH, _outW, _outH,
                                                 _vsrRequested, _hdrActive, _fgHdrInterp,
                                                 _subW, _subH, _isRgb},
                                                err, errLen)) {
        _ready.store(false, std::memory_order_release);
        return false;
    }
    // 时域资源随尺寸重建(CreateFrameResources 已作废;失败降级 0,模式
    // 仍可 live 再切)。历史随纹理作废 —— 下帧播种。
    if (resize && _curAntiFlicker > 0) {
        if (RebuildTemporalOrDegrade(_curAntiFlicker, "resize rebuild")) {
            _tValid = false;
            _tPubState.store(1, std::memory_order_relaxed);
            _tPubRoute.store(_curAntiFlicker, std::memory_order_relaxed);
        }
    }
    const int resMs = segMs();
    // NR 特征段跳过判定:CreateFeature 参数块只含几何/质量键(源码验证
    // 2026-10-02:Magpie SetCreateParametersUnsafe 无纹理;两仓 eval 模式
    // 纹理均为每帧传入参数块)—— 纹理对象更换由 eval 参数块天然吸收,
    // 只有 NR 创建键(尺寸/preset/res/scaling/评估域格式)变化才必须重建。
    // 纯 RTX/FG 形态变化(vsrMode/fg 档位)时跳过 ~0.7s 的模型重建
    // (实测日志 nr=617/775ms,是 VSR 切档重建的大头)。
    const DXGI_FORMAT nrFmtAfter = _d3d12->ColorFormat();
    // res 项带 scalingEnabled 前提 = 唯一权威 CreateParamsChanged 本尊
    // (dlssnr_params.h):scaling 关时 res 不进管线,不得单独触发重建 ——
    // 否则"关缩放拖 res 滑块 + 任意形态热化"会白付 ~0.7s 模型重建。
    // 比较走 _appliedCreate 真身(_cur* 三成员已删,三元组规则只在
    // dlssnr_params.h 一处;请求侧仅填 create 键,新增 create 键时默认值
    // 与存档必不等 = 朝重建方向误报,安全侧)。
    DlssnrParams reqCreate{};
    reqCreate.preset = preset;
    reqCreate.scalingEnabled = scalingEnabled ? 1 : 0;
    reqCreate.inputResolutionPercent = resPercent;
    const bool nrKeysChanged = dimsChange ||
                               CreateParamsChanged(reqCreate, _appliedCreate) ||
                               nrFmtBefore != nrFmtAfter;
    if (resize && _fgRequested) {
        if (_fg && _fg->Enabled()) {
            // FG feature 随尺寸/格式重建(proxy Release + CreateFeature;
            // Rebuild 内部键 = 尺寸 + 格式,相同即免)。失败 = FG 闩停,
            // 降级复制帧,NR 不受影响。历史在重建时作废。backbuffer
            // 尺寸/格式 = PIPE/RTX 形态(与冷初始化同判据)。
            char fgErr[192]{};
            if (!_fg->Rebuild(_pipeW, _pipeH,
                              _fgHdrInterp ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                           : DXGI_FORMAT_B8G8R8A8_UNORM,
                              fgErr, sizeof(fgErr))) {
                char msg[288];
                std::snprintf(msg, sizeof(msg),
                              "DLSSNR STATUS: dlssfg rebuild failed (%s); FG off (dup)",
                              fgErr);
                DbgLine(msg);
                TimingStatusLine(msg);
                // 簿记收口(2026-10-05 评审修):与 SetupFgSession 失败路径
                // 对称 —— 此前 _fgCreateMult/_fgDetail 不动,面板仍显示
                // "会话创建 Mx"且无归因,实际 FG 已死降级复制帧。
                SanitizeJsonDetail(fgErr, _fgDetail, sizeof(_fgDetail));
                _fgCreateMult.store(0, std::memory_order_relaxed);
            }
        } else {
            // off→on / 旧会话闩停:重建会话(SetupFgSession 与冷初始化同
            // 一条链)。auto 档先补预载:hotMatch 保证代理路径未变,进程
            // 缓存命中即秒回;首次进托走非设计慢路径有案可查,失败由
            // SetupFgSession 降级 FG off(_fgProxyNote 归因并入),无重试
            // 风暴(seek 边界一次性)。
            if (!DlssfgContext::CachedProxyIsHookStyle()) {
                if (std::clamp(p.fgRoute, kFgRouteMin, kFgRouteMax) == kFgRouteAuto &&
                    _fgProxyPath[0] &&
                    dlssfg_gate::GpuFamilyPrefersProxy()) {
                    char proxyErr[96]{};
                    if (DlssfgContext::PreloadProxyModule(_fgProxyPath, proxyErr, sizeof(proxyErr))) {
                        TimingStatusLine(
                            "DLSSNR STATUS: dlssfg proxy preloaded before capability (shape rebind)");
                    } else {
                        std::snprintf(_fgProxyNote, sizeof(_fgProxyNote), "%s", proxyErr);
                    }
                }
            }
            SetupFgSession(p);
        }
    } else if (resize && !_fgRequested && _fg) {
        // on→off:会话保留(热复用免费,重开无缝;FgActive() 请求感知 →
        // 插件 1:1 输出)。倍数已在形态段归零(纯 FG 关即 shapeChange)。
    }
    // 光流会话随尺寸重建(dims-only;已在 PoolHold 内,内联处理,勿调
    // RebuildOf —— 那会二次取 PoolHold 死锁)。退役旧会话不销毁(见
    // _retiredOf 注释),失败降级零 guidance,不致命。会话输入尺寸遵循
    // follow 语义(scalingEnabled/resPercent 是本次重建的目标态,内部尺寸
    // 由参数直推,不依赖尚未执行的 RebuildScaling)。纯形态变化不在此重建:
    // FG 激活变化改写 follow 语义,由 Rebind 尾部 SyncOfSession 的 stale
    // 检查收口(档位/尺寸失配才重建,无风暴)。
    if (dimsChange && _ofBackend && _curOfQuality > 0 && !_nvofFailed) {
        // FG 激活时 MVecs 契约要求源尺寸稠密运动 —— follow 被忽略。
        // (谓词 = SyncOfSession 单点;scaling 传请求态,纹理尚未重建。)
        const bool foll = OfFollowDesired(p, scalingEnabled);
        int iw = _width, ih = _height;
        if (foll) InternalSize(_width, _height, resPercent, iw, ih);
        char ofErr[160]{};
        auto next = CreateOfBackend(_curOfQuality,
                                    std::clamp(p.ofBackend, kOfBackendMin, kOfBackendMax),
                                    iw, ih, ofErr, sizeof(ofErr));
        {
            // 指针 swap/退役账收口(2026-10-04 评审修):与帧线程 SyncOfSession
            // 的 _nvofMutex 内快照并发 —— 本段持 PoolHold,RebuildOf 是
            // _nvofMutex → PoolHold,嵌 _nvofMutex 会 ABBA 死锁,走专用
            // _ofSwapMutex(偏序末端)。
            std::lock_guard<std::mutex> swapLock(_ofSwapMutex);
            if (next) {
                _retiredOf.push_back(std::move(_ofBackend));
                _ofBackend = std::move(next);
                _curOfBackend = std::clamp(p.ofBackend, kOfBackendMin, kOfBackendMax);
            } else {
                _nvofFailed = true;
                // 尺寸重建失败:旧会话尺寸必然已失配(触发本次重建的原因),
                // 退役它 —— 否则旧会话尺寸项在 ProcessFrame 触发条件里每帧
                // 为真 → RebuildOf 每帧封池风暴(2026-09-25)。降级零 guidance,
                // 重试通道由 _nvofFailed 保留。
                _retiredOf.push_back(std::move(_ofBackend));
                _ofBackend = nullptr;
            }
        }
        if (!next) {
            char msg[288];
            std::snprintf(msg, sizeof(msg),
                          "DLSSNR STATUS: of resize failed (%s); zero guidance", ofErr);
            DbgLine(msg);
            TimingLog(msg);
        }
    }
    // scaling 纹理维护(原在 NR 特征段内,挪出使其独立于该段是否执行):
    // 槽重造后 reduced 系纹理为空(CreateSlotResources 只 Reset 不建),
    // scaling 启用时必须重建;键 = internalW/H(由 resPercent 定),键没变
    // 时重建等价恢复,幂等。无 NGX 调用,与 NR 特征段无顺序耦合。
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
    const int postMs = segMs();
    // ---- NR 特征段(仅创建键变化时执行;纯形态热化跳过)----
    int nrMs = 0;
    if (nrKeysChanged) {
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
    }
    if (!_d3d12->BeginCtlRecording()) {
        if (err && errLen) std::snprintf(err, errLen, "RecreateFeature: BeginCtlRecording failed");
        _ready.store(false, std::memory_order_release);
        return false;
    }
    {
        DWORD sehCode = 0;
        if (!SetCreateParametersSafely(p, &sehCode)) {
            // create 键 = 入口单一快照 p(2026-10-04 评审修):此前函数内自取
            // Snapshot,重建窗口内面板再推 scaling 会让 NGX 键的内部尺寸与
            // 已按入参建好的纹理错配一帧。
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
    if (!_d3d12->ExecuteCtlAndWait(err, errLen, "nr recreate")) {
        _ready.store(false, std::memory_order_release);
        return false;
    }
    nrMs = segMs();
    } // end nrKeysChanged
    // c9b5d9e 曾在此残留一份无守卫的 CreateFeature 拷贝:每次重建多建一个
    // 特征覆盖 _feature 句柄(旧特征泄漏,显存只涨不降)—— 已删,创建仅
    // 存在于上方 nrKeysChanged 段。
    const int totalMs = static_cast<int>((tSeg.QuadPart - tTotal.QuadPart) * 1000 / qpf.QuadPart);
    char msg[224];
    snprintf(msg, sizeof(msg),
             "DLSSNR STATUS: preset=%d res=%d%% scaling=%d internal=%dx%d d=%d %s%s frame=%d "
             "rebuild[drain=%d shape=%d res=%d post=%d nr=%d total=%dms]",
             preset, scalingEnabled ? resPercent : 100, scalingEnabled,
             _d3d12->InternalWidth(), _d3d12->InternalHeight(), _depth,
             nrKeysChanged ? "feature recreated" : "feature reused (hot shape)",
             resize ? " (size changed)" : "", _lastFrameN.load(std::memory_order_relaxed),
             drainMs, shapeMs, resMs, postMs, nrMs, totalMs);
    DbgLine(msg);
    TimingLog(msg);
    // 完整建参数存档(覆盖冷初始化值;三个调用点共享本收口点)。create 键
    // 以本次入参显式覆写落账 —— 此前依赖"入口快照 p 的 create 键恒 ==
    // 入参"的跨函数时序论证(只靠注释成立),存档即所建,不再需要该前提。
    _appliedCreate = p;
    _appliedCreate.preset = preset;
    _appliedCreate.scalingEnabled = scalingEnabled ? 1 : 0;
    _appliedCreate.inputResolutionPercent = resPercent;
    return true;
}

std::unique_ptr<IOpticalFlowBackend> DlssnrContext::CreateOfBackend(
    int q, int backendReq, int dstW, int dstH, char *err, size_t errLen) noexcept {
    // of_backend = 单一后端(用户裁定 2026-09-21:默认 FFX,跨厂商通用;
    // 上游 Magpie 同款 RTX 5070 Ti 上 AMD OF 实测有流)。仅 1 = nvof 时
    // 走 NVOF(NVOF 引擎本身 NVIDIA 专属,内部厂商门会拦截)。失败 =
    // 零 guidance,不跨后端回落(对齐 fg_route 先例:行为可预测)。
    // backendReq 由调用方从其快照传入(2026-10-04:此处曾自取 Snapshot,
    // 与调用方 stale 裁决用的快照可差一次面板推送,建出"档位旧 + 后端新"
    // 的缝合会话)。
    if (backendReq == kOfBackendNvof) {
        auto b = std::make_unique<NvofContext>();
        if (!b->Initialize(*_d3d12, dstW, dstH, q, err, errLen)) return nullptr;
        return b;
    }
    auto b = std::make_unique<FxofContext>();
    if (!b->Initialize(*_d3d12, dstW, dstH, q, err, errLen)) return nullptr;
    return b;
}

bool DlssnrContext::SetupFgSession(const DlssnrParams &p) noexcept {
    // FG 路由选择(0=自动:预载 0.3.x hook 代理后走官方链;1=纯官方:
    // 不预载代理直连官方运行时)。钉档失败不回落(选错档 = FG 关,
    // 输出 1:1)。参数 = 调用方快照(单一快照纪律:此处曾自取 Snapshot,
    // 与调用方裁决所用的快照可差一次面板推送)。
    const int fgRoute =
        std::clamp(p.fgRoute, kFgRouteMin, kFgRouteMax);
    // 官方 NGX 链(PORTING #8):官方签名 nvngx_dlssg.dll 与模型 DLL
    // 同目录(ngx\)部署,经共享 NGX core 走官方签名链 —— 免自签
    // proxy 与杀软误报面。参数块 = GetCapability(官方 DLSSG 的 Magpie
    // 同款 create/eval 块)。RTX 30/20 的 DLSS-G 由 dlssg_for_sm86
    // 0.3.x hook 代理接管交付(自动档在冷初始化 stage 0 / 形态段预载),
    // 链路形态不变。
    wchar_t officialDll[MAX_PATH]{};
    {
        const std::filesystem::path p =
            std::filesystem::path(_appDataPath) / L"nvngx_dlssg.dll";
        const std::wstring ws = p.wstring();
        if (ws.size() < MAX_PATH) {
            const DWORD attr = GetFileAttributesW(ws.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                std::memcpy(officialDll, ws.c_str(), (ws.size() + 1) * sizeof(wchar_t));
            }
        }
    }
    // 预载失败归因并入:官方链失败时 _fgDetail 只描述官方链自身,真因
    // (proxy 没进托)在 _fgProxyNote —— 拼接后面板一行直读完整因果,
    // 不必再翻 timing log 反推。
    const auto appendProxyNote = [&]() noexcept {
        if (!_fgProxyNote[0] || !_fgDetail[0]) return;
        const size_t len = std::strlen(_fgDetail);
        std::snprintf(_fgDetail + len, sizeof(_fgDetail) - len, "; %s", _fgProxyNote);
    };
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
            // 会话重建全程不持 _fgMutex(Initialize 毫秒级,锁内会挡帧线程
            // 入口读点);指针进/出成员的两个赋值点各自锁内(2026-10-04 评审
            // 修)—— 尝试期间旧实例(如有)保持存活,锁内读点看到的一律是
            // 完整对象。
            auto fgNext = std::make_unique<DlssfgContext>();
            char fgErr[256]{};
            // FG backbuffer = 管线色:**默认恒 BGRA8 SDR 域**(TrueHDR
            // 后置:DLSSG 的 HDR 路径对 >1.0 线性值不保真,2026-09-23
            // 实验定案;插值在 SDR 域,逐输出帧 TrueHDR 提升)。实验开关
            // fg_hdr_interp=1 = FP16 backbuffer 载 **PQ 码域**(TrueHDR
            // 产物经 HdrToPq 编码,ColorBuffersHDR=0;DLSSG 在感知域插帧
            // —— 直吃 scRGB 线性 >1.0 会被钳 ~0.875,值域定案 2026-09-24),
            // 尺寸 = PIPE。
            if (fgNext->Initialize(*_d3d12, officialDll, _fgParams,
                                   _pipeW, _pipeH,
                                   _fgHdrInterp ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                : DXGI_FORMAT_B8G8R8A8_UNORM,
                                   fgErr, sizeof(fgErr))) {
                {
                    std::lock_guard<std::mutex> fgLock(_fgMutex);
                    _fg = std::move(fgNext); // 旧实例(热复用失败残留)在此销毁
                }
                fgUp = true;
                // 钩子进程级、装上不可拆:只要缓存模块是 hook 型就标
                // official-hook(与本次是否预载解耦 —— 上会话自动档
                // 预载后,本会话纯官方在 30/20 系实际仍是 hook 在托)。
                std::snprintf(_fgRouteEff, sizeof(_fgRouteEff), "%s",
                              DlssfgContext::CachedProxyIsHookStyle() ? "official-hook" : "official");
                // 成功清因(头文件契约本就如此写,2026-10-04 起兑现):
                // 瞬时失败自愈后不留过期原因串(面板会同时看到存活的
                // of/fgMode 和过期的 detail)。
                _fgDetail[0] = '\0';
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
                {
                    std::lock_guard<std::mutex> fgLock(_fgMutex);
                    _fg.reset(); // 旧实例(如有)销毁;fgNext 局部出作用域自毁
                }
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
    }
    // M0 落账单点(2026-10-04 收拢):会话边界一处同步建活/失败与簿记。
    // 此前四写点散布(Initialize 暂记/形态段/Rebind/本函数失败路径),
    // 真正建活的这一处反而不写 —— off→on 热重建(Rebind 簿记时 _fg 尚空)
    // 后面板 M0 恒 0,"需重建"红显判定被自己的簿记骗了。Rebind 的写点
    // 保留(新实例契约锚:hot-reuse 路径不进本函数,倍数非 create 键也
    // 随 seek 换实例刷新)。
    _fgCreateMult.store((fgUp && _fgRequested)
                            ? std::clamp(p.fgMultiplier, kFgMultMin, kFgMultMax)
                            : 0,
                        std::memory_order_relaxed);
    return fgUp;
}

bool DlssnrContext::OfFollowDesired(const DlssnrParams &p, bool scalingActive) const noexcept {
    std::lock_guard<std::mutex> fgLock(_fgMutex);
    const bool fgLive = _fg && _fg->Enabled() && p.fgEnabled != 0;
    return p.nvofFollowScaling != 0 && scalingActive && !fgLive;
}

bool DlssnrContext::SyncOfSession(const DlssnrParams &p, int srcW, int srcH,
                                  bool allowRetry, char *err, size_t errLen) noexcept {
    // 光流会话同步的单一裁决点:此前三份拷贝(Rebind 热路径/RecreateFeature
    // 补齐/ProcessFrame)已互相漂移 —— 前两份含 _nvofFailed 重试项,帧路径
    // 不含。统一在此,帧路径经 allowRetry=false 保持防风暴不对称。
    const bool follow = OfFollowDesired(p, _d3d12->HasScaling());
    const int nvW = follow ? _d3d12->InternalWidth() : srcW;
    const int nvH = follow ? _d3d12->InternalHeight() : srcH;
    const int backendReq = std::clamp(p.ofBackend, kOfBackendMin, kOfBackendMax);
    const int ofq = ResolveOfQuality(p); // clamp 在 helper 内(按后端值域)
    // 簿记快照在 _nvofMutex 内捕获(2026-10-04 评审修):裸读 _ofBackend 与
    // 并发 RebuildOf 的 move 是数据竞争(裸指针 UB)。捕获后即使 move 已发生
    // 也安全 —— 退役对象进程期存活(_retiredOf 哲学),悬空不可能;裁决
    // 陈旧由 RebuildOf 锁内复查兜住(后来者跳过)。
    int curQ = 0, curB = 0;
    bool nvofFailed = true;
    IOpticalFlowBackend *of = nullptr;
    {
        std::lock_guard<std::mutex> switchLock(_nvofMutex);
        curQ = _curOfQuality;
        curB = _curOfBackend;
        nvofFailed = _nvofFailed;
        // 指针本体读取收口(2026-10-04 评审修):与 RecreateFeature 尺寸段
        // (PoolHold → _ofSwapMutex)并发,嵌套 _ofSwapMutex(偏序末端)。
        std::lock_guard<std::mutex> swapLock(_ofSwapMutex);
        of = _ofBackend.get();
    }
    bool stale = ofq != curQ || backendReq != curB ||
                 (ofq > 0 && of && (of->Width() != nvW || of->Height() != nvH));
    if (!stale && ofq > 0 && allowRetry && nvofFailed) stale = true;
    if (!stale) return true;
    return RebuildOf(ofq, backendReq, nvW, nvH,
                     err, errLen); // 失败仅降级零 guidance(调用方语义)
}

bool DlssnrContext::RebuildOf(int quality, int backendReq, int dstW, int dstH,
                              char *err, size_t errLen) noexcept {
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
    // 会话账(_ofBackend/_retiredOf)swap 锁在 PoolHold 之内取(下方)。
    // 2026-10-05 评审修:此前 _ofSwapMutex 先于 PoolHold 获取,与
    // RecreateFeature 尺寸段(PoolHold → _ofSwapMutex)构成 ABBA —— A 线程
    // 持 swap 等池、B 线程持池等 swap,fmParallel 下挂死。全局偏序恒为
    // _nvofMutex → PoolHold → _ofSwapMutex;本函数此前快路径/quality=0 段
    // 的 _ofBackend 读改为主持 swap 保护,现与 SyncOfSession 快照同款裸读
    // (原子指针读 + 退役对象永生,最坏过期决策,池内重建分支在锁内复核)。
    // 调用方传入的档位已经 ResolveOfQuality 按后端值域 clamp;此处宽 clamp
    // 仅作双保险(FFX 的 2 也在 0-5 内,不受影响)。
    const int q = std::clamp(quality, kOfQualityMin, kOfQualityMax);
    // backendReq = 调用方快照(单一快照纪律:函数内自取会在 SyncOfSession
    // 裁决与重建之间混入更新的面板推送,瞬态"旧档位配新后端",2026-10-04
    // 评审修;2026-10-04 该纪律在 CreateOfBackend 落地时漏了本函数)。
    backendReq = std::clamp(backendReq, kOfBackendMin, kOfBackendMax);
    if (q == _curOfQuality && backendReq == _curOfBackend && !_nvofFailed && _ofBackend &&
        _ofBackend->Enabled() &&
        _ofBackend->Width() == dstW && _ofBackend->Height() == dstH) return true;
    if (q == 0) {
        // 保留会话仅停用:后端销毁不可靠(NVOF 引擎 destroy 实测崩溃,
        // FFX 首期同策略走 _retiredOf);空闲会话无 GPU 开销,与热上下文
        // 同哲学,进程退出统一回收。**不封池**:本分支不触碰任何 GPU 资源
        // (此前在 PoolHold 内,配合下方 _curOfBackend 缺同步 bug,后端
        // 切换 + quality=0 曾逐帧封池排空三槽 = 78-165ms 级停顿 ×149 帧,
        // 2026-09-25 真机实锤)。
        // _curOfBackend 语义 = "最近已处理的请求后端"(请求级),不是
        // "现存会话的后端"(会话级,由 _ofBackend->Kind() 表达)。缺此
        // 同步,后端切换后 SyncOfSession 每帧看见 backendReq !=
        // _curOfBackend → 每帧重建风暴(同上)。将来 quality>0 回来时,
        // 下方 else-if 由 Kind != backendReq 兜住真正重建。
        _nvofFailed = false;
        _curOfQuality = 0;
        _curOfBackend = backendReq;
        TimingStatusLine("DLSSNR STATUS: of disabled (quality=0; session kept)");
        return _ofBackend && _ofBackend->Enabled();
    }
    {
        D3D12Context::PoolHold pool(*_d3d12);
        // Finish 在途票据排空:与 RecreateFeature 对称(见其 PoolHold 后
        // 注释)。现状安全依赖"ReleaseSlot 是 Finish 最后一次槽资源访问"
        // 这一跨函数约定 —— 显式排空消除对约定的隐式依赖。
        if (!_d3d12->WaitFinishTicketsDrained(30000)) {
            if (err && errLen) {
                std::snprintf(err, errLen,
                              "RebuildOf: frame finish tickets not drained (pipeline wedged)");
            }
            // 请求落账(承认已处理)+ 亮 _nvofFailed:否则 _curOfQuality
            // 未更新 → SyncOfSession stale 恒真 → 逐帧封池 + 各等 30s 活锁
            // (2026-10-05 评审修)。面板经 nvof_zero 可见,重试经 Rebind。
            _nvofFailed = true;
            _curOfQuality = q;
            _curOfBackend = backendReq;
            TimingStatusLine("DLSSNR STATUS: of rebuild ABORTED (finish tickets not drained)");
            return false;
        }
        // 会话账锁:PoolHold 之内取(偏序末端,见本函数头部 2026-10-05 修
        // 注)。覆盖重建裁决 + swap 全段 —— 此前裁决在锁外,与并发 swap 的
        // 竞争窗口在锁内复核闭合。
        std::lock_guard<std::mutex> swapLock(_ofSwapMutex);
        if (!_ofBackend || _nvofFailed || !_ofBackend->Enabled() ||
            _ofBackend->Quality() != q || _ofBackend->Width() != dstW ||
            _ofBackend->Height() != dstH || _ofBackend->Kind() != backendReq) {
            // 退役名单全等复用:Kind/档位/尺寸一致 = 会话即所需(反复拖
            // 光流质量滑条不再线性累积显存;名单上限哲学见 _retiredOf 注释)。
            // 只捞存活会话 —— 轮转池超时退役的死会话(_ready=false)留在
            // 名单里(它们本就是"销毁不安全"的弃置体)。捞回即复位历史,
            // 下一帧重新播种。当前会话(如有)照常退役。
            std::unique_ptr<IOpticalFlowBackend> revived;
            for (auto it = _retiredOf.begin(); it != _retiredOf.end(); ++it) {
                if ((*it)->Enabled() && (*it)->Kind() == backendReq &&
                    (*it)->Quality() == q &&
                    (*it)->Width() == dstW && (*it)->Height() == dstH) {
                    revived = std::move(*it);
                    _retiredOf.erase(it);
                    revived->ResetHistory();
                    break;
                }
            }
            // 先建新会话再弃旧(旧会话仅弃引用,不销毁 —— 见 _retiredOf 注释)。
            // 弃旧的 GPU 资源随 PoolHold 排空后不再被引用,纹理显存由驱动
            // 按引用回收(对象随进程生存)。
            auto next = revived ? std::move(revived)
                                : CreateOfBackend(q, backendReq, dstW, dstH, err, errLen);
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
                // 新会话建立失败且旧会话尺寸已失配当前尺寸:退役旧会话
                // (弃引用不销毁,同 _retiredOf 语义)。否则旧会话尺寸项在
                // ProcessFrame 的重建触发条件里每帧为真 → 每帧 PoolHold 封池
                // + 重建尝试风暴(2026-09-25)。尺寸仍匹配的失败(纯档位切换
                // 失败)保留旧会话继续复用,降级语义由 _nvofFailed 表达。
                if (_ofBackend &&
                    (_ofBackend->Width() != dstW || _ofBackend->Height() != dstH)) {
                    _retiredOf.push_back(std::move(_ofBackend));
                    _ofBackend = nullptr;
                }
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
            // 尺寸/档位变化由上方的条件分支兜住;后端变化同样由
            // Kind != backendReq 兜住(能落到本分支 = 会话后端与请求一致)。
            _ofBackend->ResetHistory();
            _nvofFailed = false;
            _curOfQuality = q;
            _curOfBackend = backendReq;
        }
    }
    if (_ofBackend && _ofBackend->Enabled()) {
        char msg[160];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: of rebuilt backend=%d quality=%d %dx%d",
                      _curOfBackend.load(std::memory_order_relaxed),
                      _curOfQuality.load(std::memory_order_relaxed), dstW, dstH);
        DbgLine(msg);
        TimingLog(msg);
        // 成功清因(与 _fgDetail 同契约,2026-10-04 兑现):瞬时失败自愈后
        // 不留过期原因串,面板不再同时看到存活会话与过期 detail。
        _ofDetail[0] = '\0';
    }
    return _ofBackend && _ofBackend->Enabled();
}

void DlssnrContext::ResetNvofHistory() noexcept {
    // seek = 新时间线:流历史作废,下一帧重新播种(清零发布 + NGX PARAM_RESET;
    // FG 下一帧 eval 带 DLSSG.Reset,该帧插值输出降级复制)。会话本身保留
    // (热上下文跨 seek 存活)。
    {
        // 指针解引用收口(2026-10-05 评审修):此前裸读 _ofBackend,与帧线程
        // RebuildOf/Rebind 内联段的 swap(PoolHold → _ofSwapMutex)并发 =
        // 数据竞争。只取偏序末端锁(本函数调用点不持 _nvofMutex/池)。
        std::lock_guard<std::mutex> swapLock(_ofSwapMutex);
        if (_ofBackend) _ofBackend->ResetHistory();
    }
    {
        // Rebind 热复用分支不持池(1816 注),与帧线程写侧(PoolHold 内
        // SetupFgSession)并发 —— 指针解引用锁内(2026-10-04 评审修)。
        std::lock_guard<std::mutex> fgLock(_fgMutex);
        if (_fg) _fg->ResetHistory();
    }
}

bool DlssnrContext::Rebind(SharedParams *shared, int width, int height, int depth,
                           const RtxVideoParams &rtx, char *err, size_t errLen) noexcept {
    if (!_ready.load(std::memory_order_acquire) || !_snippetReleaseFeature) {
        if (err && errLen) std::snprintf(err, errLen, "Rebind: context not ready");
        return false;
    }
    _shared.store(shared, std::memory_order_release);
    const DlssnrParams p = shared->Snapshot();
    // FG 会话创建倍数随每次 bind 重新落定 = 新滤镜实例的输出契约 M0
    // (面板 payload 已先于此被新实例采纳,见下)。此前只写于 Initialize:
    // 档位不属 CreateParamsChanged 三元组、换档恒走热复用,stats 里
    // fg_mult_create 永远停在进程启动值 —— 面板"会话创建 Xx"红显永
    // 不清除(2026-09-25 用户实测定案)。
    bool fgLiveForM0 = false;
    {
        std::lock_guard<std::mutex> fgLock(_fgMutex); // 无池读侧收口(2026-10-04 评审修)
        fgLiveForM0 = _fg && _fg->Enabled();
    }
    _fgCreateMult.store((p.fgEnabled != 0 && fgLiveForM0)
                            ? std::clamp(p.fgMultiplier, kFgMultMin, kFgMultMax)
                            : 0,
                        std::memory_order_relaxed);
    // Snapshot already carries the ini overrides and the panel-payload adopt
    // the new filter instance loaded (BridgeLoadIni + BridgeAdoptPanelPayload
    // run in DlssnrCreate before this). Only a real create-time change needs
    // the feature rebuilt; a matching hot context keeps the NGX feature
    // completely warm across mpv's seek-triggered script re-initialization.
    // A different video size rebuilds frame resources + feature inside the
    // same pool-sealed RecreateFeature pass, so the ~1s bring-up survives
    // resolution changes too. 比对用 _appliedCreate 真身(上次成功建/重
    // 模时的完整参数快照),不再伪造三字段 cur。
    const bool dimsChanged = width != _width || height != _height || depth != _depth;
    // 形态变化(RTX 参数 / FG 请求 / FG HDR 折叠态):热复用 + 会话形态
    // 重建(RecreateFeature 形态段),NR NGX feature 不额外重建 —— 冷启动
    // (设备/核心/snippet 重载)不再发生(2026-09-27 解耦收尾)。fg_hdr_interp
    // 折叠基准 = _hdrActive 实际态(与 Initialize/RecreateFeature 同一基准
    // —— 此前用请求侧 rtxHdrEnabled,TrueHDR 降级会话(_hdrActive=false)
    // 下请求恒"想建",每次 Rebind 都误判 shapeChanged,热复用分支永不可达,
    // 每次 seek 全量排空+重建且永不收敛,2026-10-04 评审修)。纯 HDR 域内
    // 翻转由 rtx != _rtx 兜住(hdrEnabled ∈ RtxVideoParams)。
    const bool fgHdrDesired = (p.fgHdrInterp != 0) && _hdrActive;
    const bool shapeChanged = rtx != _rtx ||
                              (p.fgEnabled != 0) != (_fgRequested != 0) ||
                              fgHdrDesired != (_fgHdrInterp != 0);
    if (!dimsChanged && !shapeChanged && !CreateParamsChanged(p, _appliedCreate)) {
        // 热复用:NGX feature 保持,但 seek 是新时间线 —— 光流历史必须
        // 作废(下一帧播种),光流档位同步到新实例的参数快照;此前建立
        // 失败的会话在热复用时重试一次。FG 同理(下一帧 eval 带 Reset,
        // 该帧插值输出降级复制)。
        ResetNvofHistory();
        // seek = 新时间线,抗闪烁历史同样作废(_tLastFrame 与新帧序失配本会
        // 触发播种式重置,显式复位让首帧就走 weight=0 播种,不留歧义)。
        // 模式变化的资源重建不在此做:ProcessFrame 的逐帧同步持 PoolHold
        // 兜住,热复用分支不持池,直接重建会在在飞帧脚下换纹理。
        {
            // 热复用分支不持池,旧实例在飞帧可在 eval 域(_evaluateMutex 内
            // 读写 _t* 时域线)与本写并发 —— 同锁互斥收口;锁序
            // lifecycle → evaluate 单向,无环(2026-10-05 评审修)。
            std::lock_guard<std::mutex> evalLock(_evaluateMutex);
            _tValid = false;
        }
        // 会话输入尺寸同步(follow = 开关 + scaling 状态 + FG 未激活;热路
        // 径下内部尺寸未变,除非 ini/payload 同时带了 res% 变化 —— 那会走
        // 下方重建分支)。档位按后端取对应字段。单一裁决点见 SyncOfSession。
        // err 用本地暂存:OF 降级失败(仅零 guidance)不得污染 rebind 错误串
        //(2026-10-04 评审修)。
        char ofErr[160]{};
        SyncOfSession(p, _width, _height, /*allowRetry=*/true, ofErr, sizeof(ofErr));
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: hot rebind kept feature (preset=%d res=%d%% scaling=%d of=%d %dx%dd%d internal=%dx%d)",
                      _appliedCreate.preset,
                      _appliedCreate.scalingEnabled ? _appliedCreate.inputResolutionPercent : 100,
                      _appliedCreate.scalingEnabled != 0, _curOfQuality.load(std::memory_order_relaxed),
                      _width, _height, _depth, _d3d12->InternalWidth(), _d3d12->InternalHeight());
        DbgLine(msg);
        TimingLog(msg);
        return true;
    }
    RecreateRequest recreate;
    recreate.preset = p.preset;
    recreate.resPercent = p.inputResolutionPercent;
    recreate.scalingEnabled = p.scalingEnabled != 0;
    recreate.dims = dimsChanged;
    recreate.newWidth = width;
    recreate.newHeight = height;
    recreate.newDepth = depth;
    recreate.shape = shapeChanged;
    recreate.rtx = rtx;
    recreate.fgRequested = p.fgEnabled != 0;
    recreate.fgHdr = fgHdrDesired;
    const bool nvofOk = RecreateFeature(recreate, err, errLen);
    if (!nvofOk) {
        // 重建失败 = 会话死态:契约锚(本实例"会话创建 Xx")清零 —— 残留
        // M0 会让死态面板谎报一个死会话从未兑现的倍数契约(2026-10-04 评审修)。
        _fgCreateMult.store(0, std::memory_order_relaxed);
        return false;
    }
    // 新实例的参数快照可能换了光流档位(面板 seek 前调过):RecreateFeature
    // 只处理尺寸,档位变化在这里补齐。会话输入尺寸同样在此对齐(follow 开
    // 关/res% 变化)。单一裁决点见 SyncOfSession(err 用本地暂存:失败仅
    // 降级零 guidance,不得污染调用方的 rebind 错误串)。
    //
    // 重载 = reseek = 新时间线:光流/FG 历史必须作废。此前只写于热复用分支
    // —— vsr 形态变化时 OF 会话原样保留(dimsChange=false 不触发重建,
    // "历史已在会话(重)建时作废"的假设对它不成立),帧序门残留旧时间线的
    // _nextSeq;重载后 mpv 从帧 0 重新送帧,每帧被判迟到 → 播种式复制帧爬
    // 满旧时间线长度(2026-10-02 门打点实锤:next=200,streak 爬到 188 才自愈)。
    {
        char ofErr[160]{};
        ResetNvofHistory();
        SyncOfSession(p, _width, _height, /*allowRetry=*/true, ofErr, sizeof(ofErr));
    }
    return true;
}

// 拆分执行续体:ProcessFrame(Submit 半段)成功后把等待/unpack/stats 所需
// 的局部状态打包于此;ProcessFrameFinish(Finish 半段,调用方已释放其串行
// 锁)消费。slot 所有权随打包移交(Submit 半段的 SlotGuard 让位)。
struct FrameFinish {
    FrameSlot *slot = nullptr;
    LARGE_INTEGER qpcFreq{}, t0{}, t1{}, t2{}, t3a{}, tSub0{}, tSub1{},
                  tSlot0{}, tSlot1{}, tLock0{}, tLock1{};
    ID3D12Fence *vsrDoneFence = nullptr;
    uint64_t vsrDoneVal = 0;
    ID3D12Fence *hdrDoneFence = nullptr;
    uint64_t hdrDoneVal = 0;
    bool fgBeginOk = false;
    bool nrOff = false;
    bool skipEval = false;
    bool hdrPostSplit = false;
    bool hdrRun = false;
    bool dumpEnabled = false;
    bool realMotion = false;
    bool rbOk = false; // 真实帧回读成功(UnpackOutput);dump latch 门(防废帧烧一次性 dump)
    bool evalZeroed = false;
    bool rtxIn = false;
    bool ofNeeded = false;
    bool fgRan = false;
    int fgM = 0;
    int fgEvaluatedCount = 0;
    int nvofInputIndex = -1;
    double nvofMs = 0.0;    // OF 提交成本(eval_cpu 窗口内扣减用)
    double nvofSpanMs = 0.0; // OF 全跨度(提交+引擎+暴露等待,nvof 段上报值)
    double ofEngineMs = 0.0; // 冲刷点引擎等待(逐帧携带 —— 共享探针会被
                             // 下一帧种子帧覆盖,污染前帧读数)
    // Finish 在途票据:打包时取号,本续体消费完(析构)销号。资源重建
    // (RecreateFeature → CreateFrameResources)在票据未清零前必须等待 ——
    // 本结构持裸槽指针(读回映射/fenceEvent/时间戳),重建窗口穿过 =
    // use-after-free(2026-09-26 闪退族:memcpy INVALID_POINTER_WRITE)。
    // RAII:ProcessFrameFinish 的 FinishSelfDelete 在所有出口 delete 本体,
    // 析构销号覆盖全部路径(含失败提前 return)。
    D3D12Context *ticketCtx = nullptr;
    ~FrameFinish() {
        if (ticketCtx) ticketCtx->EndFrameFinishTicket();
    }
};

/* 本帧管线色(唯一赋值点在 ProcessFrame 内;下游 FG backbuffer /
 * post 转换 / 归位全读它)。原"四段连环覆盖 + pipeW/pipeH 平行变量"
 * 收拢为一个值对象。*/
struct PipeColor {
    ID3D12Resource *res = nullptr;
    int w = 0, h = 0; // PIPE 尺寸(vsrRun/实验模式)或源尺寸
};

bool DlssnrContext::ProcessFrame(
    const uint8_t *const *srcPlanes, const int64_t *srcStrides,
    uint8_t **dstPlanes, int64_t *dstStrides,
    int fgMultiplier,
    uint8_t **fgDstPlanes, int64_t *fgDstStrides, bool *fgGenOk,
    int width, int height, int n,
    ColorMatrix matrix, ColorRange range,
    char *err, size_t errLen,
    char *timingOut, size_t timingLen,
    FrameFinish **deferOut) noexcept {
    if (fgGenOk) {
        for (int g = 0; g < kFgGenSlots; ++g) fgGenOk[g] = false;
    }
    // 拆分模式(FFG 持锁窗口缩小):Submit 半段失败 = *deferOut 恒 NULL,
    // 槽由本半段释放;成功 = 所有权移交续体,Finish 半段负责释放。
    const bool deferMode = deferOut != nullptr;
    if (deferOut) *deferOut = nullptr;
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
    if (int newPreset = -1, newRes = -1, newScaling = -1;
        _shared.load(std::memory_order_acquire)->ConsumeRebuild(newPreset, newRes, newScaling)) {
        if (!RecreateFeature({newPreset, newRes, newScaling != 0}, err, errLen)) {
            // 面板可见:重建失败 = 本会话整体直通,具体原因进 stats。后续帧
            // 顶部 !ready 早退的发布被边缘去重,不会覆盖这条更具体的原因。
            PublishDeadState("passthrough", err);
            return false;
        }
    }
    const DlssnrParams frameParams = _shared.load(std::memory_order_acquire)->Snapshot();
    // 光流档位/后端/会话输入尺寸同步(只重建光流会话,不动 NGX feature)。
    // 在 AcquireSlot 之前消费:RebuildOf 要封池排空。失败只降级零 guidance,
    // 帧继续。会话输入尺寸 = follow(开关 + scaling 启用 + FG 未激活)?
    // 内部尺寸 : 源尺寸 —— FG 的 MVecs 契约要求与 backbuffer 同尺寸稠密
    // 运动场,FG 激活时 follow 被强制忽略。ConsumeRebuild 已在上文跑过,
    // 内部尺寸此处是新鲜的。
    bool fgGateLive = false;
    {
        std::lock_guard<std::mutex> fgLock(_fgMutex);
        fgGateLive = _fg && _fg->Enabled() && frameParams.fgEnabled;
    }
    // 本帧倍数(调用方从参数快照定格 —— 与输出帧数契约绑定;live 变化在
    // 源帧边界生效,由调用方逐帧传入)。0 = FG 未激活。
    const int fgM = fgGateLive && fgDstPlanes && _d3d12->FgSlots()
                        ? std::clamp(fgMultiplier, kFgMultMin, kFgMultMax)
                        : 0;
    // 单一裁决点见 SyncOfSession(allowRetry=false:帧路径不重试失败会话,
    // 防逐帧封池风暴;重试在 rebind/recreate 边界)。follow 尺寸换算在
    // helper 内。
    {
        char nvofErr[160]{};
        SyncOfSession(frameParams, width, height, /*allowRetry=*/false,
                      nvofErr, sizeof(nvofErr));
    }
    // 抗闪烁模式逐帧同步(live 参数):PoolHold 封池重建时域资源(毫秒级
    // 纹理分配,不动 NGX feature)。失败降级 0,模式仍可再切。PoolHold 保证
    // 无在飞帧 —— 对 _tValid 的复位与 evaluate 域内的状态推进无交叠。
    // (Rebind 热复用路径不在此重复同步:本同步逐帧兜住且天然持锁。)
    if (const int af = std::clamp(frameParams.antiFlicker, kAntiFlickerMin, kAntiFlickerMax);
        af != _curAntiFlicker) {
        {
            D3D12Context::PoolHold hold(*_d3d12);
            RebuildTemporalOrDegrade(af, "switch");
        }
        _tValid = false;
        if (_curAntiFlicker == 0 && _tPubState.load(std::memory_order_relaxed) != 3) {
            // 切到关(或重建后归 0):报 off;失败态保持 failed 不覆盖。
            _tPubState.store(0, std::memory_order_relaxed);
            _tPubRoute.store(0, std::memory_order_relaxed);
            _tPubWeight.store(0.0f, std::memory_order_relaxed);
        }
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
    LARGE_INTEGER qpcFreq{}, t0{}, t1{}, t2{}, t3a{}, t3b{}, t3c{}, t4{};
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
        _fpsMeter.Tick(static_cast<double>(entry.QuadPart) / static_cast<double>(qpcFreq.QuadPart));
    }
    // Diagnostic: VSDLSSNR_SKIP_EVAL=1 measures the pipe without NGX evaluate
    static const bool skipEval = GetEnvironmentVariableA("VSDLSSNR_SKIP_EVAL", nullptr, 0) != 0;
    static const bool dumpEnabled = GetEnvironmentVariableA("VSDLSSNR_DUMP", nullptr, 0) != 0;
    // NR 总开关(live,shared_lock 快照):关 = 跳过 NGX 评估,下游
    // (VSR/HDR/FG/输出转换)直连 C1 产物 inputColor(解耦:关闭的中间级
    // 不中转)。与诊断的 skipEval 互不相同:skipEval 连 NVOF/补帧一起跳
    // (测管线底价,直通拷贝保留)。
    const bool nrOff = frameParams.nrEnabled == 0;
    // 抗闪烁本帧是否 dispatch(eval 路径内判定;下游消费点据此改读稳定帧)。
    bool temporalRan = false;
    // RTX 实态(管线级常量,原定义在 eval 段后;前置到 C1 补做状态
    // uploadPost 之前 —— 纯定义上移,无行为变化):
    const bool vsrLive = _vsrRequested && !skipEval;
    const bool hdrLive = _hdrActive && !skipEval;
    // 探针(VSDLSSNR_RTX_NOEVAL=1):只跑本地管线不调 RTX NGX —— eval 期
    // 问题二分定位(NGX eval 段 vs 本地录制段)。
    static const bool rtxNoEval = GetEnvironmentVariableA("VSDLSSNR_RTX_NOEVAL", nullptr, 0) != 0;
    // 调试视图直出(NR 关):光流场调试图落在 outputColor,NR 关时下游
    // 直连 inputColor/vsrColor,调试图无人消费 = 面板开了也看不到。本帧
    // 旁路 VSR/HDR,让管线色 = outputColor(调试图),与 NR 开时"调试图
    // 当管线色(VSR 照常放大/FG 照常当 backbuffer)"同语义。差异视图仍
    // 需 NR(无 NR 改动 = 无差),维持 !nrOff 门不动。
    const bool debugPipe = nrOff && frameParams.debugView == 2;
    const bool vsrRun = vsrLive && !rtxNoEval && !debugPipe;
    const bool hdrRun = hdrLive && !rtxNoEval && !debugPipe;
    const bool rtxIn = vsrRun || hdrRun; // 本帧有 RTX eval:base 尾转 NSR、真实帧输出移 post 段
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
        IOpticalFlowBackend *of = nullptr;
        bool drain = false;   // 拆分模式失败路径:槽释放前排空 OF 在飞拷贝
        bool handoff = false; // 拆分模式 Submit 成功:所有权移交 FrameFinish
        // 提交后排空(2026-10-04 评审修):base/fg/post 任一提交成功后的失败
        // early-return,槽带着在飞 CL 归池 —— 下一帧 allocator Reset 撞执行中
        // 命令列表 = UB(gen eval 失败路径 3111 同款教训;此前只有 3114/3390
        // 两处有本地等待,且 3390 对门关帧等的是陈旧 fgFenceValue)。守卫携
        // "最后提交的 shared fence 值 + 用过的 RTX 队列",析构(未 handoff)
        // 时统一排空:直队列等 fence 值;RTX 队列补 SignalNow 锚再等(成功
        // 路径帧尾锚同机制,只是把锚从帧尾提前到失败点)。成功路径 handoff
        // 后由 FrameFinish 既有等待链接管,守卫不重复等待。
        uint64_t drainShared = 0;
        RtxQueue *drainQ[2] = {};
        int drainQN = 0;
        void ArmDrainShared(uint64_t v) { if (v > drainShared) drainShared = v; }
        void ArmDrainQueue(RtxQueue *q) {
            if (!q) return;
            for (int i = 0; i < drainQN; ++i)
                if (drainQ[i] == q) return;
            if (drainQN < 2) drainQ[drainQN++] = q;
        }
        ~SlotGuard() {
            if (s && !handoff) {
                // 拆分模式的 CopyDrainGuard 被禁用(成功路径排空在 Finish 尾);
                // 失败 early-return 仍会在此释放槽 —— FFX 的 copy CL 可能
                // 在飞,upload 堆 CPU 写(下一帧 PackInput)竞争由此排空。
                if (of && drain) of->WaitCopyIdle();
                if (drainShared)
                    ctx->WaitFenceValuePublic(drainShared, s->fenceEvent, nullptr, 0);
                for (int i = 0; i < drainQN; ++i)
                    if (const uint64_t v = drainQ[i]->SignalNow())
                        drainQ[i]->Wait(v, nullptr, 0, 10000);
                ctx->ReleaseSlot(s);
            }
        }
    } guard{ _d3d12, slot, _ofBackend.get(), false };
    // evaluate 域句柄(defer_lock):录制(RecordTemporal)到提交
    // (SubmitBaseFrame)必须同锁 —— 非 FG 帧无 fgMutex 串行,槽翻转在
    // evalLock 序而 GPU 执行按提交序,两序脱钩 = 抗闪烁历史前后帧颠倒
    // (2026-10-04 评审修)。所有 early-return 由 unique_lock 析构放锁;
    // 成功路径 Submit 后显式 unlock 缩窗。
    std::unique_lock<std::mutex> evalLock(_evaluateMutex, std::defer_lock);
    // drain 标志在 ofNeeded 声明后补齐(声明序在后)。
    // The pool seal only serializes; it does not refresh readiness. A
    // concurrent RecreateFeature may have failed (leaving _parameters null)
    // or a device loss may have latched while this thread waited for a slot
    // — without this re-check the frame would evaluate against a dead/null
    // parameter block and fault the latch.
    if (!_ready.load(std::memory_order_acquire) || !_parameters) {
        if (err && errLen) std::snprintf(err, errLen, "context not ready (rebuild failed or device lost)");
        return false;
    }
    // pack-segment clock starts here: PackInput is a pure-CPU YUV 三平面行拷贝
    // into the slot's upload heap (YUV 原生,RGBS 已废;YUV→RGB 在 GPU 的
    // C1 转换 pass). RecreateFeature (above, only on the
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
    bool evalZeroed = false; // 直通帧(skipEval/nrOff):无 NGX 调用,eval_cpu 记 0
    int nvofInputIndex = -1;      // 本帧写入的 NVOF 输入 ping-pong 槽位(诊断 dump 用)
    // DLSS FG:本帧各插值槽是否真插值(fgGenOk;false = 复制真实帧:复位/
    // 零光流/面板关/eval 降级)。函数级作用域 —— 统计段与 dump 都要读。
    // fgRan = 本帧向 proxy 提交过 eval(含播种;dump 判定用)。
    int fgEvaluatedCount = 0;
    bool fgRan = false;
    // OF 门(解耦:按消费者决定跑不跑):NR 开(要 guidance)或 FG 激活
    // (要运动场)才执行;双关时整段跳过 —— 每帧零提交、零记账,会话对象
    // 保留(live 重开无缝,RebuildOf 只在档位/尺寸变化时跑)。
    const bool ofNeeded = !skipEval && _ofBackend && _ofBackend->Enabled() &&
                          _curOfQuality > 0 && (!nrOff || fgM > 0);
    // NVOF 延迟 densify 的载体(声明在 ofNeeded 块外:守卫须活到 ProcessFrame
    // 出口或显式冲刷点 —— 块内声明会在块尾立即析构冲刷,延迟即失效)。
    // ofGate = StageFrame 移出的门锁(持有至冲刷);ofDensifyPending = 待冲刷;
    // post/postCopy 提升到块外供守卫与冲刷点引用。
    OfPostExecuteFn post;
    OfPostCopyFn postCopy;
    std::unique_lock<std::mutex> ofGate;
    bool ofDensifyPending = false;
    double ofEngineMs = 0.0; // 冲刷点引擎等待(逐帧,packFinish 带入 FrameFinish)
    if (ofNeeded) {
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
        // 整帧 CopyTextureRegion 要求源格式 == 注册输入格式(BGRA8):
        // 管线色 RGBA16F(>8bit 无 RTX)/ R10G10B10A2(VSR-only 10bit)时
        // 格式不匹配,nvof CL Close 恒败 → 连败闩锁 NVOF 自禁用
        //(2026-10-08 用户 10-bit 实测定案)。非 follow + 非 BGRA8 改走
        // 同款降采样 shader 1:1 直写(SRV 读格式无关);BGRA8 保留零拷贝。
        const bool nvofInputViaShader =
            ofKind == kOfBackendNvof && !densifyInternal &&
            _d3d12->ColorFormat() != DXGI_FORMAT_B8G8R8A8_UNORM;
        if (ofKind == kOfBackendNvof) {
            // ---- NVOF:网格流(1/32 像素定点)densify(原 PORTING #6 路径)----
            NvofContext *nv = static_cast<NvofContext *>(_ofBackend.get());
            const uint32_t gs = nv->GridSize();
            const uint32_t flowW = (nvW + gs - 1) / gs;
            const uint32_t flowH = (nvH + gs - 1) / gs;
            const bool hasBwd = nv->Bidirectional();
            const bool hasCost = nv->CostEnabled();
            post = [this, &slot, flowW, flowH, hasCost, hasBwd, msX, msY, densifyInternal,
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
                                      flowW, flowH,
                                      hasCost, hasBwd, hasBwd && hasCost,
                                      densifyInternal ? 1.0f : msX,
                                      densifyInternal ? 1.0f : msY,
                                      densifyInternal ? static_cast<UINT>(HeapSlot::UavReducedMotion)
                                                      : static_cast<UINT>(HeapSlot::UavMotion),
                                      densifyInternal ? static_cast<UINT>(HeapSlot::UavReducedConfidence)
                                                      : static_cast<UINT>(HeapSlot::UavConfidence));
                D3D12_RESOURCE_BARRIER g2[2]{
                    Transition(dstMotion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                    Transition(dstConf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                };
                cl->ResourceBarrier(2, g2);
            };
            // YUV 原生:postCopy 恒设 —— 回调在 nvof CL 第一次提交上记录
            // YUV→RGB 转换,follow 时追加 RecordNvofDownsample 直写注册输入
            // 纹理;非 follow 由 StageFrame 随后做整帧纹理拷贝。
            postCopy = [this, &slot, nvW, nvH, densifyInternal, nvofInputViaShader,
                        width, height, matrix, range](ID3D12GraphicsCommandList *cl, int inputIndex) {
                _d3d12->RecordConvertInput(*cl, *slot, matrix, range,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (densifyInternal || nvofInputViaShader) {
                    _d3d12->RecordNvofDownsample(*cl, *slot,
                                                 static_cast<int>(densifyInternal ? nvW : width),
                                                 static_cast<int>(densifyInternal ? nvH : height),
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
                                         densifyInternal ? static_cast<UINT>(HeapSlot::UavReducedMotion)
                                                         : static_cast<UINT>(HeapSlot::UavMotion),
                                         densifyInternal ? static_cast<UINT>(HeapSlot::UavReducedConfidence)
                                                         : static_cast<UINT>(HeapSlot::UavConfidence));
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
        // NVOF 延迟 densify(2026-09-25):execute 提交后 StageFrame 即返回,
        // CPU 等引擎 + densify 提交推迟到首个 motion 消费者 CL 提交前(下方
        // 显式冲刷点),与 NGX eval / DLSSG 录制重叠 —— 真机 of=2 引擎等待
        // 11-13ms 曾全额串进帧链(占 24fps 帧预算 ~30%)。门锁随 StageFrame
        // 移出并持有至冲刷(守卫在块外,见 ofNeeded 块后),提交互斥/轮转簿
        // 记与旧"门内全程"形态等价。FFX:门锁不动、densify 门内已提交,
        // 冲刷恒 no-op。
        const OfStageResult st =
            _ofBackend->StageFrame(n, _d3d12->InputColor(*slot), post, postCopy,
                                   densifyInternal || nvofInputViaShader, ofGate);
        nvofHistoryReset = st.historyReset;
        nvofMs = _ofBackend->LastStageMs();
        realMotion = st.waitFenceValue != 0;
        nvofInputIndex = st.inputIndex;
        ofDensifyPending = st.pendingDensify;
        // 种子/迟到帧(无待冲刷)显式归零:共享探针 _lastExeWaitMs 可能残留
        // 上一帧的等待值,不能代表本帧。
        ofEngineMs = 0.0;
    }
    // 冲刷守卫(ProcessFrame 出口前恒活):任何出口(含失败 early-return)
    // 都冲刷待定 densify —— execute 在飞时返回会让下一帧 copy 的队列 Wait
    // (doneFence)从"预满足"变成承重(NVOF 输出栅栏队列 Wait 不可靠,不
    // 可赌)。显式冲刷点只优化等待位置;此处析构兜底其余路径。声明序保证
    // 析构时 ofGate(门锁)仍被持有(先 ofFlushGuard 后 ofGate)。
    // FlushPendingDensify 幂等,显式冲刷后此处即刻返回。
    struct OfFlushGuard {
        IOpticalFlowBackend *b;
        const OfPostExecuteFn *post;
        bool on;
        ~OfFlushGuard() {
            if (on && b) b->FlushPendingDensify(*post);
        }
    } ofFlushGuard{ _ofBackend.get(), &post, ofDensifyPending };
    auto flushOfDensify = [&]() noexcept {
        if (ofDensifyPending) {
            _ofBackend->FlushPendingDensify(post);
            ofEngineMs = _ofBackend->LastExeWaitMs();
        }
    };
    // 3 连败停用(会话内部闩锁)在帧线程侧只发现不重试,防风暴;
    // Rebind/换档时经 _nvofFailed 条目重试一次。
    if (_curOfQuality > 0 && _ofBackend && !_ofBackend->Enabled() && !_nvofFailed) {
        _nvofFailed = true;
        TimingStatusLine("DLSSNR STATUS: of disabled (exhausted); zero guidance until rebind");
    }
    // early-return 路径释放槽位前排空在途拷贝(宿主复用 upload 缓冲;
    // 正常路径已被 execute/完成栅栏覆盖,no-op)。拆分模式下本半段不释放
    // 槽 —— 排空迁移到 Finish 尾(槽真正释放点)。
    struct CopyDrainGuard {
        IOpticalFlowBackend *b;
        bool on;
        ~CopyDrainGuard() { if (b && on) b->WaitCopyIdle(); }
    } drainGuard{ _ofBackend.get(), ofNeeded && !deferMode };
    // 拆分模式失败路径的槽释放排空(SlotGuard.drain):与上同步 —— 成功
    // 路径排空在 Finish 尾,失败 early-return 在 SlotGuard 析构点排空。
    guard.drain = ofNeeded && deferMode;

    if (ProbeEnabled()) TimingStatusLine("PROBE: post-stage"); // 探针(VSDLSSNR_PROBE=1)
    if (!_d3d12->BeginFrameRecording(*slot)) {
        if (err && errLen) std::snprintf(err, errLen, "BeginFrameRecording(frame) failed");
        return false;
    }
    // NOTE: 2026-09-05 单次观测:NR snippet eval 的宿主 CL 内联 timestamp
    // EndQuery 后该帧 SEH 崩溃,改独立括号 CL 后消失。"同 CL 内联 = 危险"
    // 曾作假设(n=1,异常码未留痕)。2026-10-02 按协议复核(testkit/
    // ts_inline_probe.py,VSDLSSNR_TS_INLINE=1):内联组 3 会话 × 121 eval
    // = 363 次全部存活、零 SEH、零设备移除(内联 delta 读数正常 5-8ms),
    // 对照组同规模 0 失败 —— 本机(RTX 3080 / 驱动 617.14 / snippet
    // 2026-09-27)假设不成立,当年单次崩溃应归因于未留痕的混淆项(旧查询
    // 堆用法/当时驱动版本),真因不可考。生产路径仍维持独立括号 CL:零成
    // 本、已证稳定,且 base 段 GPU 时间戳账目依赖它;内联探针保留备用。
    // RTX 队列独立括号化不受此条任何约束。

    // C1 产物落点(直连接线):本帧有 RTX eval → NSR(VSR/HDR/NGX 直读,
    // NR 关时 VSR 直接吃 inputColor);否则 COMMON(下游 fgBar/C2 统一
    // NSR 化)。skipEval 时 rtxIn 恒 false → COMMON,诊断拷贝语义不变。
    const D3D12_RESOURCE_STATES uploadPost = rtxIn
                                                 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                                 : D3D12_RESOURCE_STATE_COMMON;
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
    if (skipEval) {
        // 直通拷贝(诊断底价,字节级保留):Input → Output,不经 NR 推理,
        // NVOF/补帧整体跳过。输入状态:nvof CL 转换落 NSR(convertedOnNvof)/
        // 槽 CL 补转换落 COMMON,拷贝后统一归 COMMON。
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
        // 直通分支没有 NGX 调用:eval_cpu 记 0(窗口 t1→t2 扣掉 nvof 后只剩
        // 拷贝录制的 ~10µs 噪声,"eval_cpu(NGX 调用)" 段不该为它显形)。
        evalZeroed = true;
    } else if (nrOff) {
        // NR 关直连(解耦):不经 NGX,也不做 Input→Output 中转拷贝 ——
        // 下游(VSR/HDR eval / FG backbuffer / post 段输出转换)直接消费
        // inputColor(uploadPost 已按消费方落 NSR/COMMON;rtxIn 帧的
        // NSR→COMMON 归位在 post CL 尾 recordInputPark)。base CL 可能整帧
        // 为空(空 CL 提交 = 纯栅栏,合法);gpu 段账目归 conv(见末尾拆账)。
        QueryPerformanceCounter(&t2);
        evalZeroed = true;
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
        if (ProbeEnabled()) TimingStatusLine("PROBE: pre-eval-lock"); // 探针(VSDLSSNR_PROBE=1)
        // 探针:evaluate 互斥等待。eval_cpu 段包含它(无法从分段里拆出),
        // 这里单独测量:"NGX CPU 变慢"与"被别的槽的 evaluate 排队"由此分家。
        QueryPerformanceCounter(&tLock0);
        evalLock.lock(); // defer_lock 句柄,见上方声明(录制→提交同锁,2026-10-04 评审修)
        QueryPerformanceCounter(&tLock1);
        if (ProbeEnabled()) TimingStatusLine("PROBE: eval-locked"); // 探针(VSDLSSNR_PROBE=1)
        DWORD sehCode = 0;
        // NGX PARAM_RESET 只由帧序门的播种帧携带(大跳/回退/缺口的恢复路径)。
        if (!SetEvaluateParametersSafely(*slot, nvofHistoryReset, realMotion, frameParams, &sehCode)) {
            if (err && errLen) {
                if (NgxRuntimeGuard::IsFaulted() && !sehCode) {
                    std::snprintf(err, errLen,
                                  "NGX faulted (SEH 0x%x); SDK disabled until host restart",
                                  NgxRuntimeGuard::FaultCode());
                } else {
                    std::snprintf(err, errLen, "Evaluate parameter setup raised SEH");
                }
                TimingStatusLine(err);
            }
            return false;
        }
        // TS-INLINE 探针(2026-10-02 复核协议,见帧路径 NOTE):EndQuery/
        // Resolve 内联在 NGX eval 同一 CL —— 被验证的正是这个组合。查询堆
        // 索引 14/15(常驻账目 0..5 + RTX 括号 6..11),Resolve 落 readback
        // 偏移 112/120;Finish 的 WaitFrame 后读账。故障面与协议一致:录制
        // 期 SEH 由 SnippetEvaluateSafely 接,GPU 侧故障走既有设备移除熔断,
        // 均留痕。
        const bool tsInline = TsInlineProbeEnabled() && slot->tsQueryHeap.Get() != nullptr;
        if (tsInline) {
            cl->EndQuery(slot->tsQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 14);
        }
        const NVSDK_NGX_Result r = SnippetEvaluateSafely(cl, _parameters, &sehCode);
        if (tsInline) {
            cl->EndQuery(slot->tsQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 15);
            cl->ResolveQueryData(slot->tsQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                 14, 2, slot->tsReadback.res.Get(), 112);
        }
        QueryPerformanceCounter(&t2);
        if (sehCode) {
            char buf[160];
            snprintf(buf, sizeof(buf), "EvaluateFeature raised SEH 0x%x (scaling=%d); NGX latched, no further SDK entry",
                     sehCode, scaling ? 1 : 0);
            DbgLine(buf);
            TimingStatusLine(buf);
            if (err && errLen) snprintf(err, errLen, "EvaluateFeature raised SEH 0x%x; NGX disabled until host restart", sehCode);
            return false;
        }
        if (ProbeEnabled()) TimingStatusLine("PROBE: eval-ok"); // 探针(VSDLSSNR_PROBE=1)
        if (!NVSDK_NGX_SUCCEED(r)) {
            if (err && errLen) {
                if (NgxRuntimeGuard::IsFaulted()) {
                    std::snprintf(err, errLen,
                                  "NGX faulted (SEH 0x%x); SDK disabled until host restart",
                                  NgxRuntimeGuard::FaultCode());
                } else {
                    snprintf(err, errLen, "EvaluateFeature failed (0x%x)", static_cast<unsigned>(r));
                }
                TimingStatusLine(err);
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

        // ---- 抗闪烁时域稳定(上游 DLSSNRTemporal::Draw 语义)----
        // 仍在 evalLock 域内:_tNext/时间线状态推进与历史缓冲读写同锁串行。
        // weight = exp(-Δt/80ms);帧序不连续/播种帧/useMotion 翻转/距上帧
        // >250ms 一律 weight 0(仍 dispatch:历史播种为当前残差,下游正常)。
        // dup 帧(同源帧重复评估)整帧跳过 —— 历史/EMA 双不动(上游
        // Duplicate 语义),下游读原始 NR 输出。
        if (_curAntiFlicker > 0 && frameParams.debugView == 0 &&
            _d3d12->TemporalMode() == _curAntiFlicker) {
            const bool dup = _tValid && _tLastFrame == static_cast<long long>(n);
            if (!dup) {
                const bool useMotion = _curAntiFlicker >= 2 && realMotion;
                const bool reset = !_tValid || nvofHistoryReset ||
                                   useMotion != _tLastUseMotion || _tLastFrame < 0 ||
                                   static_cast<long long>(n) != _tLastFrame + 1;
                LARGE_INTEGER tNow{};
                QueryPerformanceCounter(&tNow);
                const double nowSec =
                    static_cast<double>(tNow.QuadPart) / static_cast<double>(qpcFreq.QuadPart);
                const double dt = reset ? 0.0 : nowSec - _tLastQpc;
                const float weight = (reset || dt <= 0.0 || dt > 0.25)
                                         ? 0.0f
                                         : static_cast<float>(std::exp(-dt / 0.08));
                if (ProbeEnabled()) {
                    char tp[96];
                    std::snprintf(tp, sizeof(tp),
                                  "PROBE: temporal n=%d reset=%d dt=%.1fms w=%.3f next=%d",
                                  n, reset ? 1 : 0, dt * 1000.0, weight, _tNext);
                    TimingStatusLine(tp);
                }
                UINT motionSrv = 10;
                UINT motionW = static_cast<UINT>(width), motionH = static_cast<UINT>(height);
                if (useMotion && densifyInternal) {
                    // follow 内部管线:densify 直写 reducedMotion(内部尺寸,
                    // 内部像素单位)—— shader 按 MotionExtent 比例采样并
                    // 把向量换算回源像素。
                    motionSrv = 18;
                    motionW = static_cast<UINT>(_d3d12->InternalWidth());
                    motionH = static_cast<UINT>(_d3d12->InternalHeight());
                }
                _d3d12->RecordTemporal(*slot, _curAntiFlicker, _tNext, useMotion,
                                       motionSrv, motionW, motionH, weight);
                _tLastFrame = static_cast<long long>(n);
                _tLastQpc = nowSec;
                _tLastUseMotion = useMotion;
                _tValid = true;
                _tNext ^= 1;
                temporalRan = true;
                // stats 发布(诊断页"请求 vs 实际"):weight==0 的播种帧报
                // seed,正常混合报 steady。
                _tPubState.store(weight > 0.0f ? 2 : 1, std::memory_order_relaxed);
                _tPubRoute.store(_curAntiFlicker, std::memory_order_relaxed);
                _tPubWeight.store(weight, std::memory_order_relaxed);
            }
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
    // 管线色 = DLSSG 的 backbuffer。两种形态(fg_hdr_interp 实验开关):
    //   0(默认)= SDR 域:vsrRun → vsrColor BGRA8 @PIPE;否则 outputColor
    //     BGRA8 @src。DLSSG 的 HDR 路径对 >1.0 的 scRGB 线性值不保真
    //     (2026-09-24 观测插值帧高光 276→70 nits;2026-10-02 复现确认 ——
    //     scRGB 直喂 + ColorBuffersHDR=1 时插值帧整帧垃圾 ~4654 nits,
    //     呈现形态随下游转换链而变,根因同为 >1.0 不保真)。现行 fg_hdr
    //     实验路径经 HdrToPq 编码 ≤1.0 避开该缺陷(同日验证:插值/真实
    //     max-nits 比 0.90 无塌陷;默认仍 0,"部分驱动闪烁"另行定夺),
    //     插值在 SDR 域,每个输出帧再各自过一次
    //     TrueHDR → postB 转换(hdrPostSplit 三段提交)。
    //   1(实验)= HDR 域:TrueHDR 只做真实帧一次,hdrColor FP16 scRGB 即
    //     backbuffer,DLSSG 直接插出 HDR 插值帧(postA 内逐 gen 转换/回读,
    //     无 postB/无插值帧 TrueHDR)。省 TrueHDR ×(M-1),部分驱动闪烁。
    const bool hdrPostSplit = hdrRun && !_fgHdrInterp;    // 修复后形态(默认)
    const bool hdrLegacyInterp = hdrRun && _fgHdrInterp;  // 实验模式
    /* 本帧管线色(单一赋值点;下游 FG backbuffer / post 转换 / 归位全读它):
     * res + 几何(vsrRun/实验模式 = PIPE 尺寸,否则源尺寸)。原"四段连环覆盖 +
     * pipeW/pipeH 平行变量"收拢于此 —— 落点选择逻辑不变,唯一赋值点。*/
    PipeColor pipe{_d3d12->OutputColor(*slot), width, height};
    // NR 关直连:无 NGX 产出,管线色 = C1 产物 inputColor(下游 FG
    // backbuffer / post 段 C2 直读,不经 outputColor 中转)。debugPipe 帧
    // 例外:outputColor 已是调试图,保持默认管线色(2513 赋值)。
    if (nrOff && !debugPipe) pipe.res = slot->inputColor.Get();
    // 抗闪烁激活帧:无 RTX 覆盖时管线色 = 稳定帧(下游 FG backbuffer /
    // hdr-only DLSSG backbuffer / post 转换统一改读;vsrRun/hdrLegacyInterp
    // 的覆盖在其后 —— 它们的输出才是管线色)。
    if (temporalRan) pipe.res = _d3d12->TemporalOut(*slot);
    if (vsrRun) {
        pipe.res = slot->vsrColor.Get();
        pipe.w = _pipeW;
        pipe.h = _pipeH;
    }
    if (hdrLegacyInterp) {
        // PQ 域插帧:DLSSG backbuffer = fgBack(PQ 码,HdrToPq 编码 pass 在
        // fg CL 上由 hdrColor 产出 —— postA 等 TrueHDR 链尾栅栏,就绪闭合)。
        // hdrColor 本体仍归真实帧转换(post CL PqToYuv)。
        pipe.res = slot->fgBack.Get();
        pipe.w = _pipeW;
        pipe.h = _pipeH;
    }
    // FG backbuffer 形态门:backbuffer 在 PIPE 几何需 VSR 实跑或实验模式的
    // HDR backbuffer;hdr-only 修复形态 PIPE==src,outputColor 直接可用。
    // skipEval/NOEVAL 帧整体无 FG。
    const bool fgPipeOk = vsrRun || hdrLegacyInterp || !_vsrRequested;

    // ---- base/postA(TrueHDR 链)/postB 分段提交(处理用时拆账)----
    // 各段 GPU 耗时 = 时间戳括号差分(NGX 同 CL 内联打点 2026-09-05 单次
    // 观测 SEH,2026-10-02 复核未复现;全部段旁路括号 CL,见上方 NOTE;
    // RTX 段括号化同日落地,SubmitTsBracket,索引 6..11):
    //   base CL          = 基础管线(NR 推理/残差/直通)
    //   postA(fg CL)    = DLSSG 推理链(HDR 会话;非 HDR = 推理+转换旧形态)
    //   TrueHDR 专用队列 = 真实帧 + 逐插值帧(每输出帧一次 eval)
    //   postB(post CL)  = HDR 会话的输出转换/回读(等 TrueHDR 链尾栅栏)
    const bool fgGateOpen = fgM > 0 && !skipEval && realMotion && !nvofHistoryReset &&
                            _fg && _fg->Enabled() && fgPipeOk;
    // postA(fg CL)= 纯 DLSSG 推理段(输出转换已全部移 post CL)+ NR 开时
    // 的 RTX 输入归位屏障(inBack)。NR 关直连帧 outputColor 无消费者,
    // 无屏障可录 → 不开启(空 CL 无意义)。Begin 失败:FG 降级门关;RTX
    // 帧无 inBack 落点 = 整帧失败。
    const bool postBeginReq = fgGateOpen || (rtxIn && !nrOff && !hdrPostSplit);
    bool fgBeginOk = false;
    if (postBeginReq) {
        fgBeginOk = _d3d12->BeginFgRecording(*slot);
        if (!fgBeginOk) {
            if (rtxIn && !hdrRun) {
                if (err && errLen) std::snprintf(err, errLen, "post CL begin failed (rtx frame)");
                TimingStatusLine("DLSSNR STATUS: post CL begin FAILED on rtx frame");
                return false;
            }
            TimingStatusLine("DLSSNR STATUS: fg CL begin FAILED; interpolation degraded to dup");
        }
    }
    const bool fgOnFgCl = fgGateOpen && fgBeginOk;

    // 调试视图(面板"调试视图"下拉):1 = |输出−输入|×20 灰度;2 = 光流场
    // (方向→色相、幅值→亮度)。此处是 skipEval/eval 两路径帧末
    // outputColor=UAV 的唯一公共插入点(nrOff 直连帧跳过差异视图);
    // FG 激活时插帧链会拿到调试图当 backbuffer(调试态可接受)。
    // 光流场取材:真运动帧按管线取 slot 场
    // (follow 内部管线 = reducedMotion,否则源尺寸 motion;均已 NSR),
    // 播种/OF 关帧绑静态零纹理 —— 全黑 = 无光流数据,与差异视图
    // "一片灰 = 没动"同款语义。
    // 冲刷点 A:光流场调试视图读 per-slot 运动场 —— densify 必须先于本 CL
    // 的 FIFO(差异视图不读运动场,不触发)。
    if (frameParams.debugView == 2) flushOfDensify();
    if (frameParams.debugView == 2) {
        // outputColor 入态:NR 开帧 eval/直通拷贝同 CL 留 UAV(默认);
        // NR 关帧本 CL 无人触碰,跨 ECL 衰减后实际 COMMON(skipEval 的
        // 直通拷贝除外 —— 它先跑,同 CL 留 UAV)。
        _d3d12->RecordFlowView(*slot, realMotion && densifyInternal, realMotion,
                               nrOff && !skipEval ? D3D12_RESOURCE_STATE_COMMON
                                                  : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    } else if (frameParams.debugView == 1 && !nrOff) {
        // 差异视图 = |NR 改动|:NR 关(直连)时 outputColor 无本帧内容,
        // diff 无语义 —— 跳过(画面原样),而非把陈旧 outputColor 当真。
        _d3d12->RecordDebugDiff(*slot);
    }
    // base CL 收尾(解耦后只剩 RTX 输入准备):NR 开 + RTX eval → NGX 产出
    // outputColor 转 NSR 作 VSR/HDR 输入;NR 关直连 → inputColor 已由 C1
    // 落 NSR(uploadPost),outputColor 本帧无消费者不做屏障(post CL 尾
    // recordInputPark 归位 inputColor)。真实帧输出转换(C2)与回读已全部
    // 移 post CL 常驻转换段(记账归 conv,不再粘 base/fg CL)。
    auto *baseCl = slot->commandList.Get();
    if (rtxIn && !nrOff) {
        // RTX 输入 NSR 化:常规帧 = outputColor(eval/残差帧末 UAV 不变量);
        // 抗闪烁帧 = 稳定帧(RecordTemporal 已显式 COMMON 落位,outputColor
        // 本帧无下游消费者、停在 COMMON 不动)。
        D3D12_RESOURCE_BARRIER inPrep[1]{
            temporalRan ? TransitionFromTo(_d3d12->TemporalOut(*slot),
                                           D3D12_RESOURCE_STATE_COMMON,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                        : Transition(_d3d12->OutputColor(*slot),
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        baseCl->ResourceBarrier(1, inPrep);
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
    // guidance 归位恒落 post CL 尾(本帧最后一条主队列 CL,见下方 post 段)。
    // NOTE: timestamp disabled, see note above
    // NVOF flow 已在 StageFrame 里 CPU 等待完成(execute 后的输出栅栏),
    // 槽 CL 提交时 GPU 侧 flow 已就绪 —— 不再需要队列级 Wait(实测该栅栏
    // 在队列 Wait 语义下可能永不满足(根因系 91ea1d1 栅栏计数器重计 bug,
    // b0a669f 即日修复,队列 Wait 无过错;见 NvofContext::FlushPendingDensify
    // 注释)。
    // 冲刷点 B:NR 开时 base CL 的 NGX eval / guidance 降采样是首个 motion
    // 消费者 —— densify CL 必须先于 base CL 入队(FIFO)。CPU 等引擎在 eval
    // 录制期间已被消耗大半,此处常态所剩无几。NR 关帧跳过(base CL 无运动
    // 消费),把重叠窗口让给 fg CL 录制(冲刷点 C)。
    if (!nrOff) flushOfDensify();
    if (!_d3d12->SubmitBaseFrame(*slot, err, errLen)) {
        if (err && errLen) {
            TimingStatusLine(err);
            std::snprintf(err, errLen, "Submit(base) failed");
        }
        return false;
    }
    guard.ArmDrainShared(slot->baseFenceValue); // 失败路径槽释放前排空在飞 base CL
    if (evalLock.owns_lock())
        evalLock.unlock(); // 录制→提交同锁收口完成,后续 RTX eval 自取(2026-10-04 评审修;skipEval/nrOff 帧未加锁,不得误 unlock)
    if (ProbeEnabled()) TimingStatusLine("PROBE: base submitted"); // 探针(VSDLSSNR_PROBE=1)
    // base 完成观测(t3a,2026-09-25 时间戳化):默认路径不再 CPU 阻塞 ——
    // base GPU 执行与后续 RTX 提交链 + fg/post 录制重叠(此前阻塞把这段
    // CPU 时间串在 base 之后,FG 模式下还撑大 fgMutex 持锁窗口)。t3a 由
    // Finish 读 ts CL 的时间戳回读得出(精确免阻塞,SubmitBaseFrame 内已
    // 提交);时间戳不可用/本帧 ts 失败时回退阻塞观测(旧行为,账目仍真,
    // 只是不重叠)。栅栏超时 = GPU 挂起/设备移除,熔断停帧。
    if (!_d3d12->GpuTsEnabled() || !slot->tsValid) {
        if (!_d3d12->WaitBaseFrame(*slot, err, errLen)) {
            if (_d3d12->IsDeviceLost()) _ready.store(false, std::memory_order_release);
            if (err && errLen) {
                TimingStatusLine(err);
                std::snprintf(err, errLen, "Wait(base) failed");
            }
            return false;
        }
        if (ProbeEnabled()) TimingStatusLine("PROBE: base waited"); // 探针(VSDLSSNR_PROBE=1)
        QueryPerformanceCounter(&t3a);
    }

    // ---- RTX eval(专用队列;fence 链 base → vsr → postA → hdr 链)----
    // CPU 端不等 eval 完成:post 段提交时主队列 Wait(生产者 fence)保证
    // 消费顺序,Unpack 由 post 栅栏覆盖。录制经 _evaluateMutex 与 NR/DLSSG
    // 串行(NGX 非线程安全;两队列的 CL/allocator 均为单例,锁内顺序录制)。
    // 分段计时锚:每段 eval 的完成栅栏单独留存(t3a 后有界 CPU 等待,把
    // RTX 墙钟拆回 vsr/hdr 名下)。hdrChainFence/Val = TrueHDR 链尾(真实
    // 帧或最后一个插值帧),postB 消费等待 + 计时锚。
    ID3D12Fence *vsrDoneFence = nullptr;
    uint64_t vsrDoneVal = 0;
    ID3D12Fence *hdrDoneFence = nullptr;
    uint64_t hdrDoneVal = 0;
    ID3D12Fence *hdrChainFence = nullptr;
    uint64_t hdrChainVal = 0;
    // per-eval 参数走本帧快照(面板质量/HDR 滑块逐帧生效;几何/形态仍按
    // 创建时的 _rtx)。clamp 在 helper 内的常量引用,越界面板值安全。
    // 单一快照语义:RTX 键与帧内其它消费同源 frameParams(此前此处再取一次
    // 快照,NR 键旧 RTX 键新的分叉语义已消除)。
    const DlssnrParams &rtxLive = frameParams;
    // 提交链探针:t3a(RTX eval 提交起点)→ 等待区起点 = 本帧全部 NGX
    // eval 的 CPU 提交耗时(vsr/hdr/dlssg 的参数录制,专用队列逐笔串行)。
    LARGE_INTEGER tSub0{}, tSub1{};
    QueryPerformanceCounter(&tSub0);
    // RTX 输入源(直连接线):NR 开 = NGX 产出 outputColor;base 尾已 NSR。
    // 抗闪烁帧 = 稳定帧(同一屏障点 NSR 化)。NR 关 = C1 产物 inputColor
    // 直连(C1 已落 NSR),不经中转。
    ID3D12Resource *rtxColorIn = nrOff ? slot->inputColor.Get()
                                       : (temporalRan ? _d3d12->TemporalOut(*slot)
                                                      : _d3d12->OutputColor(*slot));
    if (vsrRun) {
        std::lock_guard<std::mutex> rtxLock(_evaluateMutex);
        // 计时括号(6/7):pre 自带队列 Wait(base fence,与 eval 生产者等待
        // 同值)—— 章落点 = VSR 纯执行起点,队列空闲不入账。括号与 eval 同
        // 锁同线程程序序提交,FIFO 包住;失败路径 post 括号不提交 → 缺账
        // 记 0(帧已失败)。
        _d3d12->SubmitTsBracket(*slot, _vsr->Queue().Native(),
                                slot->ts[kTsPreVsr].alloc.Get(),
                                slot->ts[kTsPreVsr].cl.Get(), kTsPreVsr,
                                _d3d12->Fence(), slot->baseFenceValue);
        guard.ArmDrainQueue(&_vsr->Queue()); // 失败路径排空含本括号
        char rtxErr[160]{};
        uint64_t fv = 0;
        if (!_vsr->Evaluate(rtxColorIn, width, height,
                            slot->vsrColor.Get(), _pipeW, _pipeH,
                            std::clamp(rtxLive.rtxVsrStrength, kVsrStrengthMin, kVsrStrengthMax),
                            _d3d12->Fence(), slot->baseFenceValue, &fv,
                            rtxErr, sizeof(rtxErr))) {
            if (err && errLen) std::snprintf(err, errLen, "%.180s", rtxErr);
            return false;
        }
        _d3d12->SubmitTsBracket(*slot, _vsr->Queue().Native(),
                                slot->ts[kTsPostVsr].alloc.Get(),
                                slot->ts[kTsPostVsr].cl.Get(), kTsPostVsr, nullptr, 0);
        vsrDoneFence = _vsr->Queue().Fence();
        vsrDoneVal = fv;
        // fv 只覆盖 eval,不覆盖其后入队的 post 括号;队尾补一记 Signal 作
        // 完成锚点,帧槽回收等 vsrDone 才真正蕴含括号完成(0 = Signal 失败,
        // 设备域故障,回落 eval 值由帧失败路径收尾)。
        if (const uint64_t sealed = _vsr->Queue().SignalNow()) vsrDoneVal = sealed;
        _pipeLedger.Set(slot->vsrColor.Get(), D3D12_RESOURCE_STATE_COMMON); // NGX 写后衰减
    }
    // 真实帧 TrueHDR(输入 = vsrColor(vsrRun)或 outputColor —— 恒 SDR 域;
    // 插值帧的 TrueHDR 在 postA 提交后逐 gen 追加)。官方契约:TrueHDR 必须
    // 在 VSR 之后(fence 链保证)。输出 hdrColor(FP16 scRGB @PIPE)。
    if (hdrRun) {
        std::lock_guard<std::mutex> rtxLock(_evaluateMutex);
        // 计时括号(8/9):TrueHDR 真实帧(生产者等待与 eval 同值)。
        _d3d12->SubmitTsBracket(*slot, _hdr->Queue().Native(),
                                slot->ts[kTsPreHdr].alloc.Get(),
                                slot->ts[kTsPreHdr].cl.Get(), kTsPreHdr,
                                vsrRun ? _vsr->Queue().Fence() : _d3d12->Fence(),
                                vsrRun ? vsrDoneVal : slot->baseFenceValue);
        guard.ArmDrainQueue(&_hdr->Queue()); // 失败路径排空含本括号
        char rtxErr[160]{};
        uint64_t fv = 0;
        if (!_hdr->Evaluate(vsrRun ? slot->vsrColor.Get()
                                   : rtxColorIn,
                            pipe.w, pipe.h, slot->hdrColor.Get(),
                            rtxLive.rtxHdrContrast, rtxLive.rtxHdrSaturation,
                            rtxLive.rtxHdrMiddleGray, rtxLive.rtxHdrMaxLuminance,
                            vsrRun ? _vsr->Queue().Fence() : _d3d12->Fence(),
                            vsrRun ? vsrDoneVal : slot->baseFenceValue, &fv,
                            rtxErr, sizeof(rtxErr))) {
            if (err && errLen) std::snprintf(err, errLen, "%.180s", rtxErr);
            return false;
        }
        _d3d12->SubmitTsBracket(*slot, _hdr->Queue().Native(),
                                slot->ts[kTsPostHdr].alloc.Get(),
                                slot->ts[kTsPostHdr].cl.Get(), kTsPostHdr, nullptr, 0);
        hdrDoneFence = _hdr->Queue().Fence();
        hdrDoneVal = fv;
        hdrChainFence = hdrDoneFence;
        hdrChainVal = fv;
        // 同 postVsr:fv 不覆盖 post 括号,队尾补 Signal 收口。
        if (const uint64_t sealed = _hdr->Queue().SignalNow()) {
            hdrDoneVal = sealed;
            hdrChainVal = sealed;
        }
        _pipeLedger.Set(slot->hdrColor.Get(), D3D12_RESOURCE_STATE_COMMON); // NGX 写后衰减
    }

    // ---- postA(fg CL:DLSSG 推理)→ TrueHDR 链(逐插值帧)→ postB(转换)----
    // TrueHDR 后置管线(HDR 会话,2026-09-24 用户裁定):postA 只写
    // fgInterp[g](恒 SDR 域),postA 提交后逐 gen 追加 TrueHDR eval 到专用
    // 队列(等 postA 栅栏),postB 等链尾栅栏做全部输出转换/回读。非 HDR
    // 会话:postA = 旧形态(FG 推理 + 逐 gen 转换/回读),无 postB。
    // backbuffer = 管线色 pipe.res(vsrRun → vsrColor,NGX 写后衰减 COMMON,
    // fgBar 显式 COMMON→NSR;hdr-only → outputColor,base 尾已 NSR 直读;
    // 实验 fgHdrInterp → fgBack,由编码 pass 在本 CL 生成 PQ 码域内容),
    // MVecs = PIPE 尺寸稠密运动(motionDense;FG 激活时 follow 被忽略),
    // Depth = 静态零纹理(PIPE≠src 用 PIPE 版)。倍数 M:按序 eval 插值槽
    // 1..M-1(官方 MFG 契约)。播种帧只提交首个 eval(DLSSG.Reset=1,输出
    // 不消费);零光流帧整块跳过。
    ID3D12GraphicsCommandList *fgCl = slot->fgCommandList.Get();
    if (rtxIn && !nrOff && !hdrPostSplit) {
        // 非 HDR 拆分形态(NR 开):base 尾 RTX 输入(outputColor/抗闪烁
        // 稳定帧)已 NSR 化;NSR 读不衰减,归位 COMMON。(hdrPostSplit 会话
        // 不在此归位 —— hdr-only 的 backbuffer 还是 DLSSG 输入,vsrRun 的
        // vsrColor 才是管线色,见 fgBar;归位在 post 尾按 pipe.res 统一做。
        // NR 关直连帧 outputColor 本帧无消费者、从未 NSR 化,同样不做屏障。)
        D3D12_RESOURCE_BARRIER inBack[1]{
            TransitionFromTo(temporalRan ? _d3d12->TemporalOut(*slot)
                                         : _d3d12->OutputColor(*slot),
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_COMMON),
        };
        fgCl->ResourceBarrier(1, inBack);
    }
    if (fgOnFgCl) {
        const bool fgResetEval = _fg->NeedsReset();
        // fgBar:管线色 → NSR 作 DLSSG 输入。vsrColor/outputColor = NGX 写后
        // 衰减 COMMON(hdrPostSplit 形态);无 RTX 的 outputColor = 常规 COMMON。
        // 实验模式(PQ 域)恒不走 fgBar:backbuffer = fgBack(生产于编码
        // pass,非 NGX 衰减路径),legacy+vsr 时 pipe.res 指向 fgBack 而
        // 真正 NGX 衰减的 vsrColor 由 TrueHDR 输入侧 NSR 化、post 尾归位。
        const bool pipeNeedsBar = !hdrLegacyInterp && (vsrRun || !rtxIn);
        // NR 关直连帧的管线色 = inputColor,已在 OF copy CL 转换落 NSR
        // (2947 注 "C1 已落 NSR";C2 侧 3312-3335 同判据)。已 NSR 则免
        // 屏障只对账 —— 此前恒声明 COMMON→NSR,转换帧 StateBefore 失配
        // (debug 层报错,release 静默 no-op)。
        const bool pipeAlreadyNsr = nrOff && !debugPipe && convertedOnNvof;
        if (pipeNeedsBar) {
            _pipeLedger.Expect(pipe.res,
                               pipeAlreadyNsr ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                              : D3D12_RESOURCE_STATE_COMMON,
                               "fgBar");
            if (!pipeAlreadyNsr) {
                D3D12_RESOURCE_BARRIER fgBar[1]{
                    TransitionFromTo(pipe.res,
                                     D3D12_RESOURCE_STATE_COMMON,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                };
                fgCl->ResourceBarrier(1, fgBar);
            }
            _pipeLedger.Set(pipe.res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if (hdrLegacyInterp) {
            // PQ 域插帧编码(TrueHDR 真实帧产物 → DLSSG backbuffer):
            // hdrColor NGX 衰减 COMMON → NSR;fgBack COMMON→UAV 编码 → NSR。
            // 本段在 fg CL 上、SubmitFgFrame 等 TrueHDR 链尾栅栏之后执行,
            // hdrColor 就绪闭合(修复:旧形态 fg CL 直读 hdrColor 只等 vsr/
            // base 栅栏,与 RTX 队列的 TrueHDR eval 存在跨队列竞态)。
            _d3d12->RecordHdrToPq(*fgCl, *slot, D3D12_RESOURCE_STATE_COMMON);
        }
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
            if (fgDstPlanes && !fgDstPlanes[g * 3]) break; // 分配失败槽:整批作废 —— continue 会把后续槽以 g+1 提交成 1,3,4 洞批,违背 MFG "in order starting at 1" 批契约;洞在槽 1 时 _frameId 还不推进,下个源帧复用同 ID(2026-10-05 评审修)
            D3D12_RESOURCE_BARRIER toUav[1]{
                Transition(slot->fgInterp[g].Get(),
                           D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            fgCl->ResourceBarrier(1, toUav);
            if (_fg->Evaluate(fgCl, pipe.res, fgMvec,
                              fgDepth, slot->fgInterp[g].Get(), pipe.w, pipe.h,
                              fgM, g + 1, false, fgErr, sizeof(fgErr))) {
                fgRan = true;
                if (fgResetEval) {
                    // 播种帧:插值输出不消费,fgInterp[g] 归 COMMON。
                    D3D12_RESOURCE_BARRIER fgSeed[1]{
                        Transition(slot->fgInterp[g].Get(),
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COMMON),
                    };
                    fgCl->ResourceBarrier(1, fgSeed);
                    break;
                }
                // 有效插值:fgInterp[g] UAV→NSR。输出转换统一移 post CL
                // (常驻转换段,解耦:fg CL = 纯 DLSSG 推理);NSR 跨段保持,
                // post CL 按消费方转换并收尾归 COMMON。
                D3D12_RESOURCE_BARRIER toNsr[1]{
                    Transition(slot->fgInterp[g].Get(),
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                };
                fgCl->ResourceBarrier(1, toNsr);
                if (fgGenOk) fgGenOk[g] = true;
                ++fgEvaluatedCount;
                // eval 恢复:解除降级日志闩锁,下次新故障重新记一条。
                _fgDupLogged.store(false, std::memory_order_release);
            } else {
                // eval 失败(会话已被闩停):fgInterp[g] 回 COMMON,本帧全部
                // 插值槽降级复制。
                D3D12_RESOURCE_BARRIER undo[1]{
                    Transition(slot->fgInterp[g].Get(),
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
        // postA 尾归位(解耦后只剩 motionDense):管线色/vsrColor 的 NSR
        // 归位与 guidance 归位全部收敛到 post CL 尾 —— fg CL 在此与 real
        // TrueHDR eval(RTX 队列并发读 vsrColor/outputColor)存在时序窗口,
        // post CL 等 TrueHDR 链尾栅栏,归位放那里才闭合竞态。
        if (slot->motionDense) {
            D3D12_RESOURCE_BARRIER msBack[1]{
                TransitionFromTo(slot->motionDense.Get(),
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_COMMON),
            };
            fgCl->ResourceBarrier(1, msBack);
        }
    }
    // 真实帧输出转换(C2)已全部移 post CL(下方统一转换段):此前粘在
    // postA(VSR-only 时被错记进 fg 段)/ base CL,HDR 拆分形态才走 postB。
    // postA 提交:等待 vsr 产出(vsrRun)或 base(hdr-only 的 DLSSG
    // backbuffer = outputColor/inputColor,等 base 尾 NSR 化完成)。
    // 冲刷点 C(兜底):NR 关 + FG 时 fg CL 的 DLSSG MVecs 是首个 motion
    // 消费者 —— 在 fg CL 提交前冲刷,CPU 等引擎与 fg CL 录制(DLSSG 逐槽
    // eval)重叠,这是本次延迟改造在 nrOff+FG 会话的主要收益位。其余路径
    // (无消费者/已被 B 冲刷)幂等即刻返回。
    flushOfDensify();
    if (fgBeginOk) {
        // 实验模式(PQ 域)的 fg CL 首消费者 = 编码 pass 读 hdrColor(TrueHDR
        // 专用队列产出)—— 必须等 HDR 链尾栅栏(vsr 经 RTX 链传递闭合);
        // 其余形态 backbuffer = vsrColor/base 产物,等各自生产者。门关帧
        // (fgOnFgCl=false)的 fg CL 只剩 inBack 屏障,无 hdrColor 依赖不白等。
        const bool fgWaitHdr = fgOnFgCl && hdrLegacyInterp && hdrDoneFence;
        if (!_d3d12->SubmitFgFrame(*slot,
                                   fgWaitHdr ? hdrDoneFence
                                             : (vsrRun ? vsrDoneFence : _d3d12->Fence()),
                                   fgWaitHdr ? hdrDoneVal
                                             : (vsrRun ? vsrDoneVal : slot->baseFenceValue),
                                   err, errLen)) {
            if (err && errLen) {
                TimingStatusLine(err);
                std::snprintf(err, errLen, "Submit(fg) failed");
            }
            return false;
        }
        guard.ArmDrainShared(slot->fgFenceValue); // 失败路径排空含在飞 fg CL
    }
    // 插值帧 TrueHDR 链(HDR 会话):逐 evaluated gen 追加到专用队列,全部
    // 等 postA 栅栏(同队列 FIFO 保序);链尾栅栏 = postB 消费锚 + 计时锚。
    // 仅 hdrPostSplit 形态(实验模式无插值帧 TrueHDR)。
    if (hdrPostSplit && fgOnFgCl && fgGenOk) {
        // 单锁包整链(此前逐笔加锁):括号(10/11)与 eval 的 FIFO 顺序靠
        // 同锁程序序;Submit 已被 fgMutex 串行,无并发竞争者。preGen 惰性
        // 提交(首个有效 gen 才开括号),全跳过帧不白提交。
        std::lock_guard<std::mutex> rtxLock(_evaluateMutex);
        bool genBracketOpen = false;
        for (int g = 0; g < fgM - 1; ++g) {
            if (!fgGenOk[g]) continue;
            if (!genBracketOpen) {
                _d3d12->SubmitTsBracket(*slot, _hdr->Queue().Native(),
                                        slot->ts[kTsPreGen].alloc.Get(),
                                        slot->ts[kTsPreGen].cl.Get(), kTsPreGen,
                                        _d3d12->Fence(), slot->fgFenceValue);
                guard.ArmDrainQueue(&_hdr->Queue());
                genBracketOpen = true;
            }
            char rtxErr[160]{};
            uint64_t fv = 0;
            if (!_hdr->Evaluate(slot->fgInterp[g].Get(), pipe.w, pipe.h,
                                slot->hdrFg[g].Get(),
                                rtxLive.rtxHdrContrast, rtxLive.rtxHdrSaturation,
                                rtxLive.rtxHdrMiddleGray, rtxLive.rtxHdrMaxLuminance,
                                _d3d12->Fence(), slot->fgFenceValue, &fv,
                                rtxErr, sizeof(rtxErr))) {
                if (err && errLen) std::snprintf(err, errLen, "%.180s", rtxErr);
                // 槽释放排空归 SlotGuard(fg/RTX 队列已在上方武装,析构统一
                // 有界等待 —— 原 2026-09-25 的本地等待已收口)。
                return false;
            }
            hdrDoneFence = _hdr->Queue().Fence();
            hdrDoneVal = fv;
            hdrChainFence = hdrDoneFence;
            hdrChainVal = fv;
            _pipeLedger.Set(slot->hdrFg[g].Get(), D3D12_RESOURCE_STATE_COMMON); // NGX 写后衰减
        }
        if (genBracketOpen) {
            _d3d12->SubmitTsBracket(*slot, _hdr->Queue().Native(),
                                    slot->ts[kTsPostGen].alloc.Get(),
                                    slot->ts[kTsPostGen].cl.Get(), kTsPostGen, nullptr, 0);
            // 同 postVsr:链尾 fv 不覆盖 postGen 括号,队尾补 Signal 收口。
            if (const uint64_t sealed = _hdr->Queue().SignalNow()) {
                hdrDoneVal = sealed;
                hdrChainVal = sealed;
            }
        }
    }
    if (!fgBeginOk) {
        // 门关帧(面板关/诊断跳过/播种/零光流/fg CL 起点失败)不向 proxy
        // 提交 eval:其内部 backbuffer 历史滞留在跳过前 —— 置位重置让恢复
        // 帧重新播种(播种帧插值输出照常降级复制),否则"关→开"/播种后的
        // 首个 eval 用陈旧历史插出鬼影。逐帧置位无害(布尔位;会话已死时
        // Enabled() 为 false 不进此分支)。(解耦后无栅栏回退:post CL 恒
        // 提交且恒最后,slot->fenceValue = postFenceValue。)
        if (_fg && _fg->Enabled()) _fg->ResetHistory();
    }

    // ---- post(常驻输出转换段;解耦:C2 独立成段,记账归 conv)----
    // 全部输出转换(真实帧 + 逐 gen)统一在此:此前真实帧转换粘在 base/
    // fg CL(VSR-only 时被错记进 fg 段)、HDR 拆分形态才走 postB —— 现在
    // 三形态统一。每帧恒提交,slot->fenceValue 恒 = postFenceValue,
    // WaitFrame(t3b)语义不变(全部 readback 完成点)。
    if (!_d3d12->BeginPostRecording(*slot)) {
        if (err && errLen) std::snprintf(err, errLen, "post CL begin failed");
        TimingStatusLine("DLSSNR STATUS: post CL begin FAILED");
        return false;
    }
    auto *postCl = slot->postCommandList.Get();
    {
        // 真实帧 C2(源 = 管线色;stateBefore 按生产者落点):
        //   hdrPostSplit   = hdrColor(TrueHDR 写后衰减 COMMON)
        //   legacy(PQ 域) = hdrColor(编码 pass 已 NSR 化;管线色 fgBack 是
        //                    DLSSG backbuffer,不是转换源)
        //   vsr            = vsrColor(fgOnFgCl 时 fgBar 转 NSR,否则 NGX 衰减
        //                    COMMON —— 统一 NSR 化,收尾归 COMMON)
        //   NR 关直连      = inputColor(C1 落 COMMON,槽 0 SRV)
        //   NR 开无 RTX    = outputColor(fgOnFgCl 消费后归 COMMON,否则 UAV)
        if (hdrPostSplit) {
            _pipeLedger.Expect(slot->hdrColor.Get(), D3D12_RESOURCE_STATE_COMMON, "post C2 hdr(split)");
            _d3d12->RecordColorOutput(*postCl, *slot, slot->hdrColor.Get(),
                                      D3D12Context::kSrvHdrColor,
                                      ColorOutKind::HdrScRgb,
                                      pipe.w, pipe.h, matrix, range,
                                      D3D12_RESOURCE_STATE_COMMON);
            _pipeLedger.Set(slot->hdrColor.Get(), D3D12_RESOURCE_STATE_COMMON);
        } else if (hdrRun) {
            _pipeLedger.Expect(slot->hdrColor.Get(),
                               fgOnFgCl ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                        : D3D12_RESOURCE_STATE_COMMON,
                               "post C2 hdr");
            _d3d12->RecordColorOutput(*postCl, *slot, slot->hdrColor.Get(),
                                      D3D12Context::kSrvHdrColor,
                                      ColorOutKind::HdrScRgb,
                                      pipe.w, pipe.h, matrix, range,
                                      fgOnFgCl ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                               : D3D12_RESOURCE_STATE_COMMON);
            _pipeLedger.Set(slot->hdrColor.Get(), D3D12_RESOURCE_STATE_COMMON);
        } else if (vsrRun) {
            _pipeLedger.Expect(slot->vsrColor.Get(),
                               fgOnFgCl ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                        : D3D12_RESOURCE_STATE_COMMON,
                               "post C2 vsr");
            _d3d12->RecordColorOutput(*postCl, *slot, slot->vsrColor.Get(),
                                      D3D12Context::kSrvVsrColor,
                                      ColorOutKind::Sdr,
                                      pipe.w, pipe.h, matrix, range,
                                      fgOnFgCl ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                               : D3D12_RESOURCE_STATE_COMMON);
            _pipeLedger.Set(slot->vsrColor.Get(), D3D12_RESOURCE_STATE_COMMON);
        } else if (nrOff) {
            // pipe.res = inputColor(直连)/ debugPipe 帧 = outputColor
            // (光流场调试图 @src)。stateBefore 按实际落点:
            // 直连帧:OF 会话已转换(inputIndex>=0,FFX/NVOF copy CL)= NSR;
            // 未转换(OF 关/失败)= 槽 CL RecordConvertInput 的 stateAfter
            // (uploadPost)。此前恒 COMMON,转换帧对已 NSR 的 inputColor
            // 记错 from-state(2026-09-25 审查)。
            // debugPipe 帧:outputColor 是 NGX 共享纹理,跨 ECL 隐式衰减
            // COMMON(fgBar 的 NSR 化随 fg CL 完成同样衰减)—— post CL 恒
            // 从 COMMON 对接,与 NR 开无 RTX 帧的 outputColor 同契约。
            _pipeLedger.Expect(pipe.res,
                               debugPipe ? D3D12_RESOURCE_STATE_COMMON
                                         : (convertedOnNvof ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                                            : uploadPost),
                               "post C2 nrOff");
            _d3d12->RecordColorOutput(*postCl, *slot, pipe.res,
                                      debugPipe ? D3D12Context::kSrvOutputColor
                                                : static_cast<UINT>(HeapSlot::SrvInput),
                                      ColorOutKind::Sdr,
                                      width, height, matrix, range,
                                      debugPipe ? D3D12_RESOURCE_STATE_COMMON
                                                : (convertedOnNvof ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                                                   : uploadPost));
            _pipeLedger.Set(pipe.res, D3D12_RESOURCE_STATE_COMMON);
        } else if (_rtxActive) {
            // 抗闪烁帧:稳定帧由 RecordTemporal 显式 COMMON 落位,base 尾
            // NSR 化(若 DLSSG 消费)后经 inBack 归位 —— 恒 COMMON,与 FG
            // 无关(常规帧的 COMMON|UAV 分叉来自 NGX 衰减语义,不适用)。
            if (temporalRan) {
                _pipeLedger.Expect(_d3d12->TemporalOut(*slot), D3D12_RESOURCE_STATE_COMMON,
                                   "post C2 rtx temporal");
                _d3d12->RecordColorOutput(*postCl, *slot,
                                          _d3d12->TemporalOut(*slot), D3D12Context::kSrvTemporalOut,
                                          ColorOutKind::Sdr, width, height, matrix, range,
                                          D3D12_RESOURCE_STATE_COMMON);
                _pipeLedger.Set(_d3d12->TemporalOut(*slot), D3D12_RESOURCE_STATE_COMMON);
            } else {
                _pipeLedger.Expect(_d3d12->OutputColor(*slot),
                                   fgOnFgCl ? D3D12_RESOURCE_STATE_COMMON
                                            : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   "post C2 rtx output");
                _d3d12->RecordColorOutput(*postCl, *slot,
                                          _d3d12->OutputColor(*slot), D3D12Context::kSrvOutputColor,
                                          ColorOutKind::Sdr, width, height, matrix, range,
                                          fgOnFgCl ? D3D12_RESOURCE_STATE_COMMON
                                                   : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                _pipeLedger.Set(_d3d12->OutputColor(*slot), D3D12_RESOURCE_STATE_COMMON);
            }
        } else {
            // NR 开无 RTX:常规帧 = outputColor(FG 消费后 COMMON / 未消费
            // UAV);抗闪烁帧 = 稳定帧(RecordTemporal 落 COMMON;FG 激活时
            // fgBar 已 NSR 化作 DLSSG backbuffer —— NSR 直读免屏障,收尾由
            // 本转换归位 COMMON)。
            _pipeLedger.Expect(temporalRan ? _d3d12->TemporalOut(*slot)
                                           : _d3d12->OutputColor(*slot),
                               temporalRan ? (fgOnFgCl ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                                       : D3D12_RESOURCE_STATE_COMMON)
                                           : (fgOnFgCl ? D3D12_RESOURCE_STATE_COMMON
                                                       : D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                               "post C2 no-rtx");
            _d3d12->RecordColorOutput(*postCl, *slot,
                                      temporalRan ? _d3d12->TemporalOut(*slot)
                                                  : _d3d12->OutputColor(*slot),
                                      temporalRan ? D3D12Context::kSrvTemporalOut
                                                  : D3D12Context::kSrvOutputColor,
                                      ColorOutKind::Sdr, _width, _height, matrix, range,
                                      temporalRan ? (fgOnFgCl ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                                              : D3D12_RESOURCE_STATE_COMMON)
                                                  : (fgOnFgCl ? D3D12_RESOURCE_STATE_COMMON
                                                              : D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
            _pipeLedger.Set(temporalRan ? _d3d12->TemporalOut(*slot)
                                        : _d3d12->OutputColor(*slot),
                            D3D12_RESOURCE_STATE_COMMON);
        }
        if (!_d3d12->RecordReadbackCopy(*postCl, *slot, err, errLen)) {
            return false;
        }
        // 逐 gen C2:hdrPostSplit = hdrFg[g] PQ 转换(TrueHDR 产物)+
        // fgInterp[g] 归位;legacy/非 HDR = fgInterp[g] 直接转换(fg CL 只
        // NSR 化;实验模式 = FP16 PQ 码直读,免逐像素 PqEncode)。
        if (fgOnFgCl && fgGenOk) {
            for (int g = 0; g < fgM - 1; ++g) {
                if (!fgGenOk[g]) continue;
                if (hdrPostSplit) {
                    _pipeLedger.Expect(slot->hdrFg[g].Get(), D3D12_RESOURCE_STATE_COMMON,
                                       "post gen C2 hdrFg");
                    _d3d12->RecordColorOutput(*postCl, *slot, slot->hdrFg[g].Get(),
                                              D3D12Context::kSrvHdrFgBase + g,
                                              ColorOutKind::HdrScRgb,
                                              pipe.w, pipe.h, matrix, range,
                                              D3D12_RESOURCE_STATE_COMMON);
                    _pipeLedger.Set(slot->hdrFg[g].Get(), D3D12_RESOURCE_STATE_COMMON);
                    if (!_d3d12->RecordReadbackCopy(*postCl, *slot, err, errLen, g)) {
                        return false;
                    }
                    // postA 只 NSR 化未归位的 fgInterp[g](TrueHDR 已消费)归位。
                    D3D12_RESOURCE_BARRIER fiBack[1]{
                        TransitionFromTo(slot->fgInterp[g].Get(),
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                         D3D12_RESOURCE_STATE_COMMON),
                    };
                    postCl->ResourceBarrier(1, fiBack);
                } else {
                    // 统一走 RecordColorOutput(2026-10-04,RecordYuvOutput 已
                    // 并入):!_rtxActive 时 pipe.w/h == 源尺寸,1:1 判据自动
                    // 选直写 PSO,与旧 Yuv 直写路径逐位同价。
                    _d3d12->RecordColorOutput(*postCl, *slot, slot->fgInterp[g].Get(),
                                              D3D12Context::kSrvFgInterpBase + g,
                                              hdrLegacyInterp ? ColorOutKind::HdrPqCodes
                                                              : ColorOutKind::Sdr,
                                              pipe.w, pipe.h, matrix, range,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    if (!_d3d12->RecordReadbackCopy(*postCl, *slot, err, errLen, g)) {
                        return false;
                    }
                }
            }
        }
        // post 尾归位(全部 NSR→COMMON):
        //  - DLSSG backbuffer 管线色(fgOnFgCl 帧):HDR 会话 vsrColor /
        //    hdr-only 拆分形态的 outputColor|inputColor —— post 不消费它们;
        //  - NR 关直连的 inputColor(= rtxColorIn,TrueHDR 消费过):hdr-only
        //    拆分形态它就是管线色(上一分支已归位),不重复。
        // 其余形态的管线色由 C2 消费,RecordColorOutput 自带
        // 收尾归位。post 等 TrueHDR 链尾栅栏 —— 这些归位与 RTX 队列并发读
        // 的竞态窗口在此闭合(原 fg CL 尾归位的既有竞态顺带修复)。
        if (fgOnFgCl) {
            if (vsrRun && hdrRun) {
                D3D12_RESOURCE_BARRIER vsrBack[1]{
                    TransitionFromTo(slot->vsrColor.Get(),
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                     D3D12_RESOURCE_STATE_COMMON),
                };
                postCl->ResourceBarrier(1, vsrBack);
                // hdrPostSplit+vsr:VSR 输入(base 尾 NSR 化,抗闪烁帧 =
                // 稳定帧)在 inBack(3038)被排除、门关帧才走 3484 补归位
                // —— 门开帧此前无人归位,NSR 跨帧滞留,下帧 base 尾
                // UAV→NSR StateBefore 失配(与 3484 已修门关帧同族)。
                if (hdrPostSplit) {
                    D3D12_RESOURCE_BARRIER vsrInBack[1]{
                        TransitionFromTo(temporalRan ? _d3d12->TemporalOut(*slot)
                                                     : _d3d12->OutputColor(*slot),
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                         D3D12_RESOURCE_STATE_COMMON),
                    };
                    postCl->ResourceBarrier(1, vsrInBack);
                }
            } else if (hdrPostSplit) {
                D3D12_RESOURCE_BARRIER fgBack[1]{
                    TransitionFromTo(pipe.res,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                     D3D12_RESOURCE_STATE_COMMON),
                };
                postCl->ResourceBarrier(1, fgBack);
            }
            if (hdrLegacyInterp) {
                // PQ 域插帧:DLSSG backbuffer(fgBack,编码 pass NSR 化)本帧
                // 消费完毕,归位 COMMON 供下帧编码 pass 的 COMMON→UAV 屏障。
                D3D12_RESOURCE_BARRIER fgBackPark[1]{
                    TransitionFromTo(slot->fgBack.Get(),
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                     D3D12_RESOURCE_STATE_COMMON),
                };
                postCl->ResourceBarrier(1, fgBackPark);
            }
        }
        recordGuidancePark(*postCl);
        // hdr-only 拆分形态且 FG 门开时 pipe.res == inputColor 已在上方
        // fgBack 分支归位,此处跳过防双归位;门关帧(3376 块整体跳过)仍需
        // 在此归位 —— 旧条件不分门态,门关帧 NSR 态跨帧滞留,下帧 C1
        // COMMON→UAV 屏障 StateBefore 失配(2026-10-04 评审修)。
        if (nrOff && rtxIn && !(fgOnFgCl && hdrPostSplit && !vsrRun)) {
            D3D12_RESOURCE_BARRIER inBack[1]{
                TransitionFromTo(slot->inputColor.Get(),
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_COMMON),
            };
            postCl->ResourceBarrier(1, inBack);
        }
        // hdr-only 拆分形态 NR 开:DLSSG backbuffer = outputColor,base 尾
        // NSR 化(TrueHDR 消费)。其归位此前只挂 fgOnFgCl 分支(上方 fgBack)
        // —— 而本形态门关帧连 fg CL 都不开(2788 postBeginReq 明确排除
        // hdrPostSplit),outputColor NSR 态跨帧滞留,下帧 base 尾 UAV→NSR
        // 屏障 StateBefore 失配(2026-10-05 评审修;与上方 nrOff 门关
        // inputColor 滞留 2026-10-04 修同族,含 fg CL begin 失败降级帧)。
        // 资源与 base 尾 NSR 化同款选择(抗闪烁帧 = TemporalOut;
        // outputColor 在抗闪烁帧停 COMMON,对它录 NSR→COMMON 反而失配)。
        if (!fgOnFgCl && hdrPostSplit && rtxIn && !nrOff) {
            D3D12_RESOURCE_BARRIER outBack[1]{
                TransitionFromTo(temporalRan ? _d3d12->TemporalOut(*slot)
                                             : _d3d12->OutputColor(*slot),
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_COMMON),
            };
            postCl->ResourceBarrier(1, outBack);
        }
        // legacy 形态 fg CL begin 失败帧:inBack 归位录进了永不提交的
        // fg CL(2796 注契约"RTX 帧无 inBack 落点 = 整帧失败"未实现,
        // begin 失败只降级门关)—— 此处补归位,与 3484/上方门开支路
        // 同账。门开成功帧 inBack 已在 fg CL 落账,勿双归位。
        if (!fgBeginOk && postBeginReq && rtxIn && !nrOff && !hdrPostSplit) {
            D3D12_RESOURCE_BARRIER inBackLate[1]{
                TransitionFromTo(temporalRan ? _d3d12->TemporalOut(*slot)
                                             : _d3d12->OutputColor(*slot),
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_COMMON),
            };
            postCl->ResourceBarrier(1, inBackLate);
        }
    }
    // post 提交(跨队列生产者双等待,排队在 Execute 前):
    //   hdrPostSplit → A = TrueHDR 链尾(传递覆盖 fg/vsr/base),B 空;
    //   legacy HDR   → A = real TrueHDR 完成,B = fg CL(fgInterp 转换输入);
    //   其余         → A = 最后颜色生产者(vsrRun ? vsr : base),
    //                  B = fg CL(fgFence 传递覆盖 A,双等幂等无害)。
    ID3D12Fence *postWaitA = nullptr;
    uint64_t postWaitAVal = 0;
    ID3D12Fence *postWaitB = nullptr;
    uint64_t postWaitBVal = 0;
    if (hdrPostSplit) {
        postWaitA = hdrChainFence;
        postWaitAVal = hdrChainVal;
    } else if (hdrRun) {
        postWaitA = hdrDoneFence;
        postWaitAVal = hdrDoneVal;
        if (fgBeginOk) {
            postWaitB = _d3d12->Fence();
            postWaitBVal = slot->fgFenceValue;
        }
    } else {
        postWaitA = vsrRun ? vsrDoneFence : _d3d12->Fence();
        postWaitAVal = vsrRun ? vsrDoneVal : slot->baseFenceValue;
        if (fgBeginOk) {
            postWaitB = _d3d12->Fence();
            postWaitBVal = slot->fgFenceValue;
        }
    }
    if (!_d3d12->SubmitPostFrame(*slot, postWaitA, postWaitAVal, postWaitB, postWaitBVal,
                                 err, errLen)) {
        if (err && errLen) {
            TimingStatusLine(err);
            std::snprintf(err, errLen, "Submit(post) failed");
        }
        // 槽释放排空归 SlotGuard(base/fg 已武装;post Close 失败 = post CL
        // 未入队,无需等 post 值 —— 原本地等待对门关帧等陈旧 fgFenceValue,
        // 已随守卫化修复)。
        return false;
    }
    guard.ArmDrainShared(slot->fenceValue); // post 值恒最后(≥fg≥base),覆盖全部直队列提交
    // Submit 半段收尾(2026-09-25 FG 持锁窗口缩小):把等待/unpack/stats
    // 所需状态打包进续体并移交槽所有权。defer 模式在此返回(调用方释放
    // 其串行锁后调 ProcessFrameFinish);同步模式就地完成后半段。
    auto packFinish = [&]() -> FrameFinish * {
        auto *ff = new (std::nothrow) FrameFinish();
        if (!ff) return nullptr;
        ff->slot = slot;
        // Finish 在途票据:本续体持裸槽指针直到消费完,资源重建必须等它
        // (见 FrameFinish::ticketCtx 注释)。取号先于移交 —— 失败路径
        // delete 本体时析构销号,票据不悬空。
        ff->ticketCtx = _d3d12;
        ff->ticketCtx->BeginFrameFinishTicket();
        guard.handoff = true; // 槽所有权移交(含失败路径:Finish 负责释放)
        ff->qpcFreq = qpcFreq;
        ff->t0 = t0; ff->t1 = t1;
        ff->t2 = t2; ff->t3a = t3a; ff->tSub0 = tSub0;
        ff->tSlot0 = tSlot0; ff->tSlot1 = tSlot1;
        ff->tLock0 = tLock0; ff->tLock1 = tLock1;
        ff->vsrDoneFence = vsrDoneFence; ff->vsrDoneVal = vsrDoneVal;
        ff->hdrDoneFence = hdrDoneFence; ff->hdrDoneVal = hdrDoneVal;
        ff->fgBeginOk = fgBeginOk; ff->nrOff = nrOff; ff->skipEval = skipEval;
        ff->hdrPostSplit = hdrPostSplit; ff->hdrRun = hdrRun;
        ff->dumpEnabled = dumpEnabled; ff->realMotion = realMotion;
        ff->evalZeroed = evalZeroed; ff->rtxIn = rtxIn;
        ff->ofNeeded = ofNeeded; ff->fgRan = fgRan;
        ff->fgM = fgM; ff->fgEvaluatedCount = fgEvaluatedCount;
        ff->nvofInputIndex = nvofInputIndex;
        // nvof 段上报值 = 光流阶段全跨度(2026-09-25 语义修正):门入口 →
        // 冲刷完成 = 提交 + 引擎计算 + 暴露等待,即真实光流处理用时。此处
        // 读取时冲刷已落位(点 A/B/C 均在 packFinish 前),fgMutex 下无跨帧
        // 污染。ff->nvofMs 保持 StageFrame 提交成本(eval_cpu 窗口扣减用)。
        ff->nvofSpanMs = (ofNeeded && _ofBackend) ? _ofBackend->LastStageTotalMs() : 0.0;
        ff->ofEngineMs = ofEngineMs;
        return ff;
    };
    if (deferMode) {
        FrameFinish *ff = packFinish();
        if (!ff) {
            if (err && errLen) std::snprintf(err, errLen, "oom: frame finish continuation");
            return false; // SlotGuard 释放槽,与其它失败路径同语义
        }
        *deferOut = ff;
        return true;
    }
    FrameFinish *ffSync = packFinish();
    if (!ffSync) {
        if (err && errLen) std::snprintf(err, errLen, "oom: frame finish continuation");
        return false;
    }
    const bool rbSync = ProcessFrameFinish(ffSync, dstPlanes, dstStrides,
                                           fgDstPlanes, fgDstStrides, fgGenOk,
                                           err, errLen, timingOut, timingLen);
    return rbSync;
}
// 拆分模式后半(2026-09-25 FG 持锁窗口缩小):全部栅栏有界等待 + unpack
// (真实帧 + 逐 gen)+ dump/stats 段 + 槽释放(含 OF 排空)。调用方在释放
// 其串行锁后调用恰好一次;成功 = 帧内容就绪,失败 = 调用方对本帧输出做
// 源拷贝兜底(帧已入缓存,可能已被消费方领引用,尚未交付)。
bool DlssnrContext::ProcessFrameFinish(FrameFinish *ff,
                                       uint8_t **dstPlanes, int64_t *dstStrides,
                                       uint8_t **fgDstPlanes, int64_t *fgDstStrides,
                                       bool *fgGenOk,
                                       char *err, size_t errLen,
                                       char *timingOut, size_t timingLen) noexcept {
    // 续体所有权归 Finish(2026-09-25 修复逐帧泄漏:此前任何出口都不释放,
    // 同步/拆分两路径每帧稳漏)。任何出口(含失败提前 return)在此统一释放。
    struct FinishSelfDelete { FrameFinish *ff; ~FinishSelfDelete() { delete ff; } };
    FinishSelfDelete ffGuard{ff};
    const bool vsTiming = timingOut && timingLen > 0;
    // t3b/t3c/t4 仅在本半段采样(WaitFrame 后/真实帧 unpack 后/插值帧 unpack 后)。
    LARGE_INTEGER t3b{}, t3c{}, t4{};
    // Finish 持有槽所有权(Submit 半段移交):本半段任何出口(含失败提前
    // return)都排空 + 释放。OF 排空在释放前 —— FFX 的 upload 堆 CPU 写/
    // GPU 读竞争排空点(WaitCopyIdle 承重化);NVOF 门内 execute CPU 等已
    // 覆盖,此处常态即刻返回。
    struct FinishSlotGuard {
        D3D12Context *ctx;
        FrameSlot *s;
        IOpticalFlowBackend *of;
        bool drain;
        ~FinishSlotGuard() {
            if (of && drain) of->WaitCopyIdle();
            if (s) ctx->ReleaseSlot(s);
        }
    } fguard{ _d3d12, ff->slot, _ofBackend.get(), ff->ofNeeded };
    // RTX 分段拆账:CPU 有界等待专用队列完成栅栏(fence 链 base→vsr→postA
    // →hdr 链;起点 ff->t3a 已在 base 提交后立即观测,见上)。post CL 本就
    // Wait 链尾 fence —— 提前 CPU 等待不改变提交时序,只把墙钟拆回各段
    // 名下。10s 上限与 WaitFrame 同级:专用队列 wedge 时等待会超时落空,
    // 帧的最终判定仍归 WaitFrame 既有失败路径,此处不闩错。
    QueryPerformanceCounter(&ff->tSub1);
    const double subWaitMs = ff->rtxIn ? (ff->tSub1.QuadPart - ff->tSub0.QuadPart) * 1000.0 / ff->qpcFreq.QuadPart
                                   : 0.0;
    LARGE_INTEGER tVsrDone = ff->t3a, tHdrDone = ff->t3a, tFgDone = ff->t3a;
    if (ff->vsrDoneFence) {
        _vsr->Queue().Wait(ff->vsrDoneVal, nullptr, 0, 10000);
        QueryPerformanceCounter(&tVsrDone);
    }
    // postA(fg CL 栅栏 ≤ post CL 栅栏 ≤ WaitFrame 目标栅栏,必不后完成 ——
    // 等待近零开销;复用 ff->slot.fenceEvent 与 WaitFrame 串行,无并发)。FG 未
    // 提交(门关)帧不采样:fgFenceValue 是上一帧的陈旧值。
    if (ff->fgBeginOk) {
        _d3d12->WaitFenceValuePublic(ff->slot->fgFenceValue, ff->slot->fenceEvent, nullptr, 0);
        QueryPerformanceCounter(&tFgDone);
    }
    if (ff->hdrDoneFence) {
        _hdr->Queue().Wait(ff->hdrDoneVal, nullptr, 0, 10000);
        QueryPerformanceCounter(&tHdrDone);
    }
    if (!_d3d12->WaitFrame(*ff->slot, err, errLen)) {
        if (_d3d12->IsDeviceLost()) _ready.store(false, std::memory_order_release);
        if (err && errLen) {
            TimingStatusLine(err);
            std::snprintf(err, errLen, "Wait(fg) failed");
        }
        return false;
    }
    if (ProbeEnabled()) TimingStatusLine("PROBE: waited"); // 探针(VSDLSSNR_PROBE=1)
    QueryPerformanceCounter(&t3b);
    // t3a 观测点(2026-09-25 时间戳化):base 完成的 GPU 时间戳已在 ts CL
    // 执行时写入 READBACK(post FIFO 在 ts 后,WaitFrame 蕴含数据就绪)。
    // 换算:GetClockCalibration 校准点 + (tsEnd − gpuCal) 按 GPU 频率 →
    // QPC 刻度。提交链与 base GPU 重叠后,阻塞观测会把 vsr/fg 段压成 0
    // (2026-09-24 实测根因),时间戳免阻塞且精确。兜底默认 t3a := t2
    // (gpu 段记 0,总量恒真):覆盖换算失败场景 —— 超快空 CL(nrOff)在
    // 校准点采样前完成(tsEnd ≤ gpuCal)/时钟关联异常,不会让面板出现
    // 负值/开机以来巨值。
    if (ff->slot->tsValid) {
        ff->t3a = ff->t2;
        // postBase 括号 = 索引 1(索引 0 已被 preBase 占用,见 d3d12_context.h 布局)
        const UINT64 tsEnd = static_cast<const UINT64 *>(ff->slot->tsReadback.mapped)[1];
        if (tsEnd > ff->slot->tsGpuCal) {
            const double deltaQpc =
                static_cast<double>(tsEnd - ff->slot->tsGpuCal) / _d3d12->GpuTsFreq() *
                static_cast<double>(ff->qpcFreq.QuadPart);
            LARGE_INTEGER t3aTs{};
            t3aTs.QuadPart = ff->slot->tsCpuCal + static_cast<LONGLONG>(deltaQpc);
            if (t3aTs.QuadPart > ff->t2.QuadPart) ff->t3a = t3aTs;
        }
    }
    // 时间戳括号账目(2026-09-25 账目诚实化):每段只记自己名下的 GPU 执行
    // 时间 —— 段间/帧间排队(queue)与冲刷点引擎等待(of_engine)单独成段,
    // 不折进任何处理段。段差异用 GPU tick 直接换算(同钟,免校准);排队
    // 需要绝对墙钟锚(preBase − 提交时刻),走校准点换算。
    double baseGpuMs = 0.0, queueWaitMs = 0.0, fgGpuMs = 0.0, convGpuMs = 0.0;
    double rtxVsrGpuMs = 0.0, rtxHdrRealMs = 0.0, rtxHdrGenMs = 0.0;
    const bool tsAcc = ff->slot->tsValid;
    const UINT64 *tsq = ff->slot->tsReadback.mapped
                            ? static_cast<const UINT64 *>(ff->slot->tsReadback.mapped)
                            : nullptr;
    const double tickMs = 1000.0 / _d3d12->GpuTsFreq();
    // 括号差分防线(2026-10-02):readback 帧首已清零(SubmitBaseFrame),
    // 格子为 0 = 本帧该括号录制失败(无账);负差分 = 时钟异常;两者钳 0,
    // 杜绝新旧混搭的虚高/无符号回绕巨值。正常路径与裸差分完全一致。
    const auto tickDiffMs = [&](UINT64 a, UINT64 b) -> double {
        if (!a || !b) return 0.0;
        const INT64 d = static_cast<INT64>(b) - static_cast<INT64>(a);
        return d > 0 ? static_cast<double>(d) * tickMs : 0.0;
    };
    if (tsAcc) {
        baseGpuMs = tickDiffMs(tsq[0], tsq[1]);
        convGpuMs = tickDiffMs(tsq[4], tsq[5]);
        if (ff->fgBeginOk) fgGpuMs = tickDiffMs(tsq[2], tsq[3]);
        // RTX 段括号(2026-10-02):vsr/hdr 与常驻段同语义 —— GPU 纯执行,
        // 提交链遮盖/队列空闲不入账(CPU-bound 帧不再显示假 0,九段语义
        // 统一);hdr = 真实帧括号(8/9)+ 插值链括号(10/11)之和,两段间
        // 等 postA 的队列空闲天然留在括号外。括号未提交(段未跑/失败帧)
        // 索引保持帧首清零,记 0。
        rtxVsrGpuMs = tickDiffMs(tsq[6], tsq[7]);
        rtxHdrRealMs = tickDiffMs(tsq[8], tsq[9]);
        rtxHdrGenMs = tickDiffMs(tsq[10], tsq[11]);
        const double qpcPerGpuTick =
            static_cast<double>(ff->qpcFreq.QuadPart) / _d3d12->GpuTsFreq();
        const double preBaseQpc = static_cast<double>(ff->slot->tsCpuCal) +
            static_cast<double>(static_cast<LONGLONG>(tsq[0]) -
                                static_cast<LONGLONG>(ff->slot->tsGpuCal)) * qpcPerGpuTick;
        queueWaitMs = (preBaseQpc - static_cast<double>(ff->slot->submitQpc)) *
                      1000.0 / static_cast<double>(ff->qpcFreq.QuadPart);
        if (queueWaitMs < 0.0) queueWaitMs = 0.0; // 时钟换算噪声钳零
    }
    // TS-INLINE 探针读账(2026-10-02):索引 14/15 = eval 前后内联章(帧首已
    // 随整本清零),等待已由 WaitFrame 蕴含。每帧一行 —— 计数证据:ok 行数
    // = 内联组合存活帧数;NO-TS = 本帧没走 eval(播种/直通);SEH/设备移除
    // 在别处留痕(帧路径 eval 域与 WaitFrame 失败路径)。
    if (TsInlineProbeEnabled() && ff->slot->tsReadback.mapped) {
        const UINT64 *tsqP = static_cast<const UINT64 *>(ff->slot->tsReadback.mapped);
        char pbuf[128];
        if (tsqP[14] && tsqP[15] && tsqP[15] > tsqP[14]) {
            std::snprintf(pbuf, sizeof(pbuf), "TS-INLINE: f=%d ok delta=%.3fms",
                          _lastFrameN.load(std::memory_order_relaxed),
                          static_cast<double>(tsqP[15] - tsqP[14]) * 1000.0 /
                              _d3d12->GpuTsFreq());
        } else {
            std::snprintf(pbuf, sizeof(pbuf), "TS-INLINE: f=%d NO-TS t14=%llu t15=%llu",
                          _lastFrameN.load(std::memory_order_relaxed),
                          static_cast<unsigned long long>(tsqP[14]),
                          static_cast<unsigned long long>(tsqP[15]));
        }
        TimingStatusLine(pbuf);
    }
    const bool rb = _d3d12->UnpackOutput(*ff->slot, dstPlanes, dstStrides, _outW, _outH, err, errLen);
    ff->rbOk = rb;
    // unpack 段 = 真实帧回读(t3b→t3c);插值帧回读(t3c→t4)是 FG 的
    // 输出搬运成本,归 fg 段 —— FG 4x 时 4 帧 P10 @OUT 几何可达 100MB+,
    // 混在 unpack 里会让"解包"凭空翻倍而"补帧"恒 0(2026-09-24 用户
    // 实测定案)。
    QueryPerformanceCounter(&t3c);
    // FG 插值帧回读(各组独立缓冲;eval 成功的槽才有内容)。失败按整体
    // 失败处理:调用方会对全部输出做源帧复制降级。
    if (rb && fgDstPlanes) {
        for (int g = 0; g < ff->fgM - 1; ++g) {
            if (fgGenOk && fgGenOk[g] &&
                !_d3d12->UnpackOutput(*ff->slot, &fgDstPlanes[g * 3], &fgDstStrides[g * 3],
                                      _outW, _outH, err, errLen, g)) {
                return false;
            }
        }
    }
    {
        QueryPerformanceCounter(&t4);
        // 诊断 IO(纹理回读 + 磁盘写)不计入 t3c→t4 段(2026-10-05 修:
        // 此前 dump 帧的面板 fg 耗时虚高;dump 是"帧已收敛后的旁路")。
        DumpFrameDiagnostics(*ff);
        const auto ms = [](LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER f) {
            return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
        };
        const double packMs = ms(ff->t0, ff->t1, ff->qpcFreq);
        // eval_cpu = "NGX 调用" 的 CPU 成本:NR eval 窗口(t1→ff->t2,扣 nvof)
        // **+ RTX evals 的 CPU 提交链(ff->tSub0→ff->tSub1,vsr/TrueHDR/DLSSG 逐笔
        // 参数录制,~1.8ms/次,全家桶 8 笔 ≈ 14ms,2026-09-24 探针定案)**。
        // 提交链与 GPU 执行并行,但它是真实的每帧 CPU 串行成本 —— 唯一
        // 诚实呈现的位置就是本段(NR 关时不再恒 0,RTX 关时仍 0)。
        // 直通帧(ff->evalZeroed)没有 NGX 调用,恒 0。
        const double evalCpuMs = ms(ff->t1, ff->t2, ff->qpcFreq);
        const double evalBase = ff->evalZeroed ? 0.0
                                : evalCpuMs > ff->nvofMs ? evalCpuMs - ff->nvofMs
                                                     : 0.0;
        const double evalOnlyMs = evalBase + subWaitMs;
        // 分段 GPU 时间 = 时间戳括号差分(布局见 d3d12_context.h;栅栏差分
        // 仅 tsAcc=false 的回退账目):
        // gpu 段 = base CL(NR 推理/残差/直通),
        // vsr 段 = VSR eval(RTX 专用队列括号,2026-10-02 起纯执行账),
        // fg 段 = postA(HDR 会话 = 纯 DLSSG 推理;非 HDR = post 全部:
        // 推理 + 转换/回读 旧形态),
        // hdr 段 = TrueHDR 真实帧 + 插值链两括号之和(仅 HDR 会话非零),
        // 门关帧 fg 段 ≈ 0(WaitFrame 等的就是 base 值,立即满足)。
        const double gpuWaitMs = ms(ff->t2, ff->t3a, ff->qpcFreq);
        // NR 关直连帧:base CL 为空(零 NR 工作)。时间戳括号路径 base 纯执行
        // 自然 ≈0,无需特判;栅栏差分回退路径保留原约定(等空栅栏的墙钟含
        // 队列拥塞,记 0 防止"NR 推理"段被拥塞污染)。
        const double gpuSegMs = tsAcc ? baseGpuMs
                                      : ((ff->nrOff && !ff->skipEval) ? 0.0 : gpuWaitMs);
        // unpack 段 = 真实帧回读;插值帧回读(t3c→t4)= FG 输出搬运,
        // 并入 fg 段(无 FG 帧两者差 ≈0)。
        const double unpackMs = ms(t3b, t3c, ff->qpcFreq);
        const double fgReadbackMs = ms(t3c, t4, ff->qpcFreq);
        // 九段拆账(2026-10-02 起 vsr/hdr 括号化):tsAcc 时全 GPU 段统一
        // "纯执行"语义;回退路径(括号失败帧)整体退回栅栏墙钟账 —— 段间
        // 含队列空闲/等待,不精确,总量恒真(旧账目,历史细节留分支内)。
        double rtxVsrMs = 0.0;
        double fgMs = 0.0, rtxHdrMs = 0.0, convMs = 0.0;
        if (tsAcc) {
            // 时间戳括号:fg = DLSSG 纯执行(+ 回读拷贝见下);conv = post CL
            // 纯执行(CL 内嵌首尾打点,跨队列栅栏等待与队列积压不计入)。
            // NR 关直连帧的 base 窗口(队列积压)归 queue 段,不折进 conv。
            // vsr/hdr = RTX 专用队列括号(索引 6..11),hdr = 真实帧 + 插值链
            // 两括号之和。
            fgMs = ff->fgBeginOk ? fgGpuMs : 0.0;
            convMs = convGpuMs;
            rtxVsrMs = rtxVsrGpuMs;
            rtxHdrMs = rtxHdrRealMs + rtxHdrGenMs;
        } else {
            // 栅栏差分回退(旧账目;段间含队列空闲/等待,不精确)
            const LARGE_INTEGER &fgStart = ff->vsrDoneFence ? tVsrDone : ff->tSub1;
            // vsr/hdr 墙钟账(GPU-bound 时真实,CPU-bound 时被提交链覆盖
            // ≈0;ce6970b 后 tsValid 恒真,此分支只在括号失败帧走 —— 2026-
            // 09-26 曾因 hdr 只在回退分支算而整段消失,勿再挪回主路径)。
            rtxVsrMs = ff->vsrDoneFence ? ms(ff->tSub1, tVsrDone, ff->qpcFreq) : 0.0;
            if (ff->hdrPostSplit) {
                rtxHdrMs = ms(ff->fgBeginOk ? tFgDone : fgStart, tHdrDone, ff->qpcFreq);
            } else if (ff->hdrRun) {
                rtxHdrMs = ms(ff->tSub1, tHdrDone, ff->qpcFreq);
            }
            if (ff->fgBeginOk) fgMs = ms(fgStart, tFgDone, ff->qpcFreq);
            if (ff->hdrPostSplit) {
                convMs = ms(tHdrDone, t3b, ff->qpcFreq);
            } else if (ff->hdrRun) {
                const LARGE_INTEGER &convAnchor =
                    (ff->fgBeginOk ? tFgDone.QuadPart : fgStart.QuadPart) > tHdrDone.QuadPart
                        ? (ff->fgBeginOk ? tFgDone : fgStart) : tHdrDone;
                convMs = ms(convAnchor, t3b, ff->qpcFreq);
            } else {
                const LARGE_INTEGER &convAnchor =
                    ff->fgBeginOk ? tFgDone : (ff->vsrDoneFence ? tVsrDone : ff->t3a);
                convMs = ms(convAnchor, t3b, ff->qpcFreq);
            }
            // NR 关直连帧:base CL 只剩 C1 补做(转换),gpu 段记 0、窗口归
            // conv。NR 开帧的 C1(在 eval_cpu 窗口或 gpu 段内,微秒级)不入
            // conv。(仅回退路径;时间戳路径排队单独成段。)
            if (ff->nrOff && !ff->skipEval) convMs += gpuWaitMs;
        }
        // 插值帧回读(FG 输出搬运,CPU 行拷贝)归入 fg 段:DLSSG 推理被
        // 提交链遮盖时,这里就是补帧成本的主要可见账目(4x @OUT 几何可达
        // 10ms+)。
        fgMs += fgReadbackMs;
        // Magpie-style perf log line into dlssnr_timing.log (time-gated ≥1s,
        // see perfDue below). TimingLog takes g_timingMutex itself — format
        // the line under the lock, log outside of it, or this thread
        // self-deadlocks on frame 1 and burns one ff->slot forever.
        char line[384] = "";
        // 九段 last(每帧值,面板"处理用时"数据源;五段管线 + vsr/hdr/conv
        // 分段)+ EMA(perf 日志行专用)。pack/nvof/eval_cpu/unpack/vsr/hdr/conv
        // 的 last 就是本帧裸值(与日志对齐);gpuLast 取窗口内最近样本。EMA 同
        // 锁内算,fmParallel 并发安全。
        double gpuLast = 0.0, gpuEma = 0.0, packEma = 0.0, nvofEma = 0.0,
               evalCpuEma = 0.0, unpackEma = 0.0;
        const double packLast = packMs, nvofLast = ff->nvofSpanMs,
                     evalCpuLast = evalOnlyMs, unpackLast = unpackMs,
                     fgLast = fgMs, rtxVsrLast = rtxVsrMs, rtxHdrLast = rtxHdrMs,
                     convLast = convMs;
        // perf 行数据两段式:g_timing 相关在锁内备齐,外部量(Snapshot/
        // FrameRateWindow,各自持独立互斥)在锁外取 —— 不在 g_timingMutex
        // 内叠锁(块尾注释纪律;曾塞进 snprintf 参数与该纪律相反,acc2e69
        // 引入)。perfLineDue = 锁内时间门判定,供锁外拼行。
        bool perfLineDue = false;
        double perfGpuP99 = 0.0, perfSlotEma = 0.0, perfLockEma = 0.0;
        double perfNvofLast = 0.0, perfSlotLast = 0.0, perfLockLast = 0.0;
        {
            std::lock_guard<std::mutex> timingLock(g_timingMutex);
            const double slotWaitMs = ms(ff->tSlot0, ff->tSlot1, ff->qpcFreq);
            const double lockWaitMs = ms(ff->tLock0, ff->tLock1, ff->qpcFreq);
            g_timing.Push(gpuSegMs, packMs, ff->nvofSpanMs, evalOnlyMs, unpackMs, slotWaitMs, lockWaitMs);
            const int lastIdx = g_timing.idx - 1 < 0 ? g_timing.count - 1 : g_timing.idx - 1;
            gpuLast = g_timing.gpu[lastIdx];
            // perf 行按时间门(≥1s 一行)而非帧数:诊断日志的样本密度不应
            // 随源帧率漂(60fps=1s 一行而 30fps=2s 一行)。静态量在
            // g_timingMutex 内读写,fmParallel 并发安全;首帧立即落一行。
            static LARGE_INTEGER lastPerfQpc{};
            LARGE_INTEGER nowQpc{};
            QueryPerformanceCounter(&nowQpc);
            const bool perfDue = lastPerfQpc.QuadPart == 0 ||
                                 (nowQpc.QuadPart - lastPerfQpc.QuadPart) >= ff->qpcFreq.QuadPart;
            if (perfDue) lastPerfQpc = nowQpc;
            if (perfDue) {
                // EMA×5 只被 perf 行消费(≥1s 一次),挪进门内,不再每帧
                // 在 g_timingMutex 内白算 600 次浮点(2026-09-25)。
                gpuEma = TimingWindow::Ema(g_timing.gpu, g_timing.count);
                packEma = TimingWindow::Ema(g_timing.pack, g_timing.count);
                nvofEma = TimingWindow::Ema(g_timing.nvof, g_timing.count);
                evalCpuEma = TimingWindow::Ema(g_timing.evalCpu, g_timing.count);
                unpackEma = TimingWindow::Ema(g_timing.unpack, g_timing.count);
                perfGpuP99 = TimingWindow::P99(g_timing.gpu, g_timing.count);
                perfSlotEma = TimingWindow::Ema(g_timing.slotW, g_timing.count);
                perfLockEma = TimingWindow::Ema(g_timing.lockW, g_timing.count);
                perfNvofLast = g_timing.nvof[lastIdx];
                perfSlotLast = g_timing.slotW[lastIdx];
                perfLockLast = g_timing.lockW[lastIdx];
                perfLineDue = true;
            }
        }
        if (perfLineDue) {
            // 门细分探针为 NvofContext 专属(其它后端无引擎等待);读法与
            // 下方 stats 块同款(仅读,锁外)。
            NvofContext *nvProbe = _ofBackend ? _ofBackend->AsNvof() : nullptr;
            // Snapshot/FrameRateWindow 锁外取(各自持独立互斥,勿叠锁)。
            // res 本就是会话配置回显;NR 关(live 门)时缩放整级绕过(nrOff
            // 直连 inputColor),行内就地标注,防误读"正在按内部尺寸处理"。
            const int perfResPct = std::clamp(_shared.load(std::memory_order_acquire)->Snapshot().inputResolutionPercent,
                                              kResPctMin, kResPctMax);
            const bool perfNrOff = !_shared.load(std::memory_order_acquire)->Snapshot().nrEnabled;
            const double perfFps = _fpsMeter.Rate();
            snprintf(line, sizeof(line),
                     "DLSSNR perf: gpu=%.1f ema=%.1f p99=%.1f | pack=%.1f of=%.1f/%.1f g%.1f c%.1f e%.1f x%u r%u | eval_cpu=%.1f fg=%.1f rtx=%.1f/%.1f conv=%.1f unpack=%.1f sub=%.1f | slot=%.1f/%.1f lock=%.1f/%.1f q=%.1f | res=%d%%%s ofq=%d %dx%d f=%d fps=%.0f",
                     gpuLast, gpuEma, perfGpuP99, packEma, nvofEma, perfNvofLast,
                     nvProbe ? nvProbe->LastGateWaitMs() : 0.0,
                     nvProbe ? nvProbe->LastCpyWaitMs() : 0.0,
                     ff->ofEngineMs,
                     nvProbe ? nvProbe->GateExpired() : 0u,
                     nvProbe ? nvProbe->ResetCount() : 0u,
                     evalCpuEma, fgLast, rtxVsrLast, rtxHdrLast, convLast, unpackEma, subWaitMs,
                     perfSlotEma, perfSlotLast,
                     perfLockEma, perfLockLast,
                     queueWaitMs,
                     perfResPct,
                     perfNrOff ? "(nrOff)" : "",
                     _curOfQuality.load(std::memory_order_relaxed),
                     _width, _height,
                     _lastFrameN.load(std::memory_order_relaxed),
                     perfFps);
            // nvof=ema/last;后缀 g/c/e = 门等待/前帧拷贝等待/引擎输出等待,
            // x/r = 过期帧/历史重置累计(探针保留:时序类问题的第一手证据;
            // 2026-10-04 撤恒 0 的 s=GateSkips,死指标,原 cv 超时门已删)。
            // ff->slot/lock = 槽池等待 / evaluate 互斥等待
            // (ema/last);f = 本行前一帧的帧号(与 STATUS 行对齐用);
            // fps = 4s 窗口帧入口计数均值,处理帧率 < 源帧率 = 宿主侧没来帧。
        }
        // stats 每帧发布(九段 last + 共享内存写,开销可忽略):面板
        // "处理用时"随帧呼吸,不再按日志节流跳变。perf 行(磁盘 IO)按时间
        // 门 ≥1s 一行(与帧率无关)。Snapshot/FrameRateWindow 在锁外取
        // (各自持独立互斥,勿在 g_timingMutex 内叠锁)。
        {
            // 排队细分 last(ff->slot/lock 等待)与门累计:诊断页数据源。nvof
            // 探针仅 NVOF 后端存在(FFX 无引擎等待,恒 0);指针读法与上方
            // perf 行同款(仅读,换会话在 PoolHold 内,帧线程读不撕裂)。
            const double slotWaitLast = ms(ff->tSlot0, ff->tSlot1, ff->qpcFreq);
            const double lockWaitLast = ms(ff->tLock0, ff->tLock1, ff->qpcFreq);
            NvofContext *nvStats = _ofBackend ? _ofBackend->AsNvof() : nullptr;
            StatsPayload st{}; // 未携带字段保持零/空;gpuLast 保持 -1 哨兵
            // FG 状态:on = 本帧有真插值(附当前倍数);dup = 复制真实帧
            // (复位/零光流/面板关/降级);off = 本会话未激活;unavailable =
            // official 初始化或 eval 失败闩停(帧率仍 ×M,内容为复制帧)。
            // fgMult = 当前倍数(面板显示 "3x" 用;未激活 = 0)。
            // fgRouteEff = 实际生效路由(off/official-hook/official/copy),
            // fgMultCreate = 会话创建倍数 —— "auto 档这次到底走没走 hook
            // 代理""4x 为什么只跑 2x"面板直读,不翻 timing log。
            const char *fgState = !_fg ? "off"
                : (!_fg->Enabled() ? "unavailable"
                                   : (ff->fgEvaluatedCount > 0 ? "on" : "dup"));
            st.gpuLast = static_cast<float>(gpuLast);
            st.packLast = static_cast<float>(packLast);
            st.evalCpuLast = static_cast<float>(evalCpuLast);
            st.unpackLast = static_cast<float>(unpackLast);
            st.ofLast = static_cast<float>(nvofLast);
            st.fgLast = static_cast<float>(fgLast);
            st.rtxVsrLast = static_cast<float>(rtxVsrLast);
            st.rtxHdrLast = static_cast<float>(rtxHdrLast);
            st.convLast = static_cast<float>(convLast);
            st.queueLast = static_cast<float>(queueWaitMs);
            st.internalW = static_cast<uint32_t>(_d3d12->InternalWidth());
            st.internalH = static_cast<uint32_t>(_d3d12->InternalHeight());
            st.width = static_cast<uint32_t>(_width);
            st.height = static_cast<uint32_t>(_height);
            st.scaling = _d3d12->HasScaling() ? 1u : 0u;
            st.fps = static_cast<float>(_fpsMeter.Rate());
            // 每帧实效位(DSL9;面板直读,免镜像门控公式):eval = 本帧有
            // NGX 调用;of = OF 消费门(SyncOfSession 的 ofNeeded);scaling
            // = 内部缩放档真参与(NR 关直连帧整级绕过 = 0)。
            st.evalActive = ff->evalZeroed ? 0u : 1u;
            st.ofActive = ff->ofNeeded ? 1u : 0u;
            st.scalingActive = (_d3d12->HasScaling() && !ff->nrOff) ? 1u : 0u;
            // rtx 实效位/数值尺寸(DSLA):面板 HDR 打标与分辨率链直读,rtx
            // 显示串回归纯显示(免 strstr/sscanf 人读串 parse)。位 = 本帧
            // RTX 段真跑(完成栅栏已挂);尺寸 = 输出几何成员(会话稳定)。
            st.rtxVsrActive = ff->vsrDoneFence ? 1u : 0u;
            st.rtxHdrActive = ff->hdrDoneFence ? 1u : 0u;
            st.rtxOutW = static_cast<uint32_t>(_outW);
            st.rtxOutH = static_cast<uint32_t>(_outH);
            FillStatsCommon(st);
            CopyStatStr(st.fgState, fgState);
            st.fgMult = static_cast<uint32_t>(ff->fgM);
            st.slotWait = static_cast<float>(slotWaitLast);
            st.lockWait = static_cast<float>(lockWaitLast);
            st.gateExpired = nvStats ? nvStats->GateExpired() : 0u;
            st.gateResets = nvStats ? nvStats->ResetCount() : 0u;
            CopyStatStr(st.temporal,
                        _tPubState.load(std::memory_order_relaxed) == 3 ? "failed"
                        : _tPubState.load(std::memory_order_relaxed) == 2 ? "steady"
                        : _tPubState.load(std::memory_order_relaxed) == 1 ? "seed" : "off");
            st.temporalRoute = static_cast<uint32_t>(_tPubRoute.load(std::memory_order_relaxed));
            st.temporalW = _tPubWeight.load(std::memory_order_relaxed);
            PublishStats(st);
        }
        if (line[0]) TimingLog(line); // outside g_timingMutex (TimingLog locks it)

        if (vsTiming) {
            std::snprintf(timingOut, timingLen, "pack=%.1f,of=%.1f,eval_cpu=%.1f,gpu=%.1f,fg=%.1f,conv=%.1f,unpack=%.1f",
                          packMs, ff->nvofSpanMs, evalOnlyMs, gpuSegMs, fgMs, convMs, unpackMs);
        }
    }
    return rb;
}

// StatsPayload 公共体(init 发布与逐帧发布共用;原两份手抄字段清单)。
// 帧态字段(用时/fgState/fgMult/slotWait 等)由调用方在公共体之外填充。
void DlssnrContext::FillStatsCommon(StatsPayload &st) noexcept {
    CopyStatStr(st.gpuName, _gpuNameUtf8);
    CopyStatStr(st.modelDll, _modelDllUtf8);
    CopyStatStr(st.rtx, _rtxStateStr);
    CopyStatStr(st.rtxDetail, _rtxDetail);
    CopyStatStr(st.filterState, (_nvofFailed && _curOfQuality > 0) ? "nvof_zero" : "ok");
    {
        // 会话指针收口(2026-10-05 评审修):OfModeString 三连裸读
        // _ofBackend 与并发 swap 竞争 —— 整段挪 _ofSwapMutex 内(偏序末端;
        // 调用方不持会与末端成环的锁)。退役对象进程期存活,悬垂不可能。
        std::lock_guard<std::mutex> swapLock(_ofSwapMutex);
        CopyStatStr(st.ofMode, OfModeString());
    }
    CopyStatStr(st.fgRouteEff, _fgRouteEff);
    st.fgMultCreate = static_cast<uint32_t>(_fgCreateMult.load(std::memory_order_relaxed));
    CopyStatStr(st.fgDetail, _fgDetail);
    // fgMultMax = 运行库插值帧上限(gate 解锁结果定格值):40 系解锁失败
    // 回落 2x 时创建/面板仍报 6,没有它面板无从知道实际密度只有 (max+1)x。
    st.fgMultMax = static_cast<uint32_t>(_fg ? _fg->MaxGen() : 0);
    CopyStatStr(st.ofDetail, _ofDetail);
}

// 帧诊断 dump(VSDLSSNR_DUMP;原内联在 ProcessFrameFinish 中段,2026-10-03
// 整体迁出 —— 诊断是"帧已收敛后的旁路",不该长在热路径函数里。)
// 调用点 = Finish unpack 之后、计时段之前,语义不变。
void DlssnrContext::DumpFrameDiagnostics(FrameFinish &ff) noexcept {
    // concurrent frame threads: dump at most once, no interleaved writes;
    // the mutex is only taken when a dump is actually pending (the old
    // code locked it every frame even with dumping disabled).
    // 颜色 dump 与 NVOF motion/flow dump 分开锁存:首帧必然是播种帧
    // (播种清零,无真流),motion dump 要等到第一个 densify 帧。
    static std::atomic<bool> dumped{ false };
    static std::atomic<bool> dumpedMotion{ false };
    if (ProbeEnabled()) {
        char probe[96];
        std::snprintf(probe, sizeof(probe), "PROBE: dump-site realMotion=%d ofQ=%d",
                      ff.realMotion ? 1 : 0, _curOfQuality.load(std::memory_order_relaxed));
        TimingStatusLine(probe);
    }
    // VSDLSSNR_DUMP_SKIP=N:跳过前 N 个真运动帧再 dump(warmup 帧
    // 的流场未成熟,FFX/NVOF A/B 对照用)。
    static std::atomic<int> realCount{ 0 };
    const int myIdx = ff.realMotion ? realCount.fetch_add(1, std::memory_order_relaxed) : -1;
    static const int dumpSkip = [] {
        char b[16]{};
        GetEnvironmentVariableA("VSDLSSNR_DUMP_SKIP", b, sizeof(b) - 1);
        return b[0] ? std::atoi(b) : 0;
    }();
    if (ff.dumpEnabled && ff.realMotion && ff.rbOk && myIdx >= dumpSkip &&
        !dumpedMotion.load(std::memory_order_relaxed)) {
        static std::mutex dumpMotionMutex;
        std::lock_guard<std::mutex> dumpLock(dumpMotionMutex);
        if (!dumpedMotion.exchange(true)) {
            TimingStatusLine("DLSSNR STATUS: motion latch fired");
            wchar_t dir[MAX_PATH];
            if (GetModuleFileNameW(nullptr, dir, MAX_PATH)) {
                std::filesystem::path base = std::filesystem::path(dir).parent_path();
                const bool scaling = _d3d12->HasScaling();
                std::lock_guard<std::mutex> ctlLock(_d3d12->CtlMutex());
                // NGX 实际消费的运动场(缩放时为降采样版,R16G16_FLOAT)+
                // 原始网格流(S10.5,R16G16_SINT)。
                bool motionDumped = _d3d12->DumpTextureToFile(
                    scaling ? ff.slot->reducedMotion.Get() : ff.slot->motion.Get(),
                    (base / L"dump_motion.bin").c_str());
                if (!motionDumped) TimingStatusLine("DLSSNR STATUS: motion dump FAILED");
                else TimingStatusLine("DLSSNR STATUS: motion dump OK");
                if (NvofContext *nvDump = _ofBackend ? _ofBackend->AsNvof() : nullptr;
                    nvDump && nvDump->FlowForward()) {
                    const bool flowOk = _d3d12->DumpTextureToFile(
                        nvDump->FlowForward(),
                        (base / L"dump_flow.bin").c_str());
                    if (!flowOk) TimingStatusLine("DLSSNR STATUS: flow dump FAILED");
                } else if (_ofBackend && _ofBackend->Kind() == kOfBackendFfx) {
                    // FFX 中间场:_ffxInput(R8G8B8A8 逻辑 RGBA 直写) +
                    // 稀疏流(R16G16_SINT,1/8 OF extent)。
                    auto *fx = static_cast<FxofContext *>(_ofBackend.get());
                    if (!_d3d12->DumpTextureToFile(fx->FfxInput(),
                                                   (base / L"dump_fxof_input.bin").c_str())) {
                        TimingStatusLine("DLSSNR STATUS: fxof input dump FAILED");
                    }
                    if (!_d3d12->DumpTextureToFile(fx->SparseFlow(),
                                                   (base / L"dump_fxof_sparse.bin").c_str())) {
                        TimingStatusLine("DLSSNR STATUS: fxof sparse dump FAILED");
                    }
                }
            }
        }
    }
    if (ff.dumpEnabled && ff.rbOk && !dumped.load(std::memory_order_relaxed)) {
        static std::mutex dumpMutex;
        std::lock_guard<std::mutex> dumpLock(dumpMutex);
        if (!dumped.exchange(true)) {
            wchar_t dir[MAX_PATH];
            if (GetModuleFileNameW(nullptr, dir, MAX_PATH)) {
                std::filesystem::path base = std::filesystem::path(dir).parent_path();
                const bool scaling = _d3d12->HasScaling();
                std::lock_guard<std::mutex> ctlLock(_d3d12->CtlMutex());
                // footprint 格式/尺寸由 DumpTextureToFile 自取资源 desc
                //(调用方传参版三次踩坑,见 d3d12_context.h 头注释);
                // 探针:dump 失败逐个留痕,"dump 文件缺失/尺寸不对"从
                // 这行直接定位。
                auto dumpOrLog = [&](ID3D12Resource *tex, const wchar_t *name) {
                    char why[128]{};
                    if (!_d3d12->DumpTextureToFile(tex, (base / name).c_str(),
                                                   why, sizeof(why))) {
                        char msg[224];
                        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: dump %ls FAILED: %.120s",
                                      name, why);
                        TimingStatusLine(msg);
                    }
                };
                dumpOrLog(_d3d12->InputColor(*ff.slot), L"dump_input.bin");
                // YUV 输入平面(YUV→RGB 转换验收:python 参考按矩阵/范围
                // 重算 BGRA8 与 dump_input 对比 ≤1-2 LSB)。色度尺寸按
                // 真实布局(444 全分辨率等),勿自推半分辨率。
                {
                    const wchar_t *names[3]{ L"dump_yuvin_y.bin", L"dump_yuvin_u.bin", L"dump_yuvin_v.bin" };
                    for (int i = 0; i < 3; ++i) {
                        if (!_d3d12->DumpYuvInPlane(*ff.slot, i, (base / names[i]).c_str())) {
                            char msg[160];
                            std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: dump %ls FAILED",
                                          names[i]);
                            TimingStatusLine(msg);
                        }
                    }
                }
                // GPU 光流输入降采样结果(注册输入纹理,会话尺寸):
                // 数值验证用 —— python 参考脚本从 dump_input.bin 重算
                // 双线性,断言 ≤1 LSB(#46 改 GPU 的验收)。
                if (_ofBackend && ff.nvofInputIndex >= 0) {
                    if (ID3D12Resource *ofIn =
                            _ofBackend->InputTexture(ff.nvofInputIndex)) {
                        dumpOrLog(ofIn, L"dump_nvof_input.bin");
                    }
                }
                if (scaling) {
                    dumpOrLog(_d3d12->ReducedColor(*ff.slot), L"dump_reduced_color.bin");
                    dumpOrLog(_d3d12->ReducedDenoised(*ff.slot), L"dump_reduced_denoised.bin");
                    dumpOrLog(_d3d12->HorizontalRes(*ff.slot), L"dump_horizontal.bin");
                }
                dumpOrLog(_d3d12->OutputColor(*ff.slot), L"dump_output.bin");
                // TrueHDR 输出本体(FP16 scRGB,PIPE 尺寸):黑屏排查的
                // "没写 vs 写了零 vs 写了错值"判据(VSDLSSNR_DUMP=1)。
                // 三态全留痕(2026-09-25):此前 OK/跳过都静默,
                // "为什么没有 hdrColor dump"无法定位。
                {
                    if (ff.slot->hdrColor) {
                        char why[128]{};
                        const bool okHC = _d3d12->DumpTextureToFile(
                            ff.slot->hdrColor.Get(),
                            (base / L"dump_hdrcolor.bin").c_str(), why, sizeof(why));
                        char msgHC[192];
                        std::snprintf(msgHC, sizeof(msgHC), "DLSSNR STATUS: dump_hdrcolor %s%.150s",
                                      okHC ? "OK" : "FAILED: ", okHC ? "" : why);
                        TimingStatusLine(msgHC);
                    } else {
                        TimingStatusLine("DLSSNR STATUS: dump_hdrcolor SKIPPED (slot->hdrColor null)");
                    }
                }
                // FG 插值输出(仅 eval 过的帧有内容;首帧必为播种,等
                // 第一个真插值帧才有意义 —— 与 motion dump 同款锁存)。
                // 恒 SDR 域 BGRA8 @PIPE(VSR 时 = _pipeW/H,else 源尺寸)。
                if (ff.fgRan) {
                    dumpOrLog(_d3d12->FgInterp(*ff.slot, 0), L"dump_fg_interp.bin");
                }
                // YUV 输出平面(RGB→YUV 转换验收:python 参考 script 重算
                // Y/U/V 与 dump 对比,≤1-2 LSB;P10 = 右对齐 word)。
                // 尺寸/格式随资源 desc 自取 —— RTX VSR 会话 yuvOut 在 OUT
                // 尺寸、HDR 会话 P10,历史上手传参数两次踩坑(2026-09-25
                // "dump 三连失败真因"),desc 自取后整类失配消失。
                {
                    const wchar_t *names[3]{ L"dump_y_plane.bin", L"dump_u_plane.bin", L"dump_v_plane.bin" };
                    for (int i = 0; i < 3; ++i) {
                        dumpOrLog(_d3d12->YuvOutPlane(*ff.slot, i), names[i]);
                    }
                }
            }
        }
    }
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
    StatsPayload st{};
    CopyStatStr(st.filterState, state);
    CopyStatStr(st.stateDetail, safe); // 空串 = 无原因(面板同缺键清空)
    PublishStats(st);
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
