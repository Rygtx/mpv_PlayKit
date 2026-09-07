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
    writeInt(L"preset", p.preset);
    writeInt(L"style", p.style);
    writeX100(L"intensity_x100", p.intensity);
    writeX100(L"local_tone_x100", p.localToneStrength);
    writeX100(L"local_structure_x100", p.localStructureStrength);
    writeX100(L"skin_structure_x100", p.skinStructureStrength);
    writeInt(L"use_auto_mask", p.useAutoMask ? 1 : 0);
    writeInt(L"ui_correction", p.uiCorrection ? 1 : 0);
    writeInt(L"input_resolution", std::clamp(p.inputResolutionPercent, kResPctMin, kResPctMax));
    writeInt(L"scaling_enabled", p.scalingEnabled ? 1 : 0);
    writeX100(L"residual_multiplier_x100", p.residualMultiplier);
    writeX100(L"residual_saturation_x100", p.residualSaturation);
    writeX100(L"residual_lightness_x100", p.residualLightness);
    writeX100(L"shadow_structure_x100", p.shadowStructureMultiplier);
    writeX100(L"reflection_glow_x100", p.reflectionGlowMultiplier);
    writeInt(L"motion_vector_quality", std::clamp(p.motionVectorQuality, kOfQualityMin, kOfQualityMax));
    writeInt(L"nvof_follow_scaling", p.nvofFollowScaling ? 1 : 0);
    writeInt(L"saved", 1);
    return ok;
}

// Load the saved profile into p (fields absent from the file keep their
// current value). Returns false when no profile has been saved yet.
inline bool LoadDlssnrIni(DlssnrParams &p, const wchar_t *iniPath) noexcept {
    wchar_t buf[64]{};
    if (!GetPrivateProfileStringW(L"dlssnr", L"saved", L"", buf, 64, iniPath) || !buf[0]) {
        return false; // no saved profile
    }
    const auto readInt = [&](const wchar_t *key, int def) -> int {
        return static_cast<int>(GetPrivateProfileIntW(L"dlssnr", key, def, iniPath));
    };
    // *_x100 values are hand-editable on disk: clamp to the documented ranges
    // (the panel and vpy paths clamp; without this an edited ini would push
    // out-of-range floats into the NGX evaluate keys every frame).
    const auto readX100 = [&](const wchar_t *key, float def, float lo, float hi) -> float {
        return std::clamp(
            static_cast<float>(readInt(key, static_cast<int>(def * 100))) / 100.0f, lo, hi);
    };
    p.preset = std::clamp(readInt(L"preset", p.preset), kPresetMin, kPresetMax);
    p.style = std::clamp(readInt(L"style", p.style), kStyleMin, kStyleMax);
    p.intensity = readX100(L"intensity_x100", p.intensity, kStrengthMin, kStrengthMax);
    p.localToneStrength = readX100(L"local_tone_x100", p.localToneStrength, kStrengthMin, kStrengthMax);
    p.localStructureStrength = readX100(L"local_structure_x100", p.localStructureStrength, kStrengthMin, kStrengthMax);
    p.skinStructureStrength = readX100(L"skin_structure_x100", p.skinStructureStrength, kSkinMin, kSkinMax);
    p.useAutoMask = readInt(L"use_auto_mask", p.useAutoMask ? 1 : 0) != 0;
    p.uiCorrection = readInt(L"ui_correction", p.uiCorrection ? 1 : 0) != 0;
    p.inputResolutionPercent = std::clamp(readInt(L"input_resolution", p.inputResolutionPercent), kResPctMin, kResPctMax);
    // 与其它布尔位同款 != 0 归一:GetPrivateProfileInt 对非数字("true")
    // 返回 0 会静默关掉缩放;>1 的值又会让 CreateParamsChanged 每次热复用
    // 误判为变更而多付一次 feature 重建。
    p.scalingEnabled = readInt(L"scaling_enabled", p.scalingEnabled) != 0;
    p.residualMultiplier = readX100(L"residual_multiplier_x100", p.residualMultiplier, kResidualMultMin, kResidualMultMax);
    p.residualSaturation = readX100(L"residual_saturation_x100", p.residualSaturation, kResidualFineMin, kResidualFineMax);
    p.residualLightness = readX100(L"residual_lightness_x100", p.residualLightness, kResidualFineMin, kResidualFineMax);
    p.shadowStructureMultiplier = readX100(L"shadow_structure_x100", p.shadowStructureMultiplier, kResidualFineMin, kResidualFineMax);
    p.reflectionGlowMultiplier = readX100(L"reflection_glow_x100", p.reflectionGlowMultiplier, kResidualFineMin, kResidualFineMax);
    p.motionVectorQuality = std::clamp(readInt(L"motion_vector_quality", p.motionVectorQuality), kOfQualityMin, kOfQualityMax);
    p.nvofFollowScaling = readInt(L"nvof_follow_scaling", p.nvofFollowScaling) != 0;
    return true;
}

} // namespace vsdlssnr
