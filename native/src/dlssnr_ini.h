#pragma once
// Single implementation of the dlssnr_ui.ini persistence, shared by the
// plugin (bridge) and the panel — one key list, one authority. Two
// hand-maintained copies had already drifted (the plugin's SaveIni omitted
// scaling_enabled). Callers resolve the ini path themselves (panel and
// plugin derive it from different modules).

#include "dlssnr_params.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <windows.h>

// ---------------------------------------------------------------------------
// 保存键单一权威表(编译期):写侧(WriteDlssnrIni)与剪除表
// (kDlssnrSectionKeys / kRtxSectionKeys)由同一组宏展开 —— 键名/值式漂移
// 在构造上不可能(此前靠"同文件紧邻 + review 可见")。改键名/增删键只动
// DLSSNR_FIELDS / RTX_FIELDS 两组宏表;LoadDlssnrIni 读侧手动同步(漏加
// 无害:缺键保留当前值),但宏表漏加 = 启动剪除把刚写的键当垃圾删掉,
// 故新增键必经宏表。注意:宏体内禁用 // 注释(会吞掉续行反斜杠),行内
// 说明用块注释;含逗号的表达式须整体加括号。
// v20:fg_route 两档(0=自动/1=纯官方),保存即把存量 2/3 归一;
// v23:fg_hdr_interp 实验性(HDR 域插帧,可能闪烁)。
#define DLSSNR_STRW(name) L## #name

// 行格式:X(键名, 写函数, 值表达式)。
#define DLSSNR_FIELDS(X) \
    X(nr_enabled, writeInt, p.nrEnabled ? 1 : 0) \
    X(preset, writeInt, (std::clamp(p.preset, kPresetMin, kPresetMax))) \
    X(style, writeInt, p.style) \
    X(intensity_x100, writeX100, p.intensity) \
    X(local_tone_x100, writeX100, p.localToneStrength) \
    X(local_structure_x100, writeX100, p.localStructureStrength) \
    X(skin_structure_x100, writeX100, p.skinStructureStrength) \
    X(use_auto_mask, writeInt, p.useAutoMask ? 1 : 0) \
    X(input_resolution, writeInt, (std::clamp(p.inputResolutionPercent, kResPctMin, kResPctMax))) \
    X(scaling_enabled, writeInt, p.scalingEnabled ? 1 : 0) \
    X(residual_multiplier_x100, writeX100, p.residualMultiplier) \
    X(residual_saturation_x100, writeX100, p.residualSaturation) \
    X(residual_lightness_x100, writeX100, p.residualLightness) \
    X(shadow_structure_x100, writeX100, p.shadowStructureMultiplier) \
    X(reflection_glow_x100, writeX100, p.reflectionGlowMultiplier) \
    X(motion_vector_quality, writeInt, (std::clamp(p.motionVectorQuality, kOfQualityMin, kOfQualityMax))) \
    X(ffx_quality, writeInt, (std::clamp(p.ffxQuality, kFfxQualityMin, kFfxQualityMax))) \
    X(nvof_follow_scaling, writeInt, p.nvofFollowScaling ? 1 : 0) \
    X(fg_enabled, writeInt, p.fgEnabled ? 1 : 0) \
    X(fg_multiplier, writeInt, (std::clamp(p.fgMultiplier, kFgMultMin, kFgMultMax))) \
    X(fg_route, writeInt, (std::clamp(p.fgRoute, kFgRouteMin, kFgRouteMax))) \
    X(fg_hdr_interp, writeInt, p.fgHdrInterp ? 1 : 0) \
    X(of_backend, writeInt, (std::clamp(p.ofBackend, kOfBackendMin, kOfBackendMax))) \
    X(saved, writeInt, 1)

// RTX Video(VSR/TrueHDR):[rtxvideo] 独立节(v22 起面板全量接管;
// 面板"保存设置"与 bridge saveRequest 共用写侧)。
#define RTX_FIELDS(X) \
    X(vsr_mode, writeRtxInt, (std::clamp(p.rtxVsrMode, kVsrModeMin, kVsrModeMax))) \
    X(vsr_scale_x100, writeRtxInt, (static_cast<int>(std::lround(std::clamp(p.rtxVsrScale, kVsrScaleMin, kVsrScaleMax) * 100.0f)))) \
    X(vsr_strength, writeRtxInt, (std::clamp(p.rtxVsrStrength, kVsrStrengthMin, kVsrStrengthMax))) \
    X(hdr_enabled, writeRtxInt, p.rtxHdrEnabled ? 1 : 0) \
    X(hdr_contrast, writeRtxInt, (std::clamp(p.rtxHdrContrast, kHdrContrastMin, kHdrContrastMax))) \
    X(hdr_saturation, writeRtxInt, (std::clamp(p.rtxHdrSaturation, kHdrSaturationMin, kHdrSaturationMax))) \
    X(hdr_middle_gray, writeRtxInt, (std::clamp(p.rtxHdrMiddleGray, kHdrMiddleGrayMin, kHdrMiddleGrayMax))) \
    X(hdr_peak_nits, writeRtxInt, (std::clamp(p.rtxHdrMaxLuminance, kHdrMaxLumMin, kHdrMaxLumMax)))

