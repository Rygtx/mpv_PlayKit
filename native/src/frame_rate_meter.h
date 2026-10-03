#pragma once
// 帧率环形计数器(纯 QPC 数学,与 D3D12 零关系;原住 D3D12Context,
// 2026-10-03 迁出 —— God Object 里唯一与设备无关的成员)。
#include <mutex>

namespace vsdlssnr {

// 最近 4 秒的帧数 / 4 即帧率(计数式)。播放节奏由宿主决定:卡顿时宿主积压,
// 恢复后突发+并发拉帧(fmParallel 入口 Δt 可到亚毫秒),倒数式 EMA 会把
// 追赶吞吐当帧率冲高;计数对并发与突发免疫,停顿(窗口内无新帧)读数
// 自然回落。窗宽 4s(原 1s):窗沿相位对齐让固定帧率源读数跳 ±1
// (25fps 实测 24/25/26),拉宽窗压量化误差,显示取整才稳。
class FrameRateMeter {
public:
    void Tick(double qpcSeconds) noexcept {
        std::lock_guard<std::mutex> lock(_mutex);
        _ring[_head] = qpcSeconds;
        _head = (_head + 1) % kCap;
        if (_count < kCap) ++_count;
    }
    double Rate() noexcept {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_count == 0) return 0.0;
        // now 取最后写入的 tick:读路径不需要 QPC。停顿期间无人发布统计,面板
        // 本来就冻结;恢复后第一帧的发布会以新 now 淘汰窗口外的旧条目。
        const double now = _ring[(_head + kCap - 1) % kCap];
        const double since = now - 4.0;
        int count = 0;
        for (int i = 0; i < _count; ++i) {
            if (_ring[i] >= since) ++count;
        }
        return static_cast<double>(count) / 4.0; // 固定 4s 窗:窗口内帧数 / 4 = fps
    }

private:
    static constexpr int kCap = 4096; // 4s 窗的容量上限(超出按 1024fps 封顶)
    std::mutex _mutex;
    double _ring[kCap] = {};
    int _head = 0;  // 下一写入位
    int _count = 0; // 有效条目数(绕环前等于已写个数)
};

} // namespace vsdlssnr
