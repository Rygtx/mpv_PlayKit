#pragma once
// Ported from Magpie experimental DLSSNRFilter.cpp / NgxD3D12Core.cpp.
// NGX static core (nvsdk_ngx_s.lib) owns the parameter block; the signed
// snippet nvngx_dlssnr.dll (Feature 18) performs CreateFeature/EvaluateFeature.
// Guidance:零 guidance(静态零纹理,等价 Magpie guidanceMode=1 Force Zero)
// 或 NVOF 真运动矢量(PORTING #6,NvofContext),由 motionVectorQuality 切换。

#include "d3d12_context.h"
#include "dlssfg_context.h"
#include "dlssnr_params.h"
#include "iat_hook.h"
#include "of_backend.h" // 光流后端接口(NvofContext/FxofContext 在 cpp 内具化)
#include "rtx_video_context.h"
#include "shared_params.h"
#include <atomic>
#include <cstdio>
#include <memory>
#include <vector>
#include <mutex>
#include <nvsdk_ngx.h>

namespace vsdlssnr {

// Panel toggle for the periodic perf log (dlssnr_timing.log)
void SetTimingLogEnabled(bool enabled) noexcept;

// Exported TimingLog wrapper for nvof_context.cpp(TimingLog 本体在匿名命名空间)。
void TimingStatusLine(const char *line) noexcept;

// 逐帧级探针开关(VSDLSSNR_PROBE=1);d3d12_context 的 init 细分探针共用。
bool ProbeEnabled() noexcept;

class DlssnrContext {
public:
    DlssnrContext() = default;
    ~DlssnrContext();
    DlssnrContext(const DlssnrContext &) = delete;
    DlssnrContext &operator=(const DlssnrContext &) = delete;

    bool Initialize(D3D12Context &d3d12, const wchar_t *ngxDllPath,
                    const wchar_t *fgDllPath,
                    int width, int height, int depth, SharedParams *shared,
                    const RtxVideoParams &rtx,
                    int subW = 1, int subH = 1, bool rgb = false,
                    char *err = nullptr, size_t errLen = 0) noexcept;
    void Shutdown() noexcept;

    // Hot-context rebind: attach this kept-warm context (device, NGX feature,
    // slot pool all alive) to a new filter instance's SharedParams — including
    // one for a different video size or bit depth (frame resources + feature
    // rebuild under the pool seal; only a changed snippet DLL forces a full
    // re-init at the caller). The feature is rebuilt only when a create-time
    // parameter (preset / input_resolution / scaling_enabled) or the
    // size/depth actually differs; otherwise it is free. Returns false (and
    // leaves _ready false) when the rebuild fails; the caller then falls back
    // to a full Initialize.
    bool Rebind(SharedParams *shared, int width, int height, int depth,
                const RtxVideoParams &rtx, char *err, size_t errLen) noexcept;

    // Preset / internal-resolution / scaling-toggle are create-time NGX keys:
    // on panel change the frame thread rebuilds the feature (and scaling
    // textures for resolution changes; disabled = residual pipeline dropped).
    // newWidth/newHeight/depth >= 0 additionally rebuild the per-slot frame
    // resources for that geometry (used by Rebind across resolutions/depth).
    bool RecreateFeature(int preset, int resPercent, int scalingEnabled, char *err, size_t errLen,
                         int newWidth = -1, int newHeight = -1, int newDepth = -1) noexcept;

    // 光流会话重建(quality / of_backend / 会话输入尺寸变化)。只重建光流
    // 会话(PoolHold 内,毫秒级),NGX feature 不动。quality == 0 时停用会话
    // 回退零 guidance;会话建立失败时优雅降级(记档位防逐帧重试风暴)。
    // quality = 调用方按 ofBackend 预解析的档位(NVOF→motionVectorQuality,
    // FFX→ffxQuality)。内部自取 PoolHold:调用方必须尚未持有槽位,且不得
    // 已在 PoolHold 之中。
    bool RebuildOf(int quality, int dstW, int dstH, char *err, size_t errLen) noexcept;
    // 光流历史失效(seek = 新时间线)。热 Rebind 上调用;下一帧重新播种。
    void ResetNvofHistory() noexcept;

