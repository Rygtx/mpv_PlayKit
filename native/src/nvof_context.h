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
//   3. execute(n) 提交成功后 **CPU 等输出栅栏被延迟**(官方样例模式;队列级
//      Wait 实测不可靠,故仍是 CPU 等):StageFrame 只做提交 + 门推进,门锁
//      随 OfStageResult::pendingDensify 移交给调用方;调用方在首个 motion
//      消费者 CL(NGX base / FG fg CL)提交前调 FlushPendingDensify ——
//      CPU 等 doneFence >= m_n 在那里进行,与 eval/FG 录制重叠,醒来后把
//      densify 记录到轮转池的独立 CL 上二次提交并 Signal(copyFence, k')。
//      门锁全程由调用方持有 = 提交互斥/轮转簿记与旧"门内全程"形态等价,
//      densify(n) 仍恒先于 copy(n+1) 先于 execute(n+1)(后两者的输入栅栏
//      点含 k_{n+1} > k',GPU 队列 FIFO 保序)。
//   4. 槽 CL(n) 的 NGX evaluate:与 densifyCL(n) 同队列、由同线程后提交,
//      FIFO 顺序 ✓;跨 CL 状态链(UAV→NSR 于 nvofCL,NSR→COMMON 于槽 CL)
//      合法(同队列保序)。
//
// 帧序门(_gateMutex):_nextSeq = 上一完成帧 + 1。到达帧 == _nextSeq 才
// execute;不匹配一律播种(零 guidance)—— 到得早的(乱序/回退)与缺了
// 前驱的(host 丢帧/大跳)都走播种,链在下一帧立即恢复,不做簿记。
// 唯一等待:缺口恰为 1 时 cv 让路 15ms(fmParallel 相邻竞争下前驱大概率
// 正卡在门 mutex 上,cv 释放锁的语义让它插队完成,两帧都保住 guidance);
// 真丢帧则 15ms 后播种,代价钉死。门内工作 = 拷贝提交 + execute + CPU 等
// + densify 提交,天然按帧序串行。

#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <windows.h>

#include <nvOpticalFlowD3D12.h> // vendor/nvof(fetch-deps.ps1),含 nvOpticalFlowCommon.h

#include "of_backend.h" // IOpticalFlowBackend + OfStageResult/回调(上移共享)

namespace vsdlssnr {

class D3D12Context;

class NvofContext final : public IOpticalFlowBackend {
public:
    // 类型别名:原类内定义上移到 of_backend.h(接口共享);保持
    // NvofContext::StageResult 等既有引用点零改动。
    using StageResult = OfStageResult;
    using PostExecuteFn = OfPostExecuteFn;
    using PostCopyFn = OfPostCopyFn;

    NvofContext() = default;
    ~NvofContext() override;
    NvofContext(const NvofContext &) = delete;
    NvofContext &operator=(const NvofContext &) = delete;

    // 创建/重建 quality(1-5)档位的 OF 会话。调用方必须持 PoolHold
    // (槽池封死):in-flight 命令列表不得引用注册纹理,门也必然空闲
    // (门只被持有槽位的帧线程进入)。
    bool Initialize(D3D12Context &d3d12, int width, int height, int quality,
                    char *err, size_t errLen) noexcept override;
    // 释放会话 + 卸载驱动 DLL。PoolHold 约束同上。
    void Finalize() noexcept override;

    bool Enabled() const noexcept override { return _ready.load(std::memory_order_acquire); }
    int Quality() const noexcept override { return _quality; }
    int Width() const noexcept override { return _width; }
    int Height() const noexcept override { return _height; }

    // 注册纹理(会话存活期有效);供 D3D12Context 在槽描述符堆上建 SRV。
    ID3D12Resource *FlowForward() const noexcept { return _flow[0].Get(); }
    ID3D12Resource *FlowBackward() const noexcept { return _bidirectional ? _flow[1].Get() : nullptr; }
    ID3D12Resource *CostForward() const noexcept { return _costEnabled ? _cost[0].Get() : nullptr; }
    ID3D12Resource *CostBackward() const noexcept {
        return _bidirectional && _costEnabled ? _cost[1].Get() : nullptr;
    }
    // 注册输入纹理(ping-pong,诊断 dump 用;index 0/1)。
    ID3D12Resource *InputTexture(int index) const noexcept override { return _input[index].Get(); }
    // densify 的 cbuffer 旗标与槽提交的栅栏等待目标。
    uint32_t GridSize() const noexcept { return _gridSize; }
    bool Bidirectional() const noexcept { return _bidirectional; }
    bool CostEnabled() const noexcept { return _costEnabled; }
    ID3D12Fence *DoneFence() const noexcept { return _doneFence.Get(); }
    // SK_OF_MODE 能力段(实际模式,逐级回退的可见反馈)。
    const char *ModeString(char *buf, size_t len) noexcept override {
        std::snprintf(buf, len, "%s q%d grid%u",
                      _bidirectional ? (_costEnabled ? "both+cost" : "both")
                                     : (_costEnabled ? "forward+cost" : "forward"),
                      _quality, _gridSize);
        return buf;
    }
    int Kind() const noexcept override { return kOfBackendNvof; }

