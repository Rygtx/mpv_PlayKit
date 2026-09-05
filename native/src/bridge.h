#pragma once
// mpv-integrated parameter bridge: a lua script (uosc menu) writes
// dlssnr_live.json next to this plugin; the bridge polls that file and applies
// changes onto SharedParams. "save" requests persist SharedParams to
// dlssnr_ui.ini which later loads use as defaults.

#include "shared_params.h"

namespace vsdlssnr {

// Start the bridge (poll thread). Blocks briefly until the thread is up.
// Returns false if startup failed (plugin keeps working).
bool BridgeStart(SharedParams *params) noexcept;
// Stop the bridge iff it belongs to this SharedParams instance (a newer
// filter instance may already own it; VS filter free order is not guaranteed).
void BridgeStop(SharedParams *params) noexcept;

// Load panel-saved defaults from dlssnr_ui.ini (plugin dir), if present.
// Returns true when a saved profile was applied onto `p`.
bool BridgeLoadIni(DlssnrParams &p) noexcept;

} // namespace vsdlssnr