namespace vsdlssnr {

// Persist the parameter profile (the [panel] log toggle is panel-local and
// stays with the panel). Returns false when any key write failed (file
// locked / permission) — callers log it; a silent false-save reads back as
// "settings reset themselves" on the next load.
inline bool WriteDlssnrIni(const DlssnrParams &p, const wchar_t *iniPath) noexcept {
    wchar_t buf[32];
    bool ok = true;
    auto writeInt = [&](const wchar_t *key, int v) {
        swprintf_s(buf, L"%d", v);
        ok = WritePrivateProfileStringW(L"dlssnr", key, buf, iniPath) && ok;
    };
    // std::lround rounds half away from zero; (int)(v*100+0.5) would eat negatives
    auto writeX100 = [&](const wchar_t *key, float v) {
        writeInt(key, static_cast<int>(std::lround(v * 100.0f)));
    };
    auto writeRtxInt = [&](const wchar_t *key, int v) {
        swprintf_s(buf, L"%d", v);
        ok = WritePrivateProfileStringW(L"rtxvideo", key, buf, iniPath) && ok;
    };
    // 键名与值式全部来自上方权威宏(写侧与剪除表同源,漂移在构造上不可能)。
#define DLSSNR_WRITE_ROW(name, fn, val) fn(DLSSNR_STRW(name), val);
    DLSSNR_FIELDS(DLSSNR_WRITE_ROW)
    RTX_FIELDS(DLSSNR_WRITE_ROW)
#undef DLSSNR_WRITE_ROW
    return ok;
}

// Load the saved profile into p (fields absent from the file keep their
// current value). Returns false when no profile has been saved yet.
// legacyFgRouteRaw (optional): when non-null, receives the raw on-disk
// fg_route value whenever it fell outside the v20 range (0-1) — i.e. a
// pre-v20 profile whose pin semantics were reinterpreted by the clamp.
// Callers surface it (panel status line) so the reinterpretation is not
// silent: the user's "1 = SM86 代理钉档" silently meaning "纯官方" after
// upgrade is exactly the "FG 怎么没了" support scenario.
inline bool LoadDlssnrIni(DlssnrParams &p, const wchar_t *iniPath,
                          int *legacyFgRouteRaw = nullptr) noexcept {
    wchar_t buf[64]{};
    if (!GetPrivateProfileStringW(L"dlssnr", L"saved", L"", buf, 64, iniPath) || !buf[0]) {
        return false; // no saved profile
    }
    if (legacyFgRouteRaw) *legacyFgRouteRaw = 0;
    const auto readInt = [&](const wchar_t *key, int def) -> int {
        return static_cast<int>(GetPrivateProfileIntW(L"dlssnr", key, def, iniPath));
    };
    // 布尔键宽容解析:机器写入恒为 0/1,但手编 "true"/"false" 曾经
    // (GetPrivateProfileInt 对非数字返回 0)静默关掉功能。接受常见拼写,
    // 其余落回整数语义。
    const auto readBool = [&](const wchar_t *key, bool def) -> bool {
        wchar_t raw[16]{};
        if (GetPrivateProfileStringW(L"dlssnr", key, L"", raw, 16, iniPath) && raw[0]) {
            const auto eqi = [&](const wchar_t *lit) {
                return _wcsicmp(raw, lit) == 0;
            };
            if (eqi(L"true") || eqi(L"yes") || eqi(L"on")) return true;
            if (eqi(L"false") || eqi(L"no") || eqi(L"off")) return false;
        }
        return readInt(key, def ? 1 : 0) != 0;
    };
    // *_x100 values are hand-editable on disk: clamp to the documented ranges
    // (the panel and vpy paths clamp; without this an edited ini would push
    // out-of-range floats into the NGX evaluate keys every frame).
    const auto readX100 = [&](const wchar_t *key, float def, float lo, float hi) -> float {
        return std::clamp(
            static_cast<float>(readInt(key, static_cast<int>(def * 100))) / 100.0f, lo, hi);
    };
    p.nrEnabled = readBool(L"nr_enabled", p.nrEnabled != 0);
    p.preset = std::clamp(readInt(L"preset", p.preset), kPresetMin, kPresetMax);
    p.style = std::clamp(readInt(L"style", p.style), kStyleMin, kStyleMax);
    p.intensity = readX100(L"intensity_x100", p.intensity, kStrengthMin, kStrengthMax);
    p.localToneStrength = readX100(L"local_tone_x100", p.localToneStrength, kStrengthMin, kStrengthMax);
    p.localStructureStrength = readX100(L"local_structure_x100", p.localStructureStrength, kStrengthMin, kStrengthMax);
    p.skinStructureStrength = readX100(L"skin_structure_x100", p.skinStructureStrength, kSkinMin, kSkinMax);
    p.useAutoMask = readBool(L"use_auto_mask", p.useAutoMask != 0);
    p.inputResolutionPercent = std::clamp(readInt(L"input_resolution", p.inputResolutionPercent), kResPctMin, kResPctMax);
    // 与其它布尔位同款 != 0 归一:GetPrivateProfileInt 对非数字("true")
    // 返回 0 会静默关掉缩放;>1 的值又会让 CreateParamsChanged 每次热复用
    // 误判为变更而多付一次 feature 重建。
    // >1 的数值仍走整数语义(0/1):手编 2 会让 CreateParamsChanged 每次热
    // 复用误判为变更而多付一次 feature 重建 —— readInt != 0 归一保留。
    p.scalingEnabled = readBool(L"scaling_enabled", p.scalingEnabled != 0);
    p.residualMultiplier = readX100(L"residual_multiplier_x100", p.residualMultiplier, kResidualMultMin, kResidualMultMax);
    p.residualSaturation = readX100(L"residual_saturation_x100", p.residualSaturation, kResidualFineMin, kResidualFineMax);
    p.residualLightness = readX100(L"residual_lightness_x100", p.residualLightness, kResidualFineMin, kResidualFineMax);
    p.shadowStructureMultiplier = readX100(L"shadow_structure_x100", p.shadowStructureMultiplier, kResidualFineMin, kResidualFineMax);
    p.reflectionGlowMultiplier = readX100(L"reflection_glow_x100", p.reflectionGlowMultiplier, kResidualFineMin, kResidualFineMax);
    p.motionVectorQuality = std::clamp(readInt(L"motion_vector_quality", p.motionVectorQuality), kOfQualityMin, kOfQualityMax);
    p.ffxQuality = std::clamp(readInt(L"ffx_quality", p.ffxQuality), kFfxQualityMin, kFfxQualityMax);
    p.nvofFollowScaling = readBool(L"nvof_follow_scaling", p.nvofFollowScaling != 0);
    p.fgEnabled = readBool(L"fg_enabled", p.fgEnabled != 0);
    p.fgMultiplier = std::clamp(readInt(L"fg_multiplier", p.fgMultiplier), kFgMultMin, kFgMultMax);
    // v20 起两档(0=自动/1=纯官方),不兼容旧值:存量 1-3 按 clamp 归 1,
    // 升级后在面板选回自动即可。越界原值经 legacyFgRouteRaw 上报调用方
    // (面板状态行提示),重解释不再无声。
    if (legacyFgRouteRaw) {
        *legacyFgRouteRaw = readInt(L"fg_route", p.fgRoute);
    }
    p.fgRoute = std::clamp(readInt(L"fg_route", p.fgRoute), kFgRouteMin, kFgRouteMax);
    p.fgHdrInterp = readBool(L"fg_hdr_interp", p.fgHdrInterp != 0);
    p.ofBackend = std::clamp(readInt(L"of_backend", p.ofBackend), kOfBackendMin, kOfBackendMax);
    // RTX Video(VSR/TrueHDR):[rtxvideo] 独立节(v22 起面板全量接管,
    // 与 WriteDlssnrIni 同键表)。旧部署样例的 vsr_height 键已作废(语义
    // 换成倍率 vsr_scale_x100),不读取 —— 旧值静默失效,面板重存即归位。
    const auto readRtxInt = [&](const wchar_t *key, int def) -> int {
        return static_cast<int>(GetPrivateProfileIntW(L"rtxvideo", key, def, iniPath));
    };
    const auto readRtxBool = [&](const wchar_t *key, bool def) -> bool {
        wchar_t raw[16]{};
        if (GetPrivateProfileStringW(L"rtxvideo", key, L"", raw, 16, iniPath) && raw[0]) {
            const auto eqi = [&](const wchar_t *lit) { return _wcsicmp(raw, lit) == 0; };
            if (eqi(L"true") || eqi(L"yes") || eqi(L"on")) return true;
            if (eqi(L"false") || eqi(L"no") || eqi(L"off")) return false;
        }
        return readRtxInt(key, def ? 1 : 0) != 0;
    };
    p.rtxVsrMode = std::clamp(readRtxInt(L"vsr_mode", p.rtxVsrMode), kVsrModeMin, kVsrModeMax);
    p.rtxVsrScale = std::clamp(
        static_cast<float>(readRtxInt(L"vsr_scale_x100", static_cast<int>(p.rtxVsrScale * 100.0f))) / 100.0f,
        kVsrScaleMin, kVsrScaleMax);
    p.rtxVsrStrength = std::clamp(readRtxInt(L"vsr_strength", p.rtxVsrStrength), kVsrStrengthMin, kVsrStrengthMax);
    p.rtxHdrEnabled = readRtxBool(L"hdr_enabled", p.rtxHdrEnabled != 0);
    p.rtxHdrContrast = std::clamp(readRtxInt(L"hdr_contrast", p.rtxHdrContrast), kHdrContrastMin, kHdrContrastMax);
    p.rtxHdrSaturation = std::clamp(readRtxInt(L"hdr_saturation", p.rtxHdrSaturation), kHdrSaturationMin, kHdrSaturationMax);
    p.rtxHdrMiddleGray = std::clamp(readRtxInt(L"hdr_middle_gray", p.rtxHdrMiddleGray), kHdrMiddleGrayMin, kHdrMiddleGrayMax);
    p.rtxHdrMaxLuminance = std::clamp(readRtxInt(L"hdr_peak_nits", p.rtxHdrMaxLuminance), kHdrMaxLumMin, kHdrMaxLumMax);
    return true;
}

// 保存文件的自检键表:由上方权威宏直接生成 —— 写侧加键时本表自动跟上,
// 构造上不可能漂移。读取侧本就不认未知键;文件里的表外键(例:rtxvideo
// 的作废 vsr_height,语义已并入 vsr_scale_x100)由下方 PruneDlssnrIni 剪除。
#define DLSSNR_NAME_ROW(name, fn, val) DLSSNR_STRW(name),
inline constexpr const wchar_t *kDlssnrSectionKeys[] = {
    DLSSNR_FIELDS(DLSSNR_NAME_ROW)
};
inline constexpr const wchar_t *kRtxSectionKeys[] = {
    RTX_FIELDS(DLSSNR_NAME_ROW)
};
#undef DLSSNR_NAME_ROW
#undef DLSSNR_FIELDS
#undef RTX_FIELDS
#undef DLSSNR_STRW

// 启动自检:剪除 [dlssnr]/[rtxvideo] 两节里的表外键(跨版本回滚残留、
// 手编历史键)。读取侧本就不认未知键,留着只会在回滚/手编时制造困惑;
// 剪除只触表外键,不碰任何已知键与 [panel] 节(面板自有)。返回剪除键数;
// prunedOut(可空)以 "节:键;" 追加剪除明细。
inline int PruneDlssnrIni(const wchar_t *iniPath,
                          wchar_t *prunedOut = nullptr, size_t prunedLen = 0) noexcept {
    struct Section {
        const wchar_t *name;
        const wchar_t *const *keys;
        size_t count;
    };
    constexpr Section sections[] = {
        {L"dlssnr", kDlssnrSectionKeys,
         sizeof(kDlssnrSectionKeys) / sizeof(kDlssnrSectionKeys[0])},
        {L"rtxvideo", kRtxSectionKeys,
         sizeof(kRtxSectionKeys) / sizeof(kRtxSectionKeys[0])},
    };
    const auto eqi = [](const wchar_t *a, const wchar_t *b) {
        return _wcsicmp(a, b) == 0; // INI 键名不区分大小写
    };
    const auto appendLog = [&](const wchar_t *section, const wchar_t *key) {
        if (!prunedOut || prunedLen == 0) return;
        const size_t used = wcslen(prunedOut);
        _snwprintf_s(prunedOut + used, prunedLen - used, _TRUNCATE,
                     L"%ls:%ls;", section, key);
    };
    int pruned = 0;
    for (const Section &sec : sections) {
        size_t cap = 2048;
        for (;;) {
            const auto buf = std::unique_ptr<wchar_t[]>(new (std::nothrow) wchar_t[cap]());
            if (!buf) return pruned;
            // 键名枚举:key 传 nullptr 返回该节全部键名(双 \0 结尾)。
            const DWORD got = GetPrivateProfileStringW(
                sec.name, nullptr, L"", buf.get(), static_cast<DWORD>(cap), iniPath);
            if (got >= cap - 2) {
                cap *= 2; // 截断:翻倍重枚举(1MB 上限防失控)
                if (cap > (1u << 20)) return pruned;
                continue;
            }
            for (const wchar_t *k = buf.get(); *k; k += wcslen(k) + 1) {
                bool known = false;
                for (size_t i = 0; i < sec.count; ++i) {
                    if (eqi(k, sec.keys[i])) { known = true; break; }
                }
                if (!known) {
                    // 删键:key 赋 NULL。
                    WritePrivateProfileStringW(sec.name, k, nullptr, iniPath);
                    ++pruned;
                    appendLog(sec.name, k);
                }
            }
            break;
        }
    }
    return pruned;
}

} // namespace vsdlssnr
