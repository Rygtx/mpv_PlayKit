#pragma once
// User-facing parameters, mapped 1:1 onto Magpie's DLSSNR_AI_Filter.hlsl surface
// (see Magpie experimental src/Effects/DLSSNR/DLSSNR_AI_Filter.hlsl).

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
// NVOF 光流质量(上游 motionVectorQuality,0-5;0 = 无光流,保持零 guidance)
inline constexpr int kOfQualityMin = 0, kOfQualityMax = 5;
// DLSS 帧生成倍数(2-4;proxy MaxGeneratedFrames 上限 3 → 4X 封顶)。
// live 参数:输出节奏由逐帧 _DurationDen ×M 驱动(mpv vapoursynth 契约),
// 逐源帧求和恒等于源时长 —— M 在源帧边界生效,无需重建/重启。
inline constexpr int kFgMultMin = 2, kFgMultMax = 4;

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
    // NVOF 光流质量(0=无 → 静态零 guidance;1-5 → NVOF 会话档位,
    // 上游 NvidiaOpticalFlowQuality)。切换只重建 NVOF 会话(PoolHold 内,
    // 毫秒级),不动 NGX feature;非 NVIDIA/驱动缺 OF 时优雅回退零 guidance。
    int motionVectorQuality = 0;
    // NVOF 输入跟随降采样(0/1,live 参数):开启且 scaling 启用时,NVOF
    // 会话按内部尺寸建立,输入由 GPU compute 从本帧 upload 双线性降采样
    // 直写注册纹理(nvof CL 第一次提交,替代整块拷贝)—— 引擎成本
    // ∝ 内部像素数(4K@45% ≈ 5×)。上游不做此优化(Magpie 的 provider
    // 服务帧生成消费者,运动精度敏感,见 NvidiaOpticalFlowProvider),
    // NR guidance 对粗运动容忍度高(DLSS RR 语义即内部分辨率 motion)。
    // scaling 关闭时本开关无效。
    // 注意 FG(DLSS FG)激活时本开关被忽略:FG 的 MVecs 契约要求与
    // backbuffer 同尺寸的稠密运动场,必须按源尺寸建 NVOF 会话。
    int nvofFollowScaling = 0;
    // DLSS 帧生成(0/1)。语义分两层:
    //   create-time —— 非零且 FG 上下文初始化成功时,滤镜输出帧率 ×2
    //   (vi.fps 翻倍,奇数索引输出插值帧);初始化失败优雅回退 1:1。
    //   live —— 会话内 FG 激活时,面板切 0 = 立即停 eval 改为复制真实帧
    //   (帧数不变);切回 1 恢复 eval。创建时未激活的会话,面板开关在
    //   下次播放(seek 重建滤镜)才生效。
    // FG 路径要求挂 DLSSNR 之后:FG 的 backbuffer = NR 输出。
    int fgEnabled = 0;
    // DLSS 帧生成倍数(2-4,live 参数):每源帧产出 M 帧(1 真实 + M-1
    // 插值),输出帧时长 = 源时长/M。源帧边界生效(当前源帧按触及时的 M
    // 走完),无重建/重启。fgEnabled=0 时忽略。
    int fgMultiplier = 2;
};

// Create-time trio (preset / input_resolution / scaling_enabled) equivalence
// — the single "is a feature rebuild worth it" rule. SharedParams::ConsumeRebuild
// and DlssnrContext::Rebind both compare through this, so the two paths can
// never disagree on when the NGX feature must be recreated. Resolution only
// matters while scaling is on: with scaling off the percent is ignored by the
// pipeline and must never fire a rebuild on its own.
inline bool CreateParamsChanged(const DlssnrParams &pending, const DlssnrParams &cur) noexcept {
    if (pending.preset != cur.preset) return true;
    if (pending.scalingEnabled != cur.scalingEnabled) return true;
    return pending.scalingEnabled != 0 &&
           pending.inputResolutionPercent != cur.inputResolutionPercent;
}
