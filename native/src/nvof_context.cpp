// D3D12 原生 NVOF 会话实现(见 nvof_context.h 的时序模型)。
// API 形态对照 Magpie 的 D3D11 路径(NvidiaOpticalFlowProvider.cpp):
//   - 会话创建链:LoadLibrary(nvofapi64.dll,System32,从不随应用分发)
//     → NvOFAPICreateInstanceD3D12 → nvCreateOpticalFlowD3D12(仅设备)
//     → nvOFInit(BOTH+cost → BOTH 无 cost → FORWARD+cost → FORWARD 逐级回退)
//   - 注册纹理:DEFAULT 堆 + ALLOW_UNORDERED_ACCESS,常驻 COMMON
//   - execute:输入栅栏点(内容就绪)+ 输出栅栏点(完成后置位),异步提交
//   - densify(S10.5 网格 → 稠密运动 + 置信度)是 Magpie HLSL 原样移植,
//     在 D3D12Context 的槽命令列表上执行(PSO/_psoDensify 也在那边)。
//   - 失败语义对齐 Magpie:失败帧清零发布 + 重置历史,连续 3 败停用会话。
#include "nvof_context.h"
#include "d3d12_context.h"
#include "dlssnr_context.h" // TimingStatusLine(时序失败必须进 timing log)

#include <chrono>
#include <cstdio>
#include <cstring>

namespace vsdlssnr {

namespace {

// Magpie ResolveProfile(NvidiaOpticalFlowProvider.cpp:119-135)原样移植。
struct NvofProfile {
    uint32_t gridSize;
    NV_OF_PERF_LEVEL perfLevel;
    const char *label;
};

constexpr NvofProfile ResolveProfile(int quality) noexcept {
    switch (quality) {
    case 1: return { 4, NV_OF_PERF_LEVEL_FAST, "4F" };
    case 3: return { 4, NV_OF_PERF_LEVEL_SLOW, "4S" };
    case 4: return { 2, NV_OF_PERF_LEVEL_MEDIUM, "2M" };
    case 5: return { 2, NV_OF_PERF_LEVEL_SLOW, "2S" };
    case 2:
    default: return { 4, NV_OF_PERF_LEVEL_MEDIUM, "4M" };
    }
}

// NV_OF_BUFFER_FORMAT → DXGI(样例 NvOFBufferFormatToDxgiFormat 的三个用例)。
constexpr DXGI_FORMAT ToDxgi(NV_OF_BUFFER_FORMAT fmt) noexcept {
    switch (fmt) {
    case NV_OF_BUFFER_FORMAT_ABGR8: return DXGI_FORMAT_B8G8R8A8_UNORM; // NVOF 的 ABGR8 = DXGI BGRA8
    case NV_OF_BUFFER_FORMAT_SHORT2: return DXGI_FORMAT_R16G16_SINT;
    case NV_OF_BUFFER_FORMAT_UINT8: return DXGI_FORMAT_R8_UINT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// Magpie CreateTexture2D 等价:DEFAULT 堆、UAV 标志、COMMON 初始态
// (样例 AllocateBuffer 同款;注册纹理不做逐帧状态迁移,见头文件注释)。
bool CreateRegisteredTexture(ID3D12Device *device, DXGI_FORMAT format,
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
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(out)));
}

} // namespace

// nvofapi64.dll 进程级缓存,加载后永不卸载:quality 切换会频繁 destroy/
// recreate,而 FreeLibrary 会在 NVOF 引擎 worker 仍存活时卸载 DLL 死锁
// (实测 2026-09-07)。系统驱动 DLL(685KB),与 NGX snippet 的 DLL 钉住
// 同一哲学;进程退出由 OS 回收。
static HMODULE GetNvofModule() noexcept {
    static HMODULE module = LoadLibraryExW(L"nvofapi64.dll", nullptr,
                                           LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module;
}

// 栅栏/事件进程级单例:队列里可能挂着对 doneFence 的 Wait(StageFrame 拷贝
// 路径),会话销毁时释放栅栏对象 = 驱动侧悬空引用(实测段错误)。热上下文
// 哲学:进程退出由 OS 回收。
struct NvofFences {
    Microsoft::WRL::ComPtr<ID3D12Fence> copy;
    Microsoft::WRL::ComPtr<ID3D12Fence> done;
    HANDLE event = nullptr;
    bool ok = false;
    explicit NvofFences(ID3D12Device *device) {
        ok = device &&
             SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(copy.GetAddressOf()))) &&
             SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(done.GetAddressOf())));
        if (ok) event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        ok = ok && event;
    }
};

