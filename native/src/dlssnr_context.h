#pragma once
// Ported from Magpie experimental DLSSNRFilter.cpp / NgxD3D12Core.cpp.
// NGX static core (nvsdk_ngx_s.lib) owns the parameter block; the signed
// snippet nvngx_dlssnr.dll (Feature 18) performs CreateFeature/EvaluateFeature.
// Guidance:零 guidance(静态零纹理,等价 Magpie guidanceMode=1 Force Zero)
// 或 NVOF 真运动矢量(PORTING #6,NvofContext),由 motionVectorQuality 切换。

#include "d3d12_context.h"
#include "dlssnr_params.h"
#include "iat_hook.h"
#include "nvof_context.h"
#include "shared_params.h"
#include <atomic>
#include <cstdio>
#include <memory>
#include <vector>
#include <mutex>
#include <nvsdk_ngx.h>

namespace vsdlssnr {

// Panel toggle for the periodic perf log (dlssnr_timing.log)
void SetTimingLogEnabled(bool enabled) noexcept;

// Exported TimingLog wrapper for nvof_context.cpp(TimingLog 本体在匿名命名空间)。
void TimingStatusLine(const char *line) noexcept;

// 逐帧级探针开关(VSDLSSNR_PROBE=1);d3d12_context 的 init 细分探针共用。
bool ProbeEnabled() noexcept;

class DlssnrContext {
public:
    DlssnrContext() = default;
    ~DlssnrContext();
    DlssnrContext(const DlssnrContext &) = delete;
    DlssnrContext &operator=(const DlssnrContext &) = delete;

    bool Initialize(D3D12Context &d3d12, const wchar_t *ngxDllPath,
                    int width, int height, int depth, SharedParams *shared,
                    char *err, size_t errLen) noexcept;
    void Shutdown() noexcept;

    // Hot-context rebind: attach this kept-warm context (device, NGX feature,
    // slot pool all alive) to a new filter instance's SharedParams — including
    // one for a different video size or bit depth (frame resources + feature
    // rebuild under the pool seal; only a changed snippet DLL forces a full
    // re-init at the caller). The feature is rebuilt only when a create-time
    // parameter (preset / input_resolution / scaling_enabled) or the
    // size/depth actually differs; otherwise it is free. Returns false (and
    // leaves _ready false) when the rebuild fails; the caller then falls back
    // to a full Initialize.
    bool Rebind(SharedParams *shared, int width, int height, int depth, char *err, size_t errLen) noexcept;

    // Preset / internal-resolution / scaling-toggle are create-time NGX keys:
    // on panel change the frame thread rebuilds the feature (and scaling
    // textures for resolution changes; disabled = residual pipeline dropped).
    // newWidth/newHeight/depth >= 0 additionally rebuild the per-slot frame
    // resources for that geometry (used by Rebind across resolutions/depth).
    bool RecreateFeature(int preset, int resPercent, int scalingEnabled, char *err, size_t errLen,
                         int newWidth = -1, int newHeight = -1, int newDepth = -1) noexcept;

    // NVOF 会话重建(quality 变化)。只重建光流会话(PoolHold 内,
    // 毫秒级),NGX feature 不动。quality == 0 时销毁会话回退零 guidance;
    // 会话建立失败时优雅降级(记档位防逐帧重试风暴)。内部自取 PoolHold:
    // 调用方必须尚未持有槽位,且不得已在 PoolHold 之中。
    bool RebuildNvof(int quality, int dstW, int dstH, char *err, size_t errLen) noexcept;
    // 光流历史失效(seek = 新时间线)。热 Rebind 上调用;下一帧重新播种。
    void ResetNvofHistory() noexcept;

