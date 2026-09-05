#include "bridge.h"

#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <new>

#pragma comment(lib, "shell32.lib")

namespace vsdlssnr {

namespace {

constexpr wchar_t INI_FILE[] = L"dlssnr_ui.ini";
constexpr wchar_t LIVE_FILE[] = L"dlssnr_live.json";
constexpr wchar_t PANEL_EXE[] = L"dlssnr_panel.exe";
constexpr wchar_t ALIVE_EVENT[] = L"vs_dlssnr_bridge_alive";
constexpr int POLL_INTERVAL_MS = 150;

struct BridgeState {
    SharedParams *params = nullptr;
    HANDLE thread = nullptr;
    HANDLE aliveEvent = nullptr; // existence marks a live filter (panel watches it)
    volatile bool running = false;
    FILETIME liveBaseline{}; // ignore pre-existing live file from an old session
};

BridgeState *g_bridge = nullptr; // one live bridge at a time (last filter wins)

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

void SaveIni(const DlssnrParams &p, const wchar_t *iniPath) noexcept {
    wchar_t buf[32];
    auto writeInt = [&](const wchar_t *key, int v) {
        swprintf_s(buf, L"%d", v);
        WritePrivateProfileStringW(L"dlssnr", key, buf, iniPath);
    };
    // std::lround rounds half away from zero; (int)(v*100+0.5) would eat negatives
    auto writeX100 = [&](const wchar_t *key, float v) {
        writeInt(key, static_cast<int>(std::lround(v * 100.0f)));
    };
    writeInt(L"preset", p.preset);
    writeInt(L"style", p.style);
    writeX100(L"intensity_x100", p.intensity);
    writeX100(L"local_tone_x100", p.localToneStrength);
    writeX100(L"local_structure_x100", p.localStructureStrength);
    writeX100(L"skin_structure_x100", p.skinStructureStrength);
    writeInt(L"use_auto_mask", p.useAutoMask ? 1 : 0);
    writeInt(L"ui_correction", p.uiCorrection ? 1 : 0);
    writeInt(L"input_resolution", std::clamp(p.inputResolutionPercent, 25, 100));
    writeX100(L"residual_multiplier_x100", p.residualMultiplier);
    writeInt(L"saved", 1);
}

} // namespace

bool BridgeLoadIni(DlssnrParams &p) noexcept {
    wchar_t iniPath[MAX_PATH];
    if (!GetSelfIniPath(iniPath, MAX_PATH)) return false;
    wchar_t buf[64]{};
    if (!GetPrivateProfileStringW(L"dlssnr", L"saved", L"", buf, 64, iniPath) || !buf[0]) {
        return false; // no saved profile
    }
    const auto readInt = [&](const wchar_t *key, int def) -> int {
        return static_cast<int>(GetPrivateProfileIntW(L"dlssnr", key, def, iniPath));
    };
    p.preset = std::clamp(readInt(L"preset", p.preset), 0, 3);
    p.style = std::clamp(readInt(L"style", p.style), 0, 2);
    p.intensity = static_cast<float>(readInt(L"intensity_x100", static_cast<int>(p.intensity * 100))) / 100.0f;
    p.localToneStrength = static_cast<float>(readInt(L"local_tone_x100", static_cast<int>(p.localToneStrength * 100))) / 100.0f;
    p.localStructureStrength = static_cast<float>(readInt(L"local_structure_x100", static_cast<int>(p.localStructureStrength * 100))) / 100.0f;
    p.skinStructureStrength = static_cast<float>(readInt(L"skin_structure_x100", static_cast<int>(p.skinStructureStrength * 100))) / 100.0f;
    p.useAutoMask = readInt(L"use_auto_mask", p.useAutoMask ? 1 : 0) != 0;
    p.uiCorrection = readInt(L"ui_correction", p.uiCorrection ? 1 : 0) != 0;
    p.inputResolutionPercent = std::clamp(readInt(L"input_resolution", p.inputResolutionPercent), 25, 100);
    p.scalingEnabled = readInt(L"scaling_enabled", p.scalingEnabled);
    p.residualMultiplier = static_cast<float>(readInt(L"residual_multiplier_x100", static_cast<int>(p.residualMultiplier * 100))) / 100.0f;
    return true;
}

namespace {

// ---------------------------------------------------------------------------
// live-file polling
// ---------------------------------------------------------------------------

bool GetSelfLivePath(wchar_t *path, size_t pathLen) noexcept {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetSelfLivePath), &self)) {
        return false;
    }
    wchar_t dllPath[MAX_PATH]{};
    if (!GetModuleFileNameW(self, dllPath, MAX_PATH)) return false;
    std::wstring dir = std::filesystem::path(dllPath).parent_path().wstring();
    if (dir.size() + 1 + wcslen(LIVE_FILE) >= pathLen) return false;
    swprintf_s(path, pathLen, L"%s\\%s", dir.c_str(), LIVE_FILE);
    return true;
}

bool GetFileMTime(const wchar_t *path, FILETIME &ft) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attr)) return false;
    ft = attr.ftLastWriteTime;
    return true;
}

