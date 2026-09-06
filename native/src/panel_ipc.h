#pragma once
// Shared IPC layout between dlssnr_panel.exe (writer) and vs_dlssnr.dll
// (reader). Two named file mappings, both 512 bytes:
//   "vs_dlssnr_panel_params" - panel pushes parameter edits (seq-gated)
//   "vs_dlssnr_stats"        - plugin pushes perf stats (60-frame cadence)
// Also hosts the cross-process contract names and the single authority for
// the PanelPayload <-> DlssnrParams field mapping.

#include "dlssnr_params.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <windows.h>

namespace vsdlssnr {

constexpr wchar_t PARAMS_MAPPING[] = L"vs_dlssnr_panel_params";
constexpr wchar_t STATS_MAPPING[] = L"vs_dlssnr_stats";
constexpr wchar_t INI_FILE[] = L"dlssnr_ui.ini";             // saved profile (written/read by both sides)
constexpr wchar_t ALIVE_EVENT[] = L"vs_dlssnr_bridge_alive"; // filter-lifetime marker (bridge + panel watchdog)
constexpr uint32_t PAYLOAD_SIZE = 512;
// "DSSL3": v3 adds the four residual fine-control floats (fields reused from
// the reserved block). The bump rejects payloads from a v1/v2 panel whose
// reserved zeros would decode as saturation/lightness = 0 (= "remove the
// residual change") instead of the neutral 1.
constexpr uint32_t PAYLOAD_MAGIC = 0x334C5344u; // "DSSL3"
constexpr uint32_t STATS_MAGIC = 0x324C5344u;   // "DSSL2"

#pragma pack(push, 8)
struct PanelPayload {
    uint32_t magic;              // PAYLOAD_MAGIC
    uint32_t seq;                // panel increments per write; 0 = empty
    uint32_t generation;         // panel process instance (GetTickCount at start)
    int32_t preset;              // 0-3
    int32_t style;               // 0-2
    float intensity;             // 0-1
    float localTone;             // 0-1
    float localStructure;        // 0-1
    float skinStructure;         // -1-2
    int32_t useAutoMask;         // 0/1
    int32_t uiCorrection;        // 0/1
    int32_t inputResolution;     // 25-100
    float residualMultiplier;    // 1-2
    float residualSaturation;    // 0-2 (relative to DLSSNR's own change)
    float residualLightness;     // 0-2
    float shadowStructure;       // 0-2
    float reflectionGlow;        // 0-2
    int32_t scalingEnabled;      // 0 = ignore inputResolution (treat as 100)
    int32_t saveRequest;         // panel "保存设置" press (applied once per seq)
    int32_t resetRequest;        // panel "重置默认" press
    int32_t logEnabled;          // perf log toggle state
    uint32_t reserved[1];
};
#pragma pack(pop)

static_assert(sizeof(PanelPayload) <= PAYLOAD_SIZE, "payload must fit the mapping");

// ---------------------------------------------------------------------------
// PanelPayload <-> DlssnrParams field mapping — the single authority (was
// hand-copied in the bridge apply path, the panel adopt path and the panel
// publish path). Live params take effect on the next frame; create-time
// params (preset / input_resolution / scaling_enabled) ride
// Request*/ConsumeRebuild — the plugin's _cur side for them is owned by
// ConsumeRebuild alone, never write them behind its back.
// ---------------------------------------------------------------------------
inline void LoadLiveParams(DlssnrParams &p, const PanelPayload &pl) noexcept {
    p.style = std::clamp(pl.style, kStyleMin, kStyleMax);
    p.intensity = std::clamp(pl.intensity, kStrengthMin, kStrengthMax);
    p.localToneStrength = std::clamp(pl.localTone, kStrengthMin, kStrengthMax);
    p.localStructureStrength = std::clamp(pl.localStructure, kStrengthMin, kStrengthMax);
    p.skinStructureStrength = std::clamp(pl.skinStructure, kSkinMin, kSkinMax);
    p.useAutoMask = pl.useAutoMask != 0;
    p.uiCorrection = pl.uiCorrection != 0;
    p.residualMultiplier = std::clamp(pl.residualMultiplier, kResidualMultMin, kResidualMultMax);
    p.residualSaturation = std::clamp(pl.residualSaturation, kResidualFineMin, kResidualFineMax);
    p.residualLightness = std::clamp(pl.residualLightness, kResidualFineMin, kResidualFineMax);
    p.shadowStructureMultiplier = std::clamp(pl.shadowStructure, kResidualFineMin, kResidualFineMax);
    p.reflectionGlowMultiplier = std::clamp(pl.reflectionGlow, kResidualFineMin, kResidualFineMax);
}

inline void LoadCreateParams(DlssnrParams &p, const PanelPayload &pl) noexcept {
    p.preset = std::clamp(pl.preset, kPresetMin, kPresetMax);
    p.inputResolutionPercent = std::clamp(pl.inputResolution, kResPctMin, kResPctMax);
    p.scalingEnabled = pl.scalingEnabled != 0;
}

// Parameter fields only; the caller fills seq/generation/save/reset/log.
inline PanelPayload PayloadFromParams(const DlssnrParams &p) noexcept {
    PanelPayload pl{};
    pl.magic = PAYLOAD_MAGIC;
    pl.preset = p.preset;
    pl.style = p.style;
    pl.intensity = p.intensity;
    pl.localTone = p.localToneStrength;
    pl.localStructure = p.localStructureStrength;
    pl.skinStructure = p.skinStructureStrength;
    pl.useAutoMask = p.useAutoMask ? 1 : 0;
    pl.uiCorrection = p.uiCorrection ? 1 : 0;
    pl.inputResolution = p.inputResolutionPercent;
    pl.scalingEnabled = p.scalingEnabled ? 1 : 0;
    pl.residualMultiplier = p.residualMultiplier;
    pl.residualSaturation = p.residualSaturation;
    pl.residualLightness = p.residualLightness;
    pl.shadowStructure = p.shadowStructureMultiplier;
    pl.reflectionGlow = p.reflectionGlowMultiplier;
    return pl;
}

// ---------------------------------------------------------------------------
// Stats channel. Same publish protocol as PanelPayload: the body lands with
// seq held at 0 (readers skip 0), then the counter is moved alone, so a
// concurrent reader never parses a torn JSON blob.
// ---------------------------------------------------------------------------
#pragma pack(push, 8)
struct StatsPayload {
    uint32_t magic; // STATS_MAGIC
    uint32_t seq;   // publisher increments per write; 0 = write in progress
    char json[PAYLOAD_SIZE - 8];
};
#pragma pack(pop)
static_assert(sizeof(StatsPayload) == PAYLOAD_SIZE, "stats payload must fit the mapping");

// Stats JSON schema keys — the single authority. The writer's snprintf
// format string (dlssnr_context.cpp) must spell exactly these; the panel
// reader is constant-driven.
inline constexpr const char *SK_GPU_LAST = "gpu_last";
inline constexpr const char *SK_GPU_EMA = "gpu_ema";
inline constexpr const char *SK_GPU_P99 = "gpu_p99";
inline constexpr const char *SK_PACK_EMA = "pack_ema";
inline constexpr const char *SK_EVAL_CPU_EMA = "eval_cpu_ema";
inline constexpr const char *SK_UNPACK_EMA = "unpack_ema";
inline constexpr const char *SK_INTERNAL_W = "internal_w";
inline constexpr const char *SK_INTERNAL_H = "internal_h";
inline constexpr const char *SK_WIDTH = "width";
inline constexpr const char *SK_HEIGHT = "height";
inline constexpr const char *SK_SCALING = "scaling";
inline constexpr const char *SK_FPS = "fps";
inline constexpr const char *SK_GPU_NAME = "gpu_name";
inline constexpr const char *SK_GPU_HANG = "gpu_hang";        // published as numeric 1
inline constexpr const char *SK_REMOVED_REASON = "removed_reason";

// Plugin-side stats publisher. Mapping + writable view are created once and
// kept for the process lifetime (the kernel object dies with the plugin
// process; readers map read-only per refresh). Replacing the mapping handle
// per write would leak one handle per publish.
inline bool PublishStatsJson(const char *json) noexcept {
    static HANDLE mapping = nullptr;
    static StatsPayload *view = nullptr;
    static uint32_t seq = 0;
    if (!view) {
        if (!mapping) {
            mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                         0, PAYLOAD_SIZE, STATS_MAPPING);
            if (!mapping) return false;
        }
        view = static_cast<StatsPayload *>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, PAYLOAD_SIZE));
        if (!view) return false;
    }
    size_t n = strlen(json);
    if (n > sizeof(view->json) - 1) n = sizeof(view->json) - 1;
    view->seq = 0; // invalidate while the body lands
    memcpy(view->json, json, n);
    view->json[n] = '\0';
    view->magic = STATS_MAGIC;
    _InterlockedExchange(reinterpret_cast<volatile long *>(&view->seq),
                         static_cast<long>(++seq));
    return true;
}

} // namespace vsdlssnr
