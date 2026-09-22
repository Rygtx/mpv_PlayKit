#pragma once
// User-facing parameters, mapped 1:1 onto Magpie's DLSSNR_AI_Filter.hlsl surface
// (see Magpie experimental src/Effects/DLSSNR/DLSSNR_AI_Filter.hlsl).
#include <algorithm>

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
// FFX 光流质量(0-2;0 = 无光流,1 = 性能/OF extent 半分辨率,2 = 质量/
// 全分辨率。上游 motionVectorQuality 的 1-2/3-5 在 FFX 内各只对应一种
// 行为,收敛为独立三档 —— 不留冗余档位)
inline constexpr int kFfxQualityMin = 0, kFfxQualityMax = 2;
// DLSS 帧生成倍数(2-6;dlssg_for_sm86 0.3.x 310.9 运行库支持 6X)。
// live 参数:输出节奏由逐帧 _DurationDen ×M 驱动(mpv vapoursynth 契约),
// 逐源帧求和恒等于源时长 —— M 在源帧边界生效,无需重建/重启。
// 5x/6x 无过渡态:部署 ini 默认 MaxGeneratedFrames=5(6x 上限常开,
// fetch-deps 归一;上限只是钳位,倍数由插件按面板请求),面板改倍数原地
// 重载即时生效。输出帧率 = 源 ×M,显示端刷新率建议 ≥ 输出帧率。
inline constexpr int kFgMultMin = 2, kFgMultMax = 6;
// 调试视图(live,不持久化):0 = 关,1 = 差异 ×20(|NR改动|×20 灰度),
// 2 = 光流场(方向→色相、幅值→亮度;排查"光流有没有流/方向对不对")。
inline constexpr int kDebugViewMax = 2;
// DLSS 帧生成路由(0-1,单字段;0.3.x 起代理为 hook 型、按物理架构自动
// 选核,原 SM86/SM75 内核档与 Router INI 写入语义作废)。
//   0 = 自动:FG 初始化前预载 hook 代理(version.dll,LoadLibrary 即装钩
//       —— 只拦截 nvngx_dlssg.dll 加载替换为内嵌运行库 + fg_gate 钩子接管
//       核心能力查询/CreateFeature),官方链被其接管后 30/20 系照常出帧;
//       无代理部署 = 纯官方降级
//   1 = 纯官方:不预载代理,直连官方签名运行时(RTX 40/50;30/20 系被架构
//       门禁拒载 0xBAD0000B → 补帧关,不回落)
// 进程级,重启 mpv 生效:预载模块钉住且钩子装上不可逆(自动→纯官方同进程
// 降不回去),不参与 hotMatch。0.2.4 直驱 proxy 契约已随本版删除。
inline constexpr int kFgRouteMin = 0, kFgRouteMax = 1;
inline constexpr int kFgRouteAuto = 0, kFgRouteOfficial = 1;
// 光流后端(0-1,创建时 —— 会话重建/seek 生效,面板热改 = 下一帧):
//   0 = ffx(默认;AMD FidelityFX Optical Flow,FSR3 SDK 金字塔+块匹配;
//       跨厂商通用,需 D3D12 SM6.2 + WaveOps + R16G16_SINT UAV;上游 Magpie
//       在 RTX 5070 Ti 上同款实测有流,用户裁定 2026-09-21 免选默认)
//   1 = nvof(NVIDIA NVOF 引擎,可选;非 NVIDIA 卡会话必败 → 零 guidance,
//       不跨后端回落,对齐 fg_route 钉档先例:行为可预测)
// (原 auto 厂商排序档与 halfres 兜底已移除,不兼容旧 INI —— 越界值经
//  clamp 落 1 = nvof。)
inline constexpr int kOfBackendMin = 0, kOfBackendMax = 1;
inline constexpr int kOfBackendFfx = 0, kOfBackendNvof = 1;
// ---- RTX Video(VSR / TrueHDR)----
// NVIDIA RTX Video SDK 1.1 的两个 NGX feature(Feature 16 / 14,与 NR/FG
// 同一 NGX core;snippet nvngx_vsr.dll / nvngx_truehdr.dll 部署在 ngx\):
//   VSR     — SDR RGB 放大(输入/输出 BGRA8,quality 1-4=AI;官方 0=bicubic
//             不暴露 —— 关闭即旁路,不再付一份 GPU 价买双线性)
//   TrueHDR — SDR RGB → FP16 scRGB 线性(不能吃 HDR 输入,官方明文 VSR→HDR 序)
// 管线顺序(NR→VSR→HDR→FG)。面板全量接入(payload v22);ini 走同一份
// dlssnr_ui.ini 的 [rtxvideo] 节(面板保存时一并写入)。
inline constexpr int kVsrModeMin = 0, kVsrModeMax = 2;   // 0=关 1=自动(mpv 窗口适配)2=手动倍率
inline constexpr int kVsrStrengthMin = 1, kVsrStrengthMax = 4; // 1-4=AI 档(4 最优)
inline constexpr float kVsrScaleMin = 1.0f, kVsrScaleMax = 4.0f; // 手动倍率(mode=2)
// VSR 单 pass 官方上限倍率:超过时先 VSR 到 4x 中间尺寸,OUT 收尾由
// convert-out 双线性补完(VSR 只做放大,bilinear 从 4x 高清中间位拉到目标)。
inline constexpr float kVsrMaxScale = 4.0f;
inline constexpr int kHdrContrastMin = 0, kHdrContrastMax = 200;       // TrueHDR Contrast
inline constexpr int kHdrSaturationMin = 0, kHdrSaturationMax = 200;   // TrueHDR Saturation
inline constexpr int kHdrMiddleGrayMin = 10, kHdrMiddleGrayMax = 100;  // TrueHDR MiddleGray
inline constexpr int kHdrMaxLumMin = 400, kHdrMaxLumMax = 2000;        // MaxLuminance(nits)

