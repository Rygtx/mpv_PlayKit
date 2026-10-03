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

// ---- 候选管道名与连接/发送(2026-10-05 收拢:面板 reseek 与插件 resize
// watcher 此前各持一份候选列表 + 打开循环 + 有界发送,第三条 IPC 轴 ——
// 改兜底名/改超时只落一侧即静默分叉)。全部无状态。
inline constexpr int kMpvPipeMaxCandidates = 4; // [0]=conf 解析名,1-3 = 默认兜底

// 默认兜底候选名(静态字面量,无所有权;[0] 槽位留给调用方填 conf 解析值)。
void MpvPipeDefaultNames(const wchar_t *candidates[kMpvPipeMaxCandidates]) noexcept;

// 候选逐个 CreateFileW 连接,命中即返回(全败 nullptr)。overlapped = 命令
// 形态(FILE_FLAG_OVERLAPPED,配合 MpvPipeSendBounded);nameOut 可空(命中
// 管道名,日志用)。跳过空槽位(null/空串)。
HANDLE MpvPipeOpen(const wchar_t *const *candidates, bool overlapped,
                   wchar_t *nameOut, size_t nameLen) noexcept;

// 已连接管道写一条命令 + 排空回执(overlapped + 500ms 有界;mpv 挂起不拖
// 死调用线程,2026-09-25 教训)。ok = 命令全量写入;回执只排空不解析(两侧
// 同语义)。调用方负责 CloseHandle。
bool MpvPipeSendBounded(HANDLE pipe, const char *cmd, size_t len) noexcept;

} // namespace vsdlssnr
