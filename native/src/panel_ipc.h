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
#include <windows.h>

namespace vsdlssnr {

constexpr wchar_t PARAMS_MAPPING[] = L"vs_dlssnr_panel_params";
constexpr wchar_t STATS_MAPPING[] = L"vs_dlssnr_stats";
constexpr wchar_t PARAMS_EVENT[] = L"vs_dlssnr_panel_params_event"; // auto-reset; panel signals after each payload write
constexpr wchar_t INI_FILE[] = L"dlssnr_ui.ini";             // saved profile (written/read by both sides)
constexpr wchar_t ALIVE_EVENT[] = L"vs_dlssnr_bridge_alive"; // filter-lifetime marker (bridge + panel watchdog)
// v19 起 1024:stats JSON 加了 fg 路由/失败原因/排队细分等 8 个键,512 的
// body 在长 GPU 名 + 能力串下已逼近截断线(PublishStatsJson 超长静默截断 =
// 尾键丢失,面板读不到还不报错)。参数结构体本身 ~140B,扩容无布局影响。
constexpr uint32_t PAYLOAD_SIZE = 1024;
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
constexpr uint32_t PAYLOAD_MAGIC = 0x4D4C5344u; // "DSLM" (v22, 版本位走 hex:9 之后是 A/B/C/D/E/F)
// v23(stats DSSL4):stats JSON 新增 rtxvsr_last/rtxhdr_last(RTX Video
// VSR/TrueHDR 专用队列 eval 分段拆账 —— 此前 RTX 时间无账目:CPU 录制混进
// gpu 段窗口,GPU 执行经 post CL 的队列 Wait 全落 fg 段"补帧GPU"名下;
// 同时 NR 关的直通帧 eval_cpu 归零)。键为纯增量,bump 理由同 v21:成对
// 部署约束,两端都有明确信号。
constexpr uint32_t STATS_MAGIC = 0x344C5344u;   // "DSSL4" (v23)

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
    // RTX Video(v22):live 与创建时项全量随载荷(payload 单通道)。
    pl.vsrMode = std::clamp(p.rtxVsrMode, kVsrModeMin, kVsrModeMax);
    pl.vsrScale = std::clamp(p.rtxVsrScale, kVsrScaleMin, kVsrScaleMax);
    pl.vsrStrength = std::clamp(p.rtxVsrStrength, kVsrStrengthMin, kVsrStrengthMax);
    pl.hdrEnabled = p.rtxHdrEnabled ? 1 : 0;
    pl.hdrContrast = std::clamp(p.rtxHdrContrast, kHdrContrastMin, kHdrContrastMax);
    pl.hdrSaturation = std::clamp(p.rtxHdrSaturation, kHdrSaturationMin, kHdrSaturationMax);
    pl.hdrMiddleGray = std::clamp(p.rtxHdrMiddleGray, kHdrMiddleGrayMin, kHdrMiddleGrayMax);
    pl.hdrMaxLuminance = std::clamp(p.rtxHdrMaxLuminance, kHdrMaxLumMin, kHdrMaxLumMax);
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
// 八段用时走每帧 last(与 gpu_last 同语义):EMA 是 120 帧滚动平均,稳态
// 播放时逐帧变化 <0.1ms,面板"处理用时"会冻结成"停几秒 + 突跳"的观感;
// last 随帧呼吸。perf 日志行仍用 EMA(诊断要看趋势,不受影响)。
inline constexpr const char *SK_PACK_LAST = "pack_last";
inline constexpr const char *SK_EVAL_CPU_LAST = "eval_cpu_last";
// DLSS FG 段(post CL 耗时:FG 推理 + 插值 YUV/回读 + RTX 帧的管线色输出
// 转换/回读 —— 与 gpu 段按 base/fg 两次提交的栅栏完成点差分,timestamp
// query 与 NGX 同 CL 会 SEH 无法直测;起点 = 最后一个 RTX 完成栅栏,无 RTX
// 帧 = base 完成点)。fg 关/门关帧 ≈ 0,面板零值段自动隐藏;RTX 开 + FG 关
// 时剩输出转换/回读的零头(一次栅栏差分拆不出更细,量级 ~0.1ms)。
inline constexpr const char *SK_FG_LAST = "fg_last";
// RTX Video 分段(VSR/TrueHDR 专用队列 eval 的 GPU 墙钟):base 栅栏完成后
// CPU 有界等待各自完成栅栏拆账(fence 链 base→vsr→hdr,hdr 起点 = vsr 完成
// 点)。此前 RTX 时间无账目:CPU 录制混进 gpu 段窗口、GPU 执行经 post CL 的
// 队列 Wait 全落 fg 段 —— "补帧关了 fg 段还在跳"的真身。vsr/hdr 关闭时恒
// 0,面板零值段自动隐藏。
inline constexpr const char *SK_RTXVSR_LAST = "rtxvsr_last";
inline constexpr const char *SK_RTXHDR_LAST = "rtxhdr_last";
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
// 当前插帧倍数(2-6;FG 未激活 = 0。面板显示 "3x")
inline constexpr const char *SK_FG_MULT = "fg_mult";
// FG 路由实际生效档(面板核心诉求:auto 档下"这次到底走了谁"不再翻
// timing log):
//   off           — 创建时 FG 未请求(面板 FG = 关)
//   official-hook — 官方 NGX 链,0.3.x hook 代理在托接管 DLSS-G(RTX 30/20
//                   唯一路径;进程级,与本次是否预载解耦)
//   official      — 官方 NGX 链直连(无 hook 代理在托;RTX 40/50)
//   copy          — FG 已请求但初始化失败 → 输出回落 1:1/复制帧(与 SK_FG
//                   的 unavailable/dup 互补:那个说"帧是什么",这个说"谁产的")
inline constexpr const char *SK_FG_ROUTE_EFFECTIVE = "fg_route_eff";
// FG 会话创建倍数(2-6;FG 未激活 = 0)。live 倍数超过它时多出的档位本
// 会话无槽可填(面板红色提示"需 seek 重建")。
inline constexpr const char *SK_FG_MULT_CREATE = "fg_mult_create";
// FG 会话运行库插值帧上限(0-5;FG 未激活 = 0,含义与 fg_mult_create 的
// 0 同语义)。MaxGeneratedFrames 查询值(含 mfg gate 解锁结果)。没有它,
// 40 系 gate 解锁失败回落 2x 时创建/面板仍全绿显示 6x —— 输出按 6x 节拍
// 但只有 2x 密度(超限槽复制真实帧),用户毫无感知。面板红显条件:
// fg_mult_create > fg_mult_max > 0。
inline constexpr const char *SK_FG_MULT_MAX = "fg_mult_max";
// FG 最近一次初始化失败原因(消毒串;成功后清空)。"为什么没插帧"的
// 面板侧直接答案,不再翻 timing log。
inline constexpr const char *SK_FG_DETAIL = "fg_detail";
// 光流最近一次会话创建失败原因(消毒串;成功后清空;fg_detail 的光流
// 同款)。诊断页"光流: 请求 X | 实际 zero"只说降级事实,原因(SM6.2
// 不支持/驱动拒双向/dll 缺失)在这里直达面板。
inline constexpr const char *SK_OF_DETAIL = "of_detail";
// RTX Video 实际管线态(off / vsr / hdr / vsr+hdr,带 out 几何)与最近
// 一次 VSR/TrueHDR 初始化失败原因(消毒串;成功后清空)。请求开但实态
// off = 降级(能力/部署问题),面板诊断页红显,原因在 rtx_detail。
inline constexpr const char *SK_RTX = "rtx";
inline constexpr const char *SK_RTX_DETAIL = "rtx_detail";
// ---- 排队细分(诊断页专供;主面板只留八段用时,这里放"要翻 perf 行
// 才有"的次级数据)----
// 槽池等待 last(3 槽全在飞时的排队;gpu 段正常而此值大 = GPU 超容量)
inline constexpr const char *SK_SLOT_WAIT = "slot_wait";
// evaluate 互斥等待 last(NGX feature 单例的 CPU 侧串行排队)
inline constexpr const char *SK_LOCK_WAIT = "lock_wait";
// 光流帧序门累计(播种/过期/显式重置;NVOF/FFX 门语义见 of_frame_gate.h,
// FFX 无探针时恒 0)
inline constexpr const char *SK_GATE_SKIPS = "gate_skips";
inline constexpr const char *SK_GATE_EXPIRED = "gate_expired";
inline constexpr const char *SK_GATE_RESETS = "gate_resets";

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

// stats 通道自身建立失败的留痕钩子:PublishStatsJson 属于头文件层,够不
// 着 dlssnr_context 的 timing log;本头文件只声明函数指针,插件侧启动时
// 注册(见 dlssnr_context.cpp)。没有它,映射创建失败只有 OutputDebugString
// 一个出口 —— GUI mpv 场景没人看,面板从此空白而用户常看的日志零痕迹,
// 与"插件没加载"无法区分。
inline void (*g_statsChannelFailLog)(const char *) = nullptr;

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
