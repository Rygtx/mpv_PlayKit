// RTX Video(NGX VSR + TrueHDR)上下文实现(契约见 rtx_video_context.h)。
// eval 键面 = 官方 nvsdk_ngx_helpers_vsr.h / nvsdk_ngx_helpers_truehdr.h
// (RTX Video SDK 1.1;Feature 16/14,reserved ID 已在既有 NGX 头内)。

#include "rtx_video_context.h"
#include "dlssnr_context.h" // TimingStatusLine(失败必须进 timing log)

#include <cstdio>
#include <functional>

#include <nvsdk_ngx_helpers_truehdr.h>
#include <nvsdk_ngx_helpers_vsr.h>

namespace vsdlssnr {

namespace {

// ---- RTX Video 本地 SEH(与 DlssfgContext 同构;绝不上抛全局闩锁)----
LONG RtxCaptureException(EXCEPTION_POINTERS *exception, DWORD *sehCode) noexcept {
    *sehCode = exception->ExceptionRecord->ExceptionCode;
    return EXCEPTION_EXECUTE_HANDLER;
}

template <typename Fn>
bool RtxSehCall(Fn &&fn, DWORD *sehCode) noexcept {
    *sehCode = 0;
    __try {
        return fn() == NVSDK_NGX_Result_Success;
    } __except (RtxCaptureException(GetExceptionInformation(), sehCode)) {
        return false;
    }
}

} // namespace

// ---- 专用队列执行体(SDK CDx12NGXVSR 同款)----
bool RtxQueue::Initialize(ID3D12Device *device, const char *label, char *err, size_t errLen) noexcept {
    auto fail = [&](const char *what) {
        if (err && errLen) std::snprintf(err, errLen, "rtx queue(%s): %s", label, what);
        return false;
    };
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(_queue.GetAddressOf())))) {
        return fail("CreateCommandQueue failed");
    }
    for (int i = 0; i < kAltCount; ++i) {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(_allocator[i].GetAddressOf())))) {
            return fail("CreateCommandAllocator failed");
        }
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             _allocator[i].Get(), nullptr,
                                             IID_PPV_ARGS(_commandList[i].GetAddressOf())))) {
            return fail("CreateCommandList failed");
        }
        if (FAILED(_commandList[i]->Close())) return fail("initial Close failed");
    }
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(_fence.GetAddressOf())))) {
        return fail("CreateFence failed");
    }
    _event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!_event) return fail("CreateEvent failed");
    return true;
}

bool RtxQueue::Execute(ID3D12Fence *waitFence, uint64_t waitValue,
                       const char *what, char *err, size_t errLen,
                       const std::function<bool(ID3D12GraphicsCommandList *)> &fn,
                       uint64_t *signalValueOut) noexcept {
    auto fail = [&](const char *w) {
        if (err && errLen) std::snprintf(err, errLen, "rtx queue %s: %s", what, w);
        return false;
    };
    if (waitFence && waitValue) _queue->Wait(waitFence, waitValue);
    // 六组轮转:本笔 idx = fenceValue % 6,Reset 目标(同 idx 的 allocator)
    // 最近一笔在 6 笔之前 —— 逐帧链(≤6 笔 = mult=6 默认模式的 TrueHDR 上限)
    // 通常早已完成,等待即刻返回;GPU 落后超 6 笔同 idx(>12 笔在飞)时
    // INFINITE 等待 = 背压兜底(单 allocator 时代的 lookback 是"等上一笔"=
    // 背靠背 eval 时 CPU 逐笔等 GPU,提交链被 GPU 执行串行吸收 —— HDR 链
    // sub=17ms 根因,2026-09-24 探针定案;双组在 mult=4 HDR 链仍被同帧
    // 第 2/3 笔顶住,eval_cpu 16-18ms,同日日志定案;in-flight Reset 是 UB
    // 且有 VSR 队列静默 wedge 前科,2026-09-22,背压语义保留)。
    const int idx = static_cast<int>(_fenceValue.load(std::memory_order_acquire) % kAltCount);
    if (const uint64_t pending = _lastSignal[idx]) {
        Wait(pending, nullptr, 0);
    }
    if (FAILED(_allocator[idx]->Reset())) return fail("allocator Reset failed");
    if (FAILED(_commandList[idx]->Reset(_allocator[idx].Get(), nullptr))) return fail("CL Reset failed");
    if (!fn(_commandList[idx].Get())) {
        // 录制失败:force-close 释放 CL 到封闭态(下帧 Reset 才能成功),
        // 队列不执行(已排的 Wait 无害,自然满足)。
        _commandList[idx]->Close();
        return fail("NGX eval recording failed");
    }
    if (FAILED(_commandList[idx]->Close())) return fail("CL Close failed");
    ID3D12CommandList *lists[]{ _commandList[idx].Get() };
    _queue->ExecuteCommandLists(1, lists);
    const uint64_t v = _fenceValue.fetch_add(1) + 1;
    _queue->Signal(_fence.Get(), v);
    _lastSignal[idx] = v;
    if (signalValueOut) *signalValueOut = v;
    return true;
}

