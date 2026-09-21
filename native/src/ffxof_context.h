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
// 三段 CL 模型(自有持久 CL×2 + allocator×2 + 栅栏对;FFX dispatch 只把
// pass 命令录进我们的 CL,提交节奏完全由我们控制):
//   copy CL(alloc A):queue Wait(doneFence) → CPU 等 copyFence →
//     postCopy 回调(YUV→RGB 转换 + RecordFfxPrepare,由 dlssnr_context 的
//     lambda 记录;本 context 在回调外围做 _ffxInput 的 UAV 屏障)→ 提交。
//   main CL(alloc B):ffxOpticalflowContextDispatch(三资源 STATE_COMMON
//     传入,backend 内部自管屏障)→ 提交 Signal(doneFence)。
//   CPU 等 doneFence(main CL 完成;allocator 复用安全)→
//   densify CL(alloc A 复用):postExecute 回调(RecordFfxDensify,由
//     dlssnr_context lambda 记录,含 motion/conf 屏障)→ 提交。
//
// 与 NVOF 的差异:无独立引擎(全部在自家队列 FIFO 上,queue Wait 仅作与
// NvofContext 同构的防御);无 ping-pong 输入(FFX context 自持历史金字塔,
// 帧序门保证按帧序喂,乱序/缺口一律 dispatch.reset=true 播种)。
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
    OfStageResult StageFrame(int frameIndex, ID3D12Resource *srcTex,
                             const OfPostExecuteFn &postExecute,
                             const OfPostCopyFn &postCopy,
                             bool inputWrittenByPostCopy) noexcept override;
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
    static bool WaitForFenceReached(ID3D12Fence *fence, uint64_t value,
                                    HANDLE event, DWORD timeoutMs) noexcept;

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

    // 持久命令路径(alloc A = copy + densify 复用,alloc B = FFX dispatch)。
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> _allocatorA;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> _allocatorB;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> _commandListA;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> _commandListB;
    Microsoft::WRL::ComPtr<ID3D12Fence> _copyFence;  // copy/densify 完成(app 侧)
    Microsoft::WRL::ComPtr<ID3D12Fence> _doneFence;  // FFX dispatch 完成
    HANDLE _copyFenceEvent = nullptr;
    HANDLE _doneFenceEvent = nullptr;
    uint64_t _copySeq = 0;
    uint64_t _lastCopyFence = 0;
    uint64_t _doneSeq = 0;
    uint64_t _lastDone = 0;

    OfFrameGate _gate;
    uint32_t _consecutiveFailures = 0;
    int _executesLogged = 0; // 前 5 次 dispatch 的诊断日志计数
    std::atomic<bool> _ready{ false };
    double _lastStageMs = 0.0;
};

} // namespace vsdlssnr
