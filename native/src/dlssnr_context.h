#pragma once
// Ported from Magpie experimental DLSSNRFilter.cpp / NgxD3D12Core.cpp.
// NGX static core (nvsdk_ngx_s.lib) owns the parameter block; the signed
// snippet nvngx_dlssnr.dll (Feature 18) performs CreateFeature/EvaluateFeature.
// Zero guidance (Magpie guidanceMode=1 Force Zero): all-zero R16G16 motion +
// R32_FLOAT depth + full-frame subrects.

#include "d3d12_context.h"
#include "dlssnr_params.h"
#include "iat_hook.h"
#include "shared_params.h"
#include <atomic>
#include <mutex>
#include <nvsdk_ngx.h>

namespace vsdlssnr {

// Panel toggle for the periodic perf log (dlssnr_timing.log)
void SetTimingLogEnabled(bool enabled) noexcept;

// A backwards jump (seek back) or a gap larger than the host's prefetch
// window is a real discontinuity and resets NGX's temporal history; anything
// else (mpv's startup prefetch activates frames out of order under
// fmParallel) is ordering noise. The threshold encodes mpv's fetch pipeline
// depth — raise it only if a host legitimately requests frames further apart.
constexpr int kFrameGapResetThreshold = 32;

class DlssnrContext {
public:
    DlssnrContext() = default;
    ~DlssnrContext();
    DlssnrContext(const DlssnrContext &) = delete;
    DlssnrContext &operator=(const DlssnrContext &) = delete;

    bool Initialize(D3D12Context &d3d12, const wchar_t *ngxDllPath,
                    int width, int height, SharedParams *shared,
                    char *err, size_t errLen) noexcept;
    void Shutdown() noexcept;

    // Hot-context rebind: attach this kept-warm context (device, NGX feature,
    // slot pool all alive) to a new filter instance's SharedParams — including
    // one for a different video size (frame resources + feature rebuild under
    // the pool seal; only a changed snippet DLL forces a full re-init at the
    // caller). The feature is rebuilt only when a create-time parameter
    // (preset / input_resolution / scaling_enabled) or the size actually
    // differs; otherwise it is free. Returns false (and leaves _ready false)
    // when the rebuild fails; the caller then falls back to a full Initialize.
    bool Rebind(SharedParams *shared, int width, int height, char *err, size_t errLen) noexcept;

    // Preset / internal-resolution / scaling-toggle are create-time NGX keys:
    // on panel change the frame thread rebuilds the feature (and scaling
    // textures for resolution changes; disabled = residual pipeline dropped).
    // newWidth/newHeight >= 0 additionally rebuilds the per-slot frame
    // resources for that size (used by Rebind across resolutions).
    bool RecreateFeature(int preset, int resPercent, int scalingEnabled, char *err, size_t errLen,
                         int newWidth = -1, int newHeight = -1) noexcept;

    // RGBS float32 三平面进 → 处理 → RGBS float32 三平面出(同分辨率)
    // n is the frame index; discontinuity detection (NGX history reset) is
    // owned here, not by the glue layer. timingOut 非 NULL 时写入分段耗时
    // (毫秒,逗号分隔:pack,submit+gpu,unpack)
    bool ProcessFrame(const uint8_t *const *srcPlanes, const int64_t *srcStrides,
                      uint8_t **dstPlanes, int64_t *dstStrides,
                      int width, int height, int n,
                      char *err, size_t errLen,
                      char *timingOut = nullptr, size_t timingLen = 0) noexcept;

private:
    // ---- SEH wrappers (Magpie style; every NGX call is wrapped) ----
    NVSDK_NGX_Result CoreInitSafely(const wchar_t *appDir, ID3D12Device *device,
                                    const NVSDK_NGX_FeatureCommonInfo *info, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result CoreAllocateParametersSafely(NVSDK_NGX_Parameter **out, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result CoreDestroyParametersSafely(NVSDK_NGX_Parameter *p, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result CoreShutdownSafely(ID3D12Device *device, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result SnippetInitSafely(const wchar_t *appDataPath, ID3D12Device *device, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result SnippetCreateFeatureSafely(ID3D12GraphicsCommandList *cl, NVSDK_NGX_Parameter *params, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result SnippetEvaluateSafely(ID3D12GraphicsCommandList *cl, NVSDK_NGX_Parameter *params, DWORD *sehCode) noexcept;
    NVSDK_NGX_Result SnippetReleaseSafely(DWORD *sehCode) noexcept;
    NVSDK_NGX_Result SnippetShutdownSafely(DWORD *sehCode) noexcept;
    void SetCreateParametersUnsafe() noexcept;
    bool SetCreateParametersSafely(DWORD *sehCode) noexcept;
    void SetEvaluateParametersUnsafe(FrameSlot &slot, bool resetHistory) noexcept;
    bool SetEvaluateParametersSafely(FrameSlot &slot, bool resetHistory, DWORD *sehCode) noexcept;

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
    // Adapter description in UTF-8, filled once in Initialize and reused by
    // the periodic stats publish (GetDesc per stats tick is wasted work).
    char _gpuNameUtf8[160] = "UNAVAILABLE";
    bool _coreInitialized = false;
    bool _snippetInitialized = false;
    // Cross-thread: written by one frame thread (device-lost latch /
    // failed rebuild) while others read it under fmParallel.
    std::atomic<bool> _ready{false};
    // last requested frame index (see kFrameGapResetThreshold) — survives
    // seeks with the hot context; a seek's n restart triggers the reset.
    std::atomic<int> _lastFrame{-1 << 30};
    // create-time parameters currently baked into the NGX feature (Rebind
    // compares against these to skip a no-op RecreateFeature)
    int _curPreset = -1;
    int _curRes = -1;
    bool _curScaling = false;
    // fmParallel: several frame threads call EvaluateFeature concurrently.
    // The feature and the parameter block are singletons, so evaluate
    // (parameter setup + snippet call) is serialized; GPU-side dispatches
    // still overlap via each slot's own command list.
    std::mutex _evaluateMutex;
};

} // namespace vsdlssnr
