#pragma once
// Shared IPC layout between dlssnr_panel.exe (writer) and vs_dlssnr.dll
// (reader). Two named file mappings, both PAYLOAD_SIZE bytes:
//   "vs_dlssnr_panel_params" - panel pushes parameter edits (seq-gated)
//   "vs_dlssnr_stats"        - plugin pushes perf stats (per-frame cadence)
// Also hosts the cross-process contract names and the single authority for
// the PanelPayload <-> DlssnrParams field mapping.

#include "dlssnr_params.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <windows.h>

namespace vsdlssnr {

constexpr wchar_t PARAMS_MAPPING[] = L"vs_dlssnr_panel_params";
constexpr wchar_t STATS_MAPPING[] = L"vs_dlssnr_stats";
constexpr wchar_t PARAMS_EVENT[] = L"vs_dlssnr_panel_params_event"; // auto-reset; panel signals after each payload write
constexpr wchar_t INI_FILE[] = L"dlssnr_ui.ini";             // saved profile (written/read by both sides)
constexpr wchar_t ALIVE_EVENT[] = L"vs_dlssnr_bridge_alive"; // filter-lifetime marker (bridge + panel watchdog)
// v25 起 2048:v25 时账目诚实化新增 queue/of_engine 两键,1024 的 json 已无
// 余量(v19 同族截断风险)。混跑安全:小视图映射大对象合法,旧面板只看前
// 1024B。v27 stats 转 struct 后布局有 static_assert 兜底,扩容压力消失,
// 2048 作为容量余量保留(混部署的 VirtualQuery 钳制语义不变)。
constexpr uint32_t PAYLOAD_SIZE = 2048;
// "DSSL6": v3 added the four residual fine-control floats; v4 drops the
// write-only resetRequest command field (a reset is just a payload full of
// default values); v5 adds motionVectorQuality (NVOF 光流质量 0-5); v6 adds
// nvofFollowScaling (光流输入跟随内部降采样); v7 adds fgEnabled (DLSS 帧生成,
// 占用原 reserved[0] —— 布局不变,老面板写 0 = 关); v8 adds fgMultiplier
// (插帧倍数 2-6,live,源帧边界生效 —— 结构体增长,新旧混跑按 magic 拒读);
// v9 adds fgRouter (FG 路由 0=SM86/1=SM75,进程级,重启 mpv 生效); v10 adds
// fgBackend (FG 后端 0=自动/1=官方 NGX/2=代理,下个 seek 生效); v11 adds
// nrEnabled (NR 总开关 0/1,默认 1;0 = 跳过降噪推理,补帧/光流不受影响);
// v12 merges fgRouter+fgBackend into fgRoute (0=自动/1=SM86/2=SM75/3=官方
// NGX,进程级,重启 mpv 生效 —— 后端由硬件决定,自动档总能选对,单控件足够);
// v13 adds debugView(差异调试 ×20 视图 0/1,live,不持久化 —— 输出被替换为
// |NR改动|×20 灰度图)并退役 uiCorrection(视频管线无 UI 图层,模型端结构性
// no-op —— 字段保留占位防布局漂移,写入恒 1,面板不再暴露);
// v14/v15/v17 曾引入 antiFlicker/ofBackend/ffxQuality;v18 撤销抗闪烁时域
// 稳定器全链移植(上游 antiFlicker 2026-09-21 裁定移除 —— NR 输出收敛稳定,
// 机制维护成本 > 感知收益),ofBackend/ffxQuality 保留(FFX 后端);结构体
// 缩减,新旧混跑按 magic 拒读;
// v19:debugView 值域 0/1 → 0-2(2 = 光流场调试视图,kDebugViewMax)。
// 布局不变,但新旧混跑时旧插件把 2 归一成 true(= 差异视图),视图语义
// 漂移 —— 按 magic 拒读,面板与插件必须成对部署;
// v20:fgRoute 值域 0-3 → 0-1(0=自动 预载 0.3.x hook 代理,1=纯官方;
// dlssg_for_sm86 0.3.x 起 SM86/SM75 内核档语义作废)。布局不变,但旧面板
// 发 2/3 会被新插件 clamp 成 1(纯官方)= 语义漂移 —— 按 magic 拒读,
// 面板与插件必须成对部署;
// v21(stats DSSL3):stats JSON 新增 fg_mult_max(运行库插值帧上限 ——
// 40 系 gate 解锁失败回落 2x 时,创建/面板倍数仍报 6,没有它面板无从
// 知道实际密度)与 of_detail(光流降级原因,与 fg_detail 同语义)。
// 键为纯增量,但按仓库惯例契约变化即 bump:旧面板读到新 magic 冻结
// 显示(其 501 行 valid 判定拒绝),新版面板读旧 magic 走"版本不匹配"
// 红显 —— 两端都有明确信号,成对部署约束不变。
// v22:RTX Video(VSR/TrueHDR)参数全量进 payload —— vsrMode(0=关
// 1=自动窗口适配 2=手动倍率)/ vsrScale(1.0-4.0)/ vsrStrength(1-4)/
// hdrEnabled + HDR 四参。旧面板的 payload 无这些字段(结构体尾部缺段,
// 读取按 magic 拒)—— 面板与插件必须成对部署。
// v23(payload DSLN):fgHdrInterp(0/1,实验性补帧 HDR 域插值,创建时)
// —— DLSSG 直接吃 TrueHDR 输出(FP16 scRGB backbuffer,ColorBuffersHDR=
// 1)。部分驱动(SM86 移植内核)此路径插值帧压高光 = 闪烁,默认 0(SDR
// 域插帧 + 逐帧 TrueHDR)。结构体尾部追加,新旧混跑按 magic 拒 —— 成对
// 部署。
// v24(payload DSLO):antiFlicker(0-4,抗闪烁时域稳定器,live)回归
// —— v14 首引入、v18 随"维护成本 > 感知收益"裁定移除;NR 参数调强后
// 闪烁复现,机制原样回植(d3d12 侧 TEMPORAL_* HLSL + dlssnr 侧时间线),
// 结构体尾部追加,新旧混跑按 magic 拒 —— 成对部署。
constexpr uint32_t PAYLOAD_MAGIC = 0x4F4C5344u; // "DSLO" (v24, 版本位走 hex:9 之后是 A/B/C/D/E/F)
// v23(stats "DSL4",hex 实际末字节 '4'):stats JSON 新增 rtxvsr_last/
// rtxhdr_last(RTX Video VSR/TrueHDR 专用队列 eval 分段拆账 —— 此前 RTX
// 时间无账目:CPU 录制混进 gpu 段窗口,GPU 执行经 post CL 的队列 Wait 全落
// fg 段"补帧GPU"名下;同时 NR 关的直通帧 eval_cpu 归零)。键为纯增量,bump
// 理由同 v21:成对部署约束,两端都有明确信号。
// v24(stats "DSL5",0x354C5344):管线全流程解耦 —— stats JSON 新增
// conv_last(输出转换段 = post CL 常驻转换窗口:C2 管线色→YUV420 + readback,
// NR 关直连帧并入 C1 补做窗口;此前转换粘在 base/fg CL 被错记进 gpu/fg 段,
// VSR-only 时 fg 段显示的 3.5-8.9ms 实为 VSR 输出转换)。同时 nvof 段语义
// 变化:OF 门按消费者决定(NR 关 + FG 关 = 整段跳过,恒 0)。键为纯增量,
// bump 理由同 v21:成对部署约束,两端都有明确信号。
// stats 契约史:DSL4(v23)rtxvsr/hdr_last 拆账;DSL5(v24)conv_last +
// OF 门语义;DSL6(v25)queue/of_last;DSL7(v26)temporal 三键。
// DSL8(v27,0x384C5344):stats body 从 JSON 文本转定长二进制结构体
// —— JSON 时代的超长静默截断(v19/v25 两次扩容事故)与引号炸体
// (SanitizeJsonDetail 的存在缘由)两事故类分别被 static_assert 与定长
// 拷贝消灭,键名三处人肉对齐(SK_ 常量/格式串/strstr 模式)由编译器接管。
constexpr uint32_t STATS_MAGIC = 0x384C5344u;   // "DSL8" (v27:stats JSON → struct)

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
    float skinStructure;         // 0-2 (上游 beta3 起 -1=auto 移除,默认 0)
    int32_t useAutoMask;         // 0/1
    int32_t uiCorrection;        // 保留字段(v13 退役:视频管线无 UI 图层,恒写 1)
    int32_t inputResolution;     // 25-100
    float residualMultiplier;    // 1-2
    float residualSaturation;    // 0-2 (relative to DLSSNR's own change)
    float residualLightness;     // 0-2
    float shadowStructure;       // 0-2
    float reflectionGlow;        // 0-2
    int32_t scalingEnabled;      // 0 = ignore inputResolution (treat as 100)
    int32_t saveRequest;         // panel "保存设置" press (applied once per seq)
    int32_t logEnabled;          // perf log toggle state
    int32_t motionVectorQuality; // 0-5 NVOF 档位(0 = 无光流;live)
    int32_t ffxQuality;          // 0-2 FFX 档位(0 = 无;1 性能,2 质量;live)
    int32_t nvofFollowScaling;   // 0/1 光流输入跟随内部降采样
    int32_t fgEnabled;           // 0/1 DLSS 帧生成(原 reserved[0],v7)
    int32_t fgMultiplier;        // 2-6 插帧倍数(v8;live,会话内有效密度 = min(此值, 创建倍数))
    int32_t fgRoute;             // 0-1 FG 路由(v12;v20 两档化:0=自动预载 0.3.x 代理,1=纯官方,重启生效)
    int32_t nrEnabled;           // 0/1 NR 总开关(v11;0=跳过降噪推理,补帧/光流不受影响)
    int32_t debugView;           // 0-2 调试视图(v13;v19 起含光流场;live,不持久化)
    int32_t ofBackend;           // 0-1 光流后端(v16;0=ffx 默认 1=nvof;切档下一帧生效)
    // ---- RTX Video(v22)----
    int32_t vsrMode;             // 0=关 1=自动(mpv 窗口适配)2=手动倍率(创建时,reseek)
    float vsrScale;              // 1.0-4.0 手动放大倍率(创建时,reseek)
    int32_t vsrStrength;         // 1-4 VSR QualityLevel(live,per-eval)
    int32_t hdrEnabled;          // 0/1 TrueHDR(创建时,reseek;输出切 P10 PQ)
    int32_t hdrContrast;         // 0-200(live)
    int32_t hdrSaturation;       // 0-200(live)
    int32_t hdrMiddleGray;       // 10-100(live)
    int32_t hdrMaxLuminance;     // 400-2000 nits(live)
    int32_t fgHdrInterp;         // 0/1 实验性补帧 HDR 域插值(v23,创建时)
    int32_t antiFlicker;         // 0-4 抗闪烁时域稳定器(v24,live)
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
    p.residualMultiplier = std::clamp(pl.residualMultiplier, kResidualMultMin, kResidualMultMax);
    p.residualSaturation = std::clamp(pl.residualSaturation, kResidualFineMin, kResidualFineMax);
    p.residualLightness = std::clamp(pl.residualLightness, kResidualFineMin, kResidualFineMax);
    p.shadowStructureMultiplier = std::clamp(pl.shadowStructure, kResidualFineMin, kResidualFineMax);
    p.reflectionGlowMultiplier = std::clamp(pl.reflectionGlow, kResidualFineMin, kResidualFineMax);
    p.motionVectorQuality = std::clamp(pl.motionVectorQuality, kOfQualityMin, kOfQualityMax);
    p.ffxQuality = std::clamp(pl.ffxQuality, kFfxQualityMin, kFfxQualityMax);
    p.nvofFollowScaling = pl.nvofFollowScaling != 0;
    p.fgEnabled = pl.fgEnabled != 0;
    p.fgMultiplier = std::clamp(pl.fgMultiplier, kFgMultMin, kFgMultMax);
    p.debugView = std::clamp(pl.debugView, 0, kDebugViewMax);
    p.ofBackend = std::clamp(pl.ofBackend, kOfBackendMin, kOfBackendMax);
    p.antiFlicker = std::clamp(pl.antiFlicker, kAntiFlickerMin, kAntiFlickerMax);
    // RTX Video live 项(v22):VSR 质量 + HDR 四参,per-eval,下一帧生效。
    p.rtxVsrStrength = std::clamp(pl.vsrStrength, kVsrStrengthMin, kVsrStrengthMax);
    p.rtxHdrContrast = std::clamp(pl.hdrContrast, kHdrContrastMin, kHdrContrastMax);
    p.rtxHdrSaturation = std::clamp(pl.hdrSaturation, kHdrSaturationMin, kHdrSaturationMax);
    p.rtxHdrMiddleGray = std::clamp(pl.hdrMiddleGray, kHdrMiddleGrayMin, kHdrMiddleGrayMax);
    p.rtxHdrMaxLuminance = std::clamp(pl.hdrMaxLuminance, kHdrMaxLumMin, kHdrMaxLumMax);
}

