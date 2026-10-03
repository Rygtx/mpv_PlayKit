#include "d3d12_context.h"
#include "dlssnr_context.h" // TimingStatusLine

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgidebug.h>
#include <thread>
#include "d3d12_shaders.h" // 全部 HLSL(本 TU 唯一消费者)

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
    // 容器字宽:8bit 单字节;>8bit(VS 10/12/14/16)统一 16bit 容器右对齐
    // 采样字(VS 约定),R16 footprint 直传,无位移。
    c.containerMax = depth > 8 ? 65535.0f : 255.0f;
    c.sampleMax = static_cast<float>((1 << depth) - 1);
    // limited 阶梯 = 8bit 基准按位左移(10bit: 64/876/512/896;12bit:
    // 256/3504/2048/3584 —— ITU 量化表同构)。注意是 ×(1<<(depth-8)) 而非
    // ×(sampleMax/255):1023/255=4.0118 ≠ 4,等比会偏出 0.2 个码。
    const float shift = depth > 8 ? static_cast<float>(1 << (depth - 8)) : 1.0f;
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

// 槽描述符堆的句柄计算(全仓唯一实现;槽位一律传 enum HeapSlot):录制侧
// 构造即 SetDescriptorHeaps + 取 GPU 基址,创建侧用无 CL 构造仅取基址。
// 原"5 行 SetDescriptorHeaps/gpuBase/inc/gpu lambda"前奏 ×14 与创建侧
// slotHandle lambda ×5 均由此收敛。
struct HeapBinder {
    D3D12_CPU_DESCRIPTOR_HANDLE cpuBase{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuBase{};
    UINT inc = 1;
    // 录制侧:绑定堆(命令列表每 Reset 后必须重绑)并取双基址。
    HeapBinder(ID3D12Device *dev, ID3D12GraphicsCommandList &cl, FrameSlot &slot) {
        ID3D12DescriptorHeap *heaps[]{ slot.srvUavHeap.Get() };
        cl.SetDescriptorHeaps(1, heaps);
        cpuBase = slot.srvUavHeap->GetCPUDescriptorHandleForHeapStart();
        gpuBase = slot.srvUavHeap->GetGPUDescriptorHandleForHeapStart();
        inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    // 创建侧:堆刚建好,只写描述符不录命令。
    HeapBinder(ID3D12Device *dev, FrameSlot &slot) {
        cpuBase = slot.srvUavHeap->GetCPUDescriptorHandleForHeapStart();
        gpuBase = slot.srvUavHeap->GetGPUDescriptorHandleForHeapStart();
        inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT i) const {
        return D3D12_CPU_DESCRIPTOR_HANDLE{ cpuBase.ptr + static_cast<SIZE_T>(i) * inc };
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT i) const {
        return D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(i) * inc };
    }
    // 录制侧调用形态:句柄实例直接当函数用(替代原 gpu lambda)。
    D3D12_GPU_DESCRIPTOR_HANDLE operator()(UINT i) const { return gpu(i); }
};

D3D12Context::~D3D12Context() { Finalize(); }

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
    // GPU 时间戳频率(base 完成时间戳路径,2026-09-25):DIRECT 队列恒支持,
    // 失败仅关闭该路径(ProcessFrame 回退阻塞观测),不判致命。
    UINT64 tsFreq = 0;
    if (SUCCEEDED(_queue->GetTimestampFrequency(&tsFreq)) && tsFreq > 0) {
        _gpuTsFreq = static_cast<double>(tsFreq);
        _gpuTsEnabled = true;
    }
    // 计算 PSO 一次性构建(残差 + NVOF densify/guidance)。densify 不能等
    // scaling 路径才创建:OF 会话在 scaling_enabled=0 时同样要录 densify
    // (实测 2026-09-07:耦合在 CreateScalingForSlot 里导致 null PSO 崩溃)。
    return CreateComputeObjects(err, errLen);
}

void D3D12Context::Finalize() noexcept {
    // 探针:D3D12 上下文销毁只发生在 parked 上下文排干/非热路径释放。
    if (_device) TimingStatusLine("DLSSNR STATUS: d3d12 context finalized");
    // Finish 在途票据排空:停泊覆盖销毁(parked 上下文被下一次停泊顶替)时,
    // 楔住的 Finish 若仍解引用本上下文的槽资源,先排空再拆(自防御;
    // 正常宿主路径 plugin.cpp 1217-1231 在 g_lifecycleMutex 内、无在飞帧)。
    WaitFinishTicketsDrained(30000);
    if (_queue && _fence) {
        // Best-effort drain so the GPU is idle before releasing command objects.
        const uint64_t v = _fenceValue.fetch_add(1) + 1;
        _queue->Signal(_fence.Get(), v);
        if (_fence->GetCompletedValue() < v) {
            _fence->SetEventOnCompletion(v, _ctlEvent);
            WaitForSingleObject(_ctlEvent, 2000);
        }
    }
    // Slots:persist 映射经 PersistMapped::Unmap 单点释放(2026-10-04 RAII 化,
    // 清单不再两处手抄),Win32 事件在此显式关闭;其余 ComPtr 成员随槽析构。
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
            s.uploadYuv[p].Unmap();
            s.readbackYuv[p].Unmap();
        }
        for (int g = 0; g < kFgGenSlots; ++g) {
            for (int p = 0; p < 3; ++p) {
                s.readbackFg[g][p].Unmap();
            }
        }
        s.tsReadback.Unmap();
    }
    _freeCount = 0;
    // (PSO/RS 不在此逐个 Reset:成员析构统一回收 —— 原"选择性清单"既
    // 不完整也无顺序理由,假装有意义的随机子集,2026-10-04 删。)
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
    // A previous error return with the list still open makes Reset fail with
    // E_FAIL from then on; ResetAllocatorHealed force-closes once and retries
    // so a single SEH doesn't brick the control path for the rest of the session.
    return ResetAllocatorHealed(_ctlAllocator.Get(), _ctlCommandList.Get());
}

// ctl 路径失败点统一拉取:debug layer / 运行时的报错存进 info queue,
// 此前只进调试器输出,mpv 进程内看不到 —— 这就是"dump 失败 debug layer
// 无增量"的死结(观测手段缺位,非无错)。复用 DebugDumpInfoQueue 的
// ERROR/CORRUPTION 过滤(INFO/WARNING 级良性告警一 dump 批能攒 20+ 条,
// 会把真错挤出去);SetErr 自带的拉取(追加进 err,缓冲小恒截断)与其
// 互补:这里负责完整进 timing log,拉完即清防累积。无 debug 时零开销。
void D3D12Context::ReportInfoQueue(const char *where) noexcept {
    if (!_infoQueue) return;
    char buf[2048];
    DebugDumpInfoQueue(buf, sizeof(buf));
    if (!buf[0]) return;
    char line[2304];
    std::snprintf(line, sizeof(line), "DLSSNR INFOQ[%s]: %.2000s", where, buf);
    TimingStatusLine(line);
}

bool D3D12Context::ExecuteCtlAndWait(char *err, size_t errLen, const char *where) noexcept {
    HRESULT hr = _ctlCommandList->Close();
    if (FAILED(hr)) {
        // Close 失败此前完全静默(hr 丢弃、原因不可辨)—— dump 三连失败的
        // "ctl execute/wait failed" 就是它。现在:hr + info queue 消息落日志,
        // err 带回调用方。列表留 open,由 BeginCtlRecording 的 force-close
        // 恢复路径回收。
        char msg[128];
        std::snprintf(msg, sizeof(msg), "%s: ctl close failed hr=0x%08lX",
                      where, static_cast<unsigned long>(hr));
        TimingStatusLine(msg);
        ReportInfoQueue(where);
        SetErr(err, errLen, hr, msg);
        return false;
    }
    // 与分段提交(SubmitBaseFrame/SubmitFgFrame)共享 _fence:fetch_add +
    // Signal 必须同锁,否则并发槽提交会让 Signal 值乱序(fence 值回退 =
    // 驱动未定义行为)。CtlMutex → _submitMutex 的加锁顺序与分段提交
    // (仅 _submitMutex)无环。
    std::lock_guard<std::mutex> lock(_submitMutex);
    ID3D12CommandList *lists[]{ _ctlCommandList.Get() };
    _queue->ExecuteCommandLists(1, lists);
    const uint64_t v = _fenceValue.fetch_add(1) + 1;
    _queue->Signal(_fence.Get(), v);
    char reason[160]{};
    if (!WaitFenceValue(v, _ctlEvent, reason, sizeof(reason))) {
        // 等待原因此前写进丢弃缓冲 —— 连"超时还是信号失败"都无从分辨。
        char msg[224];
        std::snprintf(msg, sizeof(msg), "%s: ctl wait failed: %.150s",
                      where, reason[0] ? reason : "unknown");
        TimingStatusLine(msg);
        ReportInfoQueue(where);
        SetErr(err, errLen, E_FAIL, msg);
        return false;
    }
    return true;
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
            // surface through the host-supplied notify(面板 stats 通道由
            // 宿主层接线;设备层不直连 UI 传输,见 SetHangNotify)
            if (_hangNotify) {
                char reason[16];
                snprintf(reason, sizeof(reason), "0x%08lX",
                         static_cast<unsigned long>(rr));
                _hangNotify(reason);
            }
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
    // !_drain:排他排空期间新帧一律阻塞(否则归还的槽会被后续请求偷走,
    // PoolHold 等三槽全空被饿死 —— 2026-09-25 真机 1.5s 排空实锤)。
    _poolCv.wait(lock, [this] { return _freeCount > 0 && !_drain; });
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

// --- Finish 在途票据:见 WaitFinishTicketsDrained 注释 ----------------------

void D3D12Context::BeginFrameFinishTicket() noexcept {
    std::lock_guard<std::mutex> lock(_finishMutex);
    ++_pendingFinishTickets;
}

void D3D12Context::EndFrameFinishTicket() noexcept {
    {
        std::lock_guard<std::mutex> lock(_finishMutex);
        if (--_pendingFinishTickets < 0) _pendingFinishTickets = 0;
    }
    _finishCv.notify_all();
}

bool D3D12Context::WaitFinishTicketsDrained(int timeoutMs) noexcept {
    // wait_for 可能抛 system_error(罕见系统失败):noexcept 下转为响亮
    // 失败语义 —— 调用方放弃重建,不带着未排空的 Finish 踩堆。
    try {
        std::unique_lock<std::mutex> lock(_finishMutex);
        return _finishCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                  [this] { return _pendingFinishTickets == 0; });
    } catch (...) {
        return false;
    }
}

// --- PoolHold: drain + seal ------------------------------------------------

D3D12Context::PoolHold::PoolHold(D3D12Context &ctx) noexcept : _ctx(&ctx) {
    // Take the pool mutex and only release it when the hold ends: while held,
    // AcquireSlot blocks on this very mutex, so no new frame can start against
    // the resources the holder is about to replace. Waiting for the drain
    // inside the same lock is safe — the frames still in flight never take
    // _poolMutex again (they already own their slot), they just release.
    // 排他排空(2026-09-25):先置 _drain 阻塞新 AcquireSlot 再等三槽归还
    // —— 否则 mpv 追赶期的连续请求会不断偷走刚归还的槽,排空被饿(真机
    // 1.5s 实锤);置位后归还即满足,排空 ≈ 在飞帧自然完成。
    // 探针:排空等待 = recreate 的顿挫主体(#32 的"切档帧一次性顿挫")。
    // 每次切预设/滑块一行,量化换挡停顿;若在无用户操作时出现 = 有东西在
    // 反复触发重建。
    LARGE_INTEGER t0{}, t1{}, tf{};
    QueryPerformanceCounter(&t0);
    _lock = std::unique_lock<std::mutex>(_ctx->_poolMutex);
    _ctx->_drain = true;
    _ctx->_poolCv.notify_all(); // 唤醒已阻塞的 AcquireSlot 重新检查谓词(阻塞)
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
        _ctx->_drain = false;
        _ctx->_poolCv.notify_all(); // 锁内清旗 + 唤醒,锁外再补一次通知
        _lock.unlock();
        _ctx->_poolCv.notify_all();
    }
}

