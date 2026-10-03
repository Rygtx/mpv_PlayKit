// NVAPI GPU 架构探测实现(契约与来源见 nv_gpu_probe.h;原住 dlssfg_gate
// 内,2026-10-03 随接口拆出)。

#include "nv_gpu_probe.h"

#include <windows.h>

namespace vsdlssnr {
namespace nv_gpu_probe {
namespace {

// ---- NVAPI x64 接口 ID(驱动 ABI 事实标准,十余年稳定;RTX40MFG-Unlock
// adapter_discovery.h 同表核验,其实现含 owned-code 校验,此处按"nvapi64.dll
// 为系统组件"从简,仅查导出存在)----
constexpr uint32_t kNvapiIdInitialize = 0x0150e828;
constexpr uint32_t kNvapiIdEnumPhysicalGPUs = 0xe5ac921f;
// 0xd8265d24:同 ID 换过签名 —— R590 SDK 前是 GetArchitecture(handle,
// uint32*),R590 起(实测 617.14 驱动)旧标量形态恒返 -9,由 GetArchInfo
// versioned struct 接管(官方 nvapi64.lib nvlib_gen.obj 反汇编实证同 ID)。
constexpr uint32_t kNvapiIdGetArchInfo = 0xd8265d24;

// NV_GPU_ARCH_INFO_V2(官方 nvapi.h;version = sizeof|2<<16 是 NVAPI
// versioned struct 惯例)。更旧的驱动(<R590)同 ID 还是标量实现,会把
// arch 直接写进首字段(version 槽,只写 4 字节不越界)—— 两形态统一按
// "非零 architecture 槽,兜底 version 槽"取值。
struct ArchInfoV2 {
    uint32_t version;
    uint32_t architecture;
    uint32_t implementation;
    uint32_t revision;
};

} // namespace

bool GetGpuArchs(uint32_t *archs, uint32_t *impls, size_t cap, size_t *archCount) noexcept {
    *archCount = 0;
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    if (!nvapi) return false;
    using QueryFn = void *__cdecl(uint32_t);
    auto query = reinterpret_cast<QueryFn *>(
        reinterpret_cast<void *>(GetProcAddress(nvapi, "nvapi_QueryInterface")));
    using InitFn = int __cdecl();
    using EnumFn = int __cdecl(void **, uint32_t *);
    using ArchFn = int __cdecl(void *, ArchInfoV2 *);
    auto init = reinterpret_cast<InitFn *>(
        query ? reinterpret_cast<void *>(query(kNvapiIdInitialize)) : nullptr);
    auto enumGpus = reinterpret_cast<EnumFn *>(
        query ? reinterpret_cast<void *>(query(kNvapiIdEnumPhysicalGPUs)) : nullptr);
    auto getArch = reinterpret_cast<ArchFn *>(
        query ? reinterpret_cast<void *>(query(kNvapiIdGetArchInfo)) : nullptr);
    if (!init || !enumGpus || !getArch || init() != 0) {
        FreeLibrary(nvapi); // 未成功初始化,无内部线程,可安全卸载
        return false;
    }
    void *gpus[64]{};
    uint32_t count = 0;
    if (enumGpus(gpus, &count) != 0 || !count) return false;
    bool any = false;
    for (uint32_t i = 0; i < count && i < 64; ++i) {
        if (!gpus[i]) continue;
        ArchInfoV2 info{};
        info.version = uint32_t(sizeof(ArchInfoV2) | (2u << 16));
        if (getArch(gpus[i], &info) != 0) continue;
        const uint32_t arch = info.architecture ? info.architecture : info.version;
        if (!arch) continue;
        if (*archCount < cap) {
            archs[*archCount] = arch;
            if (impls) impls[*archCount] = info.implementation;
        }
        ++*archCount;
        any = true;
    }
    return any;
}

GpuArchProbe ProbePrimaryGpuArch() noexcept {
    uint32_t arch = 0, impl = 0;
    size_t count = 0;
    if (GetGpuArchs(&arch, &impl, 1, &count) && count >= 1)
        return {arch, impl};
    return {0, 0};
}

} // namespace nv_gpu_probe
} // namespace vsdlssnr