inline void LoadCreateParams(DlssnrParams &p, const PanelPayload &pl) noexcept {
    p.nrEnabled = pl.nrEnabled != 0;
    p.preset = std::clamp(pl.preset, kPresetMin, kPresetMax);
    p.inputResolutionPercent = std::clamp(pl.inputResolution, kResPctMin, kResPctMax);
    p.scalingEnabled = pl.scalingEnabled != 0;
    p.fgEnabled = pl.fgEnabled != 0;
    // Route 进程级(hook 代理预载与否随重启对齐,钩子装上不可拆):不进
    // LoadLiveParams,不参与 hotMatch;只在滤镜创建时消费,重启后全面生效。
    p.fgRoute = std::clamp(pl.fgRoute, kFgRouteMin, kFgRouteMax);
    // RTX Video 创建时项(v22):mode/scale/hdrEnabled 变化 = 槽资源几何
    // 形态变化 —— 面板经 reseek 触发链重建,新实例在 create 时采纳本节。
    p.rtxVsrMode = std::clamp(pl.vsrMode, kVsrModeMin, kVsrModeMax);
    p.rtxVsrScale = std::clamp(pl.vsrScale, kVsrScaleMin, kVsrScaleMax);
    p.rtxHdrEnabled = pl.hdrEnabled != 0;
    // v23:实验性补帧 HDR 域插值(创建时;仅 HDR+FG 会话有意义)。
    p.fgHdrInterp = pl.fgHdrInterp != 0;
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
    pl.uiCorrection = 1; // 退役保留字段(v13):视频管线无 UI 图层,恒写模型默认
    pl.inputResolution = p.inputResolutionPercent;
    pl.scalingEnabled = p.scalingEnabled ? 1 : 0;
    pl.residualMultiplier = p.residualMultiplier;
    pl.residualSaturation = p.residualSaturation;
    pl.residualLightness = p.residualLightness;
    pl.shadowStructure = p.shadowStructureMultiplier;
    pl.reflectionGlow = p.reflectionGlowMultiplier;
    pl.motionVectorQuality = p.motionVectorQuality;
    pl.ffxQuality = p.ffxQuality;
    pl.nvofFollowScaling = p.nvofFollowScaling ? 1 : 0;
    pl.fgEnabled = p.fgEnabled ? 1 : 0;
    pl.fgMultiplier = std::clamp(p.fgMultiplier, kFgMultMin, kFgMultMax);
    pl.fgRoute = std::clamp(p.fgRoute, kFgRouteMin, kFgRouteMax);
    pl.debugView = std::clamp(p.debugView, 0, kDebugViewMax);
    pl.ofBackend = std::clamp(p.ofBackend, kOfBackendMin, kOfBackendMax);
    pl.antiFlicker = std::clamp(p.antiFlicker, kAntiFlickerMin, kAntiFlickerMax);
    // RTX Video(v22):live 与创建时项全量随载荷(payload 单通道)。
    pl.vsrMode = std::clamp(p.rtxVsrMode, kVsrModeMin, kVsrModeMax);
    pl.vsrScale = std::clamp(p.rtxVsrScale, kVsrScaleMin, kVsrScaleMax);
    pl.vsrStrength = std::clamp(p.rtxVsrStrength, kVsrStrengthMin, kVsrStrengthMax);
    pl.hdrEnabled = p.rtxHdrEnabled ? 1 : 0;
    pl.hdrContrast = std::clamp(p.rtxHdrContrast, kHdrContrastMin, kHdrContrastMax);
    pl.hdrSaturation = std::clamp(p.rtxHdrSaturation, kHdrSaturationMin, kHdrSaturationMax);
    pl.hdrMiddleGray = std::clamp(p.rtxHdrMiddleGray, kHdrMiddleGrayMin, kHdrMiddleGrayMax);
    pl.hdrMaxLuminance = std::clamp(p.rtxHdrMaxLuminance, kHdrMaxLumMin, kHdrMaxLumMax);
    pl.fgHdrInterp = p.fgHdrInterp ? 1 : 0;
    return pl;
}

