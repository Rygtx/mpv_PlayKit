// vs_dlssnr - NVIDIA DLSSNR (NGX Feature 18) filter for VapourSynth.
// Ported from Magpie experimental (github.com/SAOG0721/Magpie).
// Model file: nvngx_dlssnr.dll (310.8.0.0, NVIDIA original), zero-guidance mode (Magpie
// guidanceMode=1 Force Zero), same-resolution processing.

#include "bridge.h"
#include "d3d12_context.h"
#include "dlssfg_gate.h"
#include "dlssnr_context.h"
#include "dlssnr_params.h"
#include "mpv_host.h"
#include "ngx_variant.h"
#include "panel_ipc.h"
#include "shared_params.h"

#include "VapourSynth4.h"
#include "VSHelper4.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <windows.h>

// VSDLSSNR_TIMING=1 启用 per-frame 分段计时串(Pack/Unpack 与 getFrame 两处
// 消费;2026-10-04 收拢 —— 原两处各自 static 同义双账)。
static const bool g_timingEnabled =
    GetEnvironmentVariableA("VSDLSSNR_TIMING", nullptr, 0) != 0;

using vsdlssnr::kFgGenSlots; // FG 插值槽数上界(d3d12_context.h,kFgMultMax-1)

// RTX 参数裁决:vpy args + ini + 面板 payload 已在 DlssnrParams 上合并
// (BridgeLoadIni/AdoptPanelPayload 走共享映射),本函数只剩 mode=1 的
// mpv 窗口显示矩形探测(窗口探测本体在 mpv_host,2026-10-04 拆出;
// plugin.cpp 是 vsrAutoHeight 的唯一写入点)。srcW/srcH = 源分辨率。
// 探测失败(无可见窗口)写 0:mode=1 消费侧按"目标=源"处理 = VSR 旁路,
// 窗口出现后的链重建重新探测并启用。
static void ResolveRtxParams(DlssnrParams &p, int srcW, int srcH) noexcept {
    if (p.rtxVsrMode == 1) {
        const vsdlssnr::MpvDisplayPick disp = vsdlssnr::MpvDetectTargetSize(srcW, srcH);
        p.rtxVsrAutoHeight = disp.height > 0 ? disp.height : 0;
    }
}

namespace {

constexpr char PLUGIN_IDENTIFIER[] = "dev.rygtx.vsdlssnr";
constexpr char PLUGIN_NAMESPACE[] = "dlssnr";
constexpr char PLUGIN_NAME[] = "NVIDIA DLSSNR filter (Magpie port)";
constexpr char FG_PROXY_DLL_NAME[] = "version.dll"; // dlssg_for_sm86 0.3.x hook 型代理

// vpy explicit arguments overwrite the DlssnrParams member initializers —
// the struct defaults in dlssnr_params.h are the single authority, so a
// default change cannot drift between the vpy layer and the ini/panel layers.
// Missing keys leave the field untouched; present keys are clamped at the
// boundary against the same dlssnr_params.h range constants.
void ApplyIntArg(const VSMap *in, const VSAPI *vsapi, const char *key,
                 int &field, int lo, int hi) noexcept {
    int err = 0;
    const long long v = vsapi->mapGetInt(in, key, 0, &err);
    if (!err) field = static_cast<int>(std::clamp<long long>(v, lo, hi));
}

void ApplyFloatArg(const VSMap *in, const VSAPI *vsapi, const char *key,
                   float &field, float lo, float hi) noexcept {
    int err = 0;
    const double v = vsapi->mapGetFloat(in, key, 0, &err);
    if (!err) field = vsh::doubleToFloatS(std::clamp(v, static_cast<double>(lo), static_cast<double>(hi)));
}

void ApplyFlagArg(const VSMap *in, const VSAPI *vsapi, const char *key, int &field) noexcept {
    int err = 0;
    const long long v = vsapi->mapGetInt(in, key, 0, &err);
    if (!err) field = v != 0;
}

struct FilterData {
    VSNode *node = nullptr;
    // Runtime-mutable parameters shared with the tray panel (initial values
    // come from the .vpy call). RTX Video 参数(v22 起)也住 DlssnrParams:
    // vpy args ← ini ← 面板 payload 合并,创建时经 RtxFromParams 折成
    // ctx 载荷。
    std::shared_ptr<vsdlssnr::SharedParams> params;
    // shared_ptr 而非 unique_ptr:bridge 线程超时泄漏路径(bridge.cpp
    // BridgeStopLocked 5s 超时)下,楔住的线程仍持 state->params 裸用 ——
    // unique_ptr 会随 FilterData 销毁 SharedParams = use-after-free;
    // shared_ptr 由 BridgeState 同持,生命周期对齐最后的消费者。

    // Eagerly initialized in DlssnrCreate (before playback starts); VS may
    // call getFrame on several threads under fmParallel, but each frame runs
    // on its own D3D12 slot, so no further locking is needed here. Both are
    // unique_ptr because they move into the process-level hot context on
    // Free and move back out on the next Create (mpv re-runs the whole VS
    // script on every seek; without the hot context that pays the ~1s
    // D3D12+NGX bring-up each time).
    std::unique_ptr<vsdlssnr::D3D12Context> d3d12;
    std::unique_ptr<vsdlssnr::DlssnrContext> ngx;
    std::wstring ngxDllPath;
    bool initOk = false;
    int width = 0;
    int height = 0;
    int depth = 0; // YUV 位深(8/10/12/14/16);同尺寸换深度必须走冷重建(hotMatch 拦截)
    int subW = 1;  // 输入色度抽取档(420=(1,1) 422=(1,0) 444=(0,0));RGB=0
    int subH = 1;
    bool isRgb = false; // VS RGBP 计划族直读(零矩阵)
    // resize watcher 停事件句柄(匿名 auto-reset;Free 时 SetEvent+Close,
    // watcher 线程生命周期与实例对齐)。
    HANDLE resizeWatchStop = nullptr;
    // RTX Video 输出几何(init 后从 context 读回;输出帧按此建)。
    int outW = 0;
    int outH = 0;
    bool hdrOut = false;       // 输出 = P10 BT.2020 PQ(HDR props 随帧写)
    bool rtxActive = false;    // RTX 在管线(输出格式/尺寸可能 ≠ 源)
    VSVideoFormat outFi{};     // 输出帧格式(hdr → YUV420P10,否则沿用源)
    // Failure-log latch: a wedged context fails every frame at frame rate —
    // log the first failure only, re-arm on the next success.
    std::atomic<bool> failureLogged{ false };
    // NR 总开关 stats 发布边沿(-1 = 未发布):开关翻转时向面板发一次
    // filter_state 状态行(live 关 = 直通原因;live 开但未初始化 = 提示
    // 需要 seek)。开着且已初始化时不发布 —— 常规逐帧 stats 接管。
    std::atomic<int> nrPubState{ -1 };

