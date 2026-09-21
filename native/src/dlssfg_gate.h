#pragma once
// 官方链 FG 运行库适配层(40 系 6x 解锁 + 代理族判定)。
//
// 背景:官方 nvngx_dlssg.dll(310.x)的 MFG(多帧生成)模型本身与 GPU 架构
// 无关(50 系同款 DLL 原生 2-6X),但在 Ada(RTX 40)上被两层软件策略钳到
// 1 生成帧(2x):
//   1. 运行库内部 count gate —— 一条可定位的字节序列(test dl,dl / je 拒绝 /
//      mov esi,5 上限常量),拒绝一切多帧请求;
//   2. 驱动 per-game profile 的 MFG override(仅 Streamline 游戏路径,mpv
//      直驱 NGX 不经过)。
// 因此 mpv 场景只需处理第 1 层:进程内补 2 字节跳转放行 count 1..5。
// 手法与字节判定源自 RTX40MFG-Unlock(MIT License)ngx_mfg_gate.h,精简移植:
// 仅进程内存映像、不落盘;唯一匹配 + 目标指令双锚校验,未命中/校验失败一律
// 原样返回 —— 解锁失败只意味着回落 2x,不存在更坏结果。
//
// 代理族判定:dlssg_for_sm86 0.3.x 代理(SM86 后端)只适用于 Turing/Ampere
// (NVAPI 架构 TUxxx=0x160 / GAxxx=0x170,后者为 RTX40MFG-Unlock 实证值)。
// Ada 及更新架构在 40 系上预载该代理会以 SM86 SASS 内核接管官方链 = 未定义
// 行为,自动档据此分流:代理族预载代理,其余走官方链 + gate 解锁。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace vsdlssnr {
namespace dlssfg_gate {

// 当前机器是否应预载 dlssg_for_sm86 代理(存在 Turing/Ampere 物理 GPU)。
// NVAPI 枚举不可用/失败时 fail-open 返回 true(维持无条件预载的现状,30 系
// 用户无损)。多卡边界:代理 hook 为进程级,双 NV 混插(如 30+40)本就超出
// 0.3.x 部署形态,按"存在代理族卡"处理,不再按 LUID 细分。
bool GpuFamilyPrefersProxy() noexcept;

// 尝试对 nvngx_dlssg 运行库进程内解锁 MFG count gate。currentMax = 解锁前
// 能力查询值;返回解锁后应采用的上限(未命中/已锁死/校验失败 = currentMax,
// 日志入 timing log)。仅当 provider 模块已加载时有效 —— 调用方在能力查询
// 之后调用,核心此时必然已加载运行库。
unsigned UnlockMfgCountGate(HMODULE provider, unsigned currentMax) noexcept;

} // namespace dlssfg_gate
} // namespace vsdlssnr
