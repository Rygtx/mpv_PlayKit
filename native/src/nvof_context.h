#pragma once
// D3D12 原生 NVOF 光流 provider(PORTING 清单 #6)。对应 Magpie 的
// NvidiaOpticalFlowProvider + FrameGuidanceService 的 select/zero/fallback
// 职责 —— 但不含 D3D11 互操作层:Magpie 需要 FrameGuidanceD3D12Interop 是
// 因为它的渲染器是 D3D11、多消费者;本插件单消费者、纯 D3D12,直接用
// nvofapi64.dll 的 D3D12 API(设备级会话,栅栏同步,无命令列表参数)。
//
// 时序模型(与 fmParallel 三槽乱序提交共存,全部栅栏边、顺序无关、无死锁)。
// 注意 flow/cost 输出缓冲是**固定**的(execute 双向时两块都写),不是
// ping-pong —— 帧间保护靠“densify 也在门内、按帧序提交”的链式栅栏:
//   1. copy(n)  [我们的 DIRECT 队列]   waits doneFence >= m_{n-1}
//      —— copy(n+1) 覆写 nvofInput 的 reference 槽位前,上一帧 execute 必须
//         已经读完它(queue Wait,顺序无关)。
//   2. execute(n) [NVOF 内部引擎]      waits copyFence >= k_n(输入内容就绪)
//                                      + doneFence >= m_{同槽位上一次}
//   3. execute 后 CPU 等 doneFence >= m_n(官方样例模式;队列级 Wait 实测
//      不可靠),然后在门内把 densify 记录到同一 nvof CL 上二次提交并
//      Signal(copyFence, k')—— execute(n+1) 的输入栅栏点含 k_{n+1} > k',
//      GPU 队列 FIFO 保证 densify(n) 先于 copy(n+1) 先于 execute(n+1)。
//   4. 槽 CL(n) 的 NGX evaluate:与 densifyCL(n) 同队列、由同线程后提交,
//      FIFO 顺序 ✓;跨 CL 状态链(UAV→NSR 于 nvofCL,NSR→COMMON 于槽 CL)
//      合法(同队列保序)。
//
// 帧序门(_gateMutex):VS fmParallel 激活顺序单调但可重叠,门只放行
// _nextSeq 当前帧;前驱未到时 cv 等待(有超时),超时按跳帧处理(重置历史)。
// 门内工作 = 拷贝提交 + execute + CPU 等 + densify 提交,天然按帧序串行。

#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <windows.h>

#include <nvOpticalFlowD3D12.h> // vendor/nvof(fetch-deps.ps1),含 nvOpticalFlowCommon.h

namespace vsdlssnr {

class D3D12Context;

class NvofContext {
public:
    struct StageResult {
        uint64_t waitFenceValue = 0; // SubmitFrame 需等待的 done 栅栏值(0 = 无)
        bool publishZero = false;    // 本帧清零 per-slot motion/confidence
        bool historyReset = false;   // 本帧对 NGX 置 PARAM_RESET
    };

    NvofContext() = default;
    ~NvofContext();
    NvofContext(const NvofContext &) = delete;
    NvofContext &operator=(const NvofContext &) = delete;

    // 创建/重建 quality(1-5)档位的 OF 会话。调用方必须持 PoolHold
    // (槽池封死):in-flight 命令列表不得引用注册纹理,门也必然空闲
    // (门只被持有槽位的帧线程进入)。
    bool Initialize(D3D12Context &d3d12, int width, int height, int quality,
                    char *err, size_t errLen) noexcept;
    // 释放会话 + 卸载驱动 DLL。PoolHold 约束同上。
    void Finalize() noexcept;

    bool Enabled() const noexcept { return _ready.load(std::memory_order_acquire); }
    int Quality() const noexcept { return _quality; }
    int Width() const noexcept { return _width; }
    int Height() const noexcept { return _height; }

    // 注册纹理(会话存活期有效);供 D3D12Context 在槽描述符堆上建 SRV。
    ID3D12Resource *FlowForward() const noexcept { return _flow[0].Get(); }
    ID3D12Resource *FlowBackward() const noexcept { return _bidirectional ? _flow[1].Get() : nullptr; }
    ID3D12Resource *CostForward() const noexcept { return _costEnabled ? _cost[0].Get() : nullptr; }
    ID3D12Resource *CostBackward() const noexcept {
        return _bidirectional && _costEnabled ? _cost[1].Get() : nullptr;
    }
    // densify 的 cbuffer 旗标与 SubmitFrame 的栅栏等待目标。
    uint32_t GridSize() const noexcept { return _gridSize; }
    bool Bidirectional() const noexcept { return _bidirectional; }
    bool CostEnabled() const noexcept { return _costEnabled; }
    ID3D12Fence *DoneFence() const noexcept { return _doneFence.Get(); }

