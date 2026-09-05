#include "d3d12_context.h"
#include "panel_ipc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgidebug.h>

namespace vsdlssnr {

namespace {

constexpr UINT DEVICE_VENDOR_NVIDIA = 0x10DE;

D3D12_RESOURCE_BARRIER Transition(
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

float Saturate(float v) noexcept {
    if (!(v > 0.0f)) return 0.0f;   // also catches NaN
    return v < 1.0f ? v : 1.0f;
}

} // namespace

D3D12Context::~D3D12Context() { Finalize(); }

void D3D12Context::NotifyFrameTick(double qpcSeconds) noexcept {
    // Frame-rate EMA over the last frames (the cadence is decided by the host)
    if (_lastFrameTickSec < 0) {
        _lastFrameTickSec = qpcSeconds;
        return;
    }
    const double delta = qpcSeconds - _lastFrameTickSec;
    _lastFrameTickSec = qpcSeconds;
    if (delta > 1e-4 && delta < 1.0) {
        const double fps = 1.0 / delta;
        _frameRateEma = _frameRateEma > 0 ? _frameRateEma * 0.9 + fps * 0.1 : fps;
    }
}

void D3D12Context::SetErr(char *err, size_t errLen, HRESULT hr, const char *what) const noexcept {
    if (!err || !errLen) return;
    size_t n = static_cast<size_t>(std::snprintf(err, errLen, "%s (hr=0x%08lX)", what, static_cast<unsigned long>(hr)));
    // Append D3D12 debug-layer messages when enabled; this is the only fast
    // way to localize validation failures (mirrors Magpie's VS_D3D12_DEBUG flow).
    if (_infoQueue) {
        _infoQueue->PushEmptyRetrievalFilter();
        UINT64 count = _infoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count && n + 2 < errLen; ++i) {
            SIZE_T size = 0;
            if (FAILED(_infoQueue->GetMessage(i, nullptr, &size)) || !size) continue;
            auto *msg = static_cast<D3D12_MESSAGE *>(std::malloc(size));
            if (!msg) break;
            if (SUCCEEDED(_infoQueue->GetMessage(i, msg, &size))) {
                n += static_cast<size_t>(std::snprintf(err + n, errLen - n, " | [%llu]%s",
                    static_cast<unsigned long long>(msg->ID), msg->pDescription));
            }
            std::free(msg);
        }
        _infoQueue->ClearStoredMessages();
        _infoQueue->PopRetrievalFilter();
    }
}

bool D3D12Context::Initialize(char *err, size_t errLen) noexcept {
    // Optional debug layer (VSDLSSNR_D3D12_DEBUG=1): capture InfoQueue messages
    // so D3D12 validation errors surface in our error strings.
    _debug = GetEnvironmentVariableA("VSDLSSNR_D3D12_DEBUG", nullptr, 0) != 0;
    if (_debug) {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debugController.GetAddressOf())))) {
            debugController->EnableDebugLayer();
            OutputDebugStringA("vs_dlssnr: D3D12 debug layer enabled\n");
        }
        _device = nullptr; // mark pre-device stage
    }

    // Prefer the NVIDIA adapter; fall back to the default one so error reporting
    // stays informative on non-NVIDIA machines (NGX init will fail later anyway).
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) {
        SetErr(err, errLen, E_FAIL, "CreateDXGIFactory1 failed");
        return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIAdapter1> candidate;
    for (UINT i = 0;
         factory->EnumAdapters1(i, candidate.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(candidate->GetDesc1(&desc))) continue;
        // diagnostic: dump every adapter the enumerator sees
        {
            char utf8[160], log[224];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, utf8, sizeof(utf8), nullptr, nullptr);
            snprintf(log, sizeof(log), "adapter[%u]: vendor=0x%04X name=%s\n", i, desc.VendorId, utf8);
            OutputDebugStringA(log);
        }
        if (desc.VendorId == DEVICE_VENDOR_NVIDIA) { // exact match (0x10DE & 0xFF != 0x10DE!)
            adapter = candidate;
            break;
        }
        if (!adapter) adapter = candidate; // remember first adapter as fallback
    }

    HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(_device.GetAddressOf()));
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "D3D12CreateDevice failed");
        return false;
    }
    // remember the adapter the device actually landed on (for GPU name etc.)
    if (adapter) {
        adapter.As(&_adapter);
    }
    if (_debug) {
        _device.As(&_infoQueue);
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = _device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(_queue.GetAddressOf()));
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "CreateCommandQueue failed");
        return false;
    }
    hr = _device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(_allocator.GetAddressOf()));
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "CreateCommandAllocator failed");
        return false;
    }
    hr = _device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, _allocator.Get(), nullptr,
        IID_PPV_ARGS(_commandList.GetAddressOf()));
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "CreateCommandList failed");
        return false;
    }
    hr = _commandList->Close();
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "Close initial command list failed");
        return false;
    }
    hr = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(_fence.GetAddressOf()));
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "CreateFence failed");
        return false;
    }
    _fenceEvent = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!_fenceEvent) {
        SetErr(err, errLen, E_FAIL, "Create fence event failed");
        return false;
    }
    return true;
}

