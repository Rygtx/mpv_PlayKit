// 官方链 FG 运行库适配层实现(契约与来源见 dlssfg_gate.h;GPU 架构探测
// 已拆至 nv_gpu_probe.cpp,本件只余 FG 专属:代理族判定 + mfg gate 补丁)。

#include "dlssfg_gate.h"
#include "dlssnr_context.h" // TimingStatusLine(结果必须进 timing log)

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace vsdlssnr {
namespace dlssfg_gate {
namespace {

using nv_gpu_probe::GetGpuArchs;
using nv_gpu_probe::kArchAda;
using nv_gpu_probe::kArchAmpere;
using nv_gpu_probe::kArchTuring;

// ---- Ada count gate 字节判定(RTX40MFG-Unlock ngx_mfg_gate.h 同源)----
// test dl,dl ; je <reject> ; mov esi,<cap>  —— cap = 运行库多帧上限常量
constexpr std::array<uint8_t, 13> kGatePattern{
    0x84, 0xd2, 0x0f, 0x84, 0x03, 0x01, 0x00, 0x00, 0xbe, 0x05, 0x00, 0x00, 0x00};
constexpr size_t kGateCapOffset = 9; // mov esi, imm32 的立即数字节
// 未补丁形态 = je rel32(6 字节);已补丁形态 = jmp short +4(2 字节,跳过
// 原 rel32 余量落回原 je 落点 —— 补丁后原 4 字节位移留在原地不动)。
constexpr std::array<uint8_t, 2> kGateJe{0x0f, 0x84};
constexpr std::array<uint8_t, 2> kGateJmp{0xeb, 0x04};

uint8_t *FindGateSite(HMODULE module) noexcept {
    if (!module) return nullptr;
    const auto *base = reinterpret_cast<const uint8_t *>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        dos->e_lfanew > 0x100000)
        return nullptr;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.NumberOfSections > 96)
        return nullptr;
    const size_t imageBytes = nt->OptionalHeader.SizeOfImage;
    auto valid = [imageBytes](size_t rva, size_t size) {
        return rva < imageBytes && size <= imageBytes - rva;
    };
    // je rel32 的落点必须是 cmp r8d,1(count==1 检查)—— 双锚:字节序列
    // 命中 + 语义锚存在,才算 site(RTX40MFG 同校验)。
    constexpr std::array<uint8_t, 4> kCountCmp{0x41, 0x83, 0xf8, 0x01};
    uint8_t *found = nullptr;
    size_t matches = 0;
    const auto *section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
            section->VirtualAddress >= imageBytes)
            continue;
        const size_t size = (std::min)(imageBytes - section->VirtualAddress,
            size_t((std::max)(section->Misc.VirtualSize, section->SizeOfRawData)));
        const auto *begin = base + section->VirtualAddress;
        for (size_t off = 0; off + kGatePattern.size() <= size; ++off) {
            if (memcmp(begin + off, kGatePattern.data(), 2) != 0)
                continue;
            if (memcmp(begin + off + 2, kGateJe.data(), 2) != 0 &&
                memcmp(begin + off + 2, kGateJmp.data(), 2) != 0)
                continue;
            if (memcmp(begin + off + 4, kGatePattern.data() + 4,
                    kGatePattern.size() - 4) != 0)
                continue;
            if (++matches > 1)
                return nullptr; // 歧义即放弃,绝不选中首个匹配
            const auto *p = begin + off;
            int32_t rel = 0;
            std::memcpy(&rel, p + 4, sizeof(rel));
            const size_t targetRva = size_t(p + 2 - base) + 6 + rel;
            if (!valid(targetRva, kCountCmp.size()) ||
                memcmp(base + targetRva, kCountCmp.data(), kCountCmp.size()) != 0)
                return nullptr;
            found = const_cast<uint8_t *>(p);
        }
    }
    return matches == 1 ? found : nullptr;
}

bool PageOfModuleIsExecuteRead(HMODULE provider, uintptr_t address, size_t size) noexcept {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || VirtualQuery(reinterpret_cast<void *>(address), &mbi, sizeof(mbi)) !=
                        sizeof(mbi))
        return false;
    const DWORD prot = mbi.Protect & 0xff;
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE))
        return false;
    // 页必须属于 provider 本体(拒绝改到别处映像)。
    HMODULE owner = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
               reinterpret_cast<LPCWSTR>(address), &owner) &&
           owner == provider;
}

// GPU 架构枚举(GetGpuArchs/ArchInfoV2/NVAPI 接口 ID)已随 nv_gpu_probe
// 拆出;本文件经 using 引入(FG 门是它的消费者,不再是提供者)。

} // namespace

