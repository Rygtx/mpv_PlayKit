// vs_dlssnr - NVIDIA DLSSNR (NGX Feature 18) filter for VapourSynth.
// Ported from Magpie experimental (github.com/SAOG0721/Magpie).
// Model file: nvngx_dlssnr.dll (310.9.0), zero-guidance mode (Magpie
// guidanceMode=1 Force Zero), same-resolution processing.

#include "bridge.h"
#include "d3d12_context.h"
#include "dlssnr_context.h"
#include "dlssnr_params.h"
#include "panel_ipc.h"
#include "shared_params.h"

#include "VapourSynth4.h"
#include "VSHelper4.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <windows.h>

namespace {

constexpr char PLUGIN_IDENTIFIER[] = "dev.rygtx.vsdlssnr";
constexpr char PLUGIN_NAMESPACE[] = "dlssnr";
constexpr char PLUGIN_NAME[] = "NVIDIA DLSSNR filter (Magpie port)";
constexpr char SNIPPET_DLL_NAME[] = "nvngx_dlssnr.dll";
constexpr char FG_PROXY_DLL_NAME[] = "version.dll"; // dlssg_for_sm86 原生代理

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
    // come from the .vpy call).
    std::unique_ptr<vsdlssnr::SharedParams> params;

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
    int depth = 0; // YUV 位深(8/10);同尺寸换深度必须走冷重建(hotMatch 拦截)
    // Failure-log latch: a wedged context fails every frame at frame rate —
    // log the first failure only, re-arm on the next success.
    std::atomic<bool> failureLogged{ false };

    // ---- DLSS FG 双输出 ----
    // fgDoubled = 创建时 FG 激活(proxy 初始化成功),vi.fps 已 ×2,输出
    // 帧索引 n 映射:偶数 = 真实帧 k=n/2,奇数 = 插值帧(源帧 k=(n+1)/2
    // 的一次处理同时产出两帧)。fgMutex 串行处理与缓存(FG 链的 NVOF/
    // DLSSG 历史本质按源帧顺序,缓存防止同源帧被并发双跑)。
    std::mutex fgMutex;
    bool fgDoubled = false;
    std::wstring fgDllPath;
    int fgCacheK = -1;
    const VSFrame *fgCacheReal = nullptr; // 各持一个引用,Free 时释放
    const VSFrame *fgCacheGen = nullptr;
};

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
    // FG 状态参与 hotMatch:开关/代理路径变化 = 槽资源形态变化(FG 纹理
    // 有无),必须冷重建。
    bool fgEnabled = false;
    std::wstring fgDllPath;
    bool valid = false;
};
HotContext &Hot() {
    static HotContext *inst = new HotContext();
    return *inst;
}

} // namespace

// FG 双输出共用的属性处理:输出帧各占源帧一半时长(_Duration,存在才写)。
static void HalveDuration(VSFrame *frame, const VSAPI *vsapi) noexcept {
    VSMap *props = vsapi->getFramePropertiesRW(frame);
    int err = 0;
    const double dur = vsapi->mapGetFloat(props, "_Duration", 0, &err);
    if (!err) {
        vsapi->mapSetFloat(props, "_Duration", dur * 0.5, maReplace);
    }
}

// 从源帧复制三平面(YUV420)到 dst —— 帧失败的降级路径。
static void CopyPlanes(const VSFrame *src, VSFrame *dst, const VSAPI *vsapi,
                       int width, int height) noexcept {
    const VSVideoFormat *fi = vsapi->getVideoFrameFormat(dst);
    const int bpp = fi->bytesPerSample;
    const int cw = (width + 1) >> 1, ch = (height + 1) >> 1;
    for (int p = 0; p < 3; ++p) {
        const int pw = p == 0 ? width : cw;
        const int ph = p == 0 ? height : ch;
        vsh::bitblt(vsapi->getWritePtr(dst, p), vsapi->getStride(dst, p),
                    vsapi->getReadPtr(src, p), vsapi->getStride(src, p),
                    static_cast<size_t>(pw) * bpp, static_cast<size_t>(ph));
    }
}

