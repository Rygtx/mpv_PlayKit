#pragma once
// RTX Video(NGX VSR + TrueHDR)feature 上下文 —— NVIDIA RTX Video SDK 1.1
// 的两个官方 NGX feature,与 NR snippet / DLSSG 同一 NGX core(共享静态核
// nvsdk_ngx_s + PathList=ngx\ 目录解析 nvngx_vsr.dll / nvngx_truehdr.dll,
// 本类不做 LoadLibrary)。
//   RtxVsrContext  — Feature 16(Reserved16):SDR RGB 放大;输入/输出
//                    BGRA8(+R10G10B10A2),quality 0=bicubic 1-4=AI;
//                    create 与尺寸无关 —— rect 是 eval 参数,尺寸变化只
//                    需重建槽资源,feature 本体跨 seek/分辨率热复用。
//   RtxHdrContext  — Feature 14(Reserved14):SDR RGB → FP16 scRGB 线性
//                    (R16G16B16A16_FLOAT;Magpie RTXVideoHdr 同语义)。
//                    官方契约:TrueHDR 不能吃 HDR 输入、必须排在 VSR 之后
//                    (Programming Guide 3.3.5)。参数 Contrast/Saturation/
//                    MiddleGray/MaxLuminance 为 per-eval。
//
// ---- 执行模型:专用命令队列(SDK D3D12 样例 CDx12NGXVSR 同款)----
// 真机实测(2026-09-22,RTX 3080):在应用的命令列表上录制 VSR eval 会让
// 该列表 Close() 报 E_INVALIDARG —— nvngx_vsr 与 NR/DLSSG snippet 不同,
// 不接受宿主 CL。样例形态 = feature 自持 DIRECT 队列 + allocator + CL,
// eval 时:队列 Wait(生产者 fence)→ 自家 CL eval → Close → Execute →
// Signal(自家 fence);消费者队列 Wait 该 fence 后再读输出。
// 状态契约(实测成立,SDK 样例同款):
//   输入:调用方在应用 CL 上转 NSR(NGX 直读,不再屏障)。
//   输出:NGX 在自家 CL 上自行屏障做 UAV 写;Execute 完成后 UAV 写资源
//        自动衰减回 COMMON(D3D12 隐式衰减)→ 调用方以 SRV 隐式提升读
//        (COMMON→NSR promotion),无显式状态交接。
// 集成纪律:
//   1. 故障隔离 —— SEH 走本类本地闩锁(_faulted),不上抛全局
//      NgxRuntimeGuard:VSR/HDR 崩溃只停本功能,绝不连带杀 NR。
//   2. 串行契约 —— Evaluate 期间调用方持有 _evaluateMutex(NGX 非线程
//      安全,NR/DLSSG/VSR/HDR 全部经同一互斥串行)。
//   3. 能力预检 —— capability 块(VSR.Available / TrueHDR.Available)在
//      Initialize 预检,不可用即失败,由调用方优雅降级。
//   4. 队列/feature 与热上下文同哲学:进程生存期,不做销毁路径(NGX
//      Release 与队列析构在 loader 锁下有死锁前科,进程退出统一回收)。

#include "d3d12_context.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_truehdr.h> // VSR/TrueHDR capability 键 + Feature ID
#include <nvsdk_ngx_defs_vsr.h>
#include <wrl/client.h>

namespace vsdlssnr {

using Microsoft::WRL::ComPtr;

// VSR / TrueHDR 共用的专用队列执行体(各自实例一份,互不串扰)。
class RtxQueue {
public:
    RtxQueue() = default;
    RtxQueue(const RtxQueue &) = delete;
    RtxQueue &operator=(const RtxQueue &) = delete;

