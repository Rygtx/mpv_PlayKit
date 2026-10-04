#pragma once
// AMD FidelityFX Optical Flow 会话(Magpie AmdOpticalFlowProvider 的纯 D3D12
// 移植;of_backend=0,默认后端)。跨厂商设计(SM6.2 +
// WaveOps + R16G16_SINT UAV typed store 探测通过即可建)。
//
// 算法:AMD FSR3 SDK 的 ffx_opticalflow(亮度金字塔 + 8x8 块匹配,7 pass
// 预编译 SM6.2 blob,build 时由 Generate-FidelityFXOpticalFlowShaders.ps1
// 用 dxc 生成)。质量档位映射(独立三档):1 = Performance
// (OF extent = 会话/2),2 = Quality(全分辨率)。
//
// 三段 CL 模型(2026-09-25 重构:4 深轮转池,门内 CPU 等 dispatch 已删;
// 2026-10-04 撤 NVOF 同构仪式的 queue Wait —— 三段全在同一队列 FIFO 上,
// 跨段依赖由 FIFO 单独封闭,自家队列自等零收益):
//   copy CL:postCopy 回调(YUV→RGB 转换 + RecordFfxPrepare,由
//     dlssnr_context 的 lambda 记录;本 context 在回调外围做 _ffxInput 的
//     UAV 屏障)→ 提交 Signal(copyFence)。
//   main CL:ffxOpticalflowContextDispatch(三资源 STATE_COMMON 传入,backend
//     内部自管屏障)→ 提交 Signal(doneFence)。
//   densify CL:postExecute 回调(RecordFfxDensify,由 dlssnr_context
//     lambda 记录,含 motion/conf 屏障)→ 提交 Signal(copyFence)。
//
// 顺序模型:三段全部在同一队列 FIFO 上提交,copy(n) 天然后于 dispatch(n-1)
// 完成、densify(n) 天然后于 dispatch(n) 完成,跨段依赖零额外原语;GPU 完成
// 保证迁移到 WaitCopyIdle(每帧槽释放点,upload 堆 CPU 写/GPU 读竞争的
// 排空点),门内只保留轮转池的 Reset 等待(等同 idx 上次使用的栅栏 =
// 4 段之前,常态即刻返回,RtxQueue 同款纪律;in-flight Reset = UB)。
// dispatch 的 GPU 失败发现点从门内等待后移到 WaitCopyIdle 超时 → 会话退役,
// 语义等价。
//
// 与 NVOF 的差异:无独立引擎(NVOF 的 queue Wait 是跨引擎时序、承重,FFX
// 同位置曾只作同构防御,已删);无 ping-pong 输入(FFX context 自持历史
// 金字塔,帧序门保证按帧序喂,乱序/缺口一律 dispatch.reset=true 播种)。
//
// 通道序假设(spike dump 验证点):FFX 契约输入 R8G8B8A8(内部 luma 提取
// 按 RGBA 序),RecordFfxPrepare 做 .zyxw 换序;若 FFX 实际按 BGRA 处理,
// 表现为运动场幅值/方向系统性偏置(非崩溃),由质量 A/B 暴露。
//
// destroy 安全性:ffxOpticalflowContextDestroy 预期安全(纯软件 context,
// 非 NVOF 驱动引擎),首期统一走 DlssnrContext::_retiredOf 名单(与 NVOF
// 同哲学),spike 验证后再放开直毁。

#include "of_backend.h"
#include "of_frame_gate.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <vector>

#include <ffx_opticalflow.h>

namespace vsdlssnr {

class D3D12Context;

class FxofContext final : public IOpticalFlowBackend {
public:
    enum class Mode { Quality, Performance };

    FxofContext() = default;
    ~FxofContext() override;
    FxofContext(const FxofContext &) = delete;
    FxofContext &operator=(const FxofContext &) = delete;

    bool Initialize(D3D12Context &d3d12, int width, int height, int quality,
                    char *err, size_t errLen) noexcept override;
    void Finalize() noexcept override;
    bool Enabled() const noexcept override { return _ready.load(std::memory_order_acquire); }
    int Quality() const noexcept override { return _quality; }
    int Width() const noexcept override { return _width; }
    int Height() const noexcept override { return _height; }
    void ResetHistory() noexcept override;
    void WaitCopyIdle() noexcept override;
    double LastStageMs() const noexcept override { return _lastStageMs; }
    // OF GPU 跨度 = dispatch CL 纯执行(doneFence 达标时读回精确值,未落位
    // 回退提交跨度);FFX 无独立引擎/无冲刷点,copy/densify 胶水 µs 级不计。
    double LastStageTotalMs() const noexcept override;
    OfStageResult StageFrame(int frameIndex, ID3D12Resource *srcTex,
                             const OfPostExecuteFn &postExecute,
                             const OfPostCopyFn &postCopy,
                             bool inputWrittenByPostCopy,
                             std::unique_lock<std::mutex> &gateOut) noexcept override;
    // FlushPendingDensify 不覆写:FFX 的 densify 在 StageFrame 门内即录即
    // 提交,无待冲刷态(默认 no-op 即正确)。
    ID3D12Resource *InputTexture(int index) const noexcept override {
        return index == 0 ? _ffxInput.Get() : nullptr;
    }
    const char *ModeString(char *buf, size_t len) noexcept override;
    int Kind() const noexcept override { return kOfBackendFfx; }
    // dump 探针:FFX 输入(R8G8B8A8 通道序验证)与稀疏流中间场。
    ID3D12Resource *FfxInput() const noexcept { return _ffxInput.Get(); }
    ID3D12Resource *SparseFlow() const noexcept { return _sparseFlow.Get(); }
    uint32_t SparseWidth() const noexcept { return _sparseW; }
    uint32_t SparseHeight() const noexcept { return _sparseH; }
    uint32_t OfWidth() const noexcept { return _ofW; }
    uint32_t OfHeight() const noexcept { return _ofH; }

private:
    void DestroySession() noexcept;
    bool CreateSession(D3D12Context &d3d12, int width, int height, int quality,
                       char *err, size_t errLen) noexcept;
    // CL 池轮转(OfClRotator 共用实体):背压等待/自愈 Reset/簿记内聚。
    // 提交后由调用方 RecordUse 记在飞栅栏值。
    bool AcquireCl(ID3D12CommandAllocator **allocator,
                   ID3D12GraphicsCommandList **cl) noexcept;

