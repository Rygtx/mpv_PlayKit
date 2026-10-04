#pragma once
// mpv 宿主探测与 resize 跟随(2026-10-04 自 plugin.cpp 拆出):本 DLL 活在
// mpv 进程内,窗口/管道探测与 VS 滤镜生命周期零耦合 —— plugin.cpp 只在
// create/free 时各碰一个句柄。
#include <windows.h>

namespace vsdlssnr {

// 本进程 mpv vo 窗口的视频显示矩形(letterbox min-fit;探测失败 = {0,0} =
// 调用方按 VSR 旁路处理,窗口出现后的链重建重新探到并启用)。
struct MpvDisplayPick {
    int width = 0;
    int height = 0;
};
MpvDisplayPick MpvDetectTargetSize(int srcW, int srcH) noexcept;

// 启动窗口 resize 跟随 watcher(mode=1):400ms 探测节拍 + 高度迟滞
// (max(8px, 2%)),连续两拍稳定偏离后经 mpv IPC seek 触发链重建,随即
// 自退。返回 stop 句柄(调用方持有,Free 时 MpvResizeWatchStop;句柄的
// Close 归 watcher 线程退出时自办,Stop 只 SetEvent);
// vsrAutoMode=false / 名额满(≥2)/线程创建失败 = nullptr(自动跟随关闭,
// 优雅降级为需手动 seek)。
HANDLE MpvResizeWatchStart(int srcW, int srcH, int refH, bool vsrAutoMode) noexcept;
// 停止 watcher(nullptr 恒安全);线程退出与句柄回收异步自办。
void MpvResizeWatchStop(HANDLE stop) noexcept;

} // namespace vsdlssnr
