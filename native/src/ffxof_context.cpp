// AMD FidelityFX Optical Flow 会话实现(时序模型见 ffxof_context.h)。
// 移植源:Magpie src/Magpie.Core/AmdOpticalFlowProvider.cpp —— 剥离其
// D3D11↔D3D12 互操作层(本插件纯 D3D12:Prepare/FFX dispatch/Densify 全部
// 在本插件自己的设备与队列上),FFX API 调用面与 Magpie 逐行对应。
#include "ffxof_context.h"
#include "d3d12_context.h"
#include "dlssnr_context.h" // TimingStatusLine

#include <chrono>
#include <cstdio>
#include <cstring>

#include <ffx_dx12.h>

namespace vsdlssnr {

namespace {

// 会话纹理:DEFAULT 堆 + UAV 标志、COMMON 初始态,并带 SIMULTANEOUS_ACCESS
// —— backend 每次 dispatch 都从声明的 COMMON 起录自己的屏障且结束时把
// 资源留在中间态(NSR/UAV);非 simultaneous-access 纹理不衰变,声明契约
// 从第二帧起失真(debug layer [527] 每帧报错)。带该旗标后 ECL 边界强制
// 衰变回 COMMON,声明恒真,三段 CL 的屏障全部成立。
bool CreateFfxTexture(ID3D12Device *device, DXGI_FORMAT format,
                      uint32_t width, uint32_t height,
                      ID3D12Resource **out) noexcept {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                 D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    return SUCCEEDED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(out)));
}

} // namespace

FxofContext::~FxofContext() { Finalize(); }

bool FxofContext::WaitForFenceReached(ID3D12Fence *fence, uint64_t value,
                                      HANDLE event, DWORD timeoutMs) noexcept {
    // 与 NvofContext::WaitFenceReached 同款:循环复查完成值(共享事件的
    // 唤醒可能被窃取),栅栏值单调 ⇒ 有界退出。
    for (;;) {
        if (fence->GetCompletedValue() >= value) return true;
        if (FAILED(fence->SetEventOnCompletion(value, event))) return false;
        if (WaitForSingleObject(event, timeoutMs) != WAIT_OBJECT_0) return false;
    }
}

void FxofContext::Finalize() noexcept { DestroySession(); }

void FxofContext::DestroySession() noexcept {
    // 设备已挂时跳过栅栏等待(永不满值)。
    const bool gpuOk = _d3d12 && _d3d12->Queue() && !_d3d12->IsDeviceLost();
    if (gpuOk && _copyFenceEvent && _copyFence && _lastCopyFence) {
        WaitForFenceReached(_copyFence.Get(), _lastCopyFence, _copyFenceEvent, 10000);
    }
    if (gpuOk && _doneFenceEvent && _doneFence && _lastDone) {
        WaitForFenceReached(_doneFence.Get(), _lastDone, _doneFenceEvent, 10000);
    }
    // 不调 ffxOpticalflowContextDestroy(见 ffxof_context.h destroy 注释):
    // context 与 scratch 随本对象进入 _retiredOf 名单存活到进程退出。
    _context = {};
    _contextCreated = false;
    _scratch.clear();
    _scratch.shrink_to_fit();
    _ffxInput.Reset();
    _sparseFlow.Reset();
    _scd.Reset();
    _commandListA.Reset();
    _commandListB.Reset();
    _allocatorA.Reset();
    _allocatorB.Reset();
    _copyFence.Reset();
    _doneFence.Reset();
    if (_copyFenceEvent) {
        CloseHandle(_copyFenceEvent);
        _copyFenceEvent = nullptr;
    }
    if (_doneFenceEvent) {
        CloseHandle(_doneFenceEvent);
        _doneFenceEvent = nullptr;
    }
    _copySeq = 0;
    _lastCopyFence = 0;
    _doneSeq = 0;
    _lastDone = 0;
    _d3d12 = nullptr;
    {
        std::lock_guard<std::mutex> lock(_gate.Mutex);
        _gate.ResetTimeline();
        _consecutiveFailures = 0;
    }
    _ready.store(false, std::memory_order_release);
}

bool FxofContext::Initialize(D3D12Context &d3d12, int width, int height,
                             int quality, char *err, size_t errLen) noexcept {
    DestroySession();
    return CreateSession(d3d12, width, height, quality, err, errLen);
}