    D3D12Context *_d3d12 = nullptr;
    int _width = 0;
    int _height = 0;
    int _quality = 0;
    Mode _mode = Mode::Quality;
    uint32_t _ofW = 0; // OF extent(Quality = 会话,Performance = 会话/2)
    uint32_t _ofH = 0;
    uint32_t _sparseW = 0; // FFX 稀疏流(1/8 OF extent,R16G16_SINT)
    uint32_t _sparseH = 0;

    // FFX 上下文(scratch 归 context 所有;销毁走退役名单,Finalize 只等
    // GPU 静默,不调 ffxOpticalflowContextDestroy —— 见头注释)。
    std::vector<uint8_t> _scratch;
    FfxOpticalflowContext _context{};
    bool _contextCreated = false;

    // 会话纹理(共享资源;SDK 经 ffxOpticalflowGetSharedResourceDescriptions
    // 声明 vector/SCD 的格式与尺寸)。三个资源都带 ALLOW_SIMULTANEOUS_ACCESS:
    // backend 每次 dispatch 都从声明的 COMMON 起录自己的屏障且结束时把
    // 资源留在中间态(NSR/UAV),非 simultaneous-access 纹理不衰变会使
    // 声明契约从第二帧起失真(debug layer [527] 每帧报错);带该旗标后
    // ECL 边界强制衰变回 COMMON,声明恒真,三段 CL 的屏障全部成立。
    Microsoft::WRL::ComPtr<ID3D12Resource> _ffxInput;   // R8G8B8A8 OF extent
    Microsoft::WRL::ComPtr<ID3D12Resource> _sparseFlow; // R16G16_SINT sparse
    Microsoft::WRL::ComPtr<ID3D12Resource> _scd;

    // 持久命令路径:copy/main/densify 三段统一 4 深轮转池(OfClRotator
    // 共用实体;每帧 copy+main 2 段,HDR 链 densify 追加)。门内 CPU 等
    // dispatch 已删(见头注释时序模型)。
    OfClRotator _rotator;
    Microsoft::WRL::ComPtr<ID3D12Fence> _copyFence;  // copy/densify 完成(app 侧)
    Microsoft::WRL::ComPtr<ID3D12Fence> _doneFence;  // FFX dispatch 完成
    HANDLE _copyFenceEvent = nullptr;
    HANDLE _doneFenceEvent = nullptr;
    uint64_t _copySeq = 0;      // copyFence 计数(copy + densify 提交)
    // 尾随完成值(门内写,WaitCopyIdle/LastStageTotalMs 门外读 —— fmParallel
    // 下并发,atomic 对齐惯例;2026-10-04)。
    std::atomic<uint64_t> _lastCopyFence{ 0 };
    uint64_t _doneSeq = 0;      // doneFence 计数(main CL 提交)
    std::atomic<uint64_t> _lastDone{ 0 };

    OfFrameGate _gate;
    uint32_t _consecutiveFailures = 0;
    int _executesLogged = 0; // 前 5 次 dispatch 的诊断日志计数
    // 停摆观测④连续迟到计数(Expired 分支内外共用;StageFrame 由门 mutex
    // 串行,成员化防跨实例残留 —— 原函数级 static,2026-10-05 评审修)。
    int _expStreak = 0;
    std::atomic<bool> _ready{ false };
    double _lastStageMs = 0.0;
    // OF GPU 跨度括号(v25 后端对等):main CL(dispatch,FFX 光流计算的
    // 本体,纯 compute 无 NGX)首尾同 CL 内嵌 EndQuery,量出 dispatch 纯执
    // 行 —— NVOF 段语义(阶段全跨度)的后端对等物。LastStageTotalMs 在
    // doneFence 达标时从 READBACK 读精确值,未落位回退提交跨度。
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> _tsHeap;    // 2 查询
    Microsoft::WRL::ComPtr<ID3D12Resource> _tsReadback; // 16B READBACK
    void *_tsMapped = nullptr;
    mutable double _lastGpuSpanMs = 0.0;
    // 门内写、LastStageTotalMs(const,门外)读写,atomic(2026-10-04)。
    mutable std::atomic<bool> _spanPending{ false }; // 已提交括号待 GPU 落位
    std::atomic<uint64_t> _spanFence{ 0 };   // 本帧 dispatch 的 doneFence 值
};

} // namespace vsdlssnr
