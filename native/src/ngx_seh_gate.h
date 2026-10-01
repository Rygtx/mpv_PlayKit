#pragma once
// NGX feature 级 SEH 闩锁门(RTX VSR / TrueHDR / DLSS FG 三处共用;原三类
// 各持一份逐字拷贝)。与 NgxRuntimeGuard(进程级,ngx_runtime_guard.h)
// 互补:本闩锁随 feature 实例生命周期 —— SEH 后该 feature 拒绝再进入直至
// 宿主重启;进程级 SDK core 闩锁仍由 NgxRuntimeGuard 独立把守。SEH 绝不
// 上抛全局闩锁(降级边界:哪个 feature 崩,只停哪个)。
#include <windows.h>
#include <atomic>
#include <cstdio>

#include <nvsdk_ngx.h>

namespace vsdlssnr {

namespace detail {

// SEH 过滤器本体(__try 不能落在带 unwind 语义对象的函数内,独立成函数;
// 原三处各持一份同款)。
inline LONG SehCapture(EXCEPTION_POINTERS *exception, DWORD *sehCode) noexcept {
    *sehCode = exception->ExceptionRecord->ExceptionCode;
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace detail

// 裸执行体:true = fn() 返回 Success 且无 SEH;false = 失败(sehCode 非零
// = SEH,零 = 普通 NGX 失败)。
template <typename Fn>
bool NgxSehRun(Fn &&fn, DWORD *sehCode) noexcept {
    *sehCode = 0;
    __try {
        return fn() == NVSDK_NGX_Result_Success;
    } __except (detail::SehCapture(GetExceptionInformation(), sehCode)) {
        return false;
    }
}

// 闩锁门:faulted 即拒绝进入;SEH 置闩并经 log 上报 timing log(log 由调用
// 方注入 TimingStatusLine —— 避免本头依赖 dlssnr_context.h)。
//   tag         = 日志/err 前缀("rtx vsr" / "rtx hdr" / "dlssfg")
//   what        = 本次操作名(CreateFeature/evaluate/…)
//   disableNote = SEH 后追加入 timing log 的停用说明
template <typename Fn>
bool NgxSehGate(std::atomic<bool> &faulted, Fn &&fn, const char *tag,
                const char *what, const char *disableNote,
                char *err, size_t errLen,
                void (*log)(const char *) noexcept) noexcept {
    if (faulted.load(std::memory_order_acquire)) {
        if (err && errLen) {
            std::snprintf(err, errLen, "%s: faulted latch active (%s not called)",
                          tag, what);
        }
        return false;
    }
    DWORD sehCode = 0;
    const bool ok = NgxSehRun(fn, &sehCode);
    if (!ok && sehCode) {
        faulted.store(true, std::memory_order_release);
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: %s %s raised SEH 0x%lX; %s",
                      tag, what, static_cast<unsigned long>(sehCode), disableNote);
        log(msg);
        if (err && errLen) {
            std::snprintf(err, errLen, "%s: %s raised SEH 0x%lX",
                          tag, what, static_cast<unsigned long>(sehCode));
        }
        return false;
    }
    return ok;
}

} // namespace vsdlssnr
