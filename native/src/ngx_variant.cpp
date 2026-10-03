// NGX NR 模型选档实现(契约见 ngx_variant.h;2026-10-04 自 plugin.cpp 拆出)。
#include "ngx_variant.h"
#include "nv_gpu_probe.h"
#include "status_line.h" // TimingStatusLine(前置声明收拢件)

#include <windows.h>

#include <cstdio>
#include <algorithm>

namespace vsdlssnr {

namespace {

// 社区改版模型按 GPU 系列选装(后缀即选档依据,不可改名);缺失回落原版。
constexpr char NGX_VARIANT_DLL_4090[] = "nvngx_dlssnr.4090.dll"; // RTX 4090 ONLY(AD102)
constexpr char NGX_VARIANT_DLL_40XX[] = "nvngx_dlssnr.40xx.dll"; // RTX 4080/70/60 ONLY(其余 Ada)
constexpr char NGX_VARIANT_DLL_2030[] = "nvngx_dlssnr.2030.dll"; // RTX 20,30 PLAIN FP16
constexpr uint32_t kImplAd102 = 0x2; // NVAPI implementation 是族内芯片编号(AD102=0x2,官方 NV_GPU_ARCH_IMPLEMENTATION_ID;与 GA102=0x2 跨族撞号,分档必须 arch+impl 联合判断)

// 默认模型路径选档:按 GPU 系列在 ngx\ 下挑社区改版,原版无条件兜底
// (探测失败/50 系/未选装 = 纯原版,行为与现状一致)。选档是 (arch, impl,
// 文件存在性) 的纯函数,会话内确定 —— 热重绑定按 ngxDllPath 比较不受影响。
// 变体与原版同目录,NGX core 的 app dir 取 dll.parent_path() 不受选择影响。
//
// 首拍可信度自检:枚举进程模块找"外来 version.dll"(FG 代理宿主名;
// 系统正品在 System32 下,代理在 mpv 根目录/插件目录)。
bool ForeignVersionDllResident() noexcept {
    using EnumFx = BOOL(WINAPI *)(HANDLE, HMODULE *, DWORD, LPDWORD);
    using NameFx = DWORD(WINAPI *)(HANDLE, HMODULE, LPWSTR, DWORD);
    const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return false;
    const auto enumMods = reinterpret_cast<EnumFx>(reinterpret_cast<void *>(
        GetProcAddress(k32, "K32EnumProcessModules")));
    const auto modName = reinterpret_cast<NameFx>(reinterpret_cast<void *>(
        GetProcAddress(k32, "K32GetModuleFileNameExW")));
    if (!enumMods || !modName) return false;
    HMODULE mods[512];
    DWORD needed = 0;
    if (!enumMods(GetCurrentProcess(), mods, sizeof(mods), &needed)) return false;
    const int n = static_cast<int>(
        (std::min<DWORD>)(needed / sizeof(HMODULE), 512));
    wchar_t sysDir[MAX_PATH];
    const UINT sysLen = GetSystemDirectoryW(sysDir, MAX_PATH);
    for (int i = 0; i < n; ++i) {
        wchar_t path[MAX_PATH]{};
        if (!modName(GetCurrentProcess(), mods[i], path, MAX_PATH)) continue;
        const std::wstring base = std::filesystem::path(path).filename().wstring();
        if (lstrcmpiW(base.c_str(), L"version.dll") != 0) continue;
        if (sysLen && sysLen < MAX_PATH &&
            CompareStringOrdinal(path, static_cast<int>(sysLen),
                                 sysDir, static_cast<int>(sysLen), TRUE) == CSTR_EQUAL &&
            path[sysLen] == L'\\') {
            continue; // System32 正品
        }
        return true;
    }
    return false;
}

} // namespace (internal helpers)

// 探针必须"首拍定格":FG 代理(version.dll,dlssg_for_sm86)为给官方
// DLSSG 放行,会把进程内 NVAPI GetArchInfo 钩成伪装 Blackwell(0x1B0)
// —— 2026-10-03 实锤:3080 会话开 FG 后下一次 create 探到 0x1B0 →
// tier=stock → 原版 snippet 在伪装态下 Feature 18 恒 0xbad00001,NR 整体
// 失效。首次 create 必然先于代理附着(代理随 FG 会话初始化加载),缓存
// 首拍真值后恒用之;后续探测与首拍的偏差只留一行痕(可诊断"为何没换档")。
// 首拍自身的可信度自检(2026-10-04):若代理被 PE 加载器提前拉入并先挂了
// NVAPI(非设计形态),首拍锁进 0x1B0 且 drift 行永不触发(后续探针恒等
// 首拍)= 零诊断 NR 失效。识别:dlssg_for_sm86 只部署在 Turing/Ampere,
// 探到 Blackwell 却有外来 version.dll 常驻 = 自相矛盾 —— 按 20/30 系救援
// 选档并大声留痕,而非静默 tier=stock。
std::wstring SelectNgxDllVariant(const std::filesystem::path &ngxDir) {
    static const vsdlssnr::nv_gpu_probe::GpuArchProbe firstProbe = [] {
        auto p = vsdlssnr::nv_gpu_probe::ProbePrimaryGpuArch();
        if (p.arch == vsdlssnr::nv_gpu_probe::kArchBlackwell &&
            ForeignVersionDllResident()) {
            char msg[200];
            std::snprintf(msg, sizeof(msg),
                          "DLSSNR STATUS: ngx first-shot probe = Blackwell (0x%X) with "
                          "foreign version.dll resident — NVAPI spoof suspected (dlssg "
                          "proxy only targets Turing/Ampere); forcing 20/30 tier",
                          p.arch);
            vsdlssnr::TimingStatusLine(msg);
            p.arch = vsdlssnr::nv_gpu_probe::kArchAmpere; // → 20/30 档
            p.implementation = 0;
        }
        return p;
    }();
    auto probe = vsdlssnr::nv_gpu_probe::ProbePrimaryGpuArch();
    if (probe.arch != firstProbe.arch || probe.implementation != firstProbe.implementation) {
        static bool driftLogged = false; // 一次性:此后每拍都是同一个假值
        if (!driftLogged) {
            driftLogged = true;
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "DLSSNR STATUS: ngx variant probe drifted (0x%X/0x%X -> 0x%X/0x%X, "
                          "FG proxy NVAPI spoof?); keeping first-read tier",
                          firstProbe.arch, firstProbe.implementation,
                          probe.arch, probe.implementation);
            vsdlssnr::TimingStatusLine(msg);
        }
        probe = firstProbe;
    }
    const char *tier = "stock";
    const char *candidates[2] = {NGX_SNIPPET_DLL_NAME, NGX_SNIPPET_DLL_NAME};
    if (probe.arch == vsdlssnr::nv_gpu_probe::kArchAda) {
        if (probe.implementation == kImplAd102) {
            tier = "4090";
            candidates[0] = NGX_VARIANT_DLL_4090;
        } else {
            tier = "40xx";
            candidates[0] = NGX_VARIANT_DLL_40XX;
        }
    } else if (probe.arch == vsdlssnr::nv_gpu_probe::kArchTuring ||
               probe.arch == vsdlssnr::nv_gpu_probe::kArchAmpere) {
        tier = "20/30";
        candidates[0] = NGX_VARIANT_DLL_2030;
    }
    std::filesystem::path chosen;
    for (const char *name : candidates) {
        std::error_code ec;
        if (const auto p = ngxDir / name; std::filesystem::exists(p, ec)) {
            chosen = p;
            break;
        }
    }
    if (chosen.empty()) chosen = ngxDir / NGX_SNIPPET_DLL_NAME; // 全缺也走原版:下游 LoadLibrary 失败 → 既有 passthrough
    const auto chosenUtf8 = chosen.u8string();
    char msg[512];
    std::snprintf(msg, sizeof(msg),
                  "DLSSNR STATUS: ngx model tier=%s arch=0x%X impl=0x%X -> %s", tier,
                  probe.arch, probe.implementation,
                  reinterpret_cast<const char *>(chosenUtf8.c_str()));
    vsdlssnr::TimingStatusLine(msg);
    return chosen.wstring();
}

} // namespace vsdlssnr