    // ---- DLSS FG 多帧输出 ----
    // fgActive = 创建时 FG 激活(官方链初始化成功):每源帧产出 M0 帧
    // (M0 = fgCreateMult,创建时定格 —— 输出帧数/fps 元数据/每槽时长随之
    // 与 mpv 定约,会话内恒定)。输出索引 n → (源帧 k, 槽位) 为无状态闭式
    // 映射(见 DlssnrGetFrame):序列 = R0, [G(k-1,k)×(M0-1), R_k] ... ——
    // 处理源帧 k 产出的插值帧落在 k-1 与 k 之间(DLSS-G 语义;Magpie
    // Renderer::_CompleteBackendFrame 同款发布序:gen 先、真实帧后),
    // 显示在 R_{k-1} 之后、R_k 之前。旧版"real 在前 gen 在后"把每个插值
    // 帧晚放一个源帧位,运动画面持续前后抖动(用户实测)。
    // 会话内有效密度 = min(live 倍数, M0):live 关闭/降档即时生效(多余
    // 槽位回落真实帧引用);升档超过 M0 无槽可填,需下个 seek(重建即新
    // M0)。输出帧数/节奏契约因此恒定 —— 旧版随 live 倍数改组尺寸会撕裂
    // 契约(提前 EOF / 尾部冻结)。
    // 注:曾有按 n 单调推进的顺序状态机,实测 VS core(fmParallel)的评估
    // 启动顺序不保证单调(timing log "fg out-of-order" 行)—— 首个乱序
    // 请求即失位,此后全部请求走闭式回退且 fgM 冻结在创建值(live 倍数
    // 静默失效)。已删;闭式映射对请求顺序天然免疫。
    // fgMutex 只串行缓存与处理(同源帧一次处理、各槽从缓存出;FG 链的
    // NVOF/DLSSG 历史按处理序推进,帧序门自愈乱序)。
    std::mutex fgMutex;
    bool fgActive = false;
    std::wstring fgDllPath;
    int fgCreateMult = 2;                     // 创建时倍数(vi.fps/帧数元数据 + 组槽位结构)
    int srcFrames = 0;                        // 源帧数(vi.numFrames;0 = 未知,尾部钳制禁用)
    int fgCacheK = -1;                        // 缓存命中 = 同源帧的后继槽位请求
    int fgCacheM = 0;                         // 有效缓存条目上界(1 + 插值槽数;空位判 null)
    const VSFrame *fgCache[kFgMultMax] = {};  // [0]=真实帧 [1..]=插值帧;持引用
    // ---- 缓存内容就绪门(2026-09-25 持锁窗口缩小)----
    // fgMutex 持锁窗口缩到"pack→提交→缓存存储";GPU 等待 + unpack 移到锁外
    // (ProcessFrameFinish),使下一源帧的 CPU 链(pack/OF/NGX 录制/提交)
    // 与上一帧的 GPU 执行重叠 —— FG 会话从"零重叠"回到 3 槽流水。内容与
    // 交付的时序由世代门保证:处理线程锁内存缓存(引用仍 pending)时
    // ++fgCacheGen 并挂 pending 就绪项,本批 Finish 落账时翻旗标并 notify;
    // 消费线程在 fgMutex 内 addFrameRef 领引用(引用计数防逐出)+ 读世代,
    // 就绪项未翻则锁外等 cv —— 等的是自己那批帧的内容,不阻塞其它帧的
    // 处理。就绪状态按"代"挂账而非单调计数器:三个 Finish 出口各写自己
    // 的代,后完成的代不得放行前代等待者(前代 unpack 可能还在写缓存帧
    // —— 2026-10-04 评审修:旧 fgReadyGen ">=" 谓词正是这个假放行)。
    // 锁序恒 fgMutex → fgReadyMutex,无环(Finish 出口不持 fgMutex)。
    static constexpr int kFgReadyRing = 8;    // ≥ 在飞批数上限;被挤出 = 早于最近 8 批,必然已落账
    uint64_t fgCacheGen = 0;                  // 最近一次缓存存储的世代(fgMutex 内写)
    uint64_t fgReadyRingGen[kFgReadyRing] = {}; // 0 = 空槽(fgReadyMutex 内读写)
    bool fgReadyRingOk[kFgReadyRing] = {};
    int fgReadyRingNext = 0;
    std::mutex fgReadyMutex;
    std::condition_variable fgReadyCv;
};

// 世代门环形账(均须 fgReadyMutex;FgReadyQueryLocked 供 cv 谓词在已持锁
// 下复用)。gen 未入环 = 早于最近 kFgReadyRing 批 —— 每批必然落账(三个
// Finish 出口 + 同步兜底路径全覆盖),被挤出即早已就绪。
static bool FgReadyQueryLocked(const FilterData *d, uint64_t gen) {
    for (int i = 0; i < FilterData::kFgReadyRing; ++i)
        if (d->fgReadyRingGen[i] == gen) return d->fgReadyRingOk[i];
    return true;
}

static bool FgReadyQuery(FilterData *d, uint64_t gen) {
    std::lock_guard<std::mutex> lock(d->fgReadyMutex);
    return FgReadyQueryLocked(d, gen);
}

static void FgReadyMark(FilterData *d, uint64_t gen, bool ready) {
    std::lock_guard<std::mutex> lock(d->fgReadyMutex);
    // 已有账(本批存储时的 pending 项)→ 原位翻旗 —— 追加第二条会让查询的
    // "首个同代项"永远命中 pending false(2026-10-04 实测:每次面板改参数
    // 后 4 个在飞消费端各冻结 15s 才超时放行,真旗标被 pending 项遮蔽)。
    for (int i = 0; i < FilterData::kFgReadyRing; ++i) {
        if (d->fgReadyRingGen[i] == gen) {
            d->fgReadyRingOk[i] = ready;
            d->fgReadyCv.notify_all();
            return;
        }
    }
    d->fgReadyRingGen[d->fgReadyRingNext] = gen;
    d->fgReadyRingOk[d->fgReadyRingNext] = ready;
    d->fgReadyRingNext = (d->fgReadyRingNext + 1) % FilterData::kFgReadyRing;
    d->fgReadyCv.notify_all();
}

// Process-lifetime hot context. mpv's vf_vapoursynth tears down and
// re-creates the whole VS script on every seek; keeping the D3D12 device,
// NGX feature and slot pool alive across filter instances turns that from a
// ~1s reload into a near-free Rebind. Single-threaded by construction: VS
// serializes filter create/free, and the plugin is process-level single
// instance (IAT hook).
//
// Deliberately leaked (never destroyed): tearing the kept-warm context down
// from static teardown / DllMain(process exit) would run NGX shutdown under
// the loader lock and deadlock; the OS reclaims everything at exit anyway.
struct HotContext {
    std::unique_ptr<vsdlssnr::D3D12Context> d3d12;
    std::unique_ptr<vsdlssnr::DlssnrContext> ngx;
    std::wstring ngxDllPath;
    int width = 0;
    int height = 0;
    int depth = 0;
    // 布局参与 hotMatch:同尺寸同深度换 420/422/444/RGB = 色度面几何/核
    // 形态变化(槽纹理尺寸 baked),必须冷重建。
    int subW = 1;
    int subH = 1;
    bool isRgb = false;
    // FG/RTX 档位(开关/模式/倍率/hdr/fmt)不参与 hotMatch(2026-09-27
    // 解耦收尾):变化由 DlssnrContext::Rebind → RecreateFeature 形态段
    // 热重建(槽资源 + VSR/HDR/FG 会话),切档不再付冷启动。代理路径仍
    // 参与(hotMatch 保证形态段 _fgProxyPath 恒等);路由(route)进程级,
    // 重启生效。
    std::wstring fgDllPath;
    bool valid = false;
};
HotContext &Hot() {
    static HotContext *inst = new HotContext();
    return *inst;
}

// create/free 生命周期串行(2026-09-25):VS 的 filter free/create 顺序不
// 保证,旧实例 Free 的"停放"与新实例 Create 的"热匹配/冷初始化"可并发 ——
// 热匹配摘取与停放是 check-then-act,并发可双摘/悬垂;冷初始化在旧上下文
// 仍持 IAT hook owner 时强启会被 CAS 拒载(FG 代理链静默失效)+ 双设备短
// 暂并存。create 侧从热匹配判定到冷初始化结束持锁,free 侧从 BridgeStop
// 到停放完成持锁;冷初始化 ~1s,并发的对方最多等一个冷启动周期。
// 锁序:本锁 → bridge::g_bridgeMutex(单向,无反向获取)。
std::mutex g_lifecycleMutex;

} // namespace

// FG 多帧输出的时长语义:mpv 的 vf_vapoursynth 输出节奏只认输出帧 props
// 的整数对 _DurationNum/_DurationDen(喂数时按 pkt_duration*1e6 写入源帧,
// 收帧时读回算 nominal_fps;VS 的 vi.fps 只是元数据不参与节拍)。倍数 M 的
// 每个输出帧各占源帧 1/M:_DurationDen ×= M(分子不变)—— 同源帧 M 个输出
// 时长求和恒等于源时长,倍数变化不漂移。整数对缺失(非 mpv 宿主)时退
// 回浮点 _Duration(存在才写)。
static void ScaleOutputDuration(VSFrame *frame, const VSAPI *vsapi, int m) noexcept {
    VSMap *props = vsapi->getFramePropertiesRW(frame);
    if (!props) return;
    int err = 0;
    const int64_t num = vsapi->mapGetInt(props, "_DurationNum", 0, &err);
    int err2 = 0;
    const int64_t den = err ? 0 : vsapi->mapGetInt(props, "_DurationDen", 0, &err2);
    if (!err && !err2 && den > 0 && num > 0) {
        vsapi->mapSetInt(props, "_DurationNum", num, maReplace);
        vsapi->mapSetInt(props, "_DurationDen", den * m, maReplace);
        return;
    }
    err = 0;
    const double dur = vsapi->mapGetFloat(props, "_Duration", 0, &err);
    if (!err) {
        vsapi->mapSetFloat(props, "_Duration", dur / m, maReplace);
    }
}

// 面板参数桥启动失败提示(三条初始化路径共用;失败 = 播放照常,仅面板
// 改动不生效)。
static void WarnBridgeFailed(VSCore *core, const VSAPI *vsapi) noexcept {
    vsapi->logMessage(mtWarning,
                      "vs_dlssnr: parameter bridge failed to start; panel edits will not apply",
                      core);
    vsdlssnr::TimingStatusLine("DLSSNR STATUS: bridge start FAILED; panel edits will not apply");
}

// 从源帧复制三平面(YUV420)到 dst —— 帧失败的降级路径。
static void CopyPlanes(const VSFrame *src, VSFrame *dst, const VSAPI *vsapi,
                       int width, int height) noexcept {
    const VSVideoFormat *fi = vsapi->getVideoFrameFormat(dst);
    const int bpp = fi->bytesPerSample;
    // 色度平面尺寸按帧真实格式(subSampling 派生,VS ceil 规则):
    // 420 半、422 半宽、444/RGB 全分辨率 —— 勿再自推半分辨率。
    const int cw = fi->subSamplingW ? (width + 1) >> 1 : width;
    const int ch = fi->subSamplingH ? (height + 1) >> 1 : height;
    for (int p = 0; p < 3 && p < fi->numPlanes; ++p) {
        const int pw = p == 0 ? width : cw;
        const int ph = p == 0 ? height : ch;
        vsh::bitblt(vsapi->getWritePtr(dst, p), vsapi->getStride(dst, p),
                    vsapi->getReadPtr(src, p), vsapi->getStride(src, p),
                    static_cast<size_t>(pw) * bpp, static_cast<size_t>(ph));
    }
}

// ctx 载荷(创建时定格):DlssnrParams 合并值 → RtxVideoParams(纯创建时
// 几何/形态;per-eval live 值运行中由 Snapshot 逐帧取,不进卡片)。
static RtxVideoParams RtxFromParams(const DlssnrParams &p) noexcept {
    RtxVideoParams rtx;
    rtx.vsrMode = std::clamp(p.rtxVsrMode, kVsrModeMin, kVsrModeMax);
    rtx.vsrScale = std::clamp(p.rtxVsrScale, kVsrScaleMin, kVsrScaleMax);
    rtx.hdrEnabled = p.rtxHdrEnabled != 0;
    // 探测失败哨兵 0 原样直达:消费侧(vsrAutoHeight > 0 ? clamp : srcH)
    // 以 0 = "目标=源,VSR 旁路"分支消费;此处若钳 0→144 会令该分支不可达
    // (旁路语义全靠 ratio<=1.001 巧合兜住)。
    rtx.vsrAutoHeight = p.rtxVsrAutoHeight;
    return rtx;
}

// HDR 输出帧色彩签名(BT.2020 PQ limited;mpv 按 props 上屏/色调映射)。
static void SetHdrFrameProps(VSFrame *frame, const VSAPI *vsapi) noexcept {
    VSMap *props = vsapi->getFramePropertiesRW(frame);
    if (!props) return;
    vsapi->mapSetInt(props, "_Matrix", 9, maReplace);      // BT.2020 NCL
    vsapi->mapSetInt(props, "_Transfer", 16, maReplace);   // ST 2084 (PQ)
    vsapi->mapSetInt(props, "_Primaries", 9, maReplace);   // BT.2020
    vsapi->mapSetInt(props, "_ColorRange", 1, maReplace);  // limited
}

