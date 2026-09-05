// dlssnr_panel - independent control panel for the vs_dlssnr VapourSynth
// plugin. ImGui + D3D11 rendered UI (no native controls). Pushes the full
// parameter set into a named shared-memory mapping (panel_ipc.h; the plugin's
// bridge polls it) for live tuning, and dlssnr_ui.ini for saved defaults.

#include "dlssnr_params.h"
#include "panel_ipc.h"

using namespace vsdlssnr;

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr wchar_t WINDOW_CLASS[] = L"vs_dlssnr_panel_app";
constexpr wchar_t WINDOW_TITLE[] = L"DLSSNR 控制面板";
constexpr wchar_t TRAY_TIP[] = L"DLSSNR 控制面板";
constexpr wchar_t INI_FILE[] = L"dlssnr_ui.ini";
constexpr wchar_t ALIVE_EVENT[] = L"vs_dlssnr_bridge_alive";
constexpr UINT WM_APP_TRAYICON = WM_APP + 1;

// clang-format off
constexpr struct { const char *key; const wchar_t *label; const wchar_t *tip;
                   float def, lo, hi; } kSliders[] = {
    { "intensity",         L"强度",     L"整体处理强度(0-2,默认 1)。数值越高降噪/增强越明显。", 1, 0, 2 },
    { "local_tone",        L"局部色调", L"局部色调强度(0-2,默认 1)。影响明暗过渡区域的处理力度。", 1, 0, 2 },
    { "local_structure",   L"局部结构", L"局部结构强度(0-2,默认 1)。越高保留越多细节纹理。", 1, 0, 2 },
    { "skin_structure",    L"皮肤结构", L"皮肤结构强度(-1=保持默认行为,范围 -1~2)。影响人物皮肤区域的细节保留。", -1, -1, 2 },
};
constexpr struct { const char *key; const wchar_t *label; const wchar_t *tip;
                   int def; int lo; int hi; } kEnums[] = {
    { "preset", L"预设", L"NR 推理预设:0=默认,1-3=预设 #1/#2/#3。切换会短暂重建模型(毫秒级)。", 0, 0, 3 },
    { "style",  L"风格", L"处理风格:0=默认,1=自然(Natural),2=电影(Cinematic)。", 0, 0, 2 },
};
constexpr struct { const char *key; const wchar_t *label; const wchar_t *tip;
                   bool def; } kFlags[] = {
    { "use_auto_mask", L"自动蒙版", L"自动蒙版。模型自动识别区域并区别处理。", true },
    { "ui_correction", L"UI 文字修正", L"UI 修正。降低对画面内文字/UI 元素的涂抹。", true },
};
// clang-format on

struct AppState {
    DlssnrParams params{};
    float uiScale = 1.0f;
    int dpi = 96;
    bool liveDirty = false;
    bool timingLog = true;
    double lastLiveWrite = 0.0;
    char status[160]{};
};

AppState g_app;
DWORD g_mainThreadId = 0;
HWND g_hwnd = nullptr;

// ---------------------------------------------------------------------------
// paths + persistence
// ---------------------------------------------------------------------------

bool BasePath(wchar_t *path, size_t len) noexcept {
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    std::wstring dir = std::filesystem::path(exe).parent_path().wstring();
    if (dir.size() >= len) return false;
    wcscpy_s(path, len, dir.c_str());
    return true;
}

// Shared-memory parameter channel: the panel creates the mapping and pushes
// the full parameter set on every edit (seq-gated); the plugin polls it.
HANDLE g_paramsMapping = nullptr;
PanelPayload *g_payload = nullptr;
uint32_t g_generation = 0;

