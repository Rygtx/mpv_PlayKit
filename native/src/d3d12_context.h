#pragma once
// Owns the D3D12 environment for vs_dlssnr. The filter runs under VS
// fmParallel: several frames are in flight at once, each on its own FrameSlot
// (command list + staging + frame textures + descriptor heap). The single
// DIRECT queue serializes slot submissions in submit order, so per-slot
// textures never race on the GPU; CPU-side pack/unpack of concurrent frames
// overlaps the GPU work of other slots, which is what closes the per-frame
// GPU idle gaps of the old single-slot pipeline.
//
// NGX evaluate is additionally serialized by a mutex in DlssnrContext (the
// feature/parameter block is a singleton; on the GPU the queue order already
// serializes the dispatches).
//
// Motion/depth (zero guidance) are read-only and stay resident in
// NON_PIXEL_SHADER_RESOURCE for their whole lifetime — no per-frame state
// transitions, safe for concurrent consumer command lists.

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <cstdint>

namespace vsdlssnr {

using Microsoft::WRL::ComPtr;

// Full-subresource transition barrier — the one barrier builder for the whole
// plugin (frame path, NGX context and diagnostics all used to hand-roll the
// same struct fill).
inline D3D12_RESOURCE_BARRIER Transition(
    ID3D12Resource *resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

// Residual fine-control set (Magpie 0.6.5 r2-fix1/2d37f8c0): applied once per
// internal-resolution pixel in PrepareResidual, before the Catmull-Rom passes.
struct ResidualControls {
    float multiplier = 1.0f;
    float saturation = 1.0f;
    float lightness = 1.0f;
    float shadowStructure = 1.0f;
    float reflectionGlow = 1.0f;
};

// Per-frame-in-flight resources; acquired from the slot pool for the duration
// of one getFrame call.
struct FrameSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    HANDLE fenceEvent = nullptr;      // dedicated event for this slot's fence wait
    uint64_t fenceValue = 0;          // fence value of this slot's last submission

    ComPtr<ID3D12Resource> upload;    // RGBA8 staging, persist-mapped
    void *uploadMapped = nullptr;
    ComPtr<ID3D12Resource> inputColor;   // W×H RGBA8
    ComPtr<ID3D12Resource> outputColor;  // W×H RGBA8, UAV (NGX / composite write)
    ComPtr<ID3D12Resource> readback;     // RGBA8 readback buffer, persist-mapped
    void *readbackMapped = nullptr;
    // residual scaling pipeline, sized by the current input_resolution
    ComPtr<ID3D12Resource> reducedColor;
    ComPtr<ID3D12Resource> reducedDenoised;
    ComPtr<ID3D12Resource> controlledRes; // internalW×internalH, signed FP16 (controls applied)
    ComPtr<ID3D12Resource> horizontalRes;
    // slot-local shader-visible heap: 0=srvInput 1=srvReducedColor
    // 2=srvReducedDenoised 3=srvHorizontal 4=uavReducedColor 5=uavReducedDenoised
    // 6=uavHorizontal 7=uavOutput 8=srvControlled 9=uavControlled
    ComPtr<ID3D12DescriptorHeap> srvUavHeap;

    size_t uploadPitch = 0;
    size_t readbackPitch = 0;
};

class D3D12Context {
public:
    D3D12Context() = default;
    ~D3D12Context();
    D3D12Context(const D3D12Context &) = delete;
    D3D12Context &operator=(const D3D12Context &) = delete;

    bool Initialize(char *err, size_t errLen) noexcept;
    void Finalize() noexcept;

    ID3D12Device *Device() const noexcept { return _device.Get(); }
    IDXGIAdapter1 *Adapter() const noexcept { return _adapter.Get(); }
    ID3D12CommandQueue *Queue() const noexcept { return _queue.Get(); }

    // Control-path recording (NGX CreateFeature / diagnostics dump): guarded
    // by CtlMutex, never overlaps a slot's frame work on this list.
    std::mutex &CtlMutex() noexcept { return _ctlMutex; }
    bool BeginCtlRecording() noexcept;
    ID3D12GraphicsCommandList *CtlCommandList() const noexcept { return _ctlCommandList.Get(); }
    bool ExecuteCtlAndWait() noexcept;
    // True once a fence wait timed out (GPU hang / device removal): callers
    // should stop evaluating instead of stalling the full wait every frame.
    bool IsDeviceLost() const noexcept { return _deviceLost.load(std::memory_order_relaxed); }

    bool CreateFrameResources(int width, int height, char *err, size_t errLen) noexcept;

    // diagnostics: dump a texture's raw rows to a file (VSDLSSNR_DUMP);
    // format must match the resource (CopyTextureRegion has no cross-family
    // conversion). Uses the control path; caller holds CtlMutex.
    bool DumpTextureToFile(ID3D12Resource *tex, int width, int height,
                           const wchar_t *path,
                           DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) noexcept;

