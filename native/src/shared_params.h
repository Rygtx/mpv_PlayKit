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

    // UI thread: full live-params update from panel state. The create-time
    // fields (preset / inputResolutionPercent / scalingEnabled) are
    // deliberately NOT copied from `next`: their _cur side is owned by
    // ConsumeRebuild alone (panel_ipc.h invariant). A snapshot taken before
    // the matching Request* calls could otherwise revert a just-consumed
    // change and double-fire the rebuild.
    void Update(const DlssnrParams &next) {
        std::unique_lock lock(_lock);
        DlssnrParams merged = next;
        merged.preset = _cur.preset;
        merged.inputResolutionPercent = _cur.inputResolutionPercent;
        merged.scalingEnabled = _cur.scalingEnabled;
        _cur = merged;
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
    // pre-F1 preset invariant. The rebuild-worthiness rule itself lives in
    // dlssnr_params.h CreateParamsChanged, shared with DlssnrContext::Rebind.
    bool ConsumeRebuild(int &newPreset, int &newResolution, int &newScalingEnabled) {
        std::unique_lock lock(_lock);
        // Effective pending trio: unset pendings fall back to the current
        // values; the resolution is forced to 100 while scaling ends up
        // disabled (with scaling off the percent is ignored by the pipeline,
        // so it must never fire a rebuild on its own — a fresh scaling-off
        // load with a stored <100 percent used to recreate the just-created
        // feature on frame 1).
        DlssnrParams eff{};
        eff.preset = _pendingPreset >= 0 ? _pendingPreset : _cur.preset;
        eff.scalingEnabled = _pendingScalingEnabled >= 0 ? _pendingScalingEnabled : _cur.scalingEnabled;
        eff.inputResolutionPercent =
            eff.scalingEnabled
                ? (_pendingResolution > 0 ? _pendingResolution : _cur.inputResolutionPercent)
                : 100;
        if (!CreateParamsChanged(eff, _cur)) {
            newPreset = _cur.preset;
            newResolution = _cur.inputResolutionPercent;
            newScalingEnabled = _cur.scalingEnabled;
            return false;
        }
        // Sync only what changed. The resolution syncs only while scaling is
        // on: preserving the stored percent otherwise keeps the pending
        // request alive for the next scaling re-enable (one rebuild instead
        // of two).
        if (eff.preset != _cur.preset) _cur.preset = eff.preset;
        if (eff.scalingEnabled != _cur.scalingEnabled) _cur.scalingEnabled = eff.scalingEnabled;
        if (eff.scalingEnabled && eff.inputResolutionPercent != _cur.inputResolutionPercent) {
            _cur.inputResolutionPercent = eff.inputResolutionPercent;
        }
        newPreset = _cur.preset;
        newResolution = _cur.inputResolutionPercent;
        newScalingEnabled = _cur.scalingEnabled;
        return true;
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
