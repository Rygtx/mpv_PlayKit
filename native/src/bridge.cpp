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
// INI_FILE / PARAMS_EVENT / ALIVE_EVENT come from panel_ipc.h (cross-process
// contract names). The bridge is event-driven (the panel signals
// PARAMS_EVENT after every payload write); POLL_INTERVAL_MS is the fallback
// poll cadence when the event does not exist, and the retry cadence for
// opening the panel's mapping.
constexpr int POLL_INTERVAL_MS = 40;

struct BridgeState {
    SharedParams *params = nullptr;
    HANDLE thread = nullptr;
    volatile bool running = false;
};

BridgeState *g_bridge = nullptr; // one live bridge at a time (last filter wins)

// Process-lifetime handles (the DLL is pinned, so "process" == one mpv
// playback session). The alive event, the edit-notification event and the
// panel-params read mapping must NOT die with a single filter instance: mpv
// tears down and recreates the whole VS core on every seek. Per-instance
// lifetimes made BridgeStop close the event → the panel watchdog exit →
// BridgeStart relaunch a fresh panel, which found the mapping gone and
// pushed factory defaults (res=100) through the bridge — recreating the
// feature behind the user's back on some seeks.
// Deliberately never closed: when mpv exits the kernel reclaims them and the
// panel's OpenEventW poll starts failing, which is exactly the old "panel
// follows the filter lifetime" semantics.
HANDLE g_aliveEvent = nullptr;    // named; existence = a live filter in-process
HANDLE g_paramsEvent = nullptr;   // named auto-reset; panel signals after each write
HANDLE g_paramsMapping = nullptr; // read handle keeping the payload object alive
const PanelPayload *g_paramsView = nullptr;

// ---------------------------------------------------------------------------
// ini persistence
// ---------------------------------------------------------------------------

// Directory of this DLL — one module-path resolution shared by the ini path
// and the panel launch path.
bool GetSelfDir(wchar_t *dir, size_t dirLen) noexcept {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetSelfDir), &self)) {
        return false;
    }
    wchar_t dllPath[MAX_PATH]{};
    if (!GetModuleFileNameW(self, dllPath, MAX_PATH)) return false;
    std::wstring dirStr = std::filesystem::path(dllPath).parent_path().wstring();
    if (dirStr.size() >= dirLen) return false;
    wcscpy_s(dir, dirLen, dirStr.c_str());
    return true;
}