// ---------------------------------------------------------------------------
// Stats channel. v27 body = fixed-layout binary struct (was a JSON text —
// the silent-truncation incident class, paid twice at v19/v25, dies at
// compile time via the static_assert below). Publish protocol unchanged:
// the body lands with seq held at 0 (readers skip 0), then the counter is
// moved alone, so a concurrent reader never sees a torn body.
// ---------------------------------------------------------------------------
#pragma pack(push, 8)
struct StatsPayload {
    uint32_t magic; // STATS_MAGIC
    uint32_t seq;   // publisher increments per write; 0 = write in progress

    // ---- numbers: 14 float + 13 uint32 (pack(8), no padding gaps) ----
    // Render timings are per-frame last values (EMA is the 120-frame rolling
    // mean, consumed by the perf log line only — steady-state EMA reads
    // frozen on the panel). gpuLast sentinel: partial bodies (dead state /
    // passthrough / GPU hang / init) carry no render timing — the default
    // member init is -1, and the panel gates on >= 0 (the equivalent of the
    // old JSON "key missing" default).
    float gpuLast = -1.0f;
    float packLast;      // input packing segment
    float evalCpuLast;   // "NGX call" CPU cost: NR eval + RTX evals submit chain
    float unpackLast;    // output unpack segment
    float ofLast;        // optical-flow segment, backend-neutral full span
    float fgLast;        // DLSS FG segment = all visible frame-gen cost
    float rtxVsrLast;    // RTX Video VSR eval (dedicated queue GPU wall clock)
    float rtxHdrLast;    // TrueHDR chain window (real + per-interpolated frame)
    float convLast;      // post CL output conversion (C2 -> YUV420 + readback; always > 0)
    float queueLast;     // inter-frame queue wait (submit -> preBase timestamps)
    float slotWait;      // slot-pool wait last (3 slots in flight; diag page)
    float lockWait;      // evaluate mutex wait last (diag page)
    float fps;           // 1s window processed frame rate
    float temporalW;     // anti-flicker last-frame blend weight exp(-dt/80ms); 0 = seed frame
    uint32_t internalW;  // internal evaluation resolution
    uint32_t internalH;
    uint32_t width;      // source size (never includes VSR output)
    uint32_t height;
    uint32_t scaling;    // 0/1 internal downscale active
    uint32_t fgMult;        // current frame-gen multiple 2-6 (inactive = 0; panel shows "3x")
    uint32_t fgMultCreate;  // FG session creation multiple (inactive = 0)
    uint32_t fgMultMax;     // runtime generated-frame cap (0-5). Sole signal when a
                            // 40-series gate unlock falls back to 2x; panel red-rule:
                            // fgMultMax + 1 < fgMultCreate
    uint32_t gateSkips;     // OF frame-gate counters (diag page)
    uint32_t gateExpired;
    uint32_t gateResets;
    uint32_t gpuHang;       // non-0 = GPU hang/device removed (body carries only removedReason)
    uint32_t temporalRoute; // anti-flicker effective level 0-4 (diag page)

