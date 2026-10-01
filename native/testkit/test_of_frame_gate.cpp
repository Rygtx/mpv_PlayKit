// OfFrameGate 自检:三条决策路径 + 在途判据 + 事件驱动等待(单文件,无框架)。
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
        assert(g.Arrive(0, lk) == OfGateDecision::Proceed);
        g.Advance(0);
        assert(g.Arrive(1, lk) == OfGateDecision::Proceed);
        g.Advance(1);
    }
    { // 迟到帧:Expired 且不推进门(后续帧不受影响)
        OfFrameGate g;
        std::unique_lock<std::mutex> lk(g.Mutex);
        assert(g.Arrive(5, lk) == OfGateDecision::Proceed);
        g.Advance(5); // _nextSeq = 6
        assert(g.Arrive(4, lk) == OfGateDecision::Expired);
        assert(g.Arrive(6, lk) == OfGateDecision::Proceed);
    }
    { // 缺口=1 且前驱不在途:立即播种,零等待(保险丝 100ms 未触发)
        OfFrameGate g;
        std::unique_lock<std::mutex> lk(g.Mutex);
        assert(g.Arrive(0, lk) == OfGateDecision::Proceed);
        g.Advance(0); // _nextSeq = 1,帧 1 被上游丢弃永不到达
        g.MarkIncoming(2); // 帧 2 照常入径(与消费端同款)
        auto t0 = Clock::now();
        assert(g.Arrive(2, lk) == OfGateDecision::Seed);
        assert(MsSince(t0) < 15.0);
    }
    { // 缺口=1 且前驱在途但慢(门内工作 20ms,超旧版 15ms 上限):
        // 事件等待不限时,救援成功,双帧 Proceed(旧版 15ms 到点即播种,救不回)
        OfFrameGate g;
        std::thread prev([&] {
            g.MarkIncoming(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::unique_lock<std::mutex> lk(g.Mutex);
            assert(g.Arrive(1, lk) == OfGateDecision::Proceed);
            std::this_thread::sleep_for(std::chrono::milliseconds(20)); // 门内工作
            g.Advance(1);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(2)); // 前驱已声明在途
        std::unique_lock<std::mutex> lk(g.Mutex);
        assert(g.Arrive(0, lk) == OfGateDecision::Proceed);
        g.Advance(0); // _nextSeq = 1
        auto t0 = Clock::now();
        assert(g.Arrive(2, lk) == OfGateDecision::Proceed); // 阻塞至前驱 Advance
        assert(MsSince(t0) >= 30.0); // 等满前驱全程
        g.Advance(2);
        prev.join();
    }
    { // seek 复位:下一帧自定起点
        OfFrameGate g;
        std::unique_lock<std::mutex> lk(g.Mutex);
        assert(g.Arrive(9, lk) == OfGateDecision::Proceed);
        g.Advance(9);
        g.ResetTimeline();
        assert(g.Arrive(100, lk) == OfGateDecision::Proceed);
    }
    std::puts("test_of_frame_gate: all assertions passed");
    return 0;
}