void D3D12Context::Finalize() noexcept {
    if (_queue && _fence) {
        // Best-effort drain so the GPU is idle before releasing command objects.
        _queue->Signal(_fence.Get(), ++_fenceValue);
        if (_fence->GetCompletedValue() < _fenceValue) {
            _fence->SetEventOnCompletion(_fenceValue, _fenceEvent);
            WaitForSingleObject(_fenceEvent, 2000);
        }
    }
    if (_fenceEvent) {
        CloseHandle(_fenceEvent);
        _fenceEvent = nullptr;
    }
    _horizontalRes.Reset();
    _reducedDenoised.Reset();
    _reducedColor.Reset();
    _psoVertical.Reset();
    _psoHorizontal.Reset();
    _psoDownsample.Reset();
    _srvUavHeap.Reset();
    _rsCompute.Reset();
    _scalingReady = false;
    _rtvHeap.Reset();
    _motion.Reset();
    _depth.Reset();
    _upload.Reset();
    _readback.Reset();
    _outputColor.Reset();
    _inputColor.Reset();
    _commandList.Reset();
    _allocator.Reset();
    _queue.Reset();
    _device.Reset();
}

bool D3D12Context::BeginRecording() noexcept {
    HRESULT hr = _allocator->Reset();
    if (FAILED(hr)) return false;
    hr = _commandList->Reset(_allocator.Get(), nullptr);
    return SUCCEEDED(hr);
}

bool D3D12Context::ExecuteAndWait() noexcept {
    HRESULT hr = _commandList->Close();
    if (FAILED(hr)) return false;
    ID3D12CommandList *lists[]{ _commandList.Get() };
    _queue->ExecuteCommandLists(1, lists);
    _queue->Signal(_fence.Get(), ++_fenceValue);
    if (_fence->GetCompletedValue() < _fenceValue) {
        _fence->SetEventOnCompletion(_fenceValue, _fenceEvent);
        // 10s timeout: a device removal would never signal -> don't wait forever
        if (WaitForSingleObject(_fenceEvent, 10000) != WAIT_OBJECT_0) {
            char buf[160];
            HRESULT rr = _device ? _device->GetDeviceRemovedReason() : E_FAIL;
            snprintf(buf, sizeof(buf),
                     "GPU hang/removed: reason=0x%08lX fence=%llu",
                     static_cast<unsigned long>(rr), static_cast<unsigned long long>(_fenceValue));
            // surface through the stats mapping so the panel shows it
            char json[224];
            snprintf(json, sizeof(json),
                     "{\"gpu_hang\":true,\"removed_reason\":\"0x%08lX\"}",
                     static_cast<unsigned long>(rr));
            PublishStatsJson(json);
            OutputDebugStringA("vs_dlssnr: ");
            OutputDebugStringA(buf);
            OutputDebugStringA("\n");
            return false;
        }
    }
    return true;
}