bool GetSelfIniPath(wchar_t *path, size_t pathLen) noexcept {
    wchar_t dir[MAX_PATH];
    if (!GetSelfDir(dir, MAX_PATH)) return false;
    if (wcslen(dir) + 1 + wcslen(INI_FILE) >= pathLen) return false;
    swprintf_s(path, pathLen, L"%s\\%s", dir, INI_FILE);
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
            // 探针:采纳了面板哪一版 payload(#37"seek 后参数回退"的
            // 决定性证据 —— 采纳失败时这行缺失,配合 create params 行
            // 立刻看出 fallback 到了 ini 值)。
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                          "DLSSNR STATUS: adopt payload seq=%u preset=%d res=%d%% scaling=%d of=%d follow=%d",
                          snap.seq, p.preset, p.inputResolutionPercent,
                          p.scalingEnabled ? 1 : 0, p.motionVectorQuality,
                          p.nvofFollowScaling ? 1 : 0);
            TimingStatusLine(msg);
        }
    } else {
        // 探针:映射在但载荷不可用 = 异常态,必须留痕。magic 不符 =
        // 面板/插件版本不配对(DSSL5 面板 + DSSL6 插件,成对部署被破坏的
        // 第一现场);seq==0 = 面板创建映射后尚未写过(崩溃窗口)。
        char msg[128];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: adopt skipped (magic=0x%08X seq=%u)",
                      static_cast<unsigned>(view->magic), static_cast<unsigned>(view->seq));
        TimingStatusLine(msg);
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
    // 探针:每次应用的 payload 全量留痕(用户调参是低频事件)。与面板侧
    // 操作时间对齐后,"滑块动了没生效"可以直接看出是没写进来(seq 没动)、
    // 被 clamp、还是到了 SharedParams 之后才丢。
    {
        char msg[224];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: bridge apply seq=%u gen=%u preset=%d res=%d%% scaling=%d of=%d follow=%d save=%d log=%d",
                      static_cast<unsigned>(pl.seq), static_cast<unsigned>(pl.generation),
                      pl.preset, pl.inputResolution, pl.scalingEnabled,
                      pl.motionVectorQuality, pl.nvofFollowScaling,
                      pl.saveRequest, pl.logEnabled);
        TimingStatusLine(msg);
    }
    DlssnrParams p = state->params->Snapshot();
    LoadLiveParams(p, pl); // shared field mapping (panel_ipc.h), clamps included
    // Create-time params go through Request* only — their _cur side is synced
    // exclusively by SharedParams::ConsumeRebuild, otherwise the
    // pending-vs-current comparison dies and the rebuild never fires (and
    // Update() preserves them regardless, see shared_params.h). Map them
    // through LoadCreateParams so a new create-time payload field is picked
    // up here by construction.
    DlssnrParams create{};
    LoadCreateParams(create, pl);
    state->params->RequestPreset(create.preset);
    state->params->RequestResolution(create.inputResolutionPercent);
    state->params->RequestScalingEnabled(create.scalingEnabled);
    state->params->Update(p);

    if (pl.saveRequest) {
        wchar_t iniPath[MAX_PATH];
        if (GetSelfIniPath(iniPath, MAX_PATH)) {
            DlssnrParams s = state->params->Snapshot();
            s.preset = state->params->SaveTimePreset();
            s.inputResolutionPercent = state->params->SaveTimeResolution();
            // 探针:"保存设置"失败(文件被占/权限)此前完全静默,用户以为
            // 存上了,下次加载却又回到旧值。
            if (!WriteDlssnrIni(s, iniPath)) {
                TimingStatusLine("DLSSNR STATUS: save ini FAILED (bridge saveRequest)");
            }
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
        // Wake on the panel's signal when possible (an edit lands within
        // microseconds instead of up to one poll interval); the 40ms timeout
        // keeps the payload-open retry loop alive while no panel exists, and
        // is the fallback when the event could not be created.
        if (g_paramsEvent) {
            WaitForSingleObject(g_paramsEvent, POLL_INTERVAL_MS);
        } else {
            Sleep(POLL_INTERVAL_MS);
        }
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
            // 探针:参数通道建立(首启或面板重启后重开)。
            {
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "DLSSNR STATUS: bridge mapping open (seq=%u gen=%u)",
                              static_cast<unsigned>(lastSeq), static_cast<unsigned>(lastGeneration));
                TimingStatusLine(msg);
            }
            continue;
        }

        // The mapping object survives panel restarts (we hold a reference); a
        // zero seq means the (re)started panel has not written yet. A changed
        // generation resets the seq baseline so restarts never collide with
        // the previous instance's seq history.
        if (g_paramsView->generation != lastGeneration) {
            // 探针:面板重启边界(#39 面板死亡螺旋的计数锚点 —— 每次重启
            // 一行,几秒内连出多行 = 面板在反复被拉起)。
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "DLSSNR STATUS: bridge generation %u -> %u (panel restart)",
                          static_cast<unsigned>(lastGeneration), static_cast<unsigned>(g_paramsView->generation));
            TimingStatusLine(msg);
            lastGeneration = g_paramsView->generation;
            lastSeq = 0;
        }
        if (g_paramsView->magic != PAYLOAD_MAGIC) {
            // 探针:magic 不符 = 面板/插件版本不配对(成对部署被破坏)。
            // 此前静默 continue,"面板调参完全无效"无任何痕迹。按 magic 值
            // 边缘去重(同一错值只报一次)。
            static uint32_t reportedMagic = 0;
            const uint32_t m = g_paramsView->magic;
            if (m != reportedMagic) {
                reportedMagic = m;
                char msg[160];
                std::snprintf(msg, sizeof(msg),
                              "DLSSNR STATUS: bridge payload magic mismatch (got=0x%08X want=0x%08X); panel/plugin must be deployed as a pair",
                              static_cast<unsigned>(m), static_cast<unsigned>(PAYLOAD_MAGIC));
                TimingStatusLine(msg);
            }
            continue;
        }
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
// VSDLSSNR_NO_PANEL=1 跳过(无头/CI/数值验收场景:面板启动即发布 payload,
// 经 BridgeAdoptPanelPayload 覆盖 vpy 参数,哈希类测试必须无面板运行)。
void LaunchPanelSilently() noexcept {
    if (GetEnvironmentVariableA("VSDLSSNR_NO_PANEL", nullptr, 0) != 0) return;
    wchar_t dir[MAX_PATH], exePath[MAX_PATH];
    if (!GetSelfDir(dir, MAX_PATH)) return;
    if (wcslen(dir) + 1 + wcslen(PANEL_EXE) >= MAX_PATH) return;
    swprintf_s(exePath, L"%s\\%s", dir, PANEL_EXE);
    // 失败判定看返回值(<= 32),不看 GetLastError:ShellExecuteW 成功时
    // last-error 是 stale 值,原来的判定可能拿上次调用的残留误报。
    const HINSTANCE exec = ShellExecuteW(nullptr, L"open", exePath, nullptr, dir, SW_HIDE);
    if (reinterpret_cast<intptr_t>(exec) <= 32) {
        OutputDebugStringW(L"vs_dlssnr: dlssnr_panel.exe launch failed (missing next to plugin?)\n");
        // 探针:面板拉起失败此前只进 DebugView;"面板没出现"的排障从这行开始。
        TimingStatusLine("DLSSNR STATUS: dlssnr_panel.exe launch failed (missing next to plugin?)");
    }
}

} // namespace

