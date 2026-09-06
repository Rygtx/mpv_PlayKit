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
#include <mutex>
#include <windows.h>

namespace vsdlssnr {

constexpr wchar_t PARAMS_MAPPING[] = L"vs_dlssnr_panel_params";
constexpr wchar_t STATS_MAPPING[] = L"vs_dlssnr_stats";
constexpr wchar_t PARAMS_EVENT[] = L"vs_dlssnr_panel_params_event"; // auto-reset; panel signals after each payload write
constexpr wchar_t INI_FILE[] = L"dlssnr_ui.ini";             // saved profile (written/read by both sides)
constexpr wchar_t ALIVE_EVENT[] = L"vs_dlssnr_bridge_alive"; // filter-lifetime marker (bridge + panel watchdog)
constexpr uint32_t PAYLOAD_SIZE = 512;
// "DSSL5": v3 added the four residual fine-control floats; v4 drops the
// write-only resetRequest command field (a reset is just a payload full of
// default values); v5 adds motionVectorQuality (NVOF 光流质量 0-5). The bump
// keeps mixed-version panel/plugin pairs from decoding shifted offsets as
// valid payloads — panel and plugin must be deployed as a pair.
constexpr uint32_t PAYLOAD_MAGIC = 0x354C5344u; // "DSSL5"
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
    int32_t logEnabled;          // perf log toggle state
    int32_t motionVectorQuality; // 0-5 (0 = 无光流)
    uint32_t reserved[2];
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
    p.motionVectorQuality = std::clamp(pl.motionVectorQuality, kOfQualityMin, kOfQualityMax);
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
    pl.motionVectorQuality = p.motionVectorQuality;
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

// Stats JSON schema keys — the single authority. The writers' snprintf
// format strings (dlssnr_context.cpp / d3d12_context.cpp) interpolate exactly
// these; the panel reader is constant-driven.
inline constexpr const char *SK_GPU_LAST = "gpu_last";
inline constexpr const char *SK_GPU_EMA = "gpu_ema";
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
// 滤镜状态(面板可见的降级报告;GUI mpv 看不到日志,timing log 没人看):
//   ok / nvof_zero  — 存活态,由周期 stats tick 携带(缺省 = ok)
//   passthrough / ngx_faulted — 死亡态,边缘发布一次,替换冻结的旧统计 body
inline constexpr const char *SK_FILTER_STATE = "filter_state";
// 死亡状态的原因串(已经 SanitizeJsonDetail 消毒:无引号/控制字符)
inline constexpr const char *SK_STATE_DETAIL = "state_detail";
// NVOF 实际模式(请求档位 ≠ 实际能力时在这里暴露,如 Turing 无 cost):
// off | zero | forward | forward+cost | both | both+cost
inline constexpr const char *SK_OF_MODE = "of_mode";

// stats JSON 的 detail 字段走面板的朴素解析(strstr + 下一个引号):剔除
// 引号、反斜杠与控制字符,防止 D3D12 debug-layer 文本(VSDLSSNR_D3D12_DEBUG=1
// 时可含引号)截断或污染 JSON。plugin.cpp 与 dlssnr_context 的死亡状态
// 发布共用。
inline void SanitizeJsonDetail(const char *src, char *dst, size_t dstLen) noexcept {
    if (!dst || !dstLen) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dstLen; ++i) {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        dst[o++] = (c == '"' || c == '\\' || c < 0x20 || c == 0x7F) ? ' ' : static_cast<char>(c);
    }
    dst[o] = '\0';
}

// Shared publish protocol for both channels: 1) seq = 0 (readers skip 0 /
// treat it as a write in progress), 2) write the body, 3) interlocked
// seq = newSeq. Single authority so a future protocol change (extra barrier,
// generation guard) lands on both writers, not just one.
template <typename WriteBody>
inline void PublishWithSeq(volatile uint32_t *seq, uint32_t newSeq, WriteBody &&writeBody) noexcept {
    *seq = 0;
    writeBody();
    _InterlockedExchange(reinterpret_cast<volatile long *>(seq), static_cast<long>(newSeq));
}

// Plugin-side stats publisher. Mapping + writable view are created once and
// kept for the process lifetime (the kernel object dies with the plugin
// process; readers map read-only per refresh). Replacing the mapping handle
// per write would leak one handle per publish.
inline bool PublishStatsJson(const char *json) noexcept {
    // Every publisher (the 60-frame stats tick under the plugin's timing
    // mutex, the GPU-hang path in WaitFenceValue, Initialize) serializes
    // here: two writers must never interleave the invalidate/body/publish
    // sequence, or the reader accepts a torn body with a valid seq.
    static std::mutex publishLock;
    static HANDLE mapping = nullptr;
    static StatsPayload *view = nullptr;
    static uint32_t seq = 0;
    std::lock_guard<std::mutex> guard(publishLock);
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
    PublishWithSeq(&view->seq, ++seq, [&] {
        memcpy(view->json, json, n);
        view->json[n] = '\0';
        view->magic = STATS_MAGIC;
    });
    return true;
}

} // namespace vsdlssnr