bool D3D12Context::CreateColorTexture(
    ID3D12Resource **out, int width, int height,
    DXGI_FORMAT format, D3D12_RESOURCE_STATES initialState,
    D3D12_RESOURCE_FLAGS flags,
    char *err, size_t errLen) noexcept {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = static_cast<UINT64>(width);
    desc.Height = static_cast<UINT>(height);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    HRESULT hr = _device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
        IID_PPV_ARGS(out));
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "CreateCommittedResource(color) failed");
        return false;
    }
    return true;
}

bool D3D12Context::CreateFrameResources(int width, int height, char *err, size_t errLen) noexcept {
    _width = width;
    _height = height;

    if (!CreateColorTexture(_inputColor.GetAddressOf(), width, height,
                            DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_NONE, err, errLen)) {
        return false;
    }
    // NGX binds the output as a UAV; a D3D12-native resource requires
    // ALLOW_UNORDERED_ACCESS (Magpie's D3D11-shared texture had no such flag
    // constraint, our native one does).
    if (!CreateColorTexture(_outputColor.GetAddressOf(), width, height,
                            DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    // 零 guidance 纹理:R16G16_FLOAT motion + R32_FLOAT depth,内容清 0。
    // RTV clear 要求 ALLOW_RENDER_TARGET 标志。
    if (!CreateColorTexture(_motion.GetAddressOf(), width, height,
                            DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, err, errLen)) {
        return false;
    }
    if (!CreateColorTexture(_depth.GetAddressOf(), width, height,
                            DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, err, errLen)) {
        return false;
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.NumDescriptors = 2;
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HRESULT hr = _device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(_rtvHeap.GetAddressOf()));
        if (FAILED(hr)) {
            SetErr(err, errLen, hr, "CreateDescriptorHeap(RTV) failed");
            return false;
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE base = _rtvHeap->GetCPUDescriptorHandleForHeapStart();
        const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const D3D12_CPU_DESCRIPTOR_HANDLE depthRtv{ base.ptr + static_cast<SIZE_T>(inc) };
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = DXGI_FORMAT_R16G16_FLOAT;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        _device->CreateRenderTargetView(_motion.Get(), &rtv, base);
        rtv.Format = DXGI_FORMAT_R32_FLOAT;
        _device->CreateRenderTargetView(_depth.Get(), &rtv, depthRtv);

        if (!BeginRecording()) {
            SetErr(err, errLen, E_FAIL, "BeginRecording(guidance clear) failed");
            return false;
        }
        D3D12_RESOURCE_BARRIER toRt[2]{
            Transition(_motion.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
            Transition(_depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        _commandList->ResourceBarrier(2, toRt);
        const float zero[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
        _commandList->ClearRenderTargetView(base, zero, 0, nullptr);
        _commandList->ClearRenderTargetView(depthRtv, zero, 0, nullptr);
        D3D12_RESOURCE_BARRIER toCommon[2]{
            Transition(_motion.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON),
            Transition(_depth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON),
        };
        _commandList->ResourceBarrier(2, toCommon);
        if (!ExecuteAndWait()) {
            SetErr(err, errLen, E_FAIL, "Execute(guidance clear) failed");
            return false;
        }
    }

    // Upload staging:RGBA8 行距 256 对齐
    _uploadPitch = (static_cast<size_t>(width) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                   & ~static_cast<size_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = _uploadPitch * static_cast<UINT64>(height);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT hr = _device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(_upload.GetAddressOf()));
        if (FAILED(hr)) {
            SetErr(err, errLen, hr, "CreateCommittedResource(upload) failed");
            return false;
        }
    }
    {
        // READBACK heap only accepts buffers on this runtime (texture staging
        // on READBACK heap returns E_INVALIDARG), so copy via placed footprint
        // into a readback buffer.
        _readbackPitch = _uploadPitch; // same 256-aligned RGBA8 row size
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = _readbackPitch * static_cast<UINT64>(height);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT hr = _device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(_readback.GetAddressOf()));
        if (FAILED(hr)) {
            SetErr(err, errLen, hr, "CreateCommittedResource(readback) failed");
            return false;
        }
    }
    return true;
}

bool D3D12Context::PackInput(
    const uint8_t *const *srcPlanes, const int64_t *srcStrides,
    int width, int height, char *err, size_t errLen) noexcept {
    if (width != _width || height != _height) {
        SetErr(err, errLen, E_INVALIDARG, "PackInput: size mismatch");
        return false;
    }
    void *mapped = nullptr;
    HRESULT hr = _upload->Map(0, nullptr, &mapped);
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "Map(upload) failed");
        return false;
    }
    auto *dstRow = static_cast<uint8_t *>(mapped);
    for (int y = 0; y < height; ++y, dstRow += _uploadPitch) {
        const float *rowR = reinterpret_cast<const float *>(srcPlanes[0] + srcStrides[0] * y);
        const float *rowG = reinterpret_cast<const float *>(srcPlanes[1] + srcStrides[1] * y);
        const float *rowB = reinterpret_cast<const float *>(srcPlanes[2] + srcStrides[2] * y);
        uint32_t *dstPx = reinterpret_cast<uint32_t *>(dstRow);
        for (int x = 0; x < width; ++x) {
            const uint32_t r = static_cast<uint32_t>(Saturate(rowR[x]) * 255.0f + 0.5f);
            const uint32_t g = static_cast<uint32_t>(Saturate(rowG[x]) * 255.0f + 0.5f);
            const uint32_t b = static_cast<uint32_t>(Saturate(rowB[x]) * 255.0f + 0.5f);
            dstPx[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
    _upload->Unmap(0, nullptr);
    return true;
}

bool D3D12Context::RecordUploadCopy(char *err, size_t errLen) noexcept {
    D3D12_RESOURCE_BARRIER toCopyDest[1]{
        Transition(_inputColor.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    _commandList->ResourceBarrier(1, toCopyDest);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = _upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = static_cast<UINT>(_width);
    src.PlacedFootprint.Footprint.Height = static_cast<UINT>(_height);
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(_uploadPitch);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = _inputColor.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    _commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER toCommon1[1]{
        Transition(_inputColor.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
    };
    _commandList->ResourceBarrier(1, toCommon1);
    return true;
}

bool D3D12Context::RecordReadbackCopy(char *err, size_t errLen) noexcept {
    D3D12_RESOURCE_BARRIER toCopySrc[1]{
        Transition(_outputColor.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    _commandList->ResourceBarrier(1, toCopySrc);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = _outputColor.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = _readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = static_cast<UINT>(_width);
    dst.PlacedFootprint.Footprint.Height = static_cast<UINT>(_height);
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(_readbackPitch);
    _commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER backToCommon[1]{
        Transition(_outputColor.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
    };
    _commandList->ResourceBarrier(1, backToCommon);
    return true;
}

bool D3D12Context::UnpackOutput(
    uint8_t **dstPlanes, int64_t *dstStrides,
    int width, int height, char *err, size_t errLen) noexcept {
    if (width != _width || height != _height) {
        SetErr(err, errLen, E_INVALIDARG, "UnpackOutput: size mismatch");
        return false;
    }

    void *mapped = nullptr;
    HRESULT hr = _readback->Map(0, nullptr, &mapped);
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "Map(readback) failed");
        return false;
    }
    const size_t pitch = _readbackPitch;
    const auto *srcRow = static_cast<const uint8_t *>(mapped);
    for (int y = 0; y < height; ++y, srcRow += pitch) {
        const uint8_t *row = srcRow;
        float *rowR = reinterpret_cast<float *>(dstPlanes[0] + dstStrides[0] * y);
        float *rowG = reinterpret_cast<float *>(dstPlanes[1] + dstStrides[1] * y);
        float *rowB = reinterpret_cast<float *>(dstPlanes[2] + dstStrides[2] * y);
        for (int x = 0; x < width; ++x) {
            rowR[x] = row[x * 4 + 0] * (1.0f / 255.0f);
            rowG[x] = row[x * 4 + 1] * (1.0f / 255.0f);
            rowB[x] = row[x * 4 + 2] * (1.0f / 255.0f);
        }
    }
    _readback->Unmap(0, nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// Residual pipeline (ported from Magpie DLSSNRFilter.cpp:100-308):
// downsample color (area average) -> NGX evaluate at internal resolution ->
// Lanczos3 horizontal residual upsample -> Lanczos3 vertical + composite
// (original + residual * ResidualMultiplier).
// ---------------------------------------------------------------------------

namespace {

constexpr char DOWNSAMPLE_HLSL[] = R"(
Texture2D<float4> InputColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
    float2 MotionScale;
    float ResidualMultiplier;
};

[numthreads(8, 8, 1)]
void DownsampleColor(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    float2 sourceStart = float2(tid.xy) * float2(SourceExtent) / float2(TargetExtent);
    float2 sourceEnd = float2(tid.xy + 1) * float2(SourceExtent) / float2(TargetExtent);
    int2 first = int2(floor(sourceStart));
    int2 last = int2(ceil(sourceEnd));
    float4 total = 0.0;
    float totalWeight = 0.0;
    [loop]
    for (int y = first.y; y < last.y; ++y) {
        float weightY = max(0.0, min(sourceEnd.y, float(y + 1)) - max(sourceStart.y, float(y)));
        [loop]
        for (int x = first.x; x < last.x; ++x) {
            float weightX = max(0.0, min(sourceEnd.x, float(x + 1)) - max(sourceStart.x, float(x)));
            float weight = weightX * weightY;
            total += InputColor.Load(int3(clamp(int2(x, y), int2(0, 0), int2(SourceExtent) - 1), 0)) * weight;
            totalWeight += weight;
        }
    }
    OutputColor[tid.xy] = total / max(totalWeight, 1e-6);
}
)";

constexpr char RESIDUAL_HORIZONTAL_HLSL[] = R"(
Texture2D<float4> ReducedColor : register(t0);
Texture2D<float4> ReducedDenoised : register(t1);
RWTexture2D<float4> HorizontalResidual : register(u0);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
    float2 MotionScale;
    float ResidualMultiplier;
};

static const float PI = 3.14159265358979323846;

float Lanczos3(float value) {
    value = abs(value);
    if (value < 1e-5) return 1.0;
    if (value >= 3.0) return 0.0;
    float x = PI * value;
    return (sin(x) / x) * (sin(x / 3.0) / (x / 3.0));
}

[numthreads(8, 8, 1)]
void UpsampleResidualHorizontal(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= SourceExtent.x || tid.y >= TargetExtent.y) return;
    if (SourceExtent.x == TargetExtent.x) {
        HorizontalResidual[tid.xy] = ReducedDenoised.Load(int3(tid.xy, 0)) - ReducedColor.Load(int3(tid.xy, 0));
        return;
    }
    float reducedPosition = (float(tid.x) + 0.5) * float(TargetExtent.x) / float(SourceExtent.x) - 0.5;
    int center = int(floor(reducedPosition));
    float3 residual = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (int x = -2; x <= 3; ++x) {
        float weight = Lanczos3(reducedPosition - float(center + x));
        int sampleX = clamp(center + x, 0, int(TargetExtent.x) - 1);
        int3 samplePixel = int3(sampleX, tid.y, 0);
        residual += (ReducedDenoised.Load(samplePixel).rgb - ReducedColor.Load(samplePixel).rgb) * weight;
        totalWeight += weight;
    }
    residual /= abs(totalWeight) > 1e-6 ? totalWeight : 1.0;
    HorizontalResidual[tid.xy] = float4(residual, 0.0);
}
)";

constexpr char RESIDUAL_VERTICAL_HLSL[] = R"(
Texture2D<float4> OriginalColor : register(t0);
Texture2D<float4> HorizontalResidual : register(t1);
RWTexture2D<float4> OutputColor : register(u0);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
    float2 MotionScale;
    float ResidualMultiplier;
};

static const float PI = 3.14159265358979323846;

float Lanczos3(float value) {
    value = abs(value);
    if (value < 1e-5) return 1.0;
    if (value >= 3.0) return 0.0;
    float x = PI * value;
    return (sin(x) / x) * (sin(x / 3.0) / (x / 3.0));
}

[numthreads(8, 8, 1)]
void CompositeResidualVertical(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= SourceExtent)) return;
    float4 storedOriginal = OriginalColor.Load(int3(tid.xy, 0));
    float3 original = storedOriginal.rgb;
    if (SourceExtent.y == TargetExtent.y) {
        float3 residual = HorizontalResidual.Load(int3(tid.xy, 0)).rgb;
        OutputColor[tid.xy] = float4(saturate(original + residual * ResidualMultiplier), storedOriginal.a);
        return;
    }
    float reducedPosition = (float(tid.y) + 0.5) * float(TargetExtent.y) / float(SourceExtent.y) - 0.5;
    int center = int(floor(reducedPosition));
    float3 residual = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (int y = -2; y <= 3; ++y) {
        float weight = Lanczos3(reducedPosition - float(center + y));
        int sampleY = clamp(center + y, 0, int(TargetExtent.y) - 1);
        residual += HorizontalResidual.Load(int3(tid.x, sampleY, 0)).rgb * weight;
        totalWeight += weight;
    }
    residual /= abs(totalWeight) > 1e-6 ? totalWeight : 1.0;
    OutputColor[tid.xy] = float4(saturate(original + residual * ResidualMultiplier), storedOriginal.a);
}
)";

} // namespace

bool D3D12Context::DumpTextureToFile(ID3D12Resource *tex, int width, int height,
                                     const wchar_t *path) noexcept {
    const UINT pitch = (static_cast<UINT>(width) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                       & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(pitch) * height;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(buffer.GetAddressOf())))) {
        return false;
    }
    if (!BeginRecording()) return false;
    D3D12_RESOURCE_BARRIER b[1]{
        Transition(tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    _commandList->ResourceBarrier(1, b);
    D3D12_TEXTURE_COPY_LOCATION src{ tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 0 };
    D3D12_TEXTURE_COPY_LOCATION dst{ buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = static_cast<UINT>(width);
    dst.PlacedFootprint.Footprint.Height = static_cast<UINT>(height);
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = pitch;
    _commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER back[1]{
        Transition(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
    };
    _commandList->ResourceBarrier(1, back);
    if (!ExecuteAndWait()) return false;

    void *mapped = nullptr;
    if (FAILED(buffer->Map(0, nullptr, &mapped))) return false;
    FILE *f = nullptr;
    bool ok = _wfopen_s(&f, path, L"wb") == 0 && f;
    if (ok) {
        for (int y = 0; y < height; ++y) {
            fwrite(static_cast<const uint8_t *>(mapped) + static_cast<size_t>(pitch) * y, 1,
                   static_cast<size_t>(width) * 4, f);
        }
        fclose(f);
    }
    buffer->Unmap(0, nullptr);
    return ok;
}

bool D3D12Context::CreateComputeObjects(char *err, size_t errLen) noexcept {
    // root signature: b0 = 8 root constants, t0/t1 as independent SRV tables
    // (the three passes need non-adjacent descriptor pairs), (u0) UAV table
    D3D12_DESCRIPTOR_RANGE srvRange0{};
    srvRange0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange0.NumDescriptors = 1;
    srvRange0.BaseShaderRegister = 0;
    srvRange0.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE srvRange1{};
    srvRange1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange1.NumDescriptors = 1;
    srvRange1.BaseShaderRegister = 1;
    srvRange1.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 8;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &srvRange1;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &uavRange;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 4;
    rsDesc.pParameters = params;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> rsBlob, rsErr;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf()))) {
        SetErr(err, errLen, E_FAIL, "SerializeRootSignature failed");
        return false;
    }
    if (FAILED(_device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(_rsCompute.GetAddressOf())))) {
        SetErr(err, errLen, E_FAIL, "CreateRootSignature failed");
        return false;
    }

    // shader-visible descriptor heap (7 descriptors used)
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 16;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(_srvUavHeap.GetAddressOf())))) {
        SetErr(err, errLen, E_FAIL, "CreateDescriptorHeap(SRV/UAV) failed");
        return false;
    }

    struct Cso { const char *hlsl; const char *entry; ID3D12PipelineState **pso; };
    const Cso csos[] = {
        { DOWNSAMPLE_HLSL, "DownsampleColor", _psoDownsample.GetAddressOf() },
        { RESIDUAL_HORIZONTAL_HLSL, "UpsampleResidualHorizontal", _psoHorizontal.GetAddressOf() },
        { RESIDUAL_VERTICAL_HLSL, "CompositeResidualVertical", _psoVertical.GetAddressOf() },
    };
    for (const auto &cso : csos) {
        ComPtr<ID3DBlob> code, csErr;
        if (FAILED(D3DCompile(cso.hlsl, strlen(cso.hlsl), nullptr, nullptr, nullptr,
                              cso.entry, "cs_5_0", 0, 0, code.GetAddressOf(), csErr.GetAddressOf()))) {
            SetErr(err, errLen, E_FAIL, csErr ? static_cast<const char *>(csErr->GetBufferPointer()) : "D3DCompile failed");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = _rsCompute.Get();
        psoDesc.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        if (FAILED(_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(cso.pso)))) {
            SetErr(err, errLen, E_FAIL, "CreateComputePipelineState failed");
            return false;
        }
    }
    return true;
}

