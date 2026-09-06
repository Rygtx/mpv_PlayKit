#pragma once
// User-facing parameters, mapped 1:1 onto Magpie's DLSSNR_AI_Filter.hlsl surface
// (see F:\Project\Magpie\src\Effects\DLSSNR\DLSSNR_AI_Filter.hlsl).

// Documented parameter ranges — the single authority. Every std::clamp on
// these params (vpy args, dlssnr_ui.ini values, panel payload, internal
// size) must reference these constants; a bare literal is a drift bug.
inline constexpr int kPresetMin = 0, kPresetMax = 3;
inline constexpr int kStyleMin = 0, kStyleMax = 2;
// intensity / local tone / local structure: 上游 r2-fix1 从 0-2 收紧回 0-1
// (v0.6.5 DLSSNR_AI_Filter.hlsl / DLSSNRFilter.cpp ClampFinite)
inline constexpr float kStrengthMin = 0.0f, kStrengthMax = 1.0f;
inline constexpr float kSkinMin = -1.0f, kSkinMax = 2.0f;        // skin structure (-1 = auto)
inline constexpr int kResPctMin = 25, kResPctMax = 100;          // internal resolution percent
inline constexpr float kResidualMultMin = 1.0f, kResidualMultMax = 2.0f;
// 残差精调 4 项(r1-r10 新增):saturation / lightness / shadow structure / reflection glow
inline constexpr float kResidualFineMin = 0.0f, kResidualFineMax = 2.0f;

struct DlssnrParams {
    // NGX "DLSSNR.Hint.Render.Preset" 0-3
    // 上游 v0.5.7 P8 引入、r2-fix1 取消暴露(fixed-0);移植端保留为扩展功能。
    int preset = 0;
    // NGX "DLSSNR.Style" 0-2
    int style = 0;
    // NGX "DLSSNR.Intensity" 0-1
    float intensity = 1.0f;
    // NGX "DLSSNR.LocalToneStrength" 0-1
    float localToneStrength = 1.0f;
    // NGX "DLSSNR.LocalStructureStrength" 0-1
    float localStructureStrength = 1.0f;
    // NGX "DLSSNR.SkinStructureStrength" -1..2 (-1 = auto)
    float skinStructureStrength = -1.0f;
    // NGX "DLSSNR.UseAutoMask" 0/1
    int useAutoMask = 1;
    // NGX "DLSSNR.UICorrection" 0/1
    int uiCorrection = 1;
    // Internal processing resolution in percent of source (25-100, create-time;
    // changing it rebuilds the feature + scaling resources). Ignored when
    // scalingEnabled == 0.
    int inputResolutionPercent = 100;
    // Master switch for internal-resolution scaling (0 = always process at 100%)
    int scalingEnabled = 1;
    // Magpie-side residual composite weight (1-2, per-frame via compute cbuffer)
    float residualMultiplier = 1.0f;
    // 残差精调(r1-r10):全部是相对语义 —— 对 "DLSSNR 相对原图造成的变化" 做倍率
    // (1=保持,2=放大,0=移除),不是绝对调色滑块
    float residualSaturation = 1.0f;
    float residualLightness = 1.0f;
    float shadowStructureMultiplier = 1.0f;
    float reflectionGlowMultiplier = 1.0f;
    // Absolute path to nvngx_dlssnr.dll; empty = derive from plugin location
    const char *ngxDllPath = nullptr;
};