// 从源帧 props 解析色彩矩阵/范围(缺失/未知回落 709 limited);值变化时
// 留痕一行(atomic 边沿去重)。VS _Matrix:1=BT.709,5(BT470BG)/6(SMPTE170M)
// =601 族,4(XYZ)=601 近似(D65 录制内容的可用降级);9(BT.2020)/0(GBR)
// 等超范围按 709 处理(vpy 层已有 HDR 直通守卫,到这里的多半是属性缺失
// 的裸流)。
static void ParseColorProps(const VSMap *props, const VSAPI *vsapi,
                            vsdlssnr::ColorMatrix &matrix,
                            vsdlssnr::ColorRange &range) noexcept {
    int perr = 0;
    const int matrixProp = vsapi->mapGetInt(props, "_Matrix", 0, &perr);
    const bool matrixKnown = perr == 0;
    perr = 0;
    const int rangeProp = vsapi->mapGetInt(props, "_ColorRange", 0, &perr);
    const bool rangeKnown = perr == 0;
    matrix = vsdlssnr::ColorMatrix::BT709;
    if (matrixKnown && (matrixProp == 4 || matrixProp == 5 || matrixProp == 6)) {
        matrix = vsdlssnr::ColorMatrix::BT601;
    }
    range = vsdlssnr::ColorRange::Limited;
    if (rangeKnown && rangeProp == 0) range = vsdlssnr::ColorRange::Full;
    const int sig = (matrixKnown ? matrixProp : -1) * 16 + (rangeKnown ? rangeProp : -2);
    static std::atomic<int> lastSig{ INT_MIN };
    if (lastSig.exchange(sig, std::memory_order_relaxed) != sig) {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: frame props matrix=%s(%d) range=%s(%d) -> %s/%s",
                      matrixKnown ? "yes" : "missing", matrixProp,
                      rangeKnown ? "yes" : "missing", rangeProp,
                      matrix == vsdlssnr::ColorMatrix::BT709 ? "709" : "601",
                      range == vsdlssnr::ColorRange::Limited ? "limited" : "full");
        vsdlssnr::TimingStatusLine(msg);
    }
}

namespace {
// FG/非 FG 路径的公共帧执行尾(此前同一序列在 DlssnrGetFrame 两分支平行
// 抄两遍 —— 失败策略/timing 节流/HDR props 改动需双处同步,"RTX 失败不
// 兜底"裁定即双处落的实例):平面指针打包 → ProcessFrame → 失败插值槽
// 释放(FG)→ 失败闩锁(RTX = 报错终止;非 RTX = 同构行拷贝兜底)→
// HDR props → timing 节流。
// 返回 false = 已 setFilterError(调用方直接 return nullptr);成功/兜底
// 时 out 内容恒就绪。ffOut 非空 = defer 模式(ProcessFrame 只跑 Submit
// 半段,续体交还调用方);genFrame/effGens/fgGenOk 仅 FG 路径提供。
bool RunProcessFrameCommon(FilterData *d, const VSFrame *src, VSFrame *out,
                           int frameN, int effM,
                           VSFrame **genFrame, int effGens, bool *fgGenOk,
                           vsdlssnr::FrameFinish **ffOut,
                           VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) noexcept {
    const uint8_t *srcPlanes[3]{};
    int64_t srcStrides[3]{};
    uint8_t *dstPlanes[3]{};
    int64_t dstStrides[3]{};
    for (int p = 0; p < 3; ++p) {
        srcPlanes[p] = vsapi->getReadPtr(src, p);
        srcStrides[p] = vsapi->getStride(src, p);
        dstPlanes[p] = vsapi->getWritePtr(out, p);
        dstStrides[p] = vsapi->getStride(out, p);
    }
    uint8_t *genPlanes[kFgGenSlots * 3]{};
    int64_t genStrides[kFgGenSlots * 3]{};
    for (int g = 0; g < effGens; ++g) {
        if (!genFrame[g]) continue; // 分配失败槽:留空回落真实帧(2026-10-04 评审修)
        for (int p = 0; p < 3; ++p) {
            genPlanes[g * 3 + p] = vsapi->getWritePtr(genFrame[g], p);
            genStrides[g * 3 + p] = vsapi->getStride(genFrame[g], p);
        }
    }
    char err[256]{};
    char timing[128]{};
    vsdlssnr::ColorMatrix matrix = vsdlssnr::ColorMatrix::BT709;
    vsdlssnr::ColorRange range = vsdlssnr::ColorRange::Limited;
    ParseColorProps(vsapi->getFramePropertiesRO(src), vsapi, matrix, range);
    const bool procOk = d->ngx->ProcessFrame(
        srcPlanes, srcStrides, dstPlanes, dstStrides,
        effM, effGens > 0 ? genPlanes : nullptr,
        effGens > 0 ? genStrides : nullptr, fgGenOk,
        d->width, d->height, frameN, matrix, range, err, sizeof(err),
        g_timingEnabled ? timing : nullptr, g_timingEnabled ? sizeof(timing) : 0,
        ffOut);
    // 失败/未评插值槽:释放帧(出帧时槽位回落真实帧引用)。
    for (int g = 0; g < effGens; ++g) {
        if (genFrame[g] && (!procOk || !fgGenOk[g])) {
            vsapi->freeFrame(genFrame[g]);
            genFrame[g] = nullptr;
        }
    }
    if (!procOk) {
        if (!d->failureLogged.exchange(true)) {
            char msg[512];
            std::snprintf(msg, sizeof(msg), "vs_dlssnr frame %d failed: %s", frameN, err);
            vsapi->logMessage(mtWarning, msg, core);
            // 探针:首帧失败进 timing log(GUI mpv 完全看不到 logMessage)。
            vsdlssnr::TimingStatusLine(msg);
        }
        // RTX 会话失败不再兜底(2026-09-26 用户裁定):最近邻缩放拷贝
        // 产出几何错误的伪内容,宁报错终止也不静默降级。失败帧不入
        // 缓存、不推进世代 —— 等待者无从挂在本帧世代上。
        if (d->rtxActive) {
            char abortMsg[320];
            std::snprintf(abortMsg, sizeof(abortMsg),
                          "vs_dlssnr frame %d failed: %s", frameN, err);
            vsapi->freeFrame(out);
            vsapi->freeFrame(src);
            vsapi->setFilterError(abortMsg, frameCtx);
            return false;
        }
        // 非 RTX:同构行拷贝兜底,输出平面不留未初始化内容。
        CopyPlanes(src, out, vsapi, d->width, d->height);
    } else {
        d->failureLogged.store(false);
        if (d->hdrOut) SetHdrFrameProps(out, vsapi);
    }
    if (g_timingEnabled && timing[0]) {
        // Throttle: log every 30th frame (fmParallel: order irrelevant).
        static std::atomic<int> timingFrameCount{ 0 };
        if (timingFrameCount.fetch_add(1, std::memory_order_relaxed) % 30 == 1) {
            char msg[192];
            std::snprintf(msg, sizeof(msg), "vs_dlssnr timing[%d]: %s", frameN, timing);
            vsapi->logMessage(mtInformation, msg, core);
        }
    }
    return true;
}
} // namespace