    // YUV420P8/P10 三平面进 → 处理 → 三平面出。输出平面在 OUT 尺寸
    // (RTX VSR 开 = 目标尺寸,调用方按 OUT 建帧/传平面;关 = 源尺寸),
    // 位深 = HDR ? 10 : 源位深(HDR 输出 = BT.2020 PQ limited P10)。
    // matrix/range 来自源帧属性(props _Matrix/_ColorRange,plugin.cpp 读;
    // 缺省 709 limited)—— YUV↔RGB GPU 转换按此展开/压缩。
    // n is the frame index; discontinuity detection (NGX history reset) is
    // owned here, not by the glue layer. timingOut 非 NULL 时写入分段耗时
    // (毫秒,逗号分隔:pack,submit+gpu,unpack)
    // FG 多帧输出(fgDst* 非 NULL = 创建时 FG 激活):fgMultiplier = 本帧
    // 倍数 M(2-6,调用方从参数快照取 —— 与输出帧数契约绑定,必须由调用
    // 方定格),每源帧产出 1 真实 + M-1 插值帧。fgDst*/fgStrides 为扁平
    // 数组 [gen][plane](gen 0..M-2,元素 = gen*3+plane);fgGenOk 出参逐
    // 槽告知真插值/false = 复制真实帧(复位/零光流/面板关/eval 降级)。
    bool ProcessFrame(const uint8_t *const *srcPlanes, const int64_t *srcStrides,
                      uint8_t **dstPlanes, int64_t *dstStrides,
                      int fgMultiplier,
                      uint8_t **fgDstPlanes, int64_t *fgDstStrides, bool *fgGenOk,
                      int width, int height, int n,
                      ColorMatrix matrix, ColorRange range,
                      char *err, size_t errLen,
                      char *timingOut = nullptr, size_t timingLen = 0) noexcept;

    // ---- RTX Video(VSR / TrueHDR)会话事实(create-time 定格)----
    // 输出几何的单一权威(plugin.cpp 建 vi/输出帧、D3D12Context 建平面、
    // ProcessFrame 校验全部经这里)。
    int OutWidth() const noexcept { return _outW; }
    int OutHeight() const noexcept { return _outH; }
    bool HdrActive() const noexcept { return _hdrActive; }
    // RTX 段是否在管线中(vsr 或 hdr 任一存活;FG backbuffer 形态随之)。
    bool RtxActive() const noexcept { return _rtxActive; }