bool FxofContext::CreateSession(D3D12Context &d3d12, int width, int height,
                                int quality, char *err, size_t errLen) noexcept {
    auto fail = [&](const char *what) {
        if (err && errLen) std::snprintf(err, errLen, "%s", what);
        DestroySession();
        return false;
    };

    ID3D12Device *device = d3d12.Device();
    if (!device || width <= 0 || height <= 0) return fail("fxof: no device");
    _d3d12 = &d3d12;
    _width = width;
    _height = height;
    _quality = quality;

    // 能力门槛(Magpie CheckCapabilities 原样):D3D12 SM6.2 + WaveOps +
    // R16G16_SINT UAV typed store。失败 → auto 链落 nvof(N 卡)/零 guidance。
    {
        D3D12_FEATURE_DATA_SHADER_MODEL sm{ .HighestShaderModel = D3D_SHADER_MODEL_6_2 };
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 opt1{};
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fmt{ .Format = DXGI_FORMAT_R16G16_SINT };
        const bool ok =
            SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) &&
            sm.HighestShaderModel >= D3D_SHADER_MODEL_6_2 &&
            SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opt1, sizeof(opt1))) &&
            opt1.WaveOps &&
            SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fmt, sizeof(fmt))) &&
            (fmt.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
        if (!ok) return fail("fxof: adapter lacks SM6.2/WaveOps/R16G16_SINT UAV");
    }

    // 质量档位映射(独立三档):1 = Performance(会话/2),2 = Quality
    // (全分辨率)。
    _mode = quality <= 1 ? Mode::Performance : Mode::Quality;
    _ofW = _mode == Mode::Performance ? (static_cast<uint32_t>(width) + 1) / 2
                                      : static_cast<uint32_t>(width);
    _ofH = _mode == Mode::Performance ? (static_cast<uint32_t>(height) + 1) / 2
                                      : static_cast<uint32_t>(height);

    // FFX context 创建(Magpie Create 原样):scratch → backend interface →
    // context → 共享资源描述。
    _scratch.resize(ffxGetScratchMemorySizeDX12(FFX_OPTICALFLOW_CONTEXT_COUNT));
    FfxOpticalflowContextDescription ctxDesc{
        .flags = 0,
        .resolution = { _ofW, _ofH },
    };
    FfxErrorCode ffx = ffxGetInterfaceDX12(
        &ctxDesc.backendInterface, ffxGetDeviceDX12(device),
        _scratch.data(), _scratch.size(), FFX_OPTICALFLOW_CONTEXT_COUNT);
    if (ffx != FFX_OK) return fail("fxof: ffxGetInterfaceDX12 failed");
    ffx = ffxOpticalflowContextCreate(&_context, &ctxDesc);
    if (ffx != FFX_OK) return fail("fxof: ffxOpticalflowContextCreate failed");
    _contextCreated = true;

    FfxOpticalflowSharedResourceDescriptions shared{};
    ffx = ffxOpticalflowGetSharedResourceDescriptions(&_context, &shared);
    if (ffx != FFX_OK) return fail("fxof: query shared resources failed");
    const FfxApiResourceDescription &vecDesc = shared.opticalFlowVector.resourceDescription;
    if (vecDesc.format != FFX_API_SURFACE_FORMAT_R16G16_SINT) {
        return fail("fxof: unexpected sparse flow format");
    }
    _sparseW = vecDesc.width;
    _sparseH = vecDesc.height;
    const FfxApiResourceDescription &scdDesc = shared.opticalFlowSCD.resourceDescription;

    if (!CreateFfxTexture(device, DXGI_FORMAT_R8G8B8A8_UNORM, _ofW, _ofH,
                          _ffxInput.GetAddressOf()) ||
        !CreateFfxTexture(device, DXGI_FORMAT_R16G16_SINT, _sparseW, _sparseH,
                          _sparseFlow.GetAddressOf()) ||
        !CreateFfxTexture(device, ffxGetDX12FormatFromSurfaceFormat(
                                      static_cast<FfxApiSurfaceFormat>(scdDesc.format)),
                          scdDesc.width, scdDesc.height, _scd.GetAddressOf())) {
        return fail("fxof: create session textures failed");
    }

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(_allocatorA.GetAddressOf()))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(_allocatorB.GetAddressOf()))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         _allocatorA.Get(), nullptr,
                                         IID_PPV_ARGS(_commandListA.GetAddressOf()))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         _allocatorB.Get(), nullptr,
                                         IID_PPV_ARGS(_commandListB.GetAddressOf()))) ||
        FAILED(_commandListA->Close()) || FAILED(_commandListB->Close()) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(_copyFence.GetAddressOf()))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(_doneFence.GetAddressOf())))) {
        return fail("fxof: create command path failed");
    }
    _copyFenceEvent = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    _doneFenceEvent = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!_copyFenceEvent || !_doneFenceEvent) return fail("fxof: CreateEvent failed");
    _copySeq = _lastCopyFence = _copyFence->GetCompletedValue();
    _doneSeq = _lastDone = _doneFence->GetCompletedValue();

    // 视图写入每槽堆(49/50;失败 = 初始化失败)。
    if (!d3d12.BindOfResources(_ffxInput.Get(), _sparseFlow.Get())) {
        return fail("fxof: bind resources failed");
    }

    {
        std::lock_guard<std::mutex> lock(_gate.Mutex);
        _gate.ResetTimeline(); // 新会话:首帧 dispatch.reset 播种
        _consecutiveFailures = 0;
    }
    {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: fxof session mode=%s quality=%d %dx%d of=%ux%u sparse=%ux%u",
                      _mode == Mode::Performance ? "perf" : "qual",
                      _quality, width, height, _ofW, _ofH, _sparseW, _sparseH);
        OutputDebugStringA("vs_dlssnr: ");
        OutputDebugStringA(msg);
        OutputDebugStringA("\n");
        TimingStatusLine(msg);
    }
    _ready.store(true, std::memory_order_release);
    return true;
}

