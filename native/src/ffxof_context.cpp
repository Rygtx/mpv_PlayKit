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

void FxofContext::Finalize() noexcept { DestroySession(); }

void FxofContext::DestroySession() noexcept {
    // 设备已挂时跳过栅栏等待(永不满值)。
    const bool gpuOk = _d3d12 && _d3d12->Queue() && !_d3d12->IsDeviceLost();
    if (gpuOk && _copyFenceEvent && _copyFence && _lastCopyFence) {
        WaitFenceReached(_copyFence.Get(), _lastCopyFence, _copyFenceEvent, 10000);
    }
    if (gpuOk && _doneFenceEvent && _doneFence && _lastDone) {
        WaitFenceReached(_doneFence.Get(), _lastDone, _doneFenceEvent, 10000);
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
    // 轮转池 CL 随会话析构(OfClRotator 成员 ComPtr 自释);簿记清零防
    // 退役会话的悬垂栅栏指针比较(下方 copy/done 栅栏对象本体会 Reset)。
    _rotator.ResetBookkeeping();
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
    // OF GPU 跨度括号资源(READBACK 先解映射)。
    if (_tsReadback && _tsMapped) _tsReadback->Unmap(0, nullptr);
    _tsMapped = nullptr;
    _tsReadback.Reset();
    _tsHeap.Reset();
    _lastGpuSpanMs = 0.0;
    _spanPending = false;
    _spanFence = 0;
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
    try {
        _scratch.resize(ffxGetScratchMemorySizeDX12(FFX_OPTICALFLOW_CONTEXT_COUNT));
    } catch (...) { // noexcept 路径:抛出即 terminate 杀宿主,转建会话失败降级
        return fail("fxof: scratch allocation failed (oom)");
    }
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

    if (!_rotator.Create(d3d12)) {
        return fail("fxof: create command path failed");
    }
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(_copyFence.GetAddressOf()))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(_doneFence.GetAddressOf())))) {
        return fail("fxof: create fence failed");
    }
    _copyFenceEvent = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    _doneFenceEvent = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!_copyFenceEvent || !_doneFenceEvent) return fail("fxof: CreateEvent failed");
    // 计数器不再从完成值播种(2026-10-04 撤 NVOF 同构仪式):栅栏是本会话
    // 新建(CreateFence 初值 0),播种恒得 0 —— 该写法只在 NVOF 的进程级
    // 单例栅栏上承重(其 91ea1d1 历史 bug 背书),此处纯属误导性留痕。

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
    // OF GPU 跨度括号资源(GpuTsEnabled 时):heap 2 查询 + 16B READBACK,
    // 打点直接录在 main CL(dispatch 纯 compute,无 NGX,同 CL 合法)。
    if (d3d12.GpuTsEnabled()) {
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 2;
        if (FAILED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(_tsHeap.GetAddressOf())))) {
            TimingStatusLine("DLSSNR STATUS: fxof ts heap create failed; span fallback to submit");
        } else {
            D3D12_HEAP_PROPERTIES rbHeap{};
            rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rbDesc{};
            rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rbDesc.Width = 16; // 2×UINT64
            rbDesc.Height = 1;
            rbDesc.DepthOrArraySize = 1;
            rbDesc.MipLevels = 1;
            rbDesc.SampleDesc.Count = 1;
            rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device->CreateCommittedResource(
                    &rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(_tsReadback.GetAddressOf()))) ||
                FAILED(_tsReadback->Map(0, nullptr, &_tsMapped)) || !_tsMapped) {
                _tsReadback.Reset();
                _tsHeap.Reset(); // 半初始化防悬:heap 存活 + mapped null = 每帧 null 目标 ResolveQueryData
                TimingStatusLine("DLSSNR STATUS: fxof ts readback failed; span fallback to submit");
            }
        }
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
                                      bool inputWrittenByPostCopy,
                                      std::unique_lock<std::mutex> &gateOut) noexcept {
    OfStageResult result{};
    // inputWrittenByPostCopy 为 NVOF ping-pong 语义(FFX 无 _input[],Prepare
    // 恒由 postCopy lambda 记录),此处有意忽略;gateOut 同为 NVOF 延迟
    // densify 契约(FFX 门内即录即提交),恒不动。
    (void)inputWrittenByPostCopy;
    (void)gateOut;
    (void)srcTex; // 输入取材经 postCopy(RecordConvertInput → inputColor)
    if (!_ready.load(std::memory_order_acquire) || !_d3d12 || !_d3d12->Queue() ||
        !postExecute || !postCopy) {
        result.historyReset = true;
        return result;
    }
    // 停摆观测④的连续迟到计数(成员 _expStreak:函数级 static 跨实例残留,
    // 退役重建/换档后旧计数带进新会话;且 static 非 atomic,当前靠门 mutex
    // 串行纯属侥幸 —— 成员化后串行即设计保证,2026-10-05 评审修)。
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    {
        // 取锁前声明在途:门据此区分"前驱堵在 mutex 外(等它插队)"与
        // "真丢帧(零等待立即播种)"(of_frame_gate.h)。代际快照同窗口
        // 采样:ResetTimeline 插队时 Arrive 代际失配判幽灵帧(2026-10-04)。
        const uint64_t gateEpoch = _gate.Epoch();
        _gate.MarkIncoming(frameIndex);
        std::unique_lock<std::mutex> lock(_gate.Mutex);
        const OfGateDecision decision = _gate.Arrive(frameIndex, gateEpoch, lock);
        if (decision == OfGateDecision::Expired) {
            // 连续迟到帧观测(正式保留):偶发 1-2 帧 = 线程乱序,正常;
            // streak 累到 16+ = 门期望与帧号时间线错位(2026-10-02 vsr 切档
            // 门残留旧 _nextSeq 实锤:重载后 mpv 从帧 0 重送,复制帧爬满旧
            // 时间线长度才自愈)。阈值触达打一行,归零前不再刷 —— 常态零
            // 输出,perf 行 s/x/r 不覆盖"过期"形态,此行是唯一信号。
            ++_expStreak;
            if (_expStreak == 16) {
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "DLSSNR STATUS: of gate late streak=%d frame=%d next=%d",
                              _expStreak, frameIndex, static_cast<int>(_gate.NextSeq()));
                TimingStatusLine(msg);
            }
            _lastStageMs = 0.0;
            _lastGpuSpanMs = 0.0; // 过期帧无光流计算,nvof 段读 0
            return result;
        }
        _expStreak = 0;
        // 推进门:活性不变量要求过 Arrive 的帧所有路径必达 Advance,否则
        // 缺口=1 的等待方永久悬等(of_frame_gate.h)。
        if (_d3d12->IsDeviceLost()) {
            result.historyReset = true;
            _gate.InvalidateHistory();
            _gate.Advance(frameIndex);
            _lastStageMs = 0.0;
            _lastGpuSpanMs = 0.0;
            return result;
        }

        const bool seed = decision == OfGateDecision::Seed || !_gate.HistoryValid();
        ID3D12CommandQueue *queue = _d3d12->Queue();

        // 失败收口三连(copy submit / main acquire / dispatch 同构;此前逐字
        // 三连仅日志文案差异 —— copy 路径缺"连败停用"留痕属漂移,现统一):
        // 播种 + 连败闩锁(LatchFailure 单点)+ Advance + 计时 + return。
        auto failAndAdvance = [&](const char *msg) -> OfStageResult {
            TimingStatusLine(msg);
            result.historyReset = true;
            _gate.InvalidateHistory();
            LatchFailure(_consecutiveFailures, _ready, "fxof");
            _gate.Advance(frameIndex);
            QueryPerformanceCounter(&t1);
            _lastStageMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                           static_cast<double>(freq.QuadPart);
            return result;
        };

        // ---- copy CL ----
        // (2026-10-04 撤 NVOF 同构仪式的 queue Wait:copy 与上帧 dispatch
        // 在同一队列 FIFO 上,copy(n) 天然后于 dispatch(n-1) 完成 —— 自家
        // 队列的自等是零收益的队列依赖,还向读者暗示存在跨引擎同步需求。
        // NVOF 的同名等待是跨引擎时序,承重,不在此列。)
        ID3D12CommandAllocator *copyAlloc = nullptr;
        ID3D12GraphicsCommandList *copyCl = nullptr;
        bool copyOk = AcquireCl(&copyAlloc, &copyCl);
        if (!copyOk) {
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "DLSSNR STATUS: fxof copy cl acquire failed (rotator)");
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
            copyCl->ResourceBarrier(1, toUav);
            probe("toUav");
            postCopy(copyCl, 0);
            probe("postCopy");
            D3D12_RESOURCE_BARRIER toCommon[1]{
                Transition(_ffxInput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_COMMON),
            };
            copyCl->ResourceBarrier(1, toCommon);
            probe("toCommon");
            copyOk = SUCCEEDED(copyCl->Close());
            if (!copyOk) {
                char dbg[1400];
                _d3d12->DebugDumpInfoQueue(dbg, sizeof(dbg));
                char msg[1500];
                std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: fxof copy close failed: %s", dbg);
                TimingStatusLine(msg);
            }
        }
        if (copyOk) {
            _lastCopyFence = _rotator.Submit(queue, _copyFence.Get(), copyCl, ++_copySeq);
            // 转换 + Prepare 已落在成功提交的 copy CL 上(of_backend.h 契约):
            // 回填 inputIndex,调用方跳过槽 CL 的重复转换 —— 此前恒 -1,FFX
            // 每帧多付一次全量 YUV→RGB dispatch,且槽 CL 对已 NSR 的
            // inputColor 记 COMMON→UAV(from-state 错配,2026-09-25 审查)。
            result.inputIndex = 0;
        } else {
            return failAndAdvance("DLSSNR STATUS: fxof copy submit failed");
        }

        // ---- main CL:FFX dispatch ----
        // 门内不再 CPU 等 dispatch 完成(2026-09-25 重构):doneFence 是自家
        // queue->Signal 的普通栅栏,densify 的读序由其提交前的 queue Wait
        // 封闭,GPU 完成保证迁移到 WaitCopyIdle(槽释放点)。分配器复用由
        // 轮转池的 Reset 等待保证(4 段之前)。
        ID3D12CommandAllocator *mainAlloc = nullptr;
        ID3D12GraphicsCommandList *mainCl = nullptr;
        bool mainOk = AcquireCl(&mainAlloc, &mainCl);
        if (!mainOk) {
            return failAndAdvance("DLSSNR STATUS: fxof main cl acquire failed (rotator)");
        }
        // OF GPU 跨度括号:dispatch = FFX 光流计算本体(纯 compute,无
        // NGX,同 CL 打点合法),首尾 EndQuery 量出纯执行时间。
        if (_tsHeap) mainCl->EndQuery(_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        FfxOpticalflowDispatchDescription dispatch{
            .commandList = ffxGetCommandListDX12(mainCl),
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
        if (_tsHeap) {
            mainCl->EndQuery(_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            mainCl->ResolveQueryData(_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                     0, 2, _tsReadback.Get(), 0);
        }
        if (dr != FFX_OK || FAILED(mainCl->Close())) {
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "DLSSNR STATUS: fxof dispatch failed frame=%d err=%d",
                          frameIndex, static_cast<int32_t>(dr));
            OutputDebugStringA("vs_dlssnr: ");
            OutputDebugStringA(msg);
            OutputDebugStringA("\n");
            return failAndAdvance(msg);
        }
        _lastDone = _rotator.Submit(queue, _doneFence.Get(), mainCl, ++_doneSeq);
        if (_tsMapped) {
            _spanPending = true;
            _spanFence = _lastDone.load(std::memory_order_relaxed);
        }
        // 连败清零已下移 densify 成功分支:整链(main+densify)成功才算链完
        // 好 —— 此前 main 成功即清,densify 单点持续失败每帧被抵消,连败
        // 闩锁永不触发(2026-10-05 评审修)。
        if (_executesLogged < 5) {
            ++_executesLogged;
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "DLSSNR STATUS: fxof dispatch ok frame=%d reset=%d",
                          frameIndex, seed ? 1 : 0);
            TimingStatusLine(msg);
        }

        // ---- densify CL:稀疏流 → 稠密运动 ----
        // sparseFlow 经 main CL 的 UAV 写,FFX backend 收尾归 COMMON(声明
        // 契约);densify SRV 读走隐式提升 —— debug layer 校验点。读序 =
        // 同队列 FIFO(densify 后于 dispatch 入队,自然后完成;原 queue
        // Wait 同为 NVOF 同构仪式,2026-10-04 撤)。
        ID3D12CommandAllocator *densAlloc = nullptr;
        ID3D12GraphicsCommandList *densCl = nullptr;
        bool densifyOk = AcquireCl(&densAlloc, &densCl);
        if (densifyOk) {
            postExecute(densCl, 0); // motion/conf 屏障 + RecordFfxDensify
            densifyOk = SUCCEEDED(densCl->Close());
        }
        if (densifyOk) {
            _lastCopyFence = _rotator.Submit(queue, _copyFence.Get(), densCl, ++_copySeq);
            result.waitFenceValue = _lastDone; // 非 0 = realMotion
            _consecutiveFailures = 0;
        } else {
            // densify 失败与 copy/main 同类(链断:InvalidateHistory 已落),
            // 计入连败闩锁 —— 此前恰漏在"失败收口三连已统一"的网外,持续
            // 失败风暴时本后端永不自停(2026-10-04 补齐)。
            TimingStatusLine("DLSSNR STATUS: fxof densify submit failed");
            result.historyReset = true;
            result.waitFenceValue = 0;
            _gate.InvalidateHistory();
            LatchFailure(_consecutiveFailures, _ready, "fxof");
        }

        // 播种帧对齐 NVOF 播种语义(of_backend.h 契约):发布零运动 + 携带
        // NGX 重置。此前播种成功路径返回 realMotion 且不带重置 —— PARAM_RESET
        // 只由播种帧携带,FFX 后端(默认)下每次 seek NR 都缺重置,时域历史
        // 跨时间线泄漏(2026-09-25 修复)。本帧 densify 已照录,输出仅下帧
        // 起被消费,不影响。
        if (seed) {
            result.historyReset = true;
            result.waitFenceValue = 0;
        }

        if (seed && densifyOk) _gate.MarkSeeded();
        _gate.Advance(frameIndex);
    }

    QueryPerformanceCounter(&t1);
    _lastStageMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                   static_cast<double>(freq.QuadPart);
    return result;
}