static const VSFrame *VS_CC DlssnrGetFrameImpl(
    int n, int activationReason, void *instanceData, void **frameData,
    VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    auto *d = static_cast<FilterData *>(instanceData);

    if (activationReason == arInitial) {
        if (d->fgActive) {
            // 输出索引 → (源帧, 槽位) 无状态闭式映射。VS core(fmParallel)
            // 的评估启动顺序不保证 n 单调,任何依赖请求顺序的状态机都会
            // 失位 —— 每个输出索引独立换算,天然免疫乱序:
            //   n == 0    → R0(播种源帧,仅真实帧)
            //   n >= 1    → 组 g = (n-1)/M0;源帧 k = g+1;组内位 p = (n-1)%M0
            //               p < M0-1 → 槽 p+1(插值帧,随处理源帧 k 产出)
            //               p == M0-1 → 槽 0(真实帧 R_k,组末尾)
            // 尾部(k 越过最后一源帧)= 重复 R_{S-1} 占位(无后续源帧可
            // 插值,首尾帧保持优于越界请求)。
            const int m0 = d->fgCreateMult;
            int k = 0, slot = 0;
            if (n > 0) {
                k = (n - 1) / m0 + 1;
                if (d->srcFrames > 0 && k >= d->srcFrames) {
                    k = d->srcFrames - 1;
                    slot = 0;
                } else {
                    const int p = (n - 1) % m0;
                    slot = (p == m0 - 1) ? 0 : p + 1;
                }
            }
            *frameData = reinterpret_cast<void *>(static_cast<intptr_t>(
                (static_cast<intptr_t>(k) << 4) | slot)); // 有符号移位前先拓宽:1.3 亿帧后 (k<<4) 溢出 UB
            vsapi->requestFrameFilter(k, d->node, frameCtx);
            return nullptr;
        }
        vsapi->requestFrameFilter(n, d->node, frameCtx);
        return nullptr;
    }
    if (activationReason != arAllFramesReady) return nullptr;


    // NR 总开关 live 门(shared_lock 快照,fmParallel 并发安全)。FG 激活
    // 的会话:NR 关由 ProcessFrame 内部门控(跳过NR 评估,补帧/光流照常
    // —— 输出仍是增强管线的产物),不走此直通。开关边沿向面板发一次状态
    // (见 nrPubState 注释)。
    // 单帧一次参数快照(2026-09-25:此前此处与 FG 段各取一次,缓存命中
    // 路径的第二次全量拷贝+shared_lock 无效;单快照同时保证帧内语义同源)。
    const DlssnrParams frameParams = d->params->Snapshot();
    const bool nrLive = frameParams.nrEnabled != 0;
    const int nrPub = nrLive ? 1 : 0;
    if (d->nrPubState.exchange(nrPub) != nrPub) {
        // 解耦后(a2e6ae0)live 会话的真相恒由逐帧 stats 携带
        //(filter_state="ok" + 关闭段归零 + conv/pack 胶水照常记账),
        // 边沿发布只剩一种:no-session 实例收到 nr=1 —— 参数要 NR 但
        // 会话不存在,面板闭环据此补 reseek。
        // 旧 "NR off (panel)" 边沿体已删:其门(!nrLive && !fgActive)
        // 是解耦前 "NR+FG 关 = 整管线停" 的假设,漏 RTX —— VSR/TrueHDR
        // 仍在跑的会话关 NR 瞬间被误标 passthrough;且逐帧 stats 恒在
        // 一帧内覆盖它,无真实职能。持久 passthrough 只来自 create 全关
        // ("NR+FG+RTX disabled")/init 失败/死亡态 —— 那些状态 NR 本就
        // 不可能 live 恢复,面板 needsReseek 判据随之简化为
        // "passthrough 恒重建"。
        if (nrLive && !d->initOk) {
            vsdlssnr::StatsPayload st{};
            vsdlssnr::CopyStatStr(st.filterState, "passthrough");
            vsdlssnr::CopyStatStr(st.stateDetail, vsdlssnr::kStateNrSeekInit);
            vsdlssnr::PublishStats(st);
        }
    }

    if (d->fgActive) {
        const intptr_t fd = reinterpret_cast<intptr_t>(*frameData);
        const int k = static_cast<int>(fd >> 4);
        const int slot = static_cast<int>(fd & 0xF);
        // fgMutex 串行处理与缓存(同源帧一次处理,各槽从缓存出;FG 链的
        // NVOF/DLSSG 历史按处理序推进,乱序由 NVOF 帧序门自愈)。持锁窗口
        // = pack→提交→缓存存储;GPU 等待 + unpack 在锁外(见 Finish 段)。
        std::unique_lock<std::mutex> fgLock(d->fgMutex);
        // 有效密度 = min(live 倍数, 结构倍数 M0)。M0(帧数/节奏契约)创建时
        // 定格;live 倍数只决定组内多少槽位产出真插值 —— 关闭/降档即时生效
        // (多余槽位回落真实帧引用),升档超过 M0 无槽可填(需下个 seek)。
        // 播种源帧(k=0)仅真实帧:无消费槽,不带插值平面(eval 门随之关闭,
        // 历史重置留给首个插值组,与其对齐 Magpie 的 reset 帧不发布插值)。
        const int m0 = d->fgCreateMult;
        const DlssnrParams &snap = frameParams;
        int effM = snap.fgEnabled
                       ? (std::min)(std::clamp(snap.fgMultiplier, kFgMultMin, kFgMultMax), m0)
                       : 1;
        int effGens = effM - 1;
        if (k == 0) {
            effM = 1;
            effGens = 0;
        }
        if (k == d->fgCacheK && d->fgCache[0]) {
            // 缓存命中:先领引用再等内容(2026-09-25 持锁窗口缩小)。引用在
            // fgMutex 内领 —— 引用计数使后续逐出(k+1 存储覆盖)不影响本引用;
            // 内容可能仍在锁外 unpack(处理线程已提交、缓存标了 Pending)——
            // 世代门等本帧世代就绪项翻旗后交付。等待在两把锁外完成,
            // 不阻塞其它源帧的处理线程。
            // 无缓存内容的槽位(密度下调的多余槽/播种帧/eval 降级)回落
            // 真实帧 —— 槽位照常占位,输出节奏不变。
            const VSFrame *out =
                (slot >= 1 && slot < d->fgCacheM && d->fgCache[slot])
                    ? vsapi->addFrameRef(d->fgCache[slot])
                    : vsapi->addFrameRef(d->fgCache[0]);
            const uint64_t myGen = d->fgCacheGen;
            if (!FgReadyQuery(d, myGen)) {
                fgLock.unlock();
                std::unique_lock<std::mutex> readyLock(d->fgReadyMutex);
                if (!d->fgReadyCv.wait_for(readyLock, std::chrono::seconds(15),
                                           [&] { return FgReadyQueryLocked(d, myGen); })) {
                    char msg[128];
                    std::snprintf(msg, sizeof(msg),
                                  "DLSSNR STATUS: fg gen-gate wait TIMEOUT frame=%d gen=%llu",
                                  k, static_cast<unsigned long long>(myGen));
                    vsdlssnr::TimingStatusLine(msg);
                }
            }
            return out;
        }

        const VSFrame *src = vsapi->getFrameFilter(k, d->node, frameCtx);
        if (!d->initOk) {
            // passthrough on setup failure(防御:fgActive 恒蕴含 initOk;
            // NR 关不在此列 —— ProcessFrame 内部门控跳过NR 评估,补帧以
            // 直通帧为 backbuffer 照常插值)。FG 输出计数仍是 M0(帧率已
            // ×M0):源帧单次复制入缓存,各槽回落该帧,时长按 1/M0 摊分。
            // rtxActive 只在 initOk 后置位(Create 侧),此处恒 false,
            // 几何恒 = 源 → 恒同构行拷贝(缩放拷贝兜底已删,2026-09-26)。
            VSFrame *dup = vsapi->newVideoFrame(&d->outFi, d->outW, d->outH, src, core);
            if (!dup) { // OOM:帧错误上报,不空指针杀宿主(2026-10-04 评审修)
                vsapi->freeFrame(src);
                vsapi->setFilterError("dlssnr.Enhance: newVideoFrame (dup) failed", frameCtx);
                return nullptr;
            }
            CopyPlanes(src, dup, vsapi, d->width, d->height);
            if (d->hdrOut) SetHdrFrameProps(dup, vsapi);
            ScaleOutputDuration(dup, vsapi, m0);
            d->fgCacheK = k;
            d->fgCacheM = 1;
            for (int i = 0; i < kFgMultMax; ++i) {
                if (d->fgCache[i]) vsapi->freeFrame(d->fgCache[i]);
                d->fgCache[i] = nullptr;
            }
            d->fgCache[0] = dup;
            // 纯 CPU 降级:内容同步就绪 —— 世代推进 + 挂就绪账(无 pending 窗口)。
            ++d->fgCacheGen;
            FgReadyMark(d, d->fgCacheGen, true);
            const VSFrame *ret = vsapi->addFrameRef(d->fgCache[0]);
            vsapi->freeFrame(src);
            return ret;
        }

        VSFrame *out = vsapi->newVideoFrame(&d->outFi, d->outW, d->outH, src, core);
        if (!out) { // OOM:帧错误上报(同上,2026-10-04 评审修)
            vsapi->freeFrame(src);
            vsapi->setFilterError("dlssnr.Enhance: newVideoFrame failed", frameCtx);
            return nullptr;
        }
        // effGens 个插值输出帧(仅 eval 成功槽进缓存;失败/降级槽直接释放,
        // 槽位在出帧时回落真实帧引用 —— 零复制优于再拷一份重复帧)。
        // 分配失败的槽留空 = 回落真实帧,null 由下方各消费点显式判空。
        VSFrame *genFrame[kFgGenSlots] = {};
        for (int g = 0; g < effGens; ++g) {
            genFrame[g] = vsapi->newVideoFrame(&d->outFi, d->outW, d->outH, src, core);
        }

        // NGX history-reset policy (frame-gap heuristic) lives in DlssnrContext;
        // only the frame index is forwarded here. 有效密度 effM 与 genPlanes
        // 布局([gen][plane] 扁平)即 ProcessFrame 的多帧输出契约。
        // 拆分模式(2026-09-25 持锁窗口缩小):ProcessFrame 只跑到提交
        // (Submit 半段,锁内);等待 + unpack 在锁外 ProcessFrameFinish ——
        // 下一源帧的 CPU 链与本帧 GPU 执行重叠,FG 会话恢复流水。
        bool fgGenOk[kFgGenSlots] = {};
        vsdlssnr::FrameFinish *ff = nullptr;
        // Submit 半段失败(含 oom)= ff 恒 null:锁内兜底 + 缓存(内容同步
        // 就绪)+ 返回。
        if (!RunProcessFrameCommon(d, src, out, k, effM, genFrame, effGens,
                                   fgGenOk, &ff, frameCtx, core, vsapi)) {
            return nullptr;
        }

        // 时长契约:每输出帧 = 源时长/M0(_DurationNum/_DurationDen 整数对
        // —— mpv 逐帧读回累加 pts、nominal_fps 也由它重算;vi.fps 只是元
        // 数据)。密度变化不改变节奏 —— 同源帧 M0 个输出时长求和恒等于源
        // 时长。缓存帧各设一次(出帧交引用,props 随帧)。props/时长与内容
        // 无关,在缓存存储(锁内)前设置。
        ScaleOutputDuration(out, vsapi, m0);
        for (int g = 0; g < effGens; ++g) {
            if (genFrame[g]) {
                if (d->hdrOut) SetHdrFrameProps(genFrame[g], vsapi);
                ScaleOutputDuration(genFrame[g], vsapi, m0);
            }
        }
        // 缓存替换:[0]=真实帧,[1..effGens]=成功插值帧(slot g+1)。
        for (int i = 0; i < kFgMultMax; ++i) {
            if (d->fgCache[i]) {
                vsapi->freeFrame(d->fgCache[i]);
                d->fgCache[i] = nullptr; // 置空:密度下调后高位槽不再回填,
                                         // 残留旧指针会被下一次填充/Free 双重释放
            }
        }
        d->fgCacheK = k;
        d->fgCacheM = effGens + 1;
        d->fgCache[0] = out; // 接管 newVideoFrame 的引用
        for (int g = 0; g < effGens; ++g) {
            d->fgCache[g + 1] = genFrame[g];
        }
        ++d->fgCacheGen;
        const uint64_t myGen = d->fgCacheGen;
        // pending 项先行(消费端要"查得到才等得到";本批 Finish 落账翻旗标)。
        FgReadyMark(d, myGen, false);
        const VSFrame *ret =
            (slot >= 1 && slot < d->fgCacheM && d->fgCache[slot])
                ? vsapi->addFrameRef(d->fgCache[slot])
                : vsapi->addFrameRef(d->fgCache[0]);
        // src 引用持到 Finish 兜底拷贝之后(unpack 失败的降级路径还要读它)。
        // Finish 保活引用:fmParallel 下并发源帧在 fgMutex 外替换缓存并
        // freeFrame 本帧 out/genFrame,而本帧 Finish 的 unpack 持裸平面
        // 指针写它们 —— GPU 忙时(重建窗口)Finish 拖长,窗口必炸
        // (2026-09-26 闪退族:memcpy INVALID_POINTER_WRITE 写已去提交的
        // VS 帧页,复现 = VSR 切换)。引用计数保活,本帧所有出口(含
        // RTX 报错终止)释放。sync 路径 ff 为 null,Finish 已在
        // ProcessFrame 内完成,无需引用。
        const VSFrame *dstKeep[kFgGenSlots + 1] = {};
        int dstKeepN = 0;
        if (ff) {
            dstKeep[dstKeepN++] = vsapi->addFrameRef(out);
            for (int g = 0; g < effGens; ++g) {
                if (genFrame[g]) dstKeep[dstKeepN++] = vsapi->addFrameRef(genFrame[g]);
            }
        }
        // 守卫在填充后构造(按值捕获最终 n);填充与构造之间无出口。
        // 首版把构造放在填充前,按值捕获的 n 恒 0 = 引用永不释放(泄漏)。
        struct DstKeepGuard {
            const VSAPI *vsapi;
            const VSFrame *const *keep;
            int n;
            ~DstKeepGuard() {
                for (int i = 0; i < n; ++i) vsapi->freeFrame(keep[i]);
            }
        } dstGuard{vsapi, dstKeep, dstKeepN};
        if (ff) {
            // ---- 锁外 Finish:GPU 等待 + unpack(真实帧 + 逐 gen)----
            // (ff 非空 ⟺ Submit 半段成功打包了续体;失败/兜底路径内容已
            // 就绪,无 Finish 半段。)从这里到 Ready 通知之间不持 fgMutex:
            // 其它源帧的 CPU 链(pack/OF/录制/提交)与本帧 GPU 执行/unpack
            // 重叠。消费方在世代门等本帧内容,不阻塞流水。
            fgLock.unlock();
            if (g_timingEnabled) vsdlssnr::TimingStatusLine("PROBE: plugin pre-finish");
            uint8_t *finDst[3]{};
            int64_t finDstStride[3]{};
            for (int p = 0; p < 3; ++p) {
                finDst[p] = vsapi->getWritePtr(out, p);
                finDstStride[p] = vsapi->getStride(out, p);
            }
            uint8_t *finGen[kFgGenSlots * 3]{};
            int64_t finGenStride[kFgGenSlots * 3]{};
            for (int g = 0; g < effGens; ++g) {
                // 失败/未评槽已被 helper 释放置空;对应 fgGenOk[g]=false,
                // Finish 半段不读这些平面 —— 留空即可,绝不对空帧取指针。
                if (!genFrame[g]) continue;
                for (int p = 0; p < 3; ++p) {
                    finGen[g * 3 + p] = vsapi->getWritePtr(genFrame[g], p);
                    finGenStride[g * 3 + p] = vsapi->getStride(genFrame[g], p);
                }
            }
            char finErr[256]{};
            char finTiming[128]{};
            const bool finOk =
                d->ngx->ProcessFrameFinish(ff, finDst, finDstStride,
                                           effGens > 0 ? finGen : nullptr,
                                           effGens > 0 ? finGenStride : nullptr,
                                           fgGenOk, finErr, sizeof(finErr),
                                           g_timingEnabled ? finTiming : nullptr,
                                           g_timingEnabled ? sizeof(finTiming) : 0);
            if (g_timingEnabled) vsdlssnr::TimingStatusLine("PROBE: plugin post-finish");
            if (!finOk) {
                // unpack 失败:帧已入缓存、引用可能已被消费方领走(尚未交付
                // —— 世代门拦着)。日志闩锁与 Submit 失败同惯例。
                if (!d->failureLogged.exchange(true)) {
                    char msg[512];
                    std::snprintf(msg, sizeof(msg),
                                  "vs_dlssnr frame %d failed at finish: %s", k, finErr);
                    vsapi->logMessage(mtWarning, msg, core);
                    vsdlssnr::TimingStatusLine(msg);
                }
                // RTX 会话失败不再兜底(2026-09-26 用户裁定)。坏内容已入
                // 缓存:作废 k 锚(缓存命中需 fgCacheK 精确匹配,置 -1 后
                // 永不命中)+ 推进就绪世代(唤醒世代等待者,免 15s 超时
                // 拖住报错),报错终止。评估中止后坏帧无人消费。
                if (d->rtxActive) {
                    {
                        std::lock_guard<std::mutex> cacheLock(d->fgMutex);
                        d->fgCacheK = -1;
                    }
                    FgReadyMark(d, myGen, true); // 唤醒等待者免 15s 超时拖住报错
                    char abortMsg[320];
                    std::snprintf(abortMsg, sizeof(abortMsg),
                                  "vs_dlssnr frame %d failed at finish: %s", k, finErr);
                    vsapi->freeFrame(ret);
                    vsapi->freeFrame(src);
                    vsapi->setFilterError(abortMsg, frameCtx);
                    return nullptr;
                }
                // 非 RTX:同构行拷贝兜底,交付的仍是可用内容。
                CopyPlanes(src, out, vsapi, d->width, d->height);
                for (int g = 0; g < effGens; ++g) {
                    if (genFrame[g]) {
                        CopyPlanes(src, genFrame[g], vsapi, d->width, d->height);
                    }
                }
            }
            // 内容就绪:落账本批 + 唤醒等待中的消费方(失败也落账 —— 交付
            // 的是兜底内容,等待不悬空)。
            FgReadyMark(d, myGen, true);
        } else {
            // Submit 半段失败:内容已同步兜底,无 pending 窗口 —— 落账本批,
            // 防消费者按本帧世代悬挂在 cv 上。
            FgReadyMark(d, myGen, true);
        }
        vsapi->freeFrame(src);
        return ret;
    }

    // ---- 非 FG 路径(1:1,与旧管线一致;RTX 开 = OUT 几何)----
    const VSFrame *src = vsapi->getFrameFilter(n, d->node, frameCtx);
    // passthrough(NR 关 = 原帧零拷贝;RTX 开时仍要处理 —— VSR/HDR 独立
    // 于 NR 开关)。引用移交调用方。
    if (!d->initOk || (!nrLive && !d->rtxActive)) return src;

    VSFrame *out = vsapi->newVideoFrame(&d->outFi, d->outW, d->outH, src, core);
    if (!out) { // OOM:帧错误上报,不空指针杀宿主(2026-10-04 评审修)
        vsapi->freeFrame(src);
        vsapi->setFilterError("dlssnr.Enhance: newVideoFrame failed", frameCtx);
        return nullptr;
    }
    if (!RunProcessFrameCommon(d, src, out, n, 0, nullptr, 0, nullptr, nullptr,
                               frameCtx, core, vsapi)) {
        return nullptr;
    }
    // getFrameFilter handed us a reference to src; release it or every source
    // frame leaks (~24MB per 1080p frame).
    vsapi->freeFrame(src);
    return out;
}

