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
#include "ngx_seh_gate.h" // SehCall 模板(基类内联)经此展开
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
    // 只关事件句柄(COM 成员随 ComPtr 自释放;queue/fence 的进程级回收哲学
    // 不变):此前 _event 无人关,VSR/HDR 每次 off→on 循环漏一个句柄
    // (2026-10-05 评审修)。
    ~RtxQueue() { if (_event) CloseHandle(_event); }

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
    // timeoutMs:有界等待(调用点恒 10000;超时 = 专用队列 wedge/设备丢失,
    // 交由 WaitFrame 的既有失败路径收尾 —— 含 allocator 复用的背压等待,
    // 此前"背压保持 INFINITE"的语义已随 5e239eb 修复废弃)。
    bool Wait(uint64_t value, char *err, size_t errLen, DWORD timeoutMs) noexcept;
    // 括号段完成锚点(2026-10-04):post 计时括号在 Execute 的 Signal 之后
    // 入队,eval 栅栏值不覆盖括号执行 —— 帧槽回收若只等 eval 值,可对在飞
    // 括号 allocator Reset(UB)。调用方在本队列该帧全部提交收尾后调用,
    // 补一记 Signal 并返回新值;消费方等此值才真正蕴含括号完成。返回 0 =
    // Signal 失败(设备域故障),调用方回落 eval 值(帧照常走失败路径)。
    uint64_t SignalNow() noexcept;
    ID3D12Fence *Fence() const noexcept { return _fence.Get(); }
    // 原生队列句柄(时间戳括号夹提交用,2026-10-02):帧路径在本线程、
    // Evaluate 前后紧邻提交括号 CL —— 与 Execute 的提交是同线程程序序,
    // FIFO 保证括号包住 eval;跨帧线程经 _evaluateMutex 串行,无乱序。
    ID3D12CommandQueue *Native() const noexcept { return _queue.Get(); }

private:
    ComPtr<ID3D12CommandQueue> _queue;
    // **十二组 allocator/CL 轮转**:单 allocator 下 back-to-back eval
    //(HDR 链逐插值帧)的 allocator Reset 必须等上一笔 GPU 完成(lookback
    // wait)→ CPU 提交链被 GPU 执行串行吸收(实测 sub=17ms,2026-09-24),
    // vsr/fg 分段观测被填满塌 0。双组(0/1)在 mult=4 HDR 链(每源帧 4 笔
    // TrueHDR 背靠背)仍被同帧 GPU 顶住:第 2/3 笔插值 eval 各阻塞等上一笔
    // GPU 完成(eval_cpu 16-18ms 实测,2026-09-24 日志定案)。**上限是 6x**
    //(kFgMultMax=6,MaxGeneratedFrames=5):默认模式每源帧 ≤6 笔 TrueHDR,
    // 十二组轮转 = 同 idx 间隔 12 笔。HDR 链每帧笔数 = 1 真实 + (M-1) 插值,
    // fgM=6 时 = 6 笔 —— 旧 6 组恰被单帧消费完,下一帧第一笔必撞同 idx 的
    // Reset 前等待(等上一帧首笔 GPU 完成),吃掉 Submit 半段与上一帧 GPU
    // 执行的重叠(2026-09-25 审查)。12 = 6 笔/帧 × 2 帧在飞(fgMutex 串行
    // Submit、Finish 锁外,同队列最多 2 帧的链同时在飞)满额 + 余量;背压
    // 语义不变(GPU 落后超 12 笔同 idx = >24 笔在飞时有界等待兜底)。VSR
    // 队列独立实例、每帧 ≤1 笔,不构成约束。
    static constexpr int kAltCount = 12;
    ComPtr<ID3D12CommandAllocator> _allocator[kAltCount];
    ComPtr<ID3D12GraphicsCommandList> _commandList[kAltCount];
    // 每 idx 最近一次 Signal 的完成栅栏值(Reset 前有界等待它 —— 该 idx
    // 上一笔在飞时的诚实背压)。
    uint64_t _lastSignal[kAltCount] = {};
    ComPtr<ID3D12Fence> _fence;
    HANDLE _event = nullptr;
    std::atomic<uint64_t> _fenceValue{ 0 };
};

// VSR / TrueHDR 共用骨架(接入流程唯一实现;此前 Initialize/CreateFeatureOnCtl
// /SehCall/Evaluate 壳在两类间逐字复制约 180 行,修过的 bug 必须双处同步,
// 且已出现微漂移 —— VSR 的 Evaluate 失败补写 err 有空串守卫、HDR 版没有)。
// 子类只填差异点:能力键、create/eval 的 EXT 调用与日志用名。
class RtxFeatureBase {
public:
    RtxFeatureBase() = default;
    virtual ~RtxFeatureBase();
    RtxFeatureBase(const RtxFeatureBase &) = delete;
    RtxFeatureBase &operator=(const RtxFeatureBase &) = delete;

    bool Enabled() const noexcept { return _ready.load(std::memory_order_acquire); }
    RtxQueue &Queue() noexcept { return _queue; }

protected:
    // 通用接入序:能力预检(FeatureInitResult 精确归因)→ ctl 上
    // CreateFeature → 专用队列初始化 → ready 发布。失败 = 清引用 + STATUS
    // 留痕(调用方降级收口)。
    bool InitializeCommon(D3D12Context &d3d12, NVSDK_NGX_Parameter *params,
                          int width, int height, char *err, size_t errLen) noexcept;
    // ctl 路径 CreateFeature(与 DLSSFG 同款:CtlMutex + ctl CL + Execute
    // 等待)。create 与尺寸无关。
    bool CreateFeatureOnCtl(char *err, size_t errLen) noexcept;
    // Evaluate 通用壳:检活 → SehCall 包专用队列 Execute → 失败闩停 +
    // STATUS 留痕(err 已有内容时不覆盖 —— 队列 Execute 的失败原因优先)。
    bool EvaluateShell(const char *what, ID3D12Fence *waitFence, uint64_t waitValue,
                       uint64_t *signalValueOut,
                       const std::function<bool(ID3D12GraphicsCommandList *)> &eval,
                       char *err, size_t errLen) noexcept;