bool FxofContext::AcquireCl(ID3D12CommandAllocator **allocator,
                            ID3D12GraphicsCommandList **cl) noexcept {
    // CL 池轮转(OfClRotator 共用实体):背压等待按栅栏身份选事件(事件
    // 分家防唤醒窃取),自愈 Reset,门内串行(OfFrameGate),会话级簿记无
    // 并发等待者。
    const OfFenceEvent fe[2]{{_copyFence.Get(), _copyFenceEvent},
                             {_doneFence.Get(), _doneFenceEvent}};
    const auto r = _rotator.Acquire(allocator, cl, fe, 2);
    if (r == OfClRotator::AcquireResult::Timeout) {
        TimingStatusLine("DLSSNR STATUS: fxof cl rotator wait timeout; session retired");
        _ready.store(false, std::memory_order_release);
    }
    return r == OfClRotator::AcquireResult::Ok;
}

void FxofContext::WaitCopyIdle() noexcept {
    // 槽释放点的排空保证(2026-09-25 承重化):门内 CPU 等 dispatch 已删,
    // 本帧 copy(main 依赖)/dispatch(densify 依赖)/densify(slot->motion
    // 的 GPU 写)在槽释放时可能仍在飞。两个栅栏都要等:copyFence 最后值
    // 覆盖 copy+densify;doneFence 最后值覆盖 dispatch(densify 的读序
    // queue Wait 排在其后,等它即覆盖全链)。upload 堆的 CPU 写(PackInput
    // 下一帧)与 GPU 读(copy CL 的 RecordConvertInput)竞争由此排空。
    if (!_ready.load(std::memory_order_acquire) || !_d3d12 || _d3d12->IsDeviceLost()) return;
    bool ok = true;
    if (_lastDone && _doneFence->GetCompletedValue() < _lastDone) {
        ok = WaitFenceReached(_doneFence.Get(), _lastDone, _doneFenceEvent, 10000);
        if (!ok) {
            TimingStatusLine("DLSSNR STATUS: fxof dispatch fence timeout at slot release; session retired");
            _ready.store(false, std::memory_order_release);
            return;
        }
    }
    if (_lastCopyFence && _copyFence->GetCompletedValue() < _lastCopyFence) {
        ok = WaitFenceReached(_copyFence.Get(), _lastCopyFence, _copyFenceEvent, 10000);
        if (!ok) {
            TimingStatusLine("DLSSNR STATUS: fxof copy fence timeout at slot release; session retired");
            _ready.store(false, std::memory_order_release);
            return;
        }
    }
}

double FxofContext::LastStageTotalMs() const noexcept {
    // dispatch GPU 跨度(括号已提交且 doneFence 达标 = READBACK 已落位)时
    // 读精确值;未落位回退提交跨度(FFX GPU 计算量级 1-2ms,回退偏差小)。
    // 过期/失败帧已在 StageFrame 清零,此处读到 0 = 本帧无光流计算。
    if (_spanPending && _d3d12 && !_d3d12->IsDeviceLost() && _doneFence &&
        _doneFence->GetCompletedValue() >= _spanFence && _tsMapped) {
        const UINT64 *ts = static_cast<const UINT64 *>(_tsMapped);
        _lastGpuSpanMs = static_cast<double>(ts[1] - ts[0]) * 1000.0 / _d3d12->GpuTsFreq();
        _spanPending = false;
    }
    return _spanPending ? _lastStageMs : _lastGpuSpanMs;
}

} // namespace vsdlssnr