bool RtxQueue::Wait(uint64_t value, char *err, size_t errLen, DWORD timeoutMs) noexcept {
    // 共享 auto-reset 事件:多帧线程可并发等待不同栅栏值(fmParallel 锁外
    // 等待窗口),fence 值跳变时唤醒可被合并 —— 单次 Wait 返回不代表本等待
    // 的目标值已达成(被其它等待者窃取),超时误判 = 帧失败。循环复查完成
    // 值:栅栏值单调 ⇒ 有界退出(NvofContext::WaitFenceReached 同款)。
    while (_fence->GetCompletedValue() < value) {
        if (FAILED(_fence->SetEventOnCompletion(value, _event))) {
            if (err && errLen) std::snprintf(err, errLen, "rtx queue: SetEventOnCompletion failed");
            return false;
        }
        if (WaitForSingleObject(_event, timeoutMs) != WAIT_OBJECT_0) {
            // 超时(且复查仍未达标)= 专用队列 wedge/设备丢失:不闩错(帧的
            // 最终判定归 WaitFrame 的栅栏超时路径),仅向调用方报告未完成。
            if (_fence->GetCompletedValue() >= value) break;
            if (err && errLen) std::snprintf(err, errLen, "rtx queue: fence wait timed out (%lu ms)", timeoutMs);
            return false;
        }
    }
    return true;
}

RtxVsrContext::~RtxVsrContext() {
    // 与 DlssfgContext 同哲学:析构不重入 NGX(热上下文/进程退出统一回收);
    // 显式 Release 只发生在未来的 feature 重建路径(当前尺寸无关,不需要)。
}

template <typename Fn>
bool RtxVsrContext::SehCall(Fn &&fn, const char *what, char *err, size_t errLen) noexcept {
    if (_faulted.load(std::memory_order_acquire)) {
        if (err && errLen) {
            std::snprintf(err, errLen, "rtx vsr: faulted latch active (%s not called)", what);
        }
        return false;
    }
    DWORD sehCode = 0;
    const bool ok = RtxSehCall(fn, &sehCode);
    if (!ok && sehCode) {
        _faulted.store(true, std::memory_order_release);
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: rtx vsr %s raised SEH 0x%lX; VSR disabled until host restart",
                      what, static_cast<unsigned long>(sehCode));
        TimingStatusLine(msg);
        if (err && errLen) {
            std::snprintf(err, errLen, "rtx vsr: %s raised SEH 0x%lX",
                          what, static_cast<unsigned long>(sehCode));
        }
        return false;
    }
    return ok;
}

bool RtxVsrContext::Initialize(D3D12Context &d3d12, NVSDK_NGX_Parameter *params,
                               int width, int height, char *err, size_t errLen) noexcept {
    auto fail = [&](const char *what) {
        if (err && errLen) std::snprintf(err, errLen, "%s", what);
        char msg[224];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: rtx vsr init failed: %.180s",
                      what ? what : "?");
        TimingStatusLine(msg);
        _d3d12 = nullptr;
        _params = nullptr;
        return false;
    };
    if (!params) return fail("no core parameter block");
    _d3d12 = &d3d12;
    _params = params;

    // 能力预检(VSR.Available):snippet 缺失 / 驱动过旧(r550.58-)/ 非 RTX
    // 卡时为 0。FeatureInitResult 读回精确归因。
    int available = 0;
    {
        char sehErr[160]{};
        const bool haveCap = SehCall([&] {
            return _params->Get(NVSDK_NGX_Parameter_VSR_Available, &available) ==
                   NVSDK_NGX_Result_Success;
        }, "Get(VSR.Available)", sehErr, sizeof(sehErr));
        if (!haveCap || !available) {
            unsigned int initResult = 0;
            const bool haveResult =
                _params->Get(NVSDK_NGX_Parameter_VSR_FeatureInitResult, &initResult) ==
                NVSDK_NGX_Result_Success;
            char tag[48]{};
            if (haveResult) {
                std::snprintf(tag, sizeof(tag), " (FeatureInitResult 0x%X)", initResult);
            }
            char msg[160];
            std::snprintf(msg, sizeof(msg), "official NGX reports VSR unavailable%s%s",
                          tag, haveCap ? "" : " (capability key missing)");
            return fail(msg);
        }
    }
    if (!CreateFeatureOnCtl(err, errLen)) {
        _d3d12 = nullptr;
        _params = nullptr;
        return false;
    }
    char qErr[128]{};
    if (!_queue.Initialize(_d3d12->Device(), "vsr", qErr, sizeof(qErr))) {
        if (err && errLen) std::snprintf(err, errLen, "%.140s", qErr);
        TimingStatusLine(qErr);
        _d3d12 = nullptr;
        _params = nullptr;
        return false;
    }
    _ready.store(true, std::memory_order_release);
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: rtx vsr ready (%dx%d)", width, height);
        TimingStatusLine(msg);
    }
    return true;
}

