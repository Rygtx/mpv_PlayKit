#include "bridge.h"
#include "dlssnr_context.h"
#include "dlssnr_ini.h"
#include "panel_ipc.h"

#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <new>

#pragma comment(lib, "shell32.lib")

namespace vsdlssnr {

namespace {

constexpr wchar_t PANEL_EXE[] = L"dlssnr_panel.exe";
// INI_FILE / ALIVE_EVENT come from panel_ipc.h (cross-process contract names)
// Shared-memory polling: no disk IO, so poll fast enough for slider edits to
// land within a frame or two.
constexpr int POLL_INTERVAL_MS = 40;

struct BridgeState {
    SharedParams *params = nullptr;
    HANDLE thread = nullptr;
    volatile bool running = false;
};

BridgeState *g_bridge = nullptr; // one live bridge at a time (last filter wins)

// Process-lifetime handles (the DLL is pinned, so "process" == one mpv
// playback session). The alive event and the panel-params read mapping must
// NOT die with a single filter instance: mpv tears down and recreates the
// whole VS core on every seek. Per-instance lifetimes made BridgeStop close
// the event → the panel watchdog exit → BridgeStart relaunch a fresh panel,
// which found the mapping gone and pushed factory defaults (res=100) through
// the bridge — recreating the feature behind the user's back on some seeks.
// Deliberately never closed: when mpv exits the kernel reclaims them and the
// panel's OpenEventW poll starts failing, which is exactly the old "panel
// follows the filter lifetime" semantics.
HANDLE g_aliveEvent = nullptr;    // named; existence = a live filter in-process
HANDLE g_paramsMapping = nullptr; // read handle keeping the payload object alive
const PanelPayload *g_paramsView = nullptr;

// ---------------------------------------------------------------------------
// ini persistence
// ---------------------------------------------------------------------------

bool GetSelfIniPath(wchar_t *path, size_t pathLen) noexcept {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetSelfIniPath), &self)) {
        return false;
    }
    wchar_t dllPath[MAX_PATH]{};
    if (!GetModuleFileNameW(self, dllPath, MAX_PATH)) return false;
    std::wstring dir = std::filesystem::path(dllPath).parent_path().wstring();
    if (dir.size() + 1 + wcslen(INI_FILE) >= pathLen) return false;
    swprintf_s(path, pathLen, L"%s\\%s", dir.c_str(), INI_FILE);
    return true;
}

} // namespace

bool BridgeLoadIni(DlssnrParams &p) noexcept {
    wchar_t iniPath[MAX_PATH];
    if (!GetSelfIniPath(iniPath, MAX_PATH)) return false;
    return LoadDlssnrIni(p, iniPath); // shared key list + clamps (dlssnr_ini.h)
}

// Adopt the panel's CURRENT payload (last live state) onto `p`. Called once
// per filter instance at create time, after BridgeLoadIni: a seek tears down
// the whole VS core, so the new SharedParams would otherwise start from the
// stale ini/vpy values while the bridge's skip-history guard never applies
// the panel's existing payload — the parameters visibly fell back to the ini
// after every seek until the user touched the panel again. This mirrors the
// panel's own startup adopt (panel_ipc.h CreateParamsMapping): last live
// state wins over the ini. Tear-safe read, same protocol as the poll thread.
bool BridgeAdoptPanelPayload(DlssnrParams &p) noexcept {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, PARAMS_MAPPING);
    if (!mapping) return false; // no panel this session: keep ini/vpy values
    const PanelPayload *view = static_cast<const PanelPayload *>(
        MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, PAYLOAD_SIZE));
    if (!view) {
        CloseHandle(mapping);
        return false;
    }
    bool adopted = false;
    if (view->magic == PAYLOAD_MAGIC && view->seq != 0) {
        PanelPayload snap;
        memcpy(&snap, view, sizeof(snap));
        if (static_cast<const volatile PanelPayload *>(view)->seq == snap.seq) {
            LoadLiveParams(p, snap); // shared field mapping, clamps included
            LoadCreateParams(p, snap);
            adopted = true;
        }
    }
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return adopted;
}