    // Residual scaling rebuilds. The caller must hold a PoolHold (all slots
    // idle, pool sealed) — the rebuild replaces every slot's scaling
    // textures; Initialize may call these directly (single-threaded).
    bool RebuildScaling(int internalW, int internalH, char *err, size_t errLen) noexcept;
    // Drop the residual pipeline entirely (scaling disabled).
    void ClearScalingResources() noexcept;
    bool HasScaling() const noexcept { return _scalingReady; }
    int InternalWidth() const noexcept { return _internalWidth; }
    int InternalHeight() const noexcept { return _internalHeight; }

    // Frame slot pool. Acquire blocks while kSlotCount frames are in flight;
    // the caller must ReleaseSlot exactly once on every path.
    FrameSlot *AcquireSlot() noexcept;
    void ReleaseSlot(FrameSlot *slot) noexcept;

    // RAII: drain the pool (wait until every slot is released) and hold the
    // pool mutex, so AcquireSlot cannot hand out a slot while the holder
    // replaces shared NGX state / per-slot textures. Never call this while
    // holding a slot yourself.
    class PoolHold {
    public:
        explicit PoolHold(D3D12Context &ctx) noexcept;
        ~PoolHold() noexcept;
        PoolHold(const PoolHold &) = delete;
        PoolHold &operator=(const PoolHold &) = delete;
    private:
        D3D12Context *_ctx;
        std::unique_lock<std::mutex> _lock;
        friend class D3D12Context;
    };

    // Per-slot frame path.
    bool PackInput(FrameSlot &slot, const uint8_t *const *srcPlanes, const int64_t *srcStrides,
                   int width, int height, char *err, size_t errLen) noexcept;
    bool BeginFrameRecording(FrameSlot &slot) noexcept;
    // 记录:input COMMON→COPY_DEST→拷贝→stateAfter
    bool RecordUploadCopy(FrameSlot &slot, D3D12_RESOURCE_STATES stateAfter, char *err, size_t errLen) noexcept;
    // 记录:output stateBefore→COPY_SOURCE→拷贝→COMMON
    bool RecordReadbackCopy(FrameSlot &slot, D3D12_RESOURCE_STATES stateBefore, char *err, size_t errLen) noexcept;
    bool SubmitFrame(FrameSlot &slot, char *err, size_t errLen) noexcept; // close+execute+signal
    bool WaitFrame(FrameSlot &slot, char *err, size_t errLen) noexcept;   // fence wait, device-lost aware
    // GPU 完成后调用:readback buffer → RGBS 三平面(纯 CPU)
    bool UnpackOutput(FrameSlot &slot, uint8_t **dstPlanes, int64_t *dstStrides,
                      int width, int height, char *err, size_t errLen) noexcept;

    // 五段残差 compute 路径的命令记录(在该槽已 BeginFrameRecording 的列表上)。
    // 降采样两段可分离(Lanczos2),horizontalRes 复用为降采样 FP16 中间纹理
    // (与上游 resampleIntermediate 的复用方式一致)。
    void RecordDownsampleVertical(FrameSlot &slot, const ResidualControls &rc) noexcept;
    void RecordDownsampleHorizontal(FrameSlot &slot, const ResidualControls &rc) noexcept;
    void RecordResidualPrepare(FrameSlot &slot, const ResidualControls &rc) noexcept;
    void RecordResidualHorizontal(FrameSlot &slot, const ResidualControls &rc) noexcept;
    // equalWidth: 内部宽度与源一致时跳过 horizontal pass,垂直 pass 直读
    // controlledRes(Magpie 的 verticalResidual 绑定切换)。
    void RecordResidualVertical(FrameSlot &slot, const ResidualControls &rc,
                                bool equalWidth) noexcept;

