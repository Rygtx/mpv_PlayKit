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
    // Magpie-side residual composite weight, clamped [1,2]; v1 keeps the value
    // for future internal-resolution-scaling support (no effect at 100%).
    float residualMultiplier = 1.0f;
    // Absolute path to nvngx_dlssnr.dll; empty = derive from plugin location
    const char *ngxDllPath = nullptr;
};
