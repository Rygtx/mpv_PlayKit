// dlssnr_panel - independent control panel for the vs_dlssnr VapourSynth
// plugin. ImGui + D3D11 rendered UI (no native controls). Pushes the full
// parameter set into a named shared-memory mapping (panel_ipc.h; the plugin's
// bridge polls it) for live tuning, and dlssnr_ui.ini for saved defaults.

#include "dlssnr_params.h"
#include "panel_ipc.h"
#include "dlssnr_ini.h"
#include "fonts.h"

#include "resource.h"

using namespace vsdlssnr;

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
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
// INI_FILE / ALIVE_EVENT come from panel_ipc.h (cross-process contract names)
constexpr UINT WM_APP_TRAYICON = WM_APP + 1;
// 字号/间距整体缩小一档(用户偏好:面板在小屏也放得下);1.0 = 跟随 DPI 原尺寸
inline constexpr float kUiFontScale = 0.80f;
// 自绘标题栏几何(96dpi 基准,乘 uiScale)——DrawUi 画与 WM_NCHITTEST 判定
// 共用同一组常量,改动不会留下一条拖不动的死区。
inline constexpr float kTitleBarH = 38.0f;
inline constexpr float kCloseBtnSize = 30.0f;
inline constexpr float kCloseBtnPad = 10.0f;

// clang-format off
// Labels/tips are UTF-8 (the project compiles with /utf-8); the old
// wchar_t tables + per-frame WideCharToMultiByte are gone.
// 成员指针直达字段,避免键名 -> 字段的双份 if 链漂移;滑块范围同样直接
// 引用 dlssnr_params.h 的常量(文档注释里的 0-1 等只是给用户看的)。
constexpr struct { const char *key; const char *label; const char *tip;
                   float lo, hi; float DlssnrParams::*field; } kFineSliders[] = {
    { "residual_saturation", "残差饱和度", "对 DLSSNR 引起的饱和度变化的倍率(0-2,默认 1):\n1=保持,2=放大,0=移除。", kResidualFineMin, kResidualFineMax, &DlssnrParams::residualSaturation },
    { "residual_lightness",  "残差亮度",   "对 DLSSNR 引起的明度变化的倍率(0-2,默认 1):\n1=保持,2=放大,0=移除。", kResidualFineMin, kResidualFineMax, &DlssnrParams::residualLightness },
    { "shadow_structure",    "阴影结构",   "残差中变暗分量的倍率(0-2,默认 1):\n调低减轻暗部噪点,调高增强暗部结构。", kResidualFineMin, kResidualFineMax, &DlssnrParams::shadowStructureMultiplier },
    { "reflection_glow",     "反射辉光",   "残差中变亮分量的倍率(0-2,默认 1):\n调低抑制高光泛光,调高增强辉光。", kResidualFineMin, kResidualFineMax, &DlssnrParams::reflectionGlowMultiplier },
};
constexpr const char *kPresetNames[] = { "0(默认)", "1(预设 #1)", "2(预设 #2)", "3(预设 #3)" };
constexpr const char *kStyleNames[] = { "0(默认)", "1(自然)", "2(电影)" };
// 光流质量(上游 motionVectorQuality 0-5,文案对齐上游 resw)
constexpr const char *kOfQualityNames[] = {
    "无", "性能", "均衡(推荐)", "质量", "高质量(高开销)", "最高质量(极高开销)"
};
// 预设/风格/光流质量/各滑块/开关原以 kEnums/kSliders/kFlags 成员指针表
// 驱动通用循环;两列归并后每行控件异构(组合/滑块/复选框/整行),改为
// DrawUi 内联 + pairLabel/pairCombo/pairSlider lambda,tip 随行内联。
// clang-format on

struct AppState {
    DlssnrParams params{};
    float uiScale = 1.0f;
    int dpi = 96;
    bool liveDirty = false;
    bool reseekDirty = false; // 需要重建会话的变动(FG 档位/开、全关后开 NR):
                              // flush 时 WritePayload 后自动触发 mpv 原地 seek
    bool timingLog = true;
    bool advancedOpen = false; // 残差精调折叠区(ini [panel] advanced 记忆)
    double lastLiveWrite = 0.0;
    double lastStatsRead = 0.0;
    char status[160]{};
    char statsBig[64]{};
    char statsRes[96]{};
    char gpuName[128]{};
    char filterState[16]{};  // SK_FILTER_STATE: ok / nvof_zero / passthrough / ngx_faulted
    char stateDetail[160]{}; // SK_STATE_DETAIL: 死亡状态的原因串
    char ofMode[28]{};       // SK_OF_MODE: off / zero / forward[+cost] / both[+cost] q<N> grid<G>
                             // 最长 "forward+cost q5 grid4" = 22+1,28 防截断(与插件 _ofModeBuf 同尺寸)
    char fgState[16]{};      // SK_FG: on / dup / off / unavailable
    int fgMult = 0;          // SK_FG_MULT: 当前插帧倍数(未激活 = 0)
    double fps = 0.0;
    float segPack = 0.0f, segEval = 0.0f, segGpu = 0.0f, segUnpack = 0.0f, segNvof = 0.0f;
    float segFg = 0.0f;
    bool hasSegments = false;
    bool statsDirty = false; // LoadStats changed something on screen (redraw gate)
};

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
    if (dir.size() >= len) return false;
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
    if (g_payload->magic == PAYLOAD_MAGIC && g_payload->seq > 0) {
        LoadLiveParams(g_app.params, *g_payload);
        LoadCreateParams(g_app.params, *g_payload);
    }
    return true;
}

