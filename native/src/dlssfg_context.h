#pragma once
// DLSS 帧生成上下文 —— 经 dlssg_for_sm86 的原生 proxy(version.dll)驱动
// NGX DLSSG(Feature 11)eval 契约。与 NR snippet 同款集成形态:
// LoadLibrary(用户自备 DLL,进程级钉住)→ Init_Ext → 核心参数块 CreateFeature
// → 槽命令列表上 EvaluateFeature。区别于 NR 的两点:
//   1. 故障隔离 —— proxy 的 SEH 走本类本地闩锁(_faulted),不上抛全局
//      NgxRuntimeGuard:FG 崩溃只降级本功能(复制真实帧),绝不连带杀 NR。
//   2. 参数契约 —— 只设 proxy 声明的消费面(DLSSG.Backbuffer/MVecs/Depth/
//      OutputInterpolated/Reset/MultiFrame*/MvecScale*/ClipToPrevClip/
//      PrevClipToClip/DepthInverted/CmdQueue);多余键(BackbufferFrameID 等)
//      对忽略未知键的实现无害,对按官方契约实现的消费者是正确输入。
// 时序契约(README:输入 NSR / 输出 UAV,提交与同步归调用方):proxy 的
// CUDA 互操作工作以 DLSSG.CmdQueue/D3D12 互操作语义与槽队列保序,插值
// 输出的就绪由本槽 SubmitFrame→WaitFrame 的栅栏覆盖(与 NVOF densify 同
// 款"同队列 FIFO"论证);首个 eval 的输出异常由 VSDLSSNR_DUMP 诊断。

#include "d3d12_context.h"
#include <atomic>
#include <mutex>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_dlssg.h>

namespace vsdlssnr {

class DlssfgContext {
public:
    DlssfgContext() = default;
    ~DlssfgContext();
    DlssfgContext(const DlssfgContext &) = delete;
    DlssfgContext &operator=(const DlssfgContext &) = delete;

    // 加载 proxy 模块(进程级缓存,路径变化才重载;永不 FreeLibrary ——
    // 与 nvofapi64.dll 同哲学)→ Init_Ext → ctl 路径 CreateFeature。
    // params 为 NGX core 分配的专用参数块(自描述对象,proxy 经 vtable
    // 读取;调用方拥有并负责销毁)。内部自取 CtlMutex;调用方不得持有
    // 槽位(PoolHold 语义同 RecreateFeature)。
    bool Initialize(D3D12Context &d3d12, const wchar_t *dllPath,
                    const wchar_t *appDataPath, NVSDK_NGX_Parameter *params,
                    int width, int height, DXGI_FORMAT backbufferFormat,
                    char *err, size_t errLen) noexcept;

    // 尺寸变化重建 feature(旧 handle 经 ReleaseFeature 退役;Release 失败
    // 即整体停用 —— 与 NR 的"Release 后不安全重试"同语义)。PoolHold 内调用。
    bool Rebuild(int width, int height, DXGI_FORMAT backbufferFormat,
                 char *err, size_t errLen) noexcept;

    // seek = 新时间线:下一次 Evaluate 携带 DLSSG.Reset=1,该帧插值输出
    // 不消费(调用方以复制真实帧替代)。
    void ResetHistory() noexcept;

    // 下一帧 eval 是否为播种(带 Reset)。调用方据此不消费该帧插值输出
    // (播种只建历史)。_mutex 内读,帧线程安全。
    bool NeedsReset() noexcept;

    bool Enabled() const noexcept { return _ready.load(std::memory_order_acquire); }

    // 每处理帧每插值槽一次(fmParallel 并发由内部互斥串行;GPU dispatch 仍
    // 随各槽命令列表重叠)。multiplier = 本源帧倍数 M(2-4),slotIndex =
    // 插值槽 1..M-1(同源帧内必须按序调用 —— proxy 契约)。cl = 槽命令
    // 列表;资源状态契约:backbuffer/mvec/depth = NSR,interpOut = UAV
    // (调用方负责屏障)。reset=true 的 eval 属于重置帧,输出不消费。返回
    // false 时调用方降级复制真实帧;连续失败由本类闩锁(_ready=false)停用
    // 整个 FG 会话。
    bool Evaluate(ID3D12GraphicsCommandList *cl, ID3D12Resource *backbuffer,
                  ID3D12Resource *mvec, ID3D12Resource *depth,
                  ID3D12Resource *interpOut, int width, int height,
                  int multiplier, int slotIndex,
                  bool reset, char *err, size_t errLen) noexcept;

private:
    bool CreateFeatureOnCtl(int width, int height, DXGI_FORMAT backbufferFormat,
                            char *err, size_t errLen) noexcept;
    void Disable(const char *why) noexcept;

    // FG 本地 SEH:__try/__except 捕获后只置 _faulted(本类闩锁),
    // 不进全局 NgxRuntimeGuard。实现见 .cpp(与 NgxRuntimeGuard 同构)。
    template <typename Fn>
    bool SehCall(Fn &&fn, const char *what, char *err, size_t errLen) noexcept;

    using InitExtFn = NVSDK_NGX_Result(NVSDK_CONV *)(
        unsigned long long, const wchar_t *, ID3D12Device *, NVSDK_NGX_Version,
        const NVSDK_NGX_Parameter *);
    using CreateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV *)(
        ID3D12GraphicsCommandList *, NVSDK_NGX_Feature, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
    using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV *)(
        ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *,
        PFN_NVSDK_NGX_ProgressCallback);
    using ReleaseFeatureFn = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Handle *);

    D3D12Context *_d3d12 = nullptr;
    NVSDK_NGX_Parameter *_params = nullptr; // 借用;core 拥有
    NVSDK_NGX_Handle *_feature = nullptr;
    InitExtFn _initExt = nullptr;
    CreateFeatureFn _createFeature = nullptr;
    EvaluateFeatureFn _evaluateFeature = nullptr;
    ReleaseFeatureFn _releaseFeature = nullptr;

    std::mutex _mutex;            // eval + history + 计数器串行(fmParallel)
    std::atomic<bool> _ready{false};
    std::atomic<bool> _faulted{false};
    bool _needsReset = true;      // 构造/seek/重建后 = true(_mutex 保护)
    unsigned long long _frameId = 0; // DLSSG.BackbufferFrameID 单调计数
    int _width = 0;
    int _height = 0;
};

} // namespace vsdlssnr