    // 历史失效(seek = 新滤镜实例):下一帧重新播种(清零发布,不 execute)。
    // 同时复位帧序门 —— 新时间线的帧号与旧时间线无关,不清会让所有新帧
    // 被判过期。
    void ResetHistory() noexcept {
        std::lock_guard<std::mutex> lock(_gateMutex);
        _historyValid = false;
        _nextSeq = -1;
        _gateCv.notify_all();
    }

    // 帧路径:PackInput 之后(槽 upload 已含本帧 BGRA8)、SubmitFrame 之前,
    // 在帧线程上调用。帧序门 → 拷贝 CL 提交 → (历史有效时) execute →
    // CPU 等输出栅栏 → postExecute 回调(门内、同一 nvof CL 上二次提交,
    // 调用方在此记录 densify/清零 —— 与 execute 的完成构成栅栏链)。
    // postExecute 为空(OF 停用帧)时跳过 densify,其余语义不变。
    using PostExecuteFn = std::function<void(ID3D12GraphicsCommandList *cl)>;
    StageResult StageFrame(int frameIndex, ID3D12Resource *uploadBuffer,
                           UINT uploadRowPitch, const PostExecuteFn &postExecute) noexcept;

    // 本帧 NVOF 拷贝的完成等待(early-return 路径释放槽位前调用,防宿主
    // 复用 upload 缓冲撕裂在途拷贝;正常路径已被 execute 栅栏覆盖,no-op)。
    void WaitCopyIdle() noexcept;

    // 最近一次 StageFrame 的 CPU 耗时(门等待 + 拷贝提交 + execute 调用),ms。
    double LastStageMs() const noexcept { return _lastStageMs; }

private:
    void DestroySession() noexcept;
    bool CreateSession(D3D12Context &d3d12, int width, int height, int quality,
                       char *err, size_t errLen) noexcept;

    using CreateApiFn = NV_OF_STATUS(NVOFAPI *)(uint32_t, NV_OF_D3D12_API_FUNCTION_LIST *);
    using GetMaxVersionFn = NV_OF_STATUS(NVOFAPI *)(uint32_t *);

    HMODULE _module = nullptr;
    NV_OF_D3D12_API_FUNCTION_LIST _api{};
    NvOFHandle _session = nullptr;

    D3D12Context *_d3d12 = nullptr;
    int _width = 0;
    int _height = 0;
    int _quality = 0;
    uint32_t _gridSize = 0;
    bool _bidirectional = false;
    bool _costEnabled = false;

    // 注册纹理:输入 ping-pong + flow ping-pong + cost ping-pong。
    // 样例与 Magpie 同款:DEFAULT 堆 + ALLOW_UNORDERED_ACCESS,常驻 COMMON
    // (注册之后由 NVOF 引擎与我方拷贝按栅栏点互斥访问,不做逐帧状态迁移)。
    Microsoft::WRL::ComPtr<ID3D12Resource> _input[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> _flow[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> _cost[2];
    NvOFGPUBufferHandle _registered[6]{};

    // 拷贝命令路径(持久 CL + allocator;重置前栅栏校验上次提交已完成)。
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> _copyAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> _copyCommandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> _copyFence;  // 我方拷贝完成(app → NVOF)
    Microsoft::WRL::ComPtr<ID3D12Fence> _doneFence;  // NVOF 输出完成(NVOF → 我方)
    HANDLE _copyFenceEvent = nullptr;
    uint64_t _copySeq = 0;        // copyFence 单调计数
    uint64_t _lastCopyFence = 0;  // 最近一次拷贝提交的值
    uint64_t _doneSeq = 0;        // doneFence 单调计数(注册 + execute 共用)
    uint64_t _lastDone = 0;       // 最近一次 execute 的 done 值(copy(n+1) 等它)
    uint64_t _doneByParity[2]{};  // 每个输入槽位最近一次被 execute 写入的 done 值

    // 帧序门 + 会话状态(gateMutex 保护)。
    std::mutex _gateMutex;
    std::condition_variable _gateCv;
    int64_t _nextSeq = -1;        // 下一帧序;-1 = 未定(首帧自定)
    bool _historyValid = false;
    int _curInput = 0;            // ping-pong 当前写槽位
    uint32_t _consecutiveFailures = 0;
    int _executesLogged = 0;      // 前 5 次 execute 的诊断日志计数
    std::atomic<bool> _ready{ false };

    double _lastStageMs = 0.0;
};

} // namespace vsdlssnr
