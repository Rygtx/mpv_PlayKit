#include "d3d12_context.h"
#include "dlssnr_context.h" // TimingStatusLine(临时探针)
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

// YUV↔RGB 转换系数(按位深/矩阵/范围推导,root constants 下发)。全程
// 归一域 [0,1](Y)/[-0.5,0.5](C);shader 端与 CPU 参考实现共用同一组
// 公式。10-bit 的有限范围常量 = 8-bit ×4(16→64 等)。
// containerMax = UNORM 容器满值(255/65535):round(UNORM读出×containerMax)
// 精确还原整数采样字;sampleMax = 采样值域上限(255/1023)。
struct YuvCoeffs {
    float containerMax;
    float sampleMax;
    float yLo, ySpan;   // limited: 16/219(8bit) 64/876(10bit);full: 0/sampleMax
    float cMid, cSpan;  // limited: 128/224 512/896;full: sampleMax/2
    float kr, kb;       // 709: 0.2126/0.0722;601: 0.299/0.114
};

YuvCoeffs YuvCoeffsFor(ColorMatrix matrix, ColorRange range, int depth) noexcept {
    YuvCoeffs c{};
    c.containerMax = depth > 8 ? 65535.0f : 255.0f;
    c.sampleMax = depth > 8 ? 1023.0f : 255.0f;
    const float shift = depth > 8 ? 4.0f : 1.0f;
    if (range == ColorRange::Limited) {
        c.yLo = 16.0f * shift;
        c.ySpan = 219.0f * shift;
        c.cMid = 128.0f * shift;
        c.cSpan = 224.0f * shift;
    } else {
        c.yLo = 0.0f;
        c.ySpan = c.sampleMax;
        c.cMid = c.sampleMax * 0.5f;
        c.cSpan = c.sampleMax * 0.5f;
    }
    c.kr = matrix == ColorMatrix::BT709 ? 0.2126f : 0.299f;
    c.kb = matrix == ColorMatrix::BT709 ? 0.0722f : 0.114f;
    return c;
}

} // namespace

D3D12Context::~D3D12Context() { Finalize(); }

void D3D12Context::NotifyFrameTick(double qpcSeconds) noexcept {
    std::lock_guard<std::mutex> lock(_tickMutex);
    _tickRing[_tickHead] = qpcSeconds;
    _tickHead = (_tickHead + 1) % kTickRingCap;
    if (_tickCount < kTickRingCap) ++_tickCount;
}

double D3D12Context::FrameRateWindow() noexcept {
    std::lock_guard<std::mutex> lock(_tickMutex);
    if (_tickCount == 0) return 0.0;
    // now 取最后写入的 tick:读路径不需要 QPC。停顿期间无人发布统计,面板
    // 本来就冻结;恢复后第一帧的发布会以新 now 淘汰窗口外的旧条目。
    const double now = _tickRing[(_tickHead + kTickRingCap - 1) % kTickRingCap];
    const double since = now - 1.0;
    int count = 0;
    for (int i = 0; i < _tickCount; ++i) {
        if (_tickRing[i] >= since) ++count;
    }
    return static_cast<double>(count); // 固定 1s 窗:窗口内帧数即 fps
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
    // 计算 PSO 一次性构建(残差 + NVOF densify/guidance)。densify 不能等
    // scaling 路径才创建:OF 会话在 scaling_enabled=0 时同样要录 densify
    // (实测 2026-09-07:耦合在 CreateScalingForSlot 里导致 null PSO 崩溃)。
    return CreateComputeObjects(err, errLen);
}

void D3D12Context::Finalize() noexcept {
    // 探针:D3D12 上下文销毁只发生在 parked 上下文排干/非热路径释放。
    if (_device) TimingStatusLine("DLSSNR STATUS: d3d12 context finalized");
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
        for (int p = 0; p < 3; ++p) {
            if (s.uploadYuv[p] && s.uploadYuvMapped[p]) s.uploadYuv[p]->Unmap(0, nullptr);
            if (s.readbackYuv[p] && s.readbackYuvMapped[p]) s.readbackYuv[p]->Unmap(0, nullptr);
            s.uploadYuvMapped[p] = nullptr;
            s.readbackYuvMapped[p] = nullptr;
            s.uploadYuv[p].Reset();
            s.readbackYuv[p].Reset();
            s.yuvIn[p].Reset();
            s.yuvOut[p].Reset();
        }
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
        LARGE_INTEGER w0{}, w1{}, wf{};
        QueryPerformanceCounter(&w0);
        // 10s timeout: a device removal would never signal -> don't wait forever
        const DWORD wr = WaitForSingleObject(event, 10000);
        QueryPerformanceCounter(&w1);
        QueryPerformanceFrequency(&wf);
        if (wr != WAIT_OBJECT_0) {
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
            // 项目惯例:GPU 级失败必须进 timing log —— 之前只上面板+DebugView,
            // 跨进程观测时(面板没开)日志完全静默,无法定位。
            TimingStatusLine(buf);
            _deviceLost.store(true, std::memory_order_relaxed);
            OutputDebugStringA("vs_dlssnr: ");
            OutputDebugStringA(buf);
            OutputDebugStringA("\n");
            SetErr(err, errLen, E_FAIL, buf);
            return false;
        }
        // 探针:恢复型 GPU 停顿(TDR 恢复/驱动内部同步/着色器首次编译)
        // 不超时、不报错,此前完全不可见。>200ms 的"成功"等待同样是异常。
        const double waitMs = static_cast<double>(w1.QuadPart - w0.QuadPart) * 1000.0 /
                              static_cast<double>(wf.QuadPart);
        if (waitMs > 200.0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "DLSSNR STATUS: fence wait slow=%.0fms value=%llu",
                     waitMs, static_cast<unsigned long long>(value));
            TimingStatusLine(buf);
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
    // 探针:排空等待 = recreate 的顿挫主体(#32 的"切档帧一次性顿挫")。
    // 每次切预设/滑块一行,量化换挡停顿;若在无用户操作时出现 = 有东西在
    // 反复触发重建。
    LARGE_INTEGER t0{}, t1{}, tf{};
    QueryPerformanceCounter(&t0);
    _lock = std::unique_lock<std::mutex>(_ctx->_poolMutex);
    _ctx->_poolCv.wait(_lock, [&ctx] { return ctx._freeCount == kSlotCount; });
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&tf);
    const double waitMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                          static_cast<double>(tf.QuadPart);
    if (waitMs > 20.0) {
        char buf[96];
        snprintf(buf, sizeof(buf), "DLSSNR STATUS: pool drain wait=%.0fms (recreate stall)", waitMs);
        TimingStatusLine(buf);
    }
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