static NvofFences &GetNvofFences(ID3D12Device *device) noexcept {
    static NvofFences fences(device);
    return fences;
}

NvofContext::~NvofContext() { Finalize(); }

// 释放会话与驱动 DLL。调用方持 PoolHold(槽池排空 ⇒ 无 in-flight 拷贝/
// execute 引用注册纹理);等两个栅栏落位后再注销 + 销毁。
void NvofContext::Finalize() noexcept { DestroySession(); }

void NvofContext::DestroySession() noexcept {
    // 设备已挂(闩锁/移除)时跳过栅栏等待:永不满值,干等 10s×2。
    const bool gpuOk = _d3d12 && _d3d12->Queue() && !_d3d12->IsDeviceLost();
    if (gpuOk && _copyFenceEvent) {
        if (_copyFence && _lastCopyFence &&
            _copyFence->GetCompletedValue() < _lastCopyFence) {
            _copyFence->SetEventOnCompletion(_lastCopyFence, _copyFenceEvent);
            WaitForSingleObject(_copyFenceEvent, 10000);
        }
        if (_doneFence && _lastDone &&
            _doneFence->GetCompletedValue() < _lastDone) {
            _doneFence->SetEventOnCompletion(_lastDone, _copyFenceEvent);
            WaitForSingleObject(_copyFenceEvent, 10000);
        }
    }
    TimingStatusLine("PROBE: destroy begin"); // 临时探针
    if (_session && _api.nvOFUnregisterResourceD3D12) {
        for (NvOFGPUBufferHandle &h : _registered) {
            if (h) {
                NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 up{};
                up.hOFGpuBuffer = h;
                _api.nvOFUnregisterResourceD3D12(&up);
                h = nullptr;
            }
        }
    }
    TimingStatusLine("PROBE: destroy unregistered"); // 临时探针
    if (_session && _api.nvOFDestroy) {
        _api.nvOFDestroy(_session);
    }
    TimingStatusLine("PROBE: destroy done"); // 临时探针
    _session = nullptr;
    for (auto &t : _input) t.Reset();
    for (auto &t : _flow) t.Reset();
    for (auto &t : _cost) t.Reset();
    _api = {};
    // 模块进程级缓存,永不 FreeLibrary(见 GetNvofModule 注释)。
    _module = nullptr;
    _copyCommandList.Reset();
    _copyAllocator.Reset();
    // 栅栏/事件是进程级单例(GetNvofFences):不释放,只解除本会话引用。
    _copyFence.Reset();
    _doneFence.Reset();
    _copyFenceEvent = nullptr;
    _copySeq = 0;
    _lastCopyFence = 0;
    _doneSeq = 0;
    _lastDone = 0;
    _doneByParity[0] = _doneByParity[1] = 0;
    _bidirectional = false;
    _costEnabled = false;
    _gridSize = 0;
    _d3d12 = nullptr;
    {
        std::lock_guard<std::mutex> lock(_gateMutex);
        _historyValid = false;
        _curInput = 0;
        _consecutiveFailures = 0;
        _nextSeq = -1;
        _gateCv.notify_all();
    }
    _ready.store(false, std::memory_order_release);
}

bool NvofContext::Initialize(D3D12Context &d3d12, int width, int height,
                             int quality, char *err, size_t errLen) noexcept {
    // 全量重建:同尺寸换档位也走 Destroy+Create(网格尺寸随档位变化,
    // flow 纹理大小跟着变)。会话创建在 PoolHold 下,毫秒级。
    DestroySession();
    return CreateSession(d3d12, width, height, quality, err, errLen);
}

