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

float Saturate(float v) noexcept {
    if (!(v > 0.0f)) return 0.0f;   // also catches NaN
    return v < 1.0f ? v : 1.0f;
}

// 临时探针(定位 DEVICE_HUNG 时序用,验证后删除)
void DbgProbe(const char *what) noexcept {
    fprintf(stderr, "[probe %llu] %s\n",
            static_cast<unsigned long long>(GetTickCount64()), what);
    fflush(stderr);
}

} // namespace

D3D12Context::~D3D12Context() { Finalize(); }

void D3D12Context::NotifyFrameTick(double qpcSeconds) noexcept {
    // Frame-rate EMA over the last frames (the cadence is decided by the host)
    std::lock_guard<std::mutex> lock(_tickMutex);
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

double D3D12Context::FrameRateEma() noexcept {
    std::lock_guard<std::mutex> lock(_tickMutex);
    return _frameRateEma;
}

void D3D12Context::SetErr(char *err, size_t errLen, HRESULT hr, const char *what) const noexcept {
    if (!err || !errLen) return;
    size_t n = static_cast<size_t>(std::snprintf(err, errLen, "%s (hr=0x%08lX)", what, static_cast<unsigned long>(hr)));
    // Device removal: surface the driver's removal reason immediately — the
    // failing call is usually just where the async TDR was discovered.
    if (hr == static_cast<HRESULT>(0x887A0005u) || hr == static_cast<HRESULT>(0x887A0006u)) {
        HRESULT rr = _device ? _device->GetDeviceRemovedReason() : E_FAIL;
        if (n + 64 < errLen) {
            n += static_cast<size_t>(std::snprintf(err + n, errLen - n, " removed_reason=0x%08lX",
                                                   static_cast<unsigned long>(rr)));
        }
    }
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
            // Zero-init + explicit failure check: a failed/too-long
            // conversion leaves the buffer without a terminator, and the
            // %s below would read past it into uninitialized stack.
            char utf8[160] = {}, log[224];
            if (WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                    utf8, sizeof(utf8), nullptr, nullptr) <= 0) {
                utf8[0] = '\0';
            }
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
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(_ctlAllocator.GetAddressOf()));
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "CreateCommandAllocator(ctl) failed");
        return false;
    }
    hr = _device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, _ctlAllocator.Get(), nullptr,
        IID_PPV_ARGS(_ctlCommandList.GetAddressOf()));
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "CreateCommandList(ctl) failed");
        return false;
    }
    hr = _ctlCommandList->Close();
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "Close initial ctl command list failed");
        return false;
    }
    hr = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(_fence.GetAddressOf()));
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "CreateFence failed");
        return false;
    }
    _ctlEvent = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!_ctlEvent) {
        SetErr(err, errLen, E_FAIL, "Create fence event failed");
        return false;
    }
    return true;
}

void D3D12Context::Finalize() noexcept {
    if (_queue && _fence) {
        // Best-effort drain so the GPU is idle before releasing command objects.
        const uint64_t v = _fenceValue.fetch_add(1) + 1;
        _queue->Signal(_fence.Get(), v);
        if (_fence->GetCompletedValue() < v) {
            _fence->SetEventOnCompletion(v, _ctlEvent);
            WaitForSingleObject(_ctlEvent, 2000);
        }
    }
    // Slots first: their command lists reference the shared PSOs/heaps.
    for (int i = 0; i < kSlotCount; ++i) {
        FrameSlot &s = _slots[i];
        if (s.fenceEvent) {
            CloseHandle(s.fenceEvent);
            s.fenceEvent = nullptr;
        }
        s.uploadMapped = nullptr;
        s.readbackMapped = nullptr;
        s.upload.Reset();
        s.readback.Reset();
        s.inputColor.Reset();
        s.outputColor.Reset();
        s.reducedColor.Reset();
        s.reducedDenoised.Reset();
        s.horizontalRes.Reset();
        s.motion.Reset();
        s.confidence.Reset();
        s.reducedMotion.Reset();
        s.reducedConfidence.Reset();
        s.srvUavHeap.Reset();
        s.commandList.Reset();
        s.allocator.Reset();
    }
    _freeCount = 0;
    _psoVertical.Reset();
    _psoHorizontal.Reset();
    _psoPrepare.Reset();
    _psoDownsampleVertical.Reset();
    _psoDownsampleHorizontal.Reset();
    _psoDensify.Reset();
    _psoGuidanceDownsample.Reset();
    _rsCompute.Reset();
    _rsDensify.Reset();
    _rsGuidance.Reset();
    _scalingReady = false;
    _rtvHeap.Reset();
    _motion.Reset();
    _depth.Reset();
    if (_ctlEvent) {
        CloseHandle(_ctlEvent);
        _ctlEvent = nullptr;
    }
    _ctlCommandList.Reset();
    _ctlAllocator.Reset();
    _queue.Reset();
    _device.Reset();
}

bool D3D12Context::BeginCtlRecording() noexcept {
    HRESULT hr = _ctlAllocator->Reset();
    if (FAILED(hr)) {
        // A previous error return with the list still open makes Reset fail
        // with E_FAIL from then on; force-close once and retry so a single
        // SEH doesn't brick the control path for the rest of the session.
        _ctlCommandList->Close();
        hr = _ctlAllocator->Reset();
        if (FAILED(hr)) return false;
    }
    hr = _ctlCommandList->Reset(_ctlAllocator.Get(), nullptr);
    return SUCCEEDED(hr);
}

bool D3D12Context::ExecuteCtlAndWait() noexcept {
    HRESULT hr = _ctlCommandList->Close();
    if (FAILED(hr)) return false;
    // 与 SubmitFrame 共享 _fence:fetch_add + Signal 必须同锁,否则并发
    // 槽提交会让 Signal 值乱序(fence 值回退 = 驱动未定义行为)。CtlMutex
    // → _submitMutex 的加锁顺序与 SubmitFrame(仅 _submitMutex)无环。
    std::lock_guard<std::mutex> lock(_submitMutex);
    ID3D12CommandList *lists[]{ _ctlCommandList.Get() };
    _queue->ExecuteCommandLists(1, lists);
    const uint64_t v = _fenceValue.fetch_add(1) + 1;
    _queue->Signal(_fence.Get(), v);
    char ignore[96];
    return WaitFenceValue(v, _ctlEvent, ignore, sizeof(ignore));
}

bool D3D12Context::WaitFenceValue(uint64_t value, HANDLE event, char *err, size_t errLen) noexcept {
    if (_fence->GetCompletedValue() < value) {
        if (FAILED(_fence->SetEventOnCompletion(value, event))) {
            SetErr(err, errLen, E_FAIL, "SetEventOnCompletion failed");
            return false;
        }
        // 10s timeout: a device removal would never signal -> don't wait forever
        if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) {
            HRESULT rr = _device ? _device->GetDeviceRemovedReason() : E_FAIL;
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "GPU hang/removed: reason=0x%08lX fence=%llu",
                     static_cast<unsigned long>(rr), static_cast<unsigned long long>(value));
            // surface through the stats mapping so the panel shows it
            // (gpu_hang numeric + the removal reason — keys from panel_ipc.h)
            char json[224];
            snprintf(json, sizeof(json),
                     "{\"%s\":1,\"%s\":\"0x%08lX\"}",
                     SK_GPU_HANG, SK_REMOVED_REASON,
                     static_cast<unsigned long>(rr));
            PublishStatsJson(json);
            _deviceLost.store(true, std::memory_order_relaxed);
            OutputDebugStringA("vs_dlssnr: ");
            OutputDebugStringA(buf);
            OutputDebugStringA("\n");
            SetErr(err, errLen, E_FAIL, buf);
            return false;
        }
    }
    return true;
}

// --- Slot pool ------------------------------------------------------------

FrameSlot *D3D12Context::AcquireSlot() noexcept {
    std::unique_lock<std::mutex> lock(_poolMutex);
    _poolCv.wait(lock, [this] { return _freeCount > 0; });
    FrameSlot *slot = &_slots[_freeStack[--_freeCount]];
    return slot;
}

void D3D12Context::ReleaseSlot(FrameSlot *slot) noexcept {
    {
        std::lock_guard<std::mutex> lock(_poolMutex);
        _freeStack[_freeCount++] = static_cast<int>(slot - _slots);
    }
    _poolCv.notify_all();
}

// --- PoolHold: drain + seal ------------------------------------------------