bool BridgeStart(SharedParams *params) noexcept {
    if (g_bridge) {
        if (g_bridge->params == params) return true;
        // A newer filter instance is going live while the previous one is
        // still freeing (VS filter free order vs new-core create is not
        // guaranteed). Transfer ownership here: otherwise BridgeStop(old)
        // below would stop the only bridge and the new instance would never
        // see another panel edit.
        TimingStatusLine("DLSSNR STATUS: bridge ownership handoff (new filter instance)");
        BridgeStop(g_bridge->params);
    }
    auto *state = new (std::nothrow) BridgeState();
    if (!state) {
        TimingStatusLine("DLSSNR STATUS: bridge start FAILED (oom)");
        return false;
    }
    state->params = params;
    state->running = true;
    // Existence marker for the panel's watchdog: process-lifetime (never
    // closed) so a seek's BridgeStop/Start cycle doesn't kill the panel.
    if (!g_aliveEvent) {
        g_aliveEvent = CreateEventW(nullptr, TRUE, FALSE, ALIVE_EVENT);
        if (!g_aliveEvent) {
            delete state;
            TimingStatusLine("DLSSNR STATUS: bridge start FAILED (alive event create)");
            return false;
        }
    }
    // Edit-notification event for the poll loop (see BridgeThreadProc):
    // process-lifetime like the other handles; the panel opens it by name
    // and signals after every payload write. Failure falls back to polling.
    if (!g_paramsEvent) {
        g_paramsEvent = CreateEventW(nullptr, FALSE, FALSE, PARAMS_EVENT);
    }
    state->thread = CreateThread(nullptr, 0, BridgeThreadProc, state, 0, nullptr);
    if (!state->thread) {
        delete state;
        TimingStatusLine("DLSSNR STATUS: bridge start FAILED (thread create)");
        return false;
    }
    g_bridge = state;
    // 探针:bridge 生命周期锚点(每次 seek 的 stop/start 各一行,与
    // "hot rebind kept" 行构成完整的 seek 生命周期序列)。
    TimingStatusLine("DLSSNR STATUS: bridge started");
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
        const DWORD wait = WaitForSingleObject(state->thread, 5000);
        CloseHandle(state->thread);
        state->thread = nullptr;
        if (wait != WAIT_OBJECT_0) {
            // The thread is wedged (e.g. blocked writing the ini on a stalled
            // disk). Deleting `state` now would free objects the resumed
            // thread still dereferences — leak the tiny state block instead
            // of risking a use-after-free; the thread exits on its own once
            // the block clears. The alive event / mapping handles are
            // process-lifetime either way.
            TimingStatusLine("DLSSNR STATUS: bridge stop TIMED OUT (5s); state leaked, thread wedged");
            g_bridge = nullptr;
            return;
        }
    }
    // The alive event and the params mapping are process-lifetime (see the
    // globals): deliberately NOT closed here. mpv re-creates the whole VS
    // core on every seek; a per-instance event made the panel watchdog exit
    // and the relaunched panel push factory defaults mid-playback. When mpv
    // exits, the kernel reclaims the handles and the panel's OpenEventW poll
    // starts failing — the watchdog semantics are unchanged.
    TimingStatusLine("DLSSNR STATUS: bridge stopped");
    delete state;
    g_bridge = nullptr;
}

} // namespace vsdlssnr
