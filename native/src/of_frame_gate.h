#pragma once
// 光流帧序门(纯时序决策,无 D3D12 依赖)。从 nvof_context.cpp StageFrame
// 的内联门提取,FxofContext 复用;NvofContext 本期保留内联门不动(NVOF 是
// 唯一在役链路,原位重构风险/收益不成比例)。
//
// 语义与 NvofContext 门逐条一致(_nextSeq = 上一完成帧 + 1):
//   - 迟到帧(乱序/回退,序号已被越过):播种,不推进门;
//   - 缺口恰为 1:cv 让路 15ms(前驱大概率卡在门 mutex 上/fmParallel 相邻
//     竞争,让它插队完成,两帧都保住 guidance);真丢帧或更大缺口立即播种
//     (长等待会叠加进序列化延迟链 → 宿主丢帧 → 更多缺口,自持卡顿,
//     nvof 实测 2026-09 教训);
//   - 设备丢失/失败帧:历史作废,下一帧重新播种(播种帧成功则历史重建);
//   - ResetTimeline(seek):_nextSeq 归 -1(下一帧自定起点)+ 历史作废。

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace vsdlssnr {

enum class OfGateDecision {
    Proceed,  // 序号紧邻,可产出真运动
    Expired,  // 迟到帧:播种清零,不推进门
    Seed,     // 首帧/缺口未等到前驱:播种清零,结尾统一推进
};

class OfFrameGate {
public:
    // 门互斥体:调用方持 unique_lock 做门内工作(拷贝提交/execute/CPU 等),
    // 语义与 NvofContext 的 _gateMutex 完全一致 —— 门内工作按帧序串行。
    std::mutex Mutex;

    // 到达帧决策(须已持 unique_lock(Mutex);cv 等待期间放锁让前驱插队)。
    OfGateDecision Arrive(int64_t frameIndex,
                          std::unique_lock<std::mutex> &lock) noexcept {
        if (_nextSeq < 0) _nextSeq = frameIndex; // 首帧自定起点
        if (frameIndex < _nextSeq) return OfGateDecision::Expired;
        if (frameIndex > _nextSeq) {
            if (frameIndex == _nextSeq + 1) {
                _cv.wait_for(lock, std::chrono::milliseconds(15),
                             [&] { return _nextSeq >= frameIndex; });
            }
            if (_nextSeq > frameIndex) return OfGateDecision::Expired;
            if (_nextSeq < frameIndex) return OfGateDecision::Seed;
        }
        return OfGateDecision::Proceed;
    }

    // 帧完成推进(成功/失败/播种帧都推进;历史状态由调用方另行标记)。
    void Advance(int64_t frameIndex) noexcept {
        _nextSeq = frameIndex + 1;
        _cv.notify_all();
    }

    bool HistoryValid() const noexcept { return _historyValid; }
    // 播种帧拷贝成功:历史链已建立,下一帧可产出真运动。
    void MarkSeeded() noexcept { _historyValid = true; }
    // 失败帧:历史作废但门继续推进(下一帧重新播种)。
    void InvalidateHistory() noexcept { _historyValid = false; }
    // seek = 新时间线:_nextSeq 归 -1(与 NvofContext::ResetHistory 同款),
    // 旧时间线帧号不再误判迟到。
    void ResetTimeline() noexcept {
        _historyValid = false;
        _nextSeq = -1;
        _cv.notify_all();
    }

private:
    std::condition_variable _cv;
    int64_t _nextSeq = -1;
    bool _historyValid = false;
};

} // namespace vsdlssnr
