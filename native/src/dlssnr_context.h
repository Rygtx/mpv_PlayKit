#pragma once
// Ported from Magpie experimental DLSSNRFilter.cpp / NgxD3D12Core.cpp.
// NGX static core (nvsdk_ngx_s.lib) owns the parameter block; the signed
// snippet nvngx_dlssnr.dll (Feature 18) performs CreateFeature/EvaluateFeature.
// Guidance:零 guidance(静态零纹理,等价 Magpie guidanceMode=1 Force Zero)
// 或真运动矢量(NVIDIA NVOF / AMD FFX,由 of_backend 选型;见 of_backend.h),
// 由 motionVectorQuality / ffxQuality 切换。

#include "d3d12_context.h"
#include "frame_rate_meter.h"
#include "dlssfg_context.h"
#include "dlssnr_params.h"
#include "iat_hook.h"
#include "of_backend.h" // 光流后端接口(NvofContext/FxofContext 在 cpp 内具化)
#include "panel_ipc.h"  // StatsPayload(FillStatsCommon 签名;无环)
#include "rtx_video_context.h"
#include "shared_params.h"
#include <atomic>
#include <cstdio>
#include <memory>
#include <vector>
#include <mutex>
#include <nvsdk_ngx.h>

namespace vsdlssnr {

// 管线色状态账本(调试观测;零 GPU 行为变更):帧路径对管线色家族
// (outputColor/inputColor/temporalOut/vsrColor/fgBack/hdrColor/hdrFg[])的
// stateBefore 全靠手工簿记 —— 生产/消费/归位三处各自心算,历次 from-state
// 事故的根源(2026-09-25 审查类)。账本在 NGX 衰减点与消费点登记事实;
// Expect 不符仅留痕(VSDLSSNR_PROBE=1 时 TimingStatusLine),不参与任何
// 屏障决策 —— 错配当场暴露,不再等画面撕裂或 debug layer 抱怨。
// 条目 = {资源指针, 最近状态}。并发帧安全性:只登记槽资源,槽池排他下
// 并发帧的指针集互斥,数组无竞争;context 级共享资源不入账。
class PipeLedger {
public:
    void Set(ID3D12Resource *res, D3D12_RESOURCE_STATES st) noexcept {
        for (int i = 0; i < count_; ++i) {
            if (entries_[i].res == res) { entries_[i].state = st; return; }
        }
        if (count_ < kCap) entries_[count_++] = {res, st};
    }
    // 消费点断言:不符仅留痕(首见资源以期望值起账)。
    void Expect(ID3D12Resource *res, D3D12_RESOURCE_STATES expected,
                const char *where) noexcept;
private:
    struct Entry { ID3D12Resource *res; D3D12_RESOURCE_STATES state; };
    static constexpr int kCap = 24;
    Entry entries_[kCap]{};
    int count_ = 0;
};


// ProcessFrame 拆分执行的续体(实现在 cpp;FG 持锁窗口缩小用):Submit 半段
// 把等待/unpack/stats 所需的全部状态打包于此,调用方释放串行锁后交还
// ProcessFrameFinish。不透明指针,调用方不得解引用。
struct FrameFinish;

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
    // slot pool all alive) to a new filter instance's SharedParams. A changed
    // video size/depth rebuilds frame resources + feature inside one
    // pool-sealed RecreateFeature pass; a changed RTX/FG session shape
    // (vsr_mode/scale/hdr_enabled/fg_enabled/fg_hdr_interp) likewise hot-
    // rebuilds via RecreateFeature's shape segment (2026-09-27 解耦收尾:
    // hotMatch 只锁 snippet 路径/尺寸/深度/布局/代理路径,形态变化不再付
    // 冷启动). Returns false (and leaves _ready false) when the rebuild
    // fails; the caller then falls back to a full Initialize.
    bool Rebind(SharedParams *shared, int width, int height, int depth,
                const RtxVideoParams &rtx, char *err, size_t errLen) noexcept;

