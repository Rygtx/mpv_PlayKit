#pragma once
// 光流后端公共接口(AMD 光流移植;与 NvofContext 并列的后端挂载点)。
// 两实现:NvofContext(NVIDIA NVOF 引擎,在役链路,时序模型见其头注释)与
// FxofContext(AMD FidelityFX OF,跨厂商)。接口面 = NvofContext 原公开面;
// 语义契约一致:
//   - StageFrame 帧序门:迟到/缺口帧一律播种(清零发布),会话内部
//     历史只接受按帧序的连续输入;
//   - 播种帧(Seed/历史无效)必须 historyReset = true 且
//     waitFenceValue = 0(2026-09-25 钉死契约;NGX PARAM_RESET 只由播种帧
//     携带,后端漏带 = seek 后 NR 时域历史跨时间线泄漏);
//   - waitFenceValue != 0 = 本帧真运动已产出(realMotion 判据);
//   - inputIndex >= 0 = 本帧输入准备(转换)已随成功提交的会话 CL 落地,
//     调用方跳过槽 CL 的补转换;-1 = 本帧无转换(迟到/失败/OF 关),槽 CL
//     补做(NVOF = ping-pong 槽位;FFX 恒 0,单输入纹理);
//   - LastStageMs = 门内等待 + 提交 + CPU 等待合计(nvof 段计时同语义)。
// PSO 与描述符槽位在 D3D12Context(BindOfResources/Record* 助手);context
// 类只做编排(门、栅栏、FFX API 调用)。
// (原第三实现 HalfResContext 兜底已于 2026-09-21 随 FFX 的 AMD 卡验证
//  通过而移除 —— 无存活场景;of_frame_gate.h 仍由 FxofContext 使用。)

#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>

#include "dlssnr_params.h" // kOfBackend* 常量(Kind() 返回值;params 为唯一权威)
#include "d3d12_context.h" // OfClRotator:IsDeviceLost / ResetAllocatorHealed(自身不含本件,无环)

namespace vsdlssnr {

// 闩锁留痕出口(本体内 TimingStatusLine 在 dlssnr_context;避免反向 include,
// 重复声明合法)。
void TimingStatusLine(const char *line) noexcept;

// 栅栏值到达等待(两光流后端共用):共享 auto-reset 事件的唤醒可能被同
// 事件的其它等待者窃取(单次 Wait 返回不代表本等待的目标值已达成),循环
// 复查完成值;栅栏值单调 ⇒ 有界退出。
// CL 轮转池的 allocator Reset + force-close 自愈(ResetAllocatorHealed)
// 已上移 d3d12_context.h(全仓唯一实现,含 d3d12 侧五处原手写份)。
inline bool WaitFenceReached(ID3D12Fence *fence, uint64_t value,
                             HANDLE event, DWORD timeoutMs) noexcept {
    for (;;) {
        if (fence->GetCompletedValue() >= value) return true;
        if (FAILED(fence->SetEventOnCompletion(value, event))) return false;
        if (WaitForSingleObject(event, timeoutMs) != WAIT_OBJECT_0) return false;
    }
}

// 栅栏 + 等待事件配对(轮转位背压等待的事件选择;NVOF 单栅栏恒同事件,
// FFX copy/done 双栅栏按身份选事件 —— 事件分家防唤醒窃取,见两会话注释)。
struct OfFenceEvent {
    ID3D12Fence *fence;
    HANDLE event;
};

// CL 轮转池(NvofContext/FxofContext 共用脚手架;此前 AcquireCl 在两会话
// 各写一份,2026-09-25 轮转池改造被迫双处同步落盘):idx = seq % 4,Reset
// 前 CPU 等同位上一段提交 —— 4 段之前常态"等待即刻返回",GPU 落后超 4 段
// = 背压。等待期间设备丢失 = 永不满足,快速失败。Reset 含 force-close 自愈
// (d3d12_context.h)。门内串行(OfFrameGate),会话级簿记无并发等待者。
class OfClRotator {
public:
    // 建 4 深 allocator/CL 对(DIRECT;建后即 Close 待录)。失败返回 false
    // (会话建失败路径收口,报错由调用方 fail() 统一落)。设备取自 d3d12
    // (2026-10-04:撤双参数形态 —— 同一设备两种真相来源,可传不一致)。
    bool Create(D3D12Context &d3d12) noexcept {
        _d3d12 = &d3d12;
        ID3D12Device *device = d3d12.Device();
        for (int i = 0; i < kDepth; ++i) {
            if (FAILED(device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(_alloc[i].GetAddressOf()))) ||
                FAILED(device->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT, _alloc[i].Get(), nullptr,
                    IID_PPV_ARGS(_cl[i].GetAddressOf()))) ||
                FAILED(_cl[i]->Close())) {
                return false;
            }
        }
        return true;
    }

    enum class AcquireResult { Ok, Timeout, ResetFailed };
    // 取轮转位:背压等待(fe 表按栅栏身份选事件,无匹配 = fe[0].event)+
    // 自愈 Reset。Timeout = 背压 10s 未达(调用方留痕 + 退役);ResetFailed
    // = 自愈后仍失败(设备级故障,调用方直接失败)。
    AcquireResult Acquire(ID3D12CommandAllocator **allocator,
                          ID3D12GraphicsCommandList **cl,
                          const OfFenceEvent *fe, size_t feCount) noexcept {
        const int idx = static_cast<int>(_seq % kDepth);
        if (_lastFence[idx] && _lastValue[idx] && !_d3d12->IsDeviceLost()) {
            HANDLE ev = fe[0].event;
            for (size_t i = 0; i < feCount; ++i) {
                if (fe[i].fence == _lastFence[idx]) { ev = fe[i].event; break; }
            }
            if (!WaitFenceReached(_lastFence[idx], _lastValue[idx], ev, 10000)) {
                return AcquireResult::Timeout;
            }
        }
        if (!ResetAllocatorHealed(_alloc[idx].Get(), _cl[idx].Get())) {
            return AcquireResult::ResetFailed;
        }
        *allocator = _alloc[idx].Get();
        *cl = _cl[idx].Get();
        return AcquireResult::Ok;
    }

    // 提交 + 记账单点(2026-10-04 收拢):ECL → Signal → RecordUse 三步
    // 此前散布 NVOF/FFX 五处提交点,靠"每段恰一次"的注释纪律维持 —— 漏记
    // RecordUse 不会报错,错误在 4 段后才以背压假超时现形(极难归因)。
    // value = 调用方每栅栏的单调计数(copySeq/doneSeq 各自独立,外部读
    // _lastDone/_lastCopyFence 的语义不变)。返回 value(便于链式落账)。
    uint64_t Submit(ID3D12CommandQueue *queue, ID3D12Fence *fence,
                    ID3D12GraphicsCommandList *cl, uint64_t value) noexcept {
        ID3D12CommandList *lists[]{ cl };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, value);
        RecordUse(fence, value);
        return value;
    }

    // 会话销毁:栅栏指针随会话失效,簿记清零(退役会话不再 Acquire,
    // 清零防悬垂比较)。
    void ResetBookkeeping() noexcept {
        for (int i = 0; i < kDepth; ++i) {
            _lastFence[i] = nullptr;
            _lastValue[i] = 0;
        }
        _seq = 0;
    }

private:
    // 提交记账:idx 位的在飞栅栏值(经 Submit 调用;不再单独暴露 ——
    // 配对关系从纪律变结构)。
    void RecordUse(ID3D12Fence *fence, uint64_t value) noexcept {
        const int idx = static_cast<int>(_seq % kDepth);
        _lastFence[idx] = fence;
        _lastValue[idx] = value;
        ++_seq;
    }

    D3D12Context *_d3d12 = nullptr;
    static constexpr int kDepth = 4;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> _alloc[kDepth];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> _cl[kDepth];
    ID3D12Fence *_lastFence[kDepth] = {}; // 轮转位最近提交的栅栏
    uint64_t _lastValue[kDepth] = {};     // 轮转位最近提交的栅栏值
    uint64_t _seq = 0;                    // 轮转指针(每段提交 +1)
};

class D3D12Context;

// StageFrame 回调(原 NvofContext 类内定义,上移共享):
//   postExecute:真运动产出后在会话 CL 上录制 densify/估算(NVOF = 延迟到
//               FlushPendingDensify,FFX = 门内即录);inputIndex = 本帧写入
//               的输入槽位(NVOF/FFX 忽略)。
//   postCopy:   拷贝/转换 CL 上的输入准备(RecordConvertInput 等)。
using OfPostExecuteFn = std::function<void(ID3D12GraphicsCommandList *, int inputIndex)>;
using OfPostCopyFn = std::function<void(ID3D12GraphicsCommandList *, int inputIndex)>;

struct OfStageResult {
    uint64_t waitFenceValue = 0; // 非 0 = 本帧真运动已产出(densify 待录/已录)
    bool historyReset = false;   // 本帧播种:对 NGX 置 PARAM_RESET + 清零发布
    int inputIndex = -1;         // 本帧写入的输入槽位(dump 用)
    // NVOF 专属:execute 已提交、densify 录制延迟到 FlushPendingDensify。
    // 调用方在首个 motion 消费者 CL 提交前(或帧失败路径的守卫析构里)必须
    // 恰好冲刷一次;gateOut = 随之移出的会话门锁(冲刷全程由调用方持有,
    // 提交互斥与轮转簿记的串行化不变)。false = 无待冲刷(FFX 恒 false,
    // 其 densify 在门内即录即提交)。
    bool pendingDensify = false;
};

class IOpticalFlowBackend {
public:
    IOpticalFlowBackend() = default;
    virtual ~IOpticalFlowBackend() = default;
    IOpticalFlowBackend(const IOpticalFlowBackend &) = delete;
    IOpticalFlowBackend &operator=(const IOpticalFlowBackend &) = delete;

