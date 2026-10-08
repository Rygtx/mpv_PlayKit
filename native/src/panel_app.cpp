// dlssnr_panel - independent control panel for the vs_dlssnr VapourSynth
// plugin. ImGui + D3D11 rendered UI (no native controls). Pushes the full
// parameter set into a named shared-memory mapping (panel_ipc.h; the plugin's
// bridge polls it) for live tuning, and dlssnr_ui.ini for saved defaults.

#include "dlssnr_params.h"
#include "panel_ipc.h"
#include "dlssnr_ini.h"
#include "fonts.h"

#include "panel_shared.h"
#include "resource.h"

using namespace vsdlssnr;

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// 进程退出旗标:主循环(WM_QUIT)、看门狗与 HDR 打标线程轮询同一位。
// 定义在匿名 namespace 之前 —— HdrTagProc 在文件前部,须能看到它。
// atomic(2026-10-04:裸 bool 跨线程轮询是数据竞争;bridge 侧同场景用 volatile)。
std::atomic<bool> g_quit{ false };

constexpr wchar_t WINDOW_CLASS[] = L"vs_dlssnr_panel_app";
constexpr wchar_t WINDOW_TITLE[] = L"DLSSNR 控制面板";
constexpr wchar_t TRAY_TIP[] = L"DLSSNR 控制面板";
// INI_FILE / ALIVE_EVENT come from panel_ipc.h (cross-process contract names)
constexpr UINT WM_APP_TRAYICON = WM_APP + 1;


AppState g_app;
DWORD g_mainThreadId = 0;
HWND g_hwnd = nullptr;
ImFont *g_fontUI = nullptr;
ImFont *g_fontMono = nullptr;

// ---------------------------------------------------------------------------
// paths + persistence
// ---------------------------------------------------------------------------

bool BasePath(wchar_t *path, size_t len) noexcept {
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    std::wstring dir = std::filesystem::path(exe).parent_path().wstring();
    // 上限预留 64 字符给消费方拼接("\dlssnr_panel.log"、
    // "\..\portable_config\mpv.conf" 等全部落 wchar_t[MAX_PATH])—— 深安装
    // 目录拼出 >260 时 swprintf_s 触发 invalid-parameter handler = 进程
    // abort,且恰死在报错路上(2026-10-04 评审修:超限 = 优雅降级不记日志)。
    if (dir.size() + 64 >= len) return false;
    wcscpy_s(path, len, dir.c_str());
    return true;
}