    // RecreateFeature 请求(create 键恒提供;其余 = "提供才参与" 的显式
    // 旗标,替代原 -1/nullptr 哨兵位 —— 哪个字段触发哪段由此自明,新增
    // 维度加字段而非加哨兵参数)。
    struct RecreateRequest {
        int preset = 0;                 // create 键(preset / 内部分辨率 / scaling)
        int resPercent = 100;
        bool scalingEnabled = false;
        bool dims = false;              // 提供时 = 重建槽资源(newWidth/Height/Depth)
        int newWidth = 0, newHeight = 0, newDepth = 0;
        bool shape = false;             // 提供时 = 形态段(RTX/FG 会话热重建)
        RtxVideoParams rtx{};           // shape 段的新 RTX 几何(DecideRtxGeometry 入参)
        // shape 段内的 FG 请求态(2026-10-04:hasFg/hasFgHdr 旗标已删 ——
        // 全部调用点恒等于 shape,"提供才参与"零兑现,纯双账)。bridge 的
        // preset 重建路径 shape=false,两键整体跳过。
        bool fgRequested = false;
        bool fgHdr = false;
    };
    // Preset / internal-resolution / scaling-toggle are create-time NGX keys:
    // on panel change the frame thread rebuilds the feature (and scaling
    // textures for resolution changes; disabled = residual pipeline dropped).
    // dims/shape 提供时驱动:槽资源按新几何重建(Rebind 跨分辨率/位深);
    // 形态段 = capability 补查 + VSR/HDR 上下文建退 + FG 会话建/重建(降级
    // 不整体失败)。bridge 的 preset 重建路径只填 create 键,形态段整体跳过。
    bool RecreateFeature(const RecreateRequest &req, char *err, size_t errLen) noexcept;

    // 光流会话重建(quality / of_backend / 会话输入尺寸变化)。只重建光流
    // 会话(PoolHold 内,毫秒级),NGX feature 不动。quality == 0 时停用会话
    // 回退零 guidance;会话建立失败时优雅降级(记档位防逐帧重试风暴)。
    // quality = 调用方按 ofBackend 预解析的档位(NVOF→motionVectorQuality,
    // FFX→ffxQuality)。内部自取 PoolHold:调用方必须尚未持有槽位,且不得
    // 已在 PoolHold 之中。
    bool RebuildOf(int quality, int backendReq, int dstW, int dstH, char *err, size_t errLen) noexcept;
    // 光流会话同步单一裁决点(2026-09-25 收敛):follow 判定/尺寸换算/档位
    // 解析/触发条件/重建调用在此。allowRetry = 会话级边界(rebind/recreate)
    // 才置 true —— 帧路径不带 _nvofFailed 重试项,否则失败会话每帧触发
    // RebuildOf 封池风暴(该不对称是防风暴关键,不是漂移)。
    bool SyncOfSession(const DlssnrParams &p, int srcW, int srcH, bool allowRetry,
                       char *err, size_t errLen) noexcept;
    // follow 判定单一谓词(SyncOfSession 裁决点与 RecreateFeature 形态段
    // 共用;此前形态段内联版漏 fgEnabled 项 —— FG on→off seek + follow 开
    // 时按"FG 占用"在源尺寸建会话,Rebind 尾 SyncOfSession 按"FG 已关"
    // 判 stale 再建一次,刚建的会话直接退役,显存白付一份,2026-10-04
    // 收拢)。scalingActive 由调用方给语义:裁决点 = 实态 HasScaling();
    // 形态段 = 本次重建的请求态(scaling 纹理尚未重建)。
    bool OfFollowDesired(const DlssnrParams &p, bool scalingActive) const noexcept;
    // 光流历史失效(seek = 新时间线)。热 Rebind 上调用;下一帧重新播种。
    void ResetNvofHistory() noexcept;

