#include "d3d12_context.h"
#include "d3d12_internal.h" // 槽堆句柄绑定 + RS/PSO 工厂(子系统 TU 共享助手)
#include "dlssnr_context.h" // TimingStatusLine

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d12sdklayers.h>
#include <thread>

namespace vsdlssnr {

namespace {

constexpr UINT DEVICE_VENDOR_NVIDIA = 0x10DE;

} // namespace


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
    // 丢失上报单点(removed/hang 两分支共用;面板 stats 通道由宿主层接线,
    // 设备层不直连 UI 传输,见 SetHangNotify)。
    auto reportLost = [&](const char *why, uint64_t v) {
        const HRESULT rr = _device ? _device->GetDeviceRemovedReason() : E_FAIL;
        char buf[160];
        snprintf(buf, sizeof(buf), "%s reason=0x%08lX fence=%llu", why,
                 static_cast<unsigned long>(rr),
                 static_cast<unsigned long long>(v));
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
    };
    // 设备移除后 GetCompletedValue 恒 UINT64_MAX(D3D12 契约)—— 下方
    // "< value" 恒假、整个等待被跳过 = 假成功:陈旧回读当新帧交付,
    // _deviceLost/_hangNotify 永不触发(2026-10-04 评审修)。
    if (_fence->GetCompletedValue() == UINT64_MAX) {
        reportLost("fence wait: device removed:", value);
        return false;
    }
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
            reportLost("GPU hang/removed:", value);
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
                            _inColorFmt, D3D12_RESOURCE_STATE_COMMON,
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
    // 全部根签名/PSO 由子系统 Pass 组 Create*Objects 建立(实现分居各
    // d3d12_<子系统>.cpp;2026-10-04 拆分 —— 原单函数混装全部子系统七段
    // PSO/RS 清单)。PSOs are stateless and shared; descriptor heap is per-slot.
    return CreateResidualObjects(err, errLen) &&
           CreateOfObjects(err, errLen) &&
           CreateDebugObjects(err, errLen) &&
           CreateTemporalObjects(err, errLen) &&
           CreateConvertObjects(err, errLen);
}

} // namespace vsdlssnr