DXGI_FORMAT D3D12Context::NrColorFormat(bool allowFp16, bool vsrOnly) const noexcept {
    // Phase C 管线色策略(2026-09-24):>8bit 源默认 RGBA16F(NR 全程 10bit,
    // 消 P10→BGRA8 的 2bit 量化)。**否决制实证(320x180d10 探针)**:TrueHDR
    // 对 FP16 输入 EvaluateFeature 失败(BGRA 对照通过)→ rtx 请求时回落
    // BGRA8 并由调用方日志显形。VSDLSSNR_NR_FORMAT=fp16/bgra8/rgb10a2 强制
    // 覆盖(全矩阵验证用)。FFX OF / FG / NVOF-downsample 均 SRV 消费,
    // 格式无关,已随默认档实测。
    static const int envOverride = [] {
        char v[16]{};
        const DWORD n = GetEnvironmentVariableA("VSDLSSNR_NR_FORMAT", v, sizeof(v));
        if (n > 0 && n < sizeof(v)) {
            if (_stricmp(v, "fp16") == 0) return 1;
            if (_stricmp(v, "bgra8") == 0) return -1;
            if (_stricmp(v, "rgb10a2") == 0) return 2;
        }
        return 0;
    }();
    if (envOverride == 1) return DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (envOverride == 2) return DXGI_FORMAT_R10G10B10A2_UNORM;
    if (envOverride == -1) return DXGI_FORMAT_B8G8R8A8_UNORM;
    if (allowFp16) {
        return _bitDepth > 8 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    // VSR-only 10bit:R10G10B10A2(2026-10-02 定案)。TrueHDR 不在链上,FP16
    // 否决不适用;VSR 官方输入/输出格式面含 R10G10B10A2(rtx_video_context.h),
    // 10bit 源免 BGRA8 的 2bit 量化,经 vsrColor 同格式直达 C2。**真机实证
    // (2026-10-02,RTX 3080,1920x800d10 vsr+nr+fg×4 官方 DLSSG 路由)**:
    // NR snippet 输入/输出、DLSSG backbuffer、VSR 出入三腿全通过,零 eval
    // 失败,perf 与 BGRA8 基线逐项持平。回退:VSDLSSNR_NR_FORMAT=bgra8。
    if (vsrOnly && _bitDepth > 8) return DXGI_FORMAT_R10G10B10A2_UNORM;
    return DXGI_FORMAT_B8G8R8A8_UNORM;
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

bool D3D12Context::CreateFrameResources(const SessionDesc &desc,
                                        char *err, size_t errLen) noexcept {
    const int width = desc.width, height = desc.height;
    const int pipeW = desc.pipeW, pipeH = desc.pipeH;
    const int outW = desc.outW, outH = desc.outH;
    _width = width;
    _height = height;
    _bitDepth = desc.depth;
    _subW = desc.subW;
    _subH = desc.subH;
    _isRgb = desc.rgb;
    // 输入色度平面(VS 规则:ceil)—— 444 全分辨率、422 半宽、420 半宽半高。
    _chromaW = desc.subW ? (width + 1) >> 1 : width;
    _chromaH = desc.subH ? (height + 1) >> 1 : height;
    _fgSlots = desc.fg;
    // 抗闪烁时域资源随尺寸失效:context 级历史/引导纹理在此释放,每槽
    // temporalOut 同步作废(temporalOut 纹理由 RebuildTemporal 创建)。调用方
    // (Initialize/RecreateFeature)在本函数成功后按当前 antiFlicker 重新
    // RebuildTemporal —— _temporalMode 归 0 保证 no-op 门不误吞重建。
    for (auto &t : _tempHist) t.Reset();
    for (auto &t : _tempGuide) t.Reset();
    for (auto &t : _tempLow) t.Reset();
    for (int i = 0; i < kSlotCount; ++i) _slots[i].temporalOut.Reset();
    _temporalMode = 0;
    // 实验性补帧 HDR 域插帧:仅 HDR 会话有意义(HDR 关时本值无论真假管线
    // 等价 —— 插值输出恒经 SDR 域直通路径)。
    _fgHdrInterp = desc.fgHdrInterp && desc.hdr;
    // RTX Video 双尺寸定格。守卫:pipe/out 必须落在 [源, 合理上界] 内,
    // 越界 = 调用方换算 bug,按无 RTX 兜底(与占位视图语义一致)。
    _vsrSlots = desc.vsr && pipeW >= width && pipeH >= height &&
                pipeW <= width * 8 && pipeH <= height * 8;
    _hdrPipe = desc.hdr;
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
    // SDR 输出 = 输入布局(420 半 / 422 半高宽全 / 444 全);HDR P10 恒 420。
    _outChromaW = (_hdrPipe ? 1 : desc.subW) ? _outW >> 1 : _outW;
    _outChromaH = (_hdrPipe ? 1 : desc.subH) ? _outH >> 1 : _outH;
    // HDR 输出 = 恒 P10(PQ BT.2020 limited);SDR 输出 = 源位深同格式。
    _outPlaneBytes = (_hdrPipe || _bitDepth > 8) ? 2u : 1u;
    _outFmt = _outPlaneBytes > 1 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    // 管线色格式(NR input/output 缓冲):>8bit 且无 RTX = RGBA16F(NR 全程
    // 10bit,消 P10→BGRA8 的 2bit 量化);VSR-only 10bit = R10G10B10A2(见
    // NrColorFormat 注释);其余 RTX 会话回落 BGRA8(TrueHDR 拒 FP16,否决制
    // 实证 2026-09-24)。CreateSlotResources 按此建缓冲(vsrColor 同格式)。
    _inColorFmt = NrColorFormat(!desc.hdr && !desc.vsr, desc.vsr && !desc.hdr);
    // 新组合的留痕由 dlssnr_context 的 "color buffer" 观察行承担(格式名
    // 三态 + 回退提示),此处不再重复发行。

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
    const bool needDepthPipe = desc.fg && _vsrSlots && (_pipeW != width || _pipeH != height);
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
        if (!ExecuteCtlAndWait(err, errLen, "guidance clear")) return false;
    }

    if (ProbeEnabled()) TimingStatusLine("PROBE: d3d12 frame-res before slot loop");
    for (int i = 0; i < kSlotCount; ++i) {
        if (ProbeEnabled()) {
            char pb[48];
            snprintf(pb, sizeof(pb), "PROBE: d3d12 slot %d begin", i);
            TimingStatusLine(pb);
        }
        if (!CreateSlotResources(_slots[i], desc.depth, err, errLen)) return false;
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
        slot.uploadYuv[i].Unmap();
        slot.readbackYuv[i].Unmap();
    }
    for (int g = 0; g < kFgGenSlots; ++g) {
        for (int i = 0; i < 3; ++i) slot.readbackFg[g][i].Unmap();
    }
    slot.commandList.Reset();
    slot.allocator.Reset();
    slot.fgCommandList.Reset();
    slot.fgAllocator.Reset();
    slot.postCommandList.Reset();
    slot.postAllocator.Reset();
    // base 完成时间戳路径资源(2026-09-25):persist-mapped READBACK 先解映射;
    // 括号 CL 对按 TsSlot 数组统一释放(含 RTX 段 —— 旧具名形态曾漏列,
    // 靠 ComPtr 重赋值兜底;PoolHold 保证此处全部空闲)。
    for (int i = 0; i < kTsSlotCount; ++i) {
        slot.ts[i].cl.Reset();
        slot.ts[i].alloc.Reset();
    }
    slot.tsQueryHeap.Reset();
    slot.tsReadback.Unmap();
    slot.submitQpc = 0;
    slot.tsGpuCal = 0;
    slot.tsCpuCal = 0;
    slot.tsValid = false;
    for (int i = 0; i < 3; ++i) {
        slot.yuvOut[i].Reset();
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
    for (int g = 0; g < kFgGenSlots; ++g) {
        slot.fgInterp[g].Reset();
        slot.hdrFg[g].Reset();
    }
    slot.fgBack.Reset();
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
    // postB 转换段 CL(与 fg 同型;仅 HDR 会话录制提交,闲置无害)。
    hr = _device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(slot.postAllocator.GetAddressOf()));
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "CreateCommandAllocator(slot post) failed");
        return false;
    }
    hr = _device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.postAllocator.Get(), nullptr,
        IID_PPV_ARGS(slot.postCommandList.GetAddressOf()));
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "CreateCommandList(slot post) failed");
        return false;
    }
    hr = slot.postCommandList->Close();
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "Close initial slot post command list failed");
        return false;
    }
    // postB 段无专用事件:postB 栅栏值消费方经 WaitFenceValuePublic 用
    // fenceEvent/调用方自备事件等待,postFenceEvent 从无消费者(2026-09-25
    // 删除 —— 原实现还构成重建路径句柄泄漏)。

    // 颜色缓冲格式:>8bit 且无 RTX = RGBA16F(NR 全程 10bit,消 P10→BGRA8
    // 的 2bit 量化);RTX 会话回落 BGRA8(TrueHDR 拒 FP16,否决制实证
    // 2026-09-24)。ALLOW_UNORDERED_ACCESS:YUV→RGB 转换 dispatch UAV 直写
    //(NGX 对 UAV-flag 纹理做 SRV 读有 reducedColor/motion 前科)。
    if (!CreateColorTexture(slot.inputColor.GetAddressOf(), width, height,
                            _inColorFmt, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    // NGX binds the output as a UAV; a D3D12-native resource requires
    // ALLOW_UNORDERED_ACCESS (Magpie's D3D11-shared texture had no such flag
    // constraint, our native one does).
    if (!CreateColorTexture(slot.outputColor.GetAddressOf(), width, height,
                            _inColorFmt, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    // RTX Video 管线纹理(PIPE 尺寸;请求了才建,描述符侧占位视图)。
    // vsrColor: VSR 输出,跟随管线色(_inColorFmt;官方 VSR 输入/输出格式族
    // R8G8B8A8/B8G8R8A8/R10G10B10A2 全覆盖 BGRA8/R10G10B10A2 两档 —— 落
    // BGRA8 会让 R10G10B10A2 管线的量化在 VSR 输出侧卷土重来。HDR 会话管线
    // 色恒 BGRA8 = TrueHDR 输入契约。输出需 UAV 标志,SDK D3D12 样例同款)。
    // hdrColor: TrueHDR 输出 FP16 scRGB(FG backbuffer / PQ 转换源)。
    // motionDense: FG MVecs 的 PIPE 尺寸版本(PIPE==src 时无纹理,eval
    // 直用 motion)。创建顺序在描述符块之前 —— NULL 描述符 TDR 铁律。
    if (_vsrSlots &&
        !CreateColorTexture(slot.vsrColor.GetAddressOf(), _pipeW, _pipeH,
                            _inColorFmt, D3D12_RESOURCE_STATE_COMMON,
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

    // YUV 平面纹理:Out = 颜色→YUV dispatch 写出供回读(OUT 尺寸 —— RTX
    // VSR 放大后输出平面在目标尺寸;无 RTX 时 OUT=src 同旧管线)。In 侧
    // 纹理已撤(2026-10-02):typed buffer SRV 直读 upload 堆。R8_UNORM
    // (8bit)/R16_UNORM(10bit,存储字 = VS P10 采样值,右对齐 0-1023 ——
    // 2026-09-08 BlankClip 实测;UNORM 读出 word/65535,round(×65535) 精确
    // 还原整数采样值;buffer SRV 同构)。
    for (int i = 0; i < 3; ++i) {
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
                             slot.uploadYuv[i].res.GetAddressOf(), slot.uploadPitchYuv[i],
                             bytesPerRow, err, errLen)) {
            return false;
        }
        hr = slot.uploadYuv[i].res->Map(0, nullptr, &slot.uploadYuv[i].mapped);
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
                             slot.readbackYuv[i].res.GetAddressOf(), slot.readbackPitchYuv[i],
                             outBytesPerRow, err, errLen)) {
            return false;
        }
        hr = slot.readbackYuv[i].res->Map(0, nullptr, &slot.readbackYuv[i].mapped);
        if (FAILED(hr)) {
            SetErr(err, errLen, hr, "Map(readbackYuv) failed");
            return false;
        }
    }
    // FG 槽纹理/回读缓冲必须在描述符块之前创建:fgInterp 逐 gen 的 SRV
    // (槽 39-43)以它们为目标 —— 纹理不存在时 CreateShaderResourceView
    // (nullptr)= NULL 描述符,本机驱动异步 TDR(见 14-17 注释;2026-09-11
    // fg 路径首测实锤,失败在下一个 CreateCommittedResource 才浮出)。
    if (_fgSlots && !CreateFgSlotResources(slot, err, errLen)) return false;
    {
        // slot-local shader-visible descriptor heap; input/output descriptors
        // here, the residual pipeline descriptors on every scaling rebuild.
        // 35 = 0..31 原有 + 32/33(编号留空不重排,无视图)+ 34(共享差异
        // 调试纹理 UAV)+ 36/37/38(RTX:vsrColor/
        // hdrColor 的 SRV、motionDense 的 UAV;无 RTX 槽 = outputColor/motion
        // 占位视图)+ 39-43(fgInterp 逐 gen SRV)+ 44-48(hdrFg 逐 gen SRV)。
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = kHeapSlotCount; // 布局唯一权威 = enum HeapSlot
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = _device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(slot.srvUavHeap.GetAddressOf()));
        if (FAILED(hr)) {
            SetErr(err, errLen, hr, "CreateDescriptorHeap(SRV/UAV slot) failed");
            return false;
        }
        // 堆里绝不写 NULL 描述符(本机驱动 RTX 3080 上 CreateShaderResourceView
        // (nullptr,nullptr) 曾触发异步 TDR,2026-09-07 二分定位)—— 所有
        // 尚无真资源的槽位一律绑带 UAV flag 的 outputColor/inputColor 占位。
        HeapBinder heap{ _device.Get(), slot };
        _device->CreateShaderResourceView(slot.inputColor.Get(), nullptr, heap.cpu(HeapSlot::SrvInput));
        _device->CreateUnorderedAccessView(slot.outputColor.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavOutput));
        // densify 的 motion/confidence SRV+UAV(源尺寸)。
        _device->CreateShaderResourceView(slot.motion.Get(), nullptr, heap.cpu(HeapSlot::SrvMotion));
        _device->CreateShaderResourceView(slot.confidence.Get(), nullptr, heap.cpu(HeapSlot::SrvConfidence));
        _device->CreateUnorderedAccessView(slot.motion.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavMotion));
        _device->CreateUnorderedAccessView(slot.confidence.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavConfidence));
        // 14-17(SrvFlowF..SrvCostB)预置为 inputColor 的占位视图(NVOF 会话
        // 建立时由 BindNvofResources 覆盖为 flow/cost 视图;densify 只在会话
        // 存活时被记录,占位视图永不被有效读取)。
        _device->CreateShaderResourceView(slot.inputColor.Get(), nullptr, heap.cpu(HeapSlot::SrvFlowF));
        _device->CreateShaderResourceView(slot.inputColor.Get(), nullptr, heap.cpu(HeapSlot::SrvFlowB));
        _device->CreateShaderResourceView(slot.inputColor.Get(), nullptr, heap.cpu(HeapSlot::SrvCostF));
        _device->CreateShaderResourceView(slot.inputColor.Get(), nullptr, heap.cpu(HeapSlot::SrvCostB));
        // SrvOutputColor:outputColor 的 SRV(RGB→YUV 转换读;降采样直接采样
        // 槽 0 srvInput)。UavNvofInput0/1:NVOF 注册输入纹理的 UAV 占位,
        // BindNvofResources 在会话建立时覆盖为真值。
        _device->CreateShaderResourceView(slot.outputColor.Get(), nullptr, heap.cpu(HeapSlot::SrvOutputColor));
        _device->CreateUnorderedAccessView(slot.outputColor.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavNvofInput0));
        _device->CreateUnorderedAccessView(slot.outputColor.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavNvofInput1));
        // SrvYuvIn0-2:uploadYuv 的 typed buffer SRV(YUV→RGB 直读 upload 堆,
        // 2026-10-02;tsrv_probe 复核的形态,UPLOAD 堆恒 GENERIC_READ 全读态
        // 免屏障);UavYuvOut0-2:yuvOut 的 UAV(RGB→YUV 写出);UavInput:
        // inputColor 的 UAV(YUV→RGB 直写)。
        for (int i = 0; i < 3; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = _bitDepth > 8 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.NumElements = static_cast<UINT>(
                slot.uploadYuv[i].res->GetDesc().Width / (_bitDepth > 8 ? 2u : 1u));
            _device->CreateShaderResourceView(slot.uploadYuv[i].res.Get(), &srv,
                                              heap.cpu(static_cast<HeapSlot>(HeapSlot::SrvYuvIn0 + i)));
            _device->CreateUnorderedAccessView(slot.yuvOut[i].Get(), nullptr, nullptr,
                                               heap.cpu(static_cast<HeapSlot>(HeapSlot::UavYuvOut0 + i)));
        }
        _device->CreateUnorderedAccessView(slot.inputColor.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavInput));
        // 32/33(Unused32/33):逐 gen 转换视图(39-43)接手后已无消费者,不再
        // 建视图 —— 未绑定的堆区间从不被读,安全;编号留空不重排。
        // UavDebugDiff:共享差异调试纹理的 UAV(资源 = context 级 _debugDiff,
        // 已在 CreateFrameResources 槽池循环前创建;每槽堆各持一份指向同一
        // 资源的视图,dispatch 只被本槽 CL 引用)。
        _device->CreateUnorderedAccessView(_debugDiff.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavDebugDiff));
        // SrvZeroMotion:静态零运动纹理的 SRV(资源 = context 级 _motion,
        // 槽池循环前创建,常驻 NSR)—— 光流场调试视图在无真运动帧(播种/
        // OF 关)绑定它:slot.motion 的内容在首 densify 前未定义,直接绑会
        // 显示假流;零纹理保证"黑 = 无光流数据"的语义成立。
        _device->CreateShaderResourceView(_motion.Get(), nullptr, heap.cpu(HeapSlot::SrvZeroMotion));
        // SrvVsrColor/SrvHdrColor/UavMotionDense:RTX Video(占位 =
        // outputColor/motion,资源带 UAV flag;RTX 未激活时永不被有效读取
        // —— 对应的 Record 调用只在 RTX 管线被记录)。
        _device->CreateShaderResourceView(slot.vsrColor ? slot.vsrColor.Get()
                                                        : slot.outputColor.Get(),
                                          nullptr, heap.cpu(HeapSlot::SrvVsrColor));
        _device->CreateShaderResourceView(slot.hdrColor ? slot.hdrColor.Get()
                                                        : slot.outputColor.Get(),
                                          nullptr, heap.cpu(HeapSlot::SrvHdrColor));
        _device->CreateUnorderedAccessView(slot.motionDense ? slot.motionDense.Get()
                                                            : slot.motion.Get(),
                                           nullptr, nullptr, heap.cpu(HeapSlot::UavMotionDense));
        // SrvFgInterp0-4:fgInterp[0..4] 逐 gen SRV(转换读 / TrueHDR 输入读
        // —— NGX 走资源参数不需要描述符,这里只服务我们自己的转换 pass)。
        // 非 FG 槽 = outputColor 占位(永不有效读取)。
        for (int g = 0; g < kFgGenSlots; ++g) {
            _device->CreateShaderResourceView(_fgSlots ? slot.fgInterp[g].Get()
                                                       : slot.outputColor.Get(),
                                              nullptr, heap.cpu(static_cast<HeapSlot>(HeapSlot::SrvFgInterp0 + g)));
        }
        // SrvHdrFg0-4:hdrFg[0..4] 逐 gen SRV(TrueHDR 插值帧输出,PQ 转换读)。
        // 非 HDR 槽 = hdrColor/outputColor 占位。
        for (int g = 0; g < kFgGenSlots; ++g) {
            _device->CreateShaderResourceView(slot.hdrFg[g] ? slot.hdrFg[g].Get()
                                            : slot.hdrColor ? slot.hdrColor.Get()
                                                            : slot.outputColor.Get(),
                                              nullptr, heap.cpu(static_cast<HeapSlot>(HeapSlot::SrvHdrFg0 + g)));
        }
        // UavFgBack:fgBack 的 UAV(PQ 域插帧编码 pass 写,HdrToPq)。非实验槽 =
        // outputColor 占位(永不有效读取 —— 对应 Record 调用只在实验模式被
        // 记录;资源带 UAV flag,满足"绝不写 NULL 描述符"惯例)。
        _device->CreateUnorderedAccessView(slot.fgBack ? slot.fgBack.Get()
                                                       : slot.outputColor.Get(),
                                           nullptr, nullptr, heap.cpu(HeapSlot::UavFgBack));
        // 52-65 抗闪烁时域占位:关闭态 = inputColor(SRV 侧)/outputColor
        // (UAV 侧),RebuildTemporal 在 PoolHold 内覆盖写真视图 —— 时域
        // Record 只在 TemporalMode()>0 时被记录,占位永不有效读取。
        for (HeapSlot d : {HeapSlot::SrvTemporalOut, HeapSlot::Hist0Srv, HeapSlot::Hist1Srv,
                           HeapSlot::Guide0Srv, HeapSlot::Guide1Srv,
                           HeapSlot::Low0Srv, HeapSlot::Low1Srv}) {
            _device->CreateShaderResourceView(slot.inputColor.Get(), nullptr, heap.cpu(d));
        }
        for (HeapSlot d : {HeapSlot::UavTemporalOut, HeapSlot::Hist0Uav, HeapSlot::Hist1Uav,
                           HeapSlot::Guide0Uav, HeapSlot::Guide1Uav,
                           HeapSlot::Low0Uav, HeapSlot::Low1Uav}) {
            _device->CreateUnorderedAccessView(slot.outputColor.Get(), nullptr, nullptr, heap.cpu(d));
        }
    }

    // base 完成时间戳资源(2026-09-25;GpuTsEnabled 时):查询堆 + 微型 CL +
    // READBACK 回读缓冲(8 字节 UINT64,persist-mapped,初始态 COPY_DEST =
    // ResolveQueryData 目标契约,缓冲资源恒驻 COPY_DEST 无屏障)。
    if (_gpuTsEnabled) {
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 16; // 0..5 常驻段 6..11 RTX 段 14/15 TS-INLINE 探针(布局见头文件)
        qh.NodeMask = 0;
        if (FAILED(_device->CreateQueryHeap(&qh, IID_PPV_ARGS(slot.tsQueryHeap.GetAddressOf())))) {
            SetErr(err, errLen, E_FAIL, "CreateQueryHeap(ts) failed");
            return false;
        }
        // 括号 CL 对按 TsSlot 数组全量建立(Post0/Post1 = post CL 内联打点,
        // 无独立括号,跳过)。同帧内多对在飞,须各自独立 allocator(在飞
        // Reset = UB)。
        const auto createBracket = [&](TsBracket &b) -> bool {
            if (FAILED(_device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(b.alloc.GetAddressOf()))) ||
                FAILED(_device->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT, b.alloc.Get(), nullptr,
                    IID_PPV_ARGS(b.cl.GetAddressOf()))) ||
                FAILED(b.cl->Close())) {
                SetErr(err, errLen, E_FAIL, "create ts command path failed");
                return false;
            }
            return true;
        };
        for (int i = 0; i < kTsSlotCount; ++i) {
            if (i == kTsPost0 || i == kTsPost1) continue;
            if (!createBracket(slot.ts[i])) return false;
        }
        D3D12_HEAP_PROPERTIES rbHeap{};
        rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rbDesc{};
        rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rbDesc.Width = 128; // 16×UINT64(布局见头文件)
        rbDesc.Height = 1;
        rbDesc.DepthOrArraySize = 1;
        rbDesc.MipLevels = 1;
        rbDesc.SampleDesc.Count = 1;
        rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(_device->CreateCommittedResource(
                &rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(slot.tsReadback.res.GetAddressOf())))) {
            SetErr(err, errLen, E_FAIL, "create ts readback buffer failed");
            return false;
        }
        slot.tsReadback.res->Map(0, nullptr, &slot.tsReadback.mapped); // void 返回;persist-mapped
        if (!slot.tsReadback.mapped) {
            SetErr(err, errLen, E_FAIL, "map ts readback buffer failed");
            return false;
        }
    }
    return true;
}