    // ---- strings: fixed width; overlong copies truncate at the field edge,
    // the struct itself can never be corrupted (CopyStatStr below). Detail
    // strings still go through SanitizeJsonDetail upstream (display hygiene).
    char gpuName[160];       // render adapter name (UTF-8)
    char stateDetail[200];   // dead/passthrough state reason (producer-capped at 200)
    char filterState[16];    // ok / nvof_zero (alive, per-tick; default ok)
                             // passthrough / ngx_faulted (dead, edge-published once)
    char ofMode[40];         // off / zero / backend capability string (max "fxof q5 qual 1920x1080" = 23)
    char fgState[16];        // on / dup / off / unavailable
    char fgRouteEff[16];     // off / official-hook / official / copy
    char fgDetail[128];      // last FG init failure reason (sanitized; cleared on success)
    char ofDetail[96];       // last OF session creation failure reason (same)
    char rtx[96];            // RTX Video live state: off / vsr WxH / hdr WxH / vsr+hdr WxH
    char rtxDetail[96];      // last VSR/TrueHDR failure reason (same)
    char temporal[10];       // off / seed / steady / failed (anti-flicker diag)
    char removedReason[16];  // "0x%08lX" (gpuHang body only)
};
#pragma pack(pop)
static_assert(sizeof(StatsPayload) <= PAYLOAD_SIZE, "stats payload must fit the mapping");
static_assert(std::is_trivially_copyable_v<StatsPayload>, "stats payload crosses shared memory");

