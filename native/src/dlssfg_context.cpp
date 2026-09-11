// DLSS 帧生成上下文实现(契约与时序模型见 dlssfg_context.h)。
// proxy 消费面 = dlssg_for_sm86 二进制字符串表 + 官方 nvsdk_ngx_defs_dlssg.h
// 的交集;eval 布局对照 Magpie DLSSFrameGenerator.cpp 的 optionalParams。

#include "dlssfg_context.h"
#include "dlssnr_context.h" // TimingStatusLine(失败必须进 timing log)

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace vsdlssnr {

namespace {

// proxy 未要求特定 appId;与 NR snippet 的 0x0876232C 区分开,避免两条
// NGX 驱动链的身份混淆(任意稳定非零值即可)。
constexpr unsigned long long DLSSFG_PROXY_APPLICATION_ID = 0x0876232Dull;

constexpr NVSDK_NGX_Feature FEATURE_DLSSG = NVSDK_NGX_Feature_FrameGeneration; // 11

// proxy 自有键(来自 0.2.4 二进制字符串表,官方 nvsdk_ngx_defs_dlssg.h 无宏;
// MultiFrameCountMax 除外,官方头有)。能力三键是 proxy 在 Init 后写入参数块
// 的查询结果,读不到 = 旧版 proxy,fail-open 保持原行为。
constexpr char KFG_KEY_AVAILABLE[] = "?DLSSG.Available";
constexpr char KFG_KEY_NEEDS_UPDATED_DRIVER[] = "DLSSG.NeedsUpdatedDriver";
constexpr char KFG_KEY_FEATURE_INIT_RESULT[] = "DLSSG.FeatureInitResult";
// 调用方 D3D12 队列:proxy 的 CUDA 互操作与可选 GPU 计时([Diagnostics]
// Performance=1)以它为锚(二进制明文 "Performance tracing requires
// DLSSG.CmdQueue")。传槽 CL 的执行队列 → 同队列 FIFO 保序(见
// dlssfg_context.h 时序契约);不传时 proxy 自管,诊断计时不可用。
constexpr char KFG_KEY_CMD_QUEUE[] = "DLSSG.CmdQueue";

// 官方 create 资源旗标(Magpie DLSSFrameGenerator.cpp:513-518 原样):
// 声明永不提供的可选输入,省 VRAM。proxy 未读该键时无害。
constexpr unsigned int DLSSG_NEVER_PROVIDED_FLAGS =
    NVSDK_NGX_DLSSG_ResourceFlags_HUDLess |
    NVSDK_NGX_DLSSG_ResourceFlags_UI |
    NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha |
    NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField |
    NVSDK_NGX_DLSSG_ResourceFlags_OutputReal;

// ---- 进程级模块缓存:同路径只 LoadLibrary 一次,永不卸载 ----
// quality/dims 变化只重建 feature 不卸模块(nvofapi64.dll 同哲学:驱动内
// 部线程存活期不明,FreeLibrary 死锁风险;进程退出由 OS 回收)。
struct FgModuleCache {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH]{};
};
FgModuleCache &FgModule() noexcept {
    static FgModuleCache cache;
    return cache;
}

// ---- FG 本地 SEH(与 NgxRuntimeGuard::_InvokeSafely 同构,但闩锁本类)----
// FG proxy 的 SEH 绝不上抛全局闩锁:NR 的 NGX core/snippet 是独立模块,
// FG 崩溃不应连带杀 NR(降级边界:本类 _faulted = FG 永久停用)。
LONG DlssfgCaptureException(EXCEPTION_POINTERS *exception, DWORD *sehCode) noexcept {
    *sehCode = exception->ExceptionRecord->ExceptionCode;
    return EXCEPTION_EXECUTE_HANDLER;
}

// 返回值:true = fn() 返回 Success 且无 SEH;false = 失败(sehCode 非零
// = SEH,零 = 普通 NGX 失败)。
template <typename Fn>
bool DlssfgSehCall(Fn &&fn, DWORD *sehCode) noexcept {
    *sehCode = 0;
    __try {
        return fn() == NVSDK_NGX_Result_Success;
    } __except (DlssfgCaptureException(GetExceptionInformation(), sehCode)) {
        return false;
    }
}

} // namespace