bool RtxVsrContext::CreateFeatureOnCtl(char *err, size_t errLen) noexcept {
    // ctl 路径(与 DLSSFG CreateFeatureOnCtl 同款):CtlMutex + ctl 命令列表
    // 上 CreateFeature,Execute + 栅栏等待落位。VSR create 与尺寸无关。
    std::lock_guard<std::mutex> ctlLock(_d3d12->CtlMutex());
    if (!_d3d12->BeginCtlRecording()) {
        if (err && errLen) std::snprintf(err, errLen, "rtx vsr: BeginCtlRecording failed");
        return false;
    }
    _params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    _params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    NVSDK_NGX_Feature_Create_Params createParams{};
    char sehErr[160]{};
    bool ok = SehCall([&] {
        return NGX_D3D12_CREATE_VSR_EXT(_d3d12->CtlCommandList(), 1u, 1u,
                                        &_feature, _params, &createParams) ==
               NVSDK_NGX_Result_Success;
    }, "CreateFeature(VSR)", sehErr, sizeof(sehErr));
    if (ok && !_feature) ok = false;
    if (!_d3d12->ExecuteCtlAndWait(err, errLen, "vsr create")) return false;
    if (!ok) {
        unsigned int initResult = 0;
        const bool haveResult =
            _params->Get(NVSDK_NGX_Parameter_VSR_FeatureInitResult, &initResult) ==
            NVSDK_NGX_Result_Success;
        char resultTag[40]{};
        if (haveResult) {
            std::snprintf(resultTag, sizeof(resultTag), " (FeatureInitResult 0x%X)", initResult);
        }
        if (err && errLen) {
            std::snprintf(err, errLen, "rtx vsr: CreateFeature failed%s%s%s",
                          sehErr[0] ? ": " : "", sehErr[0] ? sehErr : "", resultTag);
        }
        TimingStatusLine("DLSSNR STATUS: rtx vsr CreateFeature failed; VSR off (SDR passthrough size)");
        return false;
    }
    return true;
}

bool RtxVsrContext::Evaluate(ID3D12Resource *input, int inW, int inH,
                             ID3D12Resource *output, int outW, int outH,
                             int quality,
                             ID3D12Fence *waitFence, uint64_t waitValue,
                             uint64_t *signalValueOut,
                             char *err, size_t errLen) noexcept {
    if (!_ready.load(std::memory_order_acquire) || _faulted.load(std::memory_order_acquire)) {
        if (err && errLen) std::snprintf(err, errLen, "rtx vsr: session dead");
        return false;
    }
    char sehErr[160]{};
    const bool ok = SehCall([&] {
        return _queue.Execute(waitFence, waitValue, "vsr eval", err, errLen,
                              [&](ID3D12GraphicsCommandList *cl) {
            NVSDK_NGX_D3D12_VSR_Eval_Params eval{};
            eval.pInput = input;
            eval.pOutput = output;
            eval.InputSubrectBase.X = 0;
            eval.InputSubrectBase.Y = 0;
            eval.InputSubrectSize.Width = static_cast<unsigned int>(inW);
            eval.InputSubrectSize.Height = static_cast<unsigned int>(inH);
            eval.OutputSubrectBase.X = 0;
            eval.OutputSubrectBase.Y = 0;
            eval.OutputSubrectSize.Width = static_cast<unsigned int>(outW);
            eval.OutputSubrectSize.Height = static_cast<unsigned int>(outH);
            eval.QualityLevel = static_cast<NVSDK_NGX_VSR_QualityLevel>(
                std::clamp(quality, kVsrStrengthMin, kVsrStrengthMax));
            return NGX_D3D12_EVALUATE_VSR_EXT(cl, _feature, _params, &eval) ==
                   NVSDK_NGX_Result_Success;
        }, signalValueOut);
    }, "EvaluateFeature(VSR)", sehErr, sizeof(sehErr));
    if (!ok) {
        _ready.store(false, std::memory_order_release);
        if (err && errLen && !err[0]) {
            std::snprintf(err, errLen, "rtx vsr: EvaluateFeature failed%s%s",
                          sehErr[0] ? ": " : "", sehErr[0] ? sehErr : "");
        }
        TimingStatusLine("DLSSNR STATUS: rtx vsr evaluate failed; VSR off (passthrough size)");
    }
    return ok;
}