struct RtxVideoParams {
    // VSR 目标尺寸来源(创建时):0=关 1=自动(mpv 窗口客户区适配,1:1/
    // 缩小时自动旁路 —— VSR 只支持放大,对齐浏览器端官方语义)2=手动倍率
    // (vsrScale,宽度按源宽高比推)。
    int vsrMode = 0;
    float vsrScale = 2.0f;   // 手动放大倍率(mode=2;1.0-4.0)
    // mode=1 时由 plugin.cpp 直读 mpv 窗口客户区(= osd-dimensions,进程
    // 内 GetClientRect 零桥接,clamp 到所在显示器)填入;链创建粒度,非
    // 持久化字段,ini 不读写。窗口换屏/改尺寸后的生效点 = 下一次链重建。
    int vsrAutoHeight = 2160;
    int vsrStrength = 2;     // VSR QualityLevel(1-4=AI;per-eval,live)
    // TrueHDR(0/1):输出域切换为 HDR10 —— 滤镜输出 YUV420P10(BT.2020
    // PQ limited),mpv 侧 target-colorspace-hint 上屏。创建时。
    int hdrEnabled = 0;
    int hdrContrast = 100;       // 0-200(官方默认 100;per-eval,live)
    int hdrSaturation = 100;     // 0-200
    int hdrMiddleGray = 50;      // 10-100
    int hdrMaxLuminance = 1000;  // 400-2000 nits