DlssfgContext::~DlssfgContext() {
    // 故障后绝不重入 proxy(闩锁语义;模块/feature 泄漏给 OS 回收,与热
    // 上下文同哲学)。健康路径也只弃引用:ReleaseFeature 在 Rebuild 尺寸
    // 重建时显式走,析构不补 —— 进程退出即回收,避免 DllMain/loader 锁
    // 下调 proxy 代码(与 NR Shutdown 的理由一致)。
}

template <typename Fn>
bool DlssfgContext::SehCall(Fn &&fn, const char *what, char *err, size_t errLen) noexcept {
    if (_faulted.load(std::memory_order_acquire)) {
        if (err && errLen) {
            std::snprintf(err, errLen, "dlssfg: faulted latch active (%s not called)", what);
        }
        return false;
    }
    DWORD sehCode = 0;
    const bool ok = DlssfgSehCall(fn, &sehCode);
    if (!ok && sehCode) {
        _faulted.store(true, std::memory_order_release);
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: dlssfg %s raised SEH 0x%lX; FG disabled until host restart",
                      what, static_cast<unsigned long>(sehCode));
        TimingStatusLine(msg);
        if (err && errLen) {
            std::snprintf(err, errLen, "dlssfg: %s raised SEH 0x%lX",
                          what, static_cast<unsigned long>(sehCode));
        }
        return false;
    }
    return ok;
}