bool CreateParamsMapping() noexcept {
    g_generation = GetTickCount();
    g_paramsMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                         0, PAYLOAD_SIZE, PARAMS_MAPPING);
    if (!g_paramsMapping) return false;
    g_payload = static_cast<PanelPayload *>(
        MapViewOfFile(g_paramsMapping, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, PAYLOAD_SIZE));
    if (!g_payload) {
        CloseHandle(g_paramsMapping);
        g_paramsMapping = nullptr;
        return false;
    }
    // Adopt parameters from a previous panel session (last live state wins
    // over the ini), then publish our generation + current values.
    if (g_payload->magic == PAYLOAD_MAGIC && g_payload->seq > 0) {
        g_app.params.preset = std::clamp(g_payload->preset, 0, 3);
        g_app.params.style = std::clamp(g_payload->style, 0, 2);
        g_app.params.intensity = std::clamp(g_payload->intensity, 0.0f, 2.0f);
        g_app.params.localToneStrength = std::clamp(g_payload->localTone, 0.0f, 2.0f);
        g_app.params.localStructureStrength = std::clamp(g_payload->localStructure, 0.0f, 2.0f);
        g_app.params.skinStructureStrength = std::clamp(g_payload->skinStructure, -1.0f, 2.0f);
        g_app.params.useAutoMask = g_payload->useAutoMask != 0;
        g_app.params.uiCorrection = g_payload->uiCorrection != 0;
        g_app.params.inputResolutionPercent = std::clamp(g_payload->inputResolution, 25, 100);
        g_app.params.scalingEnabled = g_payload->scalingEnabled != 0;
        g_app.params.residualMultiplier = std::clamp(g_payload->residualMultiplier, 1.0f, 2.0f);
    }
    return true;
}

void WritePayload(int saveRequest = 0, int resetRequest = 0) noexcept {
    if (!g_payload) return;
    static uint32_t seq = 0;
    const DlssnrParams &p = g_app.params;
    PanelPayload pl{};
    pl.magic = PAYLOAD_MAGIC;
    pl.seq = ++seq;
    pl.generation = g_generation;
    pl.preset = p.preset;
    pl.style = p.style;
    pl.intensity = p.intensity;
    pl.localTone = p.localToneStrength;
    pl.localStructure = p.localStructureStrength;
    pl.skinStructure = p.skinStructureStrength;
    pl.useAutoMask = p.useAutoMask ? 1 : 0;
    pl.uiCorrection = p.uiCorrection ? 1 : 0;
    pl.inputResolution = p.inputResolutionPercent;
    pl.scalingEnabled = p.scalingEnabled ? 1 : 0;
    pl.residualMultiplier = p.residualMultiplier;
    pl.saveRequest = saveRequest;
    pl.resetRequest = resetRequest;
    pl.logEnabled = g_app.timingLog ? 1 : 0;
    memcpy(g_payload, &pl, sizeof(pl));
}