    template <typename Fn>
    bool SehCall(Fn &&fn, const char *what, char *err, size_t errLen,
                 void (*log)(const char *) noexcept) noexcept {
        return NgxSehGate(_faulted, std::forward<Fn>(fn), Tag(), what,
                          DisabledNote(), err, errLen, log);
    }

    // ---- 差异点(子类填写)----
    virtual const char *Tag() const noexcept = 0;          // 队列/日志标签("vsr"/"hdr")
    virtual const char *PrettyName() const noexcept = 0;   // 能力日志用名("VSR"/"TrueHDR")
    virtual const char *DisabledNote() const noexcept = 0; // SEH 闩停后的降级说明
    virtual const char *CapKeyAvailable() const noexcept = 0; // <Feature>.Available 键(ngx_defs 宏 = 字符串键面)
    virtual const char *CapKeyInitResult() const noexcept = 0;
    // ctl CL 上的 create EXT 调用(返回 = NGX Success)。
    virtual bool CreateOnCtl(ID3D12GraphicsCommandList *cl) noexcept = 0;
    virtual const char *CreateFailedNote() const noexcept = 0; // CreateFeature 失败日志尾
    virtual const char *EvalFailedNote() const noexcept = 0;   // eval 失败日志尾

    D3D12Context *_d3d12 = nullptr;
    NVSDK_NGX_Parameter *_params = nullptr; // 借用;core 拥有
    NVSDK_NGX_Handle *_feature = nullptr;
    RtxQueue _queue;
    std::atomic<bool> _ready{false};
    std::atomic<bool> _faulted{false};
};

class RtxVsrContext final : public RtxFeatureBase {
public:
    // params 为 NGX core 的 GetCapabilityParameters 块(调用方拥有并负责
    // 销毁,与 DlssfgContext 的 _fgParams 同约定)。内部自取 CtlMutex。
    bool Initialize(D3D12Context &d3d12, NVSDK_NGX_Parameter *params,
                    int width, int height, char *err, size_t errLen) noexcept {
        return InitializeCommon(d3d12, params, width, height, err, errLen);
    }

    // 专用队列上执行一次 VSR eval。waitFence/waitValue = 生产者(NR 段)
    // 的完成栅栏,消费顺序由 GPU 排队保证;*signalValueOut 返回本次完成
    // 点(调用方 CPU 等待 + 消费者队列 Wait)。caller 已持 _evaluateMutex。
    bool Evaluate(ID3D12Resource *input, int inW, int inH,
                  ID3D12Resource *output, int outW, int outH,
                  int quality,
                  ID3D12Fence *waitFence, uint64_t waitValue,
                  uint64_t *signalValueOut,
                  char *err, size_t errLen) noexcept;

protected:
    const char *Tag() const noexcept override { return "vsr"; }
    const char *PrettyName() const noexcept override { return "VSR"; }
    const char *DisabledNote() const noexcept override { return "VSR disabled until host restart"; }
    const char *CapKeyAvailable() const noexcept override { return NVSDK_NGX_Parameter_VSR_Available; }
    const char *CapKeyInitResult() const noexcept override { return NVSDK_NGX_Parameter_VSR_FeatureInitResult; }
    bool CreateOnCtl(ID3D12GraphicsCommandList *cl) noexcept override;
    const char *CreateFailedNote() const noexcept override { return "VSR off (SDR passthrough size)"; }
    const char *EvalFailedNote() const noexcept override { return "VSR off (passthrough size)"; }
};

class RtxHdrContext final : public RtxFeatureBase {
public:
    bool Initialize(D3D12Context &d3d12, NVSDK_NGX_Parameter *params,
                    int width, int height, char *err, size_t errLen) noexcept {
        return InitializeCommon(d3d12, params, width, height, err, errLen);
    }

    // TrueHDR 不缩放:inW/inH 必须等于 outW/outH(官方 eval 的 in/out rect
    // 各自独立,SDK 样例同尺寸使用;本插件恒 1:1)。
    bool Evaluate(ID3D12Resource *input, int w, int h, ID3D12Resource *output,
                  int contrast, int saturation, int middleGray, int maxLuminance,
                  ID3D12Fence *waitFence, uint64_t waitValue,
                  uint64_t *signalValueOut,
                  char *err, size_t errLen) noexcept;

protected:
    const char *Tag() const noexcept override { return "hdr"; }
    const char *PrettyName() const noexcept override { return "TrueHDR"; }
    const char *DisabledNote() const noexcept override { return "HDR disabled until host restart"; }
    const char *CapKeyAvailable() const noexcept override { return NVSDK_NGX_Parameter_TrueHDR_Available; }
    const char *CapKeyInitResult() const noexcept override { return NVSDK_NGX_Parameter_TrueHDR_FeatureInitResult; }
    bool CreateOnCtl(ID3D12GraphicsCommandList *cl) noexcept override;
    const char *CreateFailedNote() const noexcept override { return "HDR off (SDR output)"; }
    const char *EvalFailedNote() const noexcept override { return "HDR off"; }
};

} // namespace vsdlssnr