bool NvofContext::CreateSession(D3D12Context &d3d12, int width, int height,
                                int quality, char *err, size_t errLen) noexcept {
    auto fail = [&](const char *what) {
        if (err && errLen) std::snprintf(err, errLen, "%s", what);
        DestroySession();
        return false;
    };

    _d3d12 = &d3d12;
    _width = width;
    _height = height;
    _quality = quality;

    ID3D12Device *device = d3d12.Device();
    if (!device || width <= 0 || height <= 0) {
        return fail("nvof: no device");
    }
    // 适配器必须是 NVIDIA(与 Magpie 一致;非 NVIDIA 走零 guidance 回退)。
    if (d3d12.Adapter()) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(d3d12.Adapter()->GetDesc1(&desc)) || desc.VendorId != 0x10DE) {
            return fail("nvof: adapter is not NVIDIA; zero guidance");
        }
    }

    _module = GetNvofModule();
    if (!_module) return fail("nvof: driver nvofapi64.dll not found");

    const auto createApi = reinterpret_cast<CreateApiFn>(
        GetProcAddress(_module, "NvOFAPICreateInstanceD3D12"));
    const auto getMaxVersion = reinterpret_cast<GetMaxVersionFn>(
        GetProcAddress(_module, "NvOFGetMaxSupportedApiVersion"));
    if (!createApi) return fail("nvof: NvOFAPICreateInstanceD3D12 missing");
    uint32_t driverVersion = NV_OF_API_VERSION;
    if (getMaxVersion &&
        (getMaxVersion(&driverVersion) != NV_OF_SUCCESS ||
         driverVersion < NV_OF_API_VERSION)) {
        return fail("nvof: driver API too old");
    }
    if (createApi(NV_OF_API_VERSION, &_api) != NV_OF_SUCCESS ||
        !_api.nvCreateOpticalFlowD3D12 || !_api.nvOFInit || !_api.nvOFExecuteD3D12 ||
        !_api.nvOFRegisterResourceD3D12 || !_api.nvOFUnregisterResourceD3D12 ||
        !_api.nvOFDestroy || !_api.nvOFGetCaps) {
        return fail("nvof: D3D12 function list incomplete");
    }
    if (_api.nvCreateOpticalFlowD3D12(device, &_session) != NV_OF_SUCCESS || !_session) {
        return fail("nvof: create session failed");
    }

    // 格式能力查询(Magpie QueryFormats 同款):输入要 BGRA8,输出要
    // R16G16_SINT(S10.5);cost(R8_UINT)可选。
    const auto queryFormats = [&](NV_OF_BUFFER_USAGE usage, DXGI_FORMAT want) {
        uint32_t count = 0;
        DXGI_FORMAT formats[32]{};
        if (_api.nvOFGetSurfaceFormatCountD3D12(_session, usage, NV_OF_MODE_OPTICALFLOW,
                                                &count) != NV_OF_SUCCESS ||
            count == 0 || count > 32 ||
            _api.nvOFGetSurfaceFormatD3D12(_session, usage, NV_OF_MODE_OPTICALFLOW,
                                           formats) != NV_OF_SUCCESS) {
            return false;
        }
        for (uint32_t i = 0; i < count; ++i) {
            if (formats[i] == want) return true;
        }
        return false;
    };
    if (!queryFormats(NV_OF_BUFFER_USAGE_INPUT, DXGI_FORMAT_B8G8R8A8_UNORM)) {
        return fail("nvof: BGRA8 input format unavailable");
    }
    if (!queryFormats(NV_OF_BUFFER_USAGE_OUTPUT, DXGI_FORMAT_R16G16_SINT)) {
        return fail("nvof: R16G16_SINT output format unavailable");
    }
    const bool costFormatAvailable =
        queryFormats(NV_OF_BUFFER_USAGE_COST, DXGI_FORMAT_R8_UINT);

    // 网格尺寸能力(Magpie QueryCaps 同款):档位不被支持 → 会话失败,
    // 上层回退零 guidance(与 Magpie QualityUnsupported 语义一致)。
    const NvofProfile profile = ResolveProfile(quality);
    {
        uint32_t count = 0;
        uint32_t grids[16]{};
        bool supported = false;
        if (_api.nvOFGetCaps(_session, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,
                             nullptr, &count) == NV_OF_SUCCESS &&
            count > 0 && count <= 16 &&
            _api.nvOFGetCaps(_session, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,
                             grids, &count) == NV_OF_SUCCESS) {
            for (uint32_t i = 0; i < count; ++i) {
                if (grids[i] == profile.gridSize) supported = true;
            }
        }
        if (!supported) {
            char msg[96];
            std::snprintf(msg, sizeof(msg),
                          "nvof: grid %ux%u unsupported for quality %d",
                          profile.gridSize, profile.gridSize, quality);
            return fail(msg);
        }
    }
    _gridSize = profile.gridSize;

    // nvOFInit:双向 + cost 逐级回退(Magpie 同款)。
    NV_OF_INIT_PARAMS init{};
    init.width = static_cast<uint32_t>(width);
    init.height = static_cast<uint32_t>(height);
    init.outGridSize = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(profile.gridSize);
    init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
    init.mode = NV_OF_MODE_OPTICALFLOW;
    init.perfLevel = profile.perfLevel;
    init.enableExternalHints = NV_OF_FALSE;
    init.enableOutputCost = costFormatAvailable ? NV_OF_TRUE : NV_OF_FALSE;
    init.disparityRange = NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED;
    init.enableRoi = NV_OF_FALSE;
    init.predDirection = NV_OF_PRED_DIRECTION_BOTH;
    init.enableGlobalFlow = NV_OF_FALSE;
    init.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;

    NV_OF_STATUS status = _api.nvOFInit(_session, &init);
    _bidirectional = status == NV_OF_SUCCESS;
    _costEnabled = costFormatAvailable && status == NV_OF_SUCCESS;
    if (status != NV_OF_SUCCESS && costFormatAvailable) {
        init.enableOutputCost = NV_OF_FALSE;
        status = _api.nvOFInit(_session, &init);
        _bidirectional = status == NV_OF_SUCCESS;
        _costEnabled = false;
    }
    if (status != NV_OF_SUCCESS) {
        init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
        init.enableOutputCost = costFormatAvailable ? NV_OF_TRUE : NV_OF_FALSE;
        status = _api.nvOFInit(_session, &init);
        _costEnabled = costFormatAvailable && status == NV_OF_SUCCESS;
        if (status != NV_OF_SUCCESS && costFormatAvailable) {
            init.enableOutputCost = NV_OF_FALSE;
            status = _api.nvOFInit(_session, &init);
            _costEnabled = false;
        }
        _bidirectional = false;
    }
    if (status != NV_OF_SUCCESS) return fail("nvof: nvOFInit failed");

    // 注册纹理(输入 ping-pong、flow ping-pong、cost ping-pong)。
    const uint32_t flowW =
        (static_cast<uint32_t>(width) + _gridSize - 1) / _gridSize;
    const uint32_t flowH =
        (static_cast<uint32_t>(height) + _gridSize - 1) / _gridSize;
    ID3D12Resource *tex[6]{};
    for (int i = 0; i < 2; ++i) {
        if (!CreateRegisteredTexture(device, ToDxgi(NV_OF_BUFFER_FORMAT_ABGR8),
                                     static_cast<uint32_t>(width),
                                     static_cast<uint32_t>(height),
                                     _input[i].GetAddressOf())) {
            return fail("nvof: create input texture failed");
        }
        tex[i] = _input[i].Get();
    }
    for (int i = 0; i < 2; ++i) {
        if (!CreateRegisteredTexture(device, ToDxgi(NV_OF_BUFFER_FORMAT_SHORT2),
                                     flowW, flowH, _flow[i].GetAddressOf())) {
            return fail("nvof: create flow texture failed");
        }
        tex[2 + i] = _flow[i].Get();
    }
    if (_costEnabled) {
        for (int i = 0; i < 2; ++i) {
            if (!CreateRegisteredTexture(device, ToDxgi(NV_OF_BUFFER_FORMAT_UINT8),
                                         flowW, flowH, _cost[i].GetAddressOf())) {
                return fail("nvof: create cost texture failed");
            }
            tex[4 + i] = _cost[i].Get();
        }
    }

    // 栅栏/拷贝命令路径先于注册创建:注册也走栅栏点(输出点单调递增,
    // 与 execute 共用 doneFence 计数)。栅栏/事件为进程级单例(见
    // GetNvofFences 注释)。
    {
        NvofFences &fences = GetNvofFences(device);
        if (!fences.ok) return fail("nvof: CreateFence failed");
        _copyFence = fences.copy;
        _doneFence = fences.done;
        _copyFenceEvent = fences.event;
    }
    if (FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(_copyAllocator.GetAddressOf()))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, _copyAllocator.Get(), nullptr,
            IID_PPV_ARGS(_copyCommandList.GetAddressOf()))) ||
        FAILED(_copyCommandList->Close())) {
        return fail("nvof: create copy command list failed");
    }

    for (int i = 0; i < 6; ++i) {
        if (!tex[i]) continue; // cost 未启用时 4/5 留空
        NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 rp{};
        rp.resource = tex[i];
        rp.inputFencePoint.fence = _copyFence.Get();
        rp.inputFencePoint.value = 0; // 池排空 + GPU 空闲,无需等待
        rp.outputFencePoint.fence = _doneFence.Get();
        rp.outputFencePoint.value = ++_doneSeq;
        rp.hOFGpuBuffer = &_registered[i]; // 出参:句柄写回我们的槽位
        if (_api.nvOFRegisterResourceD3D12(_session, &rp) != NV_OF_SUCCESS ||
            !_registered[i]) {
            _registered[i] = nullptr;
            return fail("nvof: RegisterResource failed");
        }
    }
    // 注册的 GPU 侧工作落位后再开始逐帧拷贝。
    if (_doneSeq && _doneFence->GetCompletedValue() < _doneSeq) {
        _doneFence->SetEventOnCompletion(_doneSeq, _copyFenceEvent);
        WaitForSingleObject(_copyFenceEvent, 10000);
    }

    // flow/cost SRV 写入每个槽的描述符堆(densify 在槽列表上执行)。
    if (!d3d12.BindNvofResources(FlowForward(), FlowBackward(),
                                 CostForward(), CostBackward())) {
        return fail("nvof: bind flow SRVs failed");
    }

    {
        std::lock_guard<std::mutex> lock(_gateMutex);
        _historyValid = false; // 新会话:首帧播种(清零发布)
        _curInput = 0;
        _consecutiveFailures = 0;
        _nextSeq = -1;         // 下一帧自定起点
        _gateCv.notify_all();
    }
    _ready.store(true, std::memory_order_release);
    return true;
}

