#pragma once
// Panel parameter channel: the tray panel (dlssnr_panel.exe) pushes the full
// parameter set into a named shared-memory mapping (panel_ipc.h); the bridge
// polls it and applies changes onto SharedParams. "save" requests persist
// SharedParams to dlssnr_ui.ini which later loads use as defaults.

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

// Adopt the panel's CURRENT payload (last live state, create-time params
// included) onto `p`. Call at filter-create time, after BridgeLoadIni:
// panel state wins over the ini, mirroring the panel's own startup adopt.
// Returns false when no panel payload exists (mapping absent / empty / torn).
bool BridgeAdoptPanelPayload(DlssnrParams &p) noexcept;

} // namespace vsdlssnr