    // 历史失效(seek = 新滤镜实例):下一帧重新播种(清零发布,不 execute)。
    // 同时复位帧序门 —— 新时间线的帧号与旧时间线无关,不清会让新帧被判
    // 迟到帧。零等待门下乱序/大跳均自愈(播种一次即恢复),此复位只服务
    // "门内残留旧时间线状态"的场景。
    void ResetHistory() noexcept override {
        std::lock_guard<std::mutex> lock(_gateMutex);
        _historyValid = false;
        _nextSeq = -1;
        ++_resetCount; // 临时探针
        _gateCv.notify_all();
    }

    // 帧路径:PackInput 之后、SubmitFrame 之前,在帧线程上调用。帧序门 →
    // 拷贝 CL 提交 → (历史有效时) execute 提交 → 门推进 → **门锁移交**
    // (execute 成功帧)返回。CPU 等引擎输出栅栏与 densify 提交(2026-09-25
    // 延迟改造)在 FlushPendingDensify 进行:调用方在首个 motion 消费者 CL
    // (NGX base / FG fg CL)提交前调用,与 eval/FG 录制重叠;失败路径由
    // 调用方守卫析构兜底冲刷。postExecute 为空(OF 停用帧)时不延迟、按旧
    // 语义跳过 densify。
    // YUV 原生:postCopy 恒设,回调在本 nvof CL 上记录 YUV→RGB 转换
    // (yuvUpload→yuvIn 拷贝 + dispatch → inputColor);inputWrittenByPostCopy
    // = follow(回调内含 RecordNvofDownsample,直接写 _input[cur])时为
    // true;非 follow 为 false —— 此时回调只做转换,本函数随后把 srcTex
    // (inputColor,NSR)整帧纹理拷贝进 _input[cur](NSR→COPY_SOURCE→copy→
    // 回 NSR;目标 COMMON 靠隐式提升,与旧 buffer 拷贝同款)。
    // **迟到帧契约**:帧序门对迟到/过期帧提前 return、不提交任何 CL,
    // inputIndex 保持 -1 —— 调用方据此在槽 CL 上补做转换
    // (convertedOnNvof = inputIndex >= 0)。copyOk=false 同样 -1。
    // 回调签名 (cl, cur):cur 为本帧输入槽位(0/1,UAV 描述符 23/24)。
    OfStageResult StageFrame(int frameIndex, ID3D12Resource *srcTex,
                             const OfPostExecuteFn &postExecute,
                             const OfPostCopyFn &postCopy, bool inputWrittenByPostCopy,
                             std::unique_lock<std::mutex> &gateOut) noexcept override;

    // 冲刷延迟的 densify(见 StageFrame 注释)。前置条件:pendingDensify
    // = true 且调用方持有 StageFrame 移出的门锁(提交互斥/轮转簿记串行)。
    // CPU 等 doneFence >= m_n 在此(有界 10s,超时 = 会话退役);醒来后
    // AcquireCl + postExecute 录制 + 提交 + Signal(copyFence)。幂等(无
    // 待冲刷即刻返回);失败仅留痕/退役 + 清除待冲刷 —— 本帧已按
    // realMotion 语义录完参数,运动场陈旧一帧可接受。
    void FlushPendingDensify(const OfPostExecuteFn &postExecute) noexcept override;

    // 本帧 NVOF 拷贝的完成等待(early-return 路径释放槽位前调用,防宿主
    // 复用 upload 缓冲撕裂在途拷贝;正常路径已被 execute 栅栏覆盖,no-op)。
    void WaitCopyIdle() noexcept override;

private:
    // 栅栏值到达等待:循环检查完成值(共享 auto-reset 事件的唤醒可能被
    // 其它等待者窃取,单次 Wait 结果不可信),单调值保证有界退出。
    static bool WaitFenceReached(ID3D12Fence *fence, uint64_t value,
                                 HANDLE event, DWORD timeoutMs) noexcept;
    // 轮转池取 CL(门内调用):Reset 前等本位上一段提交完成(4 段之前常态
    // 即刻返回;超 4 段 = 背压,10s 上限,超时会话退役)。含 force-close
    // 自愈(FfxofContext::AcquireCl 同款)。freq 仅服务 c% 探针计时。
    bool AcquireCl(ID3D12CommandAllocator **allocator,
                   ID3D12GraphicsCommandList **cl, LARGE_INTEGER freq) noexcept;

public:

    // 最近一次 StageFrame 的 CPU 耗时(门等待 + 拷贝提交 + execute 调用),ms。
    double LastStageMs() const noexcept override { return _lastStageMs; }
    // 光流阶段全跨度(2026-09-25 语义修正):门入口 → 冲刷完成 = 提交 +
    // 引擎计算 + 暴露等待,即真实光流处理用时(nvof 段上报值)。延迟冲刷
    // 帧在 FlushPendingDensify 落位;种子/迟到帧 = StageFrame 跨度。
    double LastStageTotalMs() const noexcept override { return _lastOfWallMs; }

