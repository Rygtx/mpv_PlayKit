#pragma once
// mpv.conf 的 input-ipc-server 解析(2026-10-04 自 panel_mpv_ipc.cpp 抽出为
// 两侧共用编译单元):此前面板会解析、插件只试默认名 —— 自定义管道名部署
// 下面板 reseek 正常而插件 resize 自动跟随静默失效,两份实现分叉的实害。
// 缓存/节流策略各侧自理(面板 1s 失败戳,插件进程级一次);本单元只做
// 无状态解析,漂移面收敛到这一份。
#include <windows.h>

namespace vsdlssnr {

// confPath = mpv.conf 完整路径。命中 = 返回 _wcsdup 的管道名(调用方
// free);未启用/读不到/值非法 = nullptr。free(nullptr) 恒安全。
wchar_t *MpvParseIpcServerName(const wchar_t *confPath) noexcept;

} // namespace vsdlssnr