static const VSFrame *VS_CC DlssnrGetFrame(
    int n, int activationReason, void *instanceData, void ** /*frameData*/,
    VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    auto *d = static_cast<FilterData *>(instanceData);

    if (activationReason == arInitial) {
        // FG 双输出:输出 n 的源帧 = (n+1)/2(偶 n=2k → 真实帧 k;奇
        // n=2k+1 → 处理源帧 k+1 产出区间 (k,k+1) 的插值帧)。
        const int k = d->fgDoubled ? (n + 1) / 2 : n;
        vsapi->requestFrameFilter(k, d->node, frameCtx);
        return nullptr;
    }
    if (activationReason != arAllFramesReady) return nullptr;

    // FG 双输出:处理与缓存串行(FG 链的 NVOF/DLSSG 历史本质按源帧顺序;
    // 同源帧的两条输出竞争时,后到者直接命中缓存)。
    std::unique_lock<std::mutex> fgLock(d->fgMutex, std::defer_lock);
    const int k = d->fgDoubled ? (n + 1) / 2 : n;
    const bool wantGen = d->fgDoubled && (n & 1) != 0;
    if (d->fgDoubled) {
        fgLock.lock();
        if (k == d->fgCacheK && d->fgCacheReal && d->fgCacheGen) {
            // 缓存命中:同源帧的另一条输出。交出一个新引用(缓存自留一个)。
            const VSFrame *hit = wantGen ? d->fgCacheGen : d->fgCacheReal;
            const VSFrame *out = vsapi->addFrameRef(hit);
            return out;
        }
    }

    const VSFrame *src = vsapi->getFrameFilter(k, d->node, frameCtx);

    const VSMap *props = vsapi->getFramePropertiesRO(src);
    // 源色彩元数据(仓内 props 读取首例):矩阵/范围驱动 YUV↔RGB 展开,
    // 缺失/未知回落 709 limited 并留痕(值变化才再 log,atomic 边沿去重)。
    // VS _Matrix:1=BT.709,5(BT470BG)/6(SMPTE170M)=601 族,4(XYZ)=601 近似
    // (XYZ→RGB 本需色度适配矩阵,近似到 601 是 D65 录制内容的可用降级);
    // 9(BT.2020)/0(GBR) 等超范围,按 709 处理 + log 可见(vpy 层已有
    // HDR 直通守卫,到这里的多半是属性缺失的裸流)。
    int perr = 0;
    const int matrixProp = vsapi->mapGetInt(props, "_Matrix", 0, &perr);
    const bool matrixKnown = perr == 0;
    perr = 0;
    const int rangeProp = vsapi->mapGetInt(props, "_ColorRange", 0, &perr);
    const bool rangeKnown = perr == 0;
    vsdlssnr::ColorMatrix matrix = vsdlssnr::ColorMatrix::BT709;
    if (matrixKnown && (matrixProp == 4 || matrixProp == 5 || matrixProp == 6)) {
        matrix = vsdlssnr::ColorMatrix::BT601;
    }
    vsdlssnr::ColorRange range = vsdlssnr::ColorRange::Limited;
    if (rangeKnown && rangeProp == 0) range = vsdlssnr::ColorRange::Full;
    {
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

    if (!d->initOk) {
        // passthrough on any setup failure; FG 双输出模式下输出计数仍是
        // 2×(fps 已翻倍):源帧复制入缓存,两条输出各交一个新引用。
        if (d->fgDoubled) {
            VSFrame *dup = vsapi->copyFrame(src, core);
            d->fgCacheK = k;
            if (d->fgCacheReal) vsapi->freeFrame(d->fgCacheReal);
            if (d->fgCacheGen) vsapi->freeFrame(d->fgCacheGen);
            d->fgCacheReal = dup;                       // 接管 copyFrame 引用
            d->fgCacheGen = vsapi->addFrameRef(dup);
            const VSFrame *ret = vsapi->addFrameRef(wantGen ? d->fgCacheGen : d->fgCacheReal);
            vsapi->freeFrame(src);
            return ret;
        }
        return src; // reference handed off to the caller
    }

    const VSVideoFormat *fi = vsapi->getVideoFrameFormat(src);
    VSFrame *out = vsapi->newVideoFrame(fi, d->width, d->height, src, core);
    // FG:第二个输出帧(插值帧;eval 失败/降级时由下方复制真实帧填充)。
    VSFrame *genFrame = d->fgDoubled ? vsapi->newVideoFrame(fi, d->width, d->height, src, core) : nullptr;

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
    uint8_t *genPlanes[3]{};
    int64_t genStrides[3]{};
    if (genFrame) {
        for (int p = 0; p < 3; ++p) {
            genPlanes[p] = vsapi->getWritePtr(genFrame, p);
            genStrides[p] = vsapi->getStride(genFrame, p);
        }
    }

    char err[256]{};
    char timing[128]{};
    static const bool timingEnabled = GetEnvironmentVariableA("VSDLSSNR_TIMING", nullptr, 0) != 0;
    // NGX history-reset policy (frame-gap heuristic) lives in DlssnrContext;
    // only the frame index is forwarded here.
    bool fgEvaluated = false;
    if (!d->ngx->ProcessFrame(srcPlanes, srcStrides, dstPlanes, dstStrides,
                              genPlanes, genStrides, fgEvaluated,
                              d->width, d->height, k, matrix, range, err, sizeof(err),
                              timingEnabled ? timing : nullptr, timingEnabled ? sizeof(timing) : 0)) {
        if (!d->failureLogged.exchange(true)) {
            char msg[512];
            std::snprintf(msg, sizeof(msg), "vs_dlssnr frame %d failed: %s", k, err);
            vsapi->logMessage(mtWarning, msg, core);
            // 探针:首帧失败进 timing log(GUI mpv 完全看不到 logMessage;
            // 此前 PackInput/BeginFrameRecording 等不自带留痕的失败路径在这
            // 里是唯一记录,而记录通道本身不可见)。latch 保证不刷屏。
            vsdlssnr::TimingStatusLine(msg);
        }
        // Failed frames fall back to a plain copy of the source content so the
        // output planes are never left uninitialized. YUV420:色度半尺寸。
        CopyPlanes(src, out, vsapi, d->width, d->height);
    } else {
        d->failureLogged.store(false);
    }
    // FG 插值帧内容:eval 成功 = ProcessFrame 已回读到 genFrame;否则复制
    // 真实帧(复位/零光流/面板关/eval 降级 —— 重复帧优于垃圾插值)。
    if (genFrame && !fgEvaluated) {
        CopyPlanes(out, genFrame, vsapi, d->width, d->height);
    }

    if (timingEnabled && timing[0]) {
        // Throttle: log every 30th frame to avoid flooding the log channel
        // (fmParallel: the counter is bumped from several threads — order is
        // irrelevant for a throttle, atomicity is not).
        static std::atomic<int> timingFrameCount{ 0 };
        if (timingFrameCount.fetch_add(1, std::memory_order_relaxed) % 30 == 1) {
            char msg[192];
            std::snprintf(msg, sizeof(msg), "vs_dlssnr timing[%d]: %s", k, timing);
            vsapi->logMessage(mtInformation, msg, core);
        }
    }

    // FG 双输出:两帧入缓存(FG 链按源帧序,后到的奇/偶请求直接命中);
    // 每帧时长减半(_Duration 存在才写)。
    if (d->fgDoubled) {
        HalveDuration(out, vsapi);
        if (genFrame) HalveDuration(genFrame, vsapi);
        if (d->fgCacheReal) vsapi->freeFrame(d->fgCacheReal);
        if (d->fgCacheGen) vsapi->freeFrame(d->fgCacheGen);
        d->fgCacheK = k;
        d->fgCacheReal = out;      // 接管 newVideoFrame 的引用
        d->fgCacheGen = genFrame;
        const VSFrame *ret = vsapi->addFrameRef(wantGen ? genFrame : out);
        vsapi->freeFrame(src);
        return ret;
    }

    // getFrameFilter handed us a reference to src; release it or every source
    // frame leaks (~24MB per 1080p frame).
    vsapi->freeFrame(src);
    return out;
}

static void VS_CC DlssnrFree(void *instanceData, VSCore * /*core*/, const VSAPI *vsapi) {
    auto *d = static_cast<FilterData *>(instanceData);
    if (d->node) vsapi->freeNode(d->node);
    // Stop the bridge before tearing down the contexts it observes. The D3D12
    // + NGX contexts themselves move into the hot context instead of being
    // destroyed: mpv re-runs the VS script on every seek, and a warm context
    // makes the next filter instance near-free to create.
    vsdlssnr::BridgeStop(d->params.get());
    // FG 缓存帧:最后一个引用(缓存自留),先于 filter 释放。
    if (d->fgCacheReal) vsapi->freeFrame(d->fgCacheReal);
    if (d->fgCacheGen) vsapi->freeFrame(d->fgCacheGen);
    if (d->initOk && d->d3d12 && d->ngx) {
        Hot().d3d12 = std::move(d->d3d12);
        Hot().ngx = std::move(d->ngx);
        Hot().ngxDllPath = std::move(d->ngxDllPath);
        Hot().width = d->width;
        Hot().height = d->height;
        Hot().depth = d->depth;
        Hot().fgEnabled = d->fgDoubled;
        Hot().fgDllPath = std::move(d->fgDllPath);
        Hot().valid = true;
        // 探针:停放行 —— 与下一次 create 的 "hot rebind kept"/"re-init"
        // 行配对,seek 生命周期序列(bridge stopped → freed → started →
        // rebind)在 timing log 里闭环;mpv 在停放后退出也有尾行可查。
        char msg[128];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: filter freed (hot parked %dx%dd%d fg=%d)",
                      d->width, d->height, d->depth, d->fgDoubled ? 1 : 0);
        vsdlssnr::TimingStatusLine(msg);
    }
    delete d;
}

