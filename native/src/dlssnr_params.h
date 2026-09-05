#pragma once
// User-facing parameters, mapped 1:1 onto Magpie's DLSSNR_AI_Filter.hlsl surface
// (see F:\Project\Magpie\src\Effects\DLSSNR\DLSSNR_AI_Filter.hlsl).

struct DlssnrParams {
    // NGX "DLSSNR.Hint.Render.Preset" 0-3
    int preset = 0;
    // NGX "DLSSNR.Style" 0-2
    int style = 0;
    // NGX "DLSSNR.Intensity" 0-2
    float intensity = 1.0f;
    // NGX "DLSSNR.LocalToneStrength"
    float localToneStrength = 1.0f;
    // NGX "DLSSNR.LocalStructureStrength" 0-2
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
    // Absolute path to nvngx_dlssnr.dll; empty = derive from plugin location
    const char *ngxDllPath = nullptr;
};
