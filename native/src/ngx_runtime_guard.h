#pragma once
// Process-level NGX fault latch (ported from Magpie include/NgxRuntimeGuard.h,
// v0.6.6 commit 9824d758). An SEH inside the NGX SDK can bypass the SDK's own
// unlocks — the scaling thread keeps holding the SDK critical section while a
// later CreateFeature/evaluate waits on it. After the first SEH this latch
// fails every further SDK entry fast, and faulted modules never re-enter
// their shutdown paths. The state belongs to the process, not a filter
// instance, and deliberately has no reset/retry operation: only restarting
// the host (mpv) clears it.

#include <windows.h>
#include <atomic>
#include <mutex>

namespace vsdlssnr {

class NgxRuntimeGuard {
public:
    static bool IsFaulted() noexcept { return _faultCode.load(std::memory_order_acquire) != 0; }
    static DWORD FaultCode() noexcept { return _faultCode.load(std::memory_order_acquire); }
    static uintptr_t FaultAddress() noexcept { return _faultAddress.load(std::memory_order_relaxed); }
    static DWORD FaultThread() noexcept { return _faultThread.load(std::memory_order_relaxed); }

    template <typename Function, typename Result>
    static Result Invoke(Function &&function, Result failure, DWORD *sehCode) noexcept {
        // Serialize the availability check with SDK entry and fault publication.
        // This lock is outside the SEH frame, so ordinary C++ cleanup releases it.
        std::lock_guard lock(_callMutex);
        *sehCode = 0;
        if (IsFaulted()) return failure;
        return _InvokeSafely(function, failure, sehCode);
    }

    // A non-SEH failure of the final shutdown is equally unsafe to retry
    // (upstream: "final Shutdown1 failure is treated as not safe to retry").
    static void MarkShutdownFailed() noexcept {
        std::lock_guard lock(_callMutex);
        _RecordFault(ERROR_INVALID_STATE, 0);
    }

private:
    template <typename Function, typename Result>
    static Result _InvokeSafely(Function &function, Result failure, DWORD *sehCode) noexcept {
        __try {
            return function();
        } __except (_CaptureException(GetExceptionInformation(), sehCode)) {
            return failure;
        }
    }

    static LONG _CaptureException(EXCEPTION_POINTERS *exception, DWORD *sehCode) noexcept {
        *sehCode = exception->ExceptionRecord->ExceptionCode;
        _RecordFault(*sehCode, reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress));
        return EXCEPTION_EXECUTE_HANDLER;
    }

    static void _RecordFault(DWORD code, uintptr_t address) noexcept {
        if (IsFaulted()) return;
        _faultAddress.store(address, std::memory_order_relaxed);
        _faultThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        _faultCode.store(code, std::memory_order_release);
    }

    static inline std::mutex _callMutex;
    static inline std::atomic<DWORD> _faultCode = 0;
    static inline std::atomic<uintptr_t> _faultAddress = 0;
    static inline std::atomic<DWORD> _faultThread = 0;
};

} // namespace vsdlssnr