bool D3D12Context::CreateFrameResources(int width, int height, int depth, bool fg,
                                        char *err, size_t errLen) noexcept {
    _width = width;
    _height = height;
    _bitDepth = depth;
    _chromaW = (width + 1) >> 1;
    _chromaH = (height + 1) >> 1;
    _fgSlots = fg;

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

    if (ProbeEnabled()) TimingStatusLine("PROBE: d3d12 frame-res before clear"); // init 细分(DEVICE_HUNG 时序定位)
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

    if (ProbeEnabled()) TimingStatusLine("PROBE: d3d12 frame-res before slot loop");
    for (int i = 0; i < kSlotCount; ++i) {
        if (ProbeEnabled()) {
            char pb[48];
            snprintf(pb, sizeof(pb), "PROBE: d3d12 slot %d begin", i);
            TimingStatusLine(pb);
        }
        if (!CreateSlotResources(_slots[i], depth, err, errLen)) return false;
        if (ProbeEnabled()) {
            char pb[48];
            snprintf(pb, sizeof(pb), "PROBE: d3d12 slot %d done", i);
            TimingStatusLine(pb);
        }
    }
    if (ProbeEnabled()) TimingStatusLine("PROBE: d3d12 frame-res done");
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

bool D3D12Context::CreateSlotResources(FrameSlot &slot, int depth, char *err, size_t errLen) noexcept {
    // Re-entrant on the hot path (a resolution change reuses the warm
    // context): release the previous generation of slot resources first.
    // Safe because the caller holds a PoolHold — every slot is idle and its
    // GPU work has completed (slots are only released after WaitFrame).
    if (slot.fenceEvent) {
        CloseHandle(slot.fenceEvent);
        slot.fenceEvent = nullptr;
    }
    // YUV 原生:三对 persist-mapped buffer(Y 全分辨率 + U/V 半分辨率)。
    for (int i = 0; i < 3; ++i) {
        if (slot.uploadYuv[i] && slot.uploadYuvMapped[i]) slot.uploadYuv[i]->Unmap(0, nullptr);
        if (slot.readbackYuv[i] && slot.readbackYuvMapped[i]) slot.readbackYuv[i]->Unmap(0, nullptr);
        slot.uploadYuvMapped[i] = nullptr;
        slot.readbackYuvMapped[i] = nullptr;
    }
    for (int g = 0; g < kFgGenSlots; ++g) {
        for (int i = 0; i < 3; ++i) {
            if (slot.readbackFg[g][i] && slot.readbackFgMapped[g][i])
                slot.readbackFg[g][i]->Unmap(0, nullptr);
            slot.readbackFgMapped[g][i] = nullptr;
        }
    }
    slot.commandList.Reset();
    slot.allocator.Reset();
    for (int i = 0; i < 3; ++i) {
        slot.uploadYuv[i].Reset();
        slot.readbackYuv[i].Reset();
        slot.yuvIn[i].Reset();
        slot.yuvOut[i].Reset();
    }
    for (int g = 0; g < kFgGenSlots; ++g) {
        for (int i = 0; i < 3; ++i) slot.readbackFg[g][i].Reset();
    }
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
    slot.fgInterp.Reset();
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
    // (ABGR8),NGX DLSSNR 的参考宿主也是 BGRA。ALLOW_UNORDERED_ACCESS:
    // YUV→RGB 转换 dispatch UAV 直写(NGX 对 UAV-flag 纹理做 SRV 读有
    // reducedColor/motion 生产前科)。
    if (!CreateColorTexture(slot.inputColor.GetAddressOf(), width, height,
                            DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
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

    // YUV 平面纹理:In = upload 拷入供转换采样;Out = RGB→YUV dispatch 写出
    // 供回读。R8_UNORM(8bit)/R16_UNORM(10bit,存储字 = VS P10 采样值,
    // 右对齐 0-1023 —— 2026-09-08 BlankClip 实测;UNORM 读出 word/65535,
    // round(×65535) 精确还原整数采样值)。
    const DXGI_FORMAT yuvFmt = _bitDepth > 8 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    for (int i = 0; i < 3; ++i) {
        const int pw = i == 0 ? width : _chromaW;
        const int ph = i == 0 ? height : _chromaH;
        if (!CreateColorTexture(slot.yuvIn[i].GetAddressOf(), pw, ph,
                                yuvFmt, D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_FLAG_NONE, err, errLen)) {
            return false;
        }
        if (!CreateColorTexture(slot.yuvOut[i].GetAddressOf(), pw, ph,
                                yuvFmt, D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
            return false;
        }
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

    // YUV upload/readback staging:每平面一条 buffer,行距 256 对齐,
    // persist-mapped(纯行拷贝,无像素算术)。
    const UINT planeBytes = _bitDepth > 8 ? 2 : 1;
    for (int i = 0; i < 3; ++i) {
        const int pw = i == 0 ? width : _chromaW;
        const int ph = i == 0 ? height : _chromaH;
        const UINT bytesPerRow = static_cast<UINT>(pw) * planeBytes;
        if (!CreateRawBuffer(static_cast<UINT64>(ph) * bytesPerRow, D3D12_HEAP_TYPE_UPLOAD,
                             D3D12_RESOURCE_STATE_GENERIC_READ,
                             slot.uploadYuv[i].GetAddressOf(), slot.uploadPitchYuv[i],
                             bytesPerRow, err, errLen)) {
            return false;
        }
        hr = slot.uploadYuv[i]->Map(0, nullptr, &slot.uploadYuvMapped[i]);
        if (FAILED(hr)) {
            SetErr(err, errLen, hr, "Map(uploadYuv) failed");
            return false;
        }
        // READBACK heap only accepts buffers on this runtime (texture staging
        // on READBACK heap returns E_INVALIDARG), so copy via placed footprint
        // into a readback buffer. Persist-mapped like the upload side.
        if (!CreateRawBuffer(static_cast<UINT64>(ph) * bytesPerRow, D3D12_HEAP_TYPE_READBACK,
                             D3D12_RESOURCE_STATE_COPY_DEST,
                             slot.readbackYuv[i].GetAddressOf(), slot.readbackPitchYuv[i],
                             bytesPerRow, err, errLen)) {
            return false;
        }
        hr = slot.readbackYuv[i]->Map(0, nullptr, &slot.readbackYuvMapped[i]);
        if (FAILED(hr)) {
            SetErr(err, errLen, hr, "Map(readbackYuv) failed");
            return false;
        }
    }
    // FG 槽纹理/回读缓冲必须在描述符块之前创建:槽 32/33 的视图以
    // fgInterp 为目标 —— 纹理不存在时 CreateShaderResourceView(nullptr)
    // = NULL 描述符,本机驱动异步 TDR(见 14-17 注释;2026-09-11 fg 路径
    // 首测实锤,失败在下一个 CreateCommittedResource 才浮出)。
    if (_fgSlots && !CreateFgSlotResources(slot, err, errLen)) return false;
    {
        // slot-local shader-visible descriptor heap; input/output descriptors
        // here, the residual pipeline descriptors on every scaling rebuild.
        // 34 = 0..31 原有 + 32/33(FG 插值输出的 SRV/UAV;堆空间常备,
        // 非 FG 槽写占位视图)。
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 34;
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
        // 22:outputColor 的 SRV(RGB→YUV 转换读;原 srvNvofSrc 随 nvofSrcTex
        // 删除让位 —— 降采样直接采样槽 0 srvInput)。
        _device->CreateShaderResourceView(slot.outputColor.Get(), nullptr, slotHandle(22));
        // 23/24:NVOF 注册输入纹理的 UAV 占位(outputColor 带 UAV flag,
        // 不 NULL);BindNvofResources 在会话建立时覆盖为真值。降采样只在
        // 会话存活时被记录,占位永不被有效写入。
        _device->CreateUnorderedAccessView(slot.outputColor.Get(), nullptr, nullptr, slotHandle(23));
        _device->CreateUnorderedAccessView(slot.outputColor.Get(), nullptr, nullptr, slotHandle(24));
        // 25-27:yuvIn 的 SRV(YUV→RGB 转换采样);28-30:yuvOut 的 UAV
        // (RGB→YUV 写出);31:inputColor 的 UAV(YUV→RGB 直写)。
        for (int i = 0; i < 3; ++i) {
            _device->CreateShaderResourceView(slot.yuvIn[i].Get(), nullptr, slotHandle(25 + i));
            _device->CreateUnorderedAccessView(slot.yuvOut[i].Get(), nullptr, nullptr, slotHandle(28 + i));
        }
        _device->CreateUnorderedAccessView(slot.inputColor.Get(), nullptr, nullptr, slotHandle(31));
        // 32/33:FG 插值输出的 SRV/UAV(fgInterp 已在上面创建 —— 顺序是
        // 硬约束,见描述符块前的 NULL 描述符 TDR 注释)。非 FG 槽 =
        // outputColor 占位视图(占位永不被有效读取:FG 第二遍转换仅在
        // _fg && realMotion 帧被记录)。
        if (_fgSlots) {
            _device->CreateShaderResourceView(slot.fgInterp.Get(), nullptr, slotHandle(32));
            _device->CreateUnorderedAccessView(slot.fgInterp.Get(), nullptr, nullptr, slotHandle(33));
        } else {
            _device->CreateShaderResourceView(slot.outputColor.Get(), nullptr, slotHandle(32));
            _device->CreateUnorderedAccessView(slot.outputColor.Get(), nullptr, nullptr, slotHandle(33));
        }
    }
    return true;
}

// DLSS FG 槽资源(仅 _fgSlots):插值输出纹理 + 第二组回读缓冲。
bool D3D12Context::CreateFgSlotResources(FrameSlot &slot, char *err, size_t errLen) noexcept {
    if (!CreateColorTexture(slot.fgInterp.GetAddressOf(), _width, _height,
                            DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    const UINT planeBytes = _bitDepth > 8 ? 2 : 1;
    for (int g = 0; g < kFgGenSlots; ++g) {
        for (int i = 0; i < 3; ++i) {
            const int pw = i == 0 ? _width : _chromaW;
            const int ph = i == 0 ? _height : _chromaH;
            const UINT bytesPerRow = static_cast<UINT>(pw) * planeBytes;
            if (!CreateRawBuffer(static_cast<UINT64>(ph) * bytesPerRow, D3D12_HEAP_TYPE_READBACK,
                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                 slot.readbackFg[g][i].GetAddressOf(), slot.readbackPitchFg[g][i],
                                 bytesPerRow, err, errLen)) {
                return false;
            }
            const HRESULT hr = slot.readbackFg[g][i]->Map(0, nullptr, &slot.readbackFgMapped[g][i]);
            if (FAILED(hr)) {
                SetErr(err, errLen, hr, "Map(readbackFg) failed");
                return false;
            }
        }
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
    // YUV 原生:纯行拷贝,零像素算术(YUV→RGB 在 GPU 转换 pass)。
    // [0]=Y 全分辨率,[1]/[2]=U/V 半分辨率;行距 256 对齐,VS stride 对齐。
    for (int p = 0; p < 3; ++p) {
        const int pw = p == 0 ? width : _chromaW;
        const int ph = p == 0 ? height : _chromaH;
        const size_t rowBytes = static_cast<size_t>(pw) * (_bitDepth > 8 ? 2u : 1u);
        const uint8_t *srcRow = srcPlanes[p];
        uint8_t *dstRow = static_cast<uint8_t *>(slot.uploadYuvMapped[p]);
        for (int y = 0; y < ph; ++y, srcRow += srcStrides[p], dstRow += slot.uploadPitchYuv[p]) {
            memcpy(dstRow, srcRow, rowBytes);
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

// cbuffer ConvertInParams(root constants,三处同步铁律:HLSL cbuffer /
// Num32BitValues=10 / SetComputeRoot32BitConstants):
// @0-1 uint2 DstExtent(inputColor 尺寸,dispatch 边界)
// @2 containerMax @3 yLo @4 yScale(1/ySpan) @5 cMid @6 cScale(1/cSpan)
// @7 kr @8 kb @9 pad
void D3D12Context::RecordConvertInput(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                      ColorMatrix matrix, ColorRange range,
                                      D3D12_RESOURCE_STATES stateAfter) noexcept {
    // cl 由调用方给定(nvof CL 或槽 CL)—— 见头文件注释,录错列表 = 被销毁。
    ID3D12GraphicsCommandList *cl = &clRef;
    const DXGI_FORMAT yuvFmt = _bitDepth > 8 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    const int planeW[3]{ _width, _chromaW, _chromaW };
    const int planeH[3]{ _height, _chromaH, _chromaH };

    // 1) yuvUpload → yuvIn ×3(footprint,同格式)。
    D3D12_RESOURCE_BARRIER toCopyDest[3];
    for (int i = 0; i < 3; ++i) {
        toCopyDest[i] = Transition(slot.yuvIn[i].Get(),
                                   D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    cl->ResourceBarrier(3, toCopyDest);
    for (int i = 0; i < 3; ++i) {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = slot.uploadYuv[i].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 0;
        src.PlacedFootprint.Footprint.Format = yuvFmt;
        src.PlacedFootprint.Footprint.Width = static_cast<UINT>(planeW[i]);
        src.PlacedFootprint.Footprint.Height = static_cast<UINT>(planeH[i]);
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(slot.uploadPitchYuv[i]);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = slot.yuvIn[i].Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    D3D12_RESOURCE_BARRIER toNsr[3];
    for (int i = 0; i < 3; ++i) {
        toNsr[i] = Transition(slot.yuvIn[i].Get(),
                              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    cl->ResourceBarrier(3, toNsr);

    // 2) 转换 dispatch:采样 Y/U/V(色度双线性上采)→ UAV 直写 inputColor。
    D3D12_RESOURCE_BARRIER toUav[1]{
        Transition(slot.inputColor.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cl->ResourceBarrier(1, toUav);
    const YuvCoeffs cf = YuvCoeffsFor(matrix, range, _bitDepth);
    cl->SetComputeRootSignature(_rsConvertIn.Get());
    cl->SetPipelineState(_psoConvertIn.Get());
    ID3D12DescriptorHeap *heaps[]{ slot.srvUavHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = slot.srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(i * inc) }; };
    const UINT extent[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const float consts[8]{ cf.containerMax, cf.yLo, 1.0f / cf.ySpan, cf.cMid,
                           1.0f / cf.cSpan, cf.kr, cf.kb, 0.0f };
    cl->SetComputeRoot32BitConstants(0, 2, extent, 0);
    cl->SetComputeRoot32BitConstants(0, 8, consts, 2);
    cl->SetComputeRootDescriptorTable(1, gpu(25)); // t0 PlaneY
    cl->SetComputeRootDescriptorTable(2, gpu(26)); // t1 PlaneU
    cl->SetComputeRootDescriptorTable(3, gpu(27)); // t2 PlaneV
    cl->SetComputeRootDescriptorTable(4, gpu(31)); // u0 inputColor
    cl->Dispatch((static_cast<UINT>(_width) + 7) / 8, (static_cast<UINT>(_height) + 7) / 8, 1);

    // 3) inputColor → stateAfter(NSR=NGX 待读;COMMON=skipEval);
    //    yuvIn 归位 COMMON(帧末全 COMMON 不变量,下一帧重新 COPY_DEST)。
    D3D12_RESOURCE_BARRIER outBar[1]{
        Transition(slot.inputColor.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, stateAfter),
    };
    cl->ResourceBarrier(1, outBar);
    D3D12_RESOURCE_BARRIER inBack[3];
    for (int i = 0; i < 3; ++i) {
        inBack[i] = Transition(slot.yuvIn[i].Get(),
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    cl->ResourceBarrier(3, inBack);
}

bool D3D12Context::RecordReadbackCopy(FrameSlot &slot, char *err, size_t errLen,
                                      int fgGen) noexcept {
    // RecordYuvOutput 之后调用:yuvOut 处于 UAV 态 → COPY_SOURCE → 拷贝
    // → COMMON。三平面各自 footprint(格式同资源,跨格式 = 静默 E_INVALIDARG)。
    // fgGen >= 0 = 拷入 FG 插值帧第 fgGen 组回读缓冲(同一条 CL 上真实帧与
    // 各插值槽先后转换+回读,共用 yuvOut,目标缓冲两两不同)。
    ID3D12GraphicsCommandList *cl = slot.commandList.Get();
    const DXGI_FORMAT yuvFmt = _bitDepth > 8 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    const int planeW[3]{ _width, _chromaW, _chromaW };
    const int planeH[3]{ _height, _chromaH, _chromaH };
    for (int i = 0; i < 3; ++i) {
        ID3D12Resource *dstBuf = fgGen >= 0 ? slot.readbackFg[fgGen][i].Get()
                                            : slot.readbackYuv[i].Get();
        const size_t dstPitch = fgGen >= 0 ? slot.readbackPitchFg[fgGen][i]
                                           : slot.readbackPitchYuv[i];
        D3D12_RESOURCE_BARRIER toCopySrc[1]{
            Transition(slot.yuvOut[i].Get(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        cl->ResourceBarrier(1, toCopySrc);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = slot.yuvOut[i].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = dstBuf;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = yuvFmt;
        dst.PlacedFootprint.Footprint.Width = static_cast<UINT>(planeW[i]);
        dst.PlacedFootprint.Footprint.Height = static_cast<UINT>(planeH[i]);
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(dstPitch);
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER backToCommon[1]{
            Transition(slot.yuvOut[i].Get(),
                       D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
        };
        cl->ResourceBarrier(1, backToCommon);
    }
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
    // 探针:提交互斥等待(3 槽并发提交在此串行)。>50ms = 提交路径拥塞,
    // 与 GPU 执行慢(gpu 段)分家。
    LARGE_INTEGER s0{}, s1{}, sf{};
    QueryPerformanceCounter(&s0);
    std::lock_guard<std::mutex> lock(_submitMutex);
    QueryPerformanceCounter(&s1);
    QueryPerformanceFrequency(&sf);
    {
        const double waitMs = static_cast<double>(s1.QuadPart - s0.QuadPart) * 1000.0 /
                              static_cast<double>(sf.QuadPart);
        if (waitMs > 50.0) {
            char buf[96];
            snprintf(buf, sizeof(buf), "DLSSNR STATUS: submit mutex wait=%.0fms (slots contending)", waitMs);
            TimingStatusLine(buf);
        }
    }
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
    int width, int height, char *err, size_t errLen,
    int fgGen) noexcept {
    if (width != _width || height != _height) {
        SetErr(err, errLen, E_INVALIDARG, "UnpackOutput: size mismatch");
        return false;
    }

    // YUV 原生:RGB→YUV 已在 GPU 完成,这里纯行拷贝回 VS 平面。
    // 10bit:yuvOut(R16_UNORM)存储字 = round(n*65535) = 10-bit 采样值
    // (shader 侧 w=n*1023/65535 精确缩放,见 BGRA_TO_YUV_HLSL),与 VS
    // P10 word 逐位一致 —— 所以是纯拷贝。
    // fgGen >= 0 = 读 FG 插值帧第 fgGen 组回读缓冲。
    for (int p = 0; p < 3; ++p) {
        const int pw = p == 0 ? width : _chromaW;
        const int ph = p == 0 ? height : _chromaH;
        const size_t rowBytes = static_cast<size_t>(pw) * (_bitDepth > 8 ? 2u : 1u);
        const uint8_t *srcRow = static_cast<const uint8_t *>(
            fgGen >= 0 ? slot.readbackFgMapped[fgGen][p] : slot.readbackYuvMapped[p]);
        const size_t srcPitch = fgGen >= 0 ? slot.readbackPitchFg[fgGen][p]
                                           : slot.readbackPitchYuv[p];
        uint8_t *dstRow = dstPlanes[p];
        for (int y = 0; y < ph; ++y, srcRow += srcPitch, dstRow += dstStrides[p]) {
            memcpy(dstRow, srcRow, rowBytes);
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
    uint2 SourceExtent;   // 稠密目标尺寸(源)
    uint2 FlowExtent;     // 流场网格尺寸(= 会话输入/GridSize)
    uint GridSize;
    uint HasForwardCost;
    uint HasBackward;
    uint HasBackwardCost;
    float2 MotionScale;   // 流向量单位换算(会话输入像素 → 源像素);未降采样 = (1,1)
};

float2 LoadFlow(Texture2D<int2> field, int2 p) {
    p = clamp(p, int2(0, 0), int2(FlowExtent) - 1);
    return float2(field.Load(int3(p, 0))) / 32.0;
}

float2 SampleFlow(Texture2D<int2> field, float2 sourcePixel) {
    // 源像素 → 流场网格坐标:流场均匀覆盖会话输入范围,会话输入被线性
    // 映射到源范围(MotionScale 同比),故按 FlowExtent/SourceExtent 比例
    // 采样;未降采样时该比例 = 1/GridSize(与旧 GridSize 公式一致)。
    float2 gridPos = sourcePixel * (float2(FlowExtent) / float2(SourceExtent)) - 0.5;
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
    float2 gridPos = sourcePixel * (float2(FlowExtent) / float2(SourceExtent)) - 0.5;
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
    // 流向量按 MotionScale 换算到源像素单位(下游 MotionScale/NGX 契约);
    // 未降采样时 (1,1),逐位等价旧公式。
    float2 forward = SampleFlow(ForwardFlow, p) * MotionScale;
    // Turing has no hardware cost output. A conservative non-zero baseline
    // still lets downstream consumers use coherent motion while reset frames
    // remain explicitly zero-confidence.
    float confidence = HasForwardCost != 0 ?
        1.0 - SampleCost(ForwardCost, p) : 0.65;

    if (HasBackward != 0) {
        float2 referencePixel = p + forward;
        bool inside = all(referencePixel >= 0.0) &&
            all(referencePixel < float2(SourceExtent));
        float2 backward = SampleFlow(BackwardFlow, referencePixel) * MotionScale;
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

// 光流输入 GPU 降采样(#46/#48):YUV→RGB 转换(同 CL 先行)已产出
// inputColor(NSR),本 shader 读它的 Texture2D SRV 双线性写到 NVOF 注册
// 输入纹理。核公式与被删除的 CPU PackNvofInput 逐式一致(f=(d+0.5)*s-0.5、
// (int) 截断、clamp、+1 邻域取 min);差异仅在量化顺序 —— CPU 对 RGBS 浮点
// lerp 后一次性
// 量化,GPU 对 PackInput 已量化的 0-255 值 lerp,权重和为 1 的线性映射
// 下两端差 ≤1 LSB,对光流输入(低频信号)无影响。仅缩窄方向
// (dstW<=srcW)使用,f 恒 >= 0。
constexpr char NVOF_DOWNSAMPLE_HLSL[] = R"(
Texture2D<float4> SourceColor : register(t0);  // BGRA8:SRV 返回逻辑 RGBA(通道无关于本 shader)
RWTexture2D<float4> DestColor : register(u0);

cbuffer NvofDownsampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
};

[numthreads(8, 8, 1)]
void NvofDownsample(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    const float sx = (float)SourceExtent.x / (float)TargetExtent.x;
    const float sy = (float)SourceExtent.y / (float)TargetExtent.y;

    const float fx = (tid.x + 0.5) * sx - 0.5;
    int x0 = (int)fx;
    const float wx = fx - (float)x0;
    x0 = clamp(x0, 0, (int)SourceExtent.x - 1);
    const int x1 = min(x0 + 1, (int)SourceExtent.x - 1);

    const float fy = (tid.y + 0.5) * sy - 0.5;
    int y0 = (int)fy;
    const float wy = fy - (float)y0;
    y0 = clamp(y0, 0, (int)SourceExtent.y - 1);
    const int y1 = min(y0 + 1, (int)SourceExtent.y - 1);

    // UNORM 采样值 *255 精确还原 0-255 整数域(量化语义与 CPU 对齐)。
    const float3 v00 = SourceColor.Load(int3(x0, y0, 0)).xyz * 255.0;
    const float3 v10 = SourceColor.Load(int3(x1, y0, 0)).xyz * 255.0;
    const float3 v01 = SourceColor.Load(int3(x0, y1, 0)).xyz * 255.0;
    const float3 v11 = SourceColor.Load(int3(x1, y1, 0)).xyz * 255.0;

    const float w00 = (1.0 - wx) * (1.0 - wy), w10 = wx * (1.0 - wy);
    const float w01 = (1.0 - wx) * wy, w11 = wx * wy;

    // xyz = B,G,R(与 BGRA8 分量序一致);round-half-up 后 /255 写回,
    // 舍入与 CPU(*255+0.5 截断)一致。alpha 恒 1。
    const float3 value = v00 * w00 + v10 * w10 + v01 * w01 + v11 * w11;
    DestColor[tid.xy] = float4(floor(value + 0.5) / 255.0, 1.0);
}
)";

// YUV→RGB(YUV 原生化):采样 Y/U/V 平面(色度双线性上采),按位深恢复
// 整数采样字、按范围展开,矩阵求逆得 RGB,UAV 直写 inputColor。深度/矩阵/
// 范围全在 root constants(C0/C1,由 YuvCoeffsFor 按 _bitDepth 推导)——
// R8/R16_UNORM 的 Texture2D<float> SRV 同构,单 PSO 通吃两深度。
// BGRA typed SRV 返回逻辑 RGBA(.xyz=R,G,B,Magpie r5 TODO §2.1 同款约定;
// 数值验收的 python 参考会当场抓 R/B 换位)。
constexpr char YUV_TO_BGRA_HLSL[] = R"(
Texture2D<float> PlaneY : register(t0);
Texture2D<float> PlaneU : register(t1);
Texture2D<float> PlaneV : register(t2);
RWTexture2D<float4> OutputColor : register(u0);

cbuffer ConvertInParams : register(b0) {
    uint2 DstExtent;      // inputColor 尺寸(dispatch 边界)
    float ContainerMax;   // 255 / 65535
    float YLo;            // limited 16/64;full 0
    float YScale;         // 1/219, 1/876, 1/255, 1/1023
    float CMid;           // limited 128/512;full sampleMax/2
    float CScale;         // 1/224, 1/896, 2/sampleMax
    float Kr;             // 0.2126(709) / 0.299(601)
    float Kb;             // 0.0722(709) / 0.114(601)
    float Pad0;
};

[numthreads(8, 8, 1)]
void ConvertYuvToBgra(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    // round(UNORM × containerMax) 精确还原整数采样字(8/10 位同构)。
    const float codeY = round(PlaneY[tid.xy].x * ContainerMax);
    const float nY = (codeY - YLo) * YScale;

    // 色度半分辨率 → 双线性上采(与原 zimg Bilinear 对齐;siting (0,0.5))。
    const int2 cExtent = (DstExtent + 1) >> 1;
    const float2 fc = (tid.xy + 0.5) * 0.5 - 0.5;
    const int2 c0 = int2(floor(fc));
    const float2 wf = fc - c0;
    const int2 s0 = clamp(c0, int2(0, 0), cExtent - 1);
    const int2 s1 = clamp(c0 + 1, int2(0, 0), cExtent - 1);
    const float w00 = (1.0 - wf.x) * (1.0 - wf.y), w10 = wf.x * (1.0 - wf.y);
    const float w01 = (1.0 - wf.x) * wf.y, w11 = wf.x * wf.y;
    const float codeU = round(PlaneU[s0].x * ContainerMax) * w00
                      + round(PlaneU[int2(s1.x, s0.y)].x * ContainerMax) * w10
                      + round(PlaneU[int2(s0.x, s1.y)].x * ContainerMax) * w01
                      + round(PlaneU[s1].x * ContainerMax) * w11;
    const float codeV = round(PlaneV[s0].x * ContainerMax) * w00
                      + round(PlaneV[int2(s1.x, s0.y)].x * ContainerMax) * w10
                      + round(PlaneV[int2(s0.x, s1.y)].x * ContainerMax) * w01
                      + round(PlaneV[s1].x * ContainerMax) * w11;
    const float nU = (codeU - CMid) * CScale;
    const float nV = (codeV - CMid) * CScale;

    // 矩阵求逆(Kg = 1-Kr-Kb):Cr 驱动 R、Cb 驱动 B,增益各配自己的
    // (1-Kx) —— R = Y + Cr·2(1-Kr) = Y + 1.5748·Cr(709)。
    const float Kg = 1.0 - Kr - Kb;
    const float r = nY + nV * 2.0 * (1.0 - Kr);
    const float b = nY + nU * 2.0 * (1.0 - Kb);
    const float g = (nY - Kr * r - Kb * b) / Kg;
    OutputColor[tid.xy] = float4(saturate(r), saturate(g), saturate(b), 1.0);
}
)";

// RGB→YUV(YUV 原生化):outputColor(SRV,逻辑 RGBA)→ luma 全分辨率 +
// chroma 半分辨率 2×2 box 两个入口。归一域 → 整数采样字 → 写 w =
// code/containerMax;UNORM 存储回读 = round(w×containerMax) = code,与
// VS 平面字(P8 byte / P10 word)逐位一致 → CPU 解包纯行拷贝。深度由
// LoOverCM/SpanOverCM 折叠(= yLo/CM 等,YuvCoeffsFor 推导)。
constexpr char BGRA_TO_YUV_HLSL[] = R"(
Texture2D<float4> CompositeColor : register(t0);  // BGRA8:SRV 返回逻辑 RGBA
RWTexture2D<float> OutputA : register(u0);  // luma dispatch 绑 yuvOut[0](Y)
RWTexture2D<float> OutputB : register(u1);  // chroma dispatch 绑 yuvOut[1](U)/[2](V)

cbuffer ConvertOutParams : register(b0) {
    uint2 DstExtent;      // luma: W,H;chroma: cw,ch(dispatch 边界)
    uint2 SourceExtent;   // 全分辨率尺寸(chroma 2×2 块 clamp)
    float Kr;
    float Kb;
    float LoOverCM;       // luma: yLo/CM;chroma: cMid/CM
    float SpanOverCM;     // luma: ySpan/CM;chroma: cSpan/CM
    float Pad0;
    float Pad1;
    float Pad2;
};

float LumaOf(float3 rgb) {
    return dot(rgb, float3(Kr, 1.0 - Kr - Kb, Kb));
}

// 色度写回:cb/cr ∈ [-0.5,0.5],必须先加半幅度偏移再 saturate —— saturate
// 在前会把全部负色度钳成中性 128(绿/青/蓝内容去饱和,2026-09-08 实测;
// 与 #30 负残差 UNORM clamp 同族的"负值先钳"坑)。
float ChromaToCode(float c) {
    return saturate(c * SpanOverCM + LoOverCM);
}

[numthreads(8, 8, 1)]
void BgraToYuvLuma(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    const float3 rgb = saturate(CompositeColor[tid.xy].xyz);
    const float nY = LumaOf(rgb);
    OutputA[tid.xy] = saturate(nY * SpanOverCM + LoOverCM);
}

[numthreads(8, 8, 1)]
void BgraToYuvChroma(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    // 2×2 box 平均:全程仿射,先平均后转换与逐点转换等价。
    const int2 base = tid.xy * 2;
    const int2 x1 = min(base + int2(1, 0), SourceExtent - 1);
    const int2 y1 = min(base + int2(0, 1), SourceExtent - 1);
    const int2 x1y1 = min(base + int2(1, 1), SourceExtent - 1);
    const float3 rgb = saturate(0.25 * (CompositeColor[base].xyz
                                      + CompositeColor[x1].xyz
                                      + CompositeColor[y1].xyz
                                      + CompositeColor[x1y1].xyz));
    const float nY = LumaOf(rgb);
    // cb = (B-Y)·0.5/(1-Kb),cr = (R-Y)·0.5/(1-Kr)
    const float cb = (rgb.z - nY) * (0.5 / (1.0 - Kb));
    const float cr = (rgb.x - nY) * (0.5 / (1.0 - Kr));
    OutputA[tid.xy] = ChromaToCode(cb);  // u0 → U 平面
    OutputB[tid.xy] = ChromaToCode(cr);  // u1 → V 平面
}
)";

} // namespace

bool D3D12Context::DumpTextureToFile(ID3D12Resource *tex, int width, int height,
                                     const wchar_t *path, DXGI_FORMAT format) noexcept {
    // 4 B/px covers the BGRA color dumps; the horizontal residual is
    // R16G16B16A16_FLOAT (8 B/px, signed range); R8/R16_UNORM (1/2 B/px)
    // are the YUV planes (YUV 原生化 dump 验收用)。
    UINT bpp = 4u;
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) bpp = 8u;
    else if (format == DXGI_FORMAT_R16_UNORM) bpp = 2u;
    else if (format == DXGI_FORMAT_R8_UNORM) bpp = 1u;
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
        params[0].Constants.Num32BitValues = 10; // srcWH(2)+flowWH(2)+flags(4)+MotionScale(2)
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
    // 光流输入 GPU 降采样(#46/#48):b0 5 常量 + t0 单表(inputColor 的
    // Texture2D SRV,槽 0)+ u0 单表(NVOF 注册输入纹理 UAV)。t0/u0 各自
    // 独立表参数 —— 同表重叠 range 禁忌见 densify 块注释。
    if (ProbeEnabled()) TimingStatusLine("PROBE: pso base ok");
    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;
        srvRange.OffsetInDescriptorsFromTableStart = 0;
        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;
        uavRange.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = 5; // srcWH(2)+dstWH(2)+pad(1)
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srvRange;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &uavRange;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 3;
        rsDesc.pParameters = params;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> rsBlob, rsErr;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                               rsBlob.GetAddressOf(), rsErr.GetAddressOf()))) {
            SetErr(err, errLen, E_FAIL, "SerializeRootSignature(nvof downsample) failed");
            return false;
        }
        if (FAILED(_device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                                rsBlob->GetBufferSize(), IID_PPV_ARGS(_rsNvofDownsample.GetAddressOf())))) {
            SetErr(err, errLen, E_FAIL, "CreateRootSignature(nvof downsample) failed");
            return false;
        }
        ComPtr<ID3DBlob> code, csErr;
        if (FAILED(D3DCompile(NVOF_DOWNSAMPLE_HLSL, strlen(NVOF_DOWNSAMPLE_HLSL),
                              nullptr, nullptr, nullptr, "NvofDownsample", "cs_5_0",
                              0, 0, code.GetAddressOf(), csErr.GetAddressOf()))) {
            SetErr(err, errLen, E_FAIL, csErr ? static_cast<const char *>(csErr->GetBufferPointer()) : "D3DCompile(nvof downsample) failed");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = _rsNvofDownsample.Get();
        psoDesc.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        if (FAILED(_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(_psoNvofDownsample.GetAddressOf())))) {
            SetErr(err, errLen, E_FAIL, "CreateComputePipelineState(nvof downsample) failed");
            return false;
        }
        if (ProbeEnabled()) TimingStatusLine("PROBE: pso nvofds done");
    }
    // YUV↔RGB 转换(YUV 原生化):convertIn = t0/t1/t2 三张 SRV 表(Y/U/V)
    // + u0 一张 UAV 表(inputColor)+ b0 10 常量;convertOut = t0 一张 SRV 表
    // (outputColor)+ u0/u1 两张 UAV 表(Y/U,V)+ b0 12 常量。深度/矩阵/
    // 范围全在常量里(R8/R16_UNORM 的 float 视图同构)—— 每 shader 单 PSO。
    {
        D3D12_DESCRIPTOR_RANGE srvRanges[3]{};
        for (UINT i = 0; i < 3; ++i) {
            srvRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srvRanges[i].NumDescriptors = 1;
            srvRanges[i].BaseShaderRegister = i;
            srvRanges[i].OffsetInDescriptorsFromTableStart = 0;
        }
        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;
        uavRange.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER params[5]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = 10; // extent(2)+C0(4)+C1(4)-1pad
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        for (UINT i = 0; i < 3; ++i) {
            params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
            params[1 + i].DescriptorTable.pDescriptorRanges = &srvRanges[i];
            params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[4].DescriptorTable.NumDescriptorRanges = 1;
        params[4].DescriptorTable.pDescriptorRanges = &uavRange;
        params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 5;
        rsDesc.pParameters = params;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> rsBlob, rsErr;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                               rsBlob.GetAddressOf(), rsErr.GetAddressOf()))) {
            SetErr(err, errLen, E_FAIL, "SerializeRootSignature(convert in) failed");
            return false;
        }
        if (FAILED(_device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                                rsBlob->GetBufferSize(), IID_PPV_ARGS(_rsConvertIn.GetAddressOf())))) {
            SetErr(err, errLen, E_FAIL, "CreateRootSignature(convert in) failed");
            return false;
        }
        ComPtr<ID3DBlob> code, csErr;
        if (FAILED(D3DCompile(YUV_TO_BGRA_HLSL, strlen(YUV_TO_BGRA_HLSL),
                              nullptr, nullptr, nullptr, "ConvertYuvToBgra", "cs_5_0",
                              0, 0, code.GetAddressOf(), csErr.GetAddressOf()))) {
            SetErr(err, errLen, E_FAIL, csErr ? static_cast<const char *>(csErr->GetBufferPointer()) : "D3DCompile(convert in) failed");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = _rsConvertIn.Get();
        psoDesc.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        if (FAILED(_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(_psoConvertIn.GetAddressOf())))) {
            SetErr(err, errLen, E_FAIL, "CreateComputePipelineState(convert in) failed");
            return false;
        }
    }
    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;
        srvRange.OffsetInDescriptorsFromTableStart = 0;
        D3D12_DESCRIPTOR_RANGE uavRanges[2]{};
        for (UINT i = 0; i < 2; ++i) {
            uavRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            uavRanges[i].NumDescriptors = 1;
            uavRanges[i].BaseShaderRegister = i;
            uavRanges[i].OffsetInDescriptorsFromTableStart = 0;
        }
        D3D12_ROOT_PARAMETER params[4]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = 12; // extent(2)+srcExtent(2)+系数(4)+pad(4)
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srvRange;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &uavRanges[0];
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable.NumDescriptorRanges = 1;
        params[3].DescriptorTable.pDescriptorRanges = &uavRanges[1];
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 4;
        rsDesc.pParameters = params;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> rsBlob, rsErr;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                               rsBlob.GetAddressOf(), rsErr.GetAddressOf()))) {
            SetErr(err, errLen, E_FAIL, "SerializeRootSignature(convert out) failed");
            return false;
        }
        if (FAILED(_device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                                rsBlob->GetBufferSize(), IID_PPV_ARGS(_rsConvertOut.GetAddressOf())))) {
            SetErr(err, errLen, E_FAIL, "CreateRootSignature(convert out) failed");
            return false;
        }
        struct OutCso { const char *entry; ID3D12PipelineState **pso; };
        const OutCso outCsos[]{
            { "BgraToYuvLuma", _psoConvertOutLuma.GetAddressOf() },
            { "BgraToYuvChroma", _psoConvertOutChroma.GetAddressOf() },
        };
        for (const auto &cso : outCsos) {
            ComPtr<ID3DBlob> code, csErr;
            if (FAILED(D3DCompile(BGRA_TO_YUV_HLSL, strlen(BGRA_TO_YUV_HLSL),
                                  nullptr, nullptr, nullptr, cso.entry, "cs_5_0",
                                  0, 0, code.GetAddressOf(), csErr.GetAddressOf()))) {
                SetErr(err, errLen, E_FAIL, csErr ? static_cast<const char *>(csErr->GetBufferPointer()) : "D3DCompile(convert out) failed");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
            psoDesc.pRootSignature = _rsConvertOut.Get();
            psoDesc.CS = { code->GetBufferPointer(), code->GetBufferSize() };
            if (FAILED(_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(cso.pso)))) {
                SetErr(err, errLen, E_FAIL, "CreateComputePipelineState(convert out) failed");
                return false;
            }
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
    if (!_rsCompute && !CreateComputeObjects(err, errLen)) return false; // 幂等保护(Initialize 已建)

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
                                     ID3D12Resource *costFwd, ID3D12Resource *costBwd,
                                     ID3D12Resource *inputFwd, ID3D12Resource *inputBwd) noexcept {
    // 每个槽的描述符堆各写一份(densify 在槽列表上执行)。未启用的 cost/
    // backward 槽位用 flowFwd 的视图占位 —— densify 着色器按 cbuffer 旗标
    // 跳过读取,占位永不被有效采样。inputFwd/inputBwd(23/24)为注册输入
    // 纹理的 UAV(GPU 光流输入降采样直写目标);null 时用本槽 outputColor
    // 占位(带 UAV flag)。绝不创建 NULL 描述符(本机驱动上会异步 TDR,
    // 见 CreateSlotResources 注释)。
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
        _device->CreateUnorderedAccessView(inputFwd ? inputFwd : _slots[i].outputColor.Get(),
                                           nullptr, nullptr, h(23));
        _device->CreateUnorderedAccessView(inputBwd ? inputBwd : _slots[i].outputColor.Get(),
                                           nullptr, nullptr, h(24));
    }
    return true;
}

void D3D12Context::RecordDensify(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                 uint32_t denseW, uint32_t denseH,
                                 uint32_t flowW, uint32_t flowH, uint32_t gridSize,
                                 bool hasForwardCost, bool hasBackward,
                                 bool hasBackwardCost,
                                 float motionScaleX, float motionScaleY,
                                 UINT uavMotion, UINT uavConfidence) noexcept {
    // 在 NVOF 会话的 nvof CL 上执行(门内、execute 完成后)。NGX evaluate
    // 可能重绑堆/根签名;槽列表上的其它 pass 仍各自先重绑(同款)。
    // shader 的 SourceExtent 语义 = 稠密目标尺寸:流场网格均匀覆盖会话输入
    // 范围,SourceExtent 即会话输入被线性映射到的目标域 —— 源尺寸管线(源,
    // MotionScale=会话→源)与 follow 内部管线(内部,= 会话尺寸,
    // MotionScale=(1,1))共用同一几何公式。
    ID3D12GraphicsCommandList *cl = &clRef;

    cl->SetComputeRootSignature(_rsDensify.Get());
    cl->SetPipelineState(_psoDensify.Get());
    ID3D12DescriptorHeap *heaps[]{ slot.srvUavHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = slot.srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(i * inc) }; };

    const UINT srcWH[2]{ static_cast<UINT>(denseW), static_cast<UINT>(denseH) };
    const UINT flowWH[2]{ flowW, flowH };
    cl->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    cl->SetComputeRoot32BitConstants(0, 2, flowWH, 2);
    const UINT flags[4]{ gridSize, hasForwardCost ? 1u : 0u,
                         hasBackward ? 1u : 0u, hasBackwardCost ? 1u : 0u };
    cl->SetComputeRoot32BitConstants(0, 4, flags, 4);
    const float motionScale[2]{ motionScaleX, motionScaleY };
    cl->SetComputeRoot32BitConstants(0, 2, motionScale, 8);

    cl->SetComputeRootDescriptorTable(1, gpu(14)); // t0 ForwardFlow
    cl->SetComputeRootDescriptorTable(2, gpu(15)); // t1 BackwardFlow
    cl->SetComputeRootDescriptorTable(3, gpu(16)); // t2 ForwardCost
    cl->SetComputeRootDescriptorTable(4, gpu(17)); // t3 BackwardCost
    cl->SetComputeRootDescriptorTable(5, gpu(uavMotion));      // u0 DenseMotion
    cl->SetComputeRootDescriptorTable(6, gpu(uavConfidence));  // u1 DenseConfidence
    cl->Dispatch((static_cast<UINT>(denseW) + 7) / 8,
                 (static_cast<UINT>(denseH) + 7) / 8, 1);
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

void D3D12Context::RecordNvofDownsample(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                        int dstW, int dstH, int inputIndex) noexcept {
    // 光流输入 GPU 降采样(#46/#48):在 NVOF 会话 nvof CL 第一次提交上
    // 执行,替代 follow 分支的 CopyTextureRegion。inputColor 由同 CL 上
    // 先行的 RecordConvertInput 产出(已是 NSR,直读,无屏障);注册纹理
    // 的 COMMON→UAV→COMMON 屏障由调用方(NvofContext)负责 —— 纹理归会话
    // 所有;copyFence 在 CL 完成后才 Signal,execute 的 inFence[0] 天然
    // 覆盖本 dispatch。NGX evaluate 可能重绑堆/根签名,与其它 pass 同款
    // 先重绑。
    ID3D12GraphicsCommandList *cl = &clRef;
    cl->SetComputeRootSignature(_rsNvofDownsample.Get());
    cl->SetPipelineState(_psoNvofDownsample.Get());
    ID3D12DescriptorHeap *heaps[]{ slot.srvUavHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = slot.srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(i * inc) }; };

    // cbuffer 布局(与 NVOF_DOWNSAMPLE_HLSL 同步,三处同步铁律):
    // srcWH@0 dstWH@2 pad@4。
    const UINT srcWH[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const UINT dstWH[2]{ static_cast<UINT>(dstW), static_cast<UINT>(dstH) };
    const UINT pad[1]{ 0u };
    cl->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    cl->SetComputeRoot32BitConstants(0, 2, dstWH, 2);
    cl->SetComputeRoot32BitConstants(0, 1, pad, 4);

    cl->SetComputeRootDescriptorTable(1, gpu(0));               // t0 inputColor(NSR)
    cl->SetComputeRootDescriptorTable(2, gpu(23 + inputIndex)); // u0 NvofInput[cur]
    cl->Dispatch((static_cast<UINT>(dstW) + 7) / 8,
                 (static_cast<UINT>(dstH) + 7) / 8, 1);
}

void D3D12Context::RecordYuvOutput(FrameSlot &slot, ColorMatrix matrix, ColorRange range,
                                   D3D12_RESOURCE_STATES outputStateBefore,
                                   ID3D12Resource *srcColor, UINT srcSrvIndex) noexcept {
    // RGB→YUV(YUV 原生化):srcColor(缺省 outputColor;FG 路径传
    // fgInterp)stateBefore(UAV 正常 / COMMON skipEval / NSR=FG 后)→ NSR
    // → luma + chroma 两个 dispatch 写 yuvOut(留 UAV 交
    // RecordReadbackCopy)→ srcColor 归 COMMON。constant 的 Lo/Span 按
    // 平面语义填充(luma 用 yLo/ySpan,chroma 用 cMid/cSpan)。
    if (!srcColor) {
        srcColor = slot.outputColor.Get();
        srcSrvIndex = kSrvOutputColor;
    }
    ID3D12GraphicsCommandList *cl = slot.commandList.Get();
    const YuvCoeffs cf = YuvCoeffsFor(matrix, range, _bitDepth);
    // FG 路径传 NSR(Evaluate 已消费 backbuffer):同态迁移不录屏障。
    if (outputStateBefore != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER toNsr[1]{
            Transition(srcColor, outputStateBefore,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        cl->ResourceBarrier(1, toNsr);
    }
    D3D12_RESOURCE_BARRIER toUav[3];
    for (int i = 0; i < 3; ++i) {
        toUav[i] = Transition(slot.yuvOut[i].Get(),
                              D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    cl->ResourceBarrier(3, toUav);

    cl->SetComputeRootSignature(_rsConvertOut.Get());
    cl->SetPipelineState(_psoConvertOutLuma.Get());
    ID3D12DescriptorHeap *heaps[]{ slot.srvUavHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = slot.srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(i * inc) }; };

    // luma:extent=W,H,Lo/Span = yLo/ySpan(÷CM 由调用侧折进常量)。
    {
        const UINT extent[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
        const UINT srcExtent[2]{ extent[0], extent[1] };
        const float consts[8]{ cf.kr, cf.kb,
                               cf.yLo / cf.containerMax, cf.ySpan / cf.containerMax,
                               0.0f, 0.0f, 0.0f, 0.0f };
        cl->SetComputeRoot32BitConstants(0, 2, extent, 0);
        cl->SetComputeRoot32BitConstants(0, 2, srcExtent, 2);
        cl->SetComputeRoot32BitConstants(0, 8, consts, 4);
        cl->SetComputeRootDescriptorTable(1, gpu(srcSrvIndex)); // t0 色源 SRV
        cl->SetComputeRootDescriptorTable(2, gpu(28)); // u0 yuvOut[0](Y)
        cl->SetComputeRootDescriptorTable(3, gpu(29)); // u1 绑定但本 pass 不写
        cl->Dispatch((static_cast<UINT>(_width) + 7) / 8, (static_cast<UINT>(_height) + 7) / 8, 1);
    }
    // chroma:extent=cw,ch,Lo/Span = cMid/cSpan;2×2 box 平均。u0=U,u1=V。
    cl->SetPipelineState(_psoConvertOutChroma.Get());
    {
        const UINT extent[2]{ static_cast<UINT>(_chromaW), static_cast<UINT>(_chromaH) };
        const UINT srcExtent[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
        const float consts[8]{ cf.kr, cf.kb,
                               cf.cMid / cf.containerMax, cf.cSpan / cf.containerMax,
                               0.0f, 0.0f, 0.0f, 0.0f };
        cl->SetComputeRoot32BitConstants(0, 2, extent, 0);
        cl->SetComputeRoot32BitConstants(0, 2, srcExtent, 2);
        cl->SetComputeRoot32BitConstants(0, 8, consts, 4);
        cl->SetComputeRootDescriptorTable(1, gpu(srcSrvIndex)); // t0 色源 SRV
        cl->SetComputeRootDescriptorTable(2, gpu(29)); // u0 yuvOut[1](U)
        cl->SetComputeRootDescriptorTable(3, gpu(30)); // u1 yuvOut[2](V)
        cl->Dispatch((static_cast<UINT>(_chromaW) + 7) / 8, (static_cast<UINT>(_chromaH) + 7) / 8, 1);
    }

    // 色源归位 COMMON(yuvOut 留 UAV,由 RecordReadbackCopy 收尾)。
    D3D12_RESOURCE_BARRIER back[1]{
        Transition(srcColor,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
    };
    cl->ResourceBarrier(1, back);
}

} // namespace vsdlssnr
