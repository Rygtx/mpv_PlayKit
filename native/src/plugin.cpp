// vs_dlssnr - NVIDIA DLSSNR (NGX Feature 18) filter for VapourSynth.
// Ported from Magpie experimental (github.com/SAOG0721/Magpie).
// Model file: nvngx_dlssnr.dll (310.9.0), zero-guidance mode (Magpie
// guidanceMode=1 Force Zero), same-resolution processing.

#include "bridge.h"
#include "d3d12_context.h"
#include "dlssnr_context.h"
#include "dlssnr_params.h"
#include "shared_params.h"

#include "VapourSynth4.h"
#include "VSHelper4.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <windows.h>

namespace {

constexpr char PLUGIN_IDENTIFIER[] = "dev.rygtx.vsdlssnr";
constexpr char PLUGIN_NAMESPACE[] = "dlssnr";
constexpr char PLUGIN_NAME[] = "NVIDIA DLSSNR filter (Magpie port)";
constexpr char SNIPPET_DLL_NAME[] = "nvngx_dlssnr.dll";

long long GetIntDef(const VSMap *in, const VSAPI *vsapi, const char *key, long long def) {
    int err = 0;
    const long long v = vsapi->mapGetInt(in, key, 0, &err);
    return err ? def : v;
}

double GetFloatDef(const VSMap *in, const VSAPI *vsapi, const char *key, double def) {
    int err = 0;
    const double v = vsapi->mapGetFloat(in, key, 0, &err);
    return err ? def : v;
}

struct FilterData {
    VSNode *node = nullptr;
    // Runtime-mutable parameters shared with the tray panel (initial values
    // come from the .vpy call).
    std::unique_ptr<vsdlssnr::SharedParams> params;

    // Eagerly initialized in DlssnrCreate (before playback starts); VS may
    // call getFrame on several threads under fmParallel, but each frame runs
    // on its own D3D12 slot, so no further locking is needed here.
    vsdlssnr::D3D12Context d3d12;
    vsdlssnr::DlssnrContext ngx;
    std::wstring ngxDllPath;
    bool initOk = false;
    int width = 0;
    int height = 0;
    std::atomic<int> lastN{ -1 << 30 };
};

} // namespace

static const VSFrame *VS_CC DlssnrGetFrame(
    int n, int activationReason, void *instanceData, void ** /*frameData*/,
    VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    auto *d = static_cast<FilterData *>(instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
        return nullptr;
    }
    if (activationReason != arAllFramesReady) return nullptr;

    const VSFrame *src = vsapi->getFrameFilter(n, d->node, frameCtx);

    if (!d->initOk) return src; // passthrough on any setup failure; the frame
                                // reference is handed off to the caller

    // fmParallel lets VS activate frames out of order and on several threads
    // (mpv's startup prefetch especially does), so a strict n != lastN + 1
    // would fire NGX's reset path on every reordering and stall the first
    // seconds of playback. Only treat real discontinuities as a reset:
    // backwards jump (seek back) or a large forward gap (seek past the
    // prefetch window). Relaxed ordering: a stale read at worst triggers one
    // extra harmless reset on a zero-guidance (stateless) model.
    const int lastN = d->lastN.load(std::memory_order_relaxed);
    const bool resetHistory = lastN < 0 || n < lastN || n - lastN > 32;
    d->lastN.store(n, std::memory_order_relaxed);

    const VSVideoFormat *fi = vsapi->getVideoFrameFormat(src);
    VSFrame *out = vsapi->newVideoFrame(fi, d->width, d->height, src, core);

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

    char err[256]{};
    char timing[128]{};
    static const bool timingEnabled = GetEnvironmentVariableA("VSDLSSNR_TIMING", nullptr, 0) != 0;
    if (!d->ngx.ProcessFrame(srcPlanes, srcStrides, dstPlanes, dstStrides,
                             d->width, d->height, resetHistory, err, sizeof(err),
                             timingEnabled ? timing : nullptr, timingEnabled ? sizeof(timing) : 0)) {
        char msg[512];
        std::snprintf(msg, sizeof(msg), "vs_dlssnr frame %d failed: %s", n, err);
        vsapi->logMessage(mtWarning, msg, core);
        // Failed frames fall back to a plain copy of the source content so the
        // output planes are never left uninitialized.
        vsh::bitblt(dstPlanes[0], dstStrides[0], srcPlanes[0], srcStrides[0],
                    static_cast<size_t>(d->width) * 4, d->height);
        vsh::bitblt(dstPlanes[1], dstStrides[1], srcPlanes[1], srcStrides[1],
                    static_cast<size_t>(d->width) * 4, d->height);
        vsh::bitblt(dstPlanes[2], dstStrides[2], srcPlanes[2], srcStrides[2],
                    static_cast<size_t>(d->width) * 4, d->height);
    }

    if (timingEnabled && timing[0]) {
        // Throttle: log every 30th frame to avoid flooding the log channel
        static int timingFrameCount = 0;
        if (++timingFrameCount % 30 == 1) {
            char msg[192];
            std::snprintf(msg, sizeof(msg), "vs_dlssnr timing[%d]: %s", n, timing);
            vsapi->logMessage(mtInformation, msg, core);
        }
    }

    // getFrameFilter handed us a reference to src; release it or every source
    // frame leaks (~24MB per 1080p frame).
    vsapi->freeFrame(src);
    return out;
}