bool DlssfgContext::Initialize(D3D12Context &d3d12, const wchar_t *dllPath,
                               const wchar_t *appDataPath, NVSDK_NGX_Parameter *params,
                               int width, int height, DXGI_FORMAT backbufferFormat,
                               char *err, size_t errLen) noexcept {
    auto fail = [&](const char *what) {
        if (err && errLen) std::snprintf(err, errLen, "%s", what);
        // 初始化失败 = FG 降级(NR 不受影响);原因必须进 timing log,
        // 否则"为什么没有帧生成"无迹可查。
        char msg[224];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: dlssfg init failed: %.180s",
                      what ? what : "?");
        TimingStatusLine(msg);
        _d3d12 = nullptr;
        _params = nullptr;
        return false;
    };
    if (!dllPath || !dllPath[0]) return fail("no proxy dll path");
    if (!params) return fail("no core parameter block");

    _d3d12 = &d3d12;
    _params = params;

    // 模块缓存:同路径复用;路径变化(升级/换文件)才重新 LoadLibrary。
    // 旧模块同样永不卸载(多模块共存无害,显存随 feature 释放)。
    FgModuleCache &cache = FgModule();
    if (!cache.module || wcscmp(cache.path, dllPath) != 0) {
        HMODULE mod = LoadLibraryExW(dllPath, nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                         LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!mod) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "LoadLibraryExW(proxy) failed (err=%lu)",
                          static_cast<unsigned long>(GetLastError()));
            return fail(msg);
        }
        cache.module = mod;
        wcsncpy_s(cache.path, dllPath, _TRUNCATE);
        // 部署指纹(与 NR snippet 的 dll 指纹行同款:代理文件被换时一行定位)。
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(dllPath, GetFileExInfoStandard, &fa)) {
            ULARGE_INTEGER sz{};
            sz.HighPart = fa.nFileSizeHigh;
            sz.LowPart = fa.nFileSizeLow;
            char msg[128];
            std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: dlssfg proxy %llu bytes",
                          static_cast<unsigned long long>(sz.QuadPart));
            TimingStatusLine(msg);
        }
    }
    HMODULE mod = cache.module;

    _initExt = reinterpret_cast<InitExtFn>(GetProcAddress(mod, "NVSDK_NGX_D3D12_Init_Ext"));
    _createFeature = reinterpret_cast<CreateFeatureFn>(GetProcAddress(mod, "NVSDK_NGX_D3D12_CreateFeature"));
    _evaluateFeature = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(mod, "NVSDK_NGX_D3D12_EvaluateFeature"));
    _releaseFeature = reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(mod, "NVSDK_NGX_D3D12_ReleaseFeature"));
    if (!_initExt || !_createFeature || !_evaluateFeature || !_releaseFeature) {
        return fail("proxy D3D12 exports incomplete (not the dlssg proxy dll?)");
    }

    {
        char sehErr[160]{};
        const bool ok = SehCall([&] {
            return _initExt(DLSSFG_PROXY_APPLICATION_ID, appDataPath,
                            d3d12.Device(), NVSDK_NGX_Version_API, nullptr) ==
                   NVSDK_NGX_Result_Success;
        }, "Init_Ext", sehErr, sizeof(sehErr));
        if (!ok) {
            char msg[224];
            std::snprintf(msg, sizeof(msg),
                          "Init_Ext failed%s%s (proxy refused; existing NGX runtime conflict?)",
                          sehErr[0] ? ": " : "", sehErr[0] ? sehErr : "");
            return fail(msg);
        }
    }

    // 能力预检(fail-open):proxy 在 Init_Ext 后写入参数块的能力键。读到
    // Available=0 / NeedsUpdatedDriver=1 时带因降级(比 CreateFeature 失败
    // 更早、原因更准);键不存在 = 旧版 proxy,保持原路径继续。
    {
        unsigned int available = 1, needsDriver = 0, maxGen = 0;
        const bool haveCap =
            _params->Get(KFG_KEY_AVAILABLE, &available) == NVSDK_NGX_Result_Success;
        const bool haveDriver =
            _params->Get(KFG_KEY_NEEDS_UPDATED_DRIVER, &needsDriver) == NVSDK_NGX_Result_Success;
        const bool haveMaxGen =
            _params->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &maxGen) ==
            NVSDK_NGX_Result_Success;
        if (haveCap && !available) {
            return fail("proxy reports DLSSG unavailable (?DLSSG.Available=0)");
        }
        if (haveDriver && needsDriver) {
            return fail("proxy reports driver update required (NeedsUpdatedDriver=1)");
        }
        if (haveCap || haveDriver || haveMaxGen) {
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "DLSSNR STATUS: dlssfg capability available=%d driver=%d maxGen=%u",
                          haveCap ? static_cast<int>(available) : -1,
                          haveDriver ? static_cast<int>(needsDriver) : -1,
                          haveMaxGen ? maxGen : 0u);
            TimingStatusLine(msg);
        }
    }

    _width = width;
    _height = height;
    if (!CreateFeatureOnCtl(width, height, backbufferFormat, err, errLen)) {
        _d3d12 = nullptr;
        _params = nullptr;
        return false;
    }
    _ready.store(true, std::memory_order_release);
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: dlssfg ready (%dx%d)", width, height);
        TimingStatusLine(msg);
    }
    return true;
}

