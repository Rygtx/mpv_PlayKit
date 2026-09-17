#pragma once
// User-facing parameters, mapped 1:1 onto Magpie's DLSSNR_AI_Filter.hlsl surface
// (see Magpie experimental src/Effects/DLSSNR/DLSSNR_AI_Filter.hlsl).

// Documented parameter ranges — the single authority. Every std::clamp on
// these params (vpy args, dlssnr_ui.ini values, panel payload, internal
// size) must reference these constants; a bare literal is a drift bug.
inline constexpr int kPresetMin = 0, kPresetMax = 3;
inline constexpr int kStyleMin = 0, kStyleMax = 2;
// intensity / local tone / local structure: 上游 beta3(3ee4121c,v0.6.7 起)
// 把三项上限从 0-1 重新放宽到 0-2(HEAD DLSSNRFilter.cpp ClampFinite 0-2 实证;
// r2-fix1 的 0-1 收紧期结束)。旧 ini 的 0-100 存量值天然兼容,上限放宽到 200。
inline constexpr float kStrengthMin = 0.0f, kStrengthMax = 2.0f;
inline constexpr float kSkinMin = 0.0f, kSkinMax = 2.0f;         // skin structure(上游 beta3 起 -1=auto 语义整体移除,默认 0)
inline constexpr int kResPctMin = 25, kResPctMax = 100;          // internal resolution percent
inline constexpr float kResidualMultMin = 1.0f, kResidualMultMax = 2.0f;
// 残差精调 4 项(r1-r10 新增):saturation / lightness / shadow structure / reflection glow
inline constexpr float kResidualFineMin = 0.0f, kResidualFineMax = 2.0f;
// NVOF 光流质量(上游 motionVectorQuality,0-5;0 = 无光流,保持零 guidance)
inline constexpr int kOfQualityMin = 0, kOfQualityMax = 5;
// 抗闪烁时域稳定器(上游 antiFlicker,v0.6.8 093efe21/55d4cc38;live 参数):
//   0 = 无  1 = 静态累积(稳定区域检测 + 自适应 EMA,无光流)
//   2 = 光流累积(重投影 + 输入验证 + 自适应 EMA,需光流质量 ≥ 1)
//   3 = 光流累积+(条件幅度 + 持续性迟滞,需光流质量 ≥ 1)
//   4 = 低频时域重建(半分辨率残差历史 + 原色引导重建,需光流质量 ≥ 1)
// 对 NR 链残差做运动补偿累积,**单 pass 即生效**(上游 _temporal 只看
// antiFlicker != 0)。模式 2-4 无光流时自动降级为静态验证(UseMotion=0)。
// 切换重建时域资源(PoolHold 排空,毫秒级纹理分配),不动 NGX feature。
inline constexpr int kAntiFlickerMin = 0, kAntiFlickerMax = 4;
// DLSS 帧生成倍数(2-4;proxy MaxGeneratedFrames 上限 3 → 4X 封顶)。
// live 参数:输出节奏由逐帧 _DurationDen ×M 驱动(mpv vapoursynth 契约),
// 逐源帧求和恒等于源时长 —— M 在源帧边界生效,无需重建/重启。
inline constexpr int kFgMultMin = 2, kFgMultMax = 4;
// DLSS 帧生成路由(0-3,单字段合并原 Router+Backend 两概念)。
//   0 = 自动:官方优先,不可用回落 proxy(回落路由 SM86 —— proxy 无架构
//       自动探测,RTX 20 系请显式选 SM75)
//   1 = SM86:固定走 proxy,Router=SM86(RTX 30 系)
//   2 = SM75:固定走 proxy,Router=SM75(RTX 20 系)
//   3 = 官方 NGX:固定官方签名运行时(RTX 40/50;30/20 系被架构门禁拒载
//       → 补帧关,不回落)
// 进程级,重启 mpv 生效:proxy 模块进程内钉住(SM86/SM75 切换只影响下次
// 加载),后端选择同样随重启对齐 —— 不参与 hotMatch。插件在 proxy
// LoadLibrary 前把该值自动写入 proxy 同目录 dlssg_sm86.ini 的 Router 键
// (plugin.cpp SyncProxyRouterIni),用户不接触 INI 文件。
inline constexpr int kFgRouteMin = 0, kFgRouteMax = 3;
inline constexpr int kFgRouteAuto = 0, kFgRouteProxySm86 = 1,
                     kFgRouteProxySm75 = 2, kFgRouteOfficial = 3;

struct DlssnrParams {
    // NR 总开关(0/1,默认 1)—— 只关降噪,不影响补帧/光流:
    //   create-time —— 与 fgEnabled 皆关时跳过 D3D12/NGX 全部初始化(零
    //     设备/零显存/零 GPU),滤镜纯直通;FG 开时初始化照常(补帧需要
    //     设备与 NVOF),仅降噪评估被跳过(模型驻留显存,即时重开)。
    //   live —— 会话已激活时面板切换立即生效:关 = 跳过 NGX 降噪评估,
    //     Input→Output 直拷,补帧以直通帧为 backbuffer 照常插值(输出
    //     计数与 _Duration 时长契约不变);开 = 立即恢复评估。
    int nrEnabled = 1;
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
    // NGX "DLSSNR.SkinStructureStrength" 0-2(上游 beta3 默认 0;-1=auto 已移除)
    float skinStructureStrength = 0.0f;
    // NGX "DLSSNR.UseAutoMask" 0/1
    int useAutoMask = 1;
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
    // 抗闪烁时域稳定器(0-4,live 参数):对 NR 链残差做运动补偿历史累积,
    // 治 NR 输出的逐帧明暗/结构修正抖动(上游 v0.6.8 antiFlicker,单 pass
    // 即生效)。语义见 kAntiFlickerMin 注释。切换只重建时域资源(PoolHold
    // 排空),不动 NGX feature;NR 关(skipEval/诊断/直通)时本帧跳过并
    // 作废历史。模式 2-4 建议光流质量 ≥ 1,无光流自动降级静态验证。
    int antiFlicker = 0;
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
    // DLSS 帧生成路由(0=自动 官方优先回落 proxy,1=SM86,2=SM75,3=仅官方
    // NGX;进程级,重启 mpv 生效):见 kFgRouteMin 注释。
    int fgRoute = 0;
    // 差异调试视图(0/1,live 参数,**不持久化**):1 = 输出被替换为
    // |NR改动|×20 的灰度图 —— 白 = 改动大,一片灰 = 模型没动画面
    // (OptiScaler DLSSNR fork 的 DebugView=3 同语义,回应"看不出参数
    // 有没有效果")。面板"差异调试 ×20"开关,仅当前会话有效。
    int debugView = 0;
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