D3D12Context::PoolHold::PoolHold(D3D12Context &ctx) noexcept : _ctx(&ctx) {
    // Take the pool mutex and only release it when the hold ends: while held,
    // AcquireSlot blocks on this very mutex, so no new frame can start against
    // the resources the holder is about to replace. Waiting for the drain
    // inside the same lock is safe — the frames still in flight never take
    // _poolMutex again (they already own their slot), they just release.
    _lock = std::unique_lock<std::mutex>(_ctx->_poolMutex);
    _ctx->_poolCv.wait(_lock, [&ctx] { return ctx._freeCount == kSlotCount; });
}

D3D12Context::PoolHold::~PoolHold() noexcept {
    if (_lock.owns_lock()) {
        _lock.unlock();
        _ctx->_poolCv.notify_all();
    }
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

    // 零 guidance 纹理:R16G16_FLOAT motion + R32_FLOAT depth,内容清 0。
    // RTV clear 要求 ALLOW_RENDER_TARGET 标志。clear 后常驻
    // NON_PIXEL_SHADER_RESOURCE(evaluate 只读,无每帧状态转换,
    // 并发帧的命令列表可以同时引用)。
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

    DbgProbe("frame-res: before clear");
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

        std::lock_guard<std::mutex> ctlLock(_ctlMutex);
        if (!BeginCtlRecording()) {
            SetErr(err, errLen, E_FAIL, "BeginCtlRecording(guidance clear) failed");
            return false;
        }
        D3D12_RESOURCE_BARRIER toRt[2]{
            Transition(_motion.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
            Transition(_depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        _ctlCommandList->ResourceBarrier(2, toRt);
        const float zero[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
        _ctlCommandList->ClearRenderTargetView(base, zero, 0, nullptr);
        _ctlCommandList->ClearRenderTargetView(depthRtv, zero, 0, nullptr);
        D3D12_RESOURCE_BARRIER toResident[2]{
            Transition(_motion.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(_depth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        _ctlCommandList->ResourceBarrier(2, toResident);
        if (!ExecuteCtlAndWait()) {
            SetErr(err, errLen, E_FAIL, "Execute(guidance clear) failed");
            return false;
        }
    }

    DbgProbe("frame-res: before slot loop");
    for (int i = 0; i < kSlotCount; ++i) {
        DbgProbe("slot loop: begin");
        if (!CreateSlotResources(_slots[i], err, errLen)) return false;
        DbgProbe("slot loop: end");
    }
    DbgProbe("frame-res: after slot loop");
    // Seed the free stack (LIFO) — the pool starts fully free. Without this,
    // the first DrainSlots (RebuildScaling during Initialize) would wait
    // forever for slots that were never handed out.
    _freeCount = kSlotCount;
    for (int i = 0; i < kSlotCount; ++i) {
        _freeStack[i] = kSlotCount - 1 - i;
    }
    return true;
}

bool D3D12Context::CreateRawBuffer(UINT64 bytes, D3D12_HEAP_TYPE heapType,
                                   D3D12_RESOURCE_STATES initialState,
                                   ID3D12Resource **out, size_t &alignedPitch,
                                   UINT bytesPerRow, char *err, size_t errLen) noexcept {
    alignedPitch = (static_cast<size_t>(bytesPerRow) + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                   & ~static_cast<size_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = alignedPitch * (static_cast<UINT64>(bytes) / bytesPerRow);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr = _device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, initialState,
        nullptr, IID_PPV_ARGS(out));
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "CreateCommittedResource(buffer) failed");
        return false;
    }
    return true;
}

bool D3D12Context::CreateSlotResources(FrameSlot &slot, char *err, size_t errLen) noexcept {
    // Re-entrant on the hot path (a resolution change reuses the warm
    // context): release the previous generation of slot resources first.
    // Safe because the caller holds a PoolHold — every slot is idle and its
    // GPU work has completed (slots are only released after WaitFrame).
    if (slot.fenceEvent) {
        CloseHandle(slot.fenceEvent);
        slot.fenceEvent = nullptr;
    }
    if (slot.upload && slot.uploadMapped) slot.upload->Unmap(0, nullptr);
    if (slot.readback && slot.readbackMapped) slot.readback->Unmap(0, nullptr);
    slot.uploadMapped = nullptr;
    slot.readbackMapped = nullptr;
    slot.commandList.Reset();
    slot.allocator.Reset();
    slot.upload.Reset();
    slot.readback.Reset();
    slot.inputColor.Reset();
    slot.outputColor.Reset();
    slot.reducedColor.Reset();
    slot.reducedDenoised.Reset();
    slot.controlledRes.Reset();
    slot.horizontalRes.Reset();
    slot.motion.Reset();
    slot.confidence.Reset();
    slot.reducedMotion.Reset();
    slot.reducedConfidence.Reset();
    slot.srvUavHeap.Reset();

    const int width = _width;
    const int height = _height;
    HRESULT hr = _device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(slot.allocator.GetAddressOf()));
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "CreateCommandAllocator(slot) failed");
        return false;
    }
    hr = _device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
        IID_PPV_ARGS(slot.commandList.GetAddressOf()));
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "CreateCommandList(slot) failed");
        return false;
    }
    hr = slot.commandList->Close();
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "Close initial slot command list failed");
        return false;
    }
    slot.fenceEvent = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!slot.fenceEvent) {
        SetErr(err, errLen, E_FAIL, "Create slot fence event failed");
        return false;
    }

    // B8G8R8A8_UNORM(与 Magpie 渲染管线同款):NVOF 输入格式要求 BGRA8
    // (ABGR8),NGX DLSSNR 的参考宿主也是 BGRA —— 逐像素字节序在
    // PackInput/UnpackOutput 两端换位,逻辑颜色内容与旧 RGBA8 版逐位等价。
    if (!CreateColorTexture(slot.inputColor.GetAddressOf(), width, height,
                            DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_NONE, err, errLen)) {
        return false;
    }
    // NGX binds the output as a UAV; a D3D12-native resource requires
    // ALLOW_UNORDERED_ACCESS (Magpie's D3D11-shared texture had no such flag
    // constraint, our native one does).
    if (!CreateColorTexture(slot.outputColor.GetAddressOf(), width, height,
                            DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }

    // NVOF guidance 槽纹理:densify 写稠密运动(R16G16_FLOAT)+ 置信度
    // (R8_UNORM),NGX evaluate 读。OF 关闭时保持 COMMON 不被触碰。
    if (!CreateColorTexture(slot.motion.GetAddressOf(), width, height,
                            DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    if (!CreateColorTexture(slot.confidence.GetAddressOf(), width, height,
                            DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }

    // Upload staging:RGBA8 行距 256 对齐(persist-mapped once)
    {
        if (!CreateRawBuffer(static_cast<UINT64>(height) * width * 4, D3D12_HEAP_TYPE_UPLOAD,
                             D3D12_RESOURCE_STATE_GENERIC_READ,
                             slot.upload.GetAddressOf(), slot.uploadPitch,
                             width * 4, err, errLen)) {
            return false;
        }
        hr = slot.upload->Map(0, nullptr, &slot.uploadMapped);
        if (FAILED(hr)) {
            SetErr(err, errLen, hr, "Map(upload) failed");
            return false;
        }
    }
    {
        // READBACK heap only accepts buffers on this runtime (texture staging
        // on READBACK heap returns E_INVALIDARG), so copy via placed footprint
        // into a readback buffer. Persist-mapped like the upload side: the
        // per-frame Map/Unmap pair was pure driver-call overhead.
        slot.readbackPitch = slot.uploadPitch; // same 256-aligned RGBA8 row size
        if (!CreateRawBuffer(static_cast<UINT64>(height) * width * 4, D3D12_HEAP_TYPE_READBACK,
                             D3D12_RESOURCE_STATE_COPY_DEST,
                             slot.readback.GetAddressOf(), slot.readbackPitch,
                             width * 4, err, errLen)) {
            return false;
        }
        hr = slot.readback->Map(0, nullptr, &slot.readbackMapped);
        if (FAILED(hr)) {
            SetErr(err, errLen, hr, "Map(readback) failed");
            return false;
        }
    }
    {
        // slot-local shader-visible descriptor heap; input/output descriptors
        // here, the residual pipeline descriptors on every scaling rebuild.
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 22;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = _device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(slot.srvUavHeap.GetAddressOf()));
        if (FAILED(hr)) {
            SetErr(err, errLen, hr, "CreateDescriptorHeap(SRV/UAV slot) failed");
            return false;
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE base = slot.srvUavHeap->GetCPUDescriptorHandleForHeapStart();
        _device->CreateShaderResourceView(slot.inputColor.Get(), nullptr, base);
        const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto slotHandle = [&](UINT i) {
            return D3D12_CPU_DESCRIPTOR_HANDLE{ base.ptr + static_cast<SIZE_T>(i * inc) };
        };
        _device->CreateUnorderedAccessView(slot.outputColor.Get(), nullptr, nullptr, slotHandle(7));
        // 10-13:densify 的 motion/confidence SRV+UAV(源尺寸)。
        _device->CreateShaderResourceView(slot.motion.Get(), nullptr, slotHandle(10));
        _device->CreateShaderResourceView(slot.confidence.Get(), nullptr, slotHandle(11));
        _device->CreateUnorderedAccessView(slot.motion.Get(), nullptr, nullptr, slotHandle(12));
        _device->CreateUnorderedAccessView(slot.confidence.Get(), nullptr, nullptr, slotHandle(13));
        // 14-17 预置为 inputColor 的占位视图(NVOF 会话建立时由
        // BindNvofResources 覆盖为 flow/cost 视图;densify 只在会话存活时
        // 被记录,占位视图永不被有效读取)。不要在这里建 NULL 描述符:
        // CreateShaderResourceView(nullptr,nullptr) 在本机驱动(RTX 3080)
        // 上触发异步 TDR(DEVICE_HUNG,2026-09-07 二分定位)。
        _device->CreateShaderResourceView(slot.inputColor.Get(), nullptr, slotHandle(14));
        _device->CreateShaderResourceView(slot.inputColor.Get(), nullptr, slotHandle(15));
        _device->CreateShaderResourceView(slot.inputColor.Get(), nullptr, slotHandle(16));
        _device->CreateShaderResourceView(slot.inputColor.Get(), nullptr, slotHandle(17));
    }
    return true;
}

bool D3D12Context::PackInput(
    FrameSlot &slot,
    const uint8_t *const *srcPlanes, const int64_t *srcStrides,
    int width, int height, char *err, size_t errLen) noexcept {
    if (width != _width || height != _height) {
        SetErr(err, errLen, E_INVALIDARG, "PackInput: size mismatch");
        return false;
    }
    auto *dstRow = static_cast<uint8_t *>(slot.uploadMapped);
    for (int y = 0; y < height; ++y, dstRow += slot.uploadPitch) {
        const float *rowR = reinterpret_cast<const float *>(srcPlanes[0] + srcStrides[0] * y);
        const float *rowG = reinterpret_cast<const float *>(srcPlanes[1] + srcStrides[1] * y);
        const float *rowB = reinterpret_cast<const float *>(srcPlanes[2] + srcStrides[2] * y);
        uint32_t *dstPx = reinterpret_cast<uint32_t *>(dstRow);
        for (int x = 0; x < width; ++x) {
            const uint32_t r = static_cast<uint32_t>(Saturate(rowR[x]) * 255.0f + 0.5f);
            const uint32_t g = static_cast<uint32_t>(Saturate(rowG[x]) * 255.0f + 0.5f);
            const uint32_t b = static_cast<uint32_t>(Saturate(rowB[x]) * 255.0f + 0.5f);
            // BGRA 字节序(byte0=B):NVOF 输入格式 + Magpie 管线同款。
            dstPx[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    return true;
}

bool D3D12Context::BeginFrameRecording(FrameSlot &slot) noexcept {
    HRESULT hr = slot.allocator->Reset();
    if (FAILED(hr)) {
        // A previous frame's mid-recording error return leaves the command
        // list open, which makes allocator/list Reset fail with E_FAIL from
        // then on; force-close once and retry so a single SEH doesn't brick
        // this slot for the rest of the session.
        slot.commandList->Close();
        hr = slot.allocator->Reset();
        if (FAILED(hr)) return false;
    }
    hr = slot.commandList->Reset(slot.allocator.Get(), nullptr);
    return SUCCEEDED(hr);
}

bool D3D12Context::RecordUploadCopy(FrameSlot &slot, D3D12_RESOURCE_STATES stateAfter, char *err, size_t errLen) noexcept {
    ID3D12GraphicsCommandList *cl = slot.commandList.Get();
    D3D12_RESOURCE_BARRIER toCopyDest[1]{
        Transition(slot.inputColor.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    cl->ResourceBarrier(1, toCopyDest);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = slot.upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = static_cast<UINT>(_width);
    src.PlacedFootprint.Footprint.Height = static_cast<UINT>(_height);
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(slot.uploadPitch);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = slot.inputColor.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER toAfter[1]{
        Transition(slot.inputColor.Get(), D3D12_RESOURCE_STATE_COPY_DEST, stateAfter),
    };
    cl->ResourceBarrier(1, toAfter);
    return true;
}

bool D3D12Context::RecordReadbackCopy(FrameSlot &slot, D3D12_RESOURCE_STATES stateBefore, char *err, size_t errLen) noexcept {
    ID3D12GraphicsCommandList *cl = slot.commandList.Get();
    D3D12_RESOURCE_BARRIER toCopySrc[1]{
        Transition(slot.outputColor.Get(), stateBefore, D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    cl->ResourceBarrier(1, toCopySrc);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = slot.outputColor.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = slot.readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = static_cast<UINT>(_width);
    dst.PlacedFootprint.Footprint.Height = static_cast<UINT>(_height);
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(slot.readbackPitch);
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER backToCommon[1]{
        Transition(slot.outputColor.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
    };
    cl->ResourceBarrier(1, backToCommon);
    return true;
}

bool D3D12Context::SubmitFrame(FrameSlot &slot, ID3D12Fence *waitFence,
                               uint64_t waitValue, char *err, size_t errLen) noexcept {
    HRESULT hr = slot.commandList->Close();
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "Close(slot) failed");
        return false;
    }
    // Submit mutex keeps the fence value order identical to the queue's
    // ExecuteCommandLists order — concurrent frame threads must not let a
    // later Signal (smaller value) land after an earlier one (fence values
    // must never regress).
    std::lock_guard<std::mutex> lock(_submitMutex);
    // NVOF guidance:本槽 densify 依赖 NVOF execute 的 flow 输出 —— 队列级
    // 栅栏等待(顺序无关:常规顺序下该值已满足,零开销;乱序提交时它把本
    // 槽命令排到 NVOF 输出之后,防 GPU 端读-写竞争)。
    if (waitFence && waitValue) {
        _queue->Wait(waitFence, waitValue);
    }
    ID3D12CommandList *lists[]{ slot.commandList.Get() };
    _queue->ExecuteCommandLists(1, lists);
    slot.fenceValue = _fenceValue.fetch_add(1) + 1;
    _queue->Signal(_fence.Get(), slot.fenceValue);
    return true;
}

bool D3D12Context::WaitFrame(FrameSlot &slot, char *err, size_t errLen) noexcept {
    return WaitFenceValue(slot.fenceValue, slot.fenceEvent, err, errLen);
}

bool D3D12Context::UnpackOutput(
    FrameSlot &slot,
    uint8_t **dstPlanes, int64_t *dstStrides,
    int width, int height, char *err, size_t errLen) noexcept {
    if (width != _width || height != _height) {
        SetErr(err, errLen, E_INVALIDARG, "UnpackOutput: size mismatch");
        return false;
    }

    // The readback buffer is persist-mapped at creation (mirrors the upload
    // side): the per-frame Map/Unmap pair only bought two driver calls.
    const size_t pitch = slot.readbackPitch;
    const auto *srcRow = static_cast<const uint8_t *>(slot.readbackMapped);
    for (int y = 0; y < height; ++y, srcRow += pitch) {
        const uint8_t *row = srcRow;
        float *rowR = reinterpret_cast<float *>(dstPlanes[0] + dstStrides[0] * y);
        float *rowG = reinterpret_cast<float *>(dstPlanes[1] + dstStrides[1] * y);
        float *rowB = reinterpret_cast<float *>(dstPlanes[2] + dstStrides[2] * y);
        for (int x = 0; x < width; ++x) {
            // BGRA 字节序(byte0=B):与 PackInput 的写入换位成对。
            rowB[x] = row[x * 4 + 0] * (1.0f / 255.0f);
            rowG[x] = row[x * 4 + 1] * (1.0f / 255.0f);
            rowR[x] = row[x * 4 + 2] * (1.0f / 255.0f);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Residual pipeline (ported from Magpie DLSSNRFilter.cpp, 2d37f8c0 / v0.6.6):
// two-pass Lanczos2 color downsample (vertical -> horizontal, 1cde1bae
// "lanczos2-aa"; at equal extents Lanczos2 degenerates to an exact copy) ->
// NGX evaluate at internal resolution ->
// PrepareResidual (per-pixel fine controls in the low-resolution domain) ->
// Catmull-Rom horizontal residual upsample (skipped at equal width) ->
// Catmull-Rom vertical + composite (saturate(original + residual)).
// ---------------------------------------------------------------------------

namespace {

// Upstream COLOR_DOWNSAMPLE_HLSL verbatim: vertical pass writes the shared
// FP16 intermediate (negative lobes survive), horizontal pass lands the
// reduced color in the NGX input texture.
constexpr char DOWNSAMPLE_HLSL[] = R"(
Texture2D<float4> InputColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
    float2 MotionScale;
    float ResidualMultiplier;
    float ResidualSaturation;
    float ResidualLightness;
    float ShadowStructureMultiplier;
    float ReflectionGlowMultiplier;
};

float Sinc(float x) {
    if (abs(x) < 1e-5) return 1.0;
    x *= 3.14159265358979323846;
    return sin(x) / x;
}

float Lanczos2(float x) {
    return abs(x) < 2.0 ? Sinc(x) * Sinc(x * 0.5) : 0.0;
}

[numthreads(8, 8, 1)]
void DownsampleColorVertical(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= SourceExtent.x || tid.y >= TargetExtent.y) return;
    float scale = float(TargetExtent.y) / float(SourceExtent.y);
    float position = (float(tid.y) + 0.5) / scale - 0.5;
    float support = 2.0 / scale;
    int first = int(ceil(position - support));
    int last = int(floor(position + support));
    float4 total = 0.0;
    float totalWeight = 0.0;
    [loop]
    for (int y = first; y <= last; ++y) {
        float weight = Lanczos2((float(y) - position) * scale);
        total += InputColor.Load(int3(tid.x,
            clamp(y, 0, int(SourceExtent.y) - 1), 0)) * weight;
        totalWeight += weight;
    }
    // Keep negative lobes in the shared FP16 intermediate, including alpha.
    OutputColor[tid.xy] = total / (abs(totalWeight) > 1e-6 ? totalWeight : 1.0);
}

[numthreads(8, 8, 1)]
void DownsampleColorHorizontal(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    float scale = float(TargetExtent.x) / float(SourceExtent.x);
    float position = (float(tid.x) + 0.5) / scale - 0.5;
    float support = 2.0 / scale;
    int first = int(ceil(position - support));
    int last = int(floor(position + support));
    float4 total = 0.0;
    float totalWeight = 0.0;
    [loop]
    for (int x = first; x <= last; ++x) {
        float weight = Lanczos2((float(x) - position) * scale);
        total += InputColor.Load(int3(
            clamp(x, 0, int(SourceExtent.x) - 1), tid.y, 0)) * weight;
        totalWeight += weight;
    }
    OutputColor[tid.xy] = total / (abs(totalWeight) > 1e-6 ? totalWeight : 1.0);
}
)";

constexpr char RESIDUAL_PREPARE_HLSL[] = R"(
Texture2D<float4> ReducedColor : register(t0);
Texture2D<float4> ReducedDenoised : register(t1);
RWTexture2D<float4> ControlledResidual : register(u0);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
    float2 MotionScale;
    float ResidualMultiplier;
    float ResidualSaturation;
    float ResidualLightness;
    float ShadowStructureMultiplier;
    float ReflectionGlowMultiplier;
};

float3 RGBToHSL(float3 color) {
    float maximum = max(color.r, max(color.g, color.b));
    float minimum = min(color.r, min(color.g, color.b));
    float delta = maximum - minimum;
    float lightness = (maximum + minimum) * 0.5;
    if (delta <= 1e-6) {
        return float3(0.0, 0.0, lightness);
    }

    float hue = 0.0;
    if (maximum == color.r) {
        hue = (color.g - color.b) / delta;
        if (hue < 0.0) hue += 6.0;
    } else if (maximum == color.g) {
        hue = (color.b - color.r) / delta + 2.0;
    } else {
        hue = (color.r - color.g) / delta + 4.0;
    }
    float saturation = delta / max(1.0 - abs(2.0 * lightness - 1.0), 1e-6);
    return float3(hue / 6.0, saturate(saturation), saturate(lightness));
}

float HueToRGB(float p, float q, float hue) {
    hue = frac(hue);
    if (hue < 1.0 / 6.0) return p + (q - p) * 6.0 * hue;
    if (hue < 1.0 / 2.0) return q;
    if (hue < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - hue) * 6.0;
    return p;
}

float3 HSLToRGB(float3 hsl) {
    if (hsl.y <= 1e-6) {
        return float3(hsl.z, hsl.z, hsl.z);
    }
    float q = hsl.z < 0.5 ?
        hsl.z * (1.0 + hsl.y) : hsl.z + hsl.y - hsl.z * hsl.y;
    float p = 2.0 * hsl.z - q;
    return saturate(float3(
        HueToRGB(p, q, hsl.x + 1.0 / 3.0),
        HueToRGB(p, q, hsl.x),
        HueToRGB(p, q, hsl.x - 1.0 / 3.0)));
}

float3 ToLinear(float3 color) {
    return float3(
        color.r <= 0.04045 ? color.r / 12.92 : pow(max(color.r + 0.055, 0.0) / 1.055, 2.4),
        color.g <= 0.04045 ? color.g / 12.92 : pow(max(color.g + 0.055, 0.0) / 1.055, 2.4),
        color.b <= 0.04045 ? color.b / 12.92 : pow(max(color.b + 0.055, 0.0) / 1.055, 2.4));
}

float3 ApplyResidualControls(float3 original, float3 residual) {
    residual *= ResidualMultiplier;
    if (all(residual == 0.0)) return original;
    float4 fineControls = float4(
        ResidualSaturation, ResidualLightness,
        ShadowStructureMultiplier, ReflectionGlowMultiplier);
    // Neutral fine controls preserve the multiplied residual in this low-resolution domain.
    float3 output = saturate(original + residual);
    [branch]
    if (any(abs(fineControls - 1.0) >= 1e-6)) {
        // Classify the whole pixel before directional/HSL controls. The
        // reference cannot depend on the multiplier selected by this branch.
        float deltaY = dot(ToLinear(output) - ToLinear(original),
            float3(0.2126, 0.7152, 0.0722));
        float directionalMultiplier = deltaY < 0.0 ? ShadowStructureMultiplier :
            (deltaY > 0.0 ? ReflectionGlowMultiplier : 1.0);
        float3 controlledResidual = residual * directionalMultiplier;
        float3 candidate = saturate(original + controlledResidual);
        [branch]
        if (abs(ResidualSaturation - 1.0) >= 1e-6 ||
            abs(ResidualLightness - 1.0) >= 1e-6) {
            // The SRVs are non-sRGB UNORM views, so HSL operates on normalized
            // stored SDR RGB values without an implicit transfer conversion.
            float3 originalHSL = RGBToHSL(original);
            float3 candidateHSL = RGBToHSL(candidate);
            candidateHSL.y = saturate(originalHSL.y +
                (candidateHSL.y - originalHSL.y) * ResidualSaturation);
            candidateHSL.z = saturate(originalHSL.z +
                (candidateHSL.z - originalHSL.z) * ResidualLightness);
            candidate = HSLToRGB(candidateHSL);
        }
        output = candidate;
    }
    return output;
}

[numthreads(8, 8, 1)]
void PrepareResidual(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    float3 original = ReducedColor.Load(int3(tid.xy, 0)).rgb;
    float3 denoised = ReducedDenoised.Load(int3(tid.xy, 0)).rgb;
    // Apply every residual control once per low-resolution pixel, before
    // either Catmull-Rom pass. Keep signed differences in an FP16 texture.
    ControlledResidual[tid.xy] = float4(
        ApplyResidualControls(original, denoised - original) - original, 0.0);
}
)";

constexpr char RESIDUAL_HORIZONTAL_HLSL[] = R"(
Texture2D<float4> ControlledResidual : register(t0);
RWTexture2D<float4> HorizontalResidual : register(u0);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
    float2 MotionScale;
    float ResidualMultiplier;
    float ResidualSaturation;
    float ResidualLightness;
    float ShadowStructureMultiplier;
    float ReflectionGlowMultiplier;
};

float CatmullRom(float x) {
    x = abs(x);
    if (x < 1.0) return ((1.5 * x - 2.5) * x) * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}

[numthreads(8, 8, 1)]
void UpsampleResidualHorizontal(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= SourceExtent.x || tid.y >= TargetExtent.y) return;
    if (SourceExtent.x == TargetExtent.x) {
        HorizontalResidual[tid.xy] =
            ControlledResidual.Load(int3(tid.xy, 0));
        return;
    }
    float reducedPosition = (float(tid.x) + 0.5) *
        float(TargetExtent.x) / float(SourceExtent.x) - 0.5;
    int center = int(floor(reducedPosition));
    float3 residual = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (int x = -1; x <= 2; ++x) {
        float weight = CatmullRom(reducedPosition - float(center + x));
        int sampleX = clamp(center + x, 0, int(TargetExtent.x) - 1);
        int3 samplePixel = int3(sampleX, tid.y, 0);
        residual += ControlledResidual.Load(samplePixel).rgb * weight;
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
    float ResidualSaturation;
    float ResidualLightness;
    float ShadowStructureMultiplier;
    float ReflectionGlowMultiplier;
};

float CatmullRom(float x) {
    x = abs(x);
    if (x < 1.0) return ((1.5 * x - 2.5) * x) * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}

[numthreads(8, 8, 1)]
void CompositeResidualVertical(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= SourceExtent)) return;
    float4 storedOriginal = OriginalColor.Load(int3(tid.xy, 0));
    // A typed BGRA SRV already returns logical RGBA components.
    float3 original = storedOriginal.rgb;
    float3 residual = 0.0;
    if (SourceExtent.y == TargetExtent.y) {
        residual = HorizontalResidual.Load(int3(tid.xy, 0)).rgb;
    } else {
        float reducedPosition = (float(tid.y) + 0.5) *
            float(TargetExtent.y) / float(SourceExtent.y) - 0.5;
        int center = int(floor(reducedPosition));
        residual = 0.0;
        float totalWeight = 0.0;
        [unroll]
        for (int y = -1; y <= 2; ++y) {
            float weight = CatmullRom(reducedPosition - float(center + y));
            int sampleY = clamp(center + y, 0, int(TargetExtent.y) - 1);
            residual += HorizontalResidual.Load(
                int3(tid.x, sampleY, 0)).rgb * weight;
            totalWeight += weight;
        }
        residual /= abs(totalWeight) > 1e-6 ? totalWeight : 1.0;
    }
    OutputColor[tid.xy] = float4(
        saturate(original + residual), storedOriginal.a);
}
)";

// Magpie NVOF_Densify(NvidiaOpticalFlowProvider.cpp:15-92)原样移植:S10.5
// 网格光流 + 前后向一致性校验 → 逐像素稠密运动(R16G16_FLOAT)+ 置信度
// (R8_UNORM)。Turing 无 cost 输出时的 0.65 基线保留。
constexpr char DENSIFY_HLSL[] = R"(
Texture2D<int2> ForwardFlow : register(t0);
Texture2D<int2> BackwardFlow : register(t1);
Texture2D<uint> ForwardCost : register(t2);
Texture2D<uint> BackwardCost : register(t3);
RWTexture2D<float2> DenseMotion : register(u0);
RWTexture2D<float> DenseConfidence : register(u1);

cbuffer Params : register(b0) {
    uint2 SourceExtent;
    uint2 FlowExtent;
    uint GridSize;
    uint HasForwardCost;
    uint HasBackward;
    uint HasBackwardCost;
};

float2 LoadFlow(Texture2D<int2> field, int2 p) {
    p = clamp(p, int2(0, 0), int2(FlowExtent) - 1);
    return float2(field.Load(int3(p, 0))) / 32.0;
}

float2 SampleFlow(Texture2D<int2> field, float2 sourcePixel) {
    float2 gridPos = sourcePixel / float(GridSize) - 0.5;
    int2 p0 = int2(floor(gridPos));
    float2 f = frac(gridPos);
    return lerp(
        lerp(LoadFlow(field, p0), LoadFlow(field, p0 + int2(1, 0)), f.x),
        lerp(LoadFlow(field, p0 + int2(0, 1)),
             LoadFlow(field, p0 + int2(1, 1)), f.x),
        f.y);
}

float LoadCost(Texture2D<uint> field, int2 p) {
    p = clamp(p, int2(0, 0), int2(FlowExtent) - 1);
    return float(field.Load(int3(p, 0))) / 255.0;
}

float SampleCost(Texture2D<uint> field, float2 sourcePixel) {
    float2 gridPos = sourcePixel / float(GridSize) - 0.5;
    int2 p0 = int2(floor(gridPos));
    float2 f = frac(gridPos);
    return lerp(
        lerp(LoadCost(field, p0), LoadCost(field, p0 + int2(1, 0)), f.x),
        lerp(LoadCost(field, p0 + int2(0, 1)),
             LoadCost(field, p0 + int2(1, 1)), f.x),
        f.y);
}

[numthreads(8, 8, 1)]
void Densify(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= SourceExtent)) return;

    float2 p = float2(tid.xy) + 0.5;
    float2 forward = SampleFlow(ForwardFlow, p);
    // Turing has no hardware cost output. A conservative non-zero baseline
    // still lets downstream consumers use coherent motion while reset frames
    // remain explicitly zero-confidence.
    float confidence = HasForwardCost != 0 ?
        1.0 - SampleCost(ForwardCost, p) : 0.65;

    if (HasBackward != 0) {
        float2 referencePixel = p + forward;
        bool inside = all(referencePixel >= 0.0) &&
            all(referencePixel < float2(SourceExtent));
        float2 backward = SampleFlow(BackwardFlow, referencePixel);
        float fbError = length(forward + backward);
        float threshold = 0.75 + 0.05 * length(forward);
        confidence *= inside ? saturate(1.0 - fbError / threshold) : 0.0;
        if (HasBackwardCost != 0) {
            confidence *= 1.0 - SampleCost(BackwardCost, referencePixel);
        }
    }

    DenseMotion[tid.xy] = forward;
    DenseConfidence[tid.xy] = saturate(confidence);
}
)";

// Magpie DownsampleGuidance(DLSSNRFilter.cpp:230-291)移植。与上游差异:
// 深度输出被裁掉 —— 本宿主的 depth 恒为零纹理(depth=zero-contract),
// 降采样深度是死重,NGX 直接消费静态零纹理(源尺寸或内部尺寸)。
constexpr char GUIDANCE_DOWNSAMPLE_HLSL[] = R"(
Texture2D<float2> InputMotion : register(t0);
Texture2D<float> InputConfidence : register(t1);
RWTexture2D<float2> OutputMotion : register(u0);
RWTexture2D<float> OutputConfidence : register(u1);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
    float2 MotionScale;
    float ResidualMultiplier;
    float ResidualSaturation;
    float ResidualLightness;
    float ShadowStructureMultiplier;
    float ReflectionGlowMultiplier;
};

