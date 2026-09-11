#pragma once
// Shared IPC layout between dlssnr_panel.exe (writer) and vs_dlssnr.dll
// (reader). Two named file mappings, both 512 bytes:
//   "vs_dlssnr_panel_params" - panel pushes parameter edits (seq-gated)
//   "vs_dlssnr_stats"        - plugin pushes perf stats (per-frame cadence)
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
// "DSSL6": v3 added the four residual fine-control floats; v4 drops the
// write-only resetRequest command field (a reset is just a payload full of
// default values); v5 adds motionVectorQuality (NVOF 光流质量 0-5); v6 adds
// nvofFollowScaling (光流输入跟随内部降采样); v7 adds fgEnabled (DLSS 帧生成,
// 占用原 reserved[0] —— 布局不变,老面板写 0 = 关); v8 adds fgMultiplier
// (插帧倍数 2-4,live,源帧边界生效 —— 结构体增长,新旧混跑按 magic 拒读);
// v9 adds fgRouter (FG 路由 0=SM86/1=SM75,进程级,重启 mpv 生效); v10 adds
// fgBackend (FG 后端 0=自动/1=官方 NGX/2=代理,下个 seek 生效); v11 adds
// nrEnabled (NR 总开关 0/1,默认 1;0 = 跳过降噪推理,补帧/光流不受影响);
// v12 merges fgRouter+fgBackend into fgRoute (0=自动/1=SM86/2=SM75/3=官方
// NGX,进程级,重启 mpv 生效 —— 后端由硬件决定,自动档总能选对,单控件足够)。
// The bump keeps mixed-version panel/plugin pairs from decoding shifted
// offsets as valid payloads — panel and plugin must be deployed as a pair.
constexpr uint32_t PAYLOAD_MAGIC = 0x434C5344u; // "DSLC" (v12, 版本位走 hex:9 之后是 A/B/C)
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
    int32_t nvofFollowScaling;   // 0/1 光流输入跟随内部降采样
    int32_t fgEnabled;           // 0/1 DLSS 帧生成(原 reserved[0],v7)
    int32_t fgMultiplier;        // 2-4 插帧倍数(v8;live,会话内有效密度 = min(此值, 创建倍数))
    int32_t fgRoute;             // 0-3 FG 路由(v12;0=自动,1=SM86,2=SM75,3=官方 NGX,重启生效)
    int32_t nrEnabled;           // 0/1 NR 总开关(v11;0=跳过降噪推理,补帧/光流不受影响)
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
    p.nrEnabled = pl.nrEnabled != 0;
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
    p.nvofFollowScaling = pl.nvofFollowScaling != 0;
    p.fgEnabled = pl.fgEnabled != 0;
    p.fgMultiplier = std::clamp(pl.fgMultiplier, kFgMultMin, kFgMultMax);
}

inline void LoadCreateParams(DlssnrParams &p, const PanelPayload &pl) noexcept {
    p.nrEnabled = pl.nrEnabled != 0;
    p.preset = std::clamp(pl.preset, kPresetMin, kPresetMax);
    p.inputResolutionPercent = std::clamp(pl.inputResolution, kResPctMin, kResPctMax);
    p.scalingEnabled = pl.scalingEnabled != 0;
    p.fgEnabled = pl.fgEnabled != 0;
    // Route 进程级(proxy 模块钉住 + 后端选择都随重启对齐):不进
    // LoadLiveParams,不参与 hotMatch;只在滤镜创建与 proxy INI 同步时
    // 消费,重启后全面生效。
    p.fgRoute = std::clamp(pl.fgRoute, kFgRouteMin, kFgRouteMax);
}

// Parameter fields only; the caller fills seq/generation/save/reset/log.
inline PanelPayload PayloadFromParams(const DlssnrParams &p) noexcept {
    PanelPayload pl{};
    pl.nrEnabled = p.nrEnabled ? 1 : 0;
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
    pl.nvofFollowScaling = p.nvofFollowScaling ? 1 : 0;
    pl.fgEnabled = p.fgEnabled ? 1 : 0;
    pl.fgMultiplier = std::clamp(p.fgMultiplier, kFgMultMin, kFgMultMax);
    pl.fgRoute = std::clamp(p.fgRoute, kFgRouteMin, kFgRouteMax);
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
// 六段用时走每帧 last(与 gpu_last 同语义):EMA 是 120 帧滚动平均,稳态
// 播放时逐帧变化 <0.1ms,面板"处理用时"会冻结成"停几秒 + 突跳"的观感;
// last 随帧呼吸。perf 日志行仍用 EMA(诊断要看趋势,不受影响)。
inline constexpr const char *SK_PACK_LAST = "pack_last";
inline constexpr const char *SK_EVAL_CPU_LAST = "eval_cpu_last";
// DLSS FG 段(补帧 eval 提交的 CPU 墙钟,与 eval_cpu 同口径;fg 关/门关帧
// ≈ 0,面板零值段自动隐藏)。其 GPU 执行仍计入 gpu 段(同条槽 CL)。
inline constexpr const char *SK_FG_LAST = "fg_last";
// NVOF 光流段(门等待+拷贝/降采样提交+execute+输出栅栏的 CPU 墙钟;of=0
// 时恒 0,面板零值段自动隐藏)。与 eval_cpu 互斥可加:eval_cpu 上报时已扣除。
inline constexpr const char *SK_NVOF_LAST = "nvof_last";
inline constexpr const char *SK_UNPACK_LAST = "unpack_last";
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
// DLSS 帧生成状态:on(eval)/ dup(复制真实帧:复位帧/零光流/面板关)/
// off(本会话未激活)/ unavailable(初始化失败,回退 1:1)
inline constexpr const char *SK_FG = "fg";
// 当前插帧倍数(2-4;FG 未激活 = 0。面板显示 "3x")
inline constexpr const char *SK_FG_MULT = "fg_mult";

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
    // Every publisher (the per-frame stats publish in ProcessFrame, the
    // GPU-hang path in WaitFenceValue, Initialize) serializes
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
            if (!mapping) {
                // 探针:stats 通道建立失败一次(此后每次发布都失败,面板
                // 永远空 stats —— 只报一次,不刷 DebugView)。
                static bool warnedCreate = false;
                if (!warnedCreate) {
                    warnedCreate = true;
                    OutputDebugStringA("vs_dlssnr: stats mapping create FAILED; panel stats unavailable\n");
                }
                return false;
            }
        }
        view = static_cast<StatsPayload *>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, PAYLOAD_SIZE));
        if (!view) {
            static bool warnedMap = false;
            if (!warnedMap) {
                warnedMap = true;
                OutputDebugStringA("vs_dlssnr: stats mapping MapViewOfFile FAILED\n");
            }
            return false;
        }
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
