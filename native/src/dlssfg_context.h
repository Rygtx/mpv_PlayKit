#pragma once
// DLSS 帧生成上下文 —— 官方 NGX 链驱动 NGX DLSSG(Feature 11)eval 契约:
//   OfficialNgx — 官方签名 nvngx_dlssg.dll 经共享 NGX core 解析加载(PORTING
//                #8;Magpie DLSSFrameGenerator 同款):无自签直驱,SM89/SM120
//                cubin 由 NVIDIA 预编译。调用方以 GetCapabilityParameters 块
//                传入,_params 必须来自
//                NVSDK_NGX_D3D12_GetCapabilityParameters(官方 DLSSG 的
//                create/eval 块);能力键 FrameGeneration.Available 在本类
//                Initialize 内预检,不可用即失败由调用方定夺。
//   RTX 30/20 的 DLSS-G 由 dlssg_for_sm86 0.3.x hook 代理接管交付(部署侧
//   version.dll,自动档预载):其 LoadLibrary 钩子拦截 nvngx_dlssg.dll 加载
//   替换为内嵌运行库 + SM86 后端,fg_gate 钩子接答核心能力查询/
//   CreateFeature —— 本类仍按官方链驱动,链路形态不变。0.2.4 直驱 proxy
//   契约(Init_Ext + 专用参数块)已删除,不再兼容。
// 与 NR snippet 同款集成形态;区别于 NR 的两点:
//   1. 故障隔离 —— FG 的 SEH 走本类本地闩锁(_faulted),不上抛全局
//      NgxRuntimeGuard:FG 崩溃只降级本功能(复制真实帧),绝不连带杀 NR。
//   2. 参数契约 —— 设官方 eval 契约全量(Magpie optionalParams:五矩阵恒等/
//      相机单位基座/jitter 0/可选资源 null)。
// 时序契约(README:输入 NSR / 输出 UAV,提交与同步归调用方):插值输出的
// 就绪由本槽 WaitFrame(fg 段栅栏)覆盖(与 NVOF densify 同款"同队列 FIFO"
// 论证);首个 eval 的输出异常由 VSDLSSNR_DUMP 诊断。

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

    // 官方链初始化:dllPath(官方 snippet)仅作部署指纹日志,允许为空。
    // params 为 NGX core 的 GetCapabilityParameters 块(调用方拥有并负责
    // 销毁)。内部自取 CtlMutex;调用方不得持有槽位(PoolHold 语义同
    // RecreateFeature)。
    bool Initialize(D3D12Context &d3d12, const wchar_t *dllPath,
                    NVSDK_NGX_Parameter *params,
                    int width, int height, DXGI_FORMAT backbufferFormat,
                    char *err, size_t errLen) noexcept;

    // 预载 hook 代理模块(0.3.x:LoadLibrary 即装钩,先于 DLSS-G 能力查询
    // —— 时序铁律,见 dlssnr_context FG 段)。经 FgModule 进程级缓存,重复
    // 调用路径相同即命中;永不 FreeLibrary —— 与 nvofapi64.dll 同哲学。
    // 返回预载后模块是否可用。
    static bool PreloadProxyModule(const wchar_t *dllPath,
                                   char *err = nullptr, size_t errLen = 0) noexcept;

    // 缓存中的 proxy 模块是否 0.3.x hook 型(有 DlssgProxy_Role 查询导出;
    // 仅查已缓存模块,未加载过 = false)。用于 fg_route_eff 定名
    // (official-hook/official)—— 钩子进程级不可拆,与本次是否预载解耦。
    static bool CachedProxyIsHookStyle() noexcept;

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

    // 运行库插值帧上限(能力键 MultiFrameCountMax,= 倍数上界-1;代理 ini
    // MaxGeneratedFrames 钳定,默认 kFgMultMax-1)。Initialize 时定格。
    int MaxGen() const noexcept { return _maxGen; }

    // 每处理帧每插值槽一次(fmParallel 并发由内部互斥串行;GPU dispatch 仍
    // 随各槽命令列表重叠)。multiplier = 本源帧倍数 M(2-6),slotIndex =
    // 插值槽 1..M-1(同源帧内必须按序调用 —— 官方 MFG 契约)。cl = 槽命令
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

    // 函数指针 = 静态 SDK(nvsdk_ngx_s;官方链无模块加载)。
    using CreateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV *)(
        ID3D12GraphicsCommandList *, NVSDK_NGX_Feature, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
    using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV *)(
        ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *,
        PFN_NVSDK_NGX_ProgressCallback);
    using ReleaseFeatureFn = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Handle *);

    D3D12Context *_d3d12 = nullptr;
    NVSDK_NGX_Parameter *_params = nullptr; // 借用;core 拥有
    NVSDK_NGX_Handle *_feature = nullptr;
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
    int _maxGen = kFgMultMax - 1; // 运行库插值帧上限(能力键覆写)
};

} // namespace vsdlssnr