[numthreads(8, 8, 1)]
void DownsampleGuidance(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    float2 sourceStart = float2(tid.xy) * float2(SourceExtent) /
        float2(TargetExtent);
    float2 sourceEnd = float2(tid.xy + 1) * float2(SourceExtent) /
        float2(TargetExtent);
    int2 first = int2(floor(sourceStart));
    int2 last = int2(ceil(sourceEnd));
    float2 motionTotal = 0.0;
    float2 weightedMotionTotal = 0.0;
    float confidenceTotal = 0.0;
    float totalWeight = 0.0;
    [loop]
    for (int y = first.y; y < last.y; ++y) {
        float weightY = max(0.0, min(sourceEnd.y, float(y + 1)) -
            max(sourceStart.y, float(y)));
        [loop]
        for (int x = first.x; x < last.x; ++x) {
            float weightX = max(0.0, min(sourceEnd.x, float(x + 1)) -
                max(sourceStart.x, float(x)));
            float weight = weightX * weightY;
            int2 sourcePixel = clamp(
                int2(x, y), int2(0, 0), int2(SourceExtent) - 1);
            float2 motion = InputMotion.Load(int3(sourcePixel, 0));
            float confidence = InputConfidence.Load(int3(sourcePixel, 0));
            motionTotal += motion * weight;
            weightedMotionTotal += motion * confidence * weight;
            confidenceTotal += confidence * weight;
            totalWeight += weight;
        }
    }
    float2 motion = confidenceTotal > 1e-6 ?
        weightedMotionTotal / confidenceTotal :
        motionTotal / max(totalWeight, 1e-6);
    OutputMotion[tid.xy] = motion * MotionScale;
    OutputConfidence[tid.xy] = confidenceTotal / max(totalWeight, 1e-6);
}
)";

} // namespace