bool DlssfgContext::CreateFeatureOnCtl(int width, int height, DXGI_FORMAT backbufferFormat,
                                       char *err, size_t errLen) noexcept {
    // ctl 路径(与 RecreateFeature 同款):CtlMutex + ctl 命令列表上
    // CreateFeature,Execute + 栅栏等待落位。
    std::lock_guard<std::mutex> ctlLock(_d3d12->CtlMutex());
    if (!_d3d12->BeginCtlRecording()) {
        if (err && errLen) std::snprintf(err, errLen, "dlssfg: BeginCtlRecording failed");
        return false;
    }
    // create 参数(官方 helper NGX_D3D12_CREATE_DLSSG 的子集;proxy 未读
    // 的键无害)。
    _params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    _params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    _params->Set(NVSDK_NGX_Parameter_Width, static_cast<unsigned int>(width));
    _params->Set(NVSDK_NGX_Parameter_Height, static_cast<unsigned int>(height));
    _params->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat,
                 static_cast<unsigned int>(backbufferFormat));
    _params->Set(NVSDK_NGX_DLSSG_Parameter_InternalWidth, static_cast<unsigned int>(width));
    _params->Set(NVSDK_NGX_DLSSG_Parameter_InternalHeight, static_cast<unsigned int>(height));
    _params->Set(NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0u);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags, DLSSG_NEVER_PROVIDED_FLAGS);
    // 调用方队列(代理自有键):CUDA 互操作与 GPU 计时诊断的锚;与槽 CL
    // 同队列 → FIFO 保序。Set 与 Eval 的其余键同款普通调用(参数块 vtable
    // 属 NGX core,与 proxy 故障隔离无关)。
    _params->Set(KFG_KEY_CMD_QUEUE, static_cast<void *>(_d3d12->Queue()));

    char sehErr[160]{};
    bool ok = SehCall([&] {
        return _createFeature(_d3d12->CtlCommandList(), FEATURE_DLSSG, _params, &_feature) ==
               NVSDK_NGX_Result_Success;
    }, "CreateFeature", sehErr, sizeof(sehErr));
    if (ok && !_feature) ok = false;
    if (!_d3d12->ExecuteCtlAndWait()) {
        if (err && errLen) std::snprintf(err, errLen, "dlssfg: ctl execute failed");
        return false;
    }
    if (!ok) {
        if (err && errLen) {
            // 官方 DLSS 同款:CreateFeature 失败后从同一参数块读 snippet 的
            // 精确初始化结果码(proxy 自有键;读不到 = 旧版 proxy,原样)。
            unsigned int initResult = 0;
            const bool haveResult =
                _params->Get(KFG_KEY_FEATURE_INIT_RESULT, &initResult) == NVSDK_NGX_Result_Success;
            char resultTag[40]{};
            if (haveResult) {
                std::snprintf(resultTag, sizeof(resultTag), " (FeatureInitResult 0x%X)", initResult);
            }
            std::snprintf(err, errLen, "dlssfg: CreateFeature failed%s%s%s",
                          sehErr[0] ? ": " : "", sehErr[0] ? sehErr : "", resultTag);
        }
        TimingStatusLine("DLSSNR STATUS: dlssfg CreateFeature failed; FG off (1:1 output)");
        return false;
    }
    return true;
}

bool DlssfgContext::Rebuild(int width, int height, DXGI_FORMAT backbufferFormat,
                            char *err, size_t errLen) noexcept {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_faulted.load(std::memory_order_acquire) || !_feature) {
        if (err && errLen) std::snprintf(err, errLen, "dlssfg: rebuild on dead session");
        return false;
    }
    if (_width == width && _height == height) {
        return true; // 同尺寸热复用,历史由调用方显式 ResetHistory
    }
    {
        const bool released = SehCall([&] {
            return _releaseFeature(_feature) == NVSDK_NGX_Result_Success;
        }, "ReleaseFeature", nullptr, 0);
        if (!released) {
            // Release 失败 = proxy 状态不可信,整体停用(官方"final release
            // 失败不可重试"同语义)。
            _ready.store(false, std::memory_order_release);
            if (err && errLen) std::snprintf(err, errLen, "dlssfg: ReleaseFeature failed on rebuild");
            return false;
        }
        _feature = nullptr;
    }
    _width = width;
    _height = height;
    if (!CreateFeatureOnCtl(width, height, backbufferFormat, err, errLen)) {
        _ready.store(false, std::memory_order_release);
        return false;
    }
    _needsReset = true;
    return true;
}

void DlssfgContext::ResetHistory() noexcept {
    std::lock_guard<std::mutex> lock(_mutex);
    _needsReset = true;
}

bool DlssfgContext::NeedsReset() noexcept {
    std::lock_guard<std::mutex> lock(_mutex);
    return _needsReset;
}