// ---- 帧路径 ----------------------------------------------------------------
// 门内持锁完成拷贝提交 + execute 调用(状态与提交的串行点);cv 等待期间
// 放锁,前驱帧得以推进。整段是 CPU 快速路径(亚毫秒),持锁不影响吞吐。

NvofContext::StageResult NvofContext::StageFrame(int frameIndex,
                                                 ID3D12Resource *uploadBuffer,
                                                 UINT uploadRowPitch) noexcept {
    StageResult result{};
    if (!_ready.load(std::memory_order_acquire) || !_d3d12 || !_d3d12->Queue()) {
        result.publishZero = true;
        result.historyReset = true;
        return result;
    }
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    {
        std::unique_lock<std::mutex> lock(_gateMutex);

        // ---- 帧序门 ----
        if (_nextSeq < 0) _nextSeq = frameIndex;
        if (frameIndex < _nextSeq) {
            // 过期帧:槽纹理里是别的帧的流,清零发布;不推进门、不重置。
            result.publishZero = true;
            _lastStageMs = 0.0;
            return result;
        }
        if (frameIndex > _nextSeq) {
            // 前驱可能仍在飞行(VS 重叠激活):有界等待;超时按跳帧处理
            // (前驱从未被激活 —— 重置历史,跳到本帧)。
            const int64_t target = frameIndex;
            if (_gateCv.wait_for(lock, std::chrono::milliseconds(250),
                                 [&] { return _nextSeq >= target; })) {
                // 顺序恢复。但若另一个超时跳帧已越过本帧,本帧已成过期帧:
                // 清零发布、不推进门(防 _nextSeq 回退)。
                if (_nextSeq > frameIndex) {
                    result.publishZero = true;
                    _lastStageMs = 0.0;
                    return result;
                }
            } else {
                _nextSeq = frameIndex;
                _historyValid = false;
            }
        }

        const int cur = _curInput;
        const bool seed = !_historyValid;
        bool execute = !seed;

        ID3D12CommandQueue *queue = _d3d12->Queue();

        // 拷贝路径:queue Wait(doneFence >= 上次 execute)保住上一帧对
        // reference 槽位的读,再提交本帧拷贝并发布 copyFence。
        if (_lastDone) {
            queue->Wait(_doneFence.Get(), _lastDone);
        }
        if (_lastCopyFence &&
            _copyFence->GetCompletedValue() < _lastCopyFence) {
            _copyFence->SetEventOnCompletion(_lastCopyFence, _copyFenceEvent);
            WaitForSingleObject(_copyFenceEvent, 10000);
        }
        bool copyOk = SUCCEEDED(_copyAllocator->Reset()) &&
                      SUCCEEDED(_copyCommandList->Reset(_copyAllocator.Get(), nullptr));
        if (copyOk) {
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = uploadBuffer;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = 0;
            src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            src.PlacedFootprint.Footprint.Width = static_cast<UINT>(_width);
            src.PlacedFootprint.Footprint.Height = static_cast<UINT>(_height);
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = uploadRowPitch;
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = _input[cur].Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;
            _copyCommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            copyOk = SUCCEEDED(_copyCommandList->Close());
        }
        if (copyOk) {
            ID3D12CommandList *lists[]{ _copyCommandList.Get() };
            queue->ExecuteCommandLists(1, lists);
            _lastCopyFence = ++_copySeq;
            queue->Signal(_copyFence.Get(), _lastCopyFence);
        } else {
            // 拷贝失败:清零 + 重置(下帧重新播种)。失败必须进 timing log。
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "DLSSNR STATUS: nvof copy submit failed frame=%d", frameIndex);
            OutputDebugStringA("vs_dlssnr: nvof copy submit failed\n");
            TimingStatusLine(msg);
            result.publishZero = true;
            result.historyReset = true;
            _historyValid = false; // 参考帧未更新,历史链断
            execute = false;
        }

        if (execute) {
            // execute(n):输入栅栏点 = {copyFence, k_n}(输入内容就绪)
            // + {doneFence, doneByParity[cur]}(本槽位 flow 空出);
            // 输出栅栏点 = {doneFence, 新值}(SubmitFrame 等它)。
            NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{};
            in.inputFrame = _registered[cur];
            in.referenceFrame = _registered[1 - cur];
            in.disableTemporalHints = NV_OF_FALSE; // Magpie 同款:重置靠播种帧表达
            NV_OF_FENCE_POINT inFence[2]{};
            inFence[0].fence = _copyFence.Get();
            inFence[0].value = _lastCopyFence;
            inFence[1].fence = _doneFence.Get();
            inFence[1].value = _doneByParity[cur];
            in.numFencePoints = _doneByParity[cur] ? 2u : 1u;
            in.fencePoint = inFence;
            NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{};
            out.outputBuffer = _registered[2];
            out.outputCostBuffer = _costEnabled ? _registered[4] : nullptr;
            out.bwdOutputBuffer = _bidirectional ? _registered[3] : nullptr;
            out.bwdOutputCostBuffer =
                _bidirectional && _costEnabled ? _registered[5] : nullptr;
            NV_OF_FENCE_POINT outFence{};
            outFence.fence = _doneFence.Get();
            outFence.value = ++_doneSeq;
            out.fencePoint = &outFence;

            const NV_OF_STATUS st = _api.nvOFExecuteD3D12(_session, &in, &out);
            if (st == NV_OF_SUCCESS) {
                // 官方样例同款:execute 后 CPU 等输出栅栏(NVOF 自有引擎完成
                // 后置位)。等待落位后再提交槽 CL,densify 在 GPU 上执行时
                // flow 已就绪 —— 不需要(也不可靠)队列级 Wait:NVOF 的输出
                // 栅栏在队列 Wait 语义下可能永不满足(实测 2026-09-07)。
                bool done = true;
                if (_doneFence->GetCompletedValue() < outFence.value) {
                    _doneFence->SetEventOnCompletion(outFence.value, _copyFenceEvent);
                    done = WaitForSingleObject(_copyFenceEvent, 10000) == WAIT_OBJECT_0;
                }
                if (!done) {
                    TimingStatusLine("DLSSNR STATUS: nvof output fence timeout");
                    result.publishZero = true;
                    result.historyReset = true;
                    _historyValid = false;
                    if (++_consecutiveFailures >= 3) {
                        _ready.store(false, std::memory_order_release);
                        TimingStatusLine("DLSSNR STATUS: nvof disabled after consecutive failures");
                    }
                } else {
                    result.waitFenceValue = outFence.value; // 仅作 densify 录制判据
                    _lastDone = outFence.value;
                    _doneByParity[cur] = outFence.value;
                    _consecutiveFailures = 0;
                    if (_executesLogged < 5) {
                        ++_executesLogged;
                        char msg[128];
                        std::snprintf(msg, sizeof(msg),
                                      "DLSSNR STATUS: nvof execute ok frame=%d grid=%u bidir=%d cost=%d",
                                      frameIndex, _gridSize, _bidirectional ? 1 : 0, _costEnabled ? 1 : 0);
                        TimingStatusLine(msg);
                    }
                }
            } else {
                char last[96]{};
                uint32_t lastLen = sizeof(last);
                if (_api.nvOFGetLastError) {
                    _api.nvOFGetLastError(_session, last, &lastLen);
                }
                // 失败必须进 timing log(项目惯例:跨进程观测只认它)。
                char msg[224];
                std::snprintf(msg, sizeof(msg),
                              "DLSSNR STATUS: nvof execute failed frame=%d status=0x%x %s",
                              frameIndex, static_cast<unsigned>(st), last);
                OutputDebugStringA("vs_dlssnr: ");
                OutputDebugStringA(msg);
                OutputDebugStringA("\n");
                TimingStatusLine(msg);
                result.publishZero = true;
                result.historyReset = true;
                _historyValid = false;
                if (++_consecutiveFailures >= 3) {
                    _ready.store(false, std::memory_order_release);
                    char msg2[96];
                    std::snprintf(msg2, sizeof(msg2),
                                  "DLSSNR STATUS: nvof disabled after consecutive failures");
                    OutputDebugStringA("vs_dlssnr: ");
                    OutputDebugStringA(msg2);
                    OutputDebugStringA("\n");
                    TimingStatusLine(msg2);
                }
            }
        } else if (copyOk) {
            // 播种帧:发布零运动 + NGX 重置(首帧/重置后的第一帧)。
            result.publishZero = true;
            result.historyReset = true;
        }

        // 推进门:下一帧放行;ping-pong 翻转。历史状态:
        //   execute 成功 → 历史连续(下一帧可 execute);
        //   execute 失败 → 历史作废(失败分支已置 false);
        //   播种帧(拷贝成功)→ 历史已建立,下一帧可 execute。
        _curInput = 1 - cur;
        if (seed && copyOk) _historyValid = true;
        _nextSeq = static_cast<int64_t>(frameIndex) + 1;
        _gateCv.notify_all();
    }

    QueryPerformanceCounter(&t1);
    _lastStageMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                   static_cast<double>(freq.QuadPart);
    return result;
}

} // namespace vsdlssnr