// VS C ABI 边界兜底(2026-10-04 评审修):实现链里的任何抛式操作(new/
// make_shared/filesystem 等)若逃逸出 C 回调 = std::terminate 杀宿主 mpv。
// 边界一处 catch 全收(含未来回归),比逐点 nothrow 化 ~40 个分配点更小
// 且不会漏;OOM 级失败转化为帧/创建错误上报。
static const VSFrame *VS_CC DlssnrGetFrame(
    int n, int activationReason, void *instanceData, void **frameData,
    VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    try {
        return DlssnrGetFrameImpl(n, activationReason, instanceData, frameData,
                                  frameCtx, core, vsapi);
    } catch (...) {
        vsapi->setFilterError("dlssnr.Enhance: internal error (exception)", frameCtx);
        return nullptr;
    }
}

static void VS_CC DlssnrFreeImpl(void *instanceData, VSCore * /*core*/, const VSAPI *vsapi) {
    auto *d = static_cast<FilterData *>(instanceData);
    if (d->node) vsapi->freeNode(d->node);
    // Stop the bridge before tearing down the contexts it observes. The D3D12
    // + NGX contexts themselves move into the hot context instead of being
    // destroyed: mpv re-runs the VS script on every seek, and a warm context
    // makes the next filter instance near-free to create.
    // 生命周期锁:与并发 create 的热匹配/冷初始化串行(见 g_lifecycleMutex
    // 定义处注释)。BridgeStop 的 5s 上限在此锁内,极端 wedge 场景会推迟
    // 并发的 create —— 正确性优先。
    std::lock_guard<std::mutex> lifecycleLock(g_lifecycleMutex);
    // 停掉本实例的 resize watcher(实例与线程生命周期对齐;watcher 线程
    // 检测到事件即自行退出,无需等待)。
    if (d->resizeWatchStop) {
        vsdlssnr::MpvResizeWatchStop(d->resizeWatchStop);
        d->resizeWatchStop = nullptr;
    }
    vsdlssnr::BridgeStop(d->params);
    // 卸载落账(2026-10-04 评审修):stats mapping 进程级存活,实例死后面
    // 板读到的恒是冻结末帧 + "已连接"。发布一次 unloaded 死亡体(seq 推进,
    // gpuName 空 → 面板会话区回落"未加载滤镜"兜底文案,统计清零):seek
    // 间隙闪现一拍即被新实例逐帧体覆盖,真正移除滤镜则持续显示实态。
    {
        vsdlssnr::StatsPayload st{};
        vsdlssnr::CopyStatStr(st.filterState, "unloaded");
        vsdlssnr::PublishStats(st);
    }
    // FG 缓存帧:最后一个引用(缓存自留),先于 filter 释放。
    for (int i = 0; i < kFgMultMax; ++i) {
        if (d->fgCache[i]) vsapi->freeFrame(d->fgCache[i]);
    }
    if (d->initOk && d->d3d12 && d->ngx) {
        Hot().d3d12 = std::move(d->d3d12);
        Hot().ngx = std::move(d->ngx);
        Hot().ngxDllPath = std::move(d->ngxDllPath);
        Hot().width = d->width;
        Hot().height = d->height;
        Hot().depth = d->depth;
        Hot().subW = d->subW;
        Hot().subH = d->subH;
        Hot().isRgb = d->isRgb;
        Hot().fgDllPath = std::move(d->fgDllPath);
        Hot().valid = true;
        // 探针:停放行 —— 与下一次 create 的 "hot rebind kept"/"re-init"
        // 行配对,seek 生命周期序列(bridge stopped → freed → started →
        // rebind)在 timing log 里闭环;mpv 在停放后退出也有尾行可查。
        char msg[128];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: filter freed (hot parked %dx%dd%d fg=%d)",
                      d->width, d->height, d->depth, d->fgActive ? 1 : 0);
        vsdlssnr::TimingStatusLine(msg);
    }
    delete d;
}

