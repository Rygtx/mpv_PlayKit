#pragma once
// 光流帧序门(纯时序决策,无 D3D12 依赖)。从 nvof_context.cpp StageFrame
// 的内联门提取,FxofContext 复用;NvofContext 亦已迁移至本门(FFX 转正
// 默认后端后"内联门=唯一在役链路"的理由失效,双份门逻辑不再各自维护)。
//
// 语义(_nextSeq = 上一完成帧 + 1):
//   - 迟到帧(乱序/回退,序号已被越过):播种,不推进门;
//   - 缺口恰为 1 且前驱"在途"(已 MarkIncoming 但还没走到 Arrive,即堵在
//     门 mutex 外):纯事件驱动等待 —— cv 释放锁让前驱插队,前驱 Advance
//     即醒,无定时成分,两帧都保住 guidance。前驱不在途 = 上游真丢帧:
//     立即播种,零等待(旧版盲等 15ms 的序列化延迟即 2026-09 自持卡顿
//     教训的来源;且前驱门内工作 >15ms 时旧版救援失败,新版不限时必救回);
//   - 更大缺口立即播种(长等待会叠加进序列化延迟链 → 宿主丢帧 → 更多
//     缺口,自持卡顿,nvof 实测 2026-09 教训);
//   - 设备丢失/失败帧:历史作废,下一帧重新播种(播种帧成功则历史重建);
//   - ResetTimeline(seek):_nextSeq 归 -1(下一帧自定起点)+ 历史作废。
//
// 活性不变量:凡被等待方等待的帧(缺口=1 时前驱),其 StageFrame 的所有
// 路径必达 Advance(Expired 例外 —— 等待期间前驱不可能 Expired,见下)。
// 这要求消费端设备丢失分支也 Advance(历史已作废,推进不改变下帧播种)。
// 等待期间前驱不可能 Expired:等待的前提是 _nextSeq == 前驱帧号,而 Expired
// 要求帧号 < _nextSeq;_nextSeq 只被 Advance/ResetTimeline 改动,前者出自
// 前驱自身,后者 notify 后等待方走 Seed 分支。100ms wait_for 上限仅是保险
// 丝(防未来改动破坏不变量变永久悬等),逻辑正常时由 notify 先行唤醒。

#include <atomic>
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

    // StageFrame 入口(取门锁之前)调用:声明本帧在途。位只存活于"声明 →
    // Arrive"之间(Arrive 首行即自清),故位=1 ⟺ 该帧已进入消费端但还堵
    // 在门 mutex 外 —— 这是"前驱在途(等它插队)"与"真丢帧(立即播种)"
    // 的唯一判据。原子 RMW:声明方不持门锁,与 Arrive 内的自清并发。
    void MarkIncoming(int64_t frameIndex) noexcept {
        _incoming.fetch_or(Mask(frameIndex), std::memory_order_acq_rel);
    }

    // 到达帧决策(须已持 unique_lock(Mutex);cv 等待期间放锁让前驱插队)。
    OfGateDecision Arrive(int64_t frameIndex,
                          std::unique_lock<std::mutex> &lock) noexcept {
        _incoming.fetch_and(~Mask(frameIndex), std::memory_order_acq_rel);
        if (_nextSeq < 0) _nextSeq = frameIndex; // 首帧自定起点
        if (frameIndex < _nextSeq) return OfGateDecision::Expired;
        if (frameIndex > _nextSeq) {
            if (frameIndex == _nextSeq + 1 &&
                (_incoming.load(std::memory_order_acquire) & Mask(frameIndex - 1))) {
                // 前驱已入门径(堵在 mutex 外):它必会推进,纯事件等待。
                _cv.wait_for(lock, std::chrono::milliseconds(100),
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
    // 门期望序号(诊断观测用;Arrive/Advance 持锁内调用,无并发读)。
    int64_t NextSeq() const noexcept { return _nextSeq; }
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
    // ponytail: 64 位环无代数,相距 64 的两帧共享一位 —— 需前者卡在
    // MarkIncoming 与门锁之间且后者已置位才误判"在飞",后果只是 100ms
    // 熔断多等一拍;流水深度远小于 64,实不可达。加代数计数是升级路径。
    static uint64_t Mask(int64_t i) noexcept {
        return uint64_t(1) << (uint64_t(i) & 63);
    }
    std::condition_variable _cv;
    std::atomic<uint64_t> _incoming{0};
    int64_t _nextSeq = -1;
    bool _historyValid = false;
};

} // namespace vsdlssnr
