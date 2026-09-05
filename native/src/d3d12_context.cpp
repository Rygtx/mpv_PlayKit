#include "d3d12_context.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dxgidebug.h>
#include <d3d12sdklayers.h>

namespace vsdlssnr {

namespace {

constexpr char DEVICE_VENDOR_NVIDIA = 0x10DE;

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
        if ((desc.VendorId & 0xFF) == DEVICE_VENDOR_NVIDIA) {
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
        WaitForSingleObject(_fenceEvent, INFINITE);
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

} // namespace vsdlssnr