static void VS_CC DlssnrFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    try {
        DlssnrFreeImpl(instanceData, core, vsapi);
    } catch (...) { // ABI 边界兜底(见 DlssnrGetFrame 注释);Free 无处上报,吞掉
    }
}

static void VS_CC DlssnrCreateImpl(
    const VSMap *in, VSMap *out, void * /*userData*/, VSCore *core, const VSAPI *vsapi) {
    VSNode *node = vsapi->mapGetNode(in, "clip", 0, nullptr);
    if (!node) {
        vsapi->mapSetError(out, "dlssnr.Enhance: missing clip argument");
        return;
    }
    const VSVideoInfo *vi = vsapi->getVideoInfo(node);

    // 原生格式收编(2026-09-24,撤 wrapper 自设的 420P8/P10 闸门):
    //   YUV 计划族 × {420,422,444} × 8/10/12/14/16bit —— ConvertIn 按
    //   subSampling/位深参数化采样(色度平面几何 = VS ceil 规则)。
    //   RGB 计划族(RGBP*,平面序 G/B/R)—— 零矩阵直读直写。
    // 其余(GRAY/packed RGB/YUVA 等)仍由 vpy 预转换或直通。
    const VSVideoFormat &vf = vi->format;
    const bool isYuv = vf.colorFamily == cfYUV && vf.sampleType == stInteger &&
                       vf.bitsPerSample >= 8 && vf.bitsPerSample <= 16 &&
                       vf.subSamplingW <= 1 && vf.subSamplingH <= 1;
    const bool isRgbPlanar = vf.colorFamily == cfRGB && vf.sampleType == stInteger &&
                             vf.subSamplingW == 0 && vf.subSamplingH == 0 &&
                             vf.bitsPerSample >= 8 && vf.bitsPerSample <= 16 &&
                             // 计划 RGB = 3 平面;packed(RGB24/48)单平面
                             // 不接(vpy 兜底转换),避免交错打包路径。
                             vf.numPlanes == 3;
    if (!vsh::isConstantVideoFormat(vi) || (!isYuv && !isRgbPlanar)) {
        vsapi->mapSetError(out, "dlssnr.Enhance: clip must be planar YUV (420/422/444, 8-16bit) or planar RGB (constant format)");
        vsapi->freeNode(node);
        return;
    }

    auto *d = new FilterData();
    d->node = node;
    d->width = vi->width;
    d->height = vi->height;
    d->depth = vi->format.bitsPerSample;
    d->subW = vi->format.subSamplingW;
    d->subH = vi->format.subSamplingH;
    d->isRgb = isRgbPlanar;
    d->srcFrames = vi->numFrames; // FG 尾部映射;0 = 未知长度(尾部钳制禁用)

    DlssnrParams initial{}; // member initializers are the default authority
    // NR 总开关(0/1):0 = 跳过 D3D12/NGX 全部初始化,滤镜纯直通零开销,
    // FG 一并不可用;面板开关(live)与 ini/vpy 均可控制。
    ApplyFlagArg(in, vsapi, "nr_enabled", initial.nrEnabled);
    ApplyIntArg(in, vsapi, "preset", initial.preset, kPresetMin, kPresetMax);
    ApplyIntArg(in, vsapi, "style", initial.style, kStyleMin, kStyleMax);
    ApplyFloatArg(in, vsapi, "intensity", initial.intensity, kStrengthMin, kStrengthMax);
    ApplyFloatArg(in, vsapi, "local_tone", initial.localToneStrength, kStrengthMin, kStrengthMax);
    ApplyFloatArg(in, vsapi, "local_structure", initial.localStructureStrength, kStrengthMin, kStrengthMax);
    ApplyFloatArg(in, vsapi, "skin_structure", initial.skinStructureStrength, kSkinMin, kSkinMax);
    ApplyFlagArg(in, vsapi, "use_auto_mask", initial.useAutoMask);
    ApplyFloatArg(in, vsapi, "residual_multiplier", initial.residualMultiplier, kResidualMultMin, kResidualMultMax);
    ApplyFloatArg(in, vsapi, "residual_saturation", initial.residualSaturation, kResidualFineMin, kResidualFineMax);
    ApplyFloatArg(in, vsapi, "residual_lightness", initial.residualLightness, kResidualFineMin, kResidualFineMax);
    ApplyFloatArg(in, vsapi, "shadow_structure", initial.shadowStructureMultiplier, kResidualFineMin, kResidualFineMax);
    ApplyFloatArg(in, vsapi, "reflection_glow", initial.reflectionGlowMultiplier, kResidualFineMin, kResidualFineMax);
    ApplyIntArg(in, vsapi, "input_resolution", initial.inputResolutionPercent, kResPctMin, kResPctMax);
    // scaling_enabled=0 drops the residual pipeline entirely (input_resolution ignored)
    ApplyFlagArg(in, vsapi, "scaling_enabled", initial.scalingEnabled);
    // NVOF 光流质量 0-5(0 = 零 guidance);>0 且驱动支持时启用真运动矢量
    ApplyIntArg(in, vsapi, "motion_vector_quality", initial.motionVectorQuality, kOfQualityMin, kOfQualityMax);
    // FFX 光流质量 0-2(0 = 无;1 = 性能,2 = 质量;两后端档位分字段,
    // 面板单下拉按当前后端读写)
    ApplyIntArg(in, vsapi, "ffx_quality", initial.ffxQuality, kFfxQualityMin, kFfxQualityMax);
    // 光流输入跟随内部降采样(scaling 启用时 NVOF 按内部尺寸计算)
    ApplyFlagArg(in, vsapi, "nvof_follow_scaling", initial.nvofFollowScaling);
    // DLSS 帧生成(0/1):激活时每源帧产出 M 帧(1 真实 + M-1 插值),
    // 输出帧时长 = 源时长/M(mpv vapoursynth 契约),失败优雅回退 1:1。
    // 挂 DLSSNR 之后 —— backbuffer = NR 输出。
    ApplyFlagArg(in, vsapi, "fg_enabled", initial.fgEnabled);
    // 插帧倍数 2-6(live 参数,源帧边界生效;创建值定 vi.fps 元数据)
    ApplyIntArg(in, vsapi, "fg_multiplier", initial.fgMultiplier, kFgMultMin, kFgMultMax);
    // FG 路由 0=自动(预载 0.3.x hook 代理)/1=纯官方(不预载;进程级,
    // 重启生效)
    ApplyIntArg(in, vsapi, "fg_route", initial.fgRoute, kFgRouteMin, kFgRouteMax);
    // v23 实验性:补帧 HDR 域插帧(DLSSG 直接吃 TrueHDR 输出)。部分驱动
    // 此路径插值帧压高光 = 闪烁,默认 0。仅 HDR+FG 会话有意义。
    ApplyFlagArg(in, vsapi, "fg_hdr_interp", initial.fgHdrInterp);
    ApplyIntArg(in, vsapi, "of_backend", initial.ofBackend, kOfBackendMin, kOfBackendMax);
    // 抗闪烁时域稳定器 0-4(live;0 = 关):NR 残差的跨帧时域平均,
    // 见 kAntiFlickerMin 注释。
    ApplyIntArg(in, vsapi, "anti_flicker", initial.antiFlicker, kAntiFlickerMin, kAntiFlickerMax);
    // Panel-saved profile (dlssnr_ui.ini) overrides .vpy values when present;
    // the panel's CURRENT payload (last live state) overrides the ini. Without
    // the adopt step a seek rebuilds the filter from stale ini/vpy values —
    // the bridge poll skips the existing payload (history), so the panel's
    // parameters only came back after touching the panel again.
    // RTX Video(v22 起参数住 DlssnrParams):vpy args 先于 ini 应用(与其
    // 他参数同序:vpy 默认 → ini → 面板 payload)。
    ApplyIntArg(in, vsapi, "vsr_mode", initial.rtxVsrMode, kVsrModeMin, kVsrModeMax);
    ApplyFloatArg(in, vsapi, "vsr_scale", initial.rtxVsrScale, kVsrScaleMin, kVsrScaleMax);
    ApplyIntArg(in, vsapi, "vsr_strength", initial.rtxVsrStrength, kVsrStrengthMin, kVsrStrengthMax);
    ApplyIntArg(in, vsapi, "hdr_enabled", initial.rtxHdrEnabled, 0, 1);
    ApplyIntArg(in, vsapi, "hdr_contrast", initial.rtxHdrContrast, kHdrContrastMin, kHdrContrastMax);
    ApplyIntArg(in, vsapi, "hdr_saturation", initial.rtxHdrSaturation, kHdrSaturationMin, kHdrSaturationMax);
    ApplyIntArg(in, vsapi, "hdr_middle_gray", initial.rtxHdrMiddleGray, kHdrMiddleGrayMin, kHdrMiddleGrayMax);
    ApplyIntArg(in, vsapi, "hdr_peak_nits", initial.rtxHdrMaxLuminance, kHdrMaxLumMin, kHdrMaxLumMax);
    const bool iniLoaded = vsdlssnr::BridgeLoadIni(initial);
    const bool payloadAdopted = vsdlssnr::BridgeAdoptPanelPayload(initial);
    // mode=1 的 mpv 窗口客户区探测(三层裁决的唯一 probe 写入点)。
    ResolveRtxParams(initial, d->width, d->height);
    // 探针:三层参数源(vpy 默认 → ini → 面板 payload)的最终裁决值。
    // "参数没生效/拖进度条回去了"类问题(#37)一行定位:ini/payload 哪层
    // 参与了、create-time 三元组最终是什么,一眼可查。
    {
        // 布局标签:420/422/444/RGB(消费方几何/核形态的创建时事实)。
        const char *layout = d->isRgb ? "RGB"
                                      : d->subW == 0 && d->subH == 0 ? "444"
                                      : d->subW == 1 && d->subH == 0 ? "422"
                                      : d->subW == 1 && d->subH == 1 ? "420" : "?";
        char msg[288];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: create params %dx%dd%d %s ini=%d payload=%d -> nr=%d preset=%d res=%d%% scaling=%d of=%d ffx=%d follow=%d fg=%d mult=%d route=%d fg_hdr=%d af=%d",
                      d->width, d->height, d->depth, layout, iniLoaded ? 1 : 0, payloadAdopted ? 1 : 0,
                      initial.nrEnabled ? 1 : 0, initial.preset, initial.inputResolutionPercent,
                      initial.scalingEnabled ? 1 : 0, initial.motionVectorQuality,
                      initial.ffxQuality,
                      initial.nvofFollowScaling ? 1 : 0,
                      initial.fgEnabled ? 1 : 0,
                      initial.fgMultiplier, initial.fgRoute, initial.fgHdrInterp ? 1 : 0,
                      initial.antiFlicker);
        vsdlssnr::TimingStatusLine(msg);
    }
    d->params = std::make_shared<vsdlssnr::SharedParams>(initial);

    int dllErr = 0;
    const char *dllArg = vsapi->mapGetData(in, "ngx_dll", 0, &dllErr);
    if (!dllErr && dllArg && dllArg[0]) {
        try {
            // VS map strings are UTF-8; the path(const char*) ctor would
            // decode them as ANSI and mojibake non-ASCII paths.
            d->ngxDllPath = std::filesystem::path(reinterpret_cast<const char8_t *>(dllArg)).wstring();
        } catch (...) {
            d->ngxDllPath.clear();
        }
    }
    // Default: <plugin dir>/ngx/nvngx_dlssnr.dll
    if (d->ngxDllPath.empty()) {
        const VSPlugin *self = vsapi->getPluginByNamespace(PLUGIN_NAMESPACE, core);
        const char *selfPath = self ? vsapi->getPluginPath(self) : nullptr;
        if (selfPath) {
            try {
                // getPluginPath is UTF-8 as well (see above)
                const std::filesystem::path dir =
                    std::filesystem::path(reinterpret_cast<const char8_t *>(selfPath)).parent_path() / "ngx";
                d->ngxDllPath = vsdlssnr::SelectNgxDllVariant(dir);
            } catch (...) {
                d->ngxDllPath.clear();
            }
        }
    }

    // NR+FG 皆关时也照常解析 FG DLL 路径(重开时即最新值);热/冷初始化
    // 由下方各守卫跳过 —— 零设备、零显存、零 GPU。仅 NR 关而 FG 开:
    // 初始化照常,NR 评估在 ProcessFrame 内部跳过。

    // FG hook 代理 DLL(dlssg_for_sm86 0.3.x 的 version.dll;用户自备部署,
    // 与模型 DLL 同目录约定)。默认 <plugin dir>/ngx/version.dll。
    {
        int fgDllErr = 0;
        const char *fgDllArg = vsapi->mapGetData(in, "fg_dll", 0, &fgDllErr);
        if (!fgDllErr && fgDllArg && fgDllArg[0]) {
            try {
                d->fgDllPath = std::filesystem::path(reinterpret_cast<const char8_t *>(fgDllArg)).wstring();
            } catch (...) {
                d->fgDllPath.clear();
            }
        }
        if (d->fgDllPath.empty()) {
            const VSPlugin *self = vsapi->getPluginByNamespace(PLUGIN_NAMESPACE, core);
            const char *selfPath = self ? vsapi->getPluginPath(self) : nullptr;
            if (selfPath) {
                try {
                    const std::filesystem::path dir =
                        std::filesystem::path(reinterpret_cast<const char8_t *>(selfPath)).parent_path() / "ngx";
                    d->fgDllPath = (dir / FG_PROXY_DLL_NAME).wstring();
                } catch (...) {
                    d->fgDllPath.clear();
                }
            }
        }
    }

    // Eager init: the D3D12/NGX bring-up costs ~1s (165MB snippet DLL load +
    // CreateFeature + first-evaluate warm-up). Doing it here — script
    // execution, before mpv starts the playback clock — keeps that whole
    // warm-up period out of playback. A hot context from the previous filter
    // instance (freed on the last seek) skips the bring-up entirely —
    // including for a DIFFERENT video size: Rebind rebuilds the frame
    // resources + feature inside one pool-sealed pass. Only a changed
    // snippet DLL forces the cold path. Failure keeps the passthrough
    // fallback semantics below.
    char err[256]{};
    // depth 参与 hotMatch:同尺寸换深度(P8↔P10)走冷重建 —— 热上下文的
    // 槽纹理按旧位深建,R8 纹理遇 P10 打包 = 数据撕裂(#40-① 同族)。
    // RTX/FG 档位(vsr_mode/scale/hdr/fg_enabled/fg_hdr_interp)不参与
    // (2026-09-27):Rebind 形态段热重建槽资源 + VSR/HDR/FG 会话,切档
    // 不再付冷启动;代理路径仍参与(形态段预载按同路径补)。
    // NR+FG+RTX 皆关 = 跳过热复用(实例纯直通,停泊上下文原样保留,重开
    // 秒回);仅 NR 关而 FG/RTX 开仍需热复用(设备与上下文都在用)。
    // ctx 载荷从合并后的 DlssnrParams 折出(probe 已在 ResolveRtxParams
    // 填入)。
    const RtxVideoParams rtx = RtxFromParams(initial);
    // 生命周期锁:覆盖热匹配判定 → 冷初始化结束(见 g_lifecycleMutex 定义
    // 处注释)。冷初始化 ~1s 在锁内 —— 并发的 Free 最多等一个冷启动周期。
    std::lock_guard<std::mutex> lifecycleLock(g_lifecycleMutex);
    const bool rtxRequested = rtx.vsrMode > 0 || rtx.hdrEnabled != 0;
    // FG 代理时序门:hook 代理必须先于 NGX 核心加载才能挂上钩(依赖核心的
    // 加载事件);热上下文的核心早已在,FG off→on 原地 rebind 补预载 =
    // 钩子恒挂不上,官方 DLSSG 拒 0xBAD0000B(2026-10-03 实锤:面板中途
    // 开 FG 恒"未生效(复制帧)",后续 seek 走热复用也救不回)。代理未加载
    // 时强制冷初始化 —— Initialize 的预载先于核心,设计路径。每进程只强
    // 制一次:预载成功后 ProxyLoaded 恒真自然回归热复用;version.dll 缺失
    // 时防每个 seek 都付一遍冷启动(FG 降级复制帧,与旧语义一致)。
    const bool fgProxyNeedsCold = initial.fgEnabled &&
        std::clamp(initial.fgRoute, kFgRouteMin, kFgRouteMax) == kFgRouteAuto &&
        !d->fgDllPath.empty() &&
        !vsdlssnr::DlssfgContext::ProxyLoaded() &&
        vsdlssnr::dlssfg_gate::GpuFamilyPrefersProxy();
    static bool fgProxyColdForced = false; // 每进程一次(见上)
    const bool fgProxyCold = fgProxyNeedsCold && !fgProxyColdForced;
    fgProxyColdForced |= fgProxyCold;
    const bool hotMatch = (initial.nrEnabled || initial.fgEnabled || rtxRequested) &&
                          !fgProxyCold &&
                          Hot().valid &&
                          Hot().ngxDllPath == d->ngxDllPath &&
                          Hot().width == d->width && Hot().height == d->height &&
                          Hot().depth == d->depth &&
                          // 布局(420/422/444/RGB)参与:同尺寸同深度换布局 =
                          // 色度面几何/核形态变化,必须冷重建。
                          Hot().subW == d->subW && Hot().subH == d->subH &&
                          Hot().isRgb == d->isRgb &&
                          Hot().fgDllPath == d->fgDllPath;
    if (hotMatch) {
        d->d3d12 = std::move(Hot().d3d12);
        d->ngx = std::move(Hot().ngx);
        Hot().valid = false;
        Hot().ngxDllPath.clear();
        if (d->ngx->Rebind(d->params.get(), d->width, d->height, d->depth, rtx, err, sizeof(err))) {
            d->initOk = true;
            // Filter is live: start the mpv-side parameter bridge
            if (!vsdlssnr::BridgeStart(d->params)) WarnBridgeFailed(core, vsapi);
            char msg[128];
            std::snprintf(msg, sizeof(msg), "vs_dlssnr ready from hot context (%dx%d)", d->width, d->height);
            vsapi->logMessage(mtInformation, msg, core);
        } else {
            // The warm context is wedged (NGX state lost): drop it and fall
            // through to a full re-initialization below.
            char msg[512];
            std::snprintf(msg, sizeof(msg), "vs_dlssnr hot rebind failed, re-initializing: %s", err);
            vsapi->logMessage(mtWarning, msg, core);
            vsdlssnr::TimingStatusLine(msg); // GUI mpv 不透传 logMessage,失败必须进 timing log
            d->ngx.reset();
            d->d3d12.reset();
        }
    }
    if ((initial.nrEnabled || initial.fgEnabled || rtxRequested) && !d->initOk && !d->d3d12) {
        // Any parked context left over (snippet DLL / size / depth / layout /
        // proxy path mismatch) must be torn down BEFORE a cold init: the IAT
        // hook and the NGX core are process-global singletons. A second full
        // Initialize could never install the hook (its owner CAS is held by
        // the parked context) and its eventual Shutdown1 would tear down the
        // shared core under the parked feature — silently degrading every
        // later resolution. RTX/FG 档位变化不进此路径(Rebind 形态段热重建)。
        if (Hot().valid) {
            Hot().ngx->Shutdown();
            Hot().ngx.reset();
            Hot().d3d12.reset();
            Hot().valid = false;
            Hot().ngxDllPath.clear();
        }
        d->d3d12 = std::make_unique<vsdlssnr::D3D12Context>();
        d->ngx = std::make_unique<vsdlssnr::DlssnrContext>();
        if (d->d3d12->Initialize(err, sizeof(err)) &&
            d->ngx->Initialize(*d->d3d12, d->ngxDllPath.c_str(), d->fgDllPath.c_str(),
                               d->width, d->height, d->depth, d->params.get(), rtx,
                               d->subW, d->subH, d->isRgb,
                               err, sizeof(err))) {
            d->initOk = true;
            // Filter is live: start the mpv-side parameter bridge
            if (!vsdlssnr::BridgeStart(d->params)) WarnBridgeFailed(core, vsapi);
            char msg[128];
            std::snprintf(msg, sizeof(msg), "vs_dlssnr ready (%dx%d)", d->width, d->height);
            vsapi->logMessage(mtInformation, msg, core);
        } else {
            char msg[512];
            std::snprintf(msg, sizeof(msg),
                          "vs_dlssnr init failed, falling back to passthrough: %s", err);
            vsapi->logMessage(mtWarning, msg, core);
            vsdlssnr::TimingStatusLine(msg); // GUI mpv 不透传 logMessage,失败必须进 timing log
            // 面板可见状态:D3D12/NGX 初始化失败 = 本实例整体直通。原因串
            // 可能含 D3D12 debug-layer 文本,消毒控制字符后再进 stats。
            char safe[288];
            vsdlssnr::SanitizeJsonDetail(err, safe, sizeof(safe));
            vsdlssnr::StatsPayload st{};
            vsdlssnr::CopyStatStr(st.filterState, "passthrough");
            vsdlssnr::CopyStatStr(st.stateDetail, safe);
            vsdlssnr::PublishStats(st);
        }
    }
    if (!initial.nrEnabled && !initial.fgEnabled && !rtxRequested) {
        // NR + FG + RTX 皆关:热/冷初始化全部跳过 —— 零设备、零显存、零 GPU,
        // 滤镜纯直通(getFrame 原帧交还)。仅 NR 关而 FG/RTX 开时不走此分支:
        // 初始化照常,NR 评估由 ProcessFrame 内部门控跳过。桥接照常启动:
        // 面板仍被拉起并可实时控制 —— 已激活会话 live 重开立即恢复;创建即
        // 全关的实例重开需下个 seek(停泊热上下文原样保留,同参数重开走
        // 秒回的热复用)。
        vsdlssnr::StatsPayload st{};
        vsdlssnr::CopyStatStr(st.filterState, "passthrough");
        vsdlssnr::CopyStatStr(st.stateDetail, "NR+FG+RTX disabled (panel/vpy)");
        vsdlssnr::PublishStats(st);
        if (!vsdlssnr::BridgeStart(d->params)) WarnBridgeFailed(core, vsapi);
        vsdlssnr::TimingStatusLine("DLSSNR STATUS: NR+FG+RTX disabled; passthrough (zero GPU)");
    }

    // FG 多帧输出判定(两条初始化路径汇合):会话激活 = 每源帧产出 M 帧
    // (输出帧时长 = 源时长/M,mpv 逐帧认 _DurationNum/_DurationDen);
    // 否则 1:1(FG 失败的降级语义)。vi 副本按创建值倍增 fps —— 纯元数据
    // (mpv 不据此节拍,但下游工具/时长估算消费它)。
    d->fgActive = d->initOk && d->ngx && d->ngx->FgActive();
    // RTX 输出几何(init 后从 context 读回 —— pipe/out 的最终裁决在
    // Initialize 内含 capability/倍率旁路)。未初始化实例 = 源几何直通。
    d->outW = d->width;
    d->outH = d->height;
    d->hdrOut = false;
    d->rtxActive = false;
    d->outFi = vi->format; // VS4:VSVideoInfo.format 为内嵌值(VS4 API 形态)
    if (d->initOk && d->ngx) {
        d->outW = d->ngx->OutWidth();
        d->outH = d->ngx->OutHeight();
        d->hdrOut = d->ngx->HdrActive();
        d->rtxActive = d->ngx->RtxActive();
        if (d->hdrOut) {
            // HDR 输出契约 = YUV420P10(BT.2020 PQ limited;props 逐帧写)。
            VSVideoFormat p10{};
            if (!vsapi->getVideoFormatByID(&p10, pfYUV420P10, core)) {
                vsapi->mapSetError(out, "dlssnr.Enhance: getVideoFormatByID(YUV420P10) failed");
                // 引用放出先于实例析构(对齐 format-reject 路径;漏放 = 上游
                // 整图随 core 生命周期滞留,2026-10-04 评审修)。
                if (d->node) vsapi->freeNode(d->node);
                delete d;
                return;
            }
            d->outFi = p10;
        }
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: output geometry %dx%d fmt=%dbpp hdr=%d rtx=%d (src %dx%dd%d)",
                      d->outW, d->outH, d->outFi.bitsPerSample, d->hdrOut ? 1 : 0,
                      d->rtxActive ? 1 : 0, d->width, d->height, d->depth);
        vsdlssnr::TimingStatusLine(msg);
        // 窗口 resize 跟随(mode=1):watcher 全套在 mpv_host(2026-10-04 拆出)。
        // refH = 本次创建的探测值;stop 句柄归 FilterData,Free 时 MpvResizeWatchStop。
        d->resizeWatchStop =
            vsdlssnr::MpvResizeWatchStart(d->width, d->height, initial.rtxVsrAutoHeight,
                                          initial.rtxVsrMode == 1);
    }
    VSVideoInfo viOut = *vi;
    viOut.width = d->outW;
    viOut.height = d->outH;
    viOut.format = d->outFi; // VS4:VSVideoInfo.format 为内嵌值
    if (d->fgActive) {
        // 创建契约读合并终值 initial(而非 live 快照):BridgeStart 已在上方
        // 启动,面板 payload 可在其后改 live 字段(fgMultiplier 是 live 参)
        // —— vi.fps ×M 的创建契约在竞态窗口内曾非确定(2026-10-04)。
        d->fgCreateMult = std::clamp(initial.fgMultiplier, kFgMultMin, kFgMultMax);
        viOut.fpsNum *= d->fgCreateMult;
        // 帧数同步 ×M:VS4 里 n >= numFrames 的请求会被核心拒为越界 ——
        // mpv 顺序拉流到尾帧时会提前 EOF(尾段丢插值帧)。
        if (viOut.numFrames > 0) viOut.numFrames *= d->fgCreateMult;
        char msg[160];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: fg active, output fps x%d (%I64d/%I64d -> %I64d/%I64d)",
                      d->fgCreateMult, vi->fpsNum, vi->fpsDen, viOut.fpsNum, viOut.fpsDen);
        vsdlssnr::TimingStatusLine(msg);
    }

    VSFilterDependency deps[]{ { node, rpStrictSpatial } };
    // fmParallel: mpv keeps multiple frame requests in flight; each getFrame
    // runs on its own D3D12 slot (slot pool caps the concurrency at
    // kSlotCount), which overlaps CPU pack/unpack with the GPU work of other
    // slots — the old fmUnordered path idled the GPU between frames.
    // FG 模式下 getFrame 内部经 fgMutex 串行(FG 链按源帧序),缓存条目
    // 保证同源帧的两条输出只处理一次。
    vsapi->createVideoFilter(out, "Enhance", &viOut, DlssnrGetFrame, DlssnrFree,
                             fmParallel, deps, 1, d, core);
}