bool D3D12Context::DumpTextureToFile(ID3D12Resource *tex, int width, int height,
                                     const wchar_t *path, DXGI_FORMAT format) noexcept {
    // RGBA8 (4 B/px) covers the color dumps; the horizontal residual is now
    // R16G16B16A16_FLOAT (8 B/px) since the residual needs a signed range.
    const UINT bpp = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
    size_t pitch = 0;
    ComPtr<ID3D12Resource> buffer;
    char ignore[96];
    if (!CreateRawBuffer(static_cast<UINT64>(height) * width * bpp, D3D12_HEAP_TYPE_READBACK,
                         D3D12_RESOURCE_STATE_COPY_DEST, buffer.GetAddressOf(),
                         pitch, width * bpp, ignore, sizeof(ignore))) {
        return false;
    }
    if (!BeginCtlRecording()) return false;
    D3D12_RESOURCE_BARRIER b[1]{
        Transition(tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    _ctlCommandList->ResourceBarrier(1, b);
    D3D12_TEXTURE_COPY_LOCATION src{ tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 0 };
    D3D12_TEXTURE_COPY_LOCATION dst{ buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint.Format = format;
    dst.PlacedFootprint.Footprint.Width = static_cast<UINT>(width);
    dst.PlacedFootprint.Footprint.Height = static_cast<UINT>(height);
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(pitch);
    _ctlCommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER back[1]{
        Transition(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
    };
    _ctlCommandList->ResourceBarrier(1, back);
    if (!ExecuteCtlAndWait()) return false;

    void *mapped = nullptr;
    if (FAILED(buffer->Map(0, nullptr, &mapped))) return false;
    FILE *f = nullptr;
    bool ok = _wfopen_s(&f, path, L"wb") == 0 && f;
    if (ok) {
        for (int y = 0; y < height; ++y) {
            fwrite(static_cast<const uint8_t *>(mapped) + static_cast<size_t>(pitch) * y, 1,
                   static_cast<size_t>(width) * bpp, f);
        }
        fclose(f);
    }
    buffer->Unmap(0, nullptr);
    return ok;
}

bool D3D12Context::CreateComputeObjects(char *err, size_t errLen) noexcept {
    // root signature: b0 = 12 root constants (Magpie ResampleConstants, 48B),
    // t0/t1 as independent SRV tables (the passes need non-adjacent descriptor
    // pairs), (u0) UAV table
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
    params[0].Constants.Num32BitValues = 12;
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

    // PSOs are stateless and shared; the shader-visible descriptor heap is
    // per-slot (each slot's textures get their own SRV/UAV descriptors).

    struct Cso { const char *hlsl; const char *entry; ID3D12PipelineState **pso; };
    const Cso csos[] = {
        { DOWNSAMPLE_HLSL, "DownsampleColorVertical", _psoDownsampleVertical.GetAddressOf() },
        { DOWNSAMPLE_HLSL, "DownsampleColorHorizontal", _psoDownsampleHorizontal.GetAddressOf() },
        { RESIDUAL_PREPARE_HLSL, "PrepareResidual", _psoPrepare.GetAddressOf() },
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

    // NVOF guidance(PORTING #6):densify = t0-t3 SRV 表 + u0/u1 各自独立
    // UAV 表 + b0 8 常量;guidance 降采样 = t0/t1 SRV 表 + u0/u1 独立 UAV 表
    // + b0 12 常量(cbuffer 布局与残差管线共用,多出的字段是未用死重)。
    // 注意:同一表内两个 range 的 OffsetInDescriptorsFromTableStart 若都为 0
    // 会构成重叠范围(非法),驱动侧表现为 dispatch 挂死 —— u0/u1 必须各开
    // 一个表参数。
    {
        D3D12_DESCRIPTOR_RANGE srvRanges[4]{};
        for (UINT i = 0; i < 4; ++i) {
            srvRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srvRanges[i].NumDescriptors = 1;
            srvRanges[i].BaseShaderRegister = i;
            srvRanges[i].OffsetInDescriptorsFromTableStart = 0;
        }
        D3D12_DESCRIPTOR_RANGE uavRange0{};
        uavRange0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange0.NumDescriptors = 1;
        uavRange0.BaseShaderRegister = 0;
        uavRange0.OffsetInDescriptorsFromTableStart = 0;
        D3D12_DESCRIPTOR_RANGE uavRange1{};
        uavRange1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange1.NumDescriptors = 1;
        uavRange1.BaseShaderRegister = 1;
        uavRange1.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER params[7]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = 8;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        for (UINT i = 0; i < 4; ++i) {
            params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
            params[1 + i].DescriptorTable.pDescriptorRanges = &srvRanges[i];
            params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[5].DescriptorTable.NumDescriptorRanges = 1;
        params[5].DescriptorTable.pDescriptorRanges = &uavRange0;
        params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[6].DescriptorTable.NumDescriptorRanges = 1;
        params[6].DescriptorTable.pDescriptorRanges = &uavRange1;
        params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 7;
        rsDesc.pParameters = params;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> rsBlob, rsErr;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                               rsBlob.GetAddressOf(), rsErr.GetAddressOf()))) {
            SetErr(err, errLen, E_FAIL, "SerializeRootSignature(densify) failed");
            return false;
        }
        if (FAILED(_device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                                rsBlob->GetBufferSize(), IID_PPV_ARGS(_rsDensify.GetAddressOf())))) {
            SetErr(err, errLen, E_FAIL, "CreateRootSignature(densify) failed");
            return false;
        }
        ComPtr<ID3DBlob> code, csErr;
        if (FAILED(D3DCompile(DENSIFY_HLSL, strlen(DENSIFY_HLSL), nullptr, nullptr, nullptr,
                              "Densify", "cs_5_0", 0, 0, code.GetAddressOf(), csErr.GetAddressOf()))) {
            SetErr(err, errLen, E_FAIL, csErr ? static_cast<const char *>(csErr->GetBufferPointer()) : "D3DCompile(densify) failed");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = _rsDensify.Get();
        psoDesc.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        if (FAILED(_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(_psoDensify.GetAddressOf())))) {
            SetErr(err, errLen, E_FAIL, "CreateComputePipelineState(densify) failed");
            return false;
        }
    }
    {
        D3D12_DESCRIPTOR_RANGE srvRanges[2]{};
        for (UINT i = 0; i < 2; ++i) {
            srvRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srvRanges[i].NumDescriptors = 1;
            srvRanges[i].BaseShaderRegister = i;
            srvRanges[i].OffsetInDescriptorsFromTableStart = 0;
        }
        D3D12_DESCRIPTOR_RANGE uavRange0{};
        uavRange0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange0.NumDescriptors = 1;
        uavRange0.BaseShaderRegister = 0;
        uavRange0.OffsetInDescriptorsFromTableStart = 0;
        D3D12_DESCRIPTOR_RANGE uavRange1{};
        uavRange1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange1.NumDescriptors = 1;
        uavRange1.BaseShaderRegister = 1;
        uavRange1.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER params[5]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = 12;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srvRanges[0];
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &srvRanges[1];
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        // u0/u1 各自独立表参数 + 独立 range(寄存器 0/1):同一表内两个
        // range 的 OffsetInDescriptorsFromTableStart 都为 0 会构成重叠范围
        // (非法),两个参数共用一个 range 也会被序列化拒绝。
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable.NumDescriptorRanges = 1;
        params[3].DescriptorTable.pDescriptorRanges = &uavRange0;
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[4].DescriptorTable.NumDescriptorRanges = 1;
        params[4].DescriptorTable.pDescriptorRanges = &uavRange1;
        params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 5;
        rsDesc.pParameters = params;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> rsBlob, rsErr;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                               rsBlob.GetAddressOf(), rsErr.GetAddressOf()))) {
            SetErr(err, errLen, E_FAIL, "SerializeRootSignature(guidance) failed");
            return false;
        }
        if (FAILED(_device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                                rsBlob->GetBufferSize(), IID_PPV_ARGS(_rsGuidance.GetAddressOf())))) {
            SetErr(err, errLen, E_FAIL, "CreateRootSignature(guidance) failed");
            return false;
        }
        ComPtr<ID3DBlob> code, csErr;
        if (FAILED(D3DCompile(GUIDANCE_DOWNSAMPLE_HLSL, strlen(GUIDANCE_DOWNSAMPLE_HLSL),
                              nullptr, nullptr, nullptr, "DownsampleGuidance", "cs_5_0",
                              0, 0, code.GetAddressOf(), csErr.GetAddressOf()))) {
            SetErr(err, errLen, E_FAIL, csErr ? static_cast<const char *>(csErr->GetBufferPointer()) : "D3DCompile(guidance) failed");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = _rsGuidance.Get();
        psoDesc.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        if (FAILED(_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(_psoGuidanceDownsample.GetAddressOf())))) {
            SetErr(err, errLen, E_FAIL, "CreateComputePipelineState(guidance) failed");
            return false;
        }
    }
    return true;
}