void WriteIni() noexcept {
    wchar_t base[MAX_PATH];
    if (!BasePath(base, MAX_PATH)) return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\%s", base, INI_FILE);
    const DlssnrParams &p = g_app.params;
    wchar_t buf[32];
    auto writeInt = [&](const wchar_t *key, int v) {
        swprintf_s(buf, L"%d", v);
        WritePrivateProfileStringW(L"dlssnr", key, buf, path);
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
    writeInt(L"scaling_enabled", p.scalingEnabled ? 1 : 0);
    writeInt(L"residual_multiplier_x100", static_cast<int>(std::lround(p.residualMultiplier * 100.0f)));
    writeInt(L"saved", 1);
}

void LoadIni() noexcept {
    wchar_t base[MAX_PATH];
    if (!BasePath(base, MAX_PATH)) return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\%s", base, INI_FILE);
    // panel-local setting: perf log toggle (independent of the saved profile)
    g_app.timingLog = GetPrivateProfileIntW(L"panel", L"log", 1, path) != 0;
    const auto readInt = [&](const wchar_t *key, int def) -> int {
        return static_cast<int>(GetPrivateProfileIntW(L"dlssnr", key, def, path));
    };
    wchar_t marker[16]{};
    GetPrivateProfileStringW(L"dlssnr", L"saved", L"", marker, 16, path);
    if (!marker[0]) return; // no saved profile
    DlssnrParams &p = g_app.params;
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
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

// Tooltip with automatic line wrap (long Chinese hints would clip otherwise)
void ShowTip(const char *u8tip) noexcept {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 16.0f);
    ImGui::TextUnformatted(u8tip);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void DrawUi() noexcept {
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##panel", nullptr, flags);

    // 绝对定位布局:全部坐标手动计算(96dpi 基准 × uiScale),
    // 窗口高度在末尾按内容实际 y 收口,避免固定尺寸溢出。
    const float s = g_app.uiScale;
    const float th = 38 * s;
    const float marginX = 18 * s;
    const float colCtrl = 140 * s;      // 控件列起点
    const ImVec2 wpos = ImGui::GetWindowPos();
    const ImVec2 wsize = ImGui::GetWindowSize();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // --- 自绘标题栏(WS_POPUP;拖动由 WM_NCHITTEST HTCAPTION 处理) ---
    dl->AddRectFilled(wpos, ImVec2(wpos.x + wsize.x, wpos.y + th), IM_COL32(30, 32, 42, 255));
    dl->AddLine(ImVec2(wpos.x, wpos.y + th), ImVec2(wpos.x + wsize.x, wpos.y + th), IM_COL32(58, 62, 78, 255));

    // 唯一的标题栏按钮:✕(隐藏到托盘;真正退出在托盘右键菜单)。
    // 线条自绘图标(✕ 字符不在中文字体 glyph 范围,会显示问号)。
    const float bs = 30 * s, bpad = 10 * s;
    const float bxClose = wsize.x - bs - bpad;
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + bxClose, wpos.y + (th - bs) * 0.5f));
    if (ImGui::Button("##close", ImVec2(bs, bs))) {
        ShowWindow(g_hwnd, SW_HIDE);
    }
    {
        const bool hov = ImGui::IsItemHovered();
        const ImU32 col = hov ? IM_COL32(240, 130, 130, 255) : IM_COL32(154, 160, 176, 255);
        const ImVec2 c = ImGui::GetItemRectMin();
        const ImVec2 c2 = ImGui::GetItemRectMax();
        const float cy = (c.y + c2.y) * 0.5f, cx = (c.x + c2.x) * 0.5f;
        dl->AddLine(ImVec2(cx - 5 * s, cy - 5 * s), ImVec2(cx + 5 * s, cy + 5 * s), col, 2.0f * s);
        dl->AddLine(ImVec2(cx - 5 * s, cy + 5 * s), ImVec2(cx + 5 * s, cy - 5 * s), col, 2.0f * s);
        if (hov) {
            char u8tip[128];
            WideCharToMultiByte(CP_UTF8, 0, L"隐藏到托盘(退出在托盘右键菜单)", -1, u8tip, sizeof(u8tip), nullptr, nullptr);
            ShowTip(u8tip);
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(wpos.x + 16 * s, wpos.y + (th - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextUnformatted("DLSSNR 控制面板");

    // --- 内容 ---
    float y = th + 12 * s;
    char u8[64], u8tip[256];

    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));
    ImGui::TextDisabled("参数改动在下一帧生效");
    y += 24 * s;
    dl->AddLine(ImVec2(wpos.x + marginX, wpos.y + y), ImVec2(wpos.x + wsize.x - marginX, wpos.y + y), IM_COL32(58, 62, 78, 255));
    y += 14 * s;

    for (const auto &e : kEnums) {
        WideCharToMultiByte(CP_UTF8, 0, e.label, -1, u8, sizeof(u8), nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, e.tip, -1, u8tip, sizeof(u8tip), nullptr, nullptr);
        ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y + 7 * s));
        ImGui::Text("%s", u8);
        if (ImGui::IsItemHovered()) ShowTip(u8tip);
        ImGui::SetCursorScreenPos(ImVec2(wpos.x + colCtrl, wpos.y + y));
        ImGui::SetNextItemWidth(wsize.x - colCtrl - marginX);
        int v = (e.key == std::string("preset")) ? g_app.params.preset : g_app.params.style;
        const char *items[4] = {};
        char itemBuf[4][64];
        int count = 0;
        if (e.key == std::string("preset")) {
            for (int i = 0; i <= 3; ++i) {
                snprintf(itemBuf[i], sizeof(itemBuf[i]), "%d(%s)", i, i == 0 ? "默认" : "预设");
                items[i] = itemBuf[i];
                ++count;
            }
        } else {
            const char *names[3] = {"0(默认)", "1(自然)", "2(电影)"};
            for (int i = 0; i <= 2; ++i) {
                snprintf(itemBuf[i], sizeof(itemBuf[i]), "%s", names[i]);
                items[i] = itemBuf[i];
                ++count;
            }
        }
        if (ImGui::Combo(("##" + std::string(e.key)).c_str(), &v, items, count)) {
            if (e.key == std::string("preset")) g_app.params.preset = v;
            else g_app.params.style = v;
            g_app.liveDirty = true;
        }
        y += 38 * s;
    }

    for (const auto &sl : kSliders) {
        WideCharToMultiByte(CP_UTF8, 0, sl.label, -1, u8, sizeof(u8), nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, sl.tip, -1, u8tip, sizeof(u8tip), nullptr, nullptr);
        ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y + 8 * s));
        ImGui::Text("%s", u8);
        if (ImGui::IsItemHovered()) ShowTip(u8tip);
        ImGui::SetCursorScreenPos(ImVec2(wpos.x + colCtrl, wpos.y + y));
        ImGui::SetNextItemWidth(wsize.x - colCtrl - marginX);
        float v = sl.key == std::string("intensity")         ? g_app.params.intensity
                  : sl.key == std::string("local_tone")      ? g_app.params.localToneStrength
                  : sl.key == std::string("local_structure") ? g_app.params.localStructureStrength
                                                             : g_app.params.skinStructureStrength;
        if (ImGui::SliderFloat(("##" + std::string(sl.key)).c_str(), &v, sl.lo, sl.hi, "%.2f")) {
            // NGX accepts continuous float steps (verified: 0.01 steps produce
            // distinct outputs), so no snapping to Magpie's UI-level 0.05 grid.
            if (sl.key == std::string("intensity")) g_app.params.intensity = v;
            else if (sl.key == std::string("local_tone")) g_app.params.localToneStrength = v;
            else if (sl.key == std::string("local_structure")) g_app.params.localStructureStrength = v;
            else g_app.params.skinStructureStrength = v;
            g_app.liveDirty = true;
        }
        y += 38 * s;
    }

    dl->AddLine(ImVec2(wpos.x + marginX, wpos.y + y), ImVec2(wpos.x + wsize.x - marginX, wpos.y + y), IM_COL32(58, 62, 78, 255));
    y += 14 * s;

    for (const auto &f : kFlags) {
        WideCharToMultiByte(CP_UTF8, 0, f.label, -1, u8, sizeof(u8), nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, f.tip, -1, u8tip, sizeof(u8tip), nullptr, nullptr);
        ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));
        bool v = f.key == std::string("use_auto_mask") ? g_app.params.useAutoMask : g_app.params.uiCorrection;
        if (ImGui::Checkbox(u8, &v)) {
            if (f.key == std::string("use_auto_mask")) g_app.params.useAutoMask = v;
            else g_app.params.uiCorrection = v;
            g_app.liveDirty = true;
        }
        if (ImGui::IsItemHovered()) ShowTip(u8tip);
        y += 26 * s;
    }

    y += 8 * s;
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));
    if (ImGui::Button("保存设置", ImVec2(120 * s, 30 * s))) {
        WriteIni();
        WritePayload(1, 0); // persist-through-bridge flag included
        snprintf(g_app.status, sizeof(g_app.status), "已保存: %ls", INI_FILE);
    }
    if (ImGui::IsItemHovered()) {
        char u8tip[192];
        WideCharToMultiByte(CP_UTF8, 0, L"将当前设置保存为默认值(dlssnr_ui.ini),下次加载滤镜时自动生效。", -1, u8tip, sizeof(u8tip), nullptr, nullptr);
        ShowTip(u8tip);
    }
    ImGui::SameLine(0, 14 * s);
    if (ImGui::Button("重置默认", ImVec2(120 * s, 30 * s))) {
        g_app.params = DlssnrParams{};
        WritePayload(0, 1);
        snprintf(g_app.status, sizeof(g_app.status), "已重置");
    }
    ImGui::SameLine(0, 16 * s);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6 * s);
    if (g_app.status[0]) ImGui::TextUnformatted(g_app.status);
    y += 38 * s;

    // 高度收口:按内容实际底部调整窗口(WS_POPUP 尺寸 = client 尺寸)
    const float needH = y + 10 * s;
    RECT wrc{};
    GetWindowRect(g_hwnd, &wrc);
    if (std::abs((wrc.bottom - wrc.top) - static_cast<int>(needH)) > 1) {
        SetWindowPos(g_hwnd, nullptr, 0, 0, wrc.right - wrc.left,
                     static_cast<int>(needH), SWP_NOZORDER | SWP_NOMOVE);
    }

    ImGui::End();
}

} // namespace