    // FG 会话是否激活(创建时 fgEnabled 且官方链初始化成功且槽资源在)。
    // 决定滤镜输出帧率是否 ×2(插件 create 侧)。
    bool FgActive() const noexcept {
        return _fg && _fg->Enabled();
    }

private:
    // ---- SEH wrappers (Magpie style; every NGX call is wrapped) ----
    NVSDK_NGX_Result CoreInitSafely(const wchar_t *appDir, ID3D12Device *device,
                                    const NVSDK_NGX_FeatureCommonInfo *info, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result CoreAllocateParametersSafely(NVSDK_NGX_Parameter **out, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result CoreGetCapabilityParametersSafely(NVSDK_NGX_Parameter **out, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result CoreDestroyParametersSafely(NVSDK_NGX_Parameter *p, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result CoreShutdownSafely(ID3D12Device *device, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result SnippetInitSafely(const wchar_t *appDataPath, ID3D12Device *device, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result SnippetCreateFeatureSafely(ID3D12GraphicsCommandList *cl, NVSDK_NGX_Parameter *params, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result SnippetEvaluateSafely(ID3D12GraphicsCommandList *cl, NVSDK_NGX_Parameter *params, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result SnippetReleaseSafely(DWORD *sehCode) noexcept;
    NVSDK_NGX_Result SnippetShutdownSafely(DWORD *sehCode) noexcept;
    void SetCreateParametersUnsafe() noexcept;
    bool SetCreateParametersSafely(DWORD *sehCode) noexcept;
    void SetEvaluateParametersUnsafe(FrameSlot &slot, bool resetHistory, bool realMotion) noexcept;
    bool SetEvaluateParametersSafely(FrameSlot &slot, bool resetHistory, bool realMotion, DWORD *sehCode) noexcept;

    // 死亡状态边缘发布(state = "passthrough" | "ngx_faulted",detail 为原因
    // 串,内部消毒)。发布即替换共享内存里的旧统计 body —— 帧已不再成功,
    // tick 停摆,不替换面板就会一直显示冻结的"NGX 延迟"。
    void PublishDeadState(const char *state, const char *detail) noexcept;
    // 光流后端构造(of_backend 单一后端,默认 FFX;失败不跨后端回落 ——
    // 对齐 fg_route 先例:行为可预测)。
    // q > 0;err 带最后一个失败原因(冷初始化 4b 与 RebuildOf 共用)。
    std::unique_ptr<IOpticalFlowBackend> CreateOfBackend(int q, int dstW, int dstH,
                                                         char *err, size_t errLen) noexcept;
    // OF 实际模式串(SK_OF_MODE):档位关闭 = "off",会话死亡 = "zero",
    // 存活 = backend 能力段(NvofContext "both+cost q2 grid4" /
    // FxofContext "fxof q3 qual 1920x1080")
    // —— 能力在同一块 GPU 上不随档位变化,档位才是切档可见的反馈。
    // tick 与 Initialize 的 stats 发布共用。
    char _ofModeBuf[40] = "";
    const char *OfModeString() noexcept {
        if (_curOfQuality <= 0) return "off";
        if (!_ofBackend || !_ofBackend->Enabled()) return "zero";
        return _ofBackend->ModeString(_ofModeBuf, sizeof(_ofModeBuf));
    }

    // ---- FG 会话级事实(stats 通道 SK_FG_ROUTE_EFFECTIVE / SK_FG_MULT_CREATE
    // / SK_FG_DETAIL 的数据源;"auto 档到底走了谁 / 为什么没插帧"不再翻
    // timing log)----
    // 实际生效路由:off(FG 未请求)/ official-hook(官方链,0.3.x hook
    // 代理接管)/ official(官方链直连)/ copy(请求了但初始化失败 → 复制
    // 帧)。Initialize 的 FG 段一次性定值,此后只读(路由进程级,会话内
    // 不变;运行期 eval 失败闩停由 SK_FG=unavailable 表达,路由值保留
    // "最后是谁在跑"的的事实)。
    char _fgRouteEff[16] = "off";
    // 最近一次 FG 初始化失败原因(消毒串;成功路径清空)。失败通常发生在
    // 会话创建期,不会有逐帧更新 —— 常规 tick 恒定携带,死亡态 body 缺键
    // 时面板自行清空。
    char _fgDetail[128] = "";
    // 创建倍数 M0(plugin.cpp 侧 vi.fps/帧数契约的同一值;FG 未激活 = 0)。
    // live 倍数 > M0 的面板档位本会话无槽可填 —— 面板据此红显"需重建"。
    int _fgCreateMult = 0;
    // 自动档 hook 代理预载失败原因(非空 = 预载没进托)。官方链失败时并
    // 入 _fgDetail(面板直读真因,而非误导性的 "DLSSG unavailable");
    // 官方链成功则保持无消费( FG 能跑,预载失败无实际影响)。
    char _fgProxyNote[96] = "";
    // 光流会话创建失败原因(消毒串;of_detail 键的数据源,与 _fgDetail
    // 同语义 —— "降级为零 guidance"的面板侧"为什么")。
    char _ofDetail[96] = "";
    // 超限槽降级复制的日志闩锁:gate 回落 2x 时每源帧都有槽 eval 失败,
    // 无闩锁 = 24fps 源每秒 24 行 timing log,根因行被淹没(plugin.cpp 的
    // failureLogged 同款惯例)。eval 恢复成功即解除,新故障重新记一条。
    std::atomic<bool> _fgDupLogged{false};

    D3D12Context *_d3d12 = nullptr;
    NVSDK_NGX_Parameter *_parameters = nullptr;
    NVSDK_NGX_Handle *_feature = nullptr;
    HMODULE _snippetModule = nullptr;
    SnippetCallerHook _hook{};
    SharedParams *_shared = nullptr;

    using SnippetInitExtFn = NVSDK_NGX_Result(NVSDK_CONV *)(
        unsigned long long, const wchar_t *, ID3D12Device *, NVSDK_NGX_Version, const NVSDK_NGX_Parameter *);
    using CreateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV *)(
        ID3D12GraphicsCommandList *, NVSDK_NGX_Feature, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
    using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV *)(
        ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback);
    using ReleaseFeatureFn = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Handle *);
    using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12Device *);

    SnippetInitExtFn _snippetInitExt = nullptr;
    CreateFeatureFn _snippetCreateFeature = nullptr;
    EvaluateFeatureFn _snippetEvaluateFeature = nullptr;
    ReleaseFeatureFn _snippetReleaseFeature = nullptr;
    ShutdownFn _snippetShutdown = nullptr;

    wchar_t _appDataPath[MAX_PATH]{};
    int _width = 0;
    int _height = 0;
    int _depth = 0; // YUV 位深(8/10/12/14/16;CreateFrameResources/resize 判据/日志)
    int _subW = 1;  // 输入色度抽取档(1=半,0=全):420=(1,1) 422=(1,0) 444=(0,0)
    int _subH = 1;
    bool _isRgb = false; // VS RGBP 直读
    // Adapter description in UTF-8, filled once in Initialize and reused by
    // every per-frame stats publish (GetDesc per frame is wasted work).
    char _gpuNameUtf8[160] = "UNAVAILABLE";
    bool _coreInitialized = false;
    bool _snippetInitialized = false;
    // Cross-thread: written by one frame thread (device-lost latch /
    // failed rebuild) while others read it under fmParallel.
    std::atomic<bool> _ready{false};
    // create-time parameters currently baked into the NGX feature (Rebind
    // compares against these to skip a no-op RecreateFeature)
    int _curPreset = -1;
    int _curRes = -1;
    bool _curScaling = false;
    // 光流会话(of_backend 选择后端:kOfBackendNvof/Ffx)。
    // _curOfQuality = 当前生效档位(0 = 零 guidance;语义随后端 —— NVOF
    // 1-5 / FFX 1=性能 2=质量,取自已激活后端对应字段);_nvofFailed =
    // 会话建立失败或连续失败停用(回退零 guidance,Rebind/换档时重试)。
    // _nvofMutex 串行化 RebuildOf:fmParallel 下多个帧线程会同时看到同一
    // 档位变化,不加锁会并发重建互相踩踏。
    std::unique_ptr<IOpticalFlowBackend> _ofBackend;
    // 退役会话:销毁不安全(NVOF 引擎 destroy 实测崩溃;FFX 首期同策略,
    // spike 验证 ffxOpticalflowContextDestroy 后再放开)的旧会话转入此名单
    // 存活到进程退出(热上下文哲学;每会话约 2×W×H×4B 显存,FFX 另含内部
    // 金字塔资源)。
    std::vector<std::unique_ptr<IOpticalFlowBackend>> _retiredOf;
    int _curOfQuality = 0;
    int _curOfBackend = 0; // 当前会话按 of_backend 参数构造时的请求值
    bool _nvofFailed = false;
    std::mutex _nvofMutex;
    // DLSS FG(挂 NR 之后):创建时 fgEnabled → 建 proxy 会话 + FG 槽资源
    // (_fgRequested 参与 CreateFrameResources 旗标,sticky);初始化失败 =
    // _fg 空,滤镜优雅回退 1:1 输出。面板 fgEnabled 为 live 语义(会话内
    // 关 = 复制真实帧;未激活会话内开 = 下次播放生效)。
    std::unique_ptr<DlssfgContext> _fg;
    NVSDK_NGX_Parameter *_fgParams = nullptr; // FG 专用核心参数块(core 拥有)
    bool _fgRequested = false;
    // 实验性补帧 HDR 域插帧(创建时定格):TrueHDR 前置 + DLSSG 吃其 FP16
    // **PQ 码域** backbuffer(HdrToPq 编码 pass,ColorBuffersHDR=0 —— 直吃
    // scRGB 线性 >1.0 会被 DLSSG HDR 路径钳在 ~0.875 = 插值帧高光塌陷,
    // 2026-09-24 值域定案;感知码域走 LDR 路径无损)。默认 0(SDR 域插帧 +
    // 逐帧 TrueHDR)。参与槽资源/FG create 格式形态。
    bool _fgHdrInterp = false;
    // RTX Video(VSR→TrueHDR,均挂 NR 之后、FG 之前):创建时参数快照
    // (_rtx;vpy/[rtxvideo] ini,面板 payload 不携带)。VSR/HDR feature
    // 与尺寸无关,跨 seek/分辨率热复用;SEH 本地闩锁(NR 不连坐)。
    // 失败降级:capability 不过 = 资源就不建(直通尺寸);CreateFeature
    // 在 capability 过后仍失败 = 初始化整体失败(插件回落纯直通)。
    RtxVideoParams _rtx{};
    std::unique_ptr<RtxVsrContext> _vsr;
    std::unique_ptr<RtxHdrContext> _hdr;
    NVSDK_NGX_Parameter *_vsrParams = nullptr; // 各自的 capability 块(core 拥有)
    NVSDK_NGX_Parameter *_hdrParams = nullptr;
    bool _vsrRequested = false; // VSR 在管线(模式开 + 倍率 > 1 + capability 过)
    bool _hdrActive = false;    // TrueHDR 在管线(创建时定格;输出 P10)
    bool _rtxActive = false;    // _vsrRequested || _hdrActive(输出几何判据)
    // RTX 最近一次初始化失败原因(消毒串;成功路径清空;_rtxDetail 键的
    // 数据源,与 _fgDetail/_ofDetail 同语义)。请求开但实态 off = 降级,
    // 面板诊断页红显的原因串。
    char _rtxDetail[96] = "";
    // RTX 会话实态串("vsr WxH" / "off" 等;init 时定格,_rtx 键的数据源)。
    // 曾只在 init 体发布,每帧体覆盖后键丢失 → 面板诊断恒 "(未加载)"
    // (2026-09-22 实锤)。每帧体必须带上,此串由 init 填好后逐帧复用。
    char _rtxStateStr[96] = "";
    int _pipeW = 0;             // VSR 输出 / TrueHDR / FG backbuffer 尺寸
    int _pipeH = 0;
    int _outW = 0;              // YUV 输出平面尺寸(vsr 关 = 源)
    int _outH = 0;
    // fmParallel: several frame threads call EvaluateFeature concurrently.
    // The feature and the parameter block are singletons, so evaluate
    // (parameter setup + snippet call) is serialized; GPU-side dispatches
    // still overlap via each slot's own command list.
    std::mutex _evaluateMutex;
    // 面板可见的死亡状态发布(passthrough / ngx_faulted):边缘触发,每状态
    // 每实例一次。存活态(ok / nvof_zero)由周期 stats tick 携带,不走这里。
    // 0 = passthrough,1 = ngx_faulted,-1 = 尚未发布过。
    std::atomic<int> _lastDeadState{ -1 };
    // 最近一次进入 ProcessFrame 的帧号(recreate/错误 STATUS 行带上它,
    // 用户"第几秒看到异常"即可与 timing log 的帧号对上)。
    std::atomic<int> _lastFrameN{ -1 };
};

} // namespace vsdlssnr
