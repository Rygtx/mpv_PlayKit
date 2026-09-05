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

    // UI thread: request a preset / internal-resolution / scaling-toggle change
    // (all create-time; the frame thread rebuilds the feature).
    void RequestPreset(int preset) {
        SRWLOCK_HELPER(this);
        _pendingPreset = preset;
    }

    void RequestResolution(int percent) {
        SRWLOCK_HELPER(this);
        _pendingResolution = percent;
    }

    void RequestScalingEnabled(int enabled) {
        SRWLOCK_HELPER(this);
        _pendingScalingEnabled = enabled;
    }

    // Frame thread: returns true and fills the current effective trio when a
    // rebuild is due. _cur for preset/resolution/scaling is synced ONLY here:
    // the bridge's Update() must not pre-sync create-time params, or the
    // pending-vs-current comparison dies (rebuild never fires) — mirrors the
    // pre-F1 preset invariant.
    bool ConsumeRebuild(int &newPreset, int &newResolution, int &newScalingEnabled) {
        SRWLOCK_HELPER(this);
        const bool presetChanged = _pendingPreset >= 0 && _pendingPreset != _cur.preset;
        const bool scalingChanged = _pendingScalingEnabled >= 0 && _pendingScalingEnabled != _cur.scalingEnabled;
        const bool scalingOff = (scalingChanged ? _pendingScalingEnabled : _cur.scalingEnabled) == 0;
        // Effective pending resolution: 100 when scaling ends up disabled.
        const int pendingRes = _pendingResolution > 0 ? _pendingResolution : _cur.inputResolutionPercent;
        const int effRes = scalingOff ? 100 : pendingRes;
        const bool resChanged = effRes != _cur.inputResolutionPercent;
        if (presetChanged) _cur.preset = _pendingPreset;
        if (scalingChanged) _cur.scalingEnabled = _pendingScalingEnabled;
        if (resChanged) _cur.inputResolutionPercent = effRes;
        newPreset = _cur.preset;
        newResolution = _cur.inputResolutionPercent;
        newScalingEnabled = _cur.scalingEnabled;
        return presetChanged || resChanged || scalingChanged;
    }

    // Preset value a save should persist (pending request wins over current).
    int SaveTimePreset() {
        SRWLOCK_HELPER(this);
        return _pendingPreset >= 0 ? _pendingPreset : _cur.preset;
    }

    // Resolution value a save should persist (pending request wins).
    int SaveTimeResolution() {
        SRWLOCK_HELPER(this);
        return _pendingResolution > 0 ? _pendingResolution : _cur.inputResolutionPercent;
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
    int _pendingResolution = -1;
    int _pendingScalingEnabled = -1;
};

} // namespace vsdlssnr