// ---------------------------------------------------------------------------
// Win32 + D3D11 + ImGui bootstrap
// ---------------------------------------------------------------------------

namespace {

void RebuildFontDpi(int dpi) noexcept {
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    char fontPath[MAX_PATH];
    GetWindowsDirectoryA(fontPath, MAX_PATH);
    strcat_s(fontPath, "\\Fonts\\msyh.ttc");
    static const ImWchar ranges[] = {
        0x0020, 0x00FF, 0x2000, 0x206F, 0x3000, 0x30FF, 0xFF00, 0xFFEF,
        0x4E00, 0x9FAF, 0,
    };
    io.Fonts->AddFontFromFileTTF(fontPath, 15.0f * dpi / 96.0f, nullptr, ranges);
    io.Fonts->Build();
    ImGui_ImplDX11_InvalidateDeviceObjects();
}

ID3D11Device *g_device = nullptr;
ID3D11DeviceContext *g_context = nullptr;
IDXGISwapChain *g_swap = nullptr;
ID3D11RenderTargetView *g_rtv = nullptr;
bool g_quit = false;
NOTIFYICONDATAW g_nid{};

void CreateRenderTarget() noexcept {
    ID3D11Texture2D *back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void DropRenderTarget() noexcept {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return 1;
    switch (msg) {
    case WM_SIZE:
        if (g_device && wParam != SIZE_MINIMIZED) {
            DropRenderTarget();
            g_swap->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_DPICHANGED: {
        g_app.dpi = HIWORD(wParam);
        g_app.uiScale = g_app.dpi / 96.0f;
        RebuildFontDpi(g_app.dpi);
        // Width re-scales with DPI; height self-fits to content next frame
        const auto *sug = reinterpret_cast<const RECT *>(lParam);
        RECT wrc{};
        GetWindowRect(hwnd, &wrc);
        const int wantW = static_cast<int>(460 * g_app.uiScale);
        SetWindowPos(hwnd, nullptr, sug->left, sug->top,
                     wantW, wrc.bottom - wrc.top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_NCHITTEST: {
        // Borderless drag: title bar (except the button zone, same formula as
        // DrawUi) behaves as a caption; the system move loop is smoother than
        // manual SetWindowPos.
        POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        ScreenToClient(hwnd, &pt);
        const float s = g_app.uiScale;
        const float th = 38 * s;
        if (pt.y >= 0 && pt.y <= th) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const float bxClose = rc.right - 30 * s - 10 * s;
            if (pt.x >= bxClose - 10 * s) break; // button zone -> HTCLIENT
            return HTCAPTION;
        }
        break;
    }
    case WM_CLOSE:
        // hide to tray; real exit lives in the tray menu
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_APP_TRAYICON:
        if (LOWORD(lParam) == WM_LBUTTONUP) {
            const BOOL showNow = IsWindowVisible(hwnd) ? SW_HIDE : SW_SHOW;
            ShowWindow(hwnd, showNow);
            if (showNow != SW_HIDE) SetForegroundWindow(hwnd);
        } else if (LOWORD(lParam) == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"显示面板");
            AppendMenuW(menu, MF_STRING, 2, L"退出");
            SetForegroundWindow(hwnd);
            const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == 1) { ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); }
            if (cmd == 2) { PostThreadMessageW(g_mainThreadId, WM_QUIT, 0, 0); }
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

// ---------------------------------------------------------------------------
// Lifetime watchdog: if this panel was launched by the plugin (bridge alive
// event appears within the grace window), follow the filter lifetime - when
// the event disappears (filter off / mpv killed) the panel exits and the tray
// icon goes away. A manually started panel that never sees the event stays
// resident until the user exits it from the tray menu.
// ---------------------------------------------------------------------------

namespace {

DWORD WINAPI ExitWatchProc(LPVOID) noexcept {
    bool following = false;
    for (int i = 0; i < 60 && !g_quit; ++i) { // 30s grace window
        if (const HANDLE h = OpenEventW(SYNCHRONIZE, FALSE, ALIVE_EVENT)) {
            CloseHandle(h);
            following = true;
            break;
        }
        Sleep(500);
    }
    if (!following) return 0;
    while (!g_quit) {
        Sleep(500);
        if (g_quit) break;
        if (const HANDLE h = OpenEventW(SYNCHRONIZE, FALSE, ALIVE_EVENT)) {
            CloseHandle(h);
        } else {
            PostThreadMessageW(g_mainThreadId, WM_QUIT, 0, 0);
            break;
        }
    }
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_mainThreadId = GetCurrentThreadId();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Single instance: repeated launches (e.g. filter reload auto-start) are
    // silent no-ops; the user opens the panel via the panel's own tray icon.
    CreateMutexW(nullptr, TRUE, L"vs_dlssnr_panel_single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = WINDOW_CLASS;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    wchar_t base[MAX_PATH];
    BasePath(base, MAX_PATH);

    // Tray-first start: window hidden until tray click (per original request:
    // "滤镜加载后任务栏图标,点击后开启控制面板")
    g_hwnd = CreateWindowExW(0, WINDOW_CLASS, WINDOW_TITLE,
                             WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 460, 450,
                             nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    // Windows 11 rounded corners for the borderless window
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));

    // Width must scale with DPI too (height self-fits to content per frame).
    g_app.dpi = GetDpiForWindow(g_hwnd);
    g_app.uiScale = g_app.dpi / 96.0f;
    if (g_app.dpi != 96) {
        SetWindowPos(g_hwnd, nullptr, 0, 0, static_cast<int>(460 * g_app.uiScale),
                     static_cast<int>(450 * g_app.uiScale), SWP_NOZORDER | SWP_NOMOVE);
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.OutputWindow = g_hwnd;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                             nullptr, 0, D3D11_SDK_VERSION, &scd,
                                             &g_swap, &g_device, &fl, &g_context))) {
        return 1;
    }
    CreateRenderTarget();
    // Consume the first ShowWindow call: Win32 applies STARTUPINFO.wShowWindow
    // (SW_HIDE from ShellExecute) to it and ignores the argument, so a later
    // tray-click SW_SHOW would silently hide again otherwise.
    ShowWindow(g_hwnd, SW_HIDE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; // no imgui.ini clutter
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 5.0f;
    style.GrabRounding = 4.0f;
    style.WindowPadding = ImVec2(18, 16);
    style.FramePadding = ImVec2(8, 6);
    style.ItemSpacing = ImVec2(10, 9);

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    RebuildFontDpi(g_app.dpi);

    // Tray icon
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAYICON;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, TRAY_TIP);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    LoadIni();
    CreateParamsMapping(); // adopts previous session's live params if present
    WritePayload();        // announce current values to the plugin

    HANDLE watch = CreateThread(nullptr, 0, ExitWatchProc, nullptr, 0, nullptr);

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    MSG msg;
    while (!g_quit) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quit) break;

        // Hidden-to-tray: stop rendering, wake on messages only
        if (!IsWindowVisible(g_hwnd)) {
            WaitMessage();
            continue;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        // Absolute QPC time (a per-frame delta would always be < the 100ms
        // write throttle, which permanently blocked live writes)
        const double nowSec = now.QuadPart / double(freq.QuadPart);

        // Throttled shared-memory pushes while dragging sliders
        if (g_app.liveDirty && nowSec - g_app.lastLiveWrite > 0.1) {
            WritePayload();
            g_app.liveDirty = false;
            g_app.lastLiveWrite = nowSec;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUi();
        ImGui::Render();

        const float clear[4] = { 0.09f, 0.10f, 0.13f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    if (watch) CloseHandle(watch);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DropRenderTarget();
    if (g_swap) g_swap->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    return 0;
}