bool DlssfgContext::Evaluate(ID3D12GraphicsCommandList *cl, ID3D12Resource *backbuffer,
                             ID3D12Resource *mvec, ID3D12Resource *depth,
                             ID3D12Resource *interpOut, int width, int height,
                             int multiplier, int slotIndex,
                             bool reset, char *err, size_t errLen) noexcept {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_ready.load(std::memory_order_acquire) || _faulted.load(std::memory_order_acquire)) {
        if (err && errLen) std::snprintf(err, errLen, "dlssfg: session dead");
        return false;
    }
    const bool carryReset = _needsReset || reset;
    _needsReset = false;
    ++_frameId;

    // eval 参数(官方 helper NGX_D3D12_EVALUATE_DLSSG 的 proxy 消费面子集;
    // 布局对照 Magpie DLSSFrameGenerator.cpp:724-758 —— 恒等相机 + mvecScale
    // 1,1 + 像素单位 current-to-previous)。multiFrameCount/Index:M 倍时每
    // 真实帧产出 M-1 插值帧,slotIndex 1..M-1 必须按序递增(proxy 契约
    // "MFG indices must be evaluated in order starting at 1")。
    const unsigned int genCount =
        static_cast<unsigned int>(std::clamp(multiplier - 1, 1, kFgMultMax - 1));
    const unsigned int genIndex =
        static_cast<unsigned int>(std::clamp(slotIndex, 1, kFgMultMax - 1));
    _params->Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, backbuffer);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_MVecs, mvec);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_Depth, depth);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, interpOut);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, genCount);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, genIndex);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleX, 1.0f);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleY, 1.0f);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_DepthInverted, 0u);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_CameraMotionIncluded, 1u);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_Reset, carryReset ? 1u : 0u);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_MvecInvalidValue, 0.0f);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_MvecDilated, 1u);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_MenuDetectionEnabled, 0u);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, _frameId);
    // 正反 clip 矩阵恒等(视频无相机;proxy 明确要求这两个)。
    static const float kIdentity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };
    _params->Set(NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip, const_cast<float *>(kIdentity));
    _params->Set(NVSDK_NGX_DLSSG_Parameter_PrevClipToClip, const_cast<float *>(kIdentity));
    // MVecs/Depth 子矩形 = 全幅(同尺寸管线;子矩形键 proxy 未读时无害)。
    _params->Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectBaseX, 0u);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectBaseY, 0u);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth, static_cast<unsigned int>(width));
    _params->Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, static_cast<unsigned int>(height));
    _params->Set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectBaseX, 0u);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectBaseY, 0u);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectWidth, static_cast<unsigned int>(width));
    _params->Set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectHeight, static_cast<unsigned int>(height));
    _params->Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectBaseX, 0u);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectBaseY, 0u);
    _params->Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectWidth, static_cast<unsigned int>(width));
    _params->Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectHeight, static_cast<unsigned int>(height));

    char sehErr[160]{};
    bool ok = SehCall([&] {
        return _evaluateFeature(cl, _feature, _params, nullptr) == NVSDK_NGX_Result_Success;
    }, "EvaluateFeature", sehErr, sizeof(sehErr));
    if (!ok) {
        _ready.store(false, std::memory_order_release);
        if (err && errLen) {
            std::snprintf(err, errLen, "dlssfg: EvaluateFeature failed%s%s",
                          sehErr[0] ? ": " : "", sehErr[0] ? sehErr : "");
        }
        TimingStatusLine("DLSSNR STATUS: dlssfg evaluate failed; FG off (dup fallback)");
    }
    return ok;
}

void DlssfgContext::Disable(const char *why) noexcept {
    std::lock_guard<std::mutex> lock(_mutex);
    _ready.store(false, std::memory_order_release);
    char msg[160];
    std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: dlssfg disabled (%s)", why ? why : "?");
    TimingStatusLine(msg);
}

} // namespace vsdlssnr