RtxHdrContext::~RtxHdrContext() {
    // 同 RtxVsrContext:析构不重入 NGX。
}

template <typename Fn>
bool RtxHdrContext::SehCall(Fn &&fn, const char *what, char *err, size_t errLen) noexcept {
    if (_faulted.load(std::memory_order_acquire)) {
        if (err && errLen) {
            std::snprintf(err, errLen, "rtx hdr: faulted latch active (%s not called)", what);
        }
        return false;
    }
    DWORD sehCode = 0;
    const bool ok = RtxSehCall(fn, &sehCode);
    if (!ok && sehCode) {
        _faulted.store(true, std::memory_order_release);
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: rtx hdr %s raised SEH 0x%lX; HDR disabled until host restart",
                      what, static_cast<unsigned long>(sehCode));
        TimingStatusLine(msg);
        if (err && errLen) {
            std::snprintf(err, errLen, "rtx hdr: %s raised SEH 0x%lX",
                          what, static_cast<unsigned long>(sehCode));
        }
        return false;
    }
    return ok;
}

bool RtxHdrContext::Initialize(D3D12Context &d3d12, NVSDK_NGX_Parameter *params,
                               int width, int height, char *err, size_t errLen) noexcept {
    auto fail = [&](const char *what) {
        if (err && errLen) std::snprintf(err, errLen, "%s", what);
        char msg[224];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: rtx hdr init failed: %.180s",
                      what ? what : "?");
        TimingStatusLine(msg);
        _d3d12 = nullptr;
        _params = nullptr;
        return false;
    };
    if (!params) return fail("no core parameter block");
    _d3d12 = &d3d12;
    _params = params;
    int available = 0;
    {
        char sehErr[160]{};
        const bool haveCap = SehCall([&] {
            return _params->Get(NVSDK_NGX_Parameter_TrueHDR_Available, &available) ==
                   NVSDK_NGX_Result_Success;
        }, "Get(TrueHDR.Available)", sehErr, sizeof(sehErr));
        if (!haveCap || !available) {
            unsigned int initResult = 0;
            const bool haveResult =
                _params->Get(NVSDK_NGX_Parameter_TrueHDR_FeatureInitResult, &initResult) ==
                NVSDK_NGX_Result_Success;
            char tag[48]{};
            if (haveResult) {
                std::snprintf(tag, sizeof(tag), " (FeatureInitResult 0x%X)", initResult);
            }
            char msg[160];
            std::snprintf(msg, sizeof(msg), "official NGX reports TrueHDR unavailable%s%s",
                          tag, haveCap ? "" : " (capability key missing)");
            return fail(msg);
        }
    }
    if (!CreateFeatureOnCtl(err, errLen)) {
        _d3d12 = nullptr;
        _params = nullptr;
        return false;
    }
    char qErr[128]{};
    if (!_queue.Initialize(_d3d12->Device(), "hdr", qErr, sizeof(qErr))) {
        if (err && errLen) std::snprintf(err, errLen, "%.140s", qErr);
        TimingStatusLine(qErr);
        _d3d12 = nullptr;
        _params = nullptr;
        return false;
    }
    _ready.store(true, std::memory_order_release);
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: rtx hdr ready (%dx%d)", width, height);
        TimingStatusLine(msg);
    }
    return true;
}