bool D3D12Context::CreateScalingResources(int internalW, int internalH, char *err, size_t errLen) noexcept {
    if (!_rsCompute && !CreateComputeObjects(err, errLen)) return false;

    _internalWidth = internalW;
    _internalHeight = internalH;

    // rebuild internal textures (sizes depend on the resolution percent)
    _reducedColor.Reset();
    _reducedDenoised.Reset();
    _horizontalRes.Reset();
    if (!CreateColorTexture(_reducedColor.GetAddressOf(), internalW, internalH,
                            DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    if (!CreateColorTexture(_reducedDenoised.GetAddressOf(), internalW, internalH,
                            DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    if (!CreateColorTexture(_horizontalRes.GetAddressOf(), _width, internalH,
                            DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }

    // (re)write SRV/UAV descriptors: 0=srvInput 1=srvReducedColor 2=srvReducedDenoised
    // 3=srvHorizontal 4=uavReducedColor 5=uavReducedDenoised 6=uavHorizontal 7=uavOutput
    const D3D12_CPU_DESCRIPTOR_HANDLE base = _srvUavHeap->GetCPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto slot = [&](UINT i) { return D3D12_CPU_DESCRIPTOR_HANDLE{ base.ptr + static_cast<SIZE_T>(i * inc) }; };
    _device->CreateShaderResourceView(_inputColor.Get(), nullptr, slot(0));
    _device->CreateShaderResourceView(_reducedColor.Get(), nullptr, slot(1));
    _device->CreateShaderResourceView(_reducedDenoised.Get(), nullptr, slot(2));
    _device->CreateShaderResourceView(_horizontalRes.Get(), nullptr, slot(3));
    _device->CreateUnorderedAccessView(_reducedColor.Get(), nullptr, nullptr, slot(4));
    _device->CreateUnorderedAccessView(_reducedDenoised.Get(), nullptr, nullptr, slot(5));
    _device->CreateUnorderedAccessView(_horizontalRes.Get(), nullptr, nullptr, slot(6));
    _device->CreateUnorderedAccessView(_outputColor.Get(), nullptr, nullptr, slot(7));

    _scalingReady = true;
    return true;
}

void D3D12Context::ClearScalingResources() noexcept {
    _horizontalRes.Reset();
    _reducedDenoised.Reset();
    _reducedColor.Reset();
    // zero the reported internal size too: stats consumers treat 0 as
    // "scaling disabled" instead of a stale percentage of a previous state
    _internalWidth = 0;
    _internalHeight = 0;
    _scalingReady = false;
}

void D3D12Context::RecordDownsample(float residualMultiplier) noexcept {
    _commandList->SetComputeRootSignature(_rsCompute.Get());
    _commandList->SetPipelineState(_psoDownsample.Get());
    ID3D12DescriptorHeap *heaps[]{ _srvUavHeap.Get() };
    _commandList->SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = _srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(i * inc) }; };

    const UINT srcWH[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const UINT dstWH[2]{ static_cast<UINT>(_internalWidth), static_cast<UINT>(_internalHeight) };
    _commandList->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    _commandList->SetComputeRoot32BitConstants(0, 2, dstWH, 2);
    const float zero = 0.0f;
    _commandList->SetComputeRoot32BitConstants(0, 1, &zero, 4);      // Padding0
    const float motion[2]{ 1.0f, 1.0f };
    _commandList->SetComputeRoot32BitConstants(0, 2, motion, 5);
    _commandList->SetComputeRoot32BitConstants(0, 1, &residualMultiplier, 7);

    _commandList->SetComputeRootDescriptorTable(1, gpu(0)); // t0 input (t1 unused)
    _commandList->SetComputeRootDescriptorTable(2, gpu(0)); // t1 dummy
    _commandList->SetComputeRootDescriptorTable(3, gpu(4)); // u0 reducedColor
    _commandList->Dispatch((dstWH[0] + 7) / 8, (dstWH[1] + 7) / 8, 1);
}

void D3D12Context::RecordResidualHorizontal(float residualMultiplier) noexcept {
    // NGX's evaluate may rebind its own descriptor heap / root signature on
    // this command list; rebind ours before touching our descriptors.
    _commandList->SetComputeRootSignature(_rsCompute.Get());
    _commandList->SetPipelineState(_psoHorizontal.Get());
    ID3D12DescriptorHeap *heaps[]{ _srvUavHeap.Get() };
    _commandList->SetDescriptorHeaps(1, heaps);
    const UINT srcWH[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const UINT dstWH[2]{ static_cast<UINT>(_internalWidth), static_cast<UINT>(_internalHeight) };
    _commandList->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    _commandList->SetComputeRoot32BitConstants(0, 2, dstWH, 2);
    const float zero = 0.0f;
    _commandList->SetComputeRoot32BitConstants(0, 1, &zero, 4);
    const float motion[2]{ 1.0f, 1.0f };
    _commandList->SetComputeRoot32BitConstants(0, 2, motion, 5);
    _commandList->SetComputeRoot32BitConstants(0, 1, &residualMultiplier, 7);

    const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = _srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(i * inc) }; };
    _commandList->SetComputeRootDescriptorTable(1, gpu(1)); // t0 reducedColor
    _commandList->SetComputeRootDescriptorTable(2, gpu(2)); // t1 reducedDenoised
    _commandList->SetComputeRootDescriptorTable(3, gpu(6)); // u0 horizontalRes
    _commandList->Dispatch((srcWH[0] + 7) / 8, (dstWH[1] + 7) / 8, 1);
}

void D3D12Context::RecordResidualVertical(float residualMultiplier) noexcept {
    _commandList->SetComputeRootSignature(_rsCompute.Get());
    _commandList->SetPipelineState(_psoVertical.Get());
    ID3D12DescriptorHeap *heaps[]{ _srvUavHeap.Get() };
    _commandList->SetDescriptorHeaps(1, heaps);
    const UINT srcWH[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const UINT dstWH[2]{ static_cast<UINT>(_internalWidth), static_cast<UINT>(_internalHeight) };
    _commandList->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    _commandList->SetComputeRoot32BitConstants(0, 2, dstWH, 2);
    const float zero = 0.0f;
    _commandList->SetComputeRoot32BitConstants(0, 1, &zero, 4);
    const float motion[2]{ 1.0f, 1.0f };
    _commandList->SetComputeRoot32BitConstants(0, 2, motion, 5);
    _commandList->SetComputeRoot32BitConstants(0, 1, &residualMultiplier, 7);

    const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = _srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(i * inc) }; };
    _commandList->SetComputeRootDescriptorTable(1, gpu(0)); // t0 original(input)
    _commandList->SetComputeRootDescriptorTable(2, gpu(3)); // t1 horizontalRes
    _commandList->SetComputeRootDescriptorTable(3, gpu(7)); // u0 outputColor
    _commandList->Dispatch((srcWH[0] + 7) / 8, (srcWH[1] + 7) / 8, 1);
}

} // namespace vsdlssnr
