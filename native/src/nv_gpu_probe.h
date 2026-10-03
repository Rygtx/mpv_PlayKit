#pragma once
// NVAPI GPU 架构探测(FG 门与 NR 模型选档共用的中立件;原住 dlssfg_gate,
// NR 选档路径反向依赖"FG 门"文件 = 层级倒挂,2026-10-03 拆出 —— 双方
// 平级 include 本件)。
// NV_GPU_ARCHITECTURE_ID(官方 nvapi.h 枚举;0x170 另为 RTX40MFG-Unlock
// 实证值)。Blackwell(GB200=0x1B0)原生 MFG,不碰。
#include <cstddef>
#include <cstdint>

namespace vsdlssnr {
namespace nv_gpu_probe {

constexpr uint32_t kArchTuring = 0x160;    // TU100
constexpr uint32_t kArchAmpere = 0x170;    // GA100
constexpr uint32_t kArchAda = 0x190;       // AD100 = mfg gate 解锁唯一目标
constexpr uint32_t kArchBlackwell = 0x1B0; // GB200

// NV_GPU_ARCH_INFO_V2 读取结果。implementation 是族内芯片编号(官方
// NV_GPU_ARCH_IMPLEMENTATION_ID:GA102=0x2 实测、AD102=0x2 文档值),
// 跨族撞号 —— 分档必须 arch+impl 联合判断。
struct GpuArchProbe {
    uint32_t arch;
    uint32_t implementation;
};

// 枚举物理 GPU 架构值(官方 NVAPI NV_GPU_ARCHITECTURE_ID)。返回是否有
// 任一块 GPU 查到架构;false(含 NVAPI 缺失/初始化失败/全部失败)时
// *archCount 可能为 0。成功初始化后 nvapi64.dll 进程常驻(内部工作线程
// 存活期不明,FreeLibrary 死锁风险;与 nvofapi64.dll 同哲学,进程退出
// OS 回收);未成功初始化即卸载。
bool GetGpuArchs(uint32_t *archs, uint32_t *impls, size_t cap, size_t *archCount) noexcept;

// NVAPI 枚举首块 NVIDIA 物理卡的架构与 implementation;探测失败
// (NVAPI 缺失/初始化失败/全部查询失败)时 arch == 0。
GpuArchProbe ProbePrimaryGpuArch() noexcept;

} // namespace nv_gpu_probe
} // namespace vsdlssnr
