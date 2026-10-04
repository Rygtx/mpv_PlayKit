// OfFrameGate 自检:三条决策路径 + 在途判据 + 事件驱动等待 + 代际防复活
// (单文件,无框架)。
// 编译运行(MSVC x64 shell):cl /nologo /EHsc /std:c++17 /I ../src test_of_frame_gate.cpp && test_of_frame_gate.exe
#include "../src/of_frame_gate.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace vsdlssnr;
using Clock = std::chrono::steady_clock;
static double MsSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int main() {
    { // 连续链:全 Proceed
        OfFrameGate g;
        std::unique_lock<std::mutex> lk(g.Mutex);
        assert(g.Arrive(0, g.Epoch(), lk) == OfGateDecision::Proceed);
        g.Advance(0);
        assert(g.Arrive(1, g.Epoch(), lk) == OfGateDecision::Proceed);
        g.Advance(1);
    }
    { // 迟到帧:Expired 且不推进门(后续帧不受影响)
        OfFrameGate g;
        std::unique_lock<std::mutex> lk(g.Mutex);
        assert(g.Arrive(5, g.Epoch(), lk) == OfGateDecision::Proceed);
        g.Advance(5); // _nextSeq = 6
        assert(g.Arrive(4, g.Epoch(), lk) == OfGateDecision::Expired);
        assert(g.Arrive(6, g.Epoch(), lk) == OfGateDecision::Proceed);
    }
    { // 缺口=1 且前驱不在途:立即播种,零等待(保险丝 100ms 未触发)
        OfFrameGate g;
        std::unique_lock<std::mutex> lk(g.Mutex);
        assert(g.Arrive(0, g.Epoch(), lk) == OfGateDecision::Proceed);
        g.Advance(0); // _nextSeq = 1,帧 1 被上游丢弃永不到达
        g.MarkIncoming(2); // 帧 2 照常入径(与消费端同款)
        auto t0 = Clock::now();
        assert(g.Arrive(2, g.Epoch(), lk) == OfGateDecision::Seed);
        assert(MsSince(t0) < 15.0);
    }
    { // 缺口=1 且前驱在途但慢(门内工作 20ms,超旧版 15ms 上限):
        // 事件等待不限时,救援成功,双帧 Proceed(旧版 15ms 到点即播种,救不回)
        OfFrameGate g;
        std::thread prev([&] {
            g.MarkIncoming(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::unique_lock<std::mutex> lk(g.Mutex);
            assert(g.Arrive(1, g.Epoch(), lk) == OfGateDecision::Proceed);
            std::this_thread::sleep_for(std::chrono::milliseconds(20)); // 门内工作
            g.Advance(1);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(2)); // 前驱已声明在途
        std::unique_lock<std::mutex> lk(g.Mutex);
        assert(g.Arrive(0, g.Epoch(), lk) == OfGateDecision::Proceed);
        g.Advance(0); // _nextSeq = 1
        auto t0 = Clock::now();
        assert(g.Arrive(2, g.Epoch(), lk) == OfGateDecision::Proceed); // 阻塞至前驱 Advance
        assert(MsSince(t0) >= 30.0); // 等满前驱全程
        g.Advance(2);
        prev.join();
    }
    { // seek 复位:下一帧自定起点
        OfFrameGate g;
        std::unique_lock<std::mutex> lk(g.Mutex);
        assert(g.Arrive(9, g.Epoch(), lk) == OfGateDecision::Proceed);
        g.Advance(9);
        g.ResetTimeline();
        assert(g.Arrive(100, g.Epoch(), lk) == OfGateDecision::Proceed);
    }
    { // 代际:ResetTimeline 与在途旧帧并发 —— 幽灵帧不复活 _nextSeq
        // (2026-10-02 "next=200 爬 188" 的顺序等价复现:旧帧已 MarkIncoming
        // /已在途,seek 落地后它才 Arrive)。
        OfFrameGate g;
        std::unique_lock<std::mutex> lk(g.Mutex);
        assert(g.Arrive(199, g.Epoch(), lk) == OfGateDecision::Proceed);
        g.Advance(199); // _nextSeq = 200
        const uint64_t oldEpoch = g.Epoch(); // 旧帧在 MarkIncoming 前采样
        g.MarkIncoming(200);
        g.ResetTimeline(); // seek:-1 + 代际递增(调用方持锁,与生产端同款)
        // 旧时间线幽灵帧:判 Expired(播种、不推进),_nextSeq 保持 -1。
        assert(g.Arrive(200, oldEpoch, lk) == OfGateDecision::Expired);
        assert(g.NextSeq() == -1);
        // 新时间线首帧照常自定起点(Proceed;播种与否由调用方按
        // HistoryValid 合成 —— 与 seek 复位用例同语义)。
        assert(g.Arrive(0, g.Epoch(), lk) == OfGateDecision::Proceed);
        g.Advance(0);
        assert(g.NextSeq() == 1);
    }
    { // 代际:旧 epoch 帧在 seek 后到达,即时判幽灵帧(不熔断不推进)
        OfFrameGate g;
        std::unique_lock<std::mutex> lk(g.Mutex);
        assert(g.Arrive(0, g.Epoch(), lk) == OfGateDecision::Proceed);
        g.Advance(0); // _nextSeq = 1
        const uint64_t oldEpoch = g.Epoch();
        g.MarkIncoming(2); // 帧 2 已入径(消费端同款)
        g.ResetTimeline(); // seek 落地(帧 2 在途窗口内)
        auto t0 = Clock::now();
        assert(g.Arrive(2, oldEpoch, lk) == OfGateDecision::Expired);
        assert(MsSince(t0) < 15.0); // 即时判定,未走 100ms 熔断
        assert(g.NextSeq() == -1);
    }
    { // 代际:cv 等待中的帧被 seek 唤醒后判幽灵帧(谓词含代际,不空耗熔断)
        OfFrameGate g;
        std::unique_lock<std::mutex> lk(g.Mutex);
        assert(g.Arrive(0, g.Epoch(), lk) == OfGateDecision::Proceed);
        g.Advance(0); // _nextSeq = 1;帧 2 在途等帧 1(帧 1 永不到达)
        const uint64_t oldEpoch = g.Epoch();
        g.MarkIncoming(2);
        g.MarkIncoming(1); // 帧 1 声明在途 → 帧 2 进入 cv 事件等待
        std::thread seeker([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            std::unique_lock<std::mutex> lk2(g.Mutex);
            g.ResetTimeline(); // 帧 2 等待中 seek:notify + 代际翻转
        });
        auto t0 = Clock::now();
        assert(g.Arrive(2, oldEpoch, lk) == OfGateDecision::Expired);
        assert(MsSince(t0) < 100.0); // 代际唤醒,未触 100ms 熔断
        assert(g.NextSeq() == -1);
        seeker.join();
    }
    std::puts("test_of_frame_gate: all assertions passed");
    return 0;
}