bool GpuFamilyPrefersProxy() noexcept {
    uint32_t archs[64]{};
    size_t count = 0;
    if (!GetGpuArchs(archs, nullptr, 64, &count)) {
        // fail-open:维持无条件预载的现状(30 系无损)。留痕:40/50 系 +
        // NVAPI 异常时会给 Ada 预载 SM86 代理(未定义行为场景),排查
        // "为什么 40 系也走了预载"必须有迹可循。
        TimingStatusLine(
            "DLSSNR STATUS: dlssfg GPU family probe failed; proxy preload fail-open");
        return true;
    }
    // 成功也留痕(走对更要留痕 —— 2026-10-02 probe 恒失败事故的排查成本
    // 就卡在成功路径零日志,分不清"真 Ampere"还是"fail-open 撞对了")。
    char list[80]{}; // 列前 4 块:" 0x1B0," ≤8 字节/块
    for (size_t i = 0; i < count && i < 4; ++i) {
        char one[16];
        std::snprintf(one, sizeof(one), "%s0x%X", i ? " " : "", archs[i]);
        std::strncat(list, one, sizeof(list) - strlen(list) - 1);
    }
    const bool prefersProxy = [&] {
        for (size_t i = 0; i < count; ++i)
            if (archs[i] == kArchTuring || archs[i] == kArchAmpere) return true;
        return false;
    }();
    char msg[160];
    std::snprintf(msg, sizeof(msg),
                  "DLSSNR STATUS: dlssfg gpu family probe: %zu gpu(s) arch=[%s] -> %s",
                  count, list, prefersProxy ? "proxy preload" : "official chain");
    TimingStatusLine(msg);
    return prefersProxy;
}

unsigned UnlockMfgCountGate(HMODULE provider, unsigned currentMax) noexcept {
    // 仅 Ada:50 系原生 MFG,不碰官方运行库;非 Ada/探测失败保守跳过。
    uint32_t archs[64]{};
    size_t archCount = 0;
    bool ada = false;
    const bool probed = GetGpuArchs(archs, nullptr, 64, &archCount);
    if (probed) {
        for (size_t i = 0; i < archCount; ++i)
            if (archs[i] == kArchAda) { ada = true; break; }
    }
    if (!ada) {
        // "不是 Ada"与"探测失败"分文案:混同会让"40 系为何没解锁"无从查起。
        TimingStatusLine(probed
            ? "DLSSNR STATUS: dlssfg mfg gate unlock skipped (not Ada; "
              "Blackwell ships native MFG)"
            : "DLSSNR STATUS: dlssfg mfg gate unlock skipped (GPU arch probe "
              "failed; conservative skip)");
        return currentMax;
    }
    uint8_t *site = FindGateSite(provider);
    if (!site) {
        TimingStatusLine(
            "DLSSNR STATUS: dlssfg mfg gate pattern not found (2x cap stays; "
            "driver runtime unsupported by unlock)");
        return currentMax;
    }
    uint8_t *branch = site + 2;
    const unsigned cap = kGatePattern[kGateCapOffset];
    if (memcmp(branch, kGateJmp.data(), kGateJmp.size()) == 0) {
        // 已是放行形态(本进程先前或外部解锁过):直接采信上限。
        char msg[96];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: dlssfg mfg gate already unlocked (cap %u)", cap);
        TimingStatusLine(msg);
        return cap;
    }
    if (!PageOfModuleIsExecuteRead(provider, reinterpret_cast<uintptr_t>(branch), kGateJmp.size())) {
        TimingStatusLine("DLSSNR STATUS: dlssfg mfg gate page protection unexpected (2x cap stays)");
        return currentMax;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(branch, kGateJmp.size(), PAGE_EXECUTE_WRITECOPY, &oldProtect)) {
        TimingStatusLine("DLSSNR STATUS: dlssfg mfg gate VirtualProtect failed (2x cap stays)");
        return currentMax;
    }
    // 写入前最后一读:仍为未补丁形态才动笔(拒绝外来改动,RTX40MFG 同语义)。
    unsigned result = currentMax;
    if (memcmp(branch, kGateJe.data(), kGateJe.size()) == 0) {
        std::memcpy(branch, kGateJmp.data(), kGateJmp.size());
        result = cap;
    }
    DWORD restored = 0;
    VirtualProtect(branch, kGateJmp.size(), oldProtect, &restored);
    FlushInstructionCache(GetCurrentProcess(), branch, kGateJmp.size());
    char msg[96];
    if (result != currentMax) {
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: dlssfg mfg gate unlocked (maxGen %u -> %u)",
                      currentMax, result);
    } else {
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: dlssfg mfg gate foreign edit refused (2x cap stays)");
    }
    TimingStatusLine(msg);
    return result;
}

} // namespace dlssfg_gate
} // namespace vsdlssnr