    // YUV420P8/P10 三平面进 → 处理 → 三平面出。输出平面在 OUT 尺寸
    // (RTX VSR 开 = 目标尺寸,调用方按 OUT 建帧/传平面;关 = 源尺寸),
    // 位深 = HDR ? 10 : 源位深(HDR 输出 = BT.2020 PQ limited P10)。
    // matrix/range 来自源帧属性(props _Matrix/_ColorRange,plugin.cpp 读;
    // 缺省 709 limited)—— YUV↔RGB GPU 转换按此展开/压缩。
    // n is the frame index; discontinuity detection (NGX history reset) is
    // owned here, not by the glue layer. timingOut 非 NULL 时写入分段耗时
    // (毫秒,逗号分隔七段:pack,of,eval_cpu,gpu,fg,conv,unpack)
    // FG 多帧输出(fgDst* 非 NULL = 创建时 FG 激活):fgMultiplier = 本帧
    // 倍数 M(2-6,调用方从参数快照取 —— 与输出帧数契约绑定,必须由调用
    // 方定格),每源帧产出 1 真实 + M-1 插值帧。fgDst*/fgStrides 为扁平
    // 数组 [gen][plane](gen 0..M-2,元素 = gen*3+plane);fgGenOk 出参逐
    // 槽告知真插值/false = 复制真实帧(复位/零光流/面板关/eval 降级)。
    // deferOut 非 NULL = 拆分模式:执行到 SubmitPostFrame 为止,把继续执行
    // (等待+unpack+stats)打包进 *deferOut 后返回 true;调用方释放其串行锁
    // 后必须对同一帧调用恰好一次 ProcessFrameFinish。Submit 半段任何失败 =
    // 返回 false 且 *deferOut = NULL(槽已释放,无后续)。deferOut = NULL =
    // 原同步语义(单方法完成全部,非 FG 路径照旧)。
    bool ProcessFrame(const uint8_t *const *srcPlanes, const int64_t *srcStrides,
                      uint8_t **dstPlanes, int64_t *dstStrides,
                      int fgMultiplier,
                      uint8_t **fgDstPlanes, int64_t *fgDstStrides, bool *fgGenOk,
                      int width, int height, int n,
                      ColorMatrix matrix, ColorRange range,
                      char *err, size_t errLen,
                      char *timingOut = nullptr, size_t timingLen = 0,
                      FrameFinish **deferOut = nullptr) noexcept;
    // 拆分模式后半:全部栅栏有界等待 + unpack(真实帧 + 逐 gen)+ dump/stats
    // 段 + 槽释放(含 OF 排空)。成功 = 帧内容就绪;失败 = 调用方对本帧输出
    // 做源拷贝兜底(帧已入缓存、可能已被消费方领引用,尚未交付)。fgGenOk
    // 与 ProcessFrame 传入同一数组(Submit 半段填写,Finish 半段的 unpack
    // 循环消费,调用方在其后读取)。续体所有权归 Finish:任何出口(含失败
    // 提前 return)都由 Finish 释放,调用方不得再触碰(2026-09-25 契约)。
    bool ProcessFrameFinish(FrameFinish *defer,
                            uint8_t **dstPlanes, int64_t *dstStrides,
                            uint8_t **fgDstPlanes, int64_t *fgDstStrides,
                            bool *fgGenOk,
                            char *err, size_t errLen,
                            char *timingOut, size_t timingLen) noexcept;

    // ---- RTX Video(VSR / TrueHDR)会话事实(create-time 定格)----
    // 输出几何的单一权威(plugin.cpp 建 vi/输出帧、D3D12Context 建平面、
    // ProcessFrame 校验全部经这里)。
    int OutWidth() const noexcept { return _outW; }
    int OutHeight() const noexcept { return _outH; }
    bool HdrActive() const noexcept { return _hdrActive; }
    // RTX 段是否在管线中(vsr 或 hdr 任一存活;FG backbuffer 形态随之)。
    bool RtxActive() const noexcept { return _rtxActive; }