static void VS_CC DlssnrCreate(
    const VSMap *in, VSMap *out, void * /*userData*/, VSCore *core, const VSAPI *vsapi) {
    VSNode *node = vsapi->mapGetNode(in, "clip", 0, nullptr);
    if (!node) {
        vsapi->mapSetError(out, "dlssnr.Enhance: missing clip argument");
        return;
    }
    const VSVideoInfo *vi = vsapi->getVideoInfo(node);

    // YUV 原生(全切 RGBS):仅 YUV420P8/P10,输出同格式。其它格式由 vpy
    // 直通守卫(防不支持格式打断播放)。
    const bool is420P8 = vi->format.colorFamily == cfYUV && vi->format.sampleType == stInteger &&
                         vi->format.bitsPerSample == 8 && vi->format.subSamplingW == 1 &&
                         vi->format.subSamplingH == 1;
    const bool is420P10 = vi->format.colorFamily == cfYUV && vi->format.sampleType == stInteger &&
                          vi->format.bitsPerSample == 10 && vi->format.subSamplingW == 1 &&
                          vi->format.subSamplingH == 1;
    if (!vsh::isConstantVideoFormat(vi) || (!is420P8 && !is420P10)) {
        vsapi->mapSetError(out, "dlssnr.Enhance: clip must be YUV420P8 or YUV420P10 (constant format)");
        vsapi->freeNode(node);
        return;
    }

    auto *d = new FilterData();
    d->node = node;
    d->width = vi->width;
    d->height = vi->height;
    d->depth = vi->format.bitsPerSample;

    DlssnrParams initial{}; // member initializers are the default authority
    ApplyIntArg(in, vsapi, "preset", initial.preset, kPresetMin, kPresetMax);
    ApplyIntArg(in, vsapi, "style", initial.style, kStyleMin, kStyleMax);
    ApplyFloatArg(in, vsapi, "intensity", initial.intensity, kStrengthMin, kStrengthMax);
    ApplyFloatArg(in, vsapi, "local_tone", initial.localToneStrength, kStrengthMin, kStrengthMax);
    ApplyFloatArg(in, vsapi, "local_structure", initial.localStructureStrength, kStrengthMin, kStrengthMax);
    ApplyFloatArg(in, vsapi, "skin_structure", initial.skinStructureStrength, kSkinMin, kSkinMax);
    ApplyFlagArg(in, vsapi, "use_auto_mask", initial.useAutoMask);
    ApplyFlagArg(in, vsapi, "ui_correction", initial.uiCorrection);
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
    // 光流输入跟随内部降采样(scaling 启用时 NVOF 按内部尺寸计算)
    ApplyFlagArg(in, vsapi, "nvof_follow_scaling", initial.nvofFollowScaling);
    // DLSS 帧生成(0/1):激活时输出帧率 ×2(奇数索引 = 插值帧),失败
    // 优雅回退 1:1。挂 DLSSNR 之后 —— backbuffer = NR 输出。
    ApplyFlagArg(in, vsapi, "fg_enabled", initial.fgEnabled);
    // Panel-saved profile (dlssnr_ui.ini) overrides .vpy values when present;
    // the panel's CURRENT payload (last live state) overrides the ini. Without
    // the adopt step a seek rebuilds the filter from stale ini/vpy values —
    // the bridge poll skips the existing payload (history), so the panel's
    // parameters only came back after touching the panel again.
    const bool iniLoaded = vsdlssnr::BridgeLoadIni(initial);
    const bool payloadAdopted = vsdlssnr::BridgeAdoptPanelPayload(initial);
    // 探针:三层参数源(vpy 默认 → ini → 面板 payload)的最终裁决值。
    // "参数没生效/拖进度条回去了"类问题(#37)一行定位:ini/payload 哪层
    // 参与了、create-time 三元组最终是什么,一眼可查。
    {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: create params %dx%dd%d ini=%d payload=%d -> preset=%d res=%d%% scaling=%d of=%d follow=%d fg=%d",
                      d->width, d->height, d->depth, iniLoaded ? 1 : 0, payloadAdopted ? 1 : 0,
                      initial.preset, initial.inputResolutionPercent,
                      initial.scalingEnabled ? 1 : 0, initial.motionVectorQuality,
                      initial.nvofFollowScaling ? 1 : 0, initial.fgEnabled ? 1 : 0);
        vsdlssnr::TimingStatusLine(msg);
    }
    d->params = std::make_unique<vsdlssnr::SharedParams>(initial);

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
                d->ngxDllPath = (dir / SNIPPET_DLL_NAME).wstring();
            } catch (...) {
                d->ngxDllPath.clear();
            }
        }
    }

    // FG proxy DLL(dlssg_for_sm86 的 version.dll;用户自备部署,与模型
    // DLL 同目录约定)。默认 <plugin dir>/ngx/version.dll。
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
    // FG 状态(开关 + 代理路径)同样参与:槽资源形态(FG 纹理有无)随
    // 之变化,必须冷重建。
    const bool hotMatch = Hot().valid && Hot().ngxDllPath == d->ngxDllPath &&
                          Hot().width == d->width && Hot().height == d->height &&
                          Hot().depth == d->depth &&
                          Hot().fgEnabled == (initial.fgEnabled != 0) &&
                          Hot().fgDllPath == d->fgDllPath;
    if (hotMatch) {
        d->d3d12 = std::move(Hot().d3d12);
        d->ngx = std::move(Hot().ngx);
        Hot().valid = false;
        Hot().ngxDllPath.clear();
        if (d->ngx->Rebind(d->params.get(), d->width, d->height, d->depth, err, sizeof(err))) {
            d->initOk = true;
            // Filter is live: start the mpv-side parameter bridge
            if (!vsdlssnr::BridgeStart(d->params.get())) {
                vsapi->logMessage(mtWarning,
                                  "vs_dlssnr: parameter bridge failed to start; panel edits will not apply",
                                  core);
                vsdlssnr::TimingStatusLine("DLSSNR STATUS: bridge start FAILED; panel edits will not apply");
            }
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
    if (!d->initOk && !d->d3d12) {
        // Any parked context left over (different snippet DLL) must be torn
        // down BEFORE a cold init: the IAT hook and the NGX core are
        // process-global singletons. A second full Initialize could never
        // install the hook (its owner CAS is held by the parked context) and
        // its eventual Shutdown1 would tear down the shared core under the
        // parked feature — silently degrading every later resolution.
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
                               d->width, d->height, d->depth, d->params.get(), err, sizeof(err))) {
            d->initOk = true;
            // Filter is live: start the mpv-side parameter bridge
            if (!vsdlssnr::BridgeStart(d->params.get())) {
                vsapi->logMessage(mtWarning,
                                  "vs_dlssnr: parameter bridge failed to start; panel edits will not apply",
                                  core);
                vsdlssnr::TimingStatusLine("DLSSNR STATUS: bridge start FAILED; panel edits will not apply");
            }
            char msg[128];
            std::snprintf(msg, sizeof(msg), "vs_dlssnr ready (%dx%d)", d->width, d->height);
            vsapi->logMessage(mtInformation, msg, core);
        } else {
            char msg[512];
            std::snprintf(msg, sizeof(msg),
                          "vs_dlssnr init failed, falling back to passthrough: %s", err);
            vsapi->logMessage(mtWarning, msg, core);
            vsdlssnr::TimingStatusLine(msg); // GUI mpv 不透传 logMessage,失败必须进 timing log
            OutputDebugStringA("vs_dlssnr: init failed: ");
            OutputDebugStringA(err);
            OutputDebugStringA("\n");
            // 面板可见状态:D3D12/NGX 初始化失败 = 本实例整体直通。原因串
            // 可能含 D3D12 debug-layer 文本(引号),消毒后再进 stats。
            char safe[288];
            vsdlssnr::SanitizeJsonDetail(err, safe, sizeof(safe));
            char body[384];
            std::snprintf(body, sizeof(body), "{\"%s\":\"passthrough\",\"%s\":\"%.200s\"}",
                          vsdlssnr::SK_FILTER_STATE, vsdlssnr::SK_STATE_DETAIL, safe);
            vsdlssnr::PublishStatsJson(body);
        }
    }

    // FG 双输出判定(两条初始化路径汇合):会话激活 = 输出帧率 ×2;否则
    // 1:1(FG 失败的降级语义)。vi 副本翻倍 fps —— createVideoFilter 的
    // 输出格式与输入帧格式一致,仅帧率/帧数语义变化。
    d->fgDoubled = d->initOk && d->ngx && d->ngx->FgActive();
    VSVideoInfo viOut = *vi;
    if (d->fgDoubled) {
        viOut.fpsNum *= 2;
        char msg[160];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: fg active, output fps x2 (%I64d/%I64d -> %I64d/%I64d)",
                      vi->fpsNum, vi->fpsDen, viOut.fpsNum, viOut.fpsDen);
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
        "preset:int:opt;"
        "style:int:opt;"
        "intensity:float:opt;"
        "local_tone:float:opt;"
        "local_structure:float:opt;"
        "skin_structure:float:opt;"
        "use_auto_mask:int:opt;"
        "ui_correction:int:opt;"
        "residual_multiplier:float:opt;"
        "residual_saturation:float:opt;"
        "residual_lightness:float:opt;"
        "shadow_structure:float:opt;"
        "reflection_glow:float:opt;"
        "scaling_enabled:int:opt;"
        "input_resolution:int:opt;"
        "motion_vector_quality:int:opt;"
        "nvof_follow_scaling:int:opt;"
        "fg_enabled:int:opt;"
        "fg_dll:data:opt;",
        "clip:vnode;",
        DlssnrCreate, nullptr, plugin);
}