static void VS_CC DlssnrFree(void *instanceData, VSCore * /*core*/, const VSAPI *vsapi) {
    auto *d = static_cast<FilterData *>(instanceData);
    if (d->node) vsapi->freeNode(d->node);
    // Stop the bridge before tearing down the contexts it observes.
    vsdlssnr::BridgeStop(d->params.get());
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

    if (!vsh::isConstantVideoFormat(vi) ||
        vi->format.colorFamily != cfRGB || vi->format.sampleType != stFloat ||
        vi->format.bitsPerSample != 32 || vi->format.numPlanes != 3) {
        vsapi->mapSetError(out, "dlssnr.Enhance: clip must be RGBS (RGB float32, constant format)");
        vsapi->freeNode(node);
        return;
    }

    auto *d = new FilterData();
    d->node = node;
    d->width = vi->width;
    d->height = vi->height;

    DlssnrParams initial{};
    initial.preset = static_cast<int>(std::clamp<long long>(GetIntDef(in, vsapi, "preset", 0), kPresetMin, kPresetMax));
    initial.style = static_cast<int>(std::clamp<long long>(GetIntDef(in, vsapi, "style", 0), kStyleMin, kStyleMax));
    initial.intensity = vsh::doubleToFloatS(std::clamp<double>(GetFloatDef(in, vsapi, "intensity", 1.0), kStrengthMin, kStrengthMax));
    initial.localToneStrength = vsh::doubleToFloatS(std::clamp<double>(GetFloatDef(in, vsapi, "local_tone", 1.0), kStrengthMin, kStrengthMax));
    initial.localStructureStrength = vsh::doubleToFloatS(std::clamp<double>(GetFloatDef(in, vsapi, "local_structure", 1.0), kStrengthMin, kStrengthMax));
    initial.skinStructureStrength = vsh::doubleToFloatS(std::clamp<double>(GetFloatDef(in, vsapi, "skin_structure", -1.0), kSkinMin, kSkinMax));
    initial.useAutoMask = GetIntDef(in, vsapi, "use_auto_mask", 1) != 0;
    initial.uiCorrection = GetIntDef(in, vsapi, "ui_correction", 1) != 0;
    initial.residualMultiplier = vsh::doubleToFloatS(std::clamp<double>(GetFloatDef(in, vsapi, "residual_multiplier", 1.0), kResidualMultMin, kResidualMultMax));
    initial.inputResolutionPercent = static_cast<int>(std::clamp<long long>(GetIntDef(in, vsapi, "input_resolution", 100), kResPctMin, kResPctMax));
    // scaling_enabled=0 drops the residual pipeline entirely (input_resolution ignored)
    initial.scalingEnabled = GetIntDef(in, vsapi, "scaling_enabled", 1) != 0;
    // Panel-saved profile (dlssnr_ui.ini) overrides .vpy values when present.
    vsdlssnr::BridgeLoadIni(initial);
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

    // Eager init: the D3D12/NGX bring-up costs ~1s (165MB snippet DLL load +
    // CreateFeature + first-evaluate warm-up). Doing it here — script
    // execution, before mpv starts the playback clock — keeps that whole
    // warm-up period out of playback; lazily initializing on the first
    // getFrame made the first seconds of playback stutter. Failure keeps the
    // passthrough fallback semantics below.
    char err[256]{};
    if (d->d3d12.Initialize(err, sizeof(err)) &&
        d->ngx.Initialize(d->d3d12, d->ngxDllPath.c_str(),
                          d->width, d->height, d->params.get(), err, sizeof(err))) {
        d->initOk = true;
        // Filter is live: start the mpv-side parameter bridge
        vsdlssnr::BridgeStart(d->params.get());
        char msg[128];
        std::snprintf(msg, sizeof(msg), "vs_dlssnr ready (%dx%d)", d->width, d->height);
        vsapi->logMessage(mtInformation, msg, core);
    } else {
        char msg[512];
        std::snprintf(msg, sizeof(msg),
                      "vs_dlssnr init failed, falling back to passthrough: %s", err);
        vsapi->logMessage(mtWarning, msg, core);
        OutputDebugStringA("vs_dlssnr: init failed: ");
        OutputDebugStringA(err);
        OutputDebugStringA("\n");
    }

    VSFilterDependency deps[]{ { node, rpStrictSpatial } };
    // fmParallel: mpv keeps multiple frame requests in flight; each getFrame
    // runs on its own D3D12 slot (slot pool caps the concurrency at
    // kSlotCount), which overlaps CPU pack/unpack with the GPU work of other
    // slots — the old fmUnordered path idled the GPU between frames.
    vsapi->createVideoFilter(out, "Enhance", vi, DlssnrGetFrame, DlssnrFree,
                             fmParallel, deps, 1, d, core);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
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
        "scaling_enabled:int:opt;"
        "input_resolution:int:opt;",
        "clip:vnode;",
        DlssnrCreate, nullptr, plugin);
}