bool D3D12Context::RebuildScaling(int internalW, int internalH, char *err, size_t errLen) noexcept {
    // Replaces every slot's scaling textures; the caller must hold a PoolHold
    // (all slots idle, pool sealed) so no frame can still reference them.
    for (int i = 0; i < kSlotCount; ++i) {
        ClearScalingForSlot(_slots[i]);
        if (!CreateScalingForSlot(_slots[i], internalW, internalH, err, errLen)) {
            _scalingReady = false;
            _internalWidth = 0;
            _internalHeight = 0;
            return false;
        }
    }
    _internalWidth = internalW;
    _internalHeight = internalH;
    _scalingReady = true;
    return true;
}

void D3D12Context::ClearScalingResources() noexcept {
    // Caller must hold a PoolHold (see RebuildScaling).
    for (int i = 0; i < kSlotCount; ++i) {
        ClearScalingForSlot(_slots[i]);
    }
    // zero the reported internal size too: stats consumers treat 0 as
    // "scaling disabled" instead of a stale percentage of a previous state
    _internalWidth = 0;
    _internalHeight = 0;
    _scalingReady = false;
}

bool D3D12Context::CreateScalingForSlot(FrameSlot &slot, int internalW, int internalH, char *err, size_t errLen) noexcept {
    if (!_rsCompute && !CreateComputeObjects(err, errLen)) return false;

    // rebuild internal textures (sizes depend on the resolution percent)
    slot.reducedColor.Reset();
    slot.reducedDenoised.Reset();
    slot.controlledRes.Reset();
    slot.horizontalRes.Reset();
    slot.reducedMotion.Reset();
    slot.reducedConfidence.Reset();
    if (!CreateColorTexture(slot.reducedColor.GetAddressOf(), internalW, internalH,
                            DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    if (!CreateColorTexture(slot.reducedDenoised.GetAddressOf(), internalW, internalH,
                            DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    // R16G16B16A16_FLOAT: the controlled residual is signed (denoised − color
    // after fine controls); a UNORM texture would clamp the negative (darken-
    // noise) half of the correction away (Magpie DLSSNRFilter.cpp uses 16F).
    if (!CreateColorTexture(slot.controlledRes.GetAddressOf(), internalW, internalH,
                            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    if (!CreateColorTexture(slot.horizontalRes.GetAddressOf(), _width, internalH,
                            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    // guidance 降采样目标(内部尺寸):densify 输出的源尺寸运动在缩放启用
    // 时按 Magpie DownsampleGuidance 收缩(置信度加权,运动向量乘
    // MotionScale 换算到内部像素单位)。
    if (!CreateColorTexture(slot.reducedMotion.GetAddressOf(), internalW, internalH,
                            DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    if (!CreateColorTexture(slot.reducedConfidence.GetAddressOf(), internalW, internalH,
                            DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }

    // (re)write SRV/UAV descriptors: 0=srvInput 1=srvReducedColor 2=srvReducedDenoised
    // 3=srvHorizontal 4=uavReducedColor 5=uavReducedDenoised 6=uavHorizontal 7=uavOutput
    // 8=srvControlled 9=uavControlled 18=srvReducedMotion 19=srvReducedConfidence
    // 20=uavReducedMotion 21=uavReducedConfidence
    const D3D12_CPU_DESCRIPTOR_HANDLE base = slot.srvUavHeap->GetCPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto slotHandle = [&](UINT i) { return D3D12_CPU_DESCRIPTOR_HANDLE{ base.ptr + static_cast<SIZE_T>(i * inc) }; };
    _device->CreateShaderResourceView(slot.reducedColor.Get(), nullptr, slotHandle(1));
    _device->CreateShaderResourceView(slot.reducedDenoised.Get(), nullptr, slotHandle(2));
    _device->CreateShaderResourceView(slot.horizontalRes.Get(), nullptr, slotHandle(3));
    _device->CreateUnorderedAccessView(slot.reducedColor.Get(), nullptr, nullptr, slotHandle(4));
    _device->CreateUnorderedAccessView(slot.reducedDenoised.Get(), nullptr, nullptr, slotHandle(5));
    _device->CreateUnorderedAccessView(slot.horizontalRes.Get(), nullptr, nullptr, slotHandle(6));
    _device->CreateShaderResourceView(slot.controlledRes.Get(), nullptr, slotHandle(8));
    _device->CreateUnorderedAccessView(slot.controlledRes.Get(), nullptr, nullptr, slotHandle(9));
    _device->CreateShaderResourceView(slot.reducedMotion.Get(), nullptr, slotHandle(18));
    _device->CreateShaderResourceView(slot.reducedConfidence.Get(), nullptr, slotHandle(19));
    _device->CreateUnorderedAccessView(slot.reducedMotion.Get(), nullptr, nullptr, slotHandle(20));
    _device->CreateUnorderedAccessView(slot.reducedConfidence.Get(), nullptr, nullptr, slotHandle(21));
    return true;
}

void D3D12Context::ClearScalingForSlot(FrameSlot &slot) noexcept {
    slot.horizontalRes.Reset();
    slot.controlledRes.Reset();
    slot.reducedDenoised.Reset();
    slot.reducedColor.Reset();
    slot.reducedMotion.Reset();
    slot.reducedConfidence.Reset();
}

void D3D12Context::RecordPass(FrameSlot &slot, ID3D12PipelineState *pso, UINT srv0, UINT srv1, UINT uav,
                              UINT dispatchX, UINT dispatchY, const ResidualControls &rc) noexcept {
    // NGX's evaluate may rebind its own descriptor heap / root signature on
    // this command list; rebind ours before touching our descriptors.
    ID3D12GraphicsCommandList *cl = slot.commandList.Get();
    cl->SetComputeRootSignature(_rsCompute.Get());
    cl->SetPipelineState(pso);
    ID3D12DescriptorHeap *heaps[]{ slot.srvUavHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = slot.srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(i * inc) }; };

    const UINT srcWH[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const UINT dstWH[2]{ static_cast<UINT>(_internalWidth), static_cast<UINT>(_internalHeight) };
    cl->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    cl->SetComputeRoot32BitConstants(0, 2, dstWH, 2);
    const float zero = 0.0f;
    cl->SetComputeRoot32BitConstants(0, 1, &zero, 4);      // Padding0
    const float motion[2]{ 1.0f, 1.0f };
    cl->SetComputeRoot32BitConstants(0, 2, motion, 5);
    // SetComputeRoot32BitConstants with more than one DWORD needs a real array
    // (a scalar temporary's address won't do).
    const float controls[5]{ rc.multiplier, rc.saturation, rc.lightness,
                             rc.shadowStructure, rc.reflectionGlow };
    cl->SetComputeRoot32BitConstants(0, 5, controls, 7);

    cl->SetComputeRootDescriptorTable(1, gpu(srv0));
    cl->SetComputeRootDescriptorTable(2, gpu(srv1));
    cl->SetComputeRootDescriptorTable(3, gpu(uav));
    cl->Dispatch(dispatchX, dispatchY, 1);
}

void D3D12Context::RecordDownsampleVertical(FrameSlot &slot, const ResidualControls &rc) noexcept {
    // full -> (fullW x internalH) into the shared FP16 intermediate; upstream
    // reuses one texture for this and the residual horizontal pass, the port
    // reuses horizontalRes the same way.
    RecordPass(slot, _psoDownsampleVertical.Get(), 0, 0, 6, // t0 input, t1 dummy, u0 horizontalRes
               (static_cast<UINT>(_width) + 7) / 8,
               (static_cast<UINT>(_internalHeight) + 7) / 8, rc);
}

void D3D12Context::RecordDownsampleHorizontal(FrameSlot &slot, const ResidualControls &rc) noexcept {
    // t0/t1 read the vertical pass's output through its SRV descriptor
    // (slot 3) — slot 6 holds horizontalRes's UAV descriptor, and binding a
    // UAV descriptor into an SRV root table is invalid per the D3D12 spec
    // (the debug layer flags it; other drivers may not read it correctly).
    RecordPass(slot, _psoDownsampleHorizontal.Get(), 3, 3, 4, // t0 intermediate, t1 dummy, u0 reducedColor
               (static_cast<UINT>(_internalWidth) + 7) / 8,
               (static_cast<UINT>(_internalHeight) + 7) / 8, rc);
}

void D3D12Context::RecordResidualPrepare(FrameSlot &slot, const ResidualControls &rc) noexcept {
    RecordPass(slot, _psoPrepare.Get(), 1, 2, 9, // t0 reducedColor, t1 reducedDenoised, u0 controlledRes
               (static_cast<UINT>(_internalWidth) + 7) / 8,
               (static_cast<UINT>(_internalHeight) + 7) / 8, rc);
}

void D3D12Context::RecordResidualHorizontal(FrameSlot &slot, const ResidualControls &rc) noexcept {
    RecordPass(slot, _psoHorizontal.Get(), 8, 8, 6, // t0 controlledRes, t1 dummy, u0 horizontalRes
               (static_cast<UINT>(_width) + 7) / 8,
               (static_cast<UINT>(_internalHeight) + 7) / 8, rc);
}

void D3D12Context::RecordResidualVertical(FrameSlot &slot, const ResidualControls &rc,
                                          bool equalWidth) noexcept {
    // Equal internal width skips the horizontal pass (Magpie binds the
    // controlled residual straight into the vertical pass's t1).
    const UINT residualSrv = equalWidth ? 8 : 3;
    RecordPass(slot, _psoVertical.Get(), 0, residualSrv, 7, // t0 input, t1 residual, u0 output
               (static_cast<UINT>(_width) + 7) / 8,
               (static_cast<UINT>(_height) + 7) / 8, rc);
}

// ---------------------------------------------------------------------------
// NVOF guidance(PORTING 清单 #6)
// ---------------------------------------------------------------------------

bool D3D12Context::BindNvofResources(ID3D12Resource *flowFwd, ID3D12Resource *flowBwd,
                                     ID3D12Resource *costFwd, ID3D12Resource *costBwd) noexcept {
    // 每个槽的描述符堆各写一份(densify 在槽列表上执行)。未启用的 cost/
    // backward 槽位用 flowFwd 的视图占位 —— densify 着色器按 cbuffer 旗标
    // 跳过读取,占位永不被有效采样。绝不创建 NULL 描述符(本机驱动上
    // 会异步 TDR,见 CreateSlotResources 注释)。
    if (!_device || !flowFwd) return false;
    ID3D12Resource *views[4]{ flowFwd,
                              flowBwd ? flowBwd : flowFwd,
                              costFwd ? costFwd : flowFwd,
                              costBwd ? costBwd : flowFwd };
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (int i = 0; i < kSlotCount; ++i) {
        if (!_slots[i].srvUavHeap) return false;
        const D3D12_CPU_DESCRIPTOR_HANDLE slotBase =
            _slots[i].srvUavHeap->GetCPUDescriptorHandleForHeapStart();
        auto h = [&](UINT d) {
            return D3D12_CPU_DESCRIPTOR_HANDLE{ slotBase.ptr + static_cast<SIZE_T>(d * inc) };
        };
        _device->CreateShaderResourceView(views[0], nullptr, h(14));
        _device->CreateShaderResourceView(views[1], nullptr, h(15));
        _device->CreateShaderResourceView(views[2], nullptr, h(16));
        _device->CreateShaderResourceView(views[3], nullptr, h(17));
    }
    return true;
}

void D3D12Context::RecordDensify(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                 uint32_t flowW, uint32_t flowH, uint32_t gridSize,
                                 bool hasForwardCost, bool hasBackward,
                                 bool hasBackwardCost) noexcept {
    // 在 NVOF 会话的 nvof CL 上执行(门内、execute 完成后)。NGX evaluate
    // 可能重绑堆/根签名;槽列表上的其它 pass 仍各自先重绑(同款)。
    ID3D12GraphicsCommandList *cl = &clRef;
    cl->SetComputeRootSignature(_rsDensify.Get());
    cl->SetPipelineState(_psoDensify.Get());
    ID3D12DescriptorHeap *heaps[]{ slot.srvUavHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = slot.srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(i * inc) }; };

    const UINT srcWH[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const UINT flowWH[2]{ flowW, flowH };
    cl->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    cl->SetComputeRoot32BitConstants(0, 2, flowWH, 2);
    const UINT flags[4]{ gridSize, hasForwardCost ? 1u : 0u,
                         hasBackward ? 1u : 0u, hasBackwardCost ? 1u : 0u };
    cl->SetComputeRoot32BitConstants(0, 4, flags, 4);

    cl->SetComputeRootDescriptorTable(1, gpu(14)); // t0 ForwardFlow
    cl->SetComputeRootDescriptorTable(2, gpu(15)); // t1 BackwardFlow
    cl->SetComputeRootDescriptorTable(3, gpu(16)); // t2 ForwardCost
    cl->SetComputeRootDescriptorTable(4, gpu(17)); // t3 BackwardCost
    cl->SetComputeRootDescriptorTable(5, gpu(12)); // u0 DenseMotion(uavMotion)
    cl->SetComputeRootDescriptorTable(6, gpu(13)); // u1 DenseConfidence(uavConfidence)
    cl->Dispatch((static_cast<UINT>(_width) + 7) / 8,
                 (static_cast<UINT>(_height) + 7) / 8, 1);
}

void D3D12Context::RecordClearGuidance(ID3D12GraphicsCommandList &clRef, FrameSlot &slot) noexcept {
    // 播种/失败/过期帧:发布零运动(与零 guidance 的旧行为等价)。
    ID3D12GraphicsCommandList *cl = &clRef;
    cl->SetComputeRootSignature(_rsDensify.Get());
    ID3D12DescriptorHeap *heaps[]{ slot.srvUavHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = slot.srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE cpuBase = slot.srvUavHeap->GetCPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto clear = [&](UINT desc, ID3D12Resource *res) {
        static constexpr UINT kZero[4]{};
        cl->ClearUnorderedAccessViewUint(
            D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(desc * inc) },
            D3D12_CPU_DESCRIPTOR_HANDLE{ cpuBase.ptr + static_cast<SIZE_T>(desc * inc) },
            res, kZero, 0, nullptr);
    };
    clear(12, slot.motion.Get());
    clear(13, slot.confidence.Get());
}

void D3D12Context::RecordGuidancePass(FrameSlot &slot, ID3D12PipelineState *pso,
                                      UINT srv0, UINT srv1, UINT uav0, UINT uav1,
                                      UINT dispatchX, UINT dispatchY) noexcept {
    // guidance 降采样:12 常量 cbuffer(与残差管线同布局),MotionScale =
    // 内部/源(逐轴),运动向量换算到内部像素单位(Magpie 同款,
    // PARAM_MVEC_SCALE 保持 1)。
    ID3D12GraphicsCommandList *cl = slot.commandList.Get();
    cl->SetComputeRootSignature(_rsGuidance.Get());
    cl->SetPipelineState(pso);
    ID3D12DescriptorHeap *heaps[]{ slot.srvUavHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = slot.srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(i * inc) }; };

    const UINT srcWH[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const UINT dstWH[2]{ static_cast<UINT>(_internalWidth), static_cast<UINT>(_internalHeight) };
    cl->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    cl->SetComputeRoot32BitConstants(0, 2, dstWH, 2);
    const float zero = 0.0f;
    cl->SetComputeRoot32BitConstants(0, 1, &zero, 4); // Padding0
    const float motionScale[2]{
        static_cast<float>(_internalWidth) / static_cast<float>(_width),
        static_cast<float>(_internalHeight) / static_cast<float>(_height)
    };
    cl->SetComputeRoot32BitConstants(0, 2, motionScale, 5);

    cl->SetComputeRootDescriptorTable(1, gpu(srv0));
    cl->SetComputeRootDescriptorTable(2, gpu(srv1));
    cl->SetComputeRootDescriptorTable(3, gpu(uav0));
    cl->SetComputeRootDescriptorTable(4, gpu(uav1));
    cl->Dispatch(dispatchX, dispatchY, 1);
}

void D3D12Context::RecordGuidanceDownsample(FrameSlot &slot) noexcept {
    RecordGuidancePass(slot, _psoGuidanceDownsample.Get(),
                       10, 11,          // t0 motion, t1 confidence
                       20, 21,          // u0 reducedMotion, u1 reducedConfidence
                       (static_cast<UINT>(_internalWidth) + 7) / 8,
                       (static_cast<UINT>(_internalHeight) + 7) / 8);
}

} // namespace vsdlssnr
