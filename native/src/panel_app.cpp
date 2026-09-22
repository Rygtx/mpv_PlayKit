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
#include <fstream>
#include <mutex>
#include <sstream>
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
// 参数区双列几何(96dpi 基准):滑块/下拉统一轨道宽 200(kTrackW,96dpi 下
// 160px,精调所需的最短长度),标签列 94(colCtrl−marginX,容 5 个中文字),
// 列间留白 24(kColGap,右列标签不贴左列控件),窗宽 648 = 2*18 + 2*(94+200)
// + 24,96dpi 下 518px。
inline constexpr float kPanelW = 648.0f;
inline constexpr float kTrackW = 200.0f;
inline constexpr float kColGap = 24.0f;

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
constexpr const char *kStyleNames[] = { "0(默认)", "1(自然)", "2(电影)" };
// 光流后端(0=ffx 1=nvof)
constexpr const char *kOfBackendNames[] = { "FFX (AMD 光流,默认)", "NVOF (NVIDIA 引擎)" };
// 光流质量(上游 motionVectorQuality 0-5,文案对齐上游 resw)
constexpr const char *kOfQualityNames[] = {
    "无", "性能", "均衡(推荐)", "质量", "高质量(高开销)", "最高质量(极高开销)"
};
// FFX 质量档位(独立三档,与上游 6 档解耦:1 = Performance 半分辨率,
// 2 = Quality 全分辨率 —— 上游 1-2/3-5 在 FFX 内各只对应一种行为)
constexpr const char *kFfxQualityNames[] = { "无", "性能 (1/2 分辨率)", "质量 (全分辨率)" };
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
    int page = 0;              // 功能页签:0=降噪增强,1=帧生成(ini [panel] page 记忆)
    bool pageRestore = true;   // 页签启动恢复锁:恢复期内每帧重喂 SetSelected 并
                               // 强制重绘,直到期望页真正可见(或预算烧完)才解除。
                               // 只喂首帧不够 —— SetSelected 排队到下一帧布局才
                               // 落地,期间空闲门可能不再重绘,恢复静默失败且首帧
                               // 可见的旧页会把 ini 无痕改写。恢复期内抑制页签落盘。
    int restoreFrames = 8;     // 恢复期重绘预算(帧);只在窗口可见时消耗
    double lastLiveWrite = 0.0;
    double lastStatsRead = 0.0;
    char status[160]{};
    char statsBig[64]{};
    char statsRes[96]{};
    char gpuName[128]{};
    char filterState[16]{};  // SK_FILTER_STATE: ok / nvof_zero / passthrough / ngx_faulted
    char stateDetail[160]{}; // SK_STATE_DETAIL: 死亡状态的原因串
    char ofMode[40]{};       // SK_OF_MODE: off / zero / 后端能力串(最长
                             // "fxof q5 qual 1920x1080" = 23+1;与插件 _ofModeBuf 同尺寸)
    char fgState[16]{};      // SK_FG: on / dup / off / unavailable
    int fgMult = 0;          // SK_FG_MULT: 当前插帧倍数(未激活 = 0)
    char fgRouteEff[16]{};   // SK_FG_ROUTE_EFFECTIVE: off/official-hook/official/copy
    int fgMultCreate = 0;    // SK_FG_MULT_CREATE: 会话创建倍数(FG 未激活 = 0)
    char fgDetail[128]{};    // SK_FG_DETAIL: FG 最近一次初始化失败原因(成功 = 空)
    int fgMultMax = 0;       // SK_FG_MULT_MAX: 运行库插值帧上限(FG 未激活 = 0)。
                             // gate 解锁失败回落 2x 的唯一面板侧信号源
    char ofDetail[96]{};     // SK_OF_DETAIL: 光流会话创建失败原因(成功 = 空)
    char rtxState[16]{};     // SK_RTX: off / vsr / hdr / vsr+hdr(RTX 管线实态)
    char rtxDetail[96]{};    // SK_RTX_DETAIL: VSR/TrueHDR 最近失败原因(成功 = 空)
    int connState = 0;       // stats 通道连接态:0=未检测到插件 1=已连接
                             // 2=magic 不匹配(面板/插件版本未成对更新)
    int fgOptimized = 1;     // dlssg_for_sm86 [FrameGeneration] Optimized 0-3
                             // (存储单点 = 代理 ini;面板启动回读,重启 mpv 生效)
    float slotWait = 0.0f;   // SK_SLOT_WAIT: 槽池等待 last(诊断页)
    float lockWait = 0.0f;   // SK_LOCK_WAIT: evaluate 互斥等待 last(诊断页)
    int gateSkips = 0;       // SK_GATE_SKIPS: 光流帧序门跳帧累计(诊断页)
    int gateExpired = 0;     // SK_GATE_EXPIRED: 过期帧累计(诊断页)
    int gateResets = 0;      // SK_GATE_RESETS: 历史重置累计(诊断页)
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
// mpv IPC 自动重载:需要重建滤镜会话的变动(vsrMode/scale/HDR 开关、FG
// 开关等 create-time 参数)由面板经 mpv JSON IPC 直接触发一次原地 seek
// —— vf_vapoursynth 在 seek 时整脚本重建(播放/暂停两态实测均重建,
// 2026-09-23),新实例在 create 时采纳面板刚写入的 payload,变动即时
// 生效,免手动拖进度条。输出契约(帧数/节奏、输出尺寸/格式)随创建
// 定格,会话内无法改 ——
// 真档位变化只能重建,这正是自动 seek 的存在理由。
// 管道名发现:解析 ..\portable_config\mpv.conf 的 input-ipc-server,缺失时
// 回落常见默认名。全程 best-effort:连接失败(未启用 IPC / mpv 未运行 /
// 已退出)静默放弃,变动退化为插件的会话内 live 机制(降档复制真实帧)。
// ---------------------------------------------------------------------------
bool TriggerMpvReseek() noexcept {
    wchar_t base[MAX_PATH];
    if (!BasePath(base, MAX_PATH)) return false;

    // 管道名候选:mpv.conf 解析值优先(用户自定义名也能跟上),其后默认名
    // 与 umpv 默认值(umpv 拉起的 mpv 以 --input-ipc-server=<umpv.conf 值>
    // 覆盖 mpv.conf,面板解析 mpv.conf 会扑空 —— 候选兜底,两个默认值
    // 错开的场景仍能连上)。
    wchar_t *parsedName = nullptr; // _wcsdup;末尾 free(nullptr) 恒安全
    const wchar_t *candidates[4] = { nullptr, L"mpvpipe", L"mpvsocket", L"umpv" };
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
                        // 行内注释剥离:`input-ipc-server = mpvpipe # ...`
                        // 曾经把 "# ..." 并进管道名,首选候选必然失连。
                        for (size_t h = eq; h < e; ++h) {
                            if (buf[h] == '#') { e = h; break; }
                        }
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
    for (int i = 0; i < 4 && !ok; ++i) {
        if (!candidates[i] || !candidates[i][0]) continue;
        wchar_t pipePath[MAX_PATH];
        swprintf_s(pipePath, L"\\\\.\\pipe\\%s", candidates[i]);
        HANDLE pipe = CreateFileW(pipePath, GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) continue;
        // 原地微 seek(1ms 向前,exact)触发 vf_vapoursynth 整脚本重建,
        // 新实例在 create 时采纳刚发布的 payload create-time 三元组
        // (vsrMode/scale/hdr)。实测播放/暂停两态均稳定重建(2026-09-23
        // 4/4);曾误判"seek 不重建"(23:02 风暴零新实例)—— 对照测试
        // 推翻,该次异常归因于管道归属的环境性歧义,机制本身有效。
        const char *cmd = "{\"command\":[\"seek\",\"0.001\",\"relative+exact\"]}\n";
        DWORD written = 0, got = 0;
        ok = WriteFile(pipe, cmd, static_cast<DWORD>(strlen(cmd)), &written, nullptr) &&
             written == strlen(cmd);
        char ack[128]{};
        ReadFile(pipe, ack, sizeof(ack) - 1, &got, nullptr);
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
    // 页签:0=降噪增强 1=帧生成 2=RTX 超分/HDR 3=诊断
    g_app.page = static_cast<int>(std::clamp(GetPrivateProfileIntW(L"panel", L"page", 0, path), 0u, 3u));
    // 旧版 fg_route 越界值(v20 语义作废的 2/3 钉档)被 clamp 重解释:
    // 状态行提示,不再无声 —— 老用户"补帧怎么没了"的直接答案。
    {
        int legacyRoute = 0;
        LoadDlssnrIni(g_app.params, path, &legacyRoute);
        if (legacyRoute > kFgRouteMax) {
            snprintf(g_app.status, sizeof(g_app.status),
                     "检测到旧版 fg_route=%d(档位语义已作废),已按新版解释为纯官方;"
                     "如需自动档请在面板选回 \"自动\"",
                     legacyRoute);
        }
    }
    // Optimized 档位的存储单点 = 代理 ini(代理只在进程加载时读一次),
    // 面板启动回读为准 —— 不落 ui.ini,避免双存储漂移。
    g_app.fgOptimized = ReadFgOptimizedIni();
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
        const int prevConn = g_app.connState;
        g_app.statsDirty = prevConn != 0 ||
                           g_app.statsBig[0] != 0 || g_app.statsRes[0] != 0 ||
                           g_app.filterState[0] != 0 || g_app.stateDetail[0] != 0 ||
                           g_app.ofMode[0] != 0 || g_app.fgState[0] != 0 ||
                           g_app.fgRouteEff[0] != 0 || g_app.fgDetail[0] != 0 ||
                           g_app.ofDetail[0] != 0 ||
                           g_app.rtxState[0] != 0 || g_app.rtxDetail[0] != 0;
        g_app.statsBig[0] = 0;
        g_app.statsRes[0] = 0;
        g_app.filterState[0] = 0;
        g_app.stateDetail[0] = 0;
        g_app.ofMode[0] = 0;
        g_app.fgState[0] = 0;
        g_app.fgRouteEff[0] = 0;
        g_app.fgDetail[0] = 0;
        g_app.ofDetail[0] = 0;
        g_app.rtxState[0] = 0;
        g_app.rtxDetail[0] = 0;
        g_app.fgMult = 0;
        g_app.fgMultCreate = 0;
        g_app.fgMultMax = 0;
        g_app.slotWait = g_app.lockWait = 0.0f;
        g_app.gateSkips = g_app.gateExpired = g_app.gateResets = 0;
        g_app.connState = 0; // 连接指示随之变化(statsDirty 已置)
        return;
    }
    // Seq-gated snapshot (protocol mirrors PublishStatsJson): a copy whose
    // counter moved mid-read, or a write still in progress (seq 0), is
    // discarded — the next 100ms refresh repaints it.
    const StatsPayload *view =
        static_cast<const StatsPayload *>(MapViewOfFile(m, FILE_MAP_READ, 0, 0, PAYLOAD_SIZE));
    StatsPayload st{};
    bool valid = false;
    bool badMagic = false;
    if (view) {
        // 混部署(新面板 + 旧插件的小映射)时按映射实际区域钳制拷贝量:
        // 直接 sizeof(st) 是标准意义上的越界读,分页粒度通常掩盖但不该赌。
        MEMORY_BASIC_INFORMATION mbi{};
        size_t copy = sizeof(st);
        if (VirtualQuery(view, &mbi, sizeof(mbi)) && mbi.RegionSize > 0 &&
            mbi.RegionSize < sizeof(st)) {
            copy = mbi.RegionSize;
        }
        memcpy(&st, view, copy);
        badMagic = st.magic != 0 && st.magic != STATS_MAGIC;
        valid = st.magic == STATS_MAGIC && st.seq != 0 &&
                st.seq == static_cast<const volatile StatsPayload *>(view)->seq;
        UnmapViewOfFile(view);
    }
    CloseHandle(m);
    if (!valid) {
        // magic 不符 = 面板与插件版本未成对更新:不再静默冻结旧显示,
        // 连接指示红显("看起来活着但调参无效"的最短诊断路径)。
        if (badMagic) {
            if (g_app.connState != 2) g_app.statsDirty = true;
            g_app.connState = 2;
        }
        return;
    }
    const AppState before = g_app; // display snapshot for the redraw gate below
    g_app.connState = 1;           // after the snapshot: 0→1 跳变要进下方 diff
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
    // FG 实际路由/创建倍数/失败原因 + 排队细分(缺键即清:死亡 body 不带
    // 这些键,残留旧值会让状态行说谎 —— 与 filter_state 同款规则)。
    if (!JsonGetString(body, SK_FG_ROUTE_EFFECTIVE, g_app.fgRouteEff, sizeof(g_app.fgRouteEff)))
        g_app.fgRouteEff[0] = 0;
    g_app.fgMultCreate = JsonGetInt(body, SK_FG_MULT_CREATE, 0);
    if (!JsonGetString(body, SK_FG_DETAIL, g_app.fgDetail, sizeof(g_app.fgDetail)))
        g_app.fgDetail[0] = 0;
    // v21 键:运行库上限 + 光流失败原因(旧插件 body 无键,缺键即清,
    // 与 fgDetail 同款规则 —— 残留旧值会让状态行说谎)。
    g_app.fgMultMax = JsonGetInt(body, SK_FG_MULT_MAX, 0);
    if (!JsonGetString(body, SK_OF_DETAIL, g_app.ofDetail, sizeof(g_app.ofDetail)))
        g_app.ofDetail[0] = 0;
    // v22 键:RTX Video 实态 + 失败原因(缺键即清,同款规则)。
    if (!JsonGetString(body, SK_RTX, g_app.rtxState, sizeof(g_app.rtxState)))
        g_app.rtxState[0] = 0;
    if (!JsonGetString(body, SK_RTX_DETAIL, g_app.rtxDetail, sizeof(g_app.rtxDetail)))
        g_app.rtxDetail[0] = 0;
    g_app.slotWait = static_cast<float>(JsonGetFloat(body, SK_SLOT_WAIT, 0));
    g_app.lockWait = static_cast<float>(JsonGetFloat(body, SK_LOCK_WAIT, 0));
    g_app.gateSkips = JsonGetInt(body, SK_GATE_SKIPS, 0);
    g_app.gateExpired = JsonGetInt(body, SK_GATE_EXPIRED, 0);
    g_app.gateResets = JsonGetInt(body, SK_GATE_RESETS, 0);
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
        g_app.fgRouteEff[0] = 0;
        g_app.fgDetail[0] = 0;
        g_app.ofDetail[0] = 0;
        g_app.rtxState[0] = 0;
        g_app.rtxDetail[0] = 0;
        g_app.fgMult = 0;
        g_app.fgMultCreate = 0;
        g_app.fgMultMax = 0;
        g_app.slotWait = g_app.lockWait = 0.0f;
        g_app.gateSkips = g_app.gateExpired = g_app.gateResets = 0;
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
            g_app.fgRouteEff[0] = 0;
            g_app.fgDetail[0] = 0;
            g_app.ofDetail[0] = 0;
            g_app.rtxState[0] = 0;
            g_app.rtxDetail[0] = 0;
            g_app.fgMult = 0;
            g_app.fgMultCreate = 0;
            g_app.fgMultMax = 0;
            g_app.slotWait = g_app.lockWait = 0.0f;
            g_app.gateSkips = g_app.gateExpired = g_app.gateResets = 0;
        }
    }
    g_app.statsDirty = memcmp(before.statsBig, g_app.statsBig, sizeof(g_app.statsBig)) != 0 ||
                       memcmp(before.statsRes, g_app.statsRes, sizeof(g_app.statsRes)) != 0 ||
                       memcmp(before.gpuName, g_app.gpuName, sizeof(g_app.gpuName)) != 0 ||
                       memcmp(before.filterState, g_app.filterState, sizeof(g_app.filterState)) != 0 ||
                       memcmp(before.stateDetail, g_app.stateDetail, sizeof(g_app.stateDetail)) != 0 ||
                       memcmp(before.ofMode, g_app.ofMode, sizeof(g_app.ofMode)) != 0 ||
                       memcmp(before.fgState, g_app.fgState, sizeof(g_app.fgState)) != 0 ||
                       memcmp(before.fgRouteEff, g_app.fgRouteEff, sizeof(g_app.fgRouteEff)) != 0 ||
                       memcmp(before.fgDetail, g_app.fgDetail, sizeof(g_app.fgDetail)) != 0 ||
                       memcmp(before.ofDetail, g_app.ofDetail, sizeof(g_app.ofDetail)) != 0 ||
                       memcmp(before.rtxState, g_app.rtxState, sizeof(g_app.rtxState)) != 0 ||
                       memcmp(before.rtxDetail, g_app.rtxDetail, sizeof(g_app.rtxDetail)) != 0 ||
                       before.fgMult != g_app.fgMult || before.fgMultCreate != g_app.fgMultCreate ||
                       before.fgMultMax != g_app.fgMultMax || before.connState != g_app.connState ||
                       before.slotWait != g_app.slotWait || before.lockWait != g_app.lockWait ||
                       before.gateSkips != g_app.gateSkips ||
                       before.gateExpired != g_app.gateExpired ||
                       before.gateResets != g_app.gateResets ||
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