namespace {

// Apply one panel payload onto SharedParams. Create-time params
// (preset/input_resolution/scaling_enabled) go through Request* only: their
// _cur side is synced exclusively by SharedParams::ConsumeRebuild, otherwise
// the pending-vs-current comparison dies and the rebuild never fires.
// residualMultiplier is per-frame (compute cbuffer) and merges directly.
// A reset payload needs no special handling: the panel ships the default
// values inside the same payload, so the Request*/Update path below restores
// them (writing Initial() into _cur directly would stomp the create-time
// fields past the rebuild machinery and leave NGX on the old preset).
void ApplyPanelPayload(BridgeState *state, const PanelPayload &pl) noexcept {
    DlssnrParams p = state->params->Snapshot();
    LoadLiveParams(p, pl); // shared field mapping (panel_ipc.h), clamps included
    // Create-time params: request only — their _cur side is synced
    // exclusively by SharedParams::ConsumeRebuild, otherwise
    // the pending-vs-current comparison dies and the rebuild never fires.
    state->params->RequestPreset(std::clamp(pl.preset, kPresetMin, kPresetMax));
    state->params->RequestResolution(std::clamp(pl.inputResolution, kResPctMin, kResPctMax));
    state->params->RequestScalingEnabled(pl.scalingEnabled != 0);
    state->params->Update(p);

    if (pl.saveRequest) {
        wchar_t iniPath[MAX_PATH];
        if (GetSelfIniPath(iniPath, MAX_PATH)) {
            DlssnrParams s = state->params->Snapshot();
            s.preset = state->params->SaveTimePreset();
            s.inputResolutionPercent = state->params->SaveTimeResolution();
            WriteDlssnrIni(s, iniPath);
        }
    }
    SetTimingLogEnabled(pl.logEnabled != 0);
}

DWORD WINAPI BridgeThreadProc(LPVOID param) noexcept {
    auto *state = static_cast<BridgeState *>(param);

    // Panel parameters come over shared memory (panel creates the mapping;
    // we wait for it - without a panel there is simply nothing to apply).
    // Handles are process-level (g_paramsMapping/g_paramsView): they survive
    // bridge restarts so the payload survives seeks and panel restarts.
    uint32_t lastSeq = 0;
    uint32_t lastGeneration = 0;

    while (state->running) {
        Sleep(POLL_INTERVAL_MS);
        if (!state->running) break;

        if (!g_paramsView) {
            g_paramsMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, PARAMS_MAPPING);
            if (!g_paramsMapping) continue;
            g_paramsView = static_cast<const PanelPayload *>(
                MapViewOfFile(g_paramsMapping, FILE_MAP_READ, 0, 0, PAYLOAD_SIZE));
            if (!g_paramsView) {
                CloseHandle(g_paramsMapping);
                g_paramsMapping = nullptr;
                continue;
            }
            lastGeneration = g_paramsView->generation;
            lastSeq = g_paramsView->seq; // skip history: apply only future edits
            continue;
        }

        // The mapping object survives panel restarts (we hold a reference); a
        // zero seq means the (re)started panel has not written yet. A changed
        // generation resets the seq baseline so restarts never collide with
        // the previous instance's seq history.
        if (g_paramsView->generation != lastGeneration) {
            lastGeneration = g_paramsView->generation;
            lastSeq = 0;
        }
        if (g_paramsView->magic != PAYLOAD_MAGIC) continue;
        if (g_paramsView->seq == 0 || g_paramsView->seq == lastSeq) continue;
        // Snapshot under the writer: copy the payload, then confirm the seq
        // did not move mid-copy (the panel publishes the counter only after
        // the body is stable — a volatile re-read keeps the compiler from
        // forwarding the pre-copy load).
        PanelPayload snap;
        memcpy(&snap, g_paramsView, sizeof(snap));
        if (static_cast<const volatile PanelPayload *>(g_paramsView)->seq != snap.seq ||
            snap.seq == lastSeq) {
            continue;
        }
        lastSeq = snap.seq;
        ApplyPanelPayload(state, snap);
    }

    // No UnmapViewOfFile/CloseHandle here: the handles are process-lifetime
    // (see the globals) so the payload survives seeks and panel restarts.
    return 0;
}

// Silently start the independent panel exe (single-instance guarded there).
// Called on filter load so the tray icon appears without user action.
void LaunchPanelSilently() noexcept {
    wchar_t exePath[MAX_PATH];
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&LaunchPanelSilently), &self)) {
        return;
    }
    wchar_t dllPath[MAX_PATH]{};
    if (!GetModuleFileNameW(self, dllPath, MAX_PATH)) return;
    std::wstring dir = std::filesystem::path(dllPath).parent_path().wstring();
    if (dir.size() + 1 + wcslen(PANEL_EXE) >= MAX_PATH) return;
    swprintf_s(exePath, L"%s\\%s", dir.c_str(), PANEL_EXE);
    ShellExecuteW(nullptr, L"open", exePath, nullptr, dir.c_str(), SW_HIDE);
    if (GetLastError() == ERROR_FILE_NOT_FOUND) {
        OutputDebugStringW(L"vs_dlssnr: dlssnr_panel.exe not found next to plugin\n");
    }
}

} // namespace

bool BridgeStart(SharedParams *params) noexcept {
    if (g_bridge) return true; // already running for a live instance
    auto *state = new (std::nothrow) BridgeState();
    if (!state) return false;
    state->params = params;
    state->running = true;
    // Existence marker for the panel's watchdog: process-lifetime (never
    // closed) so a seek's BridgeStop/Start cycle doesn't kill the panel.
    if (!g_aliveEvent) {
        g_aliveEvent = CreateEventW(nullptr, TRUE, FALSE, ALIVE_EVENT);
        if (!g_aliveEvent) {
            delete state;
            return false;
        }
    }
    state->thread = CreateThread(nullptr, 0, BridgeThreadProc, state, 0, nullptr);
    if (!state->thread) {
        delete state;
        return false;
    }
    g_bridge = state;
    // Bring up the independent panel (tray-only, silent start). The panel
    // process is single-instance, so repeated filter loads are no-ops there.
    LaunchPanelSilently();
    return true;
}

void BridgeStop(SharedParams *params) noexcept {
    BridgeState *state = g_bridge;
    if (!state || state->params != params) return; // newer instance owns the bridge
    state->running = false;
    if (state->thread) {
        WaitForSingleObject(state->thread, 5000);
        CloseHandle(state->thread);
        state->thread = nullptr;
    }
    // The alive event and the params mapping are process-lifetime (see the
    // globals): deliberately NOT closed here. mpv re-creates the whole VS
    // core on every seek; a per-instance event made the panel watchdog exit
    // and the relaunched panel push factory defaults mid-playback. When mpv
    // exits, the kernel reclaims the handles and the panel's OpenEventW poll
    // starts failing — the watchdog semantics are unchanged.
    delete state;
    g_bridge = nullptr;
}

} // namespace vsdlssnr
