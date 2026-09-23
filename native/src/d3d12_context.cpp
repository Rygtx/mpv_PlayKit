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

void D3D12Context::DebugDumpInfoQueue(char *buf, size_t len) const noexcept {
    if (!buf || !len) return;
    buf[0] = '\0';
    if (!_infoQueue) {
        std::snprintf(buf, len, "(infoqueue off)");
        return;
    }
    // 只取 ERROR/CORRUPTION(warning 会淹没缓冲,如 [820] 无 clear value)。
    D3D12_INFO_QUEUE_FILTER filter{};
    D3D12_MESSAGE_SEVERITY deny[2] = { D3D12_MESSAGE_SEVERITY_INFO,
                                       D3D12_MESSAGE_SEVERITY_WARNING };
    filter.DenyList.NumSeverities = 2;
    filter.DenyList.pSeverityList = deny;
    _infoQueue->PushRetrievalFilter(&filter);
    size_t n = 0;
    const UINT64 count = _infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count && n + 2 < len; ++i) {
        SIZE_T size = 0;
        if (FAILED(_infoQueue->GetMessage(i, nullptr, &size)) || !size) continue;
        auto *msg = static_cast<D3D12_MESSAGE *>(std::malloc(size));
        if (!msg) break;
        if (SUCCEEDED(_infoQueue->GetMessage(i, msg, &size))) {
            n += static_cast<size_t>(std::snprintf(buf + n, len - n, "[%llu]%s",
                static_cast<unsigned long long>(msg->ID), msg->pDescription));
        }
        std::free(msg);
    }
    _infoQueue->ClearStoredMessages();
    _infoQueue->PopRetrievalFilter();
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
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(_adapter->GetDesc1(&desc))) _vendorId = desc.VendorId;
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
        if (s.baseFenceEvent) {
            CloseHandle(s.baseFenceEvent);
            s.baseFenceEvent = nullptr;
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
    // 与分段提交(SubmitBaseFrame/SubmitFgFrame)共享 _fence:fetch_add +
    // Signal 必须同锁,否则并发槽提交会让 Signal 值乱序(fence 值回退 =
    // 驱动未定义行为)。CtlMutex → _submitMutex 的加锁顺序与分段提交
    // (仅 _submitMutex)无环。
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
                                        int pipeW, int pipeH, int outW, int outH,
                                        bool vsr, bool hdr,
                                        char *err, size_t errLen) noexcept {
    _width = width;
    _height = height;
    _bitDepth = depth;
    _chromaW = (width + 1) >> 1;
    _chromaH = (height + 1) >> 1;
    _fgSlots = fg;
    // RTX Video 双尺寸定格。守卫:pipe/out 必须落在 [源, 合理上界] 内,
    // 越界 = 调用方换算 bug,按无 RTX 兜底(与占位视图语义一致)。
    _vsrSlots = vsr && pipeW >= width && pipeH >= height &&
                pipeW <= width * 8 && pipeH <= height * 8;
    _hdrPipe = hdr;
    if (_vsrSlots) {
        _pipeW = pipeW;
        _pipeH = pipeH;
        _outW = outW;
        _outH = outH;
    } else {
        _pipeW = width;
        _pipeH = height;
        _outW = width;
        _outH = height;
    }
    // 色度面行数与 VS 帧分配规则一致(height >> subSampling = floor)。
    // 曾用 ceil:奇高输出时 UnpackOutput 多拷一行越界 VS 帧分配尾部
    // (静默堆腐蚀 → c0000005,2026-09-22 真机实锤)。几何入口已强制
    // 偶尺寸,此处 floor 是最后一道防线(即使漏进奇尺寸也只欠拷不越界)。
    _outChromaW = _outW >> 1;
    _outChromaH = _outH >> 1;
    // HDR 输出 = 恒 P10(PQ BT.2020 limited);SDR 输出 = 源位深同格式。
    _outPlaneBytes = (_hdrPipe || _bitDepth > 8) ? 2u : 1u;
    _outFmt = _outPlaneBytes > 1 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;

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
    // PIPE 尺寸零深度(FG + VSR 放大时:DLSSG Depth 子矩形 = backbuffer
    // 尺寸;内容与 _depth 同为全零)。
    const bool needDepthPipe = fg && _vsrSlots && (_pipeW != width || _pipeH != height);
    if (needDepthPipe &&
        !CreateColorTexture(_depthPipe.GetAddressOf(), _pipeW, _pipeH,
                            DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, err, errLen)) {
        return false;
    }
    // 差异调试纹理(共享单实例):每槽描述符堆的槽 34 视图都指向它,
    // 因此必须先于槽池循环创建。重建(hot 复用换尺寸)时整槽纹理按
    // GetAddressOf 惯例重造(与 _motion/_depth 同款)。
    if (!CreateColorTexture(_debugDiff.GetAddressOf(), width, height,
                            DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }

    if (ProbeEnabled()) TimingStatusLine("PROBE: d3d12 frame-res before clear"); // init 细分(DEVICE_HUNG 时序定位)
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.NumDescriptors = 3;
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HRESULT hr = _device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(_rtvHeap.GetAddressOf()));
        if (FAILED(hr)) {
            SetErr(err, errLen, hr, "CreateDescriptorHeap(RTV) failed");
            return false;
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE base = _rtvHeap->GetCPUDescriptorHandleForHeapStart();
        const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const D3D12_CPU_DESCRIPTOR_HANDLE depthRtv{ base.ptr + static_cast<SIZE_T>(inc) };
        const D3D12_CPU_DESCRIPTOR_HANDLE depthPipeRtv{ base.ptr + static_cast<SIZE_T>(inc * 2) };
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = DXGI_FORMAT_R16G16_FLOAT;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        _device->CreateRenderTargetView(_motion.Get(), &rtv, base);
        rtv.Format = DXGI_FORMAT_R32_FLOAT;
        _device->CreateRenderTargetView(_depth.Get(), &rtv, depthRtv);
        if (_depthPipe) {
            _device->CreateRenderTargetView(_depthPipe.Get(), &rtv, depthPipeRtv);
        }

        std::lock_guard<std::mutex> ctlLock(_ctlMutex);
        if (!BeginCtlRecording()) {
            SetErr(err, errLen, E_FAIL, "BeginCtlRecording(guidance clear) failed");
            return false;
        }
        D3D12_RESOURCE_BARRIER toRt[3]{
            Transition(_motion.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
            Transition(_depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
            Transition(_depthPipe.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        _ctlCommandList->ResourceBarrier(_depthPipe ? 3u : 2u, toRt);
        const float zero[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
        _ctlCommandList->ClearRenderTargetView(base, zero, 0, nullptr);
        _ctlCommandList->ClearRenderTargetView(depthRtv, zero, 0, nullptr);
        if (_depthPipe) _ctlCommandList->ClearRenderTargetView(depthPipeRtv, zero, 0, nullptr);
        D3D12_RESOURCE_BARRIER toResident[3]{
            Transition(_motion.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(_depth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(_depthPipe.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        _ctlCommandList->ResourceBarrier(_depthPipe ? 3u : 2u, toResident);
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
    if (slot.baseFenceEvent) {
        CloseHandle(slot.baseFenceEvent);
        slot.baseFenceEvent = nullptr;
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
    slot.fgCommandList.Reset();
    slot.fgAllocator.Reset();
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
    slot.vsrColor.Reset();
    slot.hdrColor.Reset();
    slot.motionDense.Reset();
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
    slot.baseFenceEvent = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!slot.baseFenceEvent) {
        SetErr(err, errLen, E_FAIL, "Create slot base fence event failed");
        return false;
    }
    // FG 分段 CL(与 base 同型;门关帧不录制不提交,资源闲置无害)。
    hr = _device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(slot.fgAllocator.GetAddressOf()));
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "CreateCommandAllocator(slot fg) failed");
        return false;
    }
    hr = _device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.fgAllocator.Get(), nullptr,
        IID_PPV_ARGS(slot.fgCommandList.GetAddressOf()));
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "CreateCommandList(slot fg) failed");
        return false;
    }
    hr = slot.fgCommandList->Close();
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "Close initial slot fg command list failed");
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
    // RTX Video 管线纹理(PIPE 尺寸;请求了才建,描述符侧占位视图)。
    // vsrColor: VSR 输出(BGRA8 —— 官方 VSR 支持的输入/输出格式族
    // R8G8B8A8/B8G8R8A8/R10G10B10A2,输出需 UAV 标志,SDK D3D12 样例同款)。
    // hdrColor: TrueHDR 输出 FP16 scRGB(FG backbuffer / PQ 转换源)。
    // motionDense: FG MVecs 的 PIPE 尺寸版本(PIPE==src 时无纹理,eval
    // 直用 motion)。创建顺序在描述符块之前 —— NULL 描述符 TDR 铁律。
    if (_vsrSlots &&
        !CreateColorTexture(slot.vsrColor.GetAddressOf(), _pipeW, _pipeH,
                            DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    if (_hdrPipe &&
        !CreateColorTexture(slot.hdrColor.GetAddressOf(), _pipeW, _pipeH,
                            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    if (_fgSlots && _vsrSlots && (_pipeW != width || _pipeH != height) &&
        !CreateColorTexture(slot.motionDense.GetAddressOf(), _pipeW, _pipeH,
                            DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }

    // YUV 平面纹理:In = upload 拷入供转换采样(源尺寸);Out = 颜色→YUV
    // dispatch 写出供回读(OUT 尺寸 —— RTX VSR 放大后输出平面在目标尺寸;
    // 无 RTX 时 OUT=src 同旧管线)。R8_UNORM(8bit)/R16_UNORM(10bit,存储字
    // = VS P10 采样值,右对齐 0-1023 —— 2026-09-08 BlankClip 实测;UNORM
    // 读出 word/65535,round(×65535) 精确还原整数采样值)。
    const DXGI_FORMAT yuvFmt = _bitDepth > 8 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    for (int i = 0; i < 3; ++i) {
        const int pw = i == 0 ? width : _chromaW;
        const int ph = i == 0 ? height : _chromaH;
        if (!CreateColorTexture(slot.yuvIn[i].GetAddressOf(), pw, ph,
                                yuvFmt, D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_FLAG_NONE, err, errLen)) {
            return false;
        }
        const int ow = i == 0 ? _outW : _outChromaW;
        const int oh = i == 0 ? _outH : _outChromaH;
        if (!CreateColorTexture(slot.yuvOut[i].GetAddressOf(), ow, oh,
                                _outFmt, D3D12_RESOURCE_STATE_COMMON,
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
    // persist-mapped(纯行拷贝,无像素算术)。upload = 源尺寸(输入平面);
    // readback = OUT 尺寸(RTX 放大后的输出平面)。
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
        const int orw = i == 0 ? _outW : _outChromaW;
        const int orh = i == 0 ? _outH : _outChromaH;
        const UINT outBytesPerRow = static_cast<UINT>(orw) * _outPlaneBytes;
        if (!CreateRawBuffer(static_cast<UINT64>(orh) * outBytesPerRow, D3D12_HEAP_TYPE_READBACK,
                             D3D12_RESOURCE_STATE_COPY_DEST,
                             slot.readbackYuv[i].GetAddressOf(), slot.readbackPitchYuv[i],
                             outBytesPerRow, err, errLen)) {
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
        // 35 = 0..31 原有 + 32/33(FG 插值输出的 SRV/UAV;堆空间常备,
        // 非 FG 槽写占位视图)+ 34(共享差异调试纹理 UAV)+ 36/37/38
        // (RTX:vsrColor/hdrColor 的 SRV、motionDense 的 UAV;无 RTX 槽 =
        // outputColor/motion 占位视图)。
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 51; // 0-34 原有 + 36/37/38(RTX)+ 49/50(FFX OF 后端)
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
        // 34:共享差异调试纹理的 UAV(资源 = context 级 _debugDiff,已在
        // CreateFrameResources 槽池循环前创建;每槽堆各持一份指向同一
        // 资源的视图,dispatch 只被本槽 CL 引用)。
        _device->CreateUnorderedAccessView(_debugDiff.Get(), nullptr, nullptr, slotHandle(34));
        // 35:静态零运动纹理的 SRV(资源 = context 级 _motion,槽池循环前
        // 创建,常驻 NSR)—— 光流场调试视图在无真运动帧(播种/OF 关)绑定
        // 它:slot.motion 的内容在首 densify 前未定义,直接绑会显示假流;
        // 零纹理保证"黑 = 无光流数据"的语义成立。
        _device->CreateShaderResourceView(_motion.Get(), nullptr, slotHandle(35));
        // 36/37/38:RTX Video(占位 = outputColor/motion,资源带 UAV flag,
        // 满足"绝不写 NULL 描述符"惯例;RTX 未激活时这些槽永不被有效读取
        // —— 对应的 Record 调用只在 RTX 管线被记录)。
        _device->CreateShaderResourceView(slot.vsrColor ? slot.vsrColor.Get()
                                                        : slot.outputColor.Get(),
                                          nullptr, slotHandle(36));
        _device->CreateShaderResourceView(slot.hdrColor ? slot.hdrColor.Get()
                                                        : slot.outputColor.Get(),
                                          nullptr, slotHandle(37));
        _device->CreateUnorderedAccessView(slot.motionDense ? slot.motionDense.Get()
                                                            : slot.motion.Get(),
                                           nullptr, nullptr, slotHandle(38));
    }
    return true;
}

// DLSS FG 槽资源(仅 _fgSlots):插值输出纹理 + 第二组回读缓冲。
// RTX 双尺寸:fgInterp 在 PIPE 尺寸(backbuffer 同侧),格式随 _hdrPipe
// (SDR BGRA8 / HDR FP16 scRGB);回读缓冲 = OUT 尺寸(与真实帧一致)。
bool D3D12Context::CreateFgSlotResources(FrameSlot &slot, char *err, size_t errLen) noexcept {
    if (!CreateColorTexture(slot.fgInterp.GetAddressOf(), _pipeW, _pipeH,
                            _hdrPipe ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                     : DXGI_FORMAT_B8G8R8A8_UNORM,
                            D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    for (int g = 0; g < kFgGenSlots; ++g) {
        for (int i = 0; i < 3; ++i) {
            const int pw = i == 0 ? _outW : _outChromaW;
            const int ph = i == 0 ? _outH : _outChromaH;
            const UINT bytesPerRow = static_cast<UINT>(pw) * _outPlaneBytes;
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

bool D3D12Context::RecordReadbackCopy(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                      char *err, size_t errLen,
                                      int fgGen) noexcept {
    // RecordYuvOutput 之后调用:yuvOut 处于 UAV 态 → COPY_SOURCE → 拷贝
    // → COMMON。三平面各自 footprint(格式同资源,跨格式 = 静默 E_INVALIDARG)。
    // fgGen >= 0 = 拷入 FG 插值帧第 fgGen 组回读缓冲(真实帧与各插值槽先后
    // 转换+回读,共用 yuvOut,目标缓冲两两不同)。cl 由调用方显式给定
    // (真实帧 = base CL,插值 = fg CL)。
    ID3D12GraphicsCommandList *cl = &clRef;
    const DXGI_FORMAT yuvFmt = _outFmt;
    const int planeW[3]{ _outW, _outChromaW, _outChromaW };
    const int planeH[3]{ _outH, _outChromaH, _outChromaH };
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

bool D3D12Context::BeginFgRecording(FrameSlot &slot) noexcept {
    // 与 BeginFrameRecording 同款防砖:上次录制中途失败遗留 open CL 会让
    // allocator Reset 报 E_FAIL —— force-close 一次再试。
    HRESULT hr = slot.fgAllocator->Reset();
    if (FAILED(hr)) {
        slot.fgCommandList->Close();
        hr = slot.fgAllocator->Reset();
        if (FAILED(hr)) return false;
    }
    hr = slot.fgCommandList->Reset(slot.fgAllocator.Get(), nullptr);
    return SUCCEEDED(hr);
}

// 分段提交共用体:Close 目标 CL → 队列执行 → signal 全局栅栏新值。提交互斥
// 保证栅栏值顺序与队列 ExecuteCommandLists 顺序一致(值永不回退)。
bool D3D12Context::SubmitBaseFrame(FrameSlot &slot, ID3D12Fence *waitFence,
                                   uint64_t waitValue, char *err, size_t errLen) noexcept {
    HRESULT hr = slot.commandList->Close();
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "Close(slot base) failed");
        return false;
    }
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
    // 槽命令排到 NVOF 输出之后,防 GPU 端读-写竞争)。现状恒空:StageFrame
    // 已 CPU 等待 NVOF execute 完成。
    if (waitFence && waitValue) {
        _queue->Wait(waitFence, waitValue);
    }
    ID3D12CommandList *lists[]{ slot.commandList.Get() };
    _queue->ExecuteCommandLists(1, lists);
    slot.baseFenceValue = _fenceValue.fetch_add(1) + 1;
    _queue->Signal(_fence.Get(), slot.baseFenceValue);
    return true;
}

bool D3D12Context::SubmitFgFrame(FrameSlot &slot, ID3D12Fence *waitFence, uint64_t waitValue,
                                 char *err, size_t errLen) noexcept {
    HRESULT hr = slot.fgCommandList->Close();
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "Close(slot fg) failed");
        return false;
    }
    std::lock_guard<std::mutex> lock(_submitMutex);
    // RTX 专用队列的产出(管线色)是本段输入 —— 跨队列 Wait 排在执行前。
    if (waitFence && waitValue) {
        _queue->Wait(waitFence, waitValue);
    }
    ID3D12CommandList *lists[]{ slot.fgCommandList.Get() };
    _queue->ExecuteCommandLists(1, lists);
    slot.fenceValue = _fenceValue.fetch_add(1) + 1;
    _queue->Signal(_fence.Get(), slot.fenceValue);
    return true;
}

bool D3D12Context::WaitBaseFrame(FrameSlot &slot, char *err, size_t errLen) noexcept {
    return WaitFenceValue(slot.baseFenceValue, slot.baseFenceEvent, err, errLen);
}

bool D3D12Context::WaitFrame(FrameSlot &slot, char *err, size_t errLen) noexcept {
    return WaitFenceValue(slot.fenceValue, slot.fenceEvent, err, errLen);
}

bool D3D12Context::UnpackOutput(
    FrameSlot &slot,
    uint8_t **dstPlanes, int64_t *dstStrides,
    int width, int height, char *err, size_t errLen,
    int fgGen) noexcept {
    // RTX 双尺寸:输出平面在 OUT 尺寸(调用方按 OUT 建帧)。width/height
    // 即调用方帧尺寸,与 _outW/_outH 对账。
    if (width != _outW || height != _outH) {
        SetErr(err, errLen, E_INVALIDARG, "UnpackOutput: size mismatch");
        return false;
    }

    // YUV 原生:RGB→YUV 已在 GPU 完成,这里纯行拷贝回 VS 平面。
    // 10bit:yuvOut(R16_UNORM)存储字 = round(n*65535) = 10-bit 采样值
    // (shader 侧 w=n*1023/65535 精确缩放,见 BGRA_TO_YUV_HLSL),与 VS
    // P10 word 逐位一致 —— 所以是纯拷贝。
    // fgGen >= 0 = 读 FG 插值帧第 fgGen 组回读缓冲。
    for (int p = 0; p < 3; ++p) {
        const int pw = p == 0 ? width : _outChromaW;
        const int ph = p == 0 ? height : _outChromaH;
        const size_t rowBytes = static_cast<size_t>(pw) * _outPlaneBytes;
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

// ---- AMD 光流后端(FFX)----
// FFX Prepare(Magpie PREPARE_INPUT_HLSL 移植):box 平均下采样到 OF extent。
// FFX 契约输入为 R8G8B8A8(内部 luma 提取按 RGBA 序)。inputColor 是 BGRA8
// 但 typed SRV Load 返回**逻辑 RGBA**(项目惯例,见 NVOF_DOWNSAMPLE_HLSL
// 注释)—— 直接写 float4 即可,无需换序(首版误加 .zyxw 反而转反通道,
// dump_fxof_input 与 dump_input 逐字节对照实锤)。
constexpr char FFX_PREPARE_HLSL[] = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Target : register(u0);
cbuffer Params : register(b0) { uint2 SourceExtent; uint2 TargetExtent; };
[numthreads(8, 8, 1)]
void Prepare(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    uint2 begin = tid.xy * SourceExtent / TargetExtent;
    uint2 end = max(begin + 1, (tid.xy + 1) * SourceExtent / TargetExtent);
    end = min(end, SourceExtent);
    float4 value = 0;
    uint count = 0;
    for (uint y = begin.y; y < end.y; ++y) {
        for (uint x = begin.x; x < end.x; ++x) {
            value += Source.Load(int3(uint2(x, y), 0));
            ++count;
        }
    }
    // SRV 已是逻辑 RGBA(R8G8B8A8 目标直写)。
    Target[tid.xy] = value / max(count, 1);
}
)";

// FFX densify(Magpie DENSIFY_HLSL 原样):1/8 分辨率稀疏流(R16G16_SINT,
// 单位 = OF extent 像素)双线性上采样到稠密运动 + 恒定置信度 0.65
// (FidelityFX OF 不暴露置信度面,Magpie 诚实保守基线)。
constexpr char FFX_DENSIFY_HLSL[] = R"(
Texture2D<int2> SparseFlow : register(t0);
RWTexture2D<float2> DenseMotion : register(u0);
RWTexture2D<float> DenseConfidence : register(u1);
cbuffer Params : register(b0) {
    uint2 SourceExtent;
    uint2 OpticalFlowExtent;
    uint2 SparseExtent;
    float2 VectorScale;
};
int2 LoadFlow(int2 p) {
    p = clamp(p, int2(0, 0), int2(SparseExtent) - 1);
    return SparseFlow.Load(int3(p, 0));
}
[numthreads(8, 8, 1)]
void Densify(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= SourceExtent)) return;
    float2 opticalPixel = (float2(tid.xy) + 0.5) *
        (float2(OpticalFlowExtent) / float2(SourceExtent));
    float2 sparsePos = opticalPixel / 8.0 - 0.5;
    int2 p0 = int2(floor(sparsePos));
    float2 f = frac(sparsePos);
    float2 a = float2(LoadFlow(p0));
    float2 b = float2(LoadFlow(p0 + int2(1, 0)));
    float2 c = float2(LoadFlow(p0 + int2(0, 1)));
    float2 d = float2(LoadFlow(p0 + int2(1, 1)));
    DenseMotion[tid.xy] = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y) *
        VectorScale;
    DenseConfidence[tid.xy] = 0.65;
}
)";

// 差异调试视图(面板"差异调试 ×20",OptiScaler DLSSNR fork 的 DebugView=3
// 同语义):|NR输出 − 原帧| 逐通道最大差 × 放大系数的灰度图 —— 白 = 改动
// 大,一片灰 = 模型没动画面。两输入同为 BGRA8 的 typed SRV(逻辑 RGBA,
// 通道序一致,差值与分量序无关);alpha 恒 1。
constexpr char DEBUG_DIFF_HLSL[] = R"(
Texture2D<float4> SourceInput : register(t0);   // 原帧(YUV→RGB 转换后)
Texture2D<float4> SourceOutput : register(t1);  // NR/残差/直通输出
RWTexture2D<float4> DebugDiff : register(u0);

cbuffer DebugDiffParams : register(b0) {
    uint2 Extent;        // dispatch 边界(全分辨率)
    float Amplification; // 20
    uint Pad0;
};

[numthreads(8, 8, 1)]
void DebugDiffMain(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= Extent)) return;
    const float3 src = SourceInput[tid.xy].xyz;
    const float3 dst = SourceOutput[tid.xy].xyz;
    const float d = max(abs(dst.r - src.r), max(abs(dst.g - src.g), abs(dst.b - src.b)));
    DebugDiff[tid.xy] = float4(saturate(d * Amplification).xxx, 1.0);
}
)";

// 光流场调试视图(面板"调试视图 = 光流场"):稠密运动场可视化 —— 方向→
// 色相(标准 HSV 环,图像坐标 y 向下:红=右/绿=下/青=左/紫=上),幅值→亮度,静止与无
// 光流 = 黑。回应"光流到底有没有在流/方向对不对":一片黑 = 无运动数据
// (OF 关/播种帧),彩色纹理 = 真运动场,颜色一致性 = 方向场正确性。
// 输入为 densify 产物(motion 或 follow 内部管线的 reducedMotion,单位 =
// 源像素 current-to-previous),R16G16_FLOAT 的 Texture2D<float2> SRV。
constexpr char FLOW_VIEW_HLSL[] = R"(
Texture2D<float2> FlowField : register(t0);
RWTexture2D<float4> FlowView : register(u0);

cbuffer FlowViewParams : register(b0) {
    uint2 Extent;    // 运动场尺寸(follow 内部管线 = 内部尺寸,否则源尺寸)
    uint2 OutExtent; // 输出尺寸(恒源尺寸;dispatch 边界)
    float Scale;     // 幅值→亮度(0.125 = 8px/帧满亮)
    uint Pad0;
};

[numthreads(8, 8, 1)]
void FlowViewMain(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= OutExtent)) return;
    // 最近邻取运动场 texel(方向不跨 texel 混合,单元值保真;follow 半
    // 分辨率下呈块状属预期。dispatch 恒按输出尺寸,follow 内部场放大铺满,
    // _debugDiff 无陈旧边缘)
    const int2 p = min(int2((tid.xy * Extent) / OutExtent), int2(Extent) - 1);
    const float2 v = FlowField[p];
    const float mag = length(v);
    if (mag < 0.05) {
        // 死区:亚 0.05px 的噪声不渲染,黑 = 静止,保证"有没有流"一眼可判
        FlowView[tid.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    const float hue = frac(atan2(v.y, v.x) * 0.15915494 + 1.0); // /2π
    const float3 rgb =
        saturate(abs(fmod(hue * 6.0 + float3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0);
    FlowView[tid.xy] = float4(rgb * saturate(mag * Scale), 1.0);
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

// ---- RTX Video:PIPE → OUT 缩放转换(SDR;BGRA8 源)----
// VSR 输出(PIPE 尺寸)→ OUT 尺寸 YUV 平面。双 bilinear 采样(归一化坐标
// 映射,手工 4 tap——全仓惯例无 sampler,SRV [] 索引)。PIPE==OUT 时坐标
// 恒等映射 = 逐位等价旧路径。cbuffer 布局与 BGRA_TO_YUV_HLSL 完全一致
// (dstExtent / srcExtent / Kr Kb LoOverCM SpanOverCM + pad),共用
// _rsConvertOut 根签名,仅换 PSO。4 tap 权重全正,色度平均后一次转换
// (与 2×2 box 同款仿射论证)。
constexpr char COLOR_TO_YUV_SCALED_HLSL[] = R"(
Texture2D<float4> CompositeColor : register(t0);
RWTexture2D<float> OutputA : register(u0);
RWTexture2D<float> OutputB : register(u1);

cbuffer ConvertOutParams : register(b0) {
    uint2 DstExtent;      // luma: OUT W,H;chroma: OUT cw,ch(dispatch 边界)
    uint2 SourceExtent;   // PIPE 全分辨率尺寸
    float Kr;
    float Kb;
    float LoOverCM;
    float SpanOverCM;
    float Pad0;
    float Pad1;
    float Pad2;
};

float LumaOf(float3 rgb) {
    return dot(rgb, float3(Kr, 1.0 - Kr - Kb, Kb));
}
float ChromaToCode(float c) {
    return saturate(c * SpanOverCM + LoOverCM);
}
// 手工双线性(4 tap,clamp 边界;idx 域 = 源纹理尺寸)。
float3 SampleBilinear(float2 srcPos) {
    const float2 f = floor(srcPos);
    const int2 i0 = int2(f);
    const float2 t = srcPos - f;
    const int2 e = SourceExtent - 1;
    const int2 a = clamp(i0, int2(0, 0), e);
    const int2 b = clamp(i0 + int2(1, 0), int2(0, 0), e);
    const int2 c = clamp(i0 + int2(0, 1), int2(0, 0), e);
    const int2 d = clamp(i0 + int2(1, 1), int2(0, 0), e);
    return CompositeColor[a].xyz * ((1 - t.x) * (1 - t.y))
         + CompositeColor[b].xyz * (t.x * (1 - t.y))
         + CompositeColor[c].xyz * ((1 - t.x) * t.y)
         + CompositeColor[d].xyz * (t.x * t.y);
}
float2 ToSrcPos(float2 dstPos) {
    return (dstPos + 0.5) * float2(SourceExtent) / float2(DstExtent) - 0.5;
}
// 色度域采样映射:入参为 out-luma 域坐标(chroma 侧 base = tid*2),映射
// 分母必须是 out-luma 域 = 2*DstExtent(色度 dispatch 的 DstExtent=色度域;
// 输出恒取偶故 2*DstExtent 精确)。误用 DstExtent = 2× 过采样 —— 色度
// 右半平面 clamp 到源右缘,表现为大色块/失饱和(2026-09-22 真机实锤,
// "只有一层分辨率放大了")。
float2 ToSrcPosChroma(float2 lumaPos) {
    return (lumaPos + 0.5) * float2(SourceExtent) / (2.0 * float2(DstExtent)) - 0.5;
}

[numthreads(8, 8, 1)]
void ScaledToYuvLuma(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    const float3 rgb = saturate(SampleBilinear(ToSrcPos(float2(tid.xy))));
    const float nY = LumaOf(rgb);
    OutputA[tid.xy] = saturate(nY * SpanOverCM + LoOverCM);
}

[numthreads(8, 8, 1)]
void ScaledToYuvChroma(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    // luma 域 2×2 覆盖区四点采样(各点双线性)平均 → 转换。
    const float2 base = tid.xy * 2;
    const float3 rgb = saturate(0.25 * (SampleBilinear(ToSrcPosChroma(base + float2(0.5, 0.5)))
                                      + SampleBilinear(ToSrcPosChroma(base + float2(1.5, 0.5)))
                                      + SampleBilinear(ToSrcPosChroma(base + float2(0.5, 1.5)))
                                      + SampleBilinear(ToSrcPosChroma(base + float2(1.5, 1.5)))));
    const float nY = LumaOf(rgb);
    const float cb = (rgb.z - nY) * (0.5 / (1.0 - Kb));
    const float cr = (rgb.x - nY) * (0.5 / (1.0 - Kr));
    OutputA[tid.xy] = ChromaToCode(cb);
    OutputB[tid.xy] = ChromaToCode(cr);
}
)";

// ---- RTX Video HDR:FP16 scRGB → BT.2020 PQ P10(TrueHDR 输出转换)----
// TrueHDR 输出语义 = linear scRGB(1.0 = 80 nits SDR 参考白,Magpie
// RTXVideoHdr 同解读)。链路:线性 709 →(线性域)2020 色域 → 各分量 PQ
// (ST 2084,归一 10000 nits)→ 2020 YCbCr limited 10bit → P10 字(右对齐
// 惯例:w = code/65535)。双线性在 LINEAR 光域 = 物理正确插值。cbuffer 同
// 上(Kr/Kb 位置复用为 2020 的 0.2627/0.0593,Lo/Span 传 limited 常量;
// 2026-09-23 实锤:Lo/Span 曾按跨度 876/896 折 —— 整个 HDR 动态范围被
// 压进 1.4 个 P10 码的"恒定纯色"。正确分母 = 容器 1023)。
constexpr char FP16_TO_YUV_PQ_HLSL[] = R"(
Texture2D<float4> HdrColor : register(t0);
RWTexture2D<float> OutputA : register(u0);
RWTexture2D<float> OutputB : register(u1);

cbuffer ConvertOutParams : register(b0) {
    uint2 DstExtent;
    uint2 SourceExtent;
    float Kr;             // BT.2020: 0.2627
    float Kb;             // BT.2020: 0.0593
    float LoOverCM;       // luma: 64/1023;chroma: 512/1023
    float SpanOverCM;     // 876/1023 或 896/1023
    float Pad0;
    float Pad1;
    float Pad2;
};

static const float3x3 M709To2020 = {
    0.6274, 0.3293, 0.0433,
    0.0690, 0.9195, 0.0112,
    0.0164, 0.0880, 0.8955,
};

// ST 2084 PQ EOTF 逆(输入 nits → [0,1] PQ 码值;归一 10000 nits)。
float PqEncode(float nits) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    float p = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}
float4 SampleHdrBilinear(float2 srcPos) {
    const float2 f = floor(srcPos);
    const int2 i0 = int2(f);
    const float2 t = srcPos - f;
    const int2 e = SourceExtent - 1;
    const int2 a = clamp(i0, int2(0, 0), e);
    const int2 b = clamp(i0 + int2(1, 0), int2(0, 0), e);
    const int2 c = clamp(i0 + int2(0, 1), int2(0, 0), e);
    const int2 d = clamp(i0 + int2(1, 1), int2(0, 0), e);
    return HdrColor[a] * ((1 - t.x) * (1 - t.y))
         + HdrColor[b] * (t.x * (1 - t.y))
         + HdrColor[c] * ((1 - t.x) * t.y)
         + HdrColor[d] * (t.x * t.y);
}
float2 ToSrcPos(float2 dstPos) {
    return (dstPos + 0.5) * float2(SourceExtent) / float2(DstExtent) - 0.5;
}
// 同 ScaledToYuvChroma 的 ToSrcPosChroma:色度侧入参为 out-luma 域坐标,
// 分母 = 2*DstExtent(误用 DstExtent = 2× 过采样,色块/失饱和)。
float2 ToSrcPosChroma(float2 lumaPos) {
    return (lumaPos + 0.5) * float2(SourceExtent) / (2.0 * float2(DstExtent)) - 0.5;
}
// 线性 scRGB(709)→ PQ 编码的 2020 RGB 三元组。
// mul(矩阵, 向量) = 行和语义(灰保持);曾写 mul(向量, 矩阵) = 列和
// (0.713/1.337/0.950)→ 灰色扭曲成 R 压 G 涨 → 全画面青色罩。
float3 ToPq2020(float3 lin709) {
    const float3 lin2020 = mul(M709To2020, max(lin709, 0.0)) * 80.0; // nits
    return float3(PqEncode(lin2020.r), PqEncode(lin2020.g), PqEncode(lin2020.b));
}

[numthreads(8, 8, 1)]
void PqToYuvLuma(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    const float3 pq = ToPq2020(SampleHdrBilinear(ToSrcPos(float2(tid.xy))).rgb);
    const float y = dot(pq, float3(Kr, 1.0 - Kr - Kb, Kb));
    // P10 存储约定(与 BGRA_TO_YUV 同款):R16_UNORM 字 = 10-bit 采样值,
    // shader 侧 ×1023/65535 精确缩放 —— 缺它 = 字 = 码×64,mpv 读出越界垃圾。
    OutputA[tid.xy] = saturate(y * SpanOverCM + LoOverCM) * (1023.0 / 65535.0);
}

[numthreads(8, 8, 1)]
void PqToYuvChroma(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    const float2 base = tid.xy * 2;
    const float3 pq = 0.25 * (ToPq2020(SampleHdrBilinear(ToSrcPosChroma(base + float2(0.5, 0.5))).rgb)
                            + ToPq2020(SampleHdrBilinear(ToSrcPosChroma(base + float2(1.5, 0.5))).rgb)
                            + ToPq2020(SampleHdrBilinear(ToSrcPosChroma(base + float2(0.5, 1.5))).rgb)
                            + ToPq2020(SampleHdrBilinear(ToSrcPosChroma(base + float2(1.5, 1.5))).rgb));
    const float y = dot(pq, float3(Kr, 1.0 - Kr - Kb, Kb));
    const float cb = (pq.b - y) * (0.5 / (1.0 - Kb));
    const float cr = (pq.r - y) * (0.5 / (1.0 - Kr));
    OutputA[tid.xy] = saturate(cb * SpanOverCM + LoOverCM) * (1023.0 / 65535.0);
    OutputB[tid.xy] = saturate(cr * SpanOverCM + LoOverCM) * (1023.0 / 65535.0);
}
)";

// ---- RTX Video + FG:源尺寸运动场 → PIPE 尺寸(mvec 放大)----
// DLSSG MVecs 契约 = backbuffer 同尺寸、像素单位 current-to-previous。
// R16G16F 向量场双线性 = 线性插值,语义保真。4 常量:dstExtent(2)+srcExtent(2)
// (CreateOfPso 通用形态)。
constexpr char MVEC_SCALE_HLSL[] = R"(
Texture2D<float2> SrcMotion : register(t0);
RWTexture2D<float2> DstMotion : register(u0);

cbuffer MotionScaleParams : register(b0) {
    uint2 DstExtent;
    uint2 SourceExtent;
};

[numthreads(8, 8, 1)]
void ScaleMotion(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    const float2 srcPos = (tid.xy + 0.5) * float2(SourceExtent) / float2(DstExtent) - 0.5;
    const float2 f = floor(srcPos);
    const int2 i0 = int2(f);
    const float2 t = srcPos - f;
    const int2 e = SourceExtent - 1;
    const int2 a = clamp(i0, int2(0, 0), e);
    const int2 b = clamp(i0 + int2(1, 0), int2(0, 0), e);
    const int2 c = clamp(i0 + int2(0, 1), int2(0, 0), e);
    const int2 d = clamp(i0 + int2(1, 1), int2(0, 0), e);
    DstMotion[tid.xy] = SrcMotion[a] * ((1 - t.x) * (1 - t.y))
                      + SrcMotion[b] * (t.x * (1 - t.y))
                      + SrcMotion[c] * ((1 - t.x) * t.y)
                      + SrcMotion[d] * (t.x * t.y);
}
)";

// AMD 光流后端通用 cs_5_0 PSO 构造:1 个 32 位常量根参数(b0,numConsts)
// + nSrv 个独立 t 表 + nUav 个独立 u 表(与 densify/nvof downsample 同构;
// 独立表参数,同表重叠 range 禁忌)。FFX 的两个 PSO 全走这里 ——
// 无 WaveOps 依赖(cs_5_0),FFX 自身 7 pass 是 SM6.2 预编译 blob 不经本编译器。
bool CreateOfPso(ID3D12Device *device, const char *hlsl, const char *entry,
                 UINT numConsts, UINT srvCount, UINT uavCount,
                 ID3D12RootSignature **rs, ID3D12PipelineState **pso,
                 const char *label, char *err, size_t errLen) noexcept {
    // err 直写(本 helper 是自由函数,SetErr 是 D3D12Context 成员)。
    auto fail = [&](const char *what) {
        if (err && errLen) std::snprintf(err, errLen, "%s: %s", label, what);
        return false;
    };
    D3D12_DESCRIPTOR_RANGE srvRanges[2]{};
    for (UINT i = 0; i < srvCount; ++i) {
        srvRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[i].NumDescriptors = 1;
        srvRanges[i].BaseShaderRegister = i;
        srvRanges[i].OffsetInDescriptorsFromTableStart = 0;
    }
    D3D12_DESCRIPTOR_RANGE uavRanges[2]{};
    for (UINT i = 0; i < uavCount; ++i) {
        uavRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRanges[i].NumDescriptors = 1;
        uavRanges[i].BaseShaderRegister = i;
        uavRanges[i].OffsetInDescriptorsFromTableStart = 0;
    }
    D3D12_ROOT_PARAMETER params[5]{};
    UINT n = 0;
    params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[n].Constants.ShaderRegister = 0;
    params[n].Constants.Num32BitValues = numConsts;
    params[n].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    ++n;
    for (UINT i = 0; i < srvCount; ++i, ++n) {
        params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[n].DescriptorTable.NumDescriptorRanges = 1;
        params[n].DescriptorTable.pDescriptorRanges = &srvRanges[i];
        params[n].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    for (UINT i = 0; i < uavCount; ++i, ++n) {
        params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[n].DescriptorTable.NumDescriptorRanges = 1;
        params[n].DescriptorTable.pDescriptorRanges = &uavRanges[i];
        params[n].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = n;
    rsDesc.pParameters = params;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> rsBlob, rsErr;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           rsBlob.GetAddressOf(), rsErr.GetAddressOf()))) {
        return fail("SerializeRootSignature failed");
    }
    if (FAILED(device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                           rsBlob->GetBufferSize(), IID_PPV_ARGS(rs)))) {
        return fail("CreateRootSignature failed");
    }
    ComPtr<ID3DBlob> code, csErr;
    if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entry, "cs_5_0",
                          0, 0, code.GetAddressOf(), csErr.GetAddressOf()))) {
        return fail(csErr ? static_cast<const char *>(csErr->GetBufferPointer())
                          : "D3DCompile failed");
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = *rs;
    psoDesc.CS = { code->GetBufferPointer(), code->GetBufferSize() };
    if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(pso)))) {
        return fail("CreateComputePipelineState failed");
    }
    return true;
}

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
    // AMD 光流后端 PSO(FFX Prepare/Densify)。仅 of_backend 选择 ffx 时才
    // 被消费;构造失败 = 初始化失败(与其它 PSO 同语义,不静默降级)。
    {
        if (!CreateOfPso(_device.Get(), FFX_PREPARE_HLSL, "Prepare", 4, 1, 1,
                         _rsFfxPrepare.GetAddressOf(), _psoFfxPrepare.GetAddressOf(),
                         "ffx prepare", err, errLen) ||
            !CreateOfPso(_device.Get(), FFX_DENSIFY_HLSL, "Densify", 8, 1, 2,
                         _rsFfxDensify.GetAddressOf(), _psoFfxDensify.GetAddressOf(),
                         "ffx densify", err, errLen)) {
            return false;
        }
    }
    // 差异调试视图:b0 4 常量 + t0/t1 两张 SRV 表(input/output)+ u0 一张
    // UAV 表(共享 _debugDiff)。SRV 各自独立表参数(同表重叠 range 禁忌
    // 见 densify 块注释)。
    {
        D3D12_DESCRIPTOR_RANGE srvRanges[2]{};
        for (UINT i = 0; i < 2; ++i) {
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
        D3D12_ROOT_PARAMETER params[4]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = 4; // extent(2)+amp(1)+pad(1)
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        for (UINT i = 0; i < 2; ++i) {
            params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
            params[1 + i].DescriptorTable.pDescriptorRanges = &srvRanges[i];
            params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable.NumDescriptorRanges = 1;
        params[3].DescriptorTable.pDescriptorRanges = &uavRange;
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 4;
        rsDesc.pParameters = params;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> rsBlob, rsErr;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                               rsBlob.GetAddressOf(), rsErr.GetAddressOf()))) {
            SetErr(err, errLen, E_FAIL, "SerializeRootSignature(debug diff) failed");
            return false;
        }
        if (FAILED(_device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                                rsBlob->GetBufferSize(), IID_PPV_ARGS(_rsDebugDiff.GetAddressOf())))) {
            SetErr(err, errLen, E_FAIL, "CreateRootSignature(debug diff) failed");
            return false;
        }
        ComPtr<ID3DBlob> code, csErr;
        if (FAILED(D3DCompile(DEBUG_DIFF_HLSL, strlen(DEBUG_DIFF_HLSL),
                              nullptr, nullptr, nullptr, "DebugDiffMain", "cs_5_0",
                              0, 0, code.GetAddressOf(), csErr.GetAddressOf()))) {
            SetErr(err, errLen, E_FAIL, csErr ? static_cast<const char *>(csErr->GetBufferPointer()) : "D3DCompile(debug diff) failed");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = _rsDebugDiff.Get();
        psoDesc.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        if (FAILED(_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(_psoDebugDiff.GetAddressOf())))) {
            SetErr(err, errLen, E_FAIL, "CreateComputePipelineState(debug diff) failed");
            return false;
        }
    }
    // 光流场调试视图:b0 4 常量 + t0 一张 SRV 表(运动场,槽 10/18 按取材
    // 二选一)+ u0 一张 UAV 表(共享 _debugDiff 中转)。
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
        params[0].Constants.Num32BitValues = 6; // extent(2)+outExtent(2)+scale(1)+pad(1)
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
            SetErr(err, errLen, E_FAIL, "SerializeRootSignature(flow view) failed");
            return false;
        }
        if (FAILED(_device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                                rsBlob->GetBufferSize(), IID_PPV_ARGS(_rsFlowView.GetAddressOf())))) {
            SetErr(err, errLen, E_FAIL, "CreateRootSignature(flow view) failed");
            return false;
        }
        ComPtr<ID3DBlob> code, csErr;
        if (FAILED(D3DCompile(FLOW_VIEW_HLSL, strlen(FLOW_VIEW_HLSL),
                              nullptr, nullptr, nullptr, "FlowViewMain", "cs_5_0",
                              0, 0, code.GetAddressOf(), csErr.GetAddressOf()))) {
            SetErr(err, errLen, E_FAIL, csErr ? static_cast<const char *>(csErr->GetBufferPointer()) : "D3DCompile(flow view) failed");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = _rsFlowView.Get();
        psoDesc.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        if (FAILED(_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(_psoFlowView.GetAddressOf())))) {
            SetErr(err, errLen, E_FAIL, "CreateComputePipelineState(flow view) failed");
            return false;
        }
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
            // RTX Video 输出转换:与上面共用 _rsConvertOut(同 12 常量 +
            // t0/u0/u1 布局),仅换 HLSL 入口。
            { "ScaledToYuvLuma", _psoConvertScaledLuma.GetAddressOf() },
            { "ScaledToYuvChroma", _psoConvertScaledChroma.GetAddressOf() },
            { "PqToYuvLuma", _psoPqLuma.GetAddressOf() },
            { "PqToYuvChroma", _psoPqChroma.GetAddressOf() },
        };
        constexpr const char *kOutHlslByEntry[] = {
            BGRA_TO_YUV_HLSL, BGRA_TO_YUV_HLSL,
            COLOR_TO_YUV_SCALED_HLSL, COLOR_TO_YUV_SCALED_HLSL,
            FP16_TO_YUV_PQ_HLSL, FP16_TO_YUV_PQ_HLSL,
        };
        for (size_t ci = 0; ci < std::size(outCsos); ++ci) {
            const auto &cso = outCsos[ci];
            ComPtr<ID3DBlob> code, csErr;
            if (FAILED(D3DCompile(kOutHlslByEntry[ci], strlen(kOutHlslByEntry[ci]),
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
    {
        // mvec 放大(源→PIPE):CreateOfPso 通用形态(1 SRV + 1 UAV + 4 常量)。
        if (!CreateOfPso(_device.Get(), MVEC_SCALE_HLSL, "ScaleMotion", 4, 1, 1,
                         _rsMotionScale.GetAddressOf(), _psoMotionScale.GetAddressOf(),
                         "motion scale", err, errLen)) {
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

void D3D12Context::RecordDebugDiff(FrameSlot &slot) noexcept {
    // 差异调试视图(契约见头文件):input COMMON / output UAV 入,输出
    // 保持 UAV、input 归 COMMON 出。dispatch 写共享 _debugDiff,同 CL 内
    // COPY_SOURCE 化后拷回 outputColor —— RGBA8 无 UAV load,读写同纹理
    // 非法,中转纹理是必须的。每槽堆的槽 34 视图都指向共享资源,dispatch
    // + 拷回在同一 CL 上原子成对,队列提交序串行,并发帧不交错。
    ID3D12GraphicsCommandList *cl = slot.commandList.Get();
    cl->SetComputeRootSignature(_rsDebugDiff.Get());
    cl->SetPipelineState(_psoDebugDiff.Get());
    ID3D12DescriptorHeap *heaps[]{ slot.srvUavHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = slot.srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(i * inc) }; };

    // cbuffer 布局(与 DEBUG_DIFF_HLSL 同步,三处同步铁律):
    // extent@0 amp@2 pad@3。
    const UINT extent[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const float amp = 20.0f;
    const UINT pad = 0u;
    cl->SetComputeRoot32BitConstants(0, 2, extent, 0);
    cl->SetComputeRoot32BitConstants(0, 1, &amp, 2);
    cl->SetComputeRoot32BitConstants(0, 1, &pad, 3);

    D3D12_RESOURCE_BARRIER pre[3]{
        Transition(_debugDiff.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Transition(slot.inputColor.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(slot.outputColor.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    cl->ResourceBarrier(3, pre);
    cl->SetComputeRootDescriptorTable(1, gpu(0));               // t0 inputColor
    cl->SetComputeRootDescriptorTable(2, gpu(kSrvOutputColor)); // t1 outputColor
    cl->SetComputeRootDescriptorTable(3, gpu(kUavDebugDiff));   // u0 debugDiff
    cl->Dispatch((static_cast<UINT>(_width) + 7) / 8,
                 (static_cast<UINT>(_height) + 7) / 8, 1);
    // input 用完即归位;debugDiff → COPY_SOURCE、output → COPY_DEST 接拷贝
    // (状态转换对先前 UAV 写入自带同步语义,无需额外 UAV barrier)。
    D3D12_RESOURCE_BARRIER mid[3]{
        Transition(slot.inputColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
        Transition(_debugDiff.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(slot.outputColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    cl->ResourceBarrier(3, mid);
    cl->CopyResource(slot.outputColor.Get(), _debugDiff.Get());
    D3D12_RESOURCE_BARRIER post[2]{
        Transition(_debugDiff.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
        Transition(slot.outputColor.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cl->ResourceBarrier(2, post);
}

void D3D12Context::RecordFlowView(FrameSlot &slot, bool useReduced, bool realMotion) noexcept {
    // 光流场调试视图(契约见头文件)。dispatch 恒按源尺寸(OutExtent),
    // 运动场 texel 在 shader 内最近邻映射 —— follow 内部场(半尺寸)也铺
    // 满输出,_debugDiff 无陈旧边缘。dispatch 写共享 _debugDiff → COPY_
    // SOURCE → 拷回 outputColor(与 RecordDebugDiff 同一条槽 CL 上原子成对;
    // 本视图不读 outputColor,免去 UAV→NSR 往返,直接 UAV→COPY_DEST→UAV)。
    ID3D12GraphicsCommandList *cl = slot.commandList.Get();
    // t0 取材:真运动帧按管线取 slot 场(已 NSR);无真运动帧绑静态零
    // 纹理(槽 35,常驻 NSR)—— 不碰 slot.motion(首 densify 前内容未定义)。
    const UINT srvIndex = !realMotion ? 35u : (useReduced ? 18u : 10u);
    cl->SetComputeRootSignature(_rsFlowView.Get());
    cl->SetPipelineState(_psoFlowView.Get());
    ID3D12DescriptorHeap *heaps[]{ slot.srvUavHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = slot.srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(i * inc) }; };

    // cbuffer 布局(与 FLOW_VIEW_HLSL 同步,三处同步铁律):
    // extent@0 outExtent@2 scale@4 pad@5。
    const UINT extent[2]{ realMotion && useReduced ? static_cast<UINT>(_internalWidth) : static_cast<UINT>(_width),
                          realMotion && useReduced ? static_cast<UINT>(_internalHeight) : static_cast<UINT>(_height) };
    const UINT outExtent[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const float scale = 0.125f; // 8px/帧满亮
    const UINT pad = 0u;
    cl->SetComputeRoot32BitConstants(0, 2, extent, 0);
    cl->SetComputeRoot32BitConstants(0, 2, outExtent, 2);
    cl->SetComputeRoot32BitConstants(0, 1, &scale, 4);
    cl->SetComputeRoot32BitConstants(0, 1, &pad, 5);

    // 入态:debugDiff→UAV / output→COPY_DEST(运动场三路取材全部 NSR 常
    // 驻或由 evaluate 消费链保持,无需屏障)。
    D3D12_RESOURCE_BARRIER pre[2]{
        Transition(_debugDiff.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Transition(slot.outputColor.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    cl->ResourceBarrier(2, pre);
    cl->SetComputeRootDescriptorTable(1, gpu(srvIndex));        // t0 运动场/零纹理
    cl->SetComputeRootDescriptorTable(2, gpu(kUavDebugDiff));   // u0 debugDiff
    cl->Dispatch((static_cast<UINT>(_width) + 7) / 8,
                 (static_cast<UINT>(_height) + 7) / 8, 1);
    // 出态:debugDiff → COPY_SOURCE 接拷贝 → 归位;真运动帧的 slot 场
    // NSR 入、NSR 出,归位仍归 recordGuidancePark(本函数不动其状态)。
    D3D12_RESOURCE_BARRIER mid[1]{
        Transition(_debugDiff.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    cl->ResourceBarrier(1, mid);
    cl->CopyResource(slot.outputColor.Get(), _debugDiff.Get());
    D3D12_RESOURCE_BARRIER post[2]{
        Transition(_debugDiff.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
        Transition(slot.outputColor.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cl->ResourceBarrier(2, post);
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

// ---------------------------------------------------------------------------
// AMD 光流后端(FFX;PSO 录制助手,由 ffxof context 编排)
// ---------------------------------------------------------------------------

bool D3D12Context::BindOfResources(ID3D12Resource *ffxInput, ID3D12Resource *ffxSparse) noexcept {
    // 每槽堆写一份;会话纹理由 context 持有、销毁走退役名单,视图内容在
    // 纹理存活期内有效。调用方保证只在会话建立时调用(PoolHold 内)。
    if (!_device) return false;
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (int i = 0; i < kSlotCount; ++i) {
        if (!_slots[i].srvUavHeap) return false;
        const D3D12_CPU_DESCRIPTOR_HANDLE slotBase =
            _slots[i].srvUavHeap->GetCPUDescriptorHandleForHeapStart();
        auto h = [&](UINT d) {
            return D3D12_CPU_DESCRIPTOR_HANDLE{ slotBase.ptr + static_cast<SIZE_T>(d * inc) };
        };
        if (ffxInput) _device->CreateUnorderedAccessView(ffxInput, nullptr, nullptr, h(49));
        if (ffxSparse) _device->CreateShaderResourceView(ffxSparse, nullptr, h(50));
    }
    return true;
}

void D3D12Context::RecordFfxPrepare(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                    uint32_t srcW, uint32_t srcH,
                                    uint32_t dstW, uint32_t dstH) noexcept {
    // inputColor(槽 0 SRV,BGRA8)→ ffxInput(49,R8G8B8A8):box 平均 + 换序。
    // inputColor 处于 NSR(convert 产出),ffxInput 的 UAV 态由调用方管理。
    ID3D12GraphicsCommandList *cl = &clRef;
    cl->SetComputeRootSignature(_rsFfxPrepare.Get());
    cl->SetPipelineState(_psoFfxPrepare.Get());
    ID3D12DescriptorHeap *heaps[]{ slot.srvUavHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = slot.srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(i * inc) }; };

    const UINT srcWH[2]{ srcW, srcH };
    const UINT dstWH[2]{ dstW, dstH };
    cl->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    cl->SetComputeRoot32BitConstants(0, 2, dstWH, 2);
    cl->SetComputeRootDescriptorTable(1, gpu(0));  // t0 inputColor(NSR)
    cl->SetComputeRootDescriptorTable(2, gpu(49)); // u0 ffxInput
    cl->Dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);
}

void D3D12Context::RecordFfxDensify(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                    uint32_t denseW, uint32_t denseH,
                                    uint32_t ofW, uint32_t ofH,
                                    uint32_t sparseW, uint32_t sparseH,
                                    float scaleX, float scaleY,
                                    UINT uavMotion, UINT uavConfidence) noexcept {
    // 稀疏流(50)→ 稠密运动 + 置信度。SourceExtent = 稠密目标尺寸
    // (dispatch 范围);VectorScale = dense/OF(源尺寸管线 = 源/OF,
    // follow 内部管线 = 会话/OF)。motion/confidence 的 UAV 态转移由
    // 调用方(postExecute lambda)负责,与 NVOF densify 同契约。
    ID3D12GraphicsCommandList *cl = &clRef;
    cl->SetComputeRootSignature(_rsFfxDensify.Get());
    cl->SetPipelineState(_psoFfxDensify.Get());
    ID3D12DescriptorHeap *heaps[]{ slot.srvUavHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = slot.srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(i * inc) }; };

    const UINT srcWH[2]{ denseW, denseH };
    const UINT ofWH[2]{ ofW, ofH };
    const UINT spWH[2]{ sparseW, sparseH };
    cl->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    cl->SetComputeRoot32BitConstants(0, 2, ofWH, 2);
    cl->SetComputeRoot32BitConstants(0, 2, spWH, 4);
    const float scale[2]{ scaleX, scaleY };
    cl->SetComputeRoot32BitConstants(0, 2, scale, 6);
    cl->SetComputeRootDescriptorTable(1, gpu(50));            // t0 sparseFlow
    cl->SetComputeRootDescriptorTable(2, gpu(uavMotion));     // u0 DenseMotion
    cl->SetComputeRootDescriptorTable(3, gpu(uavConfidence)); // u1 DenseConfidence
    cl->Dispatch((denseW + 7) / 8, (denseH + 7) / 8, 1);
}

void D3D12Context::RecordYuvOutput(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                   ColorMatrix matrix, ColorRange range,
                                   D3D12_RESOURCE_STATES outputStateBefore,
                                   ID3D12Resource *srcColor, UINT srcSrvIndex) noexcept {
    // RGB→YUV(YUV 原生化):srcColor(缺省 outputColor;FG 路径传
    // fgInterp)stateBefore(UAV 正常/直通拷贝 / NSR=FG 后)→ NSR
    // → luma + chroma 两个 dispatch 写 yuvOut(留 UAV 交
    // RecordReadbackCopy)→ srcColor 归 COMMON。constant 的 Lo/Span 按
    // 平面语义填充(luma 用 yLo/ySpan,chroma 用 cMid/cSpan)。
    // cl 由调用方显式给定(真实帧 = base CL,插值 = fg CL)。
    if (!srcColor) {
        srcColor = slot.outputColor.Get();
        srcSrvIndex = kSrvOutputColor;
    }
    ID3D12GraphicsCommandList *cl = &clRef;
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

void D3D12Context::RecordColorOutput(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                     ID3D12Resource *srcColor, UINT srcSrvIndex,
                                     bool isFp16, int srcW, int srcH,
                                     ColorMatrix matrix, ColorRange range,
                                     D3D12_RESOURCE_STATES stateBefore) noexcept {
    // RTX Video 管线的颜色输出(PIPE 尺寸源 → OUT 尺寸平面;契约同
    // RecordYuvOutput,见头文件注释)。PIPE==OUT 且 SDR 时坐标恒等映射,
    // 与旧路径逐位同价。
    ID3D12GraphicsCommandList *cl = &clRef;
    if (stateBefore != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER toNsr[1]{
            Transition(srcColor, stateBefore,
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
    ID3D12DescriptorHeap *heaps[]{ slot.srvUavHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = slot.srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(i * inc) }; };

    const YuvCoeffs cf = YuvCoeffsFor(matrix, range, _outPlaneBytes > 1 ? 10 : 8);
    ID3D12PipelineState *lumaPso = isFp16 ? _psoPqLuma.Get() : _psoConvertScaledLuma.Get();
    ID3D12PipelineState *chromaPso = isFp16 ? _psoPqChroma.Get() : _psoConvertScaledChroma.Get();
    if (!isFp16) {
        cl->SetPipelineState(lumaPso);
        {
            const UINT extent[2]{ static_cast<UINT>(_outW), static_cast<UINT>(_outH) };
            const UINT srcExtent[2]{ static_cast<UINT>(srcW), static_cast<UINT>(srcH) };
            const float consts[8]{ cf.kr, cf.kb,
                                   cf.yLo / cf.containerMax, cf.ySpan / cf.containerMax,
                                   0.0f, 0.0f, 0.0f, 0.0f };
            cl->SetComputeRoot32BitConstants(0, 2, extent, 0);
            cl->SetComputeRoot32BitConstants(0, 2, srcExtent, 2);
            cl->SetComputeRoot32BitConstants(0, 8, consts, 4);
            cl->SetComputeRootDescriptorTable(1, gpu(srcSrvIndex));
            cl->SetComputeRootDescriptorTable(2, gpu(28));
            cl->SetComputeRootDescriptorTable(3, gpu(29));
            cl->Dispatch((static_cast<UINT>(_outW) + 7) / 8, (static_cast<UINT>(_outH) + 7) / 8, 1);
        }
        cl->SetPipelineState(chromaPso);
        {
            const UINT extent[2]{ static_cast<UINT>(_outChromaW), static_cast<UINT>(_outChromaH) };
            const UINT srcExtent[2]{ static_cast<UINT>(srcW), static_cast<UINT>(srcH) };
            const float consts[8]{ cf.kr, cf.kb,
                                   cf.cMid / cf.containerMax, cf.cSpan / cf.containerMax,
                                   0.0f, 0.0f, 0.0f, 0.0f };
            cl->SetComputeRoot32BitConstants(0, 2, extent, 0);
            cl->SetComputeRoot32BitConstants(0, 2, srcExtent, 2);
            cl->SetComputeRoot32BitConstants(0, 8, consts, 4);
            cl->SetComputeRootDescriptorTable(1, gpu(srcSrvIndex));
            cl->SetComputeRootDescriptorTable(2, gpu(29));
            cl->SetComputeRootDescriptorTable(3, gpu(30));
            cl->Dispatch((static_cast<UINT>(_outChromaW) + 7) / 8, (static_cast<UINT>(_outChromaH) + 7) / 8, 1);
        }
    } else {
        // PQ:Kr/Kb = BT.2020(0.2627/0.0593);limited 10bit luma 64+876y、
        // chroma 512+896c(÷CM 折进常量,与 SDR 路径同布局)。
        constexpr float kKr2020 = 0.2627f;
        constexpr float kKb2020 = 0.0593f;
        cl->SetPipelineState(lumaPso);
        {
            const UINT extent[2]{ static_cast<UINT>(_outW), static_cast<UINT>(_outH) };
            const UINT srcExtent[2]{ static_cast<UINT>(srcW), static_cast<UINT>(srcH) };
            const float consts[8]{ kKr2020, kKb2020,
                                   64.0f / 1023.0f, 876.0f / 1023.0f,
                                   0.0f, 0.0f, 0.0f, 0.0f };
            cl->SetComputeRoot32BitConstants(0, 2, extent, 0);
            cl->SetComputeRoot32BitConstants(0, 2, srcExtent, 2);
            cl->SetComputeRoot32BitConstants(0, 8, consts, 4);
            cl->SetComputeRootDescriptorTable(1, gpu(srcSrvIndex));
            cl->SetComputeRootDescriptorTable(2, gpu(28));
            cl->SetComputeRootDescriptorTable(3, gpu(29));
            cl->Dispatch((static_cast<UINT>(_outW) + 7) / 8, (static_cast<UINT>(_outH) + 7) / 8, 1);
        }
        cl->SetPipelineState(chromaPso);
        {
            const UINT extent[2]{ static_cast<UINT>(_outChromaW), static_cast<UINT>(_outChromaH) };
            const UINT srcExtent[2]{ static_cast<UINT>(srcW), static_cast<UINT>(srcH) };
            const float consts[8]{ kKr2020, kKb2020,
                                   512.0f / 1023.0f, 896.0f / 1023.0f,
                                   0.0f, 0.0f, 0.0f, 0.0f };
            cl->SetComputeRoot32BitConstants(0, 2, extent, 0);
            cl->SetComputeRoot32BitConstants(0, 2, srcExtent, 2);
            cl->SetComputeRoot32BitConstants(0, 8, consts, 4);
            cl->SetComputeRootDescriptorTable(1, gpu(srcSrvIndex));
            cl->SetComputeRootDescriptorTable(2, gpu(29));
            cl->SetComputeRootDescriptorTable(3, gpu(30));
            cl->Dispatch((static_cast<UINT>(_outChromaW) + 7) / 8, (static_cast<UINT>(_outChromaH) + 7) / 8, 1);
        }
    }

    D3D12_RESOURCE_BARRIER back[1]{
        Transition(srcColor,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
    };
    cl->ResourceBarrier(1, back);
}

void D3D12Context::RecordMotionScale(ID3D12GraphicsCommandList &cl, FrameSlot &slot,
                                     int srcW, int srcH) noexcept {
    // 源尺寸运动场(NSR)→ motionDense(PIPE 尺寸,UAV)。motionDense 由
    // 调用方(帧路径)按 COMMON→UAV 屏障后调用,UAV→NSR 收尾归调用方。
    const UINT extent[2]{ static_cast<UINT>(_pipeW), static_cast<UINT>(_pipeH) };
    const UINT srcExtent[2]{ static_cast<UINT>(srcW), static_cast<UINT>(srcH) };
    ID3D12DescriptorHeap *heaps[]{ slot.srvUavHeap.Get() };
    cl.SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = slot.srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(i * inc) }; };
    cl.SetComputeRootSignature(_rsMotionScale.Get());
    cl.SetPipelineState(_psoMotionScale.Get());
    cl.SetComputeRoot32BitConstants(0, 2, extent, 0);
    cl.SetComputeRoot32BitConstants(0, 2, srcExtent, 2);
    cl.SetComputeRootDescriptorTable(1, gpu(10));             // t0 slot.motion SRV
    cl.SetComputeRootDescriptorTable(2, gpu(kUavMotionDense)); // u0 motionDense
    cl.Dispatch((extent[0] + 7) / 8, (extent[1] + 7) / 8, 1);
}

} // namespace vsdlssnr
