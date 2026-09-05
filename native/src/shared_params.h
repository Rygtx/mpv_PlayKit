#pragma once
// Runtime-mutable parameter store shared between the tray-panel UI thread and
// the frame thread. DLSSNR's intensity/style/... are per-evaluate NGX keys, so
// panel edits take effect on the very next frame; preset is a create-time key
// and is handled as a rebuild request consumed by the frame thread.

#include "dlssnr_params.h"
#include <mutex>
#include <shared_mutex>

namespace vsdlssnr {

class SharedParams {
public:
    explicit SharedParams(const DlssnrParams &initial) : _cur(initial) {}

    // Frame thread: point-in-time copy for this frame's evaluate call.
    DlssnrParams Snapshot() const {
        std::shared_lock lock(_lock);
        return _cur;
    }

    // UI thread: full update from panel state.
    void Update(const DlssnrParams &next) {
        std::unique_lock lock(_lock);
        _cur = next;
    }

    // UI thread: request a preset / internal-resolution / scaling-toggle change
    // (all create-time; the frame thread rebuilds the feature).
    void RequestPreset(int preset) {
        std::unique_lock lock(_lock);
        _pendingPreset = preset;
    }

    void RequestResolution(int percent) {
        std::unique_lock lock(_lock);
        _pendingResolution = percent;
    }

    void RequestScalingEnabled(int enabled) {
        std::unique_lock lock(_lock);
        _pendingScalingEnabled = enabled;
    }

    // Frame thread: returns true and fills the current effective trio when a
    // rebuild is due. _cur for preset/resolution/scaling is synced ONLY here:
    // the bridge's Update() must not pre-sync create-time params, or the
    // pending-vs-current comparison dies (rebuild never fires) — mirrors the
    // pre-F1 preset invariant.
    bool ConsumeRebuild(int &newPreset, int &newResolution, int &newScalingEnabled) {
        std::unique_lock lock(_lock);
        const bool presetChanged = _pendingPreset >= 0 && _pendingPreset != _cur.preset;
        const bool scalingChanged = _pendingScalingEnabled >= 0 && _pendingScalingEnabled != _cur.scalingEnabled;
        const bool scalingOff = (scalingChanged ? _pendingScalingEnabled : _cur.scalingEnabled) == 0;
        // Effective pending resolution: forced 100 while scaling ends up
        // disabled. A resolution change is rebuild-worthy only while scaling
        // is (staying) on — with scaling off the percent is ignored by the
        // pipeline, so it must never fire a rebuild on its own (a fresh
        // scaling-off load with a stored <100 percent used to recreate the
        // just-created feature on frame 1).
        const int pendingRes = scalingOff
                                   ? 100
                                   : (_pendingResolution > 0 ? _pendingResolution : _cur.inputResolutionPercent);
        const bool resChanged = !scalingOff && pendingRes != _cur.inputResolutionPercent;
        if (presetChanged) _cur.preset = _pendingPreset;
        if (scalingChanged) _cur.scalingEnabled = _pendingScalingEnabled;
        if (resChanged) _cur.inputResolutionPercent = pendingRes;
        newPreset = _cur.preset;
        newResolution = _cur.inputResolutionPercent;
        newScalingEnabled = _cur.scalingEnabled;
        return presetChanged || resChanged || scalingChanged;
    }

    // Preset value a save should persist (pending request wins over current).
    int SaveTimePreset() {
        std::unique_lock lock(_lock);
        return _pendingPreset >= 0 ? _pendingPreset : _cur.preset;
    }

    // Resolution value a save should persist (pending request wins).
    int SaveTimeResolution() {
        std::unique_lock lock(_lock);
        return _pendingResolution > 0 ? _pendingResolution : _cur.inputResolutionPercent;
    }

private:
    // MSVC's std::shared_mutex is an SRWLOCK wrapper; the read-side Snapshot
    // takes the shared tier, everything that mutates takes unique.
    mutable std::shared_mutex _lock{};
    DlssnrParams _cur;
    int _pendingPreset = -1;
    int _pendingResolution = -1;
    int _pendingScalingEnabled = -1;
};

} // namespace vsdlssnr
