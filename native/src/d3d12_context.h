#pragma once
// Owns the D3D12 environment for vs_dlssnr. Unlike Magpie (D3D11 renderer with
// D3D12 interop via NT handles) the VapourSynth plugin feeds CPU frames, so a
// single self-contained D3D12 device + DIRECT queue is enough: CPU pack/unpack,
// staging upload, readback heap, zero-guidance textures, no shared fences.

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdint>

namespace vsdlssnr {

using Microsoft::WRL::ComPtr;

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
    ID3D12GraphicsCommandList *CommandList() const noexcept { return _commandList.Get(); }
    // 用于 CreateFeature / EvaluateFeature 的命令提交(Magpie 在 open 的
    // command list 上调用,随后 Close + Execute,见 DLSSNRFilter.cpp:1727-1758)
    bool BeginRecording() noexcept;
    bool ExecuteAndWait() noexcept;
    // True once a fence wait timed out (GPU hang / device removal): callers
    // should stop evaluating instead of stalling the full wait every frame.
    bool IsDeviceLost() const noexcept { return _deviceLost; }

    bool CreateFrameResources(int width, int height, char *err, size_t errLen) noexcept;

    // diagnostics: dump a texture's raw rows to a file (VSDLSSNR_DUMP);
    // format must match the resource (CopyTextureRegion has no cross-family
    // conversion), the caller passes bytes-per-pixel for the file layout
    bool DumpTextureToFile(ID3D12Resource *tex, int width, int height,
                           const wchar_t *path,
                           DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) noexcept;

    // Residual pipeline resources for internal-resolution scaling
    // (Magpie DLSSNRFilter.cpp:100-308). internalW/H = source * percent/100.
    bool CreateScalingResources(int internalW, int internalH, char *err, size_t errLen) noexcept;
    // Drop the residual pipeline entirely (scaling disabled)
    void ClearScalingResources() noexcept;
    bool HasScaling() const noexcept { return _scalingReady; }

    // 三条残差 compute 路径的命令记录(在已 BeginRecording 的列表上)
    void RecordDownsample(float residualMultiplier) noexcept;
    void RecordResidualHorizontal(float residualMultiplier) noexcept;
    void RecordResidualVertical(float residualMultiplier) noexcept;

    ID3D12Resource *InputColor() const noexcept { return _inputColor.Get(); }
    ID3D12Resource *OutputColor() const noexcept { return _outputColor.Get(); }
    ID3D12Resource *ReducedColor() const noexcept { return _reducedColor.Get(); }
    ID3D12Resource *ReducedDenoised() const noexcept { return _reducedDenoised.Get(); }
    ID3D12Resource *HorizontalRes() const noexcept { return _horizontalRes.Get(); }
    int InternalWidth() const noexcept { return _internalWidth; }
    int InternalHeight() const noexcept { return _internalHeight; }
    double FrameRateEma() const noexcept { return _frameRateEma; }
    void NotifyFrameTick(double qpcSeconds) noexcept; // frame-rate EMA (播放节奏由宿主决定)
    // 零 guidance(Force Zero,等价 Magpie guidanceMode=1):
    // motion R16G16_FLOAT、depth R32_FLOAT,内容全 0
    ID3D12Resource *Motion() const noexcept { return _motion.Get(); }
    ID3D12Resource *Depth() const noexcept { return _depth.Get(); }

    // 单次提交管线(三段同步已合并):CPU pack → [一次提交: 上传拷贝 → evaluate → 回读拷贝] → CPU unpack。
    // 上传/回读拷贝的 barrier 直达下一消费者状态(帧末仍统一归位 COMMON);
    // skip-eval 诊断路径传 COMMON。RGBS float32 三平面 → RGBA8 行写入 upload buffer(纯 CPU,不提交)
    bool PackInput(const uint8_t *const *srcPlanes, const int64_t *srcStrides,
                   int width, int height, char *err, size_t errLen) noexcept;
    // 在已 BeginRecording 的命令列表上记录:input COMMON→COPY_DEST→拷贝→stateAfter
    bool RecordUploadCopy(D3D12_RESOURCE_STATES stateAfter, char *err, size_t errLen) noexcept;
    // 在已 BeginRecording 的命令列表上记录:output stateBefore→COPY_SOURCE→拷贝→COMMON
    bool RecordReadbackCopy(D3D12_RESOURCE_STATES stateBefore, char *err, size_t errLen) noexcept;
    // GPU 完成后调用:readback buffer → RGBS 三平面(纯 CPU)
    bool UnpackOutput(uint8_t **dstPlanes, int64_t *dstStrides,
                      int width, int height, char *err, size_t errLen) noexcept;

private:
    // Shared prologue of the three residual passes; only the PSO, descriptor
    // slots and dispatch dims differ. cbuffer layout (root constants):
    // SourceExtent@0, TargetExtent@2, Padding0@4, MotionScale@5,
    // ResidualMultiplier@7 — mirrors the three HLSL cbuffer blocks.
    void RecordPass(ID3D12PipelineState *pso, UINT srv0, UINT srv1, UINT uav,
                    UINT dispatchX, UINT dispatchY, float residualMultiplier) noexcept;

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
    ComPtr<ID3D12CommandAllocator> _allocator;
    ComPtr<ID3D12GraphicsCommandList> _commandList;
    ComPtr<ID3D12Fence> _fence;
    HANDLE _fenceEvent = nullptr;
    uint64_t _fenceValue = 0;
    bool _deviceLost = false;

    ComPtr<ID3D12Resource> _inputColor;
    ComPtr<ID3D12Resource> _outputColor;
    ComPtr<ID3D12Resource> _readback;
    ComPtr<ID3D12Resource> _upload;
    void *_uploadMapped = nullptr; // persist-mapped upload heap (map once, unmap with the resource)
    ComPtr<ID3D12Resource> _motion;
    ComPtr<ID3D12Resource> _depth;
    ComPtr<ID3D12DescriptorHeap> _rtvHeap;

    int _width = 0;
    int _height = 0;
    size_t _uploadPitch = 0;
    size_t _readbackPitch = 0;

    // residual scaling pipeline
    ComPtr<ID3D12RootSignature> _rsCompute;
    ComPtr<ID3D12PipelineState> _psoDownsample;
    ComPtr<ID3D12PipelineState> _psoHorizontal;
    ComPtr<ID3D12PipelineState> _psoVertical;
    ComPtr<ID3D12DescriptorHeap> _srvUavHeap;
    ComPtr<ID3D12Resource> _reducedColor;
    ComPtr<ID3D12Resource> _reducedDenoised;
    ComPtr<ID3D12Resource> _horizontalRes;
    int _internalWidth = 0;
    int _internalHeight = 0;
    bool _scalingReady = false;
    double _lastFrameTickSec = -1.0;
    double _frameRateEma = 0.0;
};

} // namespace vsdlssnr