void WritePayload(bool saveRequest = false) noexcept {
    if (!g_payload) return;
    static uint32_t seq = 0;
    const uint32_t newSeq = ++seq;
    PanelPayload pl = PayloadFromParams(g_app.params); // shared field mapping; seq stays 0
    pl.generation = g_generation;
    pl.saveRequest = saveRequest ? 1 : 0;
    pl.logEnabled = g_app.timingLog ? 1 : 0;
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

// ---------------------------------------------------------------------------
// mpv IPC 自动重载:需要重建滤镜会话的变动(FG 倍数/开关、创建即全关后重开
// NR)由面板经 mpv JSON IPC 直接触发一次原地 seek —— mpv 的 vf_vapoursynth
// 在每次 seek 时整脚本重建,新实例采纳面板刚写入的 payload,变动即时生效,
// 免手动拖进度条。输出契约(帧数/节奏)随创建倍数定格,会话内无法改 ——
// 真档位变化只能重建,这正是自动 seek 的存在理由。
// 管道名发现:解析 ..\portable_config\mpv.conf 的 input-ipc-server,缺失时
// 回落常见默认名。全程 best-effort:连接失败(未启用 IPC / mpv 未运行 /
// 已退出)静默放弃,变动退化为插件的会话内 live 机制(降档复制真实帧)。
// ---------------------------------------------------------------------------
bool TriggerMpvReseek() noexcept {
    wchar_t base[MAX_PATH];
    if (!BasePath(base, MAX_PATH)) return false;

    // 管道名候选:mpv.conf 解析值优先(用户自定义名也能跟上),其后默认名。
    wchar_t *parsedName = nullptr; // _wcsdup;末尾 free(nullptr) 恒安全
    const wchar_t *candidates[3] = { nullptr, L"mpvpipe", L"mpvsocket" };
    {
        wchar_t confPath[MAX_PATH];
        swprintf_s(confPath, L"%s\\..\\portable_config\\mpv.conf", base);
        FILE *f = nullptr;
        if (_wfopen_s(&f, confPath, L"rb") == 0 && f) {
            char buf[16384]{};
            const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            // 逐行找未注释的 input-ipc-server = <名>(值可带引号)
            size_t pos = 0;
            while (pos < n) {
                const size_t eol = pos + strcspn(buf + pos, "\r\n");
                size_t s = pos;
                while (s < eol && (buf[s] == ' ' || buf[s] == '\t')) ++s;
                if (s + 16 <= eol && _strnicmp(buf + s, "input-ipc-server", 16) == 0) {
                    size_t eq = s + 16;
                    while (eq < eol && (buf[eq] == ' ' || buf[eq] == '\t')) ++eq;
                    if (eq < eol && buf[eq] == '=') {
                        ++eq;
                        while (eq < eol && (buf[eq] == ' ' || buf[eq] == '\t')) ++eq;
                        size_t e = eol;
                        while (e > eq && (buf[e - 1] == ' ' || buf[e - 1] == '\t' ||
                                          buf[e - 1] == '"' || buf[e - 1] == '\'')) --e;
                        if (e > eq && e - eq < 64) {
                            wchar_t parsed[64]{};
                            const int cw = MultiByteToWideChar(
                                CP_UTF8, 0, buf + eq, static_cast<int>(e - eq),
                                parsed, 63);
                            if (cw > 0) {
                                parsed[cw] = L'\0';
                                parsedName = _wcsdup(parsed); // 首选 = 配置值
                                candidates[0] = parsedName;
                            }
                        }
                        break; // 注释行(前导 #)不进此分支:首字符已是 '#'
                    }
                }
                pos = eol + 1;
            }
        }
    }

    bool ok = false;
    for (int i = 0; i < 3 && !ok; ++i) {
        if (!candidates[i] || !candidates[i][0]) continue;
        wchar_t pipePath[MAX_PATH];
        swprintf_s(pipePath, L"\\\\.\\pipe\\%s", candidates[i]);
        HANDLE pipe = CreateFileW(pipePath, GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) continue;
        // 原地微 seek(1ms 向前,exact):目标必异于当前帧 → 走完整 seek 路径
        // → vf_vapoursynth 整脚本重建;显示位置几乎不动。暂停态同样生效。
        const char *cmd = "{\"command\":[\"seek\",\"0.001\",\"relative+exact\"]}\n";
        DWORD written = 0;
        ok = WriteFile(pipe, cmd, static_cast<DWORD>(strlen(cmd)), &written, nullptr) &&
             written == strlen(cmd);
        char reply[128]{}; // 读掉一行响应(mpv 回显 success/error),内容不关心
        DWORD got = 0;
        ReadFile(pipe, reply, sizeof(reply) - 1, &got, nullptr);
        CloseHandle(pipe);
        if (ok) {
            PanelLog("panel: mpv reseek via IPC pipe %ls", candidates[i]);
        }
    }
    if (!ok) {
        PanelLog("panel: mpv IPC reseek unavailable (input-ipc-server off? mpv closed?); falling back to in-session live");
    }
    free(parsedName);
    return ok;
}

void WriteIniNow() noexcept {
    wchar_t base[MAX_PATH];
    if (!BasePath(base, MAX_PATH)) return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\%s", base, INI_FILE);
    if (!WriteDlssnrIni(g_app.params, path)) { // shared key list (dlssnr_ini.h)
        PanelLog("panel: save ini FAILED (file locked?)");
    }
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
    LoadDlssnrIni(g_app.params, path);
}

// Flat-object JSON extraction for the stats blob (our own writer's format)
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

bool JsonGetString(const char *body, const char *key, char *out, size_t outLen) noexcept {
    char pat[48];
    std::snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *k = strstr(body, pat);
    if (!k) return false;
    k += strlen(pat);
    const char *end = strchr(k, '"');
    std::snprintf(out, outLen, "%.*s",
                  end ? static_cast<int>(end - k) : static_cast<int>(strlen(k)), k);
    return true;
}

// Read the plugin's per-frame stats from named shared memory (zero disk IO).
// Mapping absent = no live filter: clear the stats line. The mapping object
// dies with the plugin process, so re-open each refresh (no stale handles).
void LoadStats() noexcept {
    const HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, STATS_MAPPING);
    if (!m) {
        g_app.statsDirty = g_app.statsBig[0] != 0 || g_app.statsRes[0] != 0 ||
                           g_app.filterState[0] != 0 || g_app.stateDetail[0] != 0 ||
                           g_app.ofMode[0] != 0 || g_app.fgState[0] != 0;
        g_app.statsBig[0] = 0;
        g_app.statsRes[0] = 0;
        g_app.filterState[0] = 0;
        g_app.stateDetail[0] = 0;
        g_app.ofMode[0] = 0;
        g_app.fgState[0] = 0;
        return;
    }
    // Seq-gated snapshot (protocol mirrors PublishStatsJson): a copy whose
    // counter moved mid-read, or a write still in progress (seq 0), is
    // discarded — the next 100ms refresh repaints it.
    const StatsPayload *view =
        static_cast<const StatsPayload *>(MapViewOfFile(m, FILE_MAP_READ, 0, 0, PAYLOAD_SIZE));
    StatsPayload st{};
    bool valid = false;
    if (view) {
        memcpy(&st, view, sizeof(st));
        valid = st.magic == STATS_MAGIC && st.seq != 0 &&
                st.seq == static_cast<const volatile StatsPayload *>(view)->seq;
        UnmapViewOfFile(view);
    }
    CloseHandle(m);
    if (!valid) return;
    const AppState before = g_app; // display snapshot for the redraw gate below
    const char *body = st.json;
    // 缺键即清零(JsonGetString 命中失败不写 out;旧版本插件的 body 没有
    // 新键,残留旧值会让状态行说谎)。
    if (!JsonGetString(body, SK_FILTER_STATE, g_app.filterState, sizeof(g_app.filterState)))
        g_app.filterState[0] = 0;
    if (!JsonGetString(body, SK_STATE_DETAIL, g_app.stateDetail, sizeof(g_app.stateDetail)))
        g_app.stateDetail[0] = 0;
    if (!JsonGetString(body, SK_OF_MODE, g_app.ofMode, sizeof(g_app.ofMode)))
        g_app.ofMode[0] = 0;
    if (!JsonGetString(body, SK_FG, g_app.fgState, sizeof(g_app.fgState)))
        g_app.fgState[0] = 0;
    g_app.fgMult = JsonGetInt(body, SK_FG_MULT, 0);
    if (JsonGetInt(body, SK_GPU_HANG, 0) != 0) {
        // The hang payload has no gpu_last, so the gate below would keep
        // showing frozen pre-hang stats forever; surface it — with the
        // device-removal reason the plugin publishes alongside the flag.
        snprintf(g_app.statsBig, sizeof(g_app.statsBig), "GPU 挂起/设备移除(滤镜已回退)");
        char reason[32];
        if (JsonGetString(body, SK_REMOVED_REASON, reason, sizeof(reason)) && reason[0]) {
            snprintf(g_app.statsRes, sizeof(g_app.statsRes), "移除原因 %s", reason);
        }
        // hang 有自己的展示行,清掉状态字段防上一 body 的残留
        g_app.filterState[0] = 0;
        g_app.stateDetail[0] = 0;
        g_app.ofMode[0] = 0;
        g_app.fgState[0] = 0;
    } else {
        const double gpuLast = JsonGetFloat(body, SK_GPU_LAST, -1);
        if (gpuLast >= 0) {
            snprintf(g_app.statsBig, sizeof(g_app.statsBig), "NGX 延迟 %.1f ms", gpuLast);
            // 分辨率展示:未开启缩放 -> 原生分辨率;开启 -> 处理分辨率 → 回源分辨率
            const int scaling = JsonGetInt(body, SK_SCALING, 0);
            const int iw = JsonGetInt(body, SK_INTERNAL_W, 0);
            const int ih = JsonGetInt(body, SK_INTERNAL_H, 0);
            const int w = JsonGetInt(body, SK_WIDTH, 0);
            const int h = JsonGetInt(body, SK_HEIGHT, 0);
            if (scaling && iw > 0 && ih > 0) {
                snprintf(g_app.statsRes, sizeof(g_app.statsRes),
                         "分辨率 %dx%d → %dx%d", iw, ih, w, h);
            } else {
                snprintf(g_app.statsRes, sizeof(g_app.statsRes),
                         "分辨率 %dx%d(原生)", w, h);
            }
            // 六段读每帧 last 值(与 NGX 延迟同语义):EMA 稳态冻结,
            // last 随帧呼吸(见 panel_ipc.h SK_*_LAST 注释)。
            g_app.segPack = static_cast<float>(JsonGetFloat(body, SK_PACK_LAST, 0));
            g_app.segEval = static_cast<float>(JsonGetFloat(body, SK_EVAL_CPU_LAST, 0));
            g_app.segGpu = static_cast<float>(JsonGetFloat(body, SK_GPU_LAST, 0));
            g_app.segUnpack = static_cast<float>(JsonGetFloat(body, SK_UNPACK_LAST, 0));
            g_app.segNvof = static_cast<float>(JsonGetFloat(body, SK_NVOF_LAST, 0));
            g_app.segFg = static_cast<float>(JsonGetFloat(body, SK_FG_LAST, 0));
            g_app.hasSegments = g_app.segGpu > 0;
            g_app.fps = JsonGetFloat(body, SK_FPS, 0);
            char gnPat[32];
            snprintf(gnPat, sizeof(gnPat), "\"%s\":\"", SK_GPU_NAME);
            const char *gn = strstr(body, gnPat);
            if (gn) {
                gn += strlen(gnPat);
                const char *end = strchr(gn, '"');
                const size_t len = end ? std::min<size_t>(end - gn, 127) : 0;
                if (len) {
                    memcpy(g_app.gpuName, gn, len);
                    g_app.gpuName[len] = 0;
                }
            }
        } else {
            // 死亡 body(passthrough / ngx_faulted):清掉冻结的旧统计与
            // 分段,让状态行成为唯一内容。
            g_app.statsBig[0] = 0;
            g_app.statsRes[0] = 0;
            g_app.segPack = g_app.segEval = g_app.segGpu = g_app.segUnpack = g_app.segNvof = 0.0f;
            g_app.segFg = 0.0f;
            g_app.hasSegments = false;
            g_app.fps = 0.0;
        }
    }
    g_app.statsDirty = memcmp(before.statsBig, g_app.statsBig, sizeof(g_app.statsBig)) != 0 ||
                       memcmp(before.statsRes, g_app.statsRes, sizeof(g_app.statsRes)) != 0 ||
                       memcmp(before.gpuName, g_app.gpuName, sizeof(g_app.gpuName)) != 0 ||
                       memcmp(before.filterState, g_app.filterState, sizeof(g_app.filterState)) != 0 ||
                       memcmp(before.stateDetail, g_app.stateDetail, sizeof(g_app.stateDetail)) != 0 ||
                       memcmp(before.ofMode, g_app.ofMode, sizeof(g_app.ofMode)) != 0 ||
                       memcmp(before.fgState, g_app.fgState, sizeof(g_app.fgState)) != 0 ||
                       before.fps != g_app.fps || before.hasSegments != g_app.hasSegments ||
                       before.segPack != g_app.segPack || before.segEval != g_app.segEval ||
                       before.segGpu != g_app.segGpu || before.segUnpack != g_app.segUnpack ||
                       before.segNvof != g_app.segNvof || before.segFg != g_app.segFg;
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
    const float th = kTitleBarH * s;
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
    const float bs = kCloseBtnSize * s, bpad = kCloseBtnPad * s;
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
            ShowTip("隐藏到托盘(退出在托盘右键菜单)");
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(wpos.x + 16 * s, wpos.y + (th - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextUnformatted("DLSSNR 控制面板");

    // --- 性能分析器(照抄 Magpie Overlay Profiler 样式,数据来自插件共享内存) ---
    // 遥测带背景高度流式自适应:用上一帧的带底位置画背景(滞后一帧无感),
    // Collapse 收起/展开时窗口高度随之自动收缩。
    static float s_bandBottom = th + 120 * s;
    dl->AddRectFilled(ImVec2(wpos.x, wpos.y + th), ImVec2(wpos.x + wsize.x, wpos.y + s_bandBottom),
                      IM_COL32(24, 28, 40, 255));
    dl->AddLine(ImVec2(wpos.x, wpos.y + s_bandBottom), ImVec2(wpos.x + wsize.x, wpos.y + s_bandBottom),
                IM_COL32(58, 62, 78, 255));
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + th + 12 * s));

    // Magpie Profiler 头部:GPU 名称 + 帧率(全程显示,无滤镜时占位)。
    // 首行必须手动定位到 marginX:后续行自动回流到 WindowPadding.x,历史
    // 上这里写死 16*s,而 WindowPadding 不缩放,高 DPI 下首行比后续行更靠右。
    ImGui::Text("GPU: %s", g_app.gpuName[0] ? g_app.gpuName : "(等待滤镜加载)");
    ImGui::Text("帧率: %.1f FPS", g_app.fps);

    // 大号:NGX 纯延迟(窗口级字体缩放, imgui 1.91 的 PushFont 是单参;
    // 1.4→1.25 高度收紧,辨识度足够)
    ImGui::SetWindowFontScale(1.25f);
    ImGui::TextColored(ImVec4(120 / 255.0f, 190 / 255.0f, 1.0f, 1.0f), "%s", g_app.statsBig);
    ImGui::SetWindowFontScale(1.0f);
    if (g_app.statsRes[0]) {
        ImGui::TextDisabled("%s", g_app.statsRes);
    }
    // 滤镜状态行(SK_FILTER_STATE):这是降级/故障在面板上的唯一可见信号
    // (GUI mpv 看不到日志,timing log 没人看)。空 = 正常。
    {
        const ImVec4 warnCol(1.0f, 0.62f, 0.20f, 1.0f);
        const ImVec4 errCol(1.0f, 0.45f, 0.45f, 1.0f);
        if (std::strcmp(g_app.filterState, "passthrough") == 0) {
            ImGui::TextColored(warnCol, "滤镜已回退直通(画面未增强)");
            if (g_app.stateDetail[0]) ImGui::TextDisabled("%s", g_app.stateDetail);
        } else if (std::strcmp(g_app.filterState, "ngx_faulted") == 0) {
            ImGui::TextColored(errCol, "NGX 故障,滤镜已停用 —— 重启 mpv 恢复");
            if (g_app.stateDetail[0]) ImGui::TextDisabled("%s", g_app.stateDetail);
        } else if (std::strcmp(g_app.filterState, "nvof_zero") == 0) {
            ImGui::TextColored(warnCol, "光流已降级为零 guidance(增强继续)");
            if (g_app.stateDetail[0]) ImGui::TextDisabled("%s", g_app.stateDetail);
        }
        // 实际光流模式:请求档位 ≠ 实际能力(Turing 无 cost / 驱动拒双向)
        // 时在这里暴露;档位关闭(off)不显示。
        if (g_app.ofMode[0] && std::strcmp(g_app.ofMode, "off") != 0) {
            ImGui::TextDisabled("光流模式: %s", g_app.ofMode);
        }
        // DLSS FG 状态:on=插值中;dup=复制帧(复位/零光流/开关暂关/降级);
        // off=本会话未激活;unavailable=代理初始化失败回退 1:1。
        if (g_app.fgState[0] && std::strcmp(g_app.fgState, "off") != 0) {
            const char *desc = std::strcmp(g_app.fgState, "on") == 0 ? "插值中"
                             : std::strcmp(g_app.fgState, "dup") == 0 ? "复制帧"
                             : std::strcmp(g_app.fgState, "unavailable") == 0 ? "不可用(1:1)"
                             : g_app.fgState;
            if (g_app.fgMult >= 2 && (std::strcmp(g_app.fgState, "on") == 0 ||
                                      std::strcmp(g_app.fgState, "dup") == 0)) {
                ImGui::TextDisabled("帧生成: %s (%dx)", desc, g_app.fgMult);
            } else {
                ImGui::TextDisabled("帧生成: %s", desc);
            }
        }
    }
    ImGui::Spacing();

    // 效果渲染用时(堆叠时间线,列宽 = 耗时占比)
    const bool timingsOpen = ImGui::CollapsingHeader("处理用时", ImGuiTreeNodeFlags_DefaultOpen);
    if (timingsOpen && g_app.hasSegments) {
        // 分段 = 帧内执行顺序:nvof(光流等待)在 pack 之后、NGX 录制之前。
        // of=0 时 nvof 恒 0,零值段由下方 <1e-3f 跳过,时间线退回四段。
        const float total = g_app.segPack + g_app.segNvof + g_app.segEval +
                            g_app.segFg + g_app.segGpu + g_app.segUnpack;
        if (total > 0.5f) {
            struct Seg { float v; ImU32 c; const char *name; };
            const Seg segs[6]{
                { g_app.segPack,   IM_COL32(229, 57, 53, 255),   "pack(打包)" },
                { g_app.segNvof,   IM_COL32(156, 39, 176, 255),  "nvof(光流)" },
                { g_app.segEval,   IM_COL32(63, 81, 181, 255),   "eval_cpu(NGX 调用)" },
                { g_app.segFg,     IM_COL32(0, 150, 136, 255),   "fg(补帧GPU)" },
                { g_app.segGpu,    IM_COL32(30, 136, 229, 255),  "gpu(NR+输出)" },
                { g_app.segUnpack, IM_COL32(0, 137, 123, 255),   "unpack(解包)" },
            };
            constexpr int kSegCount = 6;
            // 实际要画的段数(零值段跳过)。BeginTable 的列数必须与之相等:
            // imgui 对本帧未 TableSetupColumn 的列按 SizingStretchSame 默认
            // 权重 1.0 补齐,而可见段权重和恒为 1.0 —— 空列恰好占掉一半
            // 宽度(of=0 时长条右侧大片空白)。
            int visibleSegs = 0;
            for (int i = 0; i < kSegCount; ++i)
                if (segs[i].v >= 1e-3f) ++visibleSegs;

            ImGui::Spacing();
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5, 5));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

            if (ImGui::BeginTable("timeline", visibleSegs)) {
                for (int i = 0; i < kSegCount; ++i) {
                    if (segs[i].v < 1e-3f) continue;
                    char colId[8];
                    snprintf(colId, sizeof(colId), "%d", i);
                    ImGui::TableSetupColumn(colId,
                        ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoReorder,
                        segs[i].v / total);
                }
                ImGui::TableNextRow();
                for (int i = 0; i < kSegCount; ++i) {
                    if (segs[i].v < 1e-3f) continue;
                    ImGui::TableNextColumn();
                    ImGui::PushID(i);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, segs[i].c);
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive, segs[i].c);
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, segs[i].c);
                    ImGui::PushStyleColor(ImGuiCol_Header, segs[i].c);
                    ImGui::Selectable("", false);
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered() || ImGui::IsItemClicked()) {
                        char tipContent[128];
                        snprintf(tipContent, sizeof(tipContent), "%s\n%.3f ms\n%d%%", segs[i].name,
                                 segs[i].v, static_cast<int>(segs[i].v / total * 100 + 0.5f));
                        ImGui::SetTooltip("%s", tipContent);
                    }
                    // 空间足够时居中显示百分比
                    char pctText[16];
                    snprintf(pctText, sizeof(pctText), "%d%%", static_cast<int>(segs[i].v / total * 100 + 0.5f));
                    const float textWidth = ImGui::CalcTextSize(pctText).x;
                    const float itemWidth = ImGui::GetItemRectSize().x;
                    if (itemWidth > textWidth + 4 * s) {
                        ImGui::SameLine(0, 0);
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (itemWidth - textWidth) / 2);
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 0.5f * s);
                        ImGui::TextUnformatted(pctText);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleVar(4);
            ImGui::Spacing();

            // timings 列表:色点 ■ + 名称 + 右对齐时间
            if (ImGui::BeginTable("timings", 1, ImGuiTableFlags_PadOuterX)) {
                ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoResize);
                for (int si = 0; si < kSegCount; ++si) {
                    const Seg &seg = segs[si];
                    if (seg.v < 1e-3f) continue; // 零值段不列(of=0 的 nvof / 关缩放)
                    ImGui::PushID(si);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    char timeStr[32];
                    snprintf(timeStr, sizeof(timeStr), "%.3f ms", seg.v);
                    const float timeWidth = ImGui::CalcTextSize(timeStr).x;
                    const float wrapPos = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - timeWidth - 8 * s;
                    ImGui::Selectable("", false, 0, ImVec2(0, ImGui::GetTextLineHeight()));
                    ImGui::SameLine(0, 3 * s);
                    const ImVec4 segCol = ImGui::ColorConvertU32ToFloat4(seg.c);
                    // ■ 字形已在 fontUI 的 glyph range 里(Magpie COLOR_INDICATOR 同款)
                    ImGui::TextColored(segCol, "%s", vsdlssnr::fonts::COLOR_INDICATOR);
                    ImGui::SameLine(0, 3 * s);
                    ImGui::PushTextWrapPos(wrapPos);
                    ImGui::TextUnformatted(seg.name);
                    ImGui::PopTextWrapPos();
                    ImGui::SameLine(0, 0);
                    ImGui::SetCursorPosX(wrapPos + 8 * s);
                    // 时间数值用等宽数字字体(Magpie _fontMonoNumbers 用途)
                    if (g_fontMono) {
                        ImGui::PushFont(g_fontMono);
                        ImGui::TextUnformatted(timeStr);
                        ImGui::PopFont();
                    } else {
                        ImGui::TextUnformatted(timeStr);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::Spacing();
        }
    }

    // 遥测带底(供下一帧背景;随 Collapse 自动收缩)
    s_bandBottom = ImGui::GetCursorPosY() + 10 * s;
    float y = s_bandBottom + 12 * s;

    // --- 内容(独立提示行已删:live-apply 语义并入 保存设置 的 tooltip) ---
    dl->AddLine(ImVec2(wpos.x + marginX, wpos.y + y), ImVec2(wpos.x + wsize.x - marginX, wpos.y + y), IM_COL32(58, 62, 78, 255));
    y += 14 * s;

    // Label 与控件垂直居中:控件文本在 frame 内偏移 FramePadding.y,标签
    // 按同一中心线对齐。旧的硬编码 +7/+8*s 在高 DPI 下漂移(字体随 s 放大,
    // FramePadding 固定不放大),表现为参数名与滑块错位。
    const float labelDy = (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f;
    // 行高 = 控件 frame + 紧凑间距(#38 度量驱动;间距从 10*s 收紧到 4*s
    // —— 高度预算的主压缩之一,控件 frame 本体不动)。
    const float rowH = ImGui::GetFrameHeight() + 4 * s;

    // 参数行布局:相关短控件两两并排(预设|风格、四条强度滑杆),复杂控件
    // 保整行 —— 13 行 → 9 行。控件列统一对齐:整行与半格行的控件都从
    // colCtrl(半格行第二列从 colCtrl+halfW)起步,标签列宽 = colCtrl−marginX。
    // 整行控件宽度封顶 260*s(下拉/滑杆拉满整行会过长);半格控件 ~102*s。
    const float halfW = (wsize.x - 2 * marginX) * 0.5f;
    const float pairLabelW = colCtrl - marginX;
    auto pairLabel = [&](int col, const char *label, const char *tip) {
        ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + col * halfW, wpos.y + y + labelDy));
        ImGui::TextUnformatted(label);
        if (tip && ImGui::IsItemHovered()) ShowTip(tip);
    };
    auto pairCombo = [&](const char *key, int DlssnrParams::*f, int count,
                         const char *const *names, int col) {
        ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + col * halfW + pairLabelW, wpos.y + y));
        ImGui::SetNextItemWidth(halfW - pairLabelW - 8 * s);
        int v = g_app.params.*f;
        if (ImGui::Combo((std::string("##") + key).c_str(), &v, names, count)) {
            g_app.params.*f = v;
            g_app.liveDirty = true;
        }
    };
    auto pairSlider = [&](const char *key, float DlssnrParams::*f, float lo, float hi, int col) {
        ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + col * halfW + pairLabelW, wpos.y + y));
        ImGui::SetNextItemWidth(halfW - pairLabelW - 8 * s);
        float v = g_app.params.*f;
        if (ImGui::SliderFloat((std::string("##") + key).c_str(), &v, lo, hi, "%.2f")) {
            // NGX accepts continuous float steps (verified: 0.01 steps produce
            // distinct outputs), so no snapping to Magpie's UI-level 0.05 grid.
            g_app.params.*f = v;
            g_app.liveDirty = true;
        }
    };
    auto fullSlider = [&](const char *key, const char *label, const char *tip,
                          float DlssnrParams::*f, float lo, float hi) {
        ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y + labelDy));
        ImGui::TextUnformatted(label);
        if (ImGui::IsItemHovered()) ShowTip(tip);
        ImGui::SetCursorScreenPos(ImVec2(wpos.x + colCtrl, wpos.y + y));
        // (std::min) 括号抑制 windows.h 的 min 宏(无 NOMINMAX)
        ImGui::SetNextItemWidth((std::min)(wsize.x - colCtrl - marginX, 260 * s));
        float v = g_app.params.*f;
        if (ImGui::SliderFloat((std::string("##") + key).c_str(), &v, lo, hi, "%.2f")) {
            g_app.params.*f = v;
            g_app.liveDirty = true;
        }
        y += rowH;
    };

    // 降噪增强总开关(整行;live 即时,只关降噪不影响补帧/光流)
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y + labelDy));
    ImGui::TextUnformatted("降噪增强");
    if (ImGui::IsItemHovered())
        ShowTip("降噪总开关。关闭 = 跳过降噪,补帧/光流照常;与帧生成都关时滤镜零开销。\n"
                "重开自动触发 mpv 重载(需 IPC,未启用时手动 seek)。面板优先于 vpy。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + colCtrl, wpos.y + y));
    {
        bool v = g_app.params.nrEnabled != 0;
        if (ImGui::Checkbox("##nr_enabled", &v)) {
            // 会话未初始化(passthrough 且非 live 关)时开 NR 无法 live 恢复
            // —— 需要重建,自动触发原地 seek;其余情况 live 门即时生效。
            const bool needsReseek =
                v && strcmp(g_app.filterState, "passthrough") == 0 &&
                strncmp(g_app.stateDetail, "NR off", 6) != 0;
            g_app.params.nrEnabled = v ? 1 : 0;
            g_app.liveDirty = true;
            if (needsReseek) g_app.reseekDirty = true;
        }
    }
    y += rowH;

    // (预设 | 风格)
    pairLabel(0, "预设", "NR 推理预设:0=默认,1-3=预设 #1/#2/#3。切换会短暂重建模型(毫秒级)。");
    pairCombo("preset", &DlssnrParams::preset, 4, kPresetNames, 0);
    pairLabel(1, "风格", "处理风格:0=默认,1=自然(Natural),2=电影(Cinematic)。");
    pairCombo("style", &DlssnrParams::style, 3, kStyleNames, 1);
    y += rowH;

    // 光流质量(整行:档位文案长,半宽会截断)
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y + labelDy));
    ImGui::TextUnformatted("光流质量");
    if (ImGui::IsItemHovered())
        ShowTip("NVIDIA 光流引导,减轻运动场景的时域伪影;档位越高越精确也越耗时。\n"
                "需 RTX Turing+,不支持时自动回退\"无\"。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + colCtrl, wpos.y + y));
    ImGui::SetNextItemWidth((std::min)(wsize.x - colCtrl - marginX, 260 * s));
    {
        int v = g_app.params.motionVectorQuality;
        if (ImGui::Combo("##motion_vector_quality", &v, kOfQualityNames, 6)) {
            g_app.params.motionVectorQuality = v;
            g_app.liveDirty = true;
        }
    }
    y += rowH;

    // 光流跟随降采样(整行)
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y + labelDy));
    ImGui::TextUnformatted("光流跟随降采样");
    if (ImGui::IsItemHovered())
        ShowTip("光流按内部缩放尺寸计算,省光流开销、精度略降(需先开分辨率缩放)。\n帧生成激活时忽略。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + colCtrl, wpos.y + y));
    {
        bool v = g_app.params.nvofFollowScaling != 0;
        if (ImGui::Checkbox("##nvof_follow_scaling", &v)) {
            g_app.params.nvofFollowScaling = v ? 1 : 0;
            g_app.liveDirty = true;
        }
    }
    y += rowH;

    // DLSS 帧生成(整行:倍数选择 关/2x/3x/4x)
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y + labelDy));
    ImGui::TextUnformatted("帧生成");
    if (ImGui::IsItemHovered())
        ShowTip("DLSS 补帧,输出帧率 ×2–×4,插值帧落在相邻真实帧之间。\n"
                "需光流质量 > 0(否则只复制帧)和 ngx 下的帧生成运行时,失败自动回退 1:1。\n"
                "改档位/开关自动触发 mpv 重载(需 IPC,未启用时升档需手动 seek)。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + colCtrl, wpos.y + y));
    {
        // 单一控件:关(=0)/2x/3x/4x,同步 fgEnabled + fgMultiplier 两键。
        const int items = 4;
        const char *labels[items] = { "关", "2x", "3x", "4x" };
        int sel = g_app.params.fgEnabled ? std::clamp(g_app.params.fgMultiplier, kFgMultMin, kFgMultMax) - 1 : 0;
        ImGui::SetNextItemWidth(110 * s);
        if (ImGui::Combo("##fg_mode", &sel, labels, items)) {
            if (sel <= 0) {
                g_app.params.fgEnabled = 0;
            } else {
                g_app.params.fgEnabled = 1;
                g_app.params.fgMultiplier = sel + 1; // 2..4
            }
            g_app.liveDirty = true;
            // 输出契约(帧数/节奏)随创建档位定格,任何档位/开关的真变化都
            // 需要 mpv 重建滤镜 —— 自动触发原地 seek(payload 先落,重建的
            // 新实例即采纳);IPC 不可用时插件侧退化为会话内降档复制。
            g_app.reseekDirty = true;
        }
    }
    y += rowH;

    // FG 路由(整行;进程级,重启 mpv 生效;单档覆盖后端与内核架构)
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y + labelDy));
    ImGui::TextUnformatted("FG 路由");
    if (ImGui::IsItemHovered())
        ShowTip("帧生成后端:自动 = 官方优先(RTX 40/50),回落代理(30 系)。\n"
                "SM86/SM75 = 固定走代理;官方 NGX = 固定官方档(拒载不回落)。\n"
                "进程级,重启 mpv 生效。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + colCtrl, wpos.y + y));
    {
        const int items = 4;
        const char *labels[items] = { "自动 (官方优先)", "SM86 (RTX 30 系)", "SM75 (RTX 20 系)",
                                      "官方 NGX (RTX 40/50 系)" };
        int sel = std::clamp(g_app.params.fgRoute, kFgRouteMin, kFgRouteMax);
        ImGui::SetNextItemWidth(170 * s);
        if (ImGui::Combo("##fg_route", &sel, labels, items)) {
            g_app.params.fgRoute = sel;
            g_app.liveDirty = true;
        }
    }
    y += rowH;

    // (强度 | 局部色调)
    pairLabel(0, "强度", "整体处理强度(0-1,默认 1)。数值越高降噪/增强越明显。");
    pairSlider("intensity", &DlssnrParams::intensity, kStrengthMin, kStrengthMax, 0);
    pairLabel(1, "局部色调", "局部色调强度(0-1,默认 1)。影响明暗过渡区域的处理力度。");
    pairSlider("local_tone", &DlssnrParams::localToneStrength, kStrengthMin, kStrengthMax, 1);
    y += rowH;

    // (局部结构 | 皮肤结构)
    pairLabel(0, "局部结构", "局部结构强度(0-1,默认 1)。越高保留越多细节纹理。");
    pairSlider("local_structure", &DlssnrParams::localStructureStrength, kStrengthMin, kStrengthMax, 0);
    pairLabel(1, "皮肤结构", "皮肤结构强度(-1=保持默认行为,范围 -1~2)。影响人物皮肤区域的细节保留。");
    pairSlider("skin_structure", &DlssnrParams::skinStructureStrength, kSkinMin, kSkinMax, 1);
    y += rowH;

    // 残差乘数(整行)
    fullSlider("residual_multiplier", "残差乘数",
               "重建细节的增强倍率(1-2,默认 1),配合分辨率缩放使用。",
               &DlssnrParams::residualMultiplier, kResidualMultMin, kResidualMultMax);

    // 分辨率缩放(整行:开关 + 百分比滑杆)
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y + labelDy));
    ImGui::TextUnformatted("分辨率缩放");
    if (ImGui::IsItemHovered())
        ShowTip("按百分比分辨率推理再重建回源,降耗省帧;关闭则按源分辨率直接处理。\n百分比改动会短暂重建模型(毫秒级)。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + colCtrl, wpos.y + y));
    {
        bool scalingOn = g_app.params.scalingEnabled != 0;
        if (ImGui::Checkbox("##scaling_enabled", &scalingOn)) {
            g_app.params.scalingEnabled = scalingOn ? 1 : 0;
            g_app.liveDirty = true;
        }
        ImGui::SameLine(0, 12 * s);
        ImGui::SetNextItemWidth((std::min)(wsize.x - colCtrl - marginX - 60 * s, 200 * s));
        int ir = g_app.params.inputResolutionPercent;
        if (ImGui::SliderInt("##input_resolution", &ir, kResPctMin, kResPctMax, "%d%%")) {
            g_app.params.inputResolutionPercent = std::clamp(ir, kResPctMin, kResPctMax);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            // Debounce: rebuilding per drag tick would recreate the feature +
            // textures every frame; push once on slider release instead.
            g_app.liveDirty = true;
        }
    }
    y += rowH;

    auto drawSliderRows = [&](const auto &table) {
        for (const auto &sl : table) {
            ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y + labelDy));
            ImGui::Text("%s", sl.label);
            if (ImGui::IsItemHovered()) ShowTip(sl.tip);
            ImGui::SetCursorScreenPos(ImVec2(wpos.x + colCtrl, wpos.y + y));
            ImGui::SetNextItemWidth((std::min)(wsize.x - colCtrl - marginX, 260 * s));
            float v = g_app.params.*(sl.field);
            if (ImGui::SliderFloat(("##" + std::string(sl.key)).c_str(), &v, sl.lo, sl.hi, "%.2f")) {
                // NGX accepts continuous float steps (verified: 0.01 steps produce
                // distinct outputs), so no snapping to Magpie's UI-level 0.05 grid.
                g_app.params.*(sl.field) = v;
                g_app.liveDirty = true;
            }
            y += rowH;
        }
    };

    // 残差精调(高级):默认收起;展开状态记忆在 ini [panel] advanced。
    // SetNextItemOpen 把 ini 值喂给 ImGui 的内部开合存储——CollapsingHeader
    // 的状态在 ImGui 自己的 storage 里,不播种的话首帧永远读到"收起",还会
    // 把 ini 值回写成 0。
    ImGui::SetNextItemOpen(g_app.advancedOpen, ImGuiCond_Once);
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));
    const bool advanced = ImGui::CollapsingHeader("残差精调(高级)");
    if (ImGui::IsItemHovered())
        ShowTip("残差微调:1 = 保持 DLSSNR 的变化,默认全部中性,一般无需调整。");
    if (advanced != g_app.advancedOpen) {
        g_app.advancedOpen = advanced;
        wchar_t base[MAX_PATH], path[MAX_PATH];
        if (BasePath(base, MAX_PATH)) {
            swprintf_s(path, MAX_PATH, L"%s\\%s", base, INI_FILE);
            WritePrivateProfileStringW(L"panel", L"advanced", advanced ? L"1" : L"0", path);
        }
    }
    // 用实际渲染高度推进 y(标题条高度随字体/DPI 变化,硬编码预算会和
    // 下一个区块贴死或重叠)。
    y = ImGui::GetItemRectMax().y - wpos.y + 8 * s;
    if (advanced) drawSliderRows(kFineSliders);

    dl->AddLine(ImVec2(wpos.x + marginX, wpos.y + y), ImVec2(wpos.x + wsize.x - marginX, wpos.y + y), IM_COL32(58, 62, 78, 255));
    y += 14 * s;

    // 模型行为开关 + 性能日志:三个短复选框一行(高度压缩;tooltip 不变)
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));
    {
        bool autoMask = g_app.params.useAutoMask != 0;
        if (ImGui::Checkbox("自动蒙版", &autoMask)) {
            g_app.params.useAutoMask = autoMask ? 1 : 0;
            g_app.liveDirty = true;
        }
        if (ImGui::IsItemHovered()) ShowTip("自动蒙版。模型自动识别区域并区别处理。");
        ImGui::SameLine(0, 24 * s);
        bool uiFix = g_app.params.uiCorrection != 0;
        if (ImGui::Checkbox("UI 文字修正", &uiFix)) {
            g_app.params.uiCorrection = uiFix ? 1 : 0;
            g_app.liveDirty = true;
        }
        if (ImGui::IsItemHovered()) ShowTip("UI 修正。降低对画面内文字/UI 元素的涂抹。");
        ImGui::SameLine(0, 24 * s);
        bool logOn = g_app.timingLog;
        if (ImGui::Checkbox("写入性能日志", &logOn)) {
            g_app.timingLog = logOn;
            wchar_t iniPath[MAX_PATH];
            if (BasePath(iniPath, MAX_PATH)) {
                wchar_t iniFile[MAX_PATH];
                swprintf_s(iniFile, L"%s\\%s", iniPath, INI_FILE);
                WritePrivateProfileStringW(L"panel", L"log", logOn ? L"1" : L"0", iniFile);
            }
            WritePayload(); // logEnabled included
            g_app.liveDirty = false;
        }
        if (ImGui::IsItemHovered()) ShowTip("每秒追加一行性能统计到 mpv 同目录 dlssnr_timing.log;排查性能问题时附上该文件。");
    }
    y += rowH;

    y += 8 * s;
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));
    if (ImGui::Button("保存设置", ImVec2(120 * s, 30 * s))) {
        WriteIniNow();
        WritePayload(true); // persist-through-bridge flag included
        snprintf(g_app.status, sizeof(g_app.status), "已保存: %ls", INI_FILE);
    }
    if (ImGui::IsItemHovered()) {
        ShowTip("当前设置即时生效(下一帧画面);保存为默认值(dlssnr_ui.ini)后,下次加载滤镜自动生效。");
    }
    ImGui::SameLine(0, 14 * s);
    if (ImGui::Button("重置默认", ImVec2(120 * s, 30 * s))) {
        // Reset = push the factory-default payload itself; the plugin applies
        // it through the same Request*/Update path as any other edit (there
        // is no separate reset command in the protocol).
        g_app.params = DlssnrParams{};
        WritePayload();
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
        const int wantW = static_cast<int>(500 * g_app.uiScale);
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
    CreateMutexW(nullptr, TRUE, L"vs_dlssnr_panel_single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        PanelLog("panel: duplicate launch exits (single-instance guard)");
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
                             WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 500, 450,
                             nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    // Windows 11 rounded corners for the borderless window
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));

    // Width must scale with DPI too (height self-fits to content per frame).
    g_app.dpi = GetDpiForWindow(g_hwnd);
    g_app.uiScale = g_app.dpi / 96.0f * kUiFontScale;
    if (g_app.dpi != 96) {
        SetWindowPos(g_hwnd, nullptr, 0, 0, static_cast<int>(500 * g_app.uiScale),
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
    Shell_NotifyIconW(NIM_ADD, &g_nid);

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
            // 需要重建会话的变动:payload 落地后立即触发 mpv 原地 seek,
            // 重建的滤镜实例即采纳新值(免手动拖进度条)。
            if (g_app.reseekDirty) {
                g_app.reseekDirty = false;
                TriggerMpvReseek();
            }
        }

        // Throttled stats read (plugin publishes into the stats mapping every
        // frame); shared memory, zero disk IO. 100ms 读节流 + 100ms 空闲唤醒:
        // 处理用时 ~10Hz 呼吸;空闲时无重绘,唤醒成本 = 一次 512B 映射读。
        if (nowSec - g_app.lastStatsRead > 0.1) {
            g_app.lastStatsRead = nowSec;
            LoadStats();
        }

        // Repaint only on input, pending edits, or changed stats: an idle
        // visible panel used to burn a full ImGui frame + vsynced Present 60
        // times a second for content that moves at most twice a second.
        if (activity || g_app.liveDirty || g_app.statsDirty) {
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
            }
        } else {
            // sleep until input arrives or the next 100ms stats tick comes due
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        }
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
    PanelLog("panel: exit");
    return 0;
}