    // ---- 临时探针(定位 seek 后持续掉帧,验证后删除)----
    // StageFrame 内三段 CPU 等待细分 + 门异常事件累计。
    double LastGateWaitMs() const noexcept { return _lastGateWaitMs; } // 门互斥+cv 等待
    double LastCpyWaitMs() const noexcept { return _lastCpyWaitMs; }   // 前帧拷贝完成 CPU 等待
    double LastExeWaitMs() const noexcept { return _lastExeWaitMs; }   // execute 输出栅栏 CPU 等待
    uint32_t GateSkips() const noexcept { return _gateSkips; }         // cv 超时跳帧累计
    uint32_t GateExpired() const noexcept { return _gateExpired; }     // 过期帧累计
    uint32_t ResetCount() const noexcept { return _resetCount; }       // ResetHistory 累计

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

    // 拷贝命令路径:4 深轮转池(2026-09-25,对齐 FfxofContext::AcquireCl)。
    // 此前单 allocator:每帧 copy/densify 复用同一对,Reset 前 CPU 等上一帧
    // 完成(_lastCopyFence 等待,探针 c%)—— GPU 落后时该等待在门内串行。
    // 池化后常态零等待,背压(落后 >4 段)才等待。copy 与 densify 各占一个
    // 轮转位(每帧 2 段,4 深 = 2 帧跑道;execute 输出的 CPU 等待天然把
    // 帧间距离拉开,跑道远大于消耗)。
    static constexpr int kClDepth = 4;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> _alloc[kClDepth];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> _cl[kClDepth];
    ID3D12Fence *_lastUseFence[kClDepth]{};   // 轮转位最近提交的栅栏(恒 _copyFence)
    uint64_t _lastUseValue[kClDepth]{};       // 轮转位最近提交的栅栏值
    uint64_t _submitSeq = 0;                  // 轮转指针(每段提交 +1)
    Microsoft::WRL::ComPtr<ID3D12Fence> _copyFence;  // 我方拷贝完成(app → NVOF)
    Microsoft::WRL::ComPtr<ID3D12Fence> _doneFence;  // NVOF 输出完成(NVOF → 我方)
    // 两个自动重置事件按栅栏分家:auto-reset 事件被多等待者共享时会发生
    // 唤醒窃取(一个等待者消费掉另一个的唤醒),跨栅栏窃取会把"未完成"
    // 误判为"已完成"—— copyFence 的等待(WaitCopyIdle 不持门)与
    // doneFence 的等待(门内串行)绝不共用。
    HANDLE _copyFenceEvent = nullptr;
    HANDLE _doneFenceEvent = nullptr;
    uint64_t _copySeq = 0;        // copyFence 单调计数
    uint64_t _lastCopyFence = 0;  // 最近一次拷贝提交的值
    uint64_t _doneSeq = 0;        // doneFence 单调计数(注册 + execute 共用)
    uint64_t _lastDone = 0;       // 最近一次 execute 的 done 值(copy(n+1) 等它)
    uint64_t _doneByParity[2]{};  // 每个输入槽位最近一次被 execute 写入的 done 值
    // 延迟 densify(2026-09-25):待冲刷帧的 execute done 值 + 输入槽位。
    // 0 = 无待冲刷。仅单在飞:门锁由调用方从 StageFrame 持到冲刷,期间无
    // 其它帧可入门置位。StageFrame 入门即清(失败路径残留作废 —— 其
    // ProcessFrame 已退出, densify 不再有意义)。
    uint64_t _pendingDensifyValue = 0;
    int _pendingDensifyInput = 0;

    // 帧序门 + 会话状态(gateMutex 保护)。
    std::mutex _gateMutex;
    std::condition_variable _gateCv;
    int64_t _nextSeq = -1;        // 上一完成帧 + 1;-1 = 未定(首帧自定)
    bool _historyValid = false;
    int _curInput = 0;            // ping-pong 当前写槽位
    uint32_t _consecutiveFailures = 0;
    int _executesLogged = 0;      // 前 5 次 execute 的诊断日志计数
    std::atomic<bool> _ready{ false };

    double _lastStageMs = 0.0;
    double _lastOfWallMs = 0.0;   // 光流阶段全跨度(门入口 → 冲刷完成,2026-09-25)
    UINT64 _stageStartQpc = 0;    // 本帧门入口 QPC(冲刷完成时算全跨度)
    double _lastGateWaitMs = 0.0; // 临时探针(验证后删除)
    double _lastCpyWaitMs = 0.0;  // 临时探针(验证后删除)
    double _lastExeWaitMs = 0.0;  // 临时探针(验证后删除)
    uint32_t _gateSkips = 0;      // 临时探针(验证后删除)
    uint32_t _gateExpired = 0;    // 临时探针(验证后删除)
    uint32_t _resetCount = 0;     // 临时探针(验证后删除)
};

} // namespace vsdlssnr