// DLSS FG 槽资源(仅 _fgSlots):插值输出纹理(逐 gen)+ 第二组回读缓冲。
// RTX 双尺寸:fgInterp[g] 在 PIPE 尺寸(backbuffer 同侧)。格式:默认恒
// BGRA8(DLSSG 在 SDR 域插值,逐输出帧 TrueHDR 提升);实验开关 fgHdrInterp
// = FP16 **PQ 码域**(DLSSG 在感知码域插帧 —— ColorBuffersHDR=1 对 >1.0
// scRGB 不保真,码域 ≤1.0 走 LDR 路径无损,2026-09-24 定案;hdrFg 不建 ——
// 插值输出即 FG 产物,postA 后码域直读转换)。fgBack = 该模式的 DLSSG
// backbuffer(HdrToPq 编码 pass 写入)。回读缓冲 = OUT 尺寸(与真实帧一致)。
bool D3D12Context::CreateFgSlotResources(FrameSlot &slot, char *err, size_t errLen) noexcept {
    const DXGI_FORMAT fgInterpFmt = _fgHdrInterp ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                 : DXGI_FORMAT_B8G8R8A8_UNORM;
    for (int g = 0; g < kFgGenSlots; ++g) {
        if (!CreateColorTexture(slot.fgInterp[g].GetAddressOf(), _pipeW, _pipeH,
                                fgInterpFmt,
                                D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
            return false;
        }
    }
    if (_fgHdrInterp) {
        if (!CreateColorTexture(slot.fgBack.GetAddressOf(), _pipeW, _pipeH,
                                DXGI_FORMAT_R16G16B16A16_FLOAT,
                                D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
            return false;
        }
    }
    if (_hdrPipe && !_fgHdrInterp) {
        for (int g = 0; g < kFgGenSlots; ++g) {
            if (!CreateColorTexture(slot.hdrFg[g].GetAddressOf(), _pipeW, _pipeH,
                                    DXGI_FORMAT_R16G16B16A16_FLOAT,
                                    D3D12_RESOURCE_STATE_COMMON,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
                return false;
            }
        }
    }
    for (int g = 0; g < kFgGenSlots; ++g) {
        for (int i = 0; i < 3; ++i) {
            const int pw = i == 0 ? _outW : _outChromaW;
            const int ph = i == 0 ? _outH : _outChromaH;
            const UINT bytesPerRow = static_cast<UINT>(pw) * _outPlaneBytes;
            if (!CreateRawBuffer(static_cast<UINT64>(ph) * bytesPerRow, D3D12_HEAP_TYPE_READBACK,
                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                 slot.readbackFg[g][i].res.GetAddressOf(), slot.readbackPitchFg[g][i],
                                 bytesPerRow, err, errLen)) {
                return false;
            }
            const HRESULT hr = slot.readbackFg[g][i].res->Map(0, nullptr, &slot.readbackFg[g][i].mapped);
            if (FAILED(hr)) {
                SetErr(err, errLen, hr, "Map(readbackFg) failed");
                return false;
            }
        }
    }
    return true;
}

// pack/unpack 并行行拷贝:最小工作单元 = 平面(或 Y 的上下对半)。
struct CopyRowSpan {
    const uint8_t *src;
    int64_t srcStride;
    uint8_t *dst;
    int64_t dstStride;
    int rows;
    size_t rowBytes;
};

void CopyRows(const CopyRowSpan &s) noexcept {
    // 行距相等且等于行宽 = 区间内存连续 → 整块一次 memcpy(常见宽度
    // 3840/1920 的行宽天然 256 对齐,upload/readback pitch 与 VS stride
    // 相等,几乎总走这条;奇宽等不连续场景才逐行)。
    if (s.srcStride == s.dstStride && static_cast<int64_t>(s.rowBytes) == s.srcStride) {
        memcpy(s.dst, s.src, s.rowBytes * static_cast<size_t>(s.rows));
        return;
    }
    const uint8_t *srcRow = s.src;
    uint8_t *dstRow = s.dst;
    for (int y = 0; y < s.rows; ++y, srcRow += s.srcStride, dstRow += s.dstStride) {
        memcpy(dstRow, srcRow, s.rowBytes);
    }
}

// 区间拆分:每平面对半,恒 6 区间。最大区间 = 最大平面的一半(并行
// 总耗时由它决定),与采样族无关:420 杆 = Y/2 = 总量 1/3;422 = 1/4;
// 444/RGBP 三平面等大 = 各 1/6。无需统计帧大小。
constexpr int kMaxCopySpans = 6;

// 恒并行(线程创建 ~20µs/个,拷贝本体数百 µs 起,恒为正收益)。最后
// 一个区间留在调用线程;线程创建失败(资源耗尽,极罕见)时该区间由
// 调用线程就地串行补拷 —— 区间不跳过,输出内容不缺。
void CopyPlanesRows(CopyRowSpan *spans, int count) noexcept {
    if (count < 2) {
        for (int i = 0; i < count; ++i) CopyRows(spans[i]);
        return;
    }
    std::thread workers[kMaxCopySpans - 1];
    int launched = 0;
    for (int i = 0; i < count - 1; ++i) {
        try {
            workers[launched++] = std::thread(CopyRows, spans[i]);
        } catch (...) {
            CopyRows(spans[i]); // 线程创建失败:就地拷,内容不丢
        }
    }
    CopyRows(spans[count - 1]);
    for (int i = 0; i < launched; ++i) workers[i].join();
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
    // [0]=Y 全分辨率,[1]/[2]=U/V 采样平面;行距 256 对齐,VS stride 对齐。
    // 恒并行拷贝:每平面对半拆(拆分规则见 kMaxCopySpans)。4K P10 单线程
    // ~1.5ms → 并行 ~0.5ms。
    CopyRowSpan spans[kMaxCopySpans];
    int count = 0;
    for (int p = 0; p < 3; ++p) {
        const int pw = p == 0 ? width : _chromaW;
        const int ph = p == 0 ? height : _chromaH;
        const size_t rowBytes = static_cast<size_t>(pw) * (_bitDepth > 8 ? 2u : 1u);
        const uint8_t *srcRow = srcPlanes[p];
        uint8_t *dstRow = static_cast<uint8_t *>(slot.uploadYuv[p].mapped);
        const int64_t dstPitch = static_cast<int64_t>(slot.uploadPitchYuv[p]);
        const int half = ph / 2;
        spans[count++] = { srcRow, srcStrides[p], dstRow, dstPitch, half, rowBytes };
        spans[count++] = { srcRow + srcStrides[p] * half, srcStrides[p],
                           dstRow + dstPitch * half, dstPitch, ph - half, rowBytes };
    }
    CopyPlanesRows(spans, count);
    return true;
}

bool D3D12Context::BeginFrameRecording(FrameSlot &slot) noexcept {
    // 上一帧录制中途失败遗留 open CL 会让 Reset 从此 E_FAIL;自愈一次
    // (ResetAllocatorHealed = 全仓唯一实现,见 d3d12_context.h)。
    return ResetAllocatorHealed(slot.allocator.Get(), slot.commandList.Get());
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

    // 1) 转换 dispatch:typed buffer SRV 直读 uploadYuv(2026-10-02 起;
    // upload→yuvIn 的 3×拷贝与 yuvIn 纹理已撤 —— tsrv_probe 复核的驱动
    // 形态)。UPLOAD 堆 buffer 恒 GENERIC_READ(全读态),SRV 绑定免屏障;
    // CPU 写(persist-mapped 行拷贝)→ GPU 读的同步不变(提交序)。
    // 采样字平铺索引:x + y*Pitch(Pitch 以采样字计,@14/15)。
    D3D12_RESOURCE_BARRIER toUav[1]{
        Transition(slot.inputColor.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cl->ResourceBarrier(1, toUav);
    const YuvCoeffs cf = YuvCoeffsFor(matrix, range, _bitDepth);
    cl->SetComputeRootSignature(_rsConvertIn.Get());
    cl->SetPipelineState(_isRgb ? _psoConvertInRgb.Get() : _psoConvertIn.Get());
    HeapBinder gpu{ _device.Get(), *cl, slot };
    const UINT extent[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const float consts[8]{ cf.containerMax, cf.yLo, 1.0f / cf.ySpan, cf.cMid,
                           1.0f / cf.cSpan, cf.kr, cf.kb, 0.0f };
    cl->SetComputeRoot32BitConstants(0, 2, extent, 0);
    cl->SetComputeRoot32BitConstants(0, 8, consts, 2);
    // ChromaExtent/ChromaScale(@10-13):420=(半,0.5) 与旧硬编码恒等;
    // 422=(半宽,1)、444=(全,(1,1) 零插值)。PSO 由 _isRgb 选(RGB 核不
    // 消费色度常量)。
    const UINT chromaExtent[2]{ static_cast<UINT>(_chromaW), static_cast<UINT>(_chromaH) };
    const float chromaScale[2]{ _subW ? 0.5f : 1.0f, _subH ? 0.5f : 1.0f };
    cl->SetComputeRoot32BitConstants(0, 2, chromaExtent, 10);
    cl->SetComputeRoot32BitConstants(0, 2, chromaScale, 12);
    const UINT texelBytes = _bitDepth > 8 ? 2u : 1u;
    const UINT planePitches[2]{
        static_cast<UINT>(slot.uploadPitchYuv[0]) / texelBytes,
        static_cast<UINT>(slot.uploadPitchYuv[1]) / texelBytes,
    };
    cl->SetComputeRoot32BitConstants(0, 2, planePitches, 14);
    cl->SetComputeRootDescriptorTable(1, gpu(HeapSlot::SrvYuvIn0)); // t0 PlaneY
    cl->SetComputeRootDescriptorTable(2, gpu(HeapSlot::SrvYuvIn1)); // t1 PlaneU
    cl->SetComputeRootDescriptorTable(3, gpu(HeapSlot::SrvYuvIn2)); // t2 PlaneV
    cl->SetComputeRootDescriptorTable(4, gpu(HeapSlot::UavInput)); // u0 inputColor
    cl->Dispatch((static_cast<UINT>(_width) + 7) / 8, (static_cast<UINT>(_height) + 7) / 8, 1);

    // 2) inputColor → stateAfter(NSR=NGX 待读;COMMON=skipEval)。单屏障
    //    (yuvIn 归位屏障已随纹理删除)。
    D3D12_RESOURCE_BARRIER tail[1];
    tail[0] = Transition(slot.inputColor.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, stateAfter);
    cl->ResourceBarrier(1, tail);
}

bool D3D12Context::RecordReadbackCopy(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                      char *err, size_t errLen,
                                      int fgGen) noexcept {
    // RecordColorOutput 之后调用:yuvOut 处于 UAV 态 → COPY_SOURCE → 拷贝
    // → COMMON。三平面各自 footprint(格式同资源,跨格式 = 静默 E_INVALIDARG)。
    // fgGen >= 0 = 拷入 FG 插值帧第 fgGen 组回读缓冲(真实帧与各插值槽先后
    // 转换+回读,共用 yuvOut,目标缓冲两两不同)。cl 由调用方显式给定
    // (真实帧 = base CL,插值 = fg CL)。
    ID3D12GraphicsCommandList *cl = &clRef;
    const DXGI_FORMAT yuvFmt = _outFmt;
    const int planeW[3]{ _outW, _outChromaW, _outChromaW };
    const int planeH[3]{ _outH, _outChromaH, _outChromaH };
    // 屏障合批(2026-09-25):三平面 UAV→COPY_SOURCE 一批、拷贝、
    // COPY_SOURCE→COMMON 一批(此前逐平面 1+1,FG 6x 每帧 36 次驱动调用;
    // 平面间无依赖,纯重排)。RecordConvertInput 同款写法。
    D3D12_RESOURCE_BARRIER toCopySrc[3];
    for (int i = 0; i < 3; ++i) {
        toCopySrc[i] = Transition(slot.yuvOut[i].Get(),
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    cl->ResourceBarrier(3, toCopySrc);
    for (int i = 0; i < 3; ++i) {
        ID3D12Resource *dstBuf = fgGen >= 0 ? slot.readbackFg[fgGen][i].res.Get()
                                            : slot.readbackYuv[i].res.Get();
        const size_t dstPitch = fgGen >= 0 ? slot.readbackPitchFg[fgGen][i]
                                           : slot.readbackPitchYuv[i];
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
    }
    D3D12_RESOURCE_BARRIER backToCommon[3];
    for (int i = 0; i < 3; ++i) {
        backToCommon[i] = Transition(slot.yuvOut[i].Get(),
                                     D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    cl->ResourceBarrier(3, backToCommon);
    return true;
}

bool D3D12Context::BeginFgRecording(FrameSlot &slot) noexcept {
    // 与 BeginFrameRecording 同款防砖:上次录制中途失败遗留 open CL 会让
    // allocator Reset 报 E_FAIL —— force-close 一次再试。
    return ResetAllocatorHealed(slot.fgAllocator.Get(), slot.fgCommandList.Get());
}

// 分段提交共用体:Close 目标 CL → 队列执行 → signal 全局栅栏新值。提交互斥
// 保证栅栏值顺序与队列 ExecuteCommandLists 顺序一致(值永不回退)。
bool D3D12Context::SubmitBaseFrame(FrameSlot &slot, char *err, size_t errLen) noexcept {
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
    // 排队段锚(2026-09-25 账目诚实化):base 纯执行的起点由 preBase 括号
    // 给出;preBase 时刻 − 本入口 QPC = 帧间排队(burst 等前帧尾部),单独
    // 成段,不折进任何处理段。
    slot.submitQpc = s0.QuadPart;
    // 本帧擦账(2026-10-02):readback 常驻映射跨帧残留 —— 某括号录制失败时
    // 该格子留旧帧 tick,与另一半新章混减 = 虚高/无符号回绕巨值(Finish 检
    // "0 = 本帧没盖章")。清零时槽刚从池子取出:上帧 WaitFrame 已闭合或设备
    // 丢失域,GPU 不会再写,CPU 独占安全;64 字节擦除开销可忽略。
    if (slot.tsReadback.mapped) std::memset(slot.tsReadback.mapped, 0, 128);
    const bool preBaseOk = RecordTsBracket(slot, slot.ts[kTsPreBase].alloc.Get(),
                                           slot.ts[kTsPreBase].cl.Get(), kTsPreBase);
    if (preBaseOk) {
        ID3D12CommandList *tsLists[]{ slot.ts[kTsPreBase].cl.Get() };
        _queue->ExecuteCommandLists(1, tsLists);
    }
    ID3D12CommandList *lists[]{ slot.commandList.Get() };
    _queue->ExecuteCommandLists(1, lists);
    slot.baseFenceValue = _fenceValue.fetch_add(1) + 1;
    _queue->Signal(_fence.Get(), slot.baseFenceValue);
    // postBase 括号(索引 1)+ 校准:微型 CL 紧随 base 提交 —— 同一把
    // _submitMutex 保证队列序 [preBase][base][postBase](跨线程
    // ExecuteCommandLists 无全序,无锁并发提交会让 ts 排到他人段之前)。
    // EndQuery + Resolve 进 READBACK;无栅栏信号:post CL FIFO 在其后,
    // WaitFrame 蕴含全部 ts 完成,Finish 读回零等待。校准点
    // (GetClockCalibration)在提交时采样,GPU tick → QPC 线性换算在 Finish
    // 做。失败非致命:tsValid=false,Finish 回退栅栏差分账目(旧行为)。
    // 2026-09-25 修:括号 CL 此前只录制未 ECL,EndQuery/Resolve 从未在 GPU
    // 执行 → readback 恒零 → NR 开着 gpu 段恒 0(真机实锤)。
    slot.tsValid = false;
    if (_gpuTsEnabled && preBaseOk) {
        if (RecordTsBracket(slot, slot.ts[kTsPostBase].alloc.Get(),
                            slot.ts[kTsPostBase].cl.Get(), kTsPostBase)) {
            ID3D12CommandList *tsLists[]{ slot.ts[kTsPostBase].cl.Get() };
            _queue->ExecuteCommandLists(1, tsLists);
            UINT64 gpuCal = 0, cpuCal = 0;
            if (SUCCEEDED(_queue->GetClockCalibration(&gpuCal, &cpuCal))) {
                slot.tsGpuCal = gpuCal;
                slot.tsCpuCal = cpuCal;
                slot.tsValid = true;
            }
        }
    }
    return true;
}

bool D3D12Context::RecordTsBracket(FrameSlot &slot, ID3D12CommandAllocator *alloc,
                                   ID3D12GraphicsCommandList *cl, UINT tsIdx) noexcept {
    if (!_gpuTsEnabled) return false;
    if (!ResetAllocatorHealed(alloc, cl)) return false;
    cl->EndQuery(slot.tsQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsIdx);
    cl->ResolveQueryData(slot.tsQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                         tsIdx, 1, slot.tsReadback.res.Get(), tsIdx * 8); // void 返回
    return SUCCEEDED(cl->Close());
}

bool D3D12Context::SubmitTsBracket(FrameSlot &slot, ID3D12CommandQueue *rtxQueue,
                                   ID3D12CommandAllocator *alloc,
                                   ID3D12GraphicsCommandList *cl, UINT tsIdx,
                                   ID3D12Fence *waitFence, uint64_t waitValue) noexcept {
    if (!_gpuTsEnabled || !rtxQueue) return false;
    if (!RecordTsBracket(slot, alloc, cl, tsIdx)) return false;
    // pre 括号的生产者等待:与被测 eval 的等待同 fence 同值 —— 章落点 =
    // 段纯执行起点(不含队列空闲);post 括号 fence 传空(FIFO 紧贴前序)。
    if (waitFence && waitValue) rtxQueue->Wait(waitFence, waitValue);
    ID3D12CommandList *lists[]{ cl };
    rtxQueue->ExecuteCommandLists(1, lists);
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
    // preFg/postFg 括号(2026-09-25 账目诚实化):DLSSG 纯执行 = postFg −
    // preFg,不含冲刷点引擎等待与队列积压。失败仅放弃本段账目。
    const bool preFgOk = RecordTsBracket(slot, slot.ts[kTsPreFg].alloc.Get(),
                                         slot.ts[kTsPreFg].cl.Get(), kTsPreFg);
    if (preFgOk) {
        ID3D12CommandList *tsLists[]{ slot.ts[kTsPreFg].cl.Get() };
        _queue->ExecuteCommandLists(1, tsLists);
    }
    ID3D12CommandList *lists[]{ slot.fgCommandList.Get() };
    _queue->ExecuteCommandLists(1, lists);
    if (preFgOk) {
        if (RecordTsBracket(slot, slot.ts[kTsPostFg].alloc.Get(),
                            slot.ts[kTsPostFg].cl.Get(), kTsPostFg)) {
            ID3D12CommandList *tsLists[]{ slot.ts[kTsPostFg].cl.Get() };
            _queue->ExecuteCommandLists(1, tsLists);
        }
    }
    slot.fgFenceValue = _fenceValue.fetch_add(1) + 1;
    slot.fenceValue = slot.fgFenceValue;
    _queue->Signal(_fence.Get(), slot.fenceValue);
    return true;
}

bool D3D12Context::BeginPostRecording(FrameSlot &slot) noexcept {
    // 与 BeginFgRecording 同款防砖:上次录制中途失败遗留 open CL 会让
    // allocator Reset 报 E_FAIL —— force-close 一次再试。
    if (!ResetAllocatorHealed(slot.postAllocator.Get(), slot.postCommandList.Get())) return false;
    // post CL 首时间戳(2026-09-25 账目诚实化):post 是自绘 shader(无
    // NGX,"同 CL 打点 SEH" 的单次观测只涉及 NGX 段),首尾同 CL 打点量出纯执行时间
    // —— 其跨队列栅栏等待(A/B)与队列积压属排队,不计入 conv。
    if (_gpuTsEnabled) {
        slot.postCommandList->EndQuery(slot.tsQueryHeap.Get(),
                                       D3D12_QUERY_TYPE_TIMESTAMP, kTsPost0);
    }
    return true;
}

bool D3D12Context::SubmitPostFrame(FrameSlot &slot,
                                   ID3D12Fence *waitFenceA, uint64_t waitValueA,
                                   ID3D12Fence *waitFenceB, uint64_t waitValueB,
                                   char *err, size_t errLen) noexcept {
    if (_gpuTsEnabled) {
        slot.postCommandList->EndQuery(slot.tsQueryHeap.Get(),
                                       D3D12_QUERY_TYPE_TIMESTAMP, kTsPost1);
        slot.postCommandList->ResolveQueryData(
            slot.tsQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, kTsPost0, 2,
            slot.tsReadback.res.Get(), kTsPost0 * 8); // void 返回;槽 4/5 = 字节偏移 32
    }
    HRESULT hr = slot.postCommandList->Close();
    if (FAILED(hr)) {
        SetErr(err, errLen, hr, "Close(slot post) failed");
        return false;
    }
    std::lock_guard<std::mutex> lock(_submitMutex);
    // post CL = 常驻输出转换段(全部形态):消费 RTX 专用队列产出
    // (vsrColor/hdrColor/hdrFg)与 RTX 链尾(TrueHDR 链)—— 跨队列 Wait
    // 全部排在 Execute 前。A/B 双等待:legacy HDR 会话同时消费 hdrColor
    // (A = hdrDoneFence)与 fgInterp 回读就绪(B = fgFence);单生产者形态
    // B 留空。
    if (waitFenceA && waitValueA) {
        _queue->Wait(waitFenceA, waitValueA);
    }
    if (waitFenceB && waitValueB) {
        _queue->Wait(waitFenceB, waitValueB);
    }
    ID3D12CommandList *lists[]{ slot.postCommandList.Get() };
    _queue->ExecuteCommandLists(1, lists);
    slot.postFenceValue = _fenceValue.fetch_add(1) + 1;
    slot.fenceValue = slot.postFenceValue;
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
    // 源是 WC(write-combined)映射内存:单核读串行化严重,并行收益比
    // Pack 侧更大。写法与 PackInput 同款(每平面对半,恒并行,见
    // kMaxCopySpans;4K P10 ~25MB)。
    CopyRowSpan spans[kMaxCopySpans];
    int count = 0;
    for (int p = 0; p < 3; ++p) {
        const int pw = p == 0 ? width : _outChromaW;
        const int ph = p == 0 ? height : _outChromaH;
        const size_t rowBytes = static_cast<size_t>(pw) * _outPlaneBytes;
        const uint8_t *srcRow = static_cast<const uint8_t *>(
            fgGen >= 0 ? slot.readbackFg[fgGen][p].mapped : slot.readbackYuv[p].mapped);
        const int64_t srcPitch64 = static_cast<int64_t>(
            fgGen >= 0 ? slot.readbackPitchFg[fgGen][p] : slot.readbackPitchYuv[p]);
        const int half = ph / 2;
        spans[count++] = { srcRow, srcPitch64, dstPlanes[p], dstStrides[p], half, rowBytes };
        spans[count++] = { srcRow + srcPitch64 * half, srcPitch64,
                           dstPlanes[p] + dstStrides[p] * half, dstStrides[p],
                           ph - half, rowBytes };
    }
    CopyPlanesRows(spans, count);
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
// 通用 compute 根签名工厂:b0(numConsts)+ srvCount 张独立 SRV 表 +
// uavCount 张独立 UAV 表。参数序 = 常量、SRV、UAV —— 全仓录制点按此绑定
// 表索引(SetComputeRootDescriptorTable(1..N)),改序 = 全部 Record* 失配。
// 此前该形态在 CreateComputeObjects 内手写 8 份(每份 60-90 行),唯一真
// 差异只有个数;"同表重叠 range 非法"禁忌由工厂内独立表参数构造性规避。
// sampler 非空 = 追加静态采样器(时域稳定器专用)。
bool CreateComputeRs(ID3D12Device *device, UINT numConsts, UINT srvCount, UINT uavCount,
                     ID3D12RootSignature **rs, const char *label,
                     const D3D12_STATIC_SAMPLER_DESC *sampler,
                     char *err, size_t errLen) noexcept {
    // err 直写(本 helper 是自由函数,SetErr 是 D3D12Context 成员)。
    auto fail = [&](const char *what) {
        if (err && errLen) std::snprintf(err, errLen, "%s: %s", label, what);
        return false;
    };
    D3D12_DESCRIPTOR_RANGE srvRanges[8]{};
    for (UINT i = 0; i < srvCount; ++i) {
        srvRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[i].NumDescriptors = 1;
        srvRanges[i].BaseShaderRegister = i;
        srvRanges[i].OffsetInDescriptorsFromTableStart = 0;
    }
    D3D12_DESCRIPTOR_RANGE uavRanges[4]{};
    for (UINT i = 0; i < uavCount; ++i) {
        uavRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRanges[i].NumDescriptors = 1;
        uavRanges[i].BaseShaderRegister = i;
        uavRanges[i].OffsetInDescriptorsFromTableStart = 0;
    }
    D3D12_ROOT_PARAMETER params[13]{};
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
    if (sampler) {
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = sampler;
    }
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
    return true;
}

// 对既有根签名编译一个入口并建 PSO(与 CreateComputeRs 配对;多入口共用
// 一张根签名时循环本函数)。
bool CreateComputePsoFor(ID3D12Device *device, const char *hlsl, const char *entry,
                         ID3D12RootSignature *rs, ID3D12PipelineState **pso,
                         const char *label, char *err, size_t errLen) noexcept {
    auto fail = [&](const char *what) {
        if (err && errLen) std::snprintf(err, errLen, "%s: %s", label, what);
        return false;
    };
    ComPtr<ID3DBlob> code, csErr;
    if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entry, "cs_5_0",
                          0, 0, code.GetAddressOf(), csErr.GetAddressOf()))) {
        return fail(csErr ? static_cast<const char *>(csErr->GetBufferPointer())
                          : "D3DCompile failed");
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rs;
    psoDesc.CS = { code->GetBufferPointer(), code->GetBufferSize() };
    if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(pso)))) {
        return fail("CreateComputePipelineState failed");
    }
    return true;
}

// AMD 光流后端通用 cs_5_0 PSO 构造(CreateComputeRs + CreateComputePsoFor
// 的单入口封装,FFX/motion scale 三处调用保留原签名)。
bool CreateOfPso(ID3D12Device *device, const char *hlsl, const char *entry,
                 UINT numConsts, UINT srvCount, UINT uavCount,
                 ID3D12RootSignature **rs, ID3D12PipelineState **pso,
                 const char *label, char *err, size_t errLen) noexcept {
    return CreateComputeRs(device, numConsts, srvCount, uavCount, rs, label,
                           nullptr, err, errLen) &&
           CreateComputePsoFor(device, hlsl, entry, *rs, pso, label, err, errLen);
}

} // namespace

bool D3D12Context::DumpYuvInPlane(FrameSlot &s, int plane, const wchar_t *path) noexcept {
    // dump_yuvin_*:uploadYuv 已无 Texture2D 中转,从 persist 映射直落 ——
    // 逐行拷 pw×texel 字节(挤掉 pitch 填充,与旧 DumpTextureToFile 布局
    // 一致,python 参考脚本按此读)。调用点 = Finish dump 锁存(WaitFrame
    // 后,GPU 对本帧 upload 的读已收敛,映射读取无竞争)。
    const int pw = plane == 0 ? _width : _chromaW;
    const int ph = plane == 0 ? _height : _chromaH;
    const UINT texel = _bitDepth > 8 ? 2u : 1u;
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return false;
    const UINT8 *row = static_cast<const UINT8 *>(s.uploadYuv[plane].mapped);
    const UINT pitch = static_cast<UINT>(s.uploadPitchYuv[plane]);
    const size_t rowBytes = static_cast<size_t>(pw) * texel;
    for (int y = 0; y < ph; ++y, row += pitch) {
        fwrite(row, 1, rowBytes, f);
    }
    fclose(f);
    return true;
}

bool D3D12Context::DumpTextureToFile(ID3D12Resource *tex, const wchar_t *path,
                                     char *err, size_t errLen) noexcept {
    // 几何/格式取资源自身 desc(头注释:传参版三次踩坑的根修)。
    const D3D12_RESOURCE_DESC desc = tex->GetDesc();
    const UINT width = static_cast<UINT>(desc.Width);
    const UINT height = desc.Height;
    const DXGI_FORMAT format = desc.Format;
    UINT bpp = 4u; // R8G8B8A8/BGRA8/R16G16 系
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) bpp = 8u;
    else if (format == DXGI_FORMAT_R16_UNORM) bpp = 2u;
    else if (format == DXGI_FORMAT_R8_UNORM) bpp = 1u;
    size_t pitch = 0;
    ComPtr<ID3D12Resource> buffer;
    char ignore[96];
    // buffer 初始态传 COMMON(运行时对 buffer 忽略 InitialState 并告警,
    // COPY_DEST 只是攒 INFOQ 噪音;拷入本就允许 COMMON 态 buffer)。
    if (!CreateRawBuffer(static_cast<UINT64>(height) * width * bpp, D3D12_HEAP_TYPE_READBACK,
                         D3D12_RESOURCE_STATE_COMMON, buffer.GetAddressOf(),
                         pitch, width * bpp, ignore, sizeof(ignore))) {
        SetErr(err, errLen, E_FAIL, "dump readback buffer create failed");
        return false;
    }
    // 失败自愈重试一次(2026-09-25):Close 静默失败曾把整批后续 dump 拖成
    // "连环失败"(open 列表要等下次 force-close 才回收,当次已丢)。重试经
    // BeginCtlRecording 的 force-close 恢复,当批内就地回收;首试失败在执行
    // 之前(命令未生效),重录同样的屏障/拷贝状态一致。等待侧失败(GPU 级)
    // 仍有 deviceLost 大声路径,重试不会掩盖。
    char execErr[160]{};
    for (int attempt = 1; ; ++attempt) {
        if (!BeginCtlRecording()) {
            SetErr(err, errLen, E_FAIL, "dump begin ctl recording failed");
            return false;
        }
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
        if (ExecuteCtlAndWait(execErr, sizeof(execErr), "dump")) break;
        if (attempt >= 2) {
            SetErr(err, errLen, E_FAIL, execErr[0] ? execErr : "dump ctl execute/wait failed");
            return false;
        }
        TimingStatusLine("DLSSNR STATUS: dump ctl retry once");
    }

    void *mapped = nullptr;
    if (FAILED(buffer->Map(0, nullptr, &mapped))) {
        SetErr(err, errLen, E_FAIL, "dump readback map failed");
        return false;
    }
    FILE *f = nullptr;
    errno_t fe = _wfopen_s(&f, path, L"wb");
    bool ok = fe == 0 && f;
    if (ok) {
        for (int y = 0; y < height; ++y) {
            fwrite(static_cast<const uint8_t *>(mapped) + static_cast<size_t>(pitch) * y, 1,
                   static_cast<size_t>(width) * bpp, f);
        }
        fclose(f);
    } else {
        SetErr(err, errLen, E_FAIL, "dump fopen failed");
    }
    buffer->Unmap(0, nullptr);
    // 失败原因带出(2026-09-25):此前四个失败点全部静默 return false,
    // 调用方只有一行 "dump X FAILED" —— "为什么"无迹可查。
    if (!ok) SetErr(err, errLen, E_FAIL, "dump write failed");
    return ok;
}

bool D3D12Context::CreateComputeObjects(char *err, size_t errLen) noexcept {
    // 全部根签名/PSO 经 CreateComputeRs + CreateComputePsoFor 表驱动建立
    // (参数序 = b0 常量、SRV 表、UAV 表 —— 录制点按序绑表索引;此前 8 块
    // 手写样板每块 60-90 行,唯一真差异就是个数与 HLSL)。PSOs are
    // stateless and shared; the shader-visible descriptor heap is per-slot
    // (each slot's textures get their own SRV/UAV descriptors).

    // 残差五段:b0 = 12 root constants (Magpie ResampleConstants, 48B),
    // t0/t1 独立 SRV 表(the passes need non-adjacent descriptor pairs)+ u0。
    if (!CreateComputeRs(_device.Get(), 12, 2, 1, _rsCompute.GetAddressOf(),
                         "compute", nullptr, err, errLen)) {
        return false;
    }
    {
        struct Cso { const char *hlsl; const char *entry; ID3D12PipelineState **pso; };
        const Cso csos[] = {
            { DOWNSAMPLE_HLSL, "DownsampleColorVertical", _psoDownsampleVertical.GetAddressOf() },
            { DOWNSAMPLE_HLSL, "DownsampleColorHorizontal", _psoDownsampleHorizontal.GetAddressOf() },
            { RESIDUAL_PREPARE_HLSL, "PrepareResidual", _psoPrepare.GetAddressOf() },
            { RESIDUAL_HORIZONTAL_HLSL, "UpsampleResidualHorizontal", _psoHorizontal.GetAddressOf() },
            { RESIDUAL_VERTICAL_HLSL, "CompositeResidualVertical", _psoVertical.GetAddressOf() },
        };
        for (const auto &cso : csos) {
            if (!CreateComputePsoFor(_device.Get(), cso.hlsl, cso.entry, _rsCompute.Get(),
                                     cso.pso, "compute", err, errLen)) {
                return false;
            }
        }
    }

    // NVOF guidance(PORTING #6):densify = t0-t3 SRV 表 + u0/u1 独立 UAV 表
    // + b0 10 常量(srcWH 2+flowWH 2+flags 4+MotionScale 2);guidance 降采样
    // = t0/t1 SRV 表 + u0/u1 独立 UAV 表 + b0 12 常量(独立表参数,同表重叠
    // range 非法 —— 禁忌由 CreateComputeRs 构造性规避)。
    if (!CreateComputeRs(_device.Get(), 10, 4, 2, _rsDensify.GetAddressOf(),
                         "densify", nullptr, err, errLen) ||
        !CreateComputePsoFor(_device.Get(), DENSIFY_HLSL, "Densify", _rsDensify.Get(),
                             _psoDensify.GetAddressOf(), "densify", err, errLen)) {
        return false;
    }
    if (!CreateComputeRs(_device.Get(), 12, 2, 2, _rsGuidance.GetAddressOf(),
                         "guidance", nullptr, err, errLen) ||
        !CreateComputePsoFor(_device.Get(), GUIDANCE_DOWNSAMPLE_HLSL, "DownsampleGuidance",
                             _rsGuidance.Get(), _psoGuidanceDownsample.GetAddressOf(),
                             "guidance", err, errLen)) {
        return false;
    }
    // 光流输入 GPU 降采样(#46/#48):b0 5 常量 + t0 单表 + u0 单表。
    if (ProbeEnabled()) TimingStatusLine("PROBE: pso base ok");
    if (!CreateComputeRs(_device.Get(), 5, 1, 1, _rsNvofDownsample.GetAddressOf(),
                         "nvof downsample", nullptr, err, errLen) ||
        !CreateComputePsoFor(_device.Get(), NVOF_DOWNSAMPLE_HLSL, "NvofDownsample",
                             _rsNvofDownsample.Get(), _psoNvofDownsample.GetAddressOf(),
                             "nvof downsample", err, errLen)) {
        return false;
    }
    if (ProbeEnabled()) TimingStatusLine("PROBE: pso nvofds done");
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
    // 差异调试视图:b0 4 常量(extent 2+amp 1+pad 1)+ t0/t1(input/output)
    // + u0(共享 _debugDiff)。
    if (!CreateComputeRs(_device.Get(), 4, 2, 1, _rsDebugDiff.GetAddressOf(),
                         "debug diff", nullptr, err, errLen) ||
        !CreateComputePsoFor(_device.Get(), DEBUG_DIFF_HLSL, "DebugDiffMain",
                             _rsDebugDiff.Get(), _psoDebugDiff.GetAddressOf(),
                             "debug diff", err, errLen)) {
        return false;
    }
    // 光流场调试视图:b0 6 常量(extent 2+outExtent 2+scale 1+pad 1)+ t0
    // (运动场,SrvMotion/SrvReducedMotion 按取材二选一)+ u0(_debugDiff 中转)。
    if (!CreateComputeRs(_device.Get(), 6, 1, 1, _rsFlowView.GetAddressOf(),
                         "flow view", nullptr, err, errLen) ||
        !CreateComputePsoFor(_device.Get(), FLOW_VIEW_HLSL, "FlowViewMain",
                             _rsFlowView.Get(), _psoFlowView.GetAddressOf(),
                             "flow view", err, errLen)) {
        return false;
    }
    // 抗闪烁时域稳定器:b0 14 常量(Size2/UseMotion/Route/Weight/
    // MotionExtent2/LowSize2/Pad + Region4)+ t0-t7 八张独立 SRV 表 +
    // u0/u1/u2 三张独立 UAV 表 + 线性静态采样器。主 shader 一份覆盖
    // Route 1-4,mode4 另有半分辨率 reduce —— 两个 PSO 无条件常驻(PSO
    // 生命周期跟着"使用条件"而不是"首次搭车路径",#43-①)。
    {
        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samp.MaxLOD = D3D12_FLOAT32_MAX;
        samp.ShaderRegister = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        if (!CreateComputeRs(_device.Get(), 14, 8, 3, _rsTemporal.GetAddressOf(),
                             "temporal", &samp, err, errLen)) {
            return false;
        }
        const struct {
            const char *hlsl;
            const char *entry;
            ComPtr<ID3D12PipelineState> *pso;
        } temporalShaders[] = {
            { TEMPORAL_MAIN_HLSL, "main", &_psoTemporalMain },
            { TEMPORAL_REDUCE_HLSL, "main", &_psoTemporalReduce },
        };
        for (const auto &ts : temporalShaders) {
            if (!CreateComputePsoFor(_device.Get(), ts.hlsl, ts.entry, _rsTemporal.Get(),
                                     ts.pso->GetAddressOf(), "temporal", err, errLen)) {
                return false;
            }
        }
    }
    // YUV↔RGB 转换(YUV 原生化):convertIn = t0/t1/t2 三张 SRV 表(Y/U/V)
    // + u0 一张 UAV 表(inputColor)+ b0 16 常量(extent 2+C0 4+C1 4+
    // chromaExtent 2+chromaScale 2+pitch 2);convertOut = t0 一张 SRV 表
    // + u0/u1/u2 三张 UAV 表(Y;U,V)+ b0 14 常量。深度/矩阵/范围全在
    // 常量里(R8/R16_UNORM 的 float 视图同构)。
    if (!CreateComputeRs(_device.Get(), 16, 3, 1, _rsConvertIn.GetAddressOf(),
                         "convert in", nullptr, err, errLen)) {
        return false;
    }
    if (!CreateComputePsoFor(_device.Get(), YUV_TO_BGRA_HLSL, "ConvertYuvToBgra",
                             _rsConvertIn.Get(), _psoConvertIn.GetAddressOf(),
                             "convert in", err, errLen) ||
        // Phase B:VS RGBP 直读核(同 RS/cbuffer,平面序 G/B/R 在核内换位)。
        !CreateComputePsoFor(_device.Get(), YUV_TO_BGRA_HLSL, "ConvertRgbToRgba",
                             _rsConvertIn.Get(), _psoConvertInRgb.GetAddressOf(),
                             "convert in rgb", err, errLen)) {
        return false;
    }
    if (!CreateComputeRs(_device.Get(), 14, 1, 3, _rsConvertOut.GetAddressOf(),
                         "convert out", nullptr, err, errLen)) {
        return false;
    }
    {
        struct OutCso { const char *entry; ID3D12PipelineState **pso; };
        const OutCso outCsos[]{
            { "BgraToYuvLuma", _psoConvertOutLuma.GetAddressOf() },
            { "BgraToYuvChroma", _psoConvertOutChroma.GetAddressOf() },
            // RTX Video 输出转换:与上面共用 _rsConvertOut,仅换 HLSL 入口。
            { "ScaledToYuvLuma", _psoConvertScaledLuma.GetAddressOf() },
            { "ScaledToYuvChroma", _psoConvertScaledChroma.GetAddressOf() },
            { "PqToYuvLuma", _psoPqLuma.GetAddressOf() },
            { "PqToYuvChroma", _psoPqChroma.GetAddressOf() },
            // PQ 域插帧(fgHdrInterp):编码 + 码域→P10。共用 _rsConvertOut
            // 布局;HdrToPq 只消费 u0,u1/u2 表绑占位描述符(根参数必须全绑)。
            { "HdrToPq", _psoHdrToPq.GetAddressOf() },
            { "PqCodesToYuvLuma", _psoPqCodesLuma.GetAddressOf() },
            { "PqCodesToYuvChroma", _psoPqCodesChroma.GetAddressOf() },
            // Phase B:RGBP 出侧(直写 + RTX 缩放版;u0/u1/u2 = G/B/R)。
            { "RgbaToRgbPlanes", _psoRgbOut.GetAddressOf() },
            { "ScaledToRgbPlanes", _psoRgbScaled.GetAddressOf() },
        };
        constexpr const char *kOutHlslByEntry[] = {
            BGRA_TO_YUV_HLSL, BGRA_TO_YUV_HLSL,
            COLOR_TO_YUV_SCALED_HLSL, COLOR_TO_YUV_SCALED_HLSL,
            FP16_TO_YUV_PQ_HLSL, FP16_TO_YUV_PQ_HLSL,
            HDR_TO_PQ_HLSL, PQ_CODES_TO_YUV_HLSL, PQ_CODES_TO_YUV_HLSL,
            RGB_TO_PLANAR_HLSL, RGB_TO_PLANAR_HLSL,
        };
        for (size_t ci = 0; ci < std::size(outCsos); ++ci) {
            if (!CreateComputePsoFor(_device.Get(), kOutHlslByEntry[ci], outCsos[ci].entry,
                                     _rsConvertOut.Get(), outCsos[ci].pso,
                                     "convert out", err, errLen)) {
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

bool D3D12Context::RebuildTemporal(int mode, char *err, size_t errLen) noexcept {
    // 替换 context 级历史纹理与每槽 temporalOut;调用方必须持 PoolHold
    // (同 RebuildScaling:所有槽空闲、池封死,无在飞帧引用)。mode 与
    // 当前一致时 no-op(调用方的逐帧同步据此免锁快路径)。
    mode = std::clamp(mode, 0, 4);
    if (mode == _temporalMode) return true;

    // 全量释放(降档/换档/换尺寸统一走这里;纹理 Release 即可,视图随后
    // 覆盖为占位 —— 绝不遗留悬空视图)。
    for (auto &t : _tempHist) t.Reset();
    for (auto &t : _tempGuide) t.Reset();
    for (auto &t : _tempLow) t.Reset();
    for (int i = 0; i < kSlotCount; ++i) _slots[i].temporalOut.Reset();
    _temporalMode = 0;
    if (mode == 0) {
        // 视图回占位(inputColor/outputColor —— placeholder 惯例,绝不 NULL
        // 描述符)。布局 = enum HeapSlot 时域区。
        for (int i = 0; i < kSlotCount; ++i) {
            HeapBinder h{ _device.Get(), _slots[i] };
            for (HeapSlot d : {HeapSlot::SrvTemporalOut, HeapSlot::Hist0Srv, HeapSlot::Hist1Srv,
                               HeapSlot::Guide0Srv, HeapSlot::Guide1Srv,
                               HeapSlot::Low0Srv, HeapSlot::Low1Srv}) {
                _device->CreateShaderResourceView(_slots[i].inputColor.Get(), nullptr, h.cpu(d));
            }
            for (HeapSlot d : {HeapSlot::UavTemporalOut, HeapSlot::Hist0Uav, HeapSlot::Hist1Uav,
                               HeapSlot::Guide0Uav, HeapSlot::Guide1Uav,
                               HeapSlot::Low0Uav, HeapSlot::Low1Uav}) {
                _device->CreateUnorderedAccessView(_slots[i].outputColor.Get(), nullptr, nullptr, h.cpu(d));
            }
        }
        return true;
    }

    // 历史/引导 ping-pong(全尺寸 FP16,alpha = 有效性标记)+ mode4 半分辨率
    // low。初始内容不定义是安全的:首个 temporal dispatch 的 weight=0
    // (状态无效)不读历史,NextHistory/NextGuide/low 全量覆写。
    for (int i = 0; i < 2; ++i) {
        if (!CreateColorTexture(_tempHist[i].GetAddressOf(), _width, _height,
                                DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen) ||
            !CreateColorTexture(_tempGuide[i].GetAddressOf(), _width, _height,
                                DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
            return false;
        }
    }
    if (mode == 4) {
        for (int i = 0; i < 2; ++i) {
            if (!CreateColorTexture(_tempLow[i].GetAddressOf(), (_width + 1) >> 1, (_height + 1) >> 1,
                                    DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
                return false;
            }
        }
    }
    // 每槽 temporalOut(管线色同格式 UAV,COMMON 起步;纹理先于视图 ——
    // NULL 描述符异步 TDR 防线,#50 同款顺序铁律)。格式随管线色:下游
    // (VSR/TrueHDR/转换)消费的就是"outputColor 会给出的东西"。
    for (int i = 0; i < kSlotCount; ++i) {
        if (!CreateColorTexture(_slots[i].temporalOut.GetAddressOf(), _width, _height,
                                _inColorFmt, D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
            return false;
        }
    }
    // 视图写入(context 级纹理 × 每槽堆;占位全覆盖)。
    for (int i = 0; i < kSlotCount; ++i) {
        HeapBinder h{ _device.Get(), _slots[i] };
        _device->CreateShaderResourceView(_tempHist[0].Get(), nullptr, h.cpu(HeapSlot::Hist0Srv));
        _device->CreateShaderResourceView(_tempHist[1].Get(), nullptr, h.cpu(HeapSlot::Hist1Srv));
        _device->CreateShaderResourceView(_tempGuide[0].Get(), nullptr, h.cpu(HeapSlot::Guide0Srv));
        _device->CreateShaderResourceView(_tempGuide[1].Get(), nullptr, h.cpu(HeapSlot::Guide1Srv));
        _device->CreateUnorderedAccessView(_tempHist[0].Get(), nullptr, nullptr, h.cpu(HeapSlot::Hist0Uav));
        _device->CreateUnorderedAccessView(_tempHist[1].Get(), nullptr, nullptr, h.cpu(HeapSlot::Hist1Uav));
        _device->CreateUnorderedAccessView(_tempGuide[0].Get(), nullptr, nullptr, h.cpu(HeapSlot::Guide0Uav));
        _device->CreateUnorderedAccessView(_tempGuide[1].Get(), nullptr, nullptr, h.cpu(HeapSlot::Guide1Uav));
        _device->CreateUnorderedAccessView(_slots[i].temporalOut.Get(), nullptr, nullptr, h.cpu(HeapSlot::UavTemporalOut));
        _device->CreateShaderResourceView(_slots[i].temporalOut.Get(), nullptr, h.cpu(HeapSlot::SrvTemporalOut));
        // mode 1-3 的 low 视图必须回占位(4→2/3 降档时旧视图悬空指向已
        // 释放纹理;Route!=4 不读 t6/t7 但绝不留悬空描述符);
        // mode 4 绑真纹理。mode<4 时 _tempLow 为空 —— 不可 CreateSRV(NULL)。
        if (mode == 4) {
            _device->CreateShaderResourceView(_tempLow[0].Get(), nullptr, h.cpu(HeapSlot::Low0Srv));
            _device->CreateShaderResourceView(_tempLow[1].Get(), nullptr, h.cpu(HeapSlot::Low1Srv));
            _device->CreateUnorderedAccessView(_tempLow[0].Get(), nullptr, nullptr, h.cpu(HeapSlot::Low0Uav));
            _device->CreateUnorderedAccessView(_tempLow[1].Get(), nullptr, nullptr, h.cpu(HeapSlot::Low1Uav));
        } else {
            _device->CreateShaderResourceView(_slots[i].inputColor.Get(), nullptr, h.cpu(HeapSlot::Low0Srv));
            _device->CreateShaderResourceView(_slots[i].inputColor.Get(), nullptr, h.cpu(HeapSlot::Low1Srv));
            _device->CreateUnorderedAccessView(_slots[i].outputColor.Get(), nullptr, nullptr, h.cpu(HeapSlot::Low0Uav));
            _device->CreateUnorderedAccessView(_slots[i].outputColor.Get(), nullptr, nullptr, h.cpu(HeapSlot::Low1Uav));
        }
    }
    _temporalMode = mode;
    char msg[128];
    std::snprintf(msg, sizeof(msg),
                  "DLSSNR STATUS: temporal rebuilt route=%d history=%dx%d", mode, _width, _height);
    TimingStatusLine(msg);
    return true;
}

void D3D12Context::RecordTemporal(FrameSlot &slot, int mode, int next, bool useMotion,
                                  UINT motionSrvIndex, UINT motionW, UINT motionH,
                                  float weight) noexcept {
    // 上游 DLSSNRTemporal::Draw 的移植:mode4 先 reduce(半分辨率残差+引导)
    // 后主 pass。t0/t1 都绑 inputColor(单 pass 链 base=输入,上游 count==1
    // 时 input/base 同纹理);t2 = outputColor(NR 链原始输出,调用点保证
    // UAV→NSR 契约由本函数屏障自管)。useMotion=false 时 t5 照绑(占位),
    // shader 侧 UseMotion=0 全路径守卫绝不读取。
    // 状态时间线(与现架构对齐,调用点 = base CL 的 eval 段末、evalLock 域内):
    //   入:outputColor UAV(eval/残差帧末不变量)、inputColor COMMON、
    //      hist/guide[prev] COMMON→NSR、hist/guide[next]+low COMMON→UAV;
    //   出:temporalOut **COMMON**(显式落位,下游 VSR/FG/转换的 from-state
    //      全按 COMMON 对接 —— 不依赖 NGX 衰减语义)、outputColor/inputColor
    //      归 COMMON(下游不再消费)、hist/guide/low 归 COMMON(UAV 跨 ECL
    //      不自动衰减,显式归位保帧末全资源 COMMON 不变量)。
    ID3D12GraphicsCommandList *cl = slot.commandList.Get();
    const int previous = next ^ 1;

    D3D12_RESOURCE_BARRIER pre[9];
    UINT preCount = 0;
    pre[preCount++] = Transition(slot.outputColor.Get(),
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    pre[preCount++] = Transition(slot.inputColor.Get(),
                                 D3D12_RESOURCE_STATE_COMMON,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    pre[preCount++] = Transition(slot.temporalOut.Get(),
                                 D3D12_RESOURCE_STATE_COMMON,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    pre[preCount++] = Transition(_tempHist[next].Get(),
                                 D3D12_RESOURCE_STATE_COMMON,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    pre[preCount++] = Transition(_tempGuide[next].Get(),
                                 D3D12_RESOURCE_STATE_COMMON,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    pre[preCount++] = Transition(_tempHist[previous].Get(),
                                 D3D12_RESOURCE_STATE_COMMON,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    pre[preCount++] = Transition(_tempGuide[previous].Get(),
                                 D3D12_RESOURCE_STATE_COMMON,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool lowNeeded = mode == 4;
    if (lowNeeded) {
        pre[preCount++] = Transition(_tempLow[0].Get(),
                                     D3D12_RESOURCE_STATE_COMMON,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        pre[preCount++] = Transition(_tempLow[1].Get(),
                                     D3D12_RESOURCE_STATE_COMMON,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    cl->ResourceBarrier(preCount, pre);

    cl->SetComputeRootSignature(_rsTemporal.Get());
    HeapBinder gpu{ _device.Get(), *cl, slot };

    // cbuffer 布局(与 TEMPORAL_*_HLSL 同步,三处同步铁律):
    // Size@0 UseMotion@2 Route@3 Weight@4 MotionExtent@5 LowSize@7 Pad@9
    // Region@10(4)。
    const UINT sizeWH[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const UINT motionWH[2]{ motionW, motionH };
    const UINT lowWH[2]{ static_cast<UINT>((_width + 1) >> 1), static_cast<UINT>((_height + 1) >> 1) };
    const UINT route = static_cast<UINT>(mode);
    const UINT useM = useMotion ? 1u : 0u;
    const UINT pad0 = 0u;
    const UINT region[4]{ 0u, 0u, sizeWH[0], sizeWH[1] };
    cl->SetComputeRoot32BitConstants(0, 2, sizeWH, 0);
    cl->SetComputeRoot32BitConstants(0, 1, &useM, 2);
    cl->SetComputeRoot32BitConstants(0, 1, &route, 3);
    cl->SetComputeRoot32BitConstants(0, 1, &weight, 4);
    cl->SetComputeRoot32BitConstants(0, 2, motionWH, 5);
    cl->SetComputeRoot32BitConstants(0, 2, lowWH, 7);
    cl->SetComputeRoot32BitConstants(0, 1, &pad0, 9);
    cl->SetComputeRoot32BitConstants(0, 4, region, 10);

    if (lowNeeded) {
        // reduce:t0 Input / t1 Base / t2 Raw(读),u0 Residual / u1 Guide(写)。
        cl->SetPipelineState(_psoTemporalReduce.Get());
        cl->SetComputeRootDescriptorTable(1, gpu(HeapSlot::SrvInput));
        cl->SetComputeRootDescriptorTable(2, gpu(HeapSlot::SrvInput));
        cl->SetComputeRootDescriptorTable(3, gpu(kSrvOutputColor));
        cl->SetComputeRootDescriptorTable(9, gpu(HeapSlot::Low0Uav));
        cl->SetComputeRootDescriptorTable(10, gpu(HeapSlot::Low1Uav));
        cl->Dispatch((lowWH[0] + 7) / 8, (lowWH[1] + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER lowReady[2]{
            Transition(_tempLow[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(_tempLow[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        cl->ResourceBarrier(2, lowReady);
    }

    // 主 pass:u0 Output(temporalOut)/ u1 NextHistory / u2 NextGuide。
    cl->SetPipelineState(_psoTemporalMain.Get());
    cl->SetComputeRootDescriptorTable(1, gpu(HeapSlot::SrvInput));                     // t0 Input
    cl->SetComputeRootDescriptorTable(2, gpu(HeapSlot::SrvInput));                     // t1 Base(=Input)
    cl->SetComputeRootDescriptorTable(3, gpu(kSrvOutputColor));       // t2 Raw
    cl->SetComputeRootDescriptorTable(4, gpu(kHistSrv[previous]));     // t3 History
    cl->SetComputeRootDescriptorTable(5, gpu(kGuideSrv[previous]));     // t4 PreviousGuide
    cl->SetComputeRootDescriptorTable(6, gpu(motionSrvIndex));        // t5 Motion
    cl->SetComputeRootDescriptorTable(7, lowNeeded ? gpu(HeapSlot::Low0Srv) : gpu(kHistSrv[previous])); // t6
    cl->SetComputeRootDescriptorTable(8, lowNeeded ? gpu(HeapSlot::Low1Srv) : gpu(kGuideSrv[previous])); // t7
    cl->SetComputeRootDescriptorTable(9, gpu(kUavTemporalOut));       // u0 Output
    cl->SetComputeRootDescriptorTable(10, gpu(kHistUav[next]));        // u1 NextHistory
    cl->SetComputeRootDescriptorTable(11, gpu(kGuideUav[next]));        // u2 NextGuide
    cl->Dispatch((sizeWH[0] + 7) / 8, (sizeWH[1] + 7) / 8, 1);

    // 归位:input/output/历史/引导/low/temporalOut 全部回 COMMON(本 pass
    // 是 outputColor/inputColor 的最后消费者;temporalOut 显式 COMMON 落位,
    // 下游 base 尾 NSR 化 / fgBar / post 转换按 COMMON 契约对接);motion 留
    // NSR(recordGuidancePark 按既有契约归位)。
    D3D12_RESOURCE_BARRIER post[9];
    UINT postCount = 0;
    post[postCount++] = Transition(slot.outputColor.Get(),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_COMMON);
    post[postCount++] = Transition(slot.inputColor.Get(),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_COMMON);
    post[postCount++] = Transition(slot.temporalOut.Get(),
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COMMON);
    post[postCount++] = Transition(_tempHist[next].Get(),
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COMMON);
    post[postCount++] = Transition(_tempGuide[next].Get(),
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COMMON);
    post[postCount++] = Transition(_tempHist[previous].Get(),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_COMMON);
    post[postCount++] = Transition(_tempGuide[previous].Get(),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_COMMON);
    if (lowNeeded) {
        post[postCount++] = Transition(_tempLow[0].Get(),
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                       D3D12_RESOURCE_STATE_COMMON);
        post[postCount++] = Transition(_tempLow[1].Get(),
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                       D3D12_RESOURCE_STATE_COMMON);
    }
    cl->ResourceBarrier(postCount, post);
}

bool D3D12Context::RebuildScaling(int internalW, int internalH, char *err, size_t errLen) noexcept {
    // Replaces every slot's scaling textures; the caller must hold a PoolHold
    // (all slots idle, pool sealed) so no frame can still reference them.
    // 恒等归一(2026-09-25):内部尺寸 == 源尺寸时,缩放/残差管线退化为
    // 恒等全分辨率搬运(等尺寸 Lanczos2/Catmull-Rom,产出与直连逐位相同),
    // 归一为 scaling-off —— 不建 6 张全尺寸中间纹理,全部消费方经
    // HasScaling()==false 自动走直连。InternalSize 对 >=100% 已返回原尺寸,
    // 此处兜住"scalingEnabled=1 + res=100"组合(出厂默认)每帧白付的
    // 4 个全分辨率 pass。请求侧参数不动(面板如实显示用户请求)。
    if (internalW >= _width && internalH >= _height) {
        ClearScalingResources();
        return true;
    }
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

    // (re)write scaling SRV/UAV descriptors(槽位 = enum HeapSlot)。
    HeapBinder heap{ _device.Get(), slot };
    _device->CreateShaderResourceView(slot.reducedColor.Get(), nullptr, heap.cpu(HeapSlot::SrvReducedColor));
    _device->CreateShaderResourceView(slot.reducedDenoised.Get(), nullptr, heap.cpu(HeapSlot::SrvReducedDenoised));
    _device->CreateShaderResourceView(slot.horizontalRes.Get(), nullptr, heap.cpu(HeapSlot::SrvHorizontal));
    _device->CreateUnorderedAccessView(slot.reducedColor.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavReducedColor));
    _device->CreateUnorderedAccessView(slot.reducedDenoised.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavReducedDenoised));
    _device->CreateUnorderedAccessView(slot.horizontalRes.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavHorizontal));
    _device->CreateShaderResourceView(slot.controlledRes.Get(), nullptr, heap.cpu(HeapSlot::SrvControlled));
    _device->CreateUnorderedAccessView(slot.controlledRes.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavControlled));
    _device->CreateShaderResourceView(slot.reducedMotion.Get(), nullptr, heap.cpu(HeapSlot::SrvReducedMotion));
    _device->CreateShaderResourceView(slot.reducedConfidence.Get(), nullptr, heap.cpu(HeapSlot::SrvReducedConfidence));
    _device->CreateUnorderedAccessView(slot.reducedMotion.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavReducedMotion));
    _device->CreateUnorderedAccessView(slot.reducedConfidence.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavReducedConfidence));
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

void D3D12Context::RecordPass(FrameSlot &slot, ID3D12PipelineState *pso, HeapSlot srv0, HeapSlot srv1,
                              HeapSlot uav,
                              UINT dispatchX, UINT dispatchY, const ResidualControls &rc) noexcept {
    // NGX's evaluate may rebind its own descriptor heap / root signature on
    // this command list; rebind ours before touching our descriptors.
    ID3D12GraphicsCommandList *cl = slot.commandList.Get();
    cl->SetComputeRootSignature(_rsCompute.Get());
    cl->SetPipelineState(pso);
    HeapBinder gpu{ _device.Get(), *cl, slot };

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
    RecordPass(slot, _psoDownsampleVertical.Get(), SrvInput, SrvInput, UavHorizontal, // t0 input, t1 dummy, u0 horizontalRes
               (static_cast<UINT>(_width) + 7) / 8,
               (static_cast<UINT>(_internalHeight) + 7) / 8, rc);
}

void D3D12Context::RecordDownsampleHorizontal(FrameSlot &slot, const ResidualControls &rc) noexcept {
    // t0/t1 read the vertical pass's output through its SRV descriptor
    // (slot 3) — slot 6 holds horizontalRes's UAV descriptor, and binding a
    // UAV descriptor into an SRV root table is invalid per the D3D12 spec
    // (the debug layer flags it; other drivers may not read it correctly).
    RecordPass(slot, _psoDownsampleHorizontal.Get(), SrvHorizontal, SrvHorizontal, UavReducedColor, // t0 intermediate, t1 dummy, u0 reducedColor
               (static_cast<UINT>(_internalWidth) + 7) / 8,
               (static_cast<UINT>(_internalHeight) + 7) / 8, rc);
}

void D3D12Context::RecordResidualPrepare(FrameSlot &slot, const ResidualControls &rc) noexcept {
    RecordPass(slot, _psoPrepare.Get(), SrvReducedColor, SrvReducedDenoised, UavControlled, // t0 reducedColor, t1 reducedDenoised, u0 controlledRes
               (static_cast<UINT>(_internalWidth) + 7) / 8,
               (static_cast<UINT>(_internalHeight) + 7) / 8, rc);
}

void D3D12Context::RecordResidualHorizontal(FrameSlot &slot, const ResidualControls &rc) noexcept {
    RecordPass(slot, _psoHorizontal.Get(), SrvControlled, SrvControlled, UavHorizontal, // t0 controlledRes, t1 dummy, u0 horizontalRes
               (static_cast<UINT>(_width) + 7) / 8,
               (static_cast<UINT>(_internalHeight) + 7) / 8, rc);
}

void D3D12Context::RecordResidualVertical(FrameSlot &slot, const ResidualControls &rc,
                                          bool equalWidth) noexcept {
    // Equal internal width skips the horizontal pass (Magpie binds the
    // controlled residual straight into the vertical pass's t1).
    const HeapSlot residualSrv = equalWidth ? SrvControlled : SrvHorizontal;
    RecordPass(slot, _psoVertical.Get(), SrvInput, residualSrv, UavOutput, // t0 input, t1 residual, u0 output
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
    for (int i = 0; i < kSlotCount; ++i) {
        if (!_slots[i].srvUavHeap) return false;
        HeapBinder h{ _device.Get(), _slots[i] };
        _device->CreateShaderResourceView(views[0], nullptr, h.cpu(HeapSlot::SrvFlowF));
        _device->CreateShaderResourceView(views[1], nullptr, h.cpu(HeapSlot::SrvFlowB));
        _device->CreateShaderResourceView(views[2], nullptr, h.cpu(HeapSlot::SrvCostF));
        _device->CreateShaderResourceView(views[3], nullptr, h.cpu(HeapSlot::SrvCostB));
        _device->CreateUnorderedAccessView(inputFwd ? inputFwd : _slots[i].outputColor.Get(),
                                           nullptr, nullptr, h.cpu(HeapSlot::UavNvofInput0));
        _device->CreateUnorderedAccessView(inputBwd ? inputBwd : _slots[i].outputColor.Get(),
                                           nullptr, nullptr, h.cpu(HeapSlot::UavNvofInput1));
    }
    return true;
}

void D3D12Context::RecordDensify(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                 uint32_t denseW, uint32_t denseH,
                                 uint32_t flowW, uint32_t flowH,
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
    HeapBinder gpu{ _device.Get(), *cl, slot };

    const UINT srcWH[2]{ static_cast<UINT>(denseW), static_cast<UINT>(denseH) };
    const UINT flowWH[2]{ flowW, flowH };
    cl->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    cl->SetComputeRoot32BitConstants(0, 2, flowWH, 2);
    const UINT flags[3]{ hasForwardCost ? 1u : 0u,
                         hasBackward ? 1u : 0u, hasBackwardCost ? 1u : 0u };
    cl->SetComputeRoot32BitConstants(0, 3, flags, 4);
    const float motionScale[2]{ motionScaleX, motionScaleY };
    cl->SetComputeRoot32BitConstants(0, 2, motionScale, 7);

    cl->SetComputeRootDescriptorTable(1, gpu(HeapSlot::SrvFlowF)); // t0 ForwardFlow
    cl->SetComputeRootDescriptorTable(2, gpu(HeapSlot::SrvFlowB)); // t1 BackwardFlow
    cl->SetComputeRootDescriptorTable(3, gpu(HeapSlot::SrvCostF)); // t2 ForwardCost
    cl->SetComputeRootDescriptorTable(4, gpu(HeapSlot::SrvCostB)); // t3 BackwardCost
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
    HeapBinder gpu{ _device.Get(), *cl, slot };

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
    HeapBinder gpu{ _device.Get(), *cl, slot };

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
    cl->SetComputeRootDescriptorTable(1, gpu(HeapSlot::SrvInput));               // t0 inputColor
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

void D3D12Context::RecordFlowView(FrameSlot &slot, bool useReduced, bool realMotion,
                                  D3D12_RESOURCE_STATES outColorIn) noexcept {
    // 光流场调试视图(契约见头文件)。dispatch 恒按源尺寸(OutExtent),
    // 运动场 texel 在 shader 内最近邻映射 —— follow 内部场(半尺寸)也铺
    // 满输出,_debugDiff 无陈旧边缘。dispatch 写共享 _debugDiff → COPY_
    // SOURCE → 拷回 outputColor(与 RecordDebugDiff 同一条槽 CL 上原子成对;
    // 本视图不读 outputColor,免去 UAV→NSR 往返,直接 UAV→COPY_DEST→UAV)。
    ID3D12GraphicsCommandList *cl = slot.commandList.Get();
    // t0 取材:真运动帧按管线取 slot 场(已 NSR);无真运动帧绑静态零
    // 纹理(槽 35,常驻 NSR)—— 不碰 slot.motion(首 densify 前内容未定义)。
    const HeapSlot srvIndex = !realMotion ? SrvZeroMotion : (useReduced ? SrvReducedMotion : SrvMotion);
    cl->SetComputeRootSignature(_rsFlowView.Get());
    cl->SetPipelineState(_psoFlowView.Get());
    HeapBinder gpu{ _device.Get(), *cl, slot };

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

    // 入态:debugDiff→UAV / output→COPY_DEST(outColorIn:NR 开帧 eval 留
    // UAV,NR 关帧跨 ECL 衰减 COMMON —— 运动场三路取材全部 NSR 常驻或由
    // evaluate 消费链保持,无需屏障)。
    D3D12_RESOURCE_BARRIER pre[2]{
        Transition(_debugDiff.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Transition(slot.outputColor.Get(), outColorIn, D3D12_RESOURCE_STATE_COPY_DEST),
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
    HeapBinder gpu{ _device.Get(), *cl, slot };

    // cbuffer 布局(与 NVOF_DOWNSAMPLE_HLSL 同步,三处同步铁律):
    // srcWH@0 dstWH@2 pad@4。
    const UINT srcWH[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const UINT dstWH[2]{ static_cast<UINT>(dstW), static_cast<UINT>(dstH) };
    const UINT pad[1]{ 0u };
    cl->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    cl->SetComputeRoot32BitConstants(0, 2, dstWH, 2);
    cl->SetComputeRoot32BitConstants(0, 1, pad, 4);

    cl->SetComputeRootDescriptorTable(1, gpu(HeapSlot::SrvInput));               // t0 inputColor(NSR)
    cl->SetComputeRootDescriptorTable(2, gpu(static_cast<HeapSlot>(HeapSlot::UavNvofInput0 + inputIndex))); // u0 NvofInput[cur]
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
    for (int i = 0; i < kSlotCount; ++i) {
        if (!_slots[i].srvUavHeap) return false;
        HeapBinder h{ _device.Get(), _slots[i] };
        if (ffxInput) _device->CreateUnorderedAccessView(ffxInput, nullptr, nullptr, h.cpu(HeapSlot::UavFfxInput));
        if (ffxSparse) _device->CreateShaderResourceView(ffxSparse, nullptr, h.cpu(HeapSlot::SrvFfxSparse));
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
    HeapBinder gpu{ _device.Get(), *cl, slot };

    const UINT srcWH[2]{ srcW, srcH };
    const UINT dstWH[2]{ dstW, dstH };
    cl->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    cl->SetComputeRoot32BitConstants(0, 2, dstWH, 2);
    cl->SetComputeRootDescriptorTable(1, gpu(HeapSlot::SrvInput));  // t0 inputColor(NSR)
    cl->SetComputeRootDescriptorTable(2, gpu(HeapSlot::UavFfxInput)); // u0 ffxInput
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
    HeapBinder gpu{ _device.Get(), *cl, slot };

    const UINT srcWH[2]{ denseW, denseH };
    const UINT ofWH[2]{ ofW, ofH };
    const UINT spWH[2]{ sparseW, sparseH };
    cl->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    cl->SetComputeRoot32BitConstants(0, 2, ofWH, 2);
    cl->SetComputeRoot32BitConstants(0, 2, spWH, 4);
    const float scale[2]{ scaleX, scaleY };
    cl->SetComputeRoot32BitConstants(0, 2, scale, 6);
    cl->SetComputeRootDescriptorTable(1, gpu(HeapSlot::SrvFfxSparse));            // t0 sparseFlow
    cl->SetComputeRootDescriptorTable(2, gpu(uavMotion));     // u0 DenseMotion
    cl->SetComputeRootDescriptorTable(3, gpu(uavConfidence)); // u1 DenseConfidence
    cl->Dispatch((denseW + 7) / 8, (denseH + 7) / 8, 1);
}

void D3D12Context::RecordColorOutput(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                     ID3D12Resource *srcColor, UINT srcSrvIndex,
                                     ColorOutKind kind, int srcW, int srcH,
                                     ColorMatrix matrix, ColorRange range,
                                     D3D12_RESOURCE_STATES stateBefore) noexcept {
    // RTX Video 管线的颜色输出(PIPE 尺寸源 → OUT 尺寸平面;契约同
    // 见头文件注释)。PIPE==OUT 且 SDR 时坐标恒等映射,
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
    HeapBinder gpu{ _device.Get(), *cl, slot };

    const YuvCoeffs cf = YuvCoeffsFor(matrix, range, _outPlaneBytes > 1 ? 10 : 8);
    const bool fp16 = kind != ColorOutKind::Sdr; // P10 输出契约 + ChromaStep (2,2)
    ID3D12PipelineState *lumaPso = nullptr;
    ID3D12PipelineState *chromaPso = nullptr;
    switch (kind) {
    case ColorOutKind::HdrScRgb:
        lumaPso = _psoPqLuma.Get();
        chromaPso = _psoPqChroma.Get();
        break;
    case ColorOutKind::HdrPqCodes:
        lumaPso = _psoPqCodesLuma.Get();
        chromaPso = _psoPqCodesChroma.Get();
        break;
    default:
        // 1:1(PIPE==OUT)改派直写 PSO:双线性 4-tap 的权重在恒等映射下
        // 退化为单位冲激,但 4 次 load 一次不少 —— 直写单 load 省 4 倍读
        // 带宽。逐位同价由函数头注释背书(2026-09-25)。
        lumaPso = (srcW == _outW && srcH == _outH)
                      ? _psoConvertOutLuma.Get() : _psoConvertScaledLuma.Get();
        chromaPso = (srcW == _outW && srcH == _outH)
                        ? _psoConvertOutChroma.Get() : _psoConvertScaledChroma.Get();
        break;
    }
    // ChromaStep:SDR 输出 = 输入布局(同格式出);HDR P10 输出恒 (2,2)。
    const UINT step[2]{
        static_cast<UINT>(fp16 ? 2 : (_subW ? 2 : 1)),
        static_cast<UINT>(fp16 ? 2 : (_subH ? 2 : 1)),
    };
    // 单个转换 dispatch(_rsConvertOut 契约同 RecordColorOutput;恒绑满 u0/u1/u2
    // 三张表 —— 未消费的表绑合法描述符,shader 不读无害)。
    auto recordConvertPass = [&](ID3D12PipelineState *pso, const UINT (&extent)[2],
                                 const UINT (&srcExtent)[2], const float (&consts)[8],
                                 UINT u0, UINT u1, UINT u2) {
        cl->SetPipelineState(pso);
        cl->SetComputeRoot32BitConstants(0, 2, extent, 0);
        cl->SetComputeRoot32BitConstants(0, 2, srcExtent, 2);
        cl->SetComputeRoot32BitConstants(0, 8, consts, 4);
        cl->SetComputeRoot32BitConstants(0, 2, step, 12);
        cl->SetComputeRootDescriptorTable(1, gpu(srcSrvIndex));
        cl->SetComputeRootDescriptorTable(2, gpu(static_cast<HeapSlot>(u0)));
        cl->SetComputeRootDescriptorTable(3, gpu(static_cast<HeapSlot>(u1)));
        cl->SetComputeRootDescriptorTable(4, gpu(static_cast<HeapSlot>(u2)));
        cl->Dispatch((extent[0] + 7) / 8, (extent[1] + 7) / 8, 1);
    };
    constexpr UINT kUnusedUav = static_cast<UINT>(HeapSlot::UavYuvOut1);

    if (kind == ColorOutKind::Sdr && _isRgb) {
        // Phase B:RGBP SDR 输出(PIPE→OUT 双线性;1:1 时坐标恒等映射,
        // 与直写逐位同价)。RGB 全域直码,零矩阵。
        const UINT extent[2]{ static_cast<UINT>(_outW), static_cast<UINT>(_outH) };
        const UINT srcExtent[2]{ static_cast<UINT>(srcW), static_cast<UINT>(srcH) };
        const float consts[8]{};
        recordConvertPass(_psoRgbScaled.Get(), extent, srcExtent, consts,
                          HeapSlot::UavYuvOut0, HeapSlot::UavYuvOut1, HeapSlot::UavYuvOut2);
    } else if (kind == ColorOutKind::Sdr) {
        const UINT extent[2]{ static_cast<UINT>(_outW), static_cast<UINT>(_outH) };
        const UINT srcExtent[2]{ static_cast<UINT>(srcW), static_cast<UINT>(srcH) };
        const float consts[8]{ cf.kr, cf.kb,
                               cf.yLo / cf.containerMax, cf.ySpan / cf.containerMax,
                               0.0f, 0.0f, 0.0f, 0.0f };
        recordConvertPass(lumaPso, extent, srcExtent, consts,
                          HeapSlot::UavYuvOut0, HeapSlot::UavYuvOut1, kUnusedUav);
        const UINT cExtent[2]{ static_cast<UINT>(_outChromaW), static_cast<UINT>(_outChromaH) };
        const float cConsts[8]{ cf.kr, cf.kb,
                                cf.cMid / cf.containerMax, cf.cSpan / cf.containerMax,
                                0.0f, 0.0f, 0.0f, 0.0f };
        recordConvertPass(chromaPso, cExtent, srcExtent, cConsts,
                          HeapSlot::UavYuvOut1, HeapSlot::UavYuvOut2, kUnusedUav);
    } else {
        // HDR(HdrScRgb / HdrPqCodes):Kr/Kb = BT.2020(0.2627/0.0593);
        // limited 10bit luma 64+876y、chroma 512+896c(÷CM 折进常量,与 SDR
        // 路径同布局)。两者常量组相同,仅 PSO 对不同(码域源免 PqEncode)。
        constexpr float kKr2020 = 0.2627f;
        constexpr float kKb2020 = 0.0593f;
        const UINT extent[2]{ static_cast<UINT>(_outW), static_cast<UINT>(_outH) };
        const UINT srcExtent[2]{ static_cast<UINT>(srcW), static_cast<UINT>(srcH) };
        const float consts[8]{ kKr2020, kKb2020,
                               64.0f / 1023.0f, 876.0f / 1023.0f,
                               0.0f, 0.0f, 0.0f, 0.0f };
        recordConvertPass(lumaPso, extent, srcExtent, consts,
                          HeapSlot::UavYuvOut0, HeapSlot::UavYuvOut1, kUnusedUav);
        const UINT cExtent[2]{ static_cast<UINT>(_outChromaW), static_cast<UINT>(_outChromaH) };
        const float cConsts[8]{ kKr2020, kKb2020,
                                512.0f / 1023.0f, 896.0f / 1023.0f,
                                0.0f, 0.0f, 0.0f, 0.0f };
        recordConvertPass(chromaPso, cExtent, srcExtent, cConsts,
                          HeapSlot::UavYuvOut1, HeapSlot::UavYuvOut2, kUnusedUav);
    }

    D3D12_RESOURCE_BARRIER back[1]{
        Transition(srcColor,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
    };
    cl->ResourceBarrier(1, back);
}

void D3D12Context::RecordHdrToPq(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                 D3D12_RESOURCE_STATES stateBefore) noexcept {
    // PQ 域插帧编码 pass(仅 fgHdrInterp 且 fg 段开启被记录):hdrColor(FP16
    // scRGB,1:1 @PIPE)→ fgBack(FP16 PQ 码 ≤1.0,UAV)→ NSR 作 DLSSG
    // backbuffer。DLSSG 的 ColorBuffersHDR=1 路径对 >1.0 线性值不保真,码域
    // 走 LDR 路径无损(原生 HDR 直通同域实证);插值输出码域直读(post 侧
    // PqCodesToYuv 免逐像素 PqEncode)。
    ID3D12GraphicsCommandList *cl = &clRef;
    if (stateBefore != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER toNsr[1]{
            Transition(slot.hdrColor.Get(), stateBefore,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        cl->ResourceBarrier(1, toNsr);
    }
    D3D12_RESOURCE_BARRIER toUav[1]{
        Transition(slot.fgBack.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cl->ResourceBarrier(1, toUav);

    cl->SetComputeRootSignature(_rsConvertOut.Get());
    HeapBinder gpu{ _device.Get(), *cl, slot };

    cl->SetPipelineState(_psoHdrToPq.Get());
    {
        // 编码只消费 t0/u0;u1/u2 表绑 yuvOut 占位(根签名必须全绑,本机
        // 驱动对未绑定根参数的死法见 #41-①)。
        const UINT extent[2]{ static_cast<UINT>(_pipeW), static_cast<UINT>(_pipeH) };
        const float consts[14]{};
        cl->SetComputeRoot32BitConstants(0, 2, extent, 0);
        cl->SetComputeRoot32BitConstants(0, 2, extent, 2); // SourceExtent:1:1
        cl->SetComputeRoot32BitConstants(0, 10, consts, 4);
        cl->SetComputeRootDescriptorTable(1, gpu(kSrvHdrColor));
        cl->SetComputeRootDescriptorTable(2, gpu(kUavFgBack));
        cl->SetComputeRootDescriptorTable(3, gpu(HeapSlot::UavYuvOut0)); // 占位(未消费)
        cl->SetComputeRootDescriptorTable(4, gpu(HeapSlot::UavYuvOut1)); // 占位(未消费)
        cl->Dispatch((static_cast<UINT>(_pipeW) + 7) / 8, (static_cast<UINT>(_pipeH) + 7) / 8, 1);
    }
    // UAV→NSR:fgBack 即 DLSSG backbuffer(eval 前 NSR 化,替代旧 fgBar)。
    D3D12_RESOURCE_BARRIER toNsr[1]{
        Transition(slot.fgBack.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    cl->ResourceBarrier(1, toNsr);
}

void D3D12Context::RecordMotionScale(ID3D12GraphicsCommandList &cl, FrameSlot &slot,
                                     int srcW, int srcH) noexcept {
    // 源尺寸运动场(NSR)→ motionDense(PIPE 尺寸,UAV)。motionDense 由
    // 调用方(帧路径)按 COMMON→UAV 屏障后调用,UAV→NSR 收尾归调用方。
    const UINT extent[2]{ static_cast<UINT>(_pipeW), static_cast<UINT>(_pipeH) };
    const UINT srcExtent[2]{ static_cast<UINT>(srcW), static_cast<UINT>(srcH) };
    HeapBinder gpu{ _device.Get(), cl, slot };
    cl.SetComputeRootSignature(_rsMotionScale.Get());
    cl.SetPipelineState(_psoMotionScale.Get());
    cl.SetComputeRoot32BitConstants(0, 2, extent, 0);
    cl.SetComputeRoot32BitConstants(0, 2, srcExtent, 2);
    cl.SetComputeRootDescriptorTable(1, gpu(HeapSlot::SrvMotion));             // t0 slot.motion SRV
    cl.SetComputeRootDescriptorTable(2, gpu(kUavMotionDense)); // u0 motionDense
    cl.Dispatch((extent[0] + 7) / 8, (extent[1] + 7) / 8, 1);
}

} // namespace vsdlssnr