// 面板侧事件日志(dlssnr_panel.log,独立于插件的 dlssnr_timing.log ——
// 两个进程写同一文件会交错)。只记低频生命周期事件:#39 的面板死亡螺旋
// 当时只能靠外部 5ms 轮询脚本取证,这里补上原生记录。每次写入 open/close
// (事件频率为个位数/会话,不值得常驻句柄)。
void PanelLog(const char *fmt, ...) noexcept {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamped[560];
    std::snprintf(stamped, sizeof(stamped), "[%02u:%02u:%02u.%03u] %s\n",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
    static std::mutex m; // 主线程 + 看门狗线程两个写者
    std::lock_guard<std::mutex> lock(m);
    wchar_t base[MAX_PATH];
    if (!BasePath(base, MAX_PATH)) return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\dlssnr_panel.log", base);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"a") != 0 || !f) return;
    fwrite(stamped, 1, strlen(stamped), f);
    fclose(f);
}

// ---- dlssg_for_sm86 代理 ini(ngx\dlssg_sm86.ini)的 Optimized 单键 ----
// 存储单点 = 代理 ini 本身(代理只在进程加载时读一次,重启 mpv 生效);
// 面板不做 ui.ini 双写,启动时回读 ini 为准。写 = 行内原位替换,文件其余
// 字节(上游注释/用户排查段)逐字节保留 —— 不走 WritePrivateProfile*,
// 其重写整文件会把 UTF-8 无 BOM 的中文注释按 ANSI 碾碎。
bool FgProxyIniPath(wchar_t *path, size_t len) noexcept {
    wchar_t base[MAX_PATH];
    if (!BasePath(base, MAX_PATH)) return false;
    swprintf_s(path, len, L"%s\\ngx\\dlssg_sm86.ini", base);
    return true;
}

int ReadFgOptimizedIni() noexcept {
    wchar_t path[MAX_PATH];
    if (!FgProxyIniPath(path, MAX_PATH)) return 1;
    std::ifstream in(path, std::ios::binary);
    if (!in) return 1; // 代理未部署:显示默认,写入时同样跳过
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    size_t pos = 0;
    while (pos < content.size()) {
        const size_t eol = content.find('\n', pos);
        const size_t stop = (eol == std::string::npos) ? content.size() : eol;
        size_t len = stop - pos;
        if (len && content[pos + len - 1] == '\r') --len;
        const size_t s = content.find_first_not_of(" \t", pos);
        if (s != std::string::npos && s < pos + len &&
            content.compare(s, 9, "Optimized") == 0) {
            const bool boundary = (s + 9 == pos + len) || content[s + 9] == '=' ||
                                  content[s + 9] == ' ' || content[s + 9] == '\t';
            const size_t eq = boundary ? content.find('=', s + 9) : std::string::npos;
            if (eq != std::string::npos && eq < pos + len) {
                return std::clamp(std::atoi(content.c_str() + eq + 1), 0, 3);
            }
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return 1;
}

bool WriteFgProxyIniKey(const char *key, int v) noexcept {
    wchar_t path[MAX_PATH];
    if (!FgProxyIniPath(path, MAX_PATH)) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        PanelLog("panel: fg proxy ini missing, %s not written", key);
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();
    in.close();
    const size_t klen = strlen(key);
    size_t pos = 0;
    while (pos < content.size()) {
        const size_t eol = content.find('\n', pos);
        const size_t stop = (eol == std::string::npos) ? content.size() : eol;
        size_t len = stop - pos;
        if (len && content[pos + len - 1] == '\r') --len;
        const size_t s = content.find_first_not_of(" \t", pos);
        if (s != std::string::npos && s < pos + len &&
            content.compare(s, klen, key) == 0) {
            const bool boundary = (s + klen == pos + len) || content[s + klen] == '=' ||
                                  content[s + klen] == ' ' || content[s + klen] == '\t';
            const size_t eq = boundary ? content.find('=', s + klen) : std::string::npos;
            if (eq != std::string::npos && eq < pos + len) {
                content.replace(eq + 1, pos + len - (eq + 1), std::to_string(v));
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                out << content;
                if (!out) {
                    PanelLog("panel: fg proxy ini write FAILED (locked?)");
                    return false;
                }
                return true;
            }
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    PanelLog("panel: fg proxy ini has no %s key; not written", key);
    return false;
}

bool WriteFgOptimizedIni(int v) noexcept {
    return WriteFgProxyIniKey("Optimized", v);
}

// Shared-memory parameter channel: the panel creates the mapping and pushes
// the full parameter set on every edit (seq-gated); the plugin waits on the
// named auto-reset event (with the 40ms poll as fallback) and applies it.
HANDLE g_paramsMapping = nullptr;
PanelPayload *g_payload = nullptr;
HANDLE g_paramsEvent = nullptr; // opened lazily; exists once the filter loads
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
    // over the ini) via the shared field mapping (panel_ipc.h).
    // 直读无 seq 复验的前提(单写者不变量):面板单实例 mutex ⇒ 参数映射
    // 恒单写者,且 adopt 时写者(上一实例)已死 —— 改互斥语义时此处是
    // 第一个要重新审视的点。
    if (g_payload->magic == PAYLOAD_MAGIC && g_payload->seq > 0) {
        LoadLiveParams(g_app.params, *g_payload);
        LoadCreateParams(g_app.params, *g_payload);
        // 探针比对基准取上一实例的已发布值:重启后 worker 读到同值不触发
        // 多余 reseek,读到不同值(文件已换)才走主循环探针分支。
        g_app.lastSrcProbe = g_payload->srcHdr;
    }
    return true;
}

void WritePayload() noexcept {
    if (!g_payload) return;
    static uint32_t seq = 0;
    const uint32_t newSeq = ++seq;
    PanelPayload pl = PayloadFromParams(g_app.params); // shared field mapping; seq stays 0
    pl.generation = g_generation;
    pl.logEnabled = g_app.timingLog ? 1 : 0;
    // 源传输函数探针(v30):worker 代读 mpv gamma,非用户参数,逐次写入
    // (-1 未读到 = 0 保守)。
    pl.srcHdr = g_srcGammaProbe.load(std::memory_order_relaxed) > 0 ? 1 : 0;
    // Publish protocol shared with the plugin's stats channel (panel_ipc.h):
    // body lands with seq 0, the counter moves alone after it is stable.
    PublishWithSeq(&g_payload->seq, newSeq, [&] {
        memcpy(g_payload, &pl, sizeof(pl));
    });
    // Wake the bridge so the edit lands immediately; opening the event is
    // lazy because the plugin creates it when the filter loads, which can
    // happen long after the panel started. Failure (old plugin absent) just
    // leaves the bridge on its fallback poll.
    if (!g_paramsEvent) {
        g_paramsEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, PARAMS_EVENT);
    }
    if (g_paramsEvent) SetEvent(g_paramsEvent);
}


bool WriteIniNow() noexcept {
    wchar_t base[MAX_PATH];
    if (!BasePath(base, MAX_PATH)) return false;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\%s", base, INI_FILE);
    if (!WriteDlssnrIni(g_app.params, path)) { // shared key list (dlssnr_ini.h)
        // 失败必须可被调用方感知:曾经失败只进本日志,按钮回调无条件报
        // "已保存" —— ini 被占用时用户看到已保存,重启全部回旧值(面板
        // 有 status 状态栏机制,这里接上)。
        PanelLog("panel: save ini FAILED (file locked?)");
        return false;
    }
    return true;
}

void LoadIni() noexcept {
    wchar_t base[MAX_PATH];
    if (!BasePath(base, MAX_PATH)) return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\%s", base, INI_FILE);
    // panel-local settings: perf log toggle + advanced-section state
    // (independent of the saved profile)
    g_app.timingLog = GetPrivateProfileIntW(L"panel", L"log", 1, path) != 0;
    g_app.advancedOpen = GetPrivateProfileIntW(L"panel", L"advanced", 0, path) != 0;
    // 页签:0=神经渲染 1=帧生成 2=RTX 超分/HDR 3=诊断
    g_app.page = static_cast<int>(std::clamp(GetPrivateProfileIntW(L"panel", L"page", 0, path), 0u, 3u));
    // 旧版 fg_route 越界值(v20 语义作废的 2/3 钉档)被 clamp 重解释:
    // 状态行提示,不再无声 —— 老用户"补帧怎么没了"的直接答案。
    {
        int legacyRoute = 0;
        // 启动自检:剪除表外键(跨版本回滚残留,见 PruneDlssnrIni);
        // 明细仅在无更重要提示时占用状态行。
        wchar_t pruned[256] = L"";
        const int prunedN = PruneDlssnrIni(path, pruned, 256);
        LoadDlssnrIni(g_app.params, path, &legacyRoute);
        if (legacyRoute > kFgRouteMax) {
            snprintf(g_app.status, sizeof(g_app.status),
                     "检测到旧版 fg_route=%d(档位语义已作废),已按新版解释为纯官方;"
                     "如需自动档请在面板选回 \"自动\"",
                     legacyRoute);
        } else if (prunedN > 0) {
            snprintf(g_app.status, sizeof(g_app.status),
                     "清理了 %d 个已作废的保存配置键:%ls", prunedN, pruned);
        }
    }
    // Optimized 档位的存储单点 = 代理 ini(代理只在进程加载时读一次),
    // 面板启动回读为准 —— 不落 ui.ini,避免双存储漂移。
    g_app.fgOptimized = ReadFgOptimizedIni();
}


// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Win32 + D3D11 + ImGui bootstrap
// ---------------------------------------------------------------------------

namespace {

// 样式间距随 UI 缩放(基值 × uiScale,幂等,DPI 变更后重应用)。不缩放的话
// WindowPadding 固定 96dpi 值,高 DPI 下与手动定位的 marginX(18*s)分道扬镳
// —— stats 首行(GPU 行)的缩进就是这么来的。
void ApplyUiScale() noexcept {
    if (!ImGui::GetCurrentContext()) return;
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(18 * g_app.uiScale, 14 * g_app.uiScale);
    style.FramePadding = ImVec2(8 * g_app.uiScale, 5 * g_app.uiScale);
    style.ItemSpacing = ImVec2(10 * g_app.uiScale, 5 * g_app.uiScale);
}

void RebuildFontDpi(int dpi) noexcept {
    if (!ImGui::GetCurrentContext()) return; // WM_DPICHANGED can precede CreateContext
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    g_fontUI = g_fontMono = nullptr;
    // Magpie 三字体架构:Segoe UI 主字 + msyh(YaHei UI)中文 merge + 数字等宽
    if (!vsdlssnr::fonts::BuildFonts(dpi / 96.0f * kUiFontScale, &g_fontUI, &g_fontMono)) {
        g_fontUI = g_fontMono = nullptr;
        OutputDebugStringA("vs_dlssnr panel: BuildFonts failed\n");
    }
    ImGui_ImplDX11_InvalidateDeviceObjects();
}

ID3D11Device *g_device = nullptr;
ID3D11DeviceContext *g_context = nullptr;
IDXGISwapChain *g_swap = nullptr;
ID3D11RenderTargetView *g_rtv = nullptr;
NOTIFYICONDATAW g_nid{};
UINT g_msgTaskbarCreated = 0; // explorer 重启广播(TaskbarCreated),托盘重挂用
bool g_trayUp = false;        // NIM_ADD 成功后置位;托盘重挂的门

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
    if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {
        // explorer 重启:托盘图标随 shell 重建而销毁,必须重挂(2026-10-05
        // 评审修 —— 此前只启动 NIM_ADD 一次,explorer 崩溃后面板进程永久
        // 不可达;托盘是隐藏窗口唯一入口)。
        if (g_trayUp) Shell_NotifyIconW(NIM_ADD, &g_nid);
        return 0;
    }
    switch (msg) {
    case WM_SIZE:
        if (g_device && wParam != SIZE_MINIMIZED) {
            DropRenderTarget();
            // On failure (e.g. device removal) there is no back buffer to
            // bind; skip re-creating the RTV instead of leaving a null one
            // for the render loop to dereference.
            if (SUCCEEDED(g_swap->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0))) {
                CreateRenderTarget();
            }
        }
        return 0;
    case WM_DPICHANGED: {
        g_app.dpi = HIWORD(wParam);
        g_app.uiScale = g_app.dpi / 96.0f * kUiFontScale;
        ApplyUiScale();
        RebuildFontDpi(g_app.dpi);
        // Width re-scales with DPI; height self-fits to content next frame
        const auto *sug = reinterpret_cast<const RECT *>(lParam);
        RECT wrc{};
        GetWindowRect(hwnd, &wrc);
        const int wantW = static_cast<int>(kPanelW * g_app.uiScale);
        SetWindowPos(hwnd, nullptr, sug->left, sug->top,
                     wantW, wrc.bottom - wrc.top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_NCHITTEST: {
        // Borderless drag: title bar (except the button zone, same constants
        // as DrawUi) behaves as a caption; the system move loop is smoother
        // than manual SetWindowPos.
        POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        ScreenToClient(hwnd, &pt);
        const float s = g_app.uiScale;
        const float th = kTitleBarH * s;
        if (pt.y >= 0 && pt.y <= th) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const float bxClose = rc.right - kCloseBtnSize * s - kCloseBtnPad * s;
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
            // KB135788:TrackPopupMenu 前的 SetForegroundWindow 要求菜单关闭
            // 后补一条无害消息归还前台,否则下一次弹出被立即吞掉(需点两次)。
            PostMessageW(hwnd, WM_NULL, 0, 0);
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
    if (!following) {
        PanelLog("panel: no filter seen in 30s; staying resident (manual mode)");
        return 0;
    }
    PanelLog("panel: watchdog following filter (alive event present)");
    while (!g_quit) {
        Sleep(500);
        if (g_quit) break;
        if (const HANDLE h = OpenEventW(SYNCHRONIZE, FALSE, ALIVE_EVENT)) {
            CloseHandle(h);
        } else {
            // 探针:看门狗退出(#39 死亡螺旋的计数锚点 —— 该行高频出现
            // = alive 事件在反复消失,直接指向插件侧 BridgeStop/生命周期)。
            PanelLog("panel: watchdog alive event LOST -> exit");
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
    // 退避重试(2026-09-25):mpv 快速重启时,bridge 立即拉起新面板,可能撞上
    // 尚在退出的旧实例的 mutex(看门狗 500ms 周期内)—— 新实例直接退出、旧
    // 实例随后也退出,之后直到下一次 seek 都没有面板。mutex 随持有者进程死
    // 而释放,短退避重试 3 次即覆盖该窗口。
    bool owned = false;
    for (int attempt = 0; attempt < 3 && !owned; ++attempt) {
        if (attempt) Sleep(500);
        // 句柄 NULL(同名恶性对象 ACCESS_DENIED 等)= 守卫状态不可知:按
        // 未持有处理(fail-closed,走下方退出)。放行会在参数映射上双写,
        // 违反单写者契约 —— 单写者优先于可用性(2026-10-04 评审修)。
        owned = CreateMutexW(nullptr, TRUE, L"vs_dlssnr_panel_single") != nullptr &&
                GetLastError() != ERROR_ALREADY_EXISTS;
        if (!owned) {
            PanelLog("panel: duplicate launch (attempt %d), retrying briefly", attempt + 1);
        }
    }
    if (!owned) {
        PanelLog("panel: single-instance guard held by a live panel; exiting");
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = WINDOW_CLASS;
    // 自家图标(资源 IDI_PANEL_ICON):托盘、任务栏与资源管理器共用。
    // hIcon 取 256px 母版由系统按需缩小(高 DPI 任务栏/Alt-Tab 不糊);
    // hIconSm 取 16px(标题栏)。
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_PANEL_ICON),
                                             IMAGE_ICON, 256, 256, LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_PANEL_ICON),
                                               IMAGE_ICON, 16, 16, LR_SHARED));
    RegisterClassExW(&wc);

    wchar_t base[MAX_PATH];
    BasePath(base, MAX_PATH);

    // Tray-first start: window hidden until tray click (per original request:
    // "滤镜加载后任务栏图标,点击后开启控制面板")
    g_hwnd = CreateWindowExW(0, WINDOW_CLASS, WINDOW_TITLE,
                             WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT,
                             static_cast<int>(kPanelW), 450,
                             nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    // Windows 11 rounded corners for the borderless window
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));

    // Width = kPanelW × uiScale at every DPI (96dpi 含 0.8 字号折减 → 499px;
    // 双列参数区恰好占满)。无条件应用:内容几何按 s 单位铺满窗宽,窗口
    // 不缩放会右侧留死区。Height self-fits to content per frame.
    g_app.dpi = GetDpiForWindow(g_hwnd);
    g_app.uiScale = g_app.dpi / 96.0f * kUiFontScale;
    SetWindowPos(g_hwnd, nullptr, 0, 0, static_cast<int>(kPanelW * g_app.uiScale),
                 static_cast<int>(450 * g_app.uiScale), SWP_NOZORDER | SWP_NOMOVE);

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
        PanelLog("panel: D3D11 device/swapchain create FAILED");
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
    ApplyUiScale(); // WindowPadding/FramePadding/ItemSpacing × uiScale(含 kUiFontScale)

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    RebuildFontDpi(g_app.dpi);

    // Tray icon
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // 托盘图标用 16px 变体(系统 DPI 缩放时由 Shell 按需缩放资源内其他尺寸);
    // 程序生命周期内常驻,句柄不销毁(LR_SHARED 归属进程,无需显式清理)。
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAYICON;
    {
        const UINT dpi = GetDpiForWindow(g_hwnd);
        const int traySize = MulDiv(16, static_cast<int>(dpi), 96); // 系统托盘标准 16px@96dpi
        g_nid.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_PANEL_ICON),
                                                    IMAGE_ICON, traySize, traySize, LR_SHARED));
    }
    wcscpy_s(g_nid.szTip, TRAY_TIP);
    // 托盘图标是隐藏窗口的唯一入口:创建失败 = 进程活着但面板永不可达,
    // 双击 exe 表现为"没反应"。必须给出可见失败,不能静默继续。
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        PanelLog("panel: Shell_NotifyIconW(NIM_ADD) FAILED (err %lu)",
                 static_cast<unsigned long>(GetLastError()));
        MessageBoxW(g_hwnd,
                    L"托盘图标创建失败,面板无法显示(Explorer 未运行或 shell 服务异常)。",
                    L"DLSSNR 控制面板", MB_OK | MB_ICONWARNING);
        return 1;
    }
    g_trayUp = true;

    LoadIni();
    const bool mappingOk = CreateParamsMapping(); // adopts previous session's live params if present
    // 探针:启动行(build 戳 + 上一会话遗留 payload 的 seq —— 面板 adopt
    // 了什么、以及"面板和插件版本是否成对"从此处一行可见)。adopt_seq 在
    // mapping 之后、首次 WritePayload 之前读,才是上一会话遗留的值。
    const uint32_t adoptSeq = (mappingOk && g_payload) ? g_payload->seq : 0;
    if (!mappingOk) PanelLog("panel: params mapping create FAILED");
    WritePayload();        // announce current values to the plugin
    PanelLog("panel: start build=\"%s %s\" gen=%lu adopt_seq=%u magic=0x%08X",
             __DATE__, __TIME__,
             static_cast<unsigned long>(g_generation), static_cast<unsigned>(adoptSeq),
             static_cast<unsigned>(PAYLOAD_MAGIC));

    HANDLE watch = CreateThread(nullptr, 0, ExitWatchProc, nullptr, 0, nullptr);
    // HDR 打标线程:独立于 UI 可见性(主循环隐藏即 WaitMessage 全休眠,
    // 打标曾因此只在面板可见时工作)。见 HdrTagProc 注释。
    HANDLE hdrTagThread = CreateThread(nullptr, 0, HdrTagProc, nullptr, 0, nullptr);

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    MSG msg;
    while (!g_quit) {
        bool activity = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            activity = true;
        }
        if (g_quit) break;

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        // Absolute QPC time (a per-frame delta would always be < the 100ms
        // write throttle, which permanently blocked live writes)
        const double nowSec = now.QuadPart / double(freq.QuadPart);

        // Throttled shared-memory pushes while dragging sliders
        if (g_app.liveDirty && nowSec - g_app.lastLiveWrite > 0.1) {
            // create-time 变动(vsrMode/FG 档位等)经 reseek 重建会话后面板
            // 会重启,但重启面板在启动时采纳最后一份 payload(CreateParams
            // Mapping 的 adopt 路径,含 rtx 三元组,后于 ini 生效)—— 切换
            // 值不落盘也能跨面板重启保持;ini 仍只由"保存设置"显式写入
            // (单写者 = 面板,bridge 侧写块已撤,2026-10-03)。
            WritePayload();
            g_app.liveDirty = false;
            g_app.lastLiveWrite = nowSec;
        }
        // reseek 独立消费(2026-09-25):此前挂在 liveDirty 节流分支内,而
        // "写入性能日志"复选框无条件清 liveDirty —— 窗口内先动创建参数再点
        // 日志开关会把待发 reseek 连坐丢失(创建参数已随 payload 发出但 mpv
        // 不重载,静默降级为"等手动 seek")。reseek 有自己的节流戳。
        if (g_app.reseekDirty && nowSec - g_app.lastReseekWrite > 0.1) {
            // reseek 前强制 flush 未落地的 live 变更(2026-10-04 评审修):
            // live 分支的 100ms 节流若恰好挡住本拍写入,reseek 会先触发 ——
            // 重建的新实例采纳旧 payload,随后落地的 live 值无人再消费,
            // 变更静默丢失且面板勾选态与实际不符。此处无视节流先写。
            if (g_app.liveDirty) {
                WritePayload();
                g_app.liveDirty = false;
                g_app.lastLiveWrite = nowSec;
            }
            g_app.reseekDirty = false;
            g_app.lastReseekWrite = nowSec;
            // 任意面板主动 reseek 都入闭环去抖戳:重建窗口内旧全关实例
            // 会因 live 参数(nr=1)边沿发布 kStateNrSeekInit,下方闭环
            // 若无此戳会对同一次开关重复 seek(幂等但多余)。
            g_app.lastSeekInitReseek = nowSec;
            // 失败不再无声:此前 IPC 不可达(mpv 未开 / input-ipc-server
            // 未启用)时变动退化为"等下次手动 seek",面板零提示。
            if (TriggerMpvReseek()) {
                snprintf(g_app.status, sizeof(g_app.status),
                         "已通知 mpv 原地重载滤镜会话");
            } else {
                snprintf(g_app.status, sizeof(g_app.status),
                         "mpv IPC 不可达 —— 该变动需手动 seek 后生效");
            }
        }

        // Throttled stats read (plugin publishes into the stats mapping every
        // frame); shared memory, zero disk IO. 100ms 读节流 + 100ms 空闲唤醒:
        // 处理用时 ~10Hz 呼吸;空闲时无重绘,唤醒成本 = 一次 512B 映射读。
        if (nowSec - g_app.lastStatsRead > 0.1) {
            g_app.lastStatsRead = nowSec;
            LoadStats();
        }
        // 源传输函数探针变化(worker 代读 gamma):折叠进 payload + 触发一次
        // reseek —— srcHdr 是创建时事实,必须经新脚本 create 采纳。换片后
        // 新 create 先吃到上一片探针值(首段 ≤0.5s 误向),本次 reseek 收敛;
        // 之后值稳定,不再触发。
        {
            const int probe = g_srcGammaProbe.load(std::memory_order_relaxed);
            if (probe >= 0 && probe != g_app.lastSrcProbe) {
                g_app.lastSrcProbe = probe;
                // reseek 分支会无视节流强制 flush live 变更,无需另设 liveDirty。
                g_app.reseekDirty = true;
            }
        }
        // 闭环兜底:DLL 报 kStateNrSeekInit = NR 已开但会话未初始化(在
        // 全关直通实例上开的 NR)。开关瞬间 stats 尚未到位等一切漏判路径
        // 由此补触发重建;2.5s 去抖防重建窗口内对旧 body 重复 seek。
        // 要求面板自身意图 nrEnabled:用户中途又关掉时旧 body 不得再触发。
        if (g_app.params.nrEnabled &&
            std::strcmp(g_app.snap.filterState, "passthrough") == 0 &&
            std::strcmp(g_app.snap.stateDetail, vsdlssnr::kStateNrSeekInit) == 0 &&
            nowSec - g_app.lastSeekInitReseek > 2.5) {
            g_app.lastSeekInitReseek = nowSec;
            if (TriggerMpvReseek()) {
                snprintf(g_app.status, sizeof(g_app.status),
                         "NR 待初始化:已通知 mpv 原地重载滤镜会话");
            }
        }

        // Repaint only on input, pending edits, or changed stats: an idle
        // visible panel used to burn a full ImGui frame + vsynced Present 60
        // times a second for content that moves at most twice a second.
        // 页签恢复期例外:恢复需要连续渲染帧推进 ImGui 的布局状态,空闲门
        // 会把窗口冻在恢复完成前的旧帧上(启动页签显示错误的根因之一)。
        // Hidden-to-tray:仅跳过渲染,上方节流工作(参数推送/reseek/闭环兜底)
        // 照常运转 —— 托盘常驻进程的核心副作用不能只在前景发生;等待走下方
        // 带超时的 MsgWait,隐藏态的闭环检查由 stats 节拍驱动,不依赖消息。
        if (IsWindowVisible(g_hwnd) &&
            (activity || g_app.liveDirty || g_app.statsDirty || g_app.pageRestore)) {
            g_app.statsDirty = false;
            if (g_rtv) { // WM_SIZE failure (device removal) leaves no RTV to bind
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
            } else {
                // 设备移除(RTV 重建失败)后窗口冻结:静默黑窗改一次性弹窗
                // 说明(2026-10-05 评审修;设备/交换链创建内联在 wWinMain,
                // 就地重建需连 ImGui_ImplDX11 一起重初始化 —— 自动恢复为
                // 升级路径,需要时抽 CreateDeviceAndSwapChain 再接)。
                static bool deviceLostReported = false;
                if (!deviceLostReported) {
                    deviceLostReported = true;
                    PanelLog("panel: render device lost; window frozen until panel restart");
                    MessageBoxW(g_hwnd,
                                L"面板渲染设备丢失(驱动重置或远程会话切换)。\n面板停止刷新;请重新启动面板恢复(设置已保存)。",
                                L"DLSSNR 控制面板", MB_OK | MB_ICONWARNING);
                }
            }
        } else {
            // sleep until input arrives or the next 100ms stats tick comes due
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        }
    }

    if (watch) CloseHandle(watch);
    if (hdrTagThread) CloseHandle(hdrTagThread);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DropRenderTarget();
    if (g_swap) g_swap->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    PanelLog("panel: exit");
    return 0;
}