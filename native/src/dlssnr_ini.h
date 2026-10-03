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
// 保存键单一权威表(编译期):写侧(WriteDlssnrIni)、读侧(LoadDlssnrIni)
// 与剪除表(kDlssnrSectionKeys / kRtxSectionKeys)由同一组宏展开 —— 键名/
// 值式漂移在构造上不可能(此前写侧漏 scaling_enabled、读侧手动同步各出过
// 一次事故形态)。改键名/增删键只动 DLSSNR_FIELDS / RTX_FIELDS 两组宏表。
// 注意:宏体内禁用 // 注释(会吞掉续行反斜杠),行内说明用块注释。
// v20:fg_route 两档(0=自动/1=纯官方),保存即把存量 2/3 归一;
// v23:fg_hdr_interp 实验性(HDR 域插帧,可能闪烁)。
#define DLSSNR_STRW(name) L## #name

// 行格式:X(键名, DlssnrParams 成员, 类型, 下限, 上限)。类型决定三
// 消费点(写/读/剪除)的行为:
//   Int  = 写 clamp(int)、读 clamp(int);Bool = 写 0/1、读宽容布尔;
//   X100 = 写 lround(v*100)、读 ÷100 后 clamp;
//   FgRoute = Int 读法 + 越界原值捕获(legacyFgRouteRaw 升级提示用);
//   Skip = 只写哨兵 1 的 saved 键(不读,表内占位保证剪除不误删)。
// 宏表行内禁 // 注释;值域说明见各行尾注释(块注释形态)。
#define DLSSNR_FIELDS(X) \
    X(nr_enabled, nrEnabled, Bool, 0, 0) \
    X(preset, preset, Int, kPresetMin, kPresetMax) \
    X(style, style, Int, kStyleMin, kStyleMax) \
    X(intensity_x100, intensity, X100, kStrengthMin, kStrengthMax) \
    X(local_tone_x100, localToneStrength, X100, kStrengthMin, kStrengthMax) \
    X(local_structure_x100, localStructureStrength, X100, kStrengthMin, kStrengthMax) \
    X(skin_structure_x100, skinStructureStrength, X100, kSkinMin, kSkinMax) \
    X(use_auto_mask, useAutoMask, Bool, 0, 0) \
    X(input_resolution, inputResolutionPercent, Int, kResPctMin, kResPctMax) \
    X(scaling_enabled, scalingEnabled, Bool, 0, 0) \
    X(residual_multiplier_x100, residualMultiplier, X100, kResidualMultMin, kResidualMultMax) \
    X(residual_saturation_x100, residualSaturation, X100, kResidualFineMin, kResidualFineMax) \
    X(residual_lightness_x100, residualLightness, X100, kResidualFineMin, kResidualFineMax) \
    X(shadow_structure_x100, shadowStructureMultiplier, X100, kResidualFineMin, kResidualFineMax) \
    X(reflection_glow_x100, reflectionGlowMultiplier, X100, kResidualFineMin, kResidualFineMax) \
    X(motion_vector_quality, motionVectorQuality, Int, kOfQualityMin, kOfQualityMax) \
    X(ffx_quality, ffxQuality, Int, kFfxQualityMin, kFfxQualityMax) \
    X(nvof_follow_scaling, nvofFollowScaling, Bool, 0, 0) \
    X(fg_enabled, fgEnabled, Bool, 0, 0) \
    X(fg_multiplier, fgMultiplier, Int, kFgMultMin, kFgMultMax) \
    X(fg_route, fgRoute, FgRoute, kFgRouteMin, kFgRouteMax) \
    X(fg_hdr_interp, fgHdrInterp, Bool, 0, 0) \
    X(of_backend, ofBackend, Int, kOfBackendMin, kOfBackendMax) \
    X(anti_flicker, antiFlicker, Int, kAntiFlickerMin, kAntiFlickerMax) \
    X(saved, nrEnabled, Skip, 0, 0)