bool RtxHdrContext::CreateFeatureOnCtl(char *err, size_t errLen) noexcept {
    std::lock_guard<std::mutex> ctlLock(_d3d12->CtlMutex());
    if (!_d3d12->BeginCtlRecording()) {
        if (err && errLen) std::snprintf(err, errLen, "rtx hdr: BeginCtlRecording failed");
        return false;
    }
    _params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    _params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    NVSDK_NGX_Feature_Create_Params createParams{};
    char sehErr[160]{};
    bool ok = SehCall([&] {
        return NGX_D3D12_CREATE_TRUEHDR_EXT(_d3d12->CtlCommandList(), 1u, 1u,
                                            &_feature, _params, &createParams) ==
               NVSDK_NGX_Result_Success;
    }, "CreateFeature(TrueHDR)", sehErr, sizeof(sehErr));
    if (ok && !_feature) ok = false;
    if (!_d3d12->ExecuteCtlAndWait(err, errLen, "hdr create")) return false;
    if (!ok) {
        unsigned int initResult = 0;
        const bool haveResult =
            _params->Get(NVSDK_NGX_Parameter_TrueHDR_FeatureInitResult, &initResult) ==
            NVSDK_NGX_Result_Success;
        char resultTag[40]{};
        if (haveResult) {
            std::snprintf(resultTag, sizeof(resultTag), " (FeatureInitResult 0x%X)", initResult);
        }
        if (err && errLen) {
            std::snprintf(err, errLen, "rtx hdr: CreateFeature failed%s%s%s",
                          sehErr[0] ? ": " : "", sehErr[0] ? sehErr : "", resultTag);
        }
        TimingStatusLine("DLSSNR STATUS: rtx hdr CreateFeature failed; HDR off (SDR output)");
        return false;
    }
    return true;
}

bool RtxHdrContext::Evaluate(ID3D12Resource *input, int w, int h, ID3D12Resource *output,
                             int contrast, int saturation, int middleGray, int maxLuminance,
                             ID3D12Fence *waitFence, uint64_t waitValue,
                             uint64_t *signalValueOut,
                             char *err, size_t errLen) noexcept {
    if (!_ready.load(std::memory_order_acquire) || _faulted.load(std::memory_order_acquire)) {
        if (err && errLen) std::snprintf(err, errLen, "rtx hdr: session dead");
        return false;
    }
    char sehErr[160]{};
    const bool ok = SehCall([&] {
        return _queue.Execute(waitFence, waitValue, "hdr eval", err, errLen,
                              [&](ID3D12GraphicsCommandList *cl) {
            NVSDK_NGX_D3D12_TRUEHDR_Eval_Params eval{};
            eval.pInput = input;
            eval.pOutput = output;
            eval.InputSubrectTL.X = 0;
            eval.InputSubrectTL.Y = 0;
            eval.InputSubrectBR.Width = static_cast<unsigned int>(w);
            eval.InputSubrectBR.Height = static_cast<unsigned int>(h);
            eval.OutputSubrectTL.X = 0;
            eval.OutputSubrectTL.Y = 0;
            eval.OutputSubrectBR.Width = static_cast<unsigned int>(w);
            eval.OutputSubrectBR.Height = static_cast<unsigned int>(h);
            eval.Contrast = static_cast<unsigned int>(std::clamp(contrast, kHdrContrastMin, kHdrContrastMax));
            eval.Saturation = static_cast<unsigned int>(std::clamp(saturation, kHdrSaturationMin, kHdrSaturationMax));
            eval.MiddleGray = static_cast<unsigned int>(std::clamp(middleGray, kHdrMiddleGrayMin, kHdrMiddleGrayMax));
            eval.MaxLuminance = static_cast<unsigned int>(std::clamp(maxLuminance, kHdrMaxLumMin, kHdrMaxLumMax));
            return NGX_D3D12_EVALUATE_TRUEHDR_EXT(cl, _feature, _params, &eval) ==
                   NVSDK_NGX_Result_Success;
        }, signalValueOut);
    }, "EvaluateFeature(TrueHDR)", sehErr, sizeof(sehErr));
    if (!ok) {
        _ready.store(false, std::memory_order_release);
        if (err && errLen) {
            std::snprintf(err, errLen, "rtx hdr: EvaluateFeature failed%s%s",
                          sehErr[0] ? ": " : "", sehErr[0] ? sehErr : "");
        }
        TimingStatusLine("DLSSNR STATUS: rtx hdr evaluate failed; HDR off");
    }
    return ok;
}

} // namespace vsdlssnr