    // FG 会话是否激活(请求态 fgEnabled 且官方链初始化成功且槽资源在)。
    // 决定滤镜输出帧率是否 ×2(插件 create 侧)。请求感知:live 关(会话内
    // 面板切 0)不动 _fgRequested → 输出保持 ×M0 契约、槽位回落真实帧;
    // seek 携带 fg=0 → Rebind 形态段清 _fgRequested → 1:1 输出(= 昔日
    // 冷重建行为)。
    bool FgActive() const noexcept {
        return _fgRequested && _fg && _fg->Enabled();
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
    // NGX tuning 键缓存(脏检查;仅 _evaluateMutex 内消费,无并发)。feature
    // 重建置 _lastEvalTuningValid = false 强制首帧重写。
    struct EvalTuning {
        int style;
        float intensity, localTone, localStructure, skin;
        int autoMask;
        bool operator!=(const EvalTuning &o) const noexcept {
            return style != o.style || intensity != o.intensity ||
                   localTone != o.localTone || localStructure != o.localStructure ||
                   skin != o.skin || autoMask != o.autoMask;
        }
    };
    // 参数快照由调用方传入(ProcessFrame 的 frameParams,单一快照语义):
    // 此前在 _evaluateMutex 临界区内再取一次锁+拷贝,且与帧内其它键不同源。
    void SetEvaluateParametersUnsafe(FrameSlot &slot, bool resetHistory, bool realMotion,
                                     const DlssnrParams &params) noexcept;
    bool SetEvaluateParametersSafely(FrameSlot &slot, bool resetHistory, bool realMotion,
                                     const DlssnrParams &params, DWORD *sehCode) noexcept;
    EvalTuning _lastEvalTuning{};
    bool _lastEvalTuningValid = false; // feature 重建时置 false(SetCreateParametersUnsafe 尾)

    // 死亡状态边缘发布(state = "passthrough" | "ngx_faulted",detail 为原因
    // 串,内部消毒)。发布即替换共享内存里的旧统计 body —— 帧已不再成功,
    // tick 停摆,不替换面板就会一直显示冻结的"NGX 延迟"。
    void PublishDeadState(const char *state, const char *detail) noexcept;
    // 帧诊断 dump(VSDLSSNR_DUMP;原内联在 Finish 中段的旁路,迁出):
    // 锁存一次性,颜色/motion 分开,ctl 锁内逐纹理落盘。
    void DumpFrameDiagnostics(FrameFinish &ff) noexcept;
    // StatsPayload 公共体(init 发布与逐帧发布共用的 11 个字段;原两份
    // 手抄清单靠人肉对齐)。帧态字段(pipeline 用时/fgState/slotWait 等)
    // 由调用方在公共体之外填充 —— init 体本就不该携带它们。
    void FillStatsCommon(StatsPayload &st) noexcept;
    // RTX feature 降级收口(capability 不过 / CreateFeature 失败共用):清对应
    // 旗标 + VSR 失败时几何回落源尺寸 + _rtxActive 重算 + _rtxDetail 记因。
    // 必须在 CreateFrameResources 之前调用 —— 资源随后按降级形态建,管线
    // 自洽,NR/FG 不连坐。上下文对象直接 reset(析构 no-op 不重入 NGX,
    // 已建的 feature handle 留在 core 内随进程回收)。
    void DegradeRtxFeature(bool vsr, const char *why) noexcept;
    // RTX capability 参数块获取(RecreateFeature 形态段 vsr/hdr 两处同构
    // 补查的收敛;2b 预检的可用性语义在 Rtx*Context::Initialize 内复读):
    // 取块失败留痕并返回 false,成功落 paramsOut(不可用也留块,Shutdown
    // 不触碰)。what = 日志标签("vsr"/"hdr")。
    bool FetchRtxCapabilityParams(NVSDK_NGX_Parameter **paramsOut, const char *what) noexcept;
    // 抗闪烁时域重建三连调用点(冷初始化 / resize 重建 / live 切换)的收敛:
    // 成功 = 建账 _curAntiFlicker=af;失败 = 降级 0 + failed 发布(state 3/
    // route 0/weight 0)+ 降级日志。why 进日志(如 "init"/"resize rebuild")。
    // 调用点各自保留成功路径的收尾差异(_tValid 复位与 running 发布节奏)。
    bool RebuildTemporalOrDegrade(int af, const char *why) noexcept;
    // RTX 几何单一裁决(Initialize 与 RecreateFeature 形态段共用):dstW/dstH
    // 换算(mode=1 autoHeight / mode=2 scale)、ratio 旁路、pipe=min(目标,
    // 源×4)、偶尺寸收口。写 _vsrRequested/_hdrActive/_rtxActive 与
    // _pipeW/_pipeH/_outW/_outH。第二份几何换算在构造上不可能。
    void DecideRtxGeometry(const RtxVideoParams &rtx, int srcW, int srcH) noexcept;
    // RTX 会话实态串刷新(_rtxStateStr:StatsPayload.rtx 键数据源;init 与形态重建
    // 共用)。逐帧 tick 复用成员,刷新即面板可见。
    void RefreshRtxStateString() noexcept;
    // FG 会话建立(冷初始化 4a 与 Rebind 形态重建共用;_fgRequested 已由
    // 调用方定格):路由日志/官方 dll 解析(_appDataPath 同目录
    // nvngx_dlssg.dll)/capability 块/DlssfgContext::Initialize/失败记因
    // (_fgDetail + 预载归因并入)一条龙。成功置 _fgRouteEff=
    // official-hook|official;失败清 _fgRequested(槽资源已带 FG 纹理时
    // 无害留用)。返回会话是否可用。
    bool SetupFgSession(const DlssnrParams &p) noexcept;
    // 光流后端构造(of_backend 单一后端,默认 FFX;失败不跨后端回落 ——
    // 对齐 fg_route 先例:行为可预测)。
    // q > 0;err 带最后一个失败原因(冷初始化 4b 与 RebuildOf 共用)。
    // backendReq = 调用方快照的请求后端(单一快照纪律,2026-10-04 起
    // 不再函数内自取)。
    std::unique_ptr<IOpticalFlowBackend> CreateOfBackend(int q, int backendReq,
                                                         int dstW, int dstH,
                                                         char *err, size_t errLen) noexcept;
    // OF 实际模式串(StatsPayload.ofMode):档位关闭 = "off",会话死亡 = "zero",
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

    // ---- FG 会话级事实(stats 通道 StatsPayload.fgRouteEff / StatsPayload.fgMultCreate
    // / StatsPayload.fgDetail 的数据源;"auto 档到底走了谁 / 为什么没插帧"不再翻
    // timing log)----
    // 实际生效路由:off(FG 未请求)/ official-hook(官方链,0.3.x hook
    // 代理接管)/ official(官方链直连)/ copy(请求了但初始化失败 → 复制
    // 帧)。Initialize 的 FG 段一次性定值,此后只读(路由进程级,会话内
    // 不变;运行期 eval 失败闩停由 StatsPayload.fgState=unavailable 表达,路由值保留
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
    // 管线色状态账本(见上方 PipeLedger;调试观测,零行为变更)。
    PipeLedger _pipeLedger;
    // 帧率环形计数(纯 QPC 数学;原住 D3D12Context,2026-10-03 迁出 ——
    // 与设备无关的统计不该住在设备上下文里)。
    FrameRateMeter _fpsMeter;
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
    // Loaded model dll filename in UTF-8 (Initialize 填一次,per-frame stats
    // 复用):面板诊断页显示实际用的是原版还是哪档社区改版。
    char _modelDllUtf8[64] = "";
    bool _coreInitialized = false;
    bool _snippetInitialized = false;
    // Cross-thread: written by one frame thread (device-lost latch /
    // failed rebuild) while others read it under fmParallel.
    std::atomic<bool> _ready{false};
    // 最近一次成功建(重)模时的完整参数快照:Initialize 落账、
    // RecreateFeature 尾部覆盖(三个调用点共享该收口点)。Rebind 的
    // "要不要重建"比对(CreateParamsChanged)与 RecreateFeature 的
    // nrKeysChanged 都对真身比较 —— create 三元组规则只在
    // dlssnr_params.h 一处(2026-10-04:三字段 _cur* 副本已删,双账
    // 必漂移)。CreateParamsChanged 只比较 create 键;其余字段顺带存档,
    // 不参与裁决。
    DlssnrParams _appliedCreate{};
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
    // 跨帧线程读写的会话事实(fmParallel):写侧恒在 PoolHold/_nvofMutex
    // 封池窗口,读侧(SyncOfSession 每帧 stale 检查、ProcessFrame、stats
    // 发布)在窗口外无锁 —— atomic 化对齐 _tPubState 惯例(2026-10-04,
    // 此前普通成员按设计就数据竞争)。
    std::atomic<int> _curOfQuality{ 0 };
    std::atomic<int> _curOfBackend{ 0 }; // 最近已处理的请求后端(请求级,见 RebuildOf 注)
    std::atomic<bool> _nvofFailed{ false };
    std::mutex _nvofMutex;
    // DLSS FG(挂 NR 之后):创建时 fgEnabled → 建 proxy 会话 + FG 槽资源
    // (_fgRequested 参与 CreateFrameResources 旗标,sticky);初始化失败 =
    // _fg 空,滤镜优雅回退 1:1 输出。面板 fgEnabled 为 live 语义(会话内
    // 关 = 复制真实帧;未激活会话内开 = 下次播放生效)。
    std::unique_ptr<DlssfgContext> _fg;
    NVSDK_NGX_Parameter *_fgParams = nullptr; // FG 专用核心参数块(core 拥有)
    bool _fgRequested = false;
    // sticky 语义仅限会话内 live 变化(面板开关只动逐帧 eval 门);seek 边界
    // 由 Rebind 按新请求重定格(形态段),FgActive() 请求感知随之。
    // FG hook 代理 dll 路径(hotMatch 保证代理路径跨 seek 恒等;形态段
    // off→on 时按此补预载 —— 冷初始化 stage 0 已预载过则进程缓存秒回)。
    wchar_t _fgProxyPath[MAX_PATH]{};
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
    // 抗闪烁时域稳定器(上游 antiFlicker,Magpie v0.6.8 DLSSNRTemporal):
    // _curAntiFlicker = 当前已建资源的模式(0 = 关)。时间线状态(上游
    // DLSSNRTemporalState 移植,捕获时间戳 → 帧入口 QPC):weight =
    // exp(-Δt/80ms),>250ms / 帧序不连续 / useMotion 翻转 / 重建帧一律
    // weight 0(播种:历史写当前残差,下一帧起正常累积)。_tNext 由
    // ProcessFrame 的 evaluate 串行域内推进,与 NGX 单例同一把
    // _evaluateMutex 保护(记录序 ≠ 提交序的风险面与 NGX 时域历史一致:
    // 乱序帧 weight 0 拒混,双缓冲容忍一帧偏斜)。
    // 帧路径 live 切换写(无全局锁),他帧线程读 —— 与上方 OF 会话事实
    // 同理 atomic 化(2026-10-04)。
    std::atomic<int> _curAntiFlicker{ 0 };
    long long _tLastFrame = -1;
    double _tLastQpc = 0.0;
    int _tNext = 0;
    bool _tValid = false;
    bool _tLastUseMotion = false;
    // 抗闪烁状态发布(stats 通道):时域块/重建路径写,FinishFrame 的 stats
    // JSON 读。跨帧线程普通成员会撕,atomics 与 _lastFrameN 同款惯例。
    // state 0=off 1=seed 2=steady 3=failed。
    std::atomic<int> _tPubState{ 0 };
    std::atomic<int> _tPubRoute{ 0 };
    std::atomic<float> _tPubWeight{ 0.0f };
    // 面板可见的死亡状态发布(passthrough / ngx_faulted):边缘触发,每状态
    // 每实例一次。存活态(ok / nvof_zero)由周期 stats tick 携带,不走这里。
    // 0 = passthrough,1 = ngx_faulted,-1 = 尚未发布过。
    std::atomic<int> _lastDeadState{ -1 };
    // 最近一次进入 ProcessFrame 的帧号(recreate/错误 STATUS 行带上它,
    // 用户"第几秒看到异常"即可与 timing log 的帧号对上)。
    std::atomic<int> _lastFrameN{ -1 };
};

} // namespace vsdlssnr