// RTX Video(VSR/TrueHDR):[rtxvideo] 独立节(v22 起面板全量接管;
// 写侧 = 面板独占,插件只读 —— 2026-10-03 单写者裁定)。
#define RTX_FIELDS(X) \
    X(vsr_mode, rtxVsrMode, Int, kVsrModeMin, kVsrModeMax) \
    X(vsr_scale_x100, rtxVsrScale, X100, kVsrScaleMin, kVsrScaleMax) \
    X(vsr_strength, rtxVsrStrength, Int, kVsrStrengthMin, kVsrStrengthMax) \
    X(hdr_enabled, rtxHdrEnabled, Bool, 0, 0) \
    X(hdr_contrast, rtxHdrContrast, Int, kHdrContrastMin, kHdrContrastMax) \
    X(hdr_saturation, rtxHdrSaturation, Int, kHdrSaturationMin, kHdrSaturationMax) \
    X(hdr_middle_gray, rtxHdrMiddleGray, Int, kHdrMiddleGrayMin, kHdrMiddleGrayMax) \
    X(hdr_peak_nits, rtxHdrMaxLuminance, Int, kHdrMaxLumMin, kHdrMaxLumMax)

namespace vsdlssnr {

// 键读取原语(节名参数化;dlssnr / rtxvideo 两节共用,读取侧单点)。
inline int ReadIniInt(const wchar_t *section, const wchar_t *key, int def,
                      const wchar_t *iniPath) noexcept {
    return static_cast<int>(GetPrivateProfileIntW(section, key, def, iniPath));
}

// 宽容布尔键解析:机器写入恒为 0/1,但手编 "true"/"false" 曾经
// (GetPrivateProfileInt 对非数字返回 0)静默关掉功能。接受常见拼写,
// 其余落回整数语义。
inline bool ReadIniBool(const wchar_t *section, const wchar_t *key, bool def,
                        const wchar_t *iniPath) noexcept {
    wchar_t raw[16]{};
    if (GetPrivateProfileStringW(section, key, L"", raw, 16, iniPath) && raw[0]) {
        const auto eqi = [&](const wchar_t *lit) { return _wcsicmp(raw, lit) == 0; };
        if (eqi(L"true") || eqi(L"yes") || eqi(L"on")) return true;
        if (eqi(L"false") || eqi(L"no") || eqi(L"off")) return false;
    }
    return ReadIniInt(section, key, def ? 1 : 0, iniPath) != 0;
}

// 键行三消费点的类型分发器(写/读各一组重载;剪除只需键名)。全部由上方
// 权威宏行展开 —— 新增键只加一行,漏写/漏读在构造上不可能。
inline void WriteIniRow_Bool(int v, const wchar_t *key, int, int,
                             const wchar_t *section, const wchar_t *path, bool &ok) noexcept {
    wchar_t buf[8];
    swprintf_s(buf, L"%d", v ? 1 : 0);
    ok = WritePrivateProfileStringW(section, key, buf, path) && ok;
}
inline void WriteIniRow_Int(int v, const wchar_t *key, int lo, int hi,
                            const wchar_t *section, const wchar_t *path, bool &ok) noexcept {
    wchar_t buf[32];
    swprintf_s(buf, L"%d", std::clamp(v, lo, hi));
    ok = WritePrivateProfileStringW(section, key, buf, path) && ok;
}
// std::lround rounds half away from zero; (int)(v*100+0.5) would eat negatives
inline void WriteIniRow_X100(float v, const wchar_t *key, int, int,
                             const wchar_t *section, const wchar_t *path, bool &ok) noexcept {
    wchar_t buf[32];
    swprintf_s(buf, L"%d", static_cast<int>(std::lround(v * 100.0f)));
    ok = WritePrivateProfileStringW(section, key, buf, path) && ok;
}
inline void WriteIniRow_FgRoute(int v, const wchar_t *key, int lo, int hi,
                                const wchar_t *section, const wchar_t *path, bool &ok) noexcept {
    WriteIniRow_Int(v, key, lo, hi, section, path, ok);
}
inline void WriteIniRow_Skip(int, const wchar_t *key, int, int,
                             const wchar_t *section, const wchar_t *path, bool &ok) noexcept {
    ok = WritePrivateProfileStringW(section, key, L"1", path) && ok;
}

// DlssnrParams 的布尔位存储为 int(NGX 参数块契约),读法收 int&。
inline void ReadIniRow_Bool(int &field, const wchar_t *key, int, int,
                            const wchar_t *section, const wchar_t *path) noexcept {
    field = ReadIniBool(section, key, field != 0, path) ? 1 : 0;
}
inline void ReadIniRow_Int(int &field, const wchar_t *key, int lo, int hi,
                           const wchar_t *section, const wchar_t *path) noexcept {
    field = std::clamp(ReadIniInt(section, key, field, path), lo, hi);
}
// *_x100 values are hand-editable on disk: clamp to the documented ranges
// (the panel and vpy paths clamp; without this an edited ini would push
// out-of-range floats into the NGX evaluate keys every frame).
inline void ReadIniRow_X100(float &field, const wchar_t *key, float lo, float hi,
                            const wchar_t *section, const wchar_t *path) noexcept {
    field = std::clamp(
        static_cast<float>(ReadIniInt(section, key, static_cast<int>(field * 100), path)) / 100.0f,
        lo, hi);
}
// FgRoute 读法 = Int(clamp);越界原值捕获在 LoadDlssnrIni 表展开前单键做
// (legacyFgRouteRaw 语义见其注释)。
inline void ReadIniRow_FgRoute(int &field, const wchar_t *key, int lo, int hi,
                               const wchar_t *section, const wchar_t *path) noexcept {
    ReadIniRow_Int(field, key, lo, hi, section, path);
}
inline void ReadIniRow_Skip(int &, const wchar_t *, int, int,
                            const wchar_t *, const wchar_t *) noexcept {}

// Persist the parameter profile (the [panel] log toggle is panel-local and
// stays with the panel). Returns false when any key write failed (file
// locked / permission) — callers log it; a silent false-save reads back as
// "settings reset themselves" on the next load.
inline bool WriteDlssnrIni(const DlssnrParams &p, const wchar_t *iniPath) noexcept {
    bool ok = true;
    // 键名与值式全部来自上方权威宏(写/读/剪除三消费点同源)。
#define DLSSNR_WRITE_ROW(name, member, kind, lo, hi) \
    WriteIniRow_##kind(p.member, DLSSNR_STRW(name), lo, hi, L"dlssnr", iniPath, ok);
    DLSSNR_FIELDS(DLSSNR_WRITE_ROW)
#undef DLSSNR_WRITE_ROW
#define RTX_WRITE_ROW(name, member, kind, lo, hi) \
    WriteIniRow_##kind(p.member, DLSSNR_STRW(name), lo, hi, L"rtxvideo", iniPath, ok);
    RTX_FIELDS(RTX_WRITE_ROW)
#undef RTX_WRITE_ROW
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
    // FgRoute 越界原值捕获(表展开前;其读法本体 = Int clamp,见
    // ReadIniRow_FgRoute 注释):pre-v20 存量的重解释由此不再无声。
    if (legacyFgRouteRaw) {
        *legacyFgRouteRaw = ReadIniInt(L"dlssnr", DLSSNR_STRW(fg_route), p.fgRoute, iniPath);
    }
    // RTX Video(v22 起面板全量接管,与写侧同键表)。旧部署样例的
    // vsr_height 键已作废(语义换成倍率 vsr_scale_x100),表内无此键 =
    // 不读取,旧值静默失效(剪除表回收),面板重存即归位。
#define DLSSNR_READ_ROW(name, member, kind, lo, hi) \
    ReadIniRow_##kind(p.member, DLSSNR_STRW(name), lo, hi, L"dlssnr", iniPath);
    DLSSNR_FIELDS(DLSSNR_READ_ROW)
#undef DLSSNR_READ_ROW
#define RTX_READ_ROW(name, member, kind, lo, hi) \
    ReadIniRow_##kind(p.member, DLSSNR_STRW(name), lo, hi, L"rtxvideo", iniPath);
    RTX_FIELDS(RTX_READ_ROW)
#undef RTX_READ_ROW
    return true;
}

// 保存文件的自检键表:由上方权威宏直接生成 —— 写侧加键时本表自动跟上,
// 构造上不可能漂移。读取侧本就不认未知键;文件里的表外键(例:rtxvideo
// 的作废 vsr_height,语义已并入 vsr_scale_x100)由下方 PruneDlssnrIni 剪除。
#define DLSSNR_NAME_ROW(name, member, kind, lo, hi) DLSSNR_STRW(name),
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
