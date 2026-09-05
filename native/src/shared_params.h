#pragma once
// Runtime-mutable parameter store shared between the tray-panel UI thread and
// the frame thread. DLSSNR's intensity/style/... are per-evaluate NGX keys, so
// panel edits take effect on the very next frame; preset is a create-time key
// and is handled as a rebuild request consumed by the frame thread.

#include "dlssnr_params.h"
#include <windows.h>

namespace vsdlssnr {

class SharedParams {
public:
    explicit SharedParams(const DlssnrParams &initial) : _cur(initial), _initial(initial) {
        InitializeSRWLock(&_lock);
    }

    // Frame thread: point-in-time copy for this frame's evaluate call.
    DlssnrParams Snapshot() const {
        SRWLOCK_HELPER(const_cast<SharedParams *>(this));
        return _cur;
    }

    // UI thread: full update from panel state.
    void Update(const DlssnrParams &next) {
        SRWLOCK_HELPER(this);
        _cur = next;
    }

    // UI thread: request a preset change (feature rebuild on frame thread).
    void RequestPreset(int preset) {
        SRWLOCK_HELPER(this);
        _pendingPreset = preset;
    }

    // Frame thread: returns true and fills newPreset once per request.
    bool ConsumePresetChange(int &newPreset) {
        SRWLOCK_HELPER(this);
        const bool has = _pendingPreset >= 0 && _pendingPreset != _cur.preset;
        if (has) {
            newPreset = _pendingPreset;
            _cur.preset = newPreset;
        }
        return has;
    }

    // Preset value a save should persist (pending request wins over current).
    int SaveTimePreset() {
        SRWLOCK_HELPER(this);
        return _pendingPreset >= 0 ? _pendingPreset : _cur.preset;
    }

    DlssnrParams Initial() const { return _initial; }

private:
    struct SRWLOCK_HELPER {
        explicit SRWLOCK_HELPER(SharedParams *p) : _p(p) {
            AcquireSRWLockExclusive(&_p->_lock);
        }
        ~SRWLOCK_HELPER() { ReleaseSRWLockExclusive(&_p->_lock); }
        SRWLOCK_HELPER(const SRWLOCK_HELPER &) = delete;
        SRWLOCK_HELPER &operator=(const SRWLOCK_HELPER &) = delete;
        SharedParams *_p;
    };

    mutable SRWLOCK _lock{};
    DlssnrParams _cur;
    DlssnrParams _initial;
    int _pendingPreset = -1;
};

} // namespace vsdlssnr