const char *FxofContext::ModeString(char *buf, size_t len) noexcept {
    std::snprintf(buf, len, "fxof q%d %s %ux%u", _quality,
                  _mode == Mode::Performance ? "perf" : "qual", _ofW, _ofH);
    return buf;
}

void FxofContext::ResetHistory() noexcept {
    std::lock_guard<std::mutex> lock(_gate.Mutex);
    _gate.ResetTimeline();
}

OfStageResult FxofContext::StageFrame(int frameIndex, ID3D12Resource *srcTex,
                                      const OfPostExecuteFn &postExecute,
                                      const OfPostCopyFn &postCopy,
                                      bool inputWrittenByPostCopy) noexcept {
    OfStageResult result{};
    // inputWrittenByPostCopy 为 NVOF ping-pong 语义(FFX 无 _input[],Prepare
    // 恒由 postCopy lambda 记录),此处有意忽略。
    (void)inputWrittenByPostCopy;
    (void)srcTex; // 输入取材经 postCopy(RecordConvertInput → inputColor)
    if (!_ready.load(std::memory_order_acquire) || !_d3d12 || !_d3d12->Queue() ||
        !postExecute || !postCopy) {
        result.publishZero = true;
        result.historyReset = true;
        return result;
    }
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    {
        std::unique_lock<std::mutex> lock(_gate.Mutex);
        const OfGateDecision decision = _gate.Arrive(frameIndex, lock);
        if (decision == OfGateDecision::Expired) {
            result.publishZero = true;
            _lastStageMs = 0.0;
            return result;
        }
        if (_d3d12->IsDeviceLost()) {
            result.publishZero = true;
            result.historyReset = true;
            _gate.InvalidateHistory();
            _lastStageMs = 0.0;
            return result;
        }

        const bool seed = decision == OfGateDecision::Seed || !_gate.HistoryValid();
        ID3D12CommandQueue *queue = _d3d12->Queue();

        // ---- copy CL(alloc A)----
        // 防御性 queue Wait(与 NvofContext 同构;FIFO 下已满足,零成本)。
        if (_lastDone) queue->Wait(_doneFence.Get(), _lastDone);
        if (_lastCopyFence) {
            if (!WaitForFenceReached(_copyFence.Get(), _lastCopyFence,
                                     _copyFenceEvent, 10000)) {
                TimingStatusLine("DLSSNR STATUS: fxof copy fence timeout");
            }
        }
        HRESULT hrA = _allocatorA->Reset();
        bool copyOk = SUCCEEDED(hrA);
        if (!copyOk) {
            _commandListA->Close();
            hrA = _allocatorA->Reset();
            copyOk = SUCCEEDED(hrA);
        }
        HRESULT hrCL = copyOk ? _commandListA->Reset(_allocatorA.Get(), nullptr) : E_FAIL;
        copyOk = copyOk && SUCCEEDED(hrCL);
        if (!copyOk) {
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "DLSSNR STATUS: fxof copy reset failed alloc=0x%08lx cl=0x%08lx",
                          static_cast<unsigned long>(hrA), static_cast<unsigned long>(hrCL));
            TimingStatusLine(msg);
        }
        if (copyOk) {
            // postCopy lambda = RecordConvertInput + RecordFfxPrepare
            // (Prepare 写 _ffxInput,屏障归本会话管理)。
            auto probe = [&](const char *tag) {
                if (!ProbeEnabled()) return;
                char dbg[2200];
                _d3d12->DebugDumpInfoQueue(dbg, sizeof(dbg));
                char msg[2400];
                std::snprintf(msg, sizeof(msg), "PROBE: fxof %s: %s", tag, dbg);
                TimingStatusLine(msg);
            };
            probe("pre");
            // SIMULTANEOUS_ACCESS 纹理在 ECL 边界强制衰变 COMMON:copy CL
            // 起点恒为 COMMON,Prepare 写完恢复 COMMON,backend dispatch
            // 从声明的 COMMON 起录屏障 —— 三段全部成立,无状态跟踪需要。
            D3D12_RESOURCE_BARRIER toUav[1]{
                Transition(_ffxInput.Get(), D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            _commandListA->ResourceBarrier(1, toUav);
            probe("toUav");
            postCopy(_commandListA.Get(), 0);
            probe("postCopy");
            D3D12_RESOURCE_BARRIER toCommon[1]{
                Transition(_ffxInput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_COMMON),
            };
            _commandListA->ResourceBarrier(1, toCommon);
            probe("toCommon");
            copyOk = SUCCEEDED(_commandListA->Close());
            if (!copyOk) {
                char dbg[1400];
                _d3d12->DebugDumpInfoQueue(dbg, sizeof(dbg));
                char msg[1500];
                std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: fxof copy close failed: %s", dbg);
                TimingStatusLine(msg);
            }
        }
        if (copyOk) {
            ID3D12CommandList *lists[]{ _commandListA.Get() };
            queue->ExecuteCommandLists(1, lists);
            _lastCopyFence = ++_copySeq;
            queue->Signal(_copyFence.Get(), _lastCopyFence);
        } else {
            TimingStatusLine("DLSSNR STATUS: fxof copy submit failed");
            result.publishZero = true;
            result.historyReset = true;
            _gate.InvalidateHistory();
            if (++_consecutiveFailures >= 3) {
                _ready.store(false, std::memory_order_release);
            }
            _gate.Advance(frameIndex);
            QueryPerformanceCounter(&t1);
            _lastStageMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                           static_cast<double>(freq.QuadPart);
            return result;
        }

        // ---- main CL(alloc B):FFX dispatch ----
        if (SUCCEEDED(_allocatorB->Reset()) &&
            SUCCEEDED(_commandListB->Reset(_allocatorB.Get(), nullptr))) {
            FfxOpticalflowDispatchDescription dispatch{
                .commandList = ffxGetCommandListDX12(_commandListB.Get()),
                .color = ffxGetResourceDX12(
                    _ffxInput.Get(),
                    ffxGetResourceDescriptionDX12(_ffxInput.Get()),
                    L"vs_dlssnr fxof color", FFX_API_RESOURCE_STATE_COMMON),
                .opticalFlowVector = ffxGetResourceDX12(
                    _sparseFlow.Get(),
                    ffxGetResourceDescriptionDX12(_sparseFlow.Get(),
                                                  FFX_API_RESOURCE_USAGE_UAV),
                    L"vs_dlssnr fxof vector", FFX_API_RESOURCE_STATE_COMMON),
                .opticalFlowSCD = ffxGetResourceDX12(
                    _scd.Get(),
                    ffxGetResourceDescriptionDX12(_scd.Get(),
                                                  FFX_API_RESOURCE_USAGE_UAV),
                    L"vs_dlssnr fxof scd", FFX_API_RESOURCE_STATE_COMMON),
                .reset = seed, // 播种帧重建金字塔/SCD 历史(Magpie 同语义)
                .backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB,
                .minMaxLuminance = { 0.0f, 1.0f },
            };
            const FfxErrorCode dr = ffxOpticalflowContextDispatch(&_context, &dispatch);
            if (dr != FFX_OK || FAILED(_commandListB->Close())) {
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "DLSSNR STATUS: fxof dispatch failed frame=%d err=%d",
                              frameIndex, static_cast<int32_t>(dr));
                OutputDebugStringA("vs_dlssnr: ");
                OutputDebugStringA(msg);
                OutputDebugStringA("\n");
                TimingStatusLine(msg);
                result.publishZero = true;
                result.historyReset = true;
                _gate.InvalidateHistory();
                if (++_consecutiveFailures >= 3) {
                    _ready.store(false, std::memory_order_release);
                    TimingStatusLine("DLSSNR STATUS: fxof disabled after consecutive failures");
                }
                _gate.Advance(frameIndex);
                QueryPerformanceCounter(&t1);
                _lastStageMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                               static_cast<double>(freq.QuadPart);
                return result;
            }
            ID3D12CommandList *lists[]{ _commandListB.Get() };
            queue->ExecuteCommandLists(1, lists);
            _lastDone = ++_doneSeq;
            queue->Signal(_doneFence.Get(), _lastDone);
            // CPU 等 FFX 输出(与 NVOF execute 后 CPU 等同款;分配器复用
            // 安全 + densify 录制时序与 nvof 段计时同语义)。超时 = 会话
            // 状态不可信,停用待 RebuildOf 重建。
            if (!WaitForFenceReached(_doneFence.Get(), _lastDone,
                                     _doneFenceEvent, 10000)) {
                TimingStatusLine("DLSSNR STATUS: fxof output fence timeout; session retired");
                result.publishZero = true;
                result.historyReset = true;
                _gate.InvalidateHistory();
                _ready.store(false, std::memory_order_release);
                _gate.Advance(frameIndex);
                QueryPerformanceCounter(&t1);
                _lastStageMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                               static_cast<double>(freq.QuadPart);
                return result;
            }
            _consecutiveFailures = 0;
            if (_executesLogged < 5) {
                ++_executesLogged;
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "DLSSNR STATUS: fxof dispatch ok frame=%d reset=%d",
                              frameIndex, seed ? 1 : 0);
                TimingStatusLine(msg);
            }
        } else {
            TimingStatusLine("DLSSNR STATUS: fxof main cl reset failed");
            result.publishZero = true;
            result.historyReset = true;
            _gate.InvalidateHistory();
            _gate.Advance(frameIndex);
            QueryPerformanceCounter(&t1);
            _lastStageMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                           static_cast<double>(freq.QuadPart);
            return result;
        }

        // ---- densify CL(alloc A 复用):稀疏流 → 稠密运动 ----
        // sparseFlow 经 main CL 的 UAV 写,FFX backend 收尾归 COMMON(声明
        // 契约);densify SRV 读走隐式提升 —— debug layer 校验点。
        bool densifyOk = SUCCEEDED(_allocatorA->Reset()) &&
                         SUCCEEDED(_commandListA->Reset(_allocatorA.Get(), nullptr));
        if (densifyOk) {
            postExecute(_commandListA.Get(), 0); // motion/conf 屏障 + RecordFfxDensify
            densifyOk = SUCCEEDED(_commandListA->Close());
        }
        if (densifyOk) {
            ID3D12CommandList *lists[]{ _commandListA.Get() };
            queue->ExecuteCommandLists(1, lists);
            _lastCopyFence = ++_copySeq;
            queue->Signal(_copyFence.Get(), _lastCopyFence);
            result.waitFenceValue = _lastDone; // 非 0 = realMotion
        } else {
            TimingStatusLine("DLSSNR STATUS: fxof densify submit failed");
            result.publishZero = true;
            result.historyReset = true;
            result.waitFenceValue = 0;
            _gate.InvalidateHistory();
        }

        if (seed && densifyOk) _gate.MarkSeeded();
        _gate.Advance(frameIndex);
    }

    QueryPerformanceCounter(&t1);
    _lastStageMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                   static_cast<double>(freq.QuadPart);
    return result;
}

void FxofContext::WaitCopyIdle() noexcept {
    if (!_ready.load(std::memory_order_acquire) || !_d3d12 || _d3d12->IsDeviceLost()) return;
    if (!_lastCopyFence || _copyFence->GetCompletedValue() >= _lastCopyFence) return;
    _copyFence->SetEventOnCompletion(_lastCopyFence, _copyFenceEvent);
    WaitForSingleObject(_copyFenceEvent, 10000);
}

} // namespace vsdlssnr