// Fixed-width truncating copy — the single entry point for every stats
// string field, both writer and panel side. Truncation clips at the field
// edge and cannot corrupt the struct: the JSON-era incident class (quotes
// breaking the body, overlong bodies silently dropping tail keys) has no
// struct-world equivalent.
template <size_t N>
inline void CopyStatStr(char (&dst)[N], const char *src) noexcept {
    strncpy_s(dst, N, src ? src : "", _TRUNCATE);
}

// 插件发布(state_detail)、面板消费的会话态字符串(单一出处,防两侧漂移):
// NR 已开但会话未初始化(全关直通实例上开 NR)—— 面板见此应自动 reseek
// 补触发重建(闭环兜底,覆盖开关瞬间 stats 漏判等一切误判路径)。
inline constexpr const char *kStateNrSeekInit = "NR on; seek to initialize";
// Detail/state strings are plain char fields now (no JSON to corrupt), but
// D3D12 debug-layer text (VSDLSSNR_D3D12_DEBUG=1) still carries quotes and
// control characters — scrub them for display hygiene. plugin.cpp and
// dlssnr_context dead-state publishes share this.
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

// stats 通道自身建立失败的留痕钩子:PublishStats 属于头文件层,够不
// 着 dlssnr_context 的 timing log;本头文件只声明函数指针,插件侧启动时
// 注册(见 dlssnr_context.cpp)。没有它,映射创建失败只有 OutputDebugString
// 一个出口 —— GUI mpv 场景没人看,面板从此空白而用户常看的日志零痕迹,
// 与"插件没加载"无法区分。
inline void (*g_statsChannelFailLog)(const char *) = nullptr;

inline bool PublishStats(const StatsPayload &st) noexcept {
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
                // 永远空 stats —— 只报一次,不刷 DebugView)。钩子注册时
                // 进 timing log(GUI mpv 不透传 OutputDebugString)。
                static bool warnedCreate = false;
                if (!warnedCreate) {
                    warnedCreate = true;
                    OutputDebugStringA("vs_dlssnr: stats mapping create FAILED; panel stats unavailable\n");
                    if (g_statsChannelFailLog) {
                        g_statsChannelFailLog(
                            "DLSSNR STATUS: stats mapping create FAILED; panel stats unavailable");
                    }
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
                if (g_statsChannelFailLog) {
                    g_statsChannelFailLog(
                        "DLSSNR STATUS: stats mapping MapViewOfFile FAILED; panel stats unavailable");
                }
            }
            return false;
        }
    }
    PublishWithSeq(&view->seq, ++seq, [&] {
        *view = st; // plain field copy; no format step, nothing to truncate
        view->magic = STATS_MAGIC;
    });
    return true;
}

} // namespace vsdlssnr