    // YUV420P8/P10 三平面进 → 处理 → 同格式三平面出(同分辨率)。
    // matrix/range 来自源帧属性(props _Matrix/_ColorRange,plugin.cpp 读;
    // 缺省 709 limited)—— YUV↔RGB GPU 转换按此展开/压缩。
    // n is the frame index; discontinuity detection (NGX history reset) is
    // owned here, not by the glue layer. timingOut 非 NULL 时写入分段耗时
    // (毫秒,逗号分隔:pack,submit+gpu,unpack)
    bool ProcessFrame(const uint8_t *const *srcPlanes, const int64_t *srcStrides,
                      uint8_t **dstPlanes, int64_t *dstStrides,
                      int width, int height, int n,
                      ColorMatrix matrix, ColorRange range,
                      char *err, size_t errLen,
                      char *timingOut = nullptr, size_t timingLen = 0) noexcept;

private:
    // ---- SEH wrappers (Magpie style; every NGX call is wrapped) ----
    NVSDK_NGX_Result CoreInitSafely(const wchar_t *appDir, ID3D12Device *device,
                                    const NVSDK_NGX_FeatureCommonInfo *info, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result CoreAllocateParametersSafely(NVSDK_NGX_Parameter **out, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result CoreDestroyParametersSafely(NVSDK_NGX_Parameter *p, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result CoreShutdownSafely(ID3D12Device *device, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result SnippetInitSafely(const wchar_t *appDataPath, ID3D12Device *device, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result SnippetCreateFeatureSafely(ID3D12GraphicsCommandList *cl, NVSDK_NGX_Parameter *params, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result SnippetEvaluateSafely(ID3D12GraphicsCommandList *cl, NVSDK_NGX_Parameter *params, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result SnippetReleaseSafely(DWORD *sehCode) noexcept;
    NVSDK_NGX_Result SnippetShutdownSafely(DWORD *sehCode) noexcept;
    void SetCreateParametersUnsafe() noexcept;
    bool SetCreateParametersSafely(DWORD *sehCode) noexcept;
    void SetEvaluateParametersUnsafe(FrameSlot &slot, bool resetHistory, bool realMotion) noexcept;
    bool SetEvaluateParametersSafely(FrameSlot &slot, bool resetHistory, bool realMotion, DWORD *sehCode) noexcept;

    // 死亡状态边缘发布(state = "passthrough" | "ngx_faulted",detail 为原因
    // 串,内部消毒)。发布即替换共享内存里的旧统计 body —— 帧已不再成功,
    // tick 停摆,不替换面板就会一直显示冻结的"NGX 延迟"。
    void PublishDeadState(const char *state, const char *detail) noexcept;
    // NVOF 实际模式串(SK_OF_MODE):档位关闭 = "off",会话死亡 = "zero",
    // 存活 = 能力组合 + 当前档位/网格("both+cost q2 grid4")——能力在同
    // 一块 GPU 上不随档位变化,档位/网格才是切档可见的反馈。tick 与
    // Initialize 的 stats 发布共用。
    char _ofModeBuf[28] = "";
    const char *OfModeString() noexcept {
        if (_curOfQuality <= 0) return "off";
        if (!_nvof || !_nvof->Enabled()) return "zero";
        std::snprintf(_ofModeBuf, sizeof(_ofModeBuf), "%s q%d grid%u",
                      _nvof->Bidirectional()
                          ? (_nvof->CostEnabled() ? "both+cost" : "both")
                          : (_nvof->CostEnabled() ? "forward+cost" : "forward"),
                      _curOfQuality, _nvof->GridSize());
        return _ofModeBuf;
    }

    D3D12Context *_d3d12 = nullptr;
    NVSDK_NGX_Parameter *_parameters = nullptr;
    NVSDK_NGX_Handle *_feature = nullptr;
    HMODULE _snippetModule = nullptr;
    SnippetCallerHook _hook{};
    SharedParams *_shared = nullptr;

    using SnippetInitExtFn = NVSDK_NGX_Result(NVSDK_CONV *)(
        unsigned long long, const wchar_t *, ID3D12Device *, NVSDK_NGX_Version, const NVSDK_NGX_Parameter *);
    using CreateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV *)(
        ID3D12GraphicsCommandList *, NVSDK_NGX_Feature, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
    using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV *)(
        ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback);
    using ReleaseFeatureFn = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Handle *);
    using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12Device *);

    SnippetInitExtFn _snippetInitExt = nullptr;
    CreateFeatureFn _snippetCreateFeature = nullptr;
    EvaluateFeatureFn _snippetEvaluateFeature = nullptr;
    ReleaseFeatureFn _snippetReleaseFeature = nullptr;
    ShutdownFn _snippetShutdown = nullptr;

    wchar_t _appDataPath[MAX_PATH]{};
    int _width = 0;
    int _height = 0;
    int _depth = 0; // YUV 位深(8/10;CreateFrameResources/resize 判据/日志)
    // Adapter description in UTF-8, filled once in Initialize and reused by
    // every per-frame stats publish (GetDesc per frame is wasted work).
    char _gpuNameUtf8[160] = "UNAVAILABLE";
    bool _coreInitialized = false;
    bool _snippetInitialized = false;
    // Cross-thread: written by one frame thread (device-lost latch /
    // failed rebuild) while others read it under fmParallel.
    std::atomic<bool> _ready{false};
    // create-time parameters currently baked into the NGX feature (Rebind
    // compares against these to skip a no-op RecreateFeature)
    int _curPreset = -1;
    int _curRes = -1;
    bool _curScaling = false;
    // NVOF 光流会话(PORTING #6)。_curOfQuality = 当前生效档位(0 = 零
    // guidance);_nvofFailed = 会话建立失败或连续失败停用(回退零 guidance,
    // Rebind/换档时重试)。_nvofMutex 串行化 RebuildNvof:fmParallel 下多个
    // 帧线程会同时看到同一档位变化,不加锁会并发重建互相踩踏。
    std::unique_ptr<NvofContext> _nvof;
    // 退役会话:实测 nvOFDestroy + 资源释放后继续 GPU 工作会触发驱动内部
    // 访问违例(nvwgf2umx,2026-09-07),换档/换尺寸的旧会话转入此名单
    // 存活到进程退出(热上下文哲学;每会话约 2×W×H×4B 显存)。
    std::vector<std::unique_ptr<NvofContext>> _retiredNvof;
    int _curOfQuality = 0;
    bool _nvofFailed = false;
    std::mutex _nvofMutex;
    // fmParallel: several frame threads call EvaluateFeature concurrently.
    // The feature and the parameter block are singletons, so evaluate
    // (parameter setup + snippet call) is serialized; GPU-side dispatches
    // still overlap via each slot's own command list.
    std::mutex _evaluateMutex;
    // 面板可见的死亡状态发布(passthrough / ngx_faulted):边缘触发,每状态
    // 每实例一次。存活态(ok / nvof_zero)由周期 stats tick 携带,不走这里。
    // 0 = passthrough,1 = ngx_faulted,-1 = 尚未发布过。
    std::atomic<int> _lastDeadState{ -1 };
    // 最近一次进入 ProcessFrame 的帧号(recreate/错误 STATUS 行带上它,
    // 用户"第几秒看到异常"即可与 timing log 的帧号对上)。
    std::atomic<int> _lastFrameN{ -1 };
};

} // namespace vsdlssnr