    bool Initialize(ID3D12Device *device, const char *label, char *err, size_t errLen) noexcept;
    // 生产者等待(waitFence)排队 → CL 重置 → fn(CLI) 录制 → Close →
    // Execute → Signal(自家 fence)→ 返回本次 fence 值。fn 返回 false =
    // NGX eval 失败(录制中止,不上 GPU)。
    bool Execute(ID3D12Fence *waitFence, uint64_t waitValue,
                 const char *what, char *err, size_t errLen,
                 const std::function<bool(ID3D12GraphicsCommandList *)> &fn,
                 uint64_t *signalValueOut) noexcept;
    // CPU 等待某次 Execute 的完成点(节点依赖链的 CPU 侧锚;GPU 侧顺序由
    // 消费者队列 Wait 保证,CPU 等待只为统一 Unpack 的完成时序)。
    // timeoutMs:分段计时用有界等待(超时 = 队列 wedge/设备丢失,交由
    // WaitFrame 的既有失败路径收尾);同 allocator 复用的背压等待保持
    // INFINITE。
    bool Wait(uint64_t value, char *err, size_t errLen, DWORD timeoutMs = INFINITE) noexcept;
    ID3D12Fence *Fence() const noexcept { return _fence.Get(); }

private:
    ComPtr<ID3D12CommandQueue> _queue;
    // **六组 allocator/CL 轮转(0-5)**:单 allocator 下 back-to-back eval
    //(HDR 链逐插值帧)的 allocator Reset 必须等上一笔 GPU 完成(lookback
    // wait)→ CPU 提交链被 GPU 执行串行吸收(实测 sub=17ms,2026-09-24),
    // vsr/fg 分段观测被填满塌 0。双组(0/1)在 mult=4 HDR 链(每源帧 4 笔
    // TrueHDR 背靠背)仍被同帧 GPU 顶住:第 2/3 笔插值 eval 各阻塞等上一笔
    // GPU 完成(eval_cpu 16-18ms 实测,2026-09-24 日志定案)。**上限是 6x**
    //(kFgMultMax=6,MaxGeneratedFrames=5):默认模式每源帧 ≤6 笔 TrueHDR,
    // 六组轮转 = 同 idx 间隔 6 笔,跨帧的 Reset 目标永不在飞,提交链回归
    // 纯录制;GPU 落后超 6 笔同 idx(>12 笔在飞)时 INFINITE 等待 = 背压
    // 兜底。VSR 队列独立实例、每帧 ≤1 笔,不构成约束。
    static constexpr int kAltCount = 6;
    ComPtr<ID3D12CommandAllocator> _allocator[kAltCount];
    ComPtr<ID3D12GraphicsCommandList> _commandList[kAltCount];
    // 每 idx 最近一次 Signal 的完成栅栏值(Reset 前有界等待它 —— 该 idx
    // 上一笔在飞时的诚实背压)。
    uint64_t _lastSignal[kAltCount] = {};
    ComPtr<ID3D12Fence> _fence;
    HANDLE _event = nullptr;
    std::atomic<uint64_t> _fenceValue{ 0 };
};

class RtxVsrContext {
public:
    RtxVsrContext() = default;
    ~RtxVsrContext();
    RtxVsrContext(const RtxVsrContext &) = delete;
    RtxVsrContext &operator=(const RtxVsrContext &) = delete;

    // params 为 NGX core 的 GetCapabilityParameters 块(调用方拥有并负责
    // 销毁,与 DlssfgContext 的 _fgParams 同约定)。内部自取 CtlMutex。
    bool Initialize(D3D12Context &d3d12, NVSDK_NGX_Parameter *params,
                    int width, int height, char *err, size_t errLen) noexcept;

    // 专用队列上执行一次 VSR eval。waitFence/waitValue = 生产者(NR 段)
    // 的完成栅栏,消费顺序由 GPU 排队保证;*signalValueOut 返回本次完成
    // 点(调用方 CPU 等待 + 消费者队列 Wait)。caller 已持 _evaluateMutex。
    bool Evaluate(ID3D12Resource *input, int inW, int inH,
                  ID3D12Resource *output, int outW, int outH,
                  int quality,
                  ID3D12Fence *waitFence, uint64_t waitValue,
                  uint64_t *signalValueOut,
                  char *err, size_t errLen) noexcept;

    bool Enabled() const noexcept { return _ready.load(std::memory_order_acquire); }
    RtxQueue &Queue() noexcept { return _queue; }

private:
    bool CreateFeatureOnCtl(char *err, size_t errLen) noexcept;

    template <typename Fn>
    bool SehCall(Fn &&fn, const char *what, char *err, size_t errLen) noexcept;

    D3D12Context *_d3d12 = nullptr;
    NVSDK_NGX_Parameter *_params = nullptr; // 借用;core 拥有
    NVSDK_NGX_Handle *_feature = nullptr;
    RtxQueue _queue;
    std::atomic<bool> _ready{false};
    std::atomic<bool> _faulted{false};
};

class RtxHdrContext {
public:
    RtxHdrContext() = default;
    ~RtxHdrContext();
    RtxHdrContext(const RtxHdrContext &) = delete;
    RtxHdrContext &operator=(const RtxHdrContext &) = delete;

    bool Initialize(D3D12Context &d3d12, NVSDK_NGX_Parameter *params,
                    int width, int height, char *err, size_t errLen) noexcept;

    // TrueHDR 不缩放:inW/inH 必须等于 outW/outH(官方 eval 的 in/out rect
    // 各自独立,SDK 样例同尺寸使用;本插件恒 1:1)。
    bool Evaluate(ID3D12Resource *input, int w, int h, ID3D12Resource *output,
                  int contrast, int saturation, int middleGray, int maxLuminance,
                  ID3D12Fence *waitFence, uint64_t waitValue,
                  uint64_t *signalValueOut,
                  char *err, size_t errLen) noexcept;

    bool Enabled() const noexcept { return _ready.load(std::memory_order_acquire); }
    RtxQueue &Queue() noexcept { return _queue; }

private:
    bool CreateFeatureOnCtl(char *err, size_t errLen) noexcept;

    template <typename Fn>
    bool SehCall(Fn &&fn, const char *what, char *err, size_t errLen) noexcept;

    D3D12Context *_d3d12 = nullptr;
    NVSDK_NGX_Parameter *_params = nullptr; // 借用;core 拥有
    NVSDK_NGX_Handle *_feature = nullptr;
    RtxQueue _queue;
    std::atomic<bool> _ready{false};
    std::atomic<bool> _faulted{false};
};

} // namespace vsdlssnr