    // 创建/重建会话。调用方必须持 PoolHold(槽池封死,门必然空闲)。
    virtual bool Initialize(D3D12Context &d3d12, int width, int height,
                            int quality, char *err, size_t errLen) noexcept = 0;
    virtual void Finalize() noexcept = 0;
    virtual bool Enabled() const noexcept = 0;
    virtual int Quality() const noexcept = 0;
    virtual int Width() const noexcept = 0;
    virtual int Height() const noexcept = 0;
    // 历史失效(seek = 新时间线):下一帧重新播种(清零发布,不产出)。
    virtual void ResetHistory() noexcept = 0;
    // 在途拷贝排空(early-return 路径释放槽位前;防宿主复用 upload 撕裂)。
    virtual void WaitCopyIdle() noexcept = 0;
    virtual double LastStageMs() const noexcept = 0;
    // gateOut:NVOF 在 execute 提交成功且 densify 延迟时把会话门锁移出
    // (调用方持锁直到 FlushPendingDensify;锁持续持有 = 提交互斥/轮转簿记
    // 与旧"门内全程"形态等价,只是 CPU 等引擎的位置挪到了首个 motion 消费
    // 者提交前,与 eval/FG 录制重叠)。FFX 恒不动此锁(其内部 OfFrameGate
    // 已在 StageFrame 内释放)。其它路径(播种/迟到/失败)NVOF 在门内解锁。
    virtual OfStageResult StageFrame(int frameIndex, ID3D12Resource *srcTex,
                                     const OfPostExecuteFn &postExecute,
                                     const OfPostCopyFn &postCopy,
                                     bool inputWrittenByPostCopy,
                                     std::unique_lock<std::mutex> &gateOut) noexcept = 0;
    // 冲刷延迟的 densify(NVOF 专属;FFX 恒 no-op)。前置条件:pendingDensify
    // = true 且调用方仍持有 StageFrame 移出的门锁。CPU 等引擎输出栅栏在此
    // 进行(有界 10s),醒来后 AcquireCl + 录制 + 提交 + signal。幂等:无
    // 待冲刷即刻返回。失败(等待超时/提交失败)仅退役/留痕 + 清除待冲刷,
    // 不改变本帧已定的 realMotion 语义(运动场陈旧一帧,可接受)。
    virtual void FlushPendingDensify(const OfPostExecuteFn &postExecute) noexcept {
        (void)postExecute;
    }
    // 最近一次冲刷的引擎输出等待 ms(逐帧账目用;FFX 无引擎,恒 0)。
    virtual double LastExeWaitMs() const noexcept { return 0.0; }
    // 光流阶段全跨度 ms(2026-09-25 语义修正:用户裁定"光流处理用时"=
    // 真实光流处理时间,不是提交胶水):门入口 → 冲刷完成(densify 落位),
    // 含提交 + 引擎计算 + 暴露等待 —— 与其他段的重叠如实计入本段(各记
    // 各的)。FFX = StageFrame 跨度(其 GPU 计算在主队列,无独立冲刷点)。
    // 供 nvof 段上报(逐帧:调用方在冲刷后/打包时读取)。
    virtual double LastStageTotalMs() const noexcept { return 0.0; }
    // 诊断 dump 探针:本帧写入的输入纹理(index 0/1)。
    virtual ID3D12Resource *InputTexture(int index) const noexcept = 0;
    // StatsPayload.ofMode 能力串(backend 特有段;off/zero 前缀由 DlssnrContext 统一)。
    virtual const char *ModeString(char *buf, size_t len) noexcept = 0;
    // NVOF 专属能力的受控下转型(门细分等待/引擎等待统计仅 NvofContext 有):
    // 消费端不再散布 Kind()==Nvof + static_cast(原四处);FFX 恒 nullptr。
    virtual class NvofContext *AsNvof() noexcept { return nullptr; }
    // 后端种类(会话重建裁决/FFX 专属 dump 分支用 —— 免 RTTI/dynamic_cast)。
    virtual int Kind() const noexcept = 0;

protected:
    // 连败停用闩锁单点(2026-10-04 收拢):链断类失败(拷贝提交/execute/
    // FFX densify —— 历史链断或会话能力存疑)统一计数,≥3 停用留痕,重试
    // 经 Rebind/换档。此前两后端四处失败路径两种口径(FFX densify 失败
    // 恰漏在自己注释宣称"已统一"的网外;NVOF 拷贝提交失败不计)。NVOF 的
    // 冲刷 densify 失败链完好,不计(语义见其注释)。
    void LatchFailure(uint32_t &consecutive, std::atomic<bool> &ready,
                      const char *tag) noexcept {
        if (++consecutive >= 3) {
            ready.store(false, std::memory_order_release);
            char msg[96];
            std::snprintf(msg, sizeof(msg),
                          "DLSSNR STATUS: %s disabled after consecutive failures", tag);
            TimingStatusLine(msg);
        }
    }
};

} // namespace vsdlssnr