    ID3D12Resource *InputColor(FrameSlot &s) const noexcept { return s.inputColor.Get(); }
    ID3D12Resource *OutputColor(FrameSlot &s) const noexcept { return s.outputColor.Get(); }
    ID3D12Resource *ReducedColor(FrameSlot &s) const noexcept { return s.reducedColor.Get(); }
    ID3D12Resource *ReducedDenoised(FrameSlot &s) const noexcept { return s.reducedDenoised.Get(); }
    ID3D12Resource *ControlledRes(FrameSlot &s) const noexcept { return s.controlledRes.Get(); }
    ID3D12Resource *HorizontalRes(FrameSlot &s) const noexcept { return s.horizontalRes.Get(); }
    double FrameRateEma() noexcept;
    void NotifyFrameTick(double qpcSeconds) noexcept; // frame-rate EMA (播放节奏由宿主决定)
    // 零 guidance(Force Zero,等价 Magpie guidanceMode=1):
    // motion R16G16_FLOAT、depth R32_FLOAT,内容全 0,常驻 NSR 只读
    ID3D12Resource *Motion() const noexcept { return _motion.Get(); }
    ID3D12Resource *Depth() const noexcept { return _depth.Get(); }

private:
    bool CreateSlotResources(FrameSlot &slot, char *err, size_t errLen) noexcept;
    bool CreateScalingForSlot(FrameSlot &slot, int iw, int ih, char *err, size_t errLen) noexcept;
    void ClearScalingForSlot(FrameSlot &slot) noexcept;
    bool WaitFenceValue(uint64_t value, HANDLE event, char *err, size_t errLen) noexcept;
    // Shared body of the three raw buffer creations (upload / readback /
    // diagnostics dump): heap type, initial state and the 256-aligned pitch
    // are the only differences between them.
    bool CreateRawBuffer(UINT64 bytes, D3D12_HEAP_TYPE heapType,
                         D3D12_RESOURCE_STATES initialState,
                         ID3D12Resource **out, size_t &alignedPitch,
                         UINT bytesPerRow, char *err, size_t errLen) noexcept;

    // Shared prologue of the four residual passes; only the PSO, descriptor
    // slots and dispatch dims differ. cbuffer layout (root constants):
    // SourceExtent@0, TargetExtent@2, Padding0@4, MotionScale@5,
    // ResidualMultiplier@7, ResidualSaturation@8, ResidualLightness@9,
    // ShadowStructureMultiplier@10, ReflectionGlowMultiplier@11 — mirrors the
    // four HLSL cbuffer blocks (Magpie ResampleConstants, 48 bytes).
    void RecordPass(FrameSlot &slot, ID3D12PipelineState *pso, UINT srv0, UINT srv1, UINT uav,
                    UINT dispatchX, UINT dispatchY, const ResidualControls &rc) noexcept;

    bool CreateColorTexture(ID3D12Resource **out, int width, int height,
                            DXGI_FORMAT format, D3D12_RESOURCE_STATES initialState,
                            D3D12_RESOURCE_FLAGS flags,
                            char *err, size_t errLen) noexcept;
    bool CreateComputeObjects(char *err, size_t errLen) noexcept;
    void SetErr(char *err, size_t errLen, HRESULT hr, const char *what) const noexcept;

    ComPtr<ID3D12Device> _device;
    ComPtr<IDXGIAdapter1> _adapter; // the adapter the device was created on
    ComPtr<ID3D12InfoQueue> _infoQueue;
    bool _debug = false;
    ComPtr<ID3D12CommandQueue> _queue;
    ComPtr<ID3D12Fence> _fence;
    std::atomic<uint64_t> _fenceValue{0};
    std::atomic<bool> _deviceLost{false};
    // serializes fetch_add + Signal so the fence value order matches the
    // queue's ExecuteCommandLists order (a fence value must never regress)
    std::mutex _submitMutex;

    // control path: CreateFeature / diagnostics (never concurrent with itself)
    std::mutex _ctlMutex;
    ComPtr<ID3D12CommandAllocator> _ctlAllocator;
    ComPtr<ID3D12GraphicsCommandList> _ctlCommandList;
    HANDLE _ctlEvent = nullptr;

    int _width = 0;
    int _height = 0;

    // shared read-only resources
    ComPtr<ID3D12Resource> _motion; // resident NON_PIXEL_SHADER_RESOURCE
    ComPtr<ID3D12Resource> _depth;  // resident NON_PIXEL_SHADER_RESOURCE
    ComPtr<ID3D12DescriptorHeap> _rtvHeap;

    // residual compute objects (shared: PSOs are stateless)
    ComPtr<ID3D12RootSignature> _rsCompute;
    ComPtr<ID3D12PipelineState> _psoDownsampleVertical;
    ComPtr<ID3D12PipelineState> _psoDownsampleHorizontal;
    ComPtr<ID3D12PipelineState> _psoPrepare;
    ComPtr<ID3D12PipelineState> _psoHorizontal;
    ComPtr<ID3D12PipelineState> _psoVertical;

    // frame slot pool
    static constexpr int kSlotCount = 3;
    FrameSlot _slots[kSlotCount];
    int _freeStack[kSlotCount];
    int _freeCount = 0;
    std::mutex _poolMutex;
    std::condition_variable _poolCv;

    int _internalWidth = 0;
    int _internalHeight = 0;
    bool _scalingReady = false;
    std::mutex _tickMutex;
    double _lastFrameTickSec = -1.0;
    double _frameRateEma = 0.0;
};

} // namespace vsdlssnr