static void VS_CC DlssnrCreate(
    const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    try {
        DlssnrCreateImpl(in, out, userData, core, vsapi);
    } catch (...) { // ABI 边界兜底(见 DlssnrGetFrame 注释)
        vsapi->mapSetError(out, "dlssnr.Enhance: internal error (exception)");
    }
}

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
    // Pin the DLL: mpv's vf_vapoursynth tears down and recreates the whole VS
    // core on every seek, which unloads and reloads every plugin DLL. Without
    // the pin, the process-level hot context (device + NGX feature + slot
    // pool) would die with our CRT heap at unload and every seek would pay
    // the ~1s bring-up again. Pinned modules never unload (FreeLibrary is
    // ignored); the OS reclaims them at process exit.
    HMODULE self = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&VapourSynthPluginInit2), &self);
    vspapi->configPlugin(PLUGIN_IDENTIFIER, PLUGIN_NAMESPACE, PLUGIN_NAME,
                         VS_MAKE_VERSION(0, 1), VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction(
        "Enhance",
        "clip:vnode;"
        "ngx_dll:data:opt;"
        "nr_enabled:int:opt;"
        "preset:int:opt;"
        "style:int:opt;"
        "intensity:float:opt;"
        "local_tone:float:opt;"
        "local_structure:float:opt;"
        "skin_structure:float:opt;"
        "use_auto_mask:int:opt;"
        "residual_multiplier:float:opt;"
        "residual_saturation:float:opt;"
        "residual_lightness:float:opt;"
        "shadow_structure:float:opt;"
        "reflection_glow:float:opt;"
        "scaling_enabled:int:opt;"
        "input_resolution:int:opt;"
        "motion_vector_quality:int:opt;"
        "ffx_quality:int:opt;"
        "nvof_follow_scaling:int:opt;"
        "fg_enabled:int:opt;"
        "fg_multiplier:int:opt;"
        "fg_route:int:opt;"
        "fg_hdr_interp:int:opt;"
        "of_backend:int:opt;"
        "anti_flicker:int:opt;"
        "vsr_mode:int:opt;"
        "vsr_scale:float:opt;"
        "vsr_strength:int:opt;"
        "hdr_enabled:int:opt;"
        "hdr_contrast:int:opt;"
        "hdr_saturation:int:opt;"
        "hdr_middle_gray:int:opt;"
        "hdr_peak_nits:int:opt;"
        "fg_dll:data:opt;",
        "clip:vnode;",
        DlssnrCreate, nullptr, plugin);
}
