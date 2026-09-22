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
#include <windows.h>

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
    writeInt(L"nr_enabled", p.nrEnabled ? 1 : 0);
    writeInt(L"preset", p.preset);
    writeInt(L"style", p.style);
    writeX100(L"intensity_x100", p.intensity);
    writeX100(L"local_tone_x100", p.localToneStrength);
    writeX100(L"local_structure_x100", p.localStructureStrength);
    writeX100(L"skin_structure_x100", p.skinStructureStrength);
    writeInt(L"use_auto_mask", p.useAutoMask ? 1 : 0);
    writeInt(L"input_resolution", std::clamp(p.inputResolutionPercent, kResPctMin, kResPctMax));
    writeInt(L"scaling_enabled", p.scalingEnabled ? 1 : 0);
    writeX100(L"residual_multiplier_x100", p.residualMultiplier);
    writeX100(L"residual_saturation_x100", p.residualSaturation);
    writeX100(L"residual_lightness_x100", p.residualLightness);
    writeX100(L"shadow_structure_x100", p.shadowStructureMultiplier);
    writeX100(L"reflection_glow_x100", p.reflectionGlowMultiplier);
    writeInt(L"motion_vector_quality", std::clamp(p.motionVectorQuality, kOfQualityMin, kOfQualityMax));
    writeInt(L"ffx_quality", std::clamp(p.ffxQuality, kFfxQualityMin, kFfxQualityMax));
    writeInt(L"nvof_follow_scaling", p.nvofFollowScaling ? 1 : 0);
    writeInt(L"fg_enabled", p.fgEnabled ? 1 : 0);
    writeInt(L"fg_multiplier", std::clamp(p.fgMultiplier, kFgMultMin, kFgMultMax));
    writeInt(L"fg_route", std::clamp(p.fgRoute, kFgRouteMin, kFgRouteMax)); // v20 两档:保存即把存量 2/3 归一
    writeInt(L"of_backend", std::clamp(p.ofBackend, kOfBackendMin, kOfBackendMax));
    writeInt(L"saved", 1);
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
    p.ofBackend = std::clamp(readInt(L"of_backend", p.ofBackend), kOfBackendMin, kOfBackendMax);
    return true;
}

} // namespace vsdlssnr