// Flat-object JSON extraction (our own writer's format only)
float JsonGetFloat(const char *body, const char *key, float def) noexcept {
    char pat[48];
    std::snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *k = strstr(body, pat);
    if (!k) return def;
    const char *c = strchr(k + strlen(pat), ':');
    if (!c) return def;
    return strtof(c + 1, nullptr);
}

int JsonGetInt(const char *body, const char *key, int def) noexcept {
    return static_cast<int>(JsonGetFloat(body, key, static_cast<float>(def)));
}

void ApplyLiveFile(BridgeState *state) noexcept {
    wchar_t livePath[MAX_PATH];
    if (!GetSelfLivePath(livePath, MAX_PATH)) return;
    FILE *f = nullptr;
    if (_wfopen_s(&f, livePath, L"rb") != 0 || !f) return;
    char body[2048]{};
    const size_t n = fread(body, 1, sizeof(body) - 1, f);
    fclose(f);
    if (!n) return;

    // Merge: only keys present in the file are applied (defaults = current).
    // Create-time params (preset/input_resolution/scaling_enabled) must NOT
    // land in the Update() merge below: _cur for them is synced exclusively by
    // SharedParams::ConsumeRebuild, otherwise the pending-vs-current comparison
    // dies and the rebuild never fires. residualMultiplier is per-frame.
    DlssnrParams p = state->params->Snapshot();
    const int preset = JsonGetInt(body, "preset", p.preset);
    const int reqRes = std::clamp(JsonGetInt(body, "input_resolution", p.inputResolutionPercent), 25, 100);
    const int reqScaling = JsonGetInt(body, "scaling_enabled", p.scalingEnabled);
    p.style = JsonGetInt(body, "style", p.style);
    p.intensity = std::clamp(JsonGetFloat(body, "intensity", p.intensity), 0.0f, 2.0f);
    p.localToneStrength = std::clamp(JsonGetFloat(body, "local_tone", p.localToneStrength), 0.0f, 2.0f);
    p.localStructureStrength = std::clamp(JsonGetFloat(body, "local_structure", p.localStructureStrength), 0.0f, 2.0f);
    p.skinStructureStrength = std::clamp(JsonGetFloat(body, "skin_structure", p.skinStructureStrength), -1.0f, 2.0f);
    p.useAutoMask = JsonGetInt(body, "use_auto_mask", p.useAutoMask ? 1 : 0) != 0;
    p.uiCorrection = JsonGetInt(body, "ui_correction", p.uiCorrection ? 1 : 0) != 0;
    p.residualMultiplier = std::clamp(JsonGetFloat(body, "residual_multiplier", p.residualMultiplier), 1.0f, 2.0f);
    state->params->RequestPreset(std::clamp(preset, 0, 3));
    state->params->RequestResolution(reqRes);
    state->params->RequestScalingEnabled(reqScaling);
    state->params->Update(p);

    // Command keys run after the merge so __save persists the just-applied values.
    if (JsonGetInt(body, "__save", 0) != 0) {
        wchar_t iniPath[MAX_PATH];
        if (GetSelfIniPath(iniPath, MAX_PATH)) {
            DlssnrParams s = state->params->Snapshot();
            s.preset = state->params->SaveTimePreset();
            s.inputResolutionPercent = state->params->SaveTimeResolution();
            SaveIni(s, iniPath);
        }
    }
    if (JsonGetInt(body, "__reset", 0) != 0) {
        state->params->Update(state->params->Initial());
    }
}

DWORD WINAPI BridgeThreadProc(LPVOID param) noexcept {
    auto *state = static_cast<BridgeState *>(param);

    wchar_t livePath[MAX_PATH];
    if (!GetSelfLivePath(livePath, MAX_PATH)) return 1;
    GetFileMTime(livePath, state->liveBaseline); // ignore stale file from old session

    while (state->running) {
        Sleep(POLL_INTERVAL_MS);
        if (!state->running) break;
        FILETIME ft{};
        if (!GetFileMTime(livePath, ft)) continue;
        if (CompareFileTime(&ft, &state->liveBaseline) != 0) {
            state->liveBaseline = ft;
            ApplyLiveFile(state);
        }
    }
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
    // Existence marker for the panel's watchdog (closed in BridgeStop, and
    // destroyed by the kernel even if mpv dies without cleanup).
    state->aliveEvent = CreateEventW(nullptr, TRUE, FALSE, ALIVE_EVENT);
    state->thread = CreateThread(nullptr, 0, BridgeThreadProc, state, 0, nullptr);
    if (!state->thread) {
        if (state->aliveEvent) CloseHandle(state->aliveEvent);
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
    // Release the alive marker first: the panel watchdog sees it disappear and
    // exits, taking the tray icon with it.
    if (state->aliveEvent) {
        CloseHandle(state->aliveEvent);
        state->aliveEvent = nullptr;
    }
    // The panel process is independent; its watchdog decides its lifetime.
    delete state;
    g_bridge = nullptr;
}

} // namespace vsdlssnr