    // 相等性只覆盖**创建时几何/形态**:mode/scale/autoHeight/hdrEnabled。
    // strength 与 HDR 四参是 per-eval live 值 —— 变化不换槽资源几何,
    // 参与 == 会让 hotMatch 拒掉本可秒回的热复用(每次拖质量滑块 = 冷重建)。
    friend bool operator==(const RtxVideoParams &a, const RtxVideoParams &b) noexcept {
        return a.vsrMode == b.vsrMode && a.vsrScale == b.vsrScale &&
               a.vsrAutoHeight == b.vsrAutoHeight && a.hdrEnabled == b.hdrEnabled;
    }
    friend bool operator!=(const RtxVideoParams &a, const RtxVideoParams &b) noexcept {
        return !(a == b);
    }
};

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
    // 光流质量(0=无 → 静态零 guidance;两后端档位各自独立):
    //   motionVectorQuality:NVOF,1-5 = 上游 NvidiaOpticalFlowQuality 五档
    //   ffxQuality:FFX,1 = 性能(OF extent 半分辨率),2 = 质量(全分辨率)
    // 面板"光流质量"下拉按当前后端读写对应字段(单控件双值)。切换只重建
    // OF 会话(PoolHold 内,毫秒级),不动 NGX feature;会话建立失败优雅
    // 回退零 guidance。
    int motionVectorQuality = 0;  // NVOF 档位(0-5)
    int ffxQuality = 0;           // FFX 档位(0=无,1=性能,2=质量)
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
    // DLSS 帧生成倍数(2-6,live 参数):每源帧产出 M 帧(1 真实 + M-1
    // 插值),输出帧时长 = 源时长/M。源帧边界生效(当前源帧按触及时的 M
    // 走完),无重建/重启。fgEnabled=0 时忽略。
    int fgMultiplier = 2;
    // DLSS 帧生成路由(0=自动 预载 0.3.x hook 代理,1=纯官方 不预载;
    // 进程级,重启 mpv 生效):见 kFgRouteMin 注释。
    int fgRoute = 0;
    // 光流后端(0=ffx 1=nvof,创建时,下一帧生效):见 kOfBackendMin 注释。
    // 切换只影响下一次光流会话建立,不改 NGX feature。
    int ofBackend = 0;
    // 调试视图(0-2,live 参数,**不持久化**):1 = 输出替换为 |NR改动|×20
    // 的灰度图 —— 白 = 改动大,一片灰 = 模型没动画面(OptiScaler DLSSNR fork
    // 的 DebugView=3 同语义);2 = 输出替换为光流场可视化 —— 方向→色相、
    // 幅值→亮度,黑 = 无运动/无光流(回应"看不出参数有没有效果"与
    // "光流到底有没有在流")。面板"调试视图"下拉,仅当前会话有效。
    int debugView = 0;

    // ---- RTX Video(VSR / TrueHDR)----
    //   创建时(seek/reload 生效;面板改动经 reseek 触发链重建):rtxVsrMode
    //   (0=关 1=自动窗口适配 2=手动倍率)/ rtxVsrScale(1.0-4.0)/
    //   rtxHdrEnabled(输出切 P10 BT.2020 PQ)。rtxVsrAutoHeight 仅 mode=1
    //   时由 plugin.cpp 直读 mpv 窗口客户区填入,不持久化。
    //   live(逐帧 per-eval,拖动即时生效):rtxVsrStrength(1-4)与 HDR
    //   四参。
    int rtxVsrMode = 0;
    float rtxVsrScale = 2.0f;    // 1.0-4.0(官方单 pass 上限 4x)
    int rtxVsrAutoHeight = 2160; // probe 字段(mode=1),非持久化
    int rtxVsrStrength = 2;      // 1-4(0=bicubic 已移除,关闭走开关)
    int rtxHdrEnabled = 0;       // 输出域 SDR → HDR10(P10 PQ)
    int rtxHdrContrast = 100;      // 0-200
    int rtxHdrSaturation = 100;    // 0-200
    int rtxHdrMiddleGray = 50;     // 10-100
    int rtxHdrMaxLuminance = 1000; // 400-2000 nits
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

// 光流档位解析:按 ofBackend 取对应字段(NVOF→motionVectorQuality 0-5,
// FFX→ffxQuality 0-2)。单一下拉双字段的唯一裁决点 —— 冷初始化 4b、
// Rebind、ProcessFrame 逐帧同步三处共用,语义漂移即编译错误。
inline int ResolveOfQuality(const DlssnrParams &p) noexcept {
    return std::clamp(p.ofBackend == kOfBackendFfx ? p.ffxQuality : p.motionVectorQuality,
                      p.ofBackend == kOfBackendFfx ? kFfxQualityMin : kOfQualityMin,
                      p.ofBackend == kOfBackendFfx ? kFfxQualityMax : kOfQualityMax);
}