// SK_FG_ROUTE_EFFECTIVE → 中文标签(状态带与诊断页共用;off/空/未知 = 不显示)
const char *FgRouteLabel(const char *v) noexcept {
    if (!v || !v[0]) return "";
    if (std::strcmp(v, "official-hook") == 0) return "官方 NGX(0.3.x 代理接管)";
    if (std::strcmp(v, "official") == 0) return "官方 NGX 直连";
    if (std::strcmp(v, "copy") == 0) return "未生效(复制帧)";
    return "";
}


// 不一致红色(请求了但没在跑):状态带 / 帧生成页 / 诊断页共用。
inline const ImVec4 kErrRed(1.0f, 0.42f, 0.42f, 1.0f);
// 常规信息灰(状态带 FG 行/诊断页取值列)
inline const ImVec4 kDimTxt(0.62f, 0.64f, 0.70f, 1.0f);

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
    const float colCtrl = 112 * s;      // 控件列起点(标签列 94 = 容 5 个中文字)
    const float trackW = kTrackW * s;   // 滑块/下拉统一轨道宽(全面板唯一宽度)
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
    // 连接指示:此前"滤镜未加载 / 插件没在跑 / stats magic 不配对"三种
    // 故障的用户可见表象完全相同(滑块能调、能保存、画面永远不变),
    // 区别只在日志文件。这里一行给出最短诊断路径。
    {
        const char *connTxt;
        ImVec4 col = kDimTxt;
        if (g_app.connState == 1) {
            connTxt = "插件: 已连接";
        } else if (g_app.connState == 2) {
            connTxt = "插件: 版本不匹配 —— 面板与 vs_dlssnr.dll 必须成对更新";
            col = kErrRed;
        } else {
            connTxt = "插件: 未检测到(等待滤镜加载;不走滤镜路线则调参仅作默认值保存)";
        }
        ImGui::TextColored(col, "%s", connTxt);
    }

    // 大号:NGX 纯延迟(窗口级字体缩放, imgui 1.91 的 PushFont 是单参;
    // 1.4→1.25 高度收紧,辨识度足够)
    ImGui::SetWindowFontScale(1.25f);
    ImGui::TextColored(ImVec4(120 / 255.0f, 190 / 255.0f, 1.0f, 1.0f), "%s", g_app.statsBig);
    ImGui::SetWindowFontScale(1.0f);
    if (g_app.statsRes[0]) {
        ImGui::TextDisabled("%s", g_app.statsRes);
    }
    // 滤镜状态行(SK_FILTER_STATE):这是降级/故障在面板上的唯一可见信号
    // (GUI mpv 看不到日志,timing log 没人看)。空 = 正常。主面板保持
    // 纯净调参体验:请求 vs 实际的分级红显集中在"诊断"页。
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
            // 原因直达(of_detail):此前"为什么降级"只活在 timing log。
            if (g_app.ofDetail[0]) ImGui::TextDisabled("%s", g_app.ofDetail);
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
                // 库上限 < 面板倍数(40 系 gate 解锁失败回落 2x):输出按
                // 面板倍数节拍走但实际密度只有 (上限+1)x —— 必须在此可见,
                // 否则"选了 6x 全绿"而实际每 6 帧里 4 帧是复制帧。
                if (g_app.fgMultMax >= 1 && g_app.fgMultMax + 1 < g_app.fgMult) {
                    ImGui::TextDisabled("帧生成: %s (%dx, 实效 %dx —— 运行库上限)",
                                        desc, g_app.fgMult, g_app.fgMultMax + 1);
                } else {
                    ImGui::TextDisabled("帧生成: %s (%dx)", desc, g_app.fgMult);
                }
            } else {
                ImGui::TextDisabled("帧生成: %s", desc);
            }
        }
    }
    ImGui::Spacing();

    // 遥测带底(供下一帧背景):带内只留状态行。处理用时整段移到窗口
    // 最底部 —— 分段行随帧出现/消失引起的高度变化只影响底部收口,
    // 不再推动上方的参数控件(高度快速抖动问题)。
    s_bandBottom = ImGui::GetCursorPosY() + 8 * s;
    float y = s_bandBottom + 12 * s;

    // Label 与控件垂直居中:控件文本在 frame 内偏移 FramePadding.y,标签
    // 按同一中心线对齐。旧的硬编码 +7/+8*s 在高 DPI 下漂移(字体随 s 放大,
    // FramePadding 固定不放大),表现为参数名与滑块错位。
    const float labelDy = (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f;
    // 行高 = 控件 frame + 紧凑间距(#38 度量驱动;间距从 10*s 收紧到 4*s
    // —— 高度预算的主压缩之一,控件 frame 本体不动)。
    const float rowH = ImGui::GetFrameHeight() + 4 * s;

    // 参数行布局:全双列,相关控件两两并排。所有滑块/下拉统一轨道宽
    // trackW(kTrackW,96dpi 下 160px)—— 不再有长短滑块,精调精度全面板
    // 一致。控件列对齐:第一列控件从 colCtrl 起步,第二列从 marginX +
    // (halfW+colGap) + pairLabelW;标签列宽 = colCtrl−marginX = 94(容
    // 5 个中文字),两列之间留 colGap 空隙,右列标签不贴左列控件。
    const float colGap = kColGap * s;
    const float halfW = (wsize.x - 2 * marginX - colGap) * 0.5f; // 单列内容宽
    const float pairLabelW = colCtrl - marginX;
    auto pairLabel = [&](int col, const char *label, const char *tip) {
        ImGui::SetCursorScreenPos(
            ImVec2(wpos.x + marginX + col * (halfW + colGap), wpos.y + y + labelDy));
        ImGui::TextUnformatted(label);
        if (tip && ImGui::IsItemHovered()) ShowTip(tip);
    };
    auto pairCombo = [&](const char *key, int DlssnrParams::*f, int count,
                         const char *const *names, int col) {
        ImGui::SetCursorScreenPos(
            ImVec2(wpos.x + marginX + col * (halfW + colGap) + pairLabelW, wpos.y + y));
        ImGui::SetNextItemWidth(trackW);
        int v = g_app.params.*f;
        if (ImGui::Combo((std::string("##") + key).c_str(), &v, names, count)) {
            g_app.params.*f = v;
            g_app.liveDirty = true;
        }
    };
    auto pairSlider = [&](const char *key, float DlssnrParams::*f, float lo, float hi, int col) {
        ImGui::SetCursorScreenPos(
            ImVec2(wpos.x + marginX + col * (halfW + colGap) + pairLabelW, wpos.y + y));
        ImGui::SetNextItemWidth(trackW);
        float v = g_app.params.*f;
        if (ImGui::SliderFloat((std::string("##") + key).c_str(), &v, lo, hi, "%.2f")) {
            // NGX accepts continuous float steps (verified: 0.01 steps produce
            // distinct outputs), so no snapping to Magpie's UI-level 0.05 grid.
            g_app.params.*f = v;
            g_app.liveDirty = true;
        }
    };
    // 半格复选框(标签 + 隐名复选框,与滑块行同款对齐)
    auto pairCheck = [&](const char *key, int DlssnrParams::*f, int col) {
        ImGui::SetCursorScreenPos(
            ImVec2(wpos.x + marginX + col * (halfW + colGap) + pairLabelW, wpos.y + y));
        bool v = g_app.params.*f != 0;
        if (ImGui::Checkbox((std::string("##") + key).c_str(), &v)) {
            g_app.params.*f = v ? 1 : 0;
            g_app.liveDirty = true;
        }
    };
    // 页签选择记忆落 ini([panel] page),与 advancedOpen 同款。
    auto savePagePref = [&](int p) {
        g_app.page = p;
        wchar_t base[MAX_PATH], path[MAX_PATH], val[8];
        if (BasePath(base, MAX_PATH)) {
            swprintf_s(path, MAX_PATH, L"%s\\%s", base, INI_FILE);
            swprintf_s(val, 8, L"%d", p);
            WritePrivateProfileStringW(L"panel", L"page", val, path);
        }
    };

    // --- 功能页签:降噪增强 / 帧生成分页,不再全挤在一页 ---
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));
    bool pgNrVis = false, pgFgVis = false, pgRtxVis = false, pgDiagVis = false; // 本帧各页可见性
    if (ImGui::BeginTabBar("##feature_tabs")) {
        // --- 页:降噪增强(NR + 光流 + 分辨率缩放) ---
        if (ImGui::BeginTabItem("降噪增强", nullptr,
                                (g_app.pageRestore && g_app.page == 0)
                                    ? ImGuiTabItemFlags_SetSelected
                                    : ImGuiTabItemFlags_None)) {
            pgNrVis = true;
            // 内容可见且 page 不符 = 用户刚点击切到本页(点击帧内容尚未可
            // 见,次日帧才渲染 —— IsItemClicked 检测不到,改为以"可见性"
            // 落盘)。恢复期内不落盘:启动早期帧的"可见"是 appearing 布局
            // 的陈旧态,不是用户选择,落盘会把 ini 打回第 0 页。
            if (!g_app.pageRestore && g_app.page != 0) savePagePref(0);
            y = ImGui::GetCursorPosY() - wpos.y + 6 * s;

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

    // (风格 | 光流后端):两个模式下拉并排。预设下拉已移除(2026-09-14)
    // —— 310.9 DLL 不消费 DLSSNR.Hint.Render.Preset,A/B 实测输出恒等
    // (死旋钮);上游 r2-fix1 同款处置。preset 参数链(vpy/ini/IPC/NGX
    // 写入)保留:vpy 是 validate_params 的 preset 哨兵断言测试通道,新 DLL
    // 激活 preset 时(哨兵变 DIFF)面板加回下拉即可。
    // 光流后端为创建时参数:切档触发 OF 会话原位重建,下一帧生效,无需
    // mpv 重启;显式档失败不跨后端回落。
    pairLabel(0, "风格", "处理风格:0=默认,1=自然(Natural),2=电影(Cinematic)。");
    pairCombo("style", &DlssnrParams::style, 3, kStyleNames, 0);
    pairLabel(1, "光流后端", "光流引擎:FFX(AMD FidelityFX 光流)= 默认,跨厂商通用\n"
              "(需 D3D12 SM6.2 + WaveOps);NVOF = NVIDIA 专属引擎,可选。\n"
              "切档下一帧生效,无需重启 mpv;质量档位选项随之变化。");
    pairCombo("of_backend", &DlssnrParams::ofBackend, 2, kOfBackendNames, 1);
    y += rowH;

    // (光流质量 | 光流降采样):光流组。质量选项随后端变化(单下拉双字段:
    // NVOF→motionVectorQuality 0-5 六档;FFX→ffxQuality 0-2 三档)。
    {
        const int ofBackend = std::clamp(g_app.params.ofBackend, kOfBackendMin, kOfBackendMax);
        const char *ofQualityTip =
            ofBackend == kOfBackendFfx
                ? "AMD FidelityFX 光流(跨厂商):性能 = 光流半分辨率(更快),\n质量 = 全分辨率(更精)。"
                : "NVIDIA 光流引导,减轻运动场景的时域伪影;档位越高越精确也越耗时。\n"
                  "需 RTX Turing+,不支持时自动回退\"无\"。";
        pairLabel(0, "光流质量", ofQualityTip);
        ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
        ImGui::SetNextItemWidth(trackW);
        bool changed = false;
        if (ofBackend == kOfBackendFfx) {
            int sel = std::clamp(g_app.params.ffxQuality, kFfxQualityMin, kFfxQualityMax);
            if (ImGui::Combo("##motion_vector_quality", &sel, kFfxQualityNames, 3)) {
                g_app.params.ffxQuality = sel;
                changed = true;
            }
        } else { // nvof:上游 0-5
            int v = g_app.params.motionVectorQuality;
            if (ImGui::Combo("##motion_vector_quality", &v, kOfQualityNames, 6)) {
                g_app.params.motionVectorQuality = v;
                changed = true;
            }
        }
        if (changed) g_app.liveDirty = true;
    }
    pairLabel(1, "光流降采样", "光流按内部缩放尺寸计算,省光流开销、精度略降(需先开分辨率缩放)。\n帧生成激活时忽略。");
    pairCheck("nvof_follow_scaling", &DlssnrParams::nvofFollowScaling, 1);
    y += rowH;

    // (强度 | 局部色调)
    pairLabel(0, "强度", "整体处理强度(0-2,默认 1)。数值越高降噪/增强越明显。");
    pairSlider("intensity", &DlssnrParams::intensity, kStrengthMin, kStrengthMax, 0);
    pairLabel(1, "局部色调", "局部色调强度(0-2,默认 1)。影响明暗过渡区域的处理力度。");
    pairSlider("local_tone", &DlssnrParams::localToneStrength, kStrengthMin, kStrengthMax, 1);
    y += rowH;

    // (局部结构 | 皮肤结构)
    pairLabel(0, "局部结构", "局部结构强度(0-2,默认 1)。越高保留越多细节纹理。");
    pairSlider("local_structure", &DlssnrParams::localStructureStrength, kStrengthMin, kStrengthMax, 0);
    pairLabel(1, "皮肤结构", "皮肤结构强度(0-2,默认 0)。影响人物皮肤区域的细节保留。");
    pairSlider("skin_structure", &DlssnrParams::skinStructureStrength, kSkinMin, kSkinMax, 1);
    y += rowH;

    // (分辨率缩放 | 残差乘数):缩放总开关独占标准控件位,不与滑杆同行
    // 挤占 —— 开关前置会把 %滑杆推出轨道列(与其他滑块左缘不齐)。
    pairLabel(0, "分辨率缩放",
              "按百分比分辨率推理再重建回源,降耗省帧;关闭则按源分辨率直接处理。");
    pairCheck("scaling_enabled", &DlssnrParams::scalingEnabled, 0);
    pairLabel(1, "残差乘数", "重建细节的增强倍率(1-2,默认 1),配合分辨率缩放使用。");
    pairSlider("residual_multiplier", &DlssnrParams::residualMultiplier,
               kResidualMultMin, kResidualMultMax, 1);
    y += rowH;

    // (缩放比例 | 自动蒙版):%滑杆占标准控件位(与全局轨道对齐),缩放
    // 关闭时置灰。百分比改动会短暂重建模型(毫秒级),松手才推送(live)。
    pairLabel(0, "缩放比例",
              "内部推理分辨率占源分辨率的百分比(25-100%);\"分辨率缩放\"关闭时无效。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
    ImGui::SetNextItemWidth(trackW);
    {
        const bool scalingOn = g_app.params.scalingEnabled != 0;
        if (!scalingOn) ImGui::BeginDisabled(true);
        int ir = g_app.params.inputResolutionPercent;
        if (ImGui::SliderInt("##input_resolution", &ir, kResPctMin, kResPctMax, "%d%%")) {
            g_app.params.inputResolutionPercent = std::clamp(ir, kResPctMin, kResPctMax);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            // Debounce: rebuilding per drag tick would recreate the feature +
            // textures every frame; push once on slider release instead.
            g_app.liveDirty = true;
        }
        if (!scalingOn) ImGui::EndDisabled();
    }
    pairLabel(1, "自动蒙版", "自动蒙版。模型自动识别区域并区别处理。");
    pairCheck("auto_mask", &DlssnrParams::useAutoMask, 1);
    y += rowH;

    // 精调四条两两并排(与主强度滑杆同款轨道宽)
    auto drawSliderRows = [&](const auto &table) {
        const int count = static_cast<int>(sizeof(table) / sizeof(table[0]));
        for (int i = 0; i < count; i += 2) {
            pairLabel(0, table[i].label, table[i].tip);
            pairSlider(table[i].key, table[i].field, table[i].lo, table[i].hi, 0);
            if (i + 1 < count) {
                pairLabel(1, table[i + 1].label, table[i + 1].tip);
                pairSlider(table[i + 1].key, table[i + 1].field,
                           table[i + 1].lo, table[i + 1].hi, 1);
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

            ImGui::EndTabItem();
        }

        // --- 页:帧生成(DLSS-FG 后端与路由) ---
        if (ImGui::BeginTabItem("帧生成", nullptr,
                                (g_app.pageRestore && g_app.page == 1)
                                    ? ImGuiTabItemFlags_SetSelected
                                    : ImGuiTabItemFlags_None)) {
            pgFgVis = true;
            if (!g_app.pageRestore && g_app.page != 1) savePagePref(1);
            y = ImGui::GetCursorPosY() - wpos.y + 6 * s;

    // (帧生成 | FG 路由):档位与路由都是创建期参数并排 —— 改档位触发
    // mpv 重载,路由进程级重启生效。倍数选择 关/2x/3x/4x/5x/6x,同步
    // fgEnabled + fgMultiplier 两键。
    pairLabel(0, "帧生成", "DLSS 补帧,输出帧率 ×2–×6,插值帧落在相邻真实帧之间。\n"
              "需光流质量 > 0(否则只复制帧)和 ngx 下的帧生成运行时,失败自动回退 1:1。\n"
              "改档位/开关自动触发 mpv 重载(需 IPC,未启用时升档需手动 seek)。\n"
              "运行库上限默认开到 6x(部署 ini MaxGeneratedFrames=5);\n"
              "输出帧率 = 源 ×M,显示端刷新率建议 ≥ 输出帧率。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
    ImGui::SetNextItemWidth(trackW);
    {
        // 运行库上限 MaxGeneratedFrames 由部署 ini 常开到 5(fetch-deps 归一),
        // 倍数选择无需触碰代理 ini —— 改档原地重载即时生效。
        const int items = kFgMultMax; // 关 + 2x..6x
        const char *labels[items] = { "关", "2x", "3x", "4x", "5x", "6x" };
        int sel = g_app.params.fgEnabled ? std::clamp(g_app.params.fgMultiplier, kFgMultMin, kFgMultMax) - 1 : 0;
        if (ImGui::Combo("##fg_mode", &sel, labels, items)) {
            if (sel <= 0) {
                g_app.params.fgEnabled = 0;
            } else {
                g_app.params.fgEnabled = 1;
                g_app.params.fgMultiplier = sel + 1; // 2..6
            }
            g_app.liveDirty = true;
            // 输出契约(帧数/节奏)随创建档位定格,任何档位/开关的真变化都
            // 需要 mpv 重建滤镜 —— 自动触发原地 seek(payload 先落,重建的
            // 新实例即采纳);IPC 不可用时插件侧退化为会话内降档复制。
            g_app.reseekDirty = true;
        }
    }
    pairLabel(1, "FG 路由", "自动 = 预载 0.3.x hook 代理(RTX 30/20 系由其接管 DLSS-G,\n"
              "仍走官方签名链;需 ngx\\version.dll)。\n"
              "纯官方 = 不预载代理,直连官方运行时(RTX 40/50)。\n"
              "进程级,重启 mpv 生效。实际生效档在\"诊断\"页显示。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + (halfW + colGap) + pairLabelW, wpos.y + y));
    ImGui::SetNextItemWidth(trackW);
    {
        const int items = 2;
        const char *labels[items] = { "自动 (预载 0.3.x 代理)", "纯官方 (不预载)" };
        int sel = std::clamp(g_app.params.fgRoute, kFgRouteMin, kFgRouteMax);
        if (ImGui::Combo("##fg_route", &sel, labels, items)) {
            g_app.params.fgRoute = sel;
            g_app.liveDirty = true;
        }
    }
    y += rowH;

    // Optimized 内核档(整行;dlssg_for_sm86 一致性档位。代理只在进程加载
    // 时读一次 ini → 重启 mpv 生效;存储单点 = 代理 ini,面板启动回读)
    pairLabel(0, "内核档位", "dlssg_for_sm86 一致性档位:0 = 原厂内核不加速;\n"
              "1 = 全部逐位一致加速(推荐,默认,与官方画面完全相同);\n"
              "2/3 = 再开有损内核,更快但画面渐让。\n"
              "写入 ngx\\dlssg_sm86.ini,重启 mpv 生效。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
    ImGui::SetNextItemWidth(trackW);
    {
        const int items = 4;
        const char *labels[items] = { "0 原厂", "1 逐位一致 (默认)", "2 有损 (PSNR>50dB)",
                                      "3 全部有损" };
        int sel = std::clamp(g_app.fgOptimized, 0, 3);
        if (ImGui::Combo("##fg_optimized", &sel, labels, items)) {
            if (WriteFgOptimizedIni(sel)) {
                g_app.fgOptimized = sel;
            } else {
                // 写失败(UI 与磁盘静默分叉曾经只进日志):不采纳选择,
                // 下拉弹回现值 + 状态栏说明,重启后"档位自己变回去"不再发生。
                snprintf(g_app.status, sizeof(g_app.status),
                         "内核档位写入失败(代理配置缺失或被占用)");
            }
        }
    }
    y += rowH;

            // 跨页依赖提示:光流质量在降噪增强页
            ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));
            ImGui::TextDisabled("提示: 帧生成需光流质量 > 0(降噪增强页),否则只复制帧。");
            y += rowH;

            ImGui::EndTabItem();
        }

        // --- 页:RTX 超分 / HDR(NVIDIA RTX Video SDK 1.1 的 NGX VSR +
        // TrueHDR;管线顺序 NR→VSR→HDR→FG,官方明文 HDR 必须在 VSR 后)---
        if (ImGui::BeginTabItem("RTX 超分/HDR", nullptr,
                                (g_app.pageRestore && g_app.page == 2)
                                    ? ImGuiTabItemFlags_SetSelected
                                    : ImGuiTabItemFlags_None)) {
            pgRtxVis = true;
            if (!g_app.pageRestore && g_app.page != 2) savePagePref(2);
            y = ImGui::GetCursorPosY() - wpos.y + 6 * s;

    // (VSR 超分 | VSR 质量):开关为创建时参数(变化触发 mpv 原地重载);
    // 质量 1-4 为 per-eval live,拖动即时生效(官方 bicubic 0 档不暴露 ——
    // 关闭走开关,不再付一份 GPU 价买双线性)。
    pairLabel(0, "VSR 超分", "NVIDIA RTX Video 超分(SDR RGB,RTX 显卡)。模式:自动 = 插件直读\n"
              "mpv 窗口客户区(= osd-dimensions)按实际显示矩形适配并 clamp 显示器,\n"
              "1:1/缩小时自动旁路(VSR 只做放大);手动 = 按倍率放大。\n"
              "开关/模式/倍率为创建时参数:变化自动触发 mpv 原地重载(需 IPC)。\n"
              "需插件 ngx\\ 下有 nvngx_vsr.dll(RTX Video SDK 1.1)。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
    {
        bool vsrOn = g_app.params.rtxVsrMode != 0;
        if (ImGui::Checkbox("##rtx_vsr_enabled", &vsrOn)) {
            g_app.params.rtxVsrMode = vsrOn ? (g_app.params.rtxVsrMode == 0 ? 1 : g_app.params.rtxVsrMode) : 0;
            g_app.liveDirty = true;
            // 槽资源几何(vsrColor/yuvOut 尺寸/输出格式)随创建定格,开关
            // 真变化只能链重建 —— 与帧生成开关同款 reseek 语义。
            g_app.reseekDirty = true;
        }
    }
    pairLabel(1, "VSR 质量", "AI 档位 1-4(4 = 最优,耗时最长;1 = 最快)。per-eval,拖动下一帧生效。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + (halfW + colGap) + pairLabelW, wpos.y + y));
    ImGui::SetNextItemWidth(trackW);
    {
        const bool vsrOn = g_app.params.rtxVsrMode != 0;
        if (!vsrOn) ImGui::BeginDisabled(true);
        int q = std::clamp(g_app.params.rtxVsrStrength, kVsrStrengthMin, kVsrStrengthMax);
        if (ImGui::SliderInt("##vsr_strength", &q, kVsrStrengthMin, kVsrStrengthMax, "%d")) {
            g_app.params.rtxVsrStrength = q;
            g_app.liveDirty = true;
        }
        if (!vsrOn) ImGui::EndDisabled();
    }
    y += rowH;

    // (模式 | RTX Video HDR):模式与 HDR 开关都是创建时参数并排。
    pairLabel(0, "模式", "自动 = mpv 窗口客户区适配(窗口/全屏的显示矩形即目标,免配置);\n"
              "手动 = 按下方倍率放大(不跟随窗口)。切档触发 mpv 原地重载。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
    ImGui::SetNextItemWidth(trackW);
    {
        const bool vsrOn = g_app.params.rtxVsrMode != 0;
        if (!vsrOn) ImGui::BeginDisabled(true);
        static const char *kVsrModeNames[] = { "自动 (窗口适配)", "手动 (倍率)" };
        // 显示索引 = mode-1(mode 1=自动→item 0,2=手动→item 1;开态可达,
        // 无 0 档)。曾用原始 mode 作索引:自动(1) 显示成 items[1]="手动",
        // 点自动后下拉弹回"手动"(mode 实际已切对,纯显示错位,真机实锤)。
        int sel = std::clamp(g_app.params.rtxVsrMode, 1, kVsrModeMax) - 1;
        if (ImGui::Combo("##vsr_mode", &sel, kVsrModeNames, 2)) {
            g_app.params.rtxVsrMode = std::clamp(sel + 1, 1, kVsrModeMax);
            g_app.liveDirty = true;
            g_app.reseekDirty = true;
        }
        if (!vsrOn) ImGui::EndDisabled();
    }
    pairLabel(1, "RTX Video HDR", "TrueHDR(SDR → HDR10):输出切 YUV420P10(BT.2020 PQ),\n"
              "mpv 侧 target-colorspace-hint 上屏。官方明文必须排在 VSR 之后\n"
              "(VSR 不吃 HDR 输入),管线顺序 NR→VSR→HDR→FG。\n"
              "开关为创建时参数:变化自动触发 mpv 原地重载。\n"
              "需插件 ngx\\ 下有 nvngx_truehdr.dll(RTX Video SDK 1.1)。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + (halfW + colGap) + pairLabelW, wpos.y + y));
    {
        bool hdr = g_app.params.rtxHdrEnabled != 0;
        if (ImGui::Checkbox("##rtx_hdr_enabled", &hdr)) {
            g_app.params.rtxHdrEnabled = hdr ? 1 : 0;
            g_app.liveDirty = true;
            g_app.reseekDirty = true; // 输出格式(P8/P10)随创建定格
        }
    }
    y += rowH;

    // (放大倍率 | HDR 对比度):倍率为创建时参数(松手才推送,免拖动连环
    // 重载),模式=手动时启用;对比度为 per-eval live。
    pairLabel(0, "放大倍率", "手动模式的目标倍率(1.0-4.0,官方单 pass 上限 4x)。\n"
              "改动松手后自动触发 mpv 原地重载(免拖动过程连环重建)。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
    {
        const bool manual = g_app.params.rtxVsrMode == 2;
        if (!manual) ImGui::BeginDisabled(true);
        float sc = std::clamp(g_app.params.rtxVsrScale, kVsrScaleMin, kVsrScaleMax);
        ImGui::SetNextItemWidth(trackW);
        if (ImGui::SliderFloat("##vsr_scale", &sc, kVsrScaleMin, kVsrScaleMax, "%.2fx")) {
            g_app.params.rtxVsrScale = sc;
            // 松手才推送:拖动过程连续重载会连环重建链(与分辨率缩放 %滑杆
            // 同款 debounce)。
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                g_app.liveDirty = true;
                g_app.reseekDirty = true;
            }
        }
        if (!manual) ImGui::EndDisabled();
    }
    pairLabel(1, "HDR 对比度", "TrueHDR Contrast(0-200,默认 100):明暗差强度。per-eval,拖动下一帧生效。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + (halfW + colGap) + pairLabelW, wpos.y + y));
    {
        const bool hdrOn = g_app.params.rtxHdrEnabled != 0;
        if (!hdrOn) ImGui::BeginDisabled(true);
        float v = static_cast<float>(std::clamp(g_app.params.rtxHdrContrast, kHdrContrastMin, kHdrContrastMax));
        ImGui::SetNextItemWidth(trackW);
        if (ImGui::SliderFloat("##hdr_contrast", &v,
                               static_cast<float>(kHdrContrastMin), static_cast<float>(kHdrContrastMax), "%.0f")) {
            g_app.params.rtxHdrContrast = static_cast<int>(v + 0.5f);
            g_app.liveDirty = true;
        }
        if (!hdrOn) ImGui::EndDisabled();
    }
    y += rowH;

    // (HDR 饱和度 | HDR 中间灰)
    pairLabel(0, "HDR 饱和度", "TrueHDR Saturation(0-200,默认 100):色彩强度。per-eval。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
    {
        const bool hdrOn = g_app.params.rtxHdrEnabled != 0;
        if (!hdrOn) ImGui::BeginDisabled(true);
        float v = static_cast<float>(std::clamp(g_app.params.rtxHdrSaturation, kHdrSaturationMin, kHdrSaturationMax));
        ImGui::SetNextItemWidth(trackW);
        if (ImGui::SliderFloat("##hdr_saturation", &v,
                               static_cast<float>(kHdrSaturationMin), static_cast<float>(kHdrSaturationMax), "%.0f")) {
            g_app.params.rtxHdrSaturation = static_cast<int>(v + 0.5f);
            g_app.liveDirty = true;
        }
        if (!hdrOn) ImGui::EndDisabled();
    }
    pairLabel(1, "HDR 中间灰", "TrueHDR MiddleGray(10-100,默认 50):平均亮度。per-eval。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + (halfW + colGap) + pairLabelW, wpos.y + y));
    {
        const bool hdrOn = g_app.params.rtxHdrEnabled != 0;
        if (!hdrOn) ImGui::BeginDisabled(true);
        float v = static_cast<float>(std::clamp(g_app.params.rtxHdrMiddleGray, kHdrMiddleGrayMin, kHdrMiddleGrayMax));
        ImGui::SetNextItemWidth(trackW);
        if (ImGui::SliderFloat("##hdr_middle_gray", &v,
                               static_cast<float>(kHdrMiddleGrayMin), static_cast<float>(kHdrMiddleGrayMax), "%.0f")) {
            g_app.params.rtxHdrMiddleGray = static_cast<int>(v + 0.5f);
            g_app.liveDirty = true;
        }
        if (!hdrOn) ImGui::EndDisabled();
    }
    y += rowH;

    // HDR 峰值亮度(整行;HDR 关时置灰)+ 跨页提示
    pairLabel(0, "HDR 峰值亮度", "TrueHDR MaxLuminance(400-2000 nits,默认 1000):显示器峰值亮度,\n"
              "与显示器的实际峰值一致时色调映射最准。per-eval。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
    {
        const bool hdrOn = g_app.params.rtxHdrEnabled != 0;
        if (!hdrOn) ImGui::BeginDisabled(true);
        int v = std::clamp(g_app.params.rtxHdrMaxLuminance, kHdrMaxLumMin, kHdrMaxLumMax);
        ImGui::SetNextItemWidth(trackW);
        if (ImGui::SliderInt("##hdr_peak_nits", &v, kHdrMaxLumMin, kHdrMaxLumMax, "%d nits")) {
            g_app.params.rtxHdrMaxLuminance = v;
            g_app.liveDirty = true;
        }
        if (!hdrOn) ImGui::EndDisabled();
    }
    y += rowH;

            // 跨页依赖提示
            ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));
            ImGui::TextDisabled("提示: 实际生效状态见\"诊断\"页;HDR 上屏需 mpv.conf target-colorspace-hint=true。");
            y += rowH;

            ImGui::EndTabItem();
        }

        // --- 页:诊断(运行时事实 + 排队细分)。timing log 里才有的次级
        // 数据搬到这里;"请求 vs 实际"不一致的行一律红 —— 全页无红 = 插件
        // 正常工作。数据与 dlssnr_timing.log 的 perf/STATUS 行同源。---
        if (ImGui::BeginTabItem("诊断", nullptr,
                                (g_app.pageRestore && g_app.page == 3)
                                    ? ImGuiTabItemFlags_SetSelected
                                    : ImGuiTabItemFlags_None)) {
            pgDiagVis = true;
            if (!g_app.pageRestore && g_app.page != 3) savePagePref(3);
            y = ImGui::GetCursorPosY() - wpos.y + 6 * s;
            ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));

            // ---- 调试与日志(从降噪增强页/全局行迁入:监控与诊断类控件
            // 集中在本页,调参页只留调参控件)----
            ImGui::TextUnformatted("—— 调试与日志 ——");
            {
                ImGui::TextUnformatted("调试视图");
                if (ImGui::IsItemHovered())
                    ShowTip("输出替换为调试图,仅当前会话有效,不写入保存设置:\n"
                            "差异 ×20 = |NR改动|×20 灰度:白 = 改动大,一片灰 = 模型没动画面。\n"
                            "光流场 = 运动可视化:方向→色相(红=右/绿=下/青=左/紫=上),亮度=速度,\n"
                            "一片黑 = 无运动数据(光流关/播种帧)。确认光流是否真的在流。");
                ImGui::SameLine(0, 8 * s);
                // 三态:关 / 差异 ×20 / 光流场(live,不持久化)
                {
                    const int items = 3;
                    const char *labels[items] = { "关", "差异 ×20", "光流场" };
                    int sel = std::clamp(g_app.params.debugView, 0, kDebugViewMax);
                    ImGui::SetNextItemWidth(trackW);
                    if (ImGui::Combo("##debug_view", &sel, labels, items)) {
                        g_app.params.debugView = sel;
                        g_app.liveDirty = true;
                    }
                }
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
                if (ImGui::IsItemHovered())
                    ShowTip("每秒追加一行性能统计到 mpv 同目录 dlssnr_timing.log;排查性能问题时附上该文件。");
            }

            ImGui::Spacing();
            // ---- 会话事实:请求 vs 实际(不一致 = 红)----
            ImGui::TextUnformatted("—— 会话 ——");
            {
                char stDesc[96] = "未加载滤镜";
                bool stRed = false;
                if (g_app.gpuName[0]) {
                    if (std::strcmp(g_app.filterState, "passthrough") == 0) {
                        const bool deliberate =
                            std::strncmp(g_app.stateDetail, "NR off", 6) == 0 ||
                            std::strncmp(g_app.stateDetail, "NR+FG disabled", 14) == 0;
                        std::snprintf(stDesc, sizeof(stDesc), "直通(画面未增强)%s%s",
                                      g_app.stateDetail[0] ? ": " : "", g_app.stateDetail);
                        stRed = !deliberate;
                    } else if (std::strcmp(g_app.filterState, "ngx_faulted") == 0) {
                        std::snprintf(stDesc, sizeof(stDesc), "NGX 故障已停用%s%s",
                                      g_app.stateDetail[0] ? ": " : "", g_app.stateDetail);
                        stRed = true;
                    } else if (std::strcmp(g_app.filterState, "nvof_zero") == 0) {
                        std::snprintf(stDesc, sizeof(stDesc), "增强中,光流零 guidance");
                        stRed = true;
                    } else {
                        // ok / 空(存活态由周期 tick 携带)
                        std::snprintf(stDesc, sizeof(stDesc), "增强中(NGX 推理运行)%s",
                                      g_app.params.nrEnabled ? "" : ";降噪已关(NR off,跳过评估)");
                    }
                }
                ImGui::TextColored(stRed ? kErrRed : kDimTxt, "滤镜状态: %s", stDesc);

                // 光流请求 vs 实际
                char ofReq[64];
                if (g_app.params.ofBackend == kOfBackendFfx) {
                    std::snprintf(ofReq, sizeof(ofReq), "FFX %s",
                                  g_app.params.ffxQuality == 0 ? "无"
                                  : g_app.params.ffxQuality == 1 ? "性能(1/2 分辨率)" : "质量(全分辨率)");
                } else {
                    std::snprintf(ofReq, sizeof(ofReq), "NVOF 质量 %d", g_app.params.motionVectorQuality);
                }
                const int ofqReq = std::clamp(g_app.params.ofBackend == kOfBackendFfx
                                                  ? g_app.params.ffxQuality
                                                  : g_app.params.motionVectorQuality,
                                              0, kOfQualityMax);
                const bool ofBroken = std::strcmp(g_app.ofMode, "zero") == 0 && ofqReq > 0;
                ImGui::TextColored(ofBroken ? kErrRed : kDimTxt, "光流: 请求 %s | 实际 %s%s%s",
                                   ofReq, g_app.ofMode[0] ? g_app.ofMode : "(未加载)",
                                   g_app.ofDetail[0] ? " —— " : "", g_app.ofDetail);

                // FG 请求 vs 实际路由
                char fgReq[96];
                if (!g_app.params.fgEnabled) {
                    std::snprintf(fgReq, sizeof(fgReq), "关");
                } else {
                    static const char *kRouteNames[] = { "自动", "纯官方" };
                    std::snprintf(fgReq, sizeof(fgReq), "开 %dx,路由 %s",
                                  g_app.params.fgMultiplier,
                                  kRouteNames[std::clamp(g_app.params.fgRoute, kFgRouteMin, kFgRouteMax)]);
                }
                const bool fgBroken = g_app.params.fgEnabled != 0 &&
                                      (std::strcmp(g_app.fgState, "unavailable") == 0 ||
                                       std::strcmp(g_app.fgRouteEff, "copy") == 0);
                char fgEff[160];
                if (!g_app.fgRouteEff[0]) {
                    std::snprintf(fgEff, sizeof(fgEff), "(未加载)");
                } else if (std::strcmp(g_app.fgRouteEff, "off") == 0) {
                    std::snprintf(fgEff, sizeof(fgEff), "未开启");
                } else {
                    const char *rl = FgRouteLabel(g_app.fgRouteEff);
                    std::snprintf(fgEff, sizeof(fgEff), "%s%s%s", rl[0] ? rl : g_app.fgRouteEff,
                                  g_app.fgDetail[0] ? " —— " : "", g_app.fgDetail);
                }
                ImGui::TextColored(fgBroken ? kErrRed : kDimTxt, "帧生成: 请求 %s | 实际 %s",
                                   fgReq, fgEff);

                // 创建倍数 vs live 倍数 vs 运行库上限:上限 < 创建值 = gate
                // 解锁失败回落(输出按高倍率节拍,超出槽位全是复制真实帧)
                // —— 此前这一事实无任何 stats 键,面板全绿,用户毫无感知。
                if (g_app.params.fgEnabled && g_app.fgMultCreate > 0) {
                    const bool multClipped = g_app.params.fgMultiplier > g_app.fgMultCreate;
                    const bool capped = g_app.fgMultMax >= 1 &&
                                        g_app.fgMultMax < g_app.fgMultCreate;
                    if (capped) {
                        ImGui::TextColored(
                            kErrRed,
                            "FG 倍数: 会话创建 %dx | 面板 %dx | 运行库上限 %dx"
                            "(实效 %dx,超出槽位为复制帧 —— gate 解锁失败?驱动更新后重试)",
                            g_app.fgMultCreate, g_app.params.fgMultiplier,
                            g_app.fgMultMax, g_app.fgMultMax + 1);
                    } else {
                        ImGui::TextColored(multClipped ? kErrRed : kDimTxt,
                                           "FG 倍数: 会话创建 %dx | 面板 %dx%s",
                                           g_app.fgMultCreate, g_app.params.fgMultiplier,
                                           multClipped ? "(超出部分需重载生效)" : "");
                    }
                }

                // RTX 请求 vs 实际:请求开(vsrMode>0 / hdrEnabled)而实态
                // off = 降级(capability/部署问题),原因串 rtx_detail 红显。
                {
                    const bool vsrReq = g_app.params.rtxVsrMode != 0;
                    const bool hdrReq = g_app.params.rtxHdrEnabled != 0;
                    char rtxReq[64];
                    if (!vsrReq && !hdrReq) {
                        std::snprintf(rtxReq, sizeof(rtxReq), "关");
                    } else if (vsrReq && hdrReq) {
                        std::snprintf(rtxReq, sizeof(rtxReq), "VSR + HDR");
                    } else {
                        std::snprintf(rtxReq, sizeof(rtxReq), "%s", vsrReq ? "VSR" : "HDR");
                    }
                    const bool rtxDown = (vsrReq || hdrReq) &&
                                         std::strcmp(g_app.rtxState, "off") == 0;
                    ImGui::TextColored(rtxDown ? kErrRed : kDimTxt,
                                       "RTX Video: 请求 %s | 实际 %s%s%s",
                                       rtxReq,
                                       g_app.rtxState[0] ? g_app.rtxState : "(未加载)",
                                       g_app.rtxDetail[0] ? " —— " : "", g_app.rtxDetail);
                }
            }
            // 处理分辨率(读 stats 缓存的六段之外字段:LoadStats 已存在
            // internal_w/h 到 statsRes 展示串;此处直接从共享内存的解析结果
            // 复述 —— 分辨率信息主面板已有,诊断页仅回显会话事实)
            ImGui::TextDisabled("分辨率: %s", g_app.statsRes[0] ? g_app.statsRes : "(未加载)");

            ImGui::Spacing();
            ImGui::TextUnformatted("—— 排队细分(每帧 last)——");
            ImGui::Text("%s: %.2f ms", "槽池等待", g_app.slotWait);
            if (ImGui::IsItemHovered())
                ShowTip("3 槽全在飞时帧线程等槽的时长。此值大而 GPU 段正常 = GPU 超容量;\n两者都小而帧率低 = 宿主侧没来帧(与 perf 行 slot= 同源)。");
            ImGui::Text("%s: %.2f ms", "NGX 串行等待", g_app.lockWait);
            if (ImGui::IsItemHovered())
                ShowTip("evaluate 互斥排队(NGX feature 单例的 CPU 侧串行)。并发槽互相\n等待的时长;与 eval_cpu 互斥等待部分重叠,perf 行 lock= 同源。");
            ImGui::Text("光流门: 跳帧 %d / 过期 %d / 重置 %d", g_app.gateSkips, g_app.gateExpired, g_app.gateResets);
            if (ImGui::IsItemHovered())
                ShowTip("光流帧序门累计:跳帧 = 缺口超时播种,过期 = 迟到帧播种,重置 =\nseek/显式复位。seek 后重置数增加属正常;持续增长 = 时序异常\n(与 perf 行 s/x/r 同源)。FFX 后端无引擎探针,数值恒 0。");

            ImGui::Spacing();
            ImGui::TextDisabled("全页无红 = 插件正常工作。红色 = 与面板请求不一致。\n"
                                "本页数据与 dlssnr_timing.log 的 perf/STATUS 行同源,更细的\n"
                                "历史(EMA/p99/门细分)仍以日志为准。");
            y = ImGui::GetCursorPosY() - wpos.y + 4 * s;

            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        // 恢复完成判定:期望页真正可见才解除恢复锁;预算烧完则放弃(防 ini
        // 值异常时锁死)。恢复期每帧重喂 SetSelected(首帧排队、次帧布局落
        // 地),并配合主循环的强制重绘 —— 空闲门不再把窗口冻在切换前旧帧。
        if (g_app.pageRestore) {
            const bool targetVis =
                (g_app.page == 0 && pgNrVis) || (g_app.page == 1 && pgFgVis) ||
                (g_app.page == 2 && pgRtxVis) || (g_app.page == 3 && pgDiagVis);
            if (targetVis || --g_app.restoreFrames <= 0)
                g_app.pageRestore = false;
        }
    }
    y += 8 * s;

    // --- 全局行:保存/重置(性能日志开关已迁往"诊断"页)---
    dl->AddLine(ImVec2(wpos.x + marginX, wpos.y + y), ImVec2(wpos.x + wsize.x - marginX, wpos.y + y), IM_COL32(58, 62, 78, 255));
    y += 14 * s;

    y += 8 * s;
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));
    if (ImGui::Button("保存设置", ImVec2(120 * s, 30 * s))) {
        // 失败不再假报成功:ini 被占用/只读时状态栏红字说明,否则用户
        // 看到"已保存",重启全部回旧值(dlssnr_ini.h 注释预言的场景)。
        if (WriteIniNow()) {
            WritePayload(true); // persist-through-bridge flag included
            snprintf(g_app.status, sizeof(g_app.status), "已保存");
        } else {
            snprintf(g_app.status, sizeof(g_app.status), "保存失败(文件被占用或不可写)");
        }
    }
    if (ImGui::IsItemHovered()) {
        ShowTip("当前设置即时生效(下一帧画面);保存为默认值(dlssnr_ui.ini)后,下次加载滤镜自动生效。");
    }
    ImGui::SameLine(0, 14 * s);
    if (ImGui::Button("重置默认", ImVec2(120 * s, 30 * s))) {
        // Reset = push the factory-default payload itself; the plugin applies
        // it through the same Request*/Update path as any other edit (there
        // is no separate reset command in the protocol).
        const DlssnrParams beforeReset = g_app.params;
        g_app.params = DlssnrParams{};
        WritePayload();
        // RTX Video 开关(mode/HDR)是创建时参数:重置真关掉了它们才触发
        // 链重建(与开关控件同款 reseek 语义;本来就没开时不付一次 seek)。
        if (beforeReset.rtxVsrMode != 0 || beforeReset.rtxHdrEnabled != 0) {
            g_app.reseekDirty = true;
        }
        // Optimized 档位不在 payload 里(插件不消费):随重置归 1 并写回
        // 代理 ini,与面板显示保持一致。写失败保持现值 + 状态栏说明
        // (与档位下拉同款语义:UI 不说谎)。
        if (WriteFgOptimizedIni(1)) {
            g_app.fgOptimized = 1;
            snprintf(g_app.status, sizeof(g_app.status), "已重置");
        } else {
            snprintf(g_app.status, sizeof(g_app.status),
                     "参数已重置;内核档位写入失败(代理配置缺失或被占用)");
        }
    }
    ImGui::SameLine(0, 16 * s);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6 * s);
    if (g_app.status[0]) ImGui::TextUnformatted(g_app.status);
    y += 38 * s;

    // --- 处理用时(窗口最底部):分段行随帧出现/消失时,高度变化只影响
    // 底部收口,上方的页签、参数与按钮保持静止(不再快速跳动)。---
    dl->AddLine(ImVec2(wpos.x + marginX, wpos.y + y), ImVec2(wpos.x + wsize.x - marginX, wpos.y + y), IM_COL32(58, 62, 78, 255));
    y += 14 * s;
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));
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

    // 高度收口:按内容实际底部调整窗口(WS_POPUP 尺寸 = client 尺寸)
    y = ImGui::GetCursorPosY() - wpos.y + 10 * s;
    const float needH = y + 8 * s;
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
            // create-time 变动(vsrMode/FG 档位等)经 reseek 重建会话后面板
            // 会重启,但重启面板在启动时采纳最后一份 payload(CreateParams
            // Mapping 的 adopt 路径,含 rtx 三元组,后于 ini 生效)—— 切换
            // 值不落盘也能跨面板重启保持;ini 仍只由"保存设置"显式写入
            // (插件侧 saveRequest 落盘带 LoadCreateParams 叠加,见 bridge)。
            WritePayload();
            g_app.liveDirty = false;
            g_app.lastLiveWrite = nowSec;
            // 需要重建会话的变动:payload 落地后立即触发 mpv 原地 seek,
            // 重建的滤镜实例即采纳新值(免手动拖进度条)。
            if (g_app.reseekDirty) {
                g_app.reseekDirty = false;
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
        // 页签恢复期例外:恢复需要连续渲染帧推进 ImGui 的布局状态,空闲门
        // 会把窗口冻在恢复完成前的旧帧上(启动页签显示错误的根因之一)。
        if (activity || g_app.liveDirty || g_app.statsDirty || g_app.pageRestore) {
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
