#pragma once
// dlssnr 面板跨编译单元共享面(2026-10-03 拆分):
//   panel_app.cpp     —— 进程生命周期/Win32/ImGui bootstrap/ini/参数通道
//   panel_mpv_ipc.cpp —— mpv 管道:一次性命令 + HDR 打标持久连接
//   panel_stats.cpp   —— stats 共享内存消费(视图缓存/快照/字段直读)
//   panel_ui.cpp      —— 全部 ImGui(DrawUi + 四页签)
// AppState 平凡可拷贝(LoadStats 的重画门按整结构 memcmp),跨文件传递
// 不改变该前提。using namespace 沿用原 panel_app.cpp 的全局形态(应用
// 目标,非库)。
#include "dlssnr_params.h"
#include "panel_ipc.h"

#include <windows.h>
#include <atomic>

using namespace vsdlssnr;

struct ImFont;

// 进程退出旗标:主循环(WM_QUIT)、看门狗与 HDR 打标线程轮询同一位。
extern std::atomic<bool> g_quit;
// 源传输函数探针(HdrTagProc worker 读 mpv video-params/gamma;-1 = 尚未
// 读到,0/1 = 非 HDR/HDR)。主循环发现变化时折叠进 payload 并触发 reseek
// —— v30 payload srcHdr 的数据源。写侧仅 worker,读侧主线程。
extern std::atomic<int> g_srcGammaProbe;
struct AppState {
    DlssnrParams params{};
    // stats 通道整快照(2026-10-04 快照化:此前 25+ 字段逐个镜像 StatsPayload
    // 并配三份手抄清单(直读/Clear/snap==0 判定),v28 加字段时清单已失序
    // 一次 —— 整拷 = "body 未携带即发布侧零值"语义天然成立,新字段零接线)。
    // UI/闭环兜底直读本快照;字段语义见 panel_ipc.h StatsPayload 注释。
    StatsPayload snap{};
    int lastVsrMode = 1; // 最近一次非零 VSR 模式(1=自动 2=手动):取消勾选
                         // 后重新勾选恢复它,而不是恒落回自动 —— 恒落回曾把
                         // "手动倍率"选择静默丢弃,勾回后 VSR 在 4K 窗口旁路
                         // 而用户以为还开着(2026-09-26 真机实锤)。
    float uiScale = 1.0f;
    int dpi = 96;
    bool liveDirty = false;
    bool reseekDirty = false; // 需要重建会话的变动(FG 档位/开、全关后开 NR):
                              // flush 时 WritePayload 后自动触发 mpv 原地 seek
    bool timingLog = true;
    bool advancedOpen = false; // 残差精调折叠区(ini [panel] advanced 记忆)
    int page = 0;              // 功能页签:0=神经渲染 1=帧生成 2=RTX 超分/HDR
                               // 3=诊断(ini [panel] page 记忆)
    bool pageRestore = true;   // 页签启动恢复锁:恢复期内每帧重喂 SetSelected 并
                               // 强制重绘,直到期望页真正可见(或预算烧完)才解除。
                               // 只喂首帧不够 —— SetSelected 排队到下一帧布局才
                               // 落地,期间空闲门可能不再重绘,恢复静默失败且首帧
                               // 可见的旧页会把 ini 无痕改写。恢复期内抑制页签落盘。
    int restoreFrames = 8;     // 恢复期重绘预算(帧);只在窗口可见时消耗
    double lastLiveWrite = 0.0;
    double lastReseekWrite = 0.0; // reseek 独立节流戳(不挂 liveDirty)
    double lastStatsRead = 0.0;
    double lastSeekInitReseek = 0.0; // kStateNrSeekInit 兜底 reseek 去抖
    int lastSrcProbe = -1;           // 已折叠进 payload 的探针值(-1 未定;
                                     // worker 读数与主循环消费的比对基准)
    char status[160]{};
    char statsBig[64]{};
    char statsRes[96]{};
    int connState = 0;       // stats 通道连接态:0=未检测到插件 1=已连接
                             // 2=magic 不匹配(面板/插件版本未成对更新)
    int fgOptimized = 1;     // dlssg_for_sm86 [FrameGeneration] Optimized 0-3
                             // (存储单点 = 代理 ini;面板启动回读,重启 mpv 生效)
    // 分段显示值(EMA 平滑,用户裁定"显示平滑、真实数据不平滑"):每拍从
    // 插件上报的裸 last 值就地喂 EMA;时间线/列表/tooltip 用平滑值,避免
    // CPU 唤醒竞争造成的瞬时 0 让段忽隐忽现。
    float segDispPack = 0.0f, segDispEval = 0.0f, segDispGpu = 0.0f, segDispUnpack = 0.0f,
          segDispNvof = 0.0f, segDispFg = 0.0f, segDispRtxVsr = 0.0f, segDispRtxHdr = 0.0f,
          segDispConv = 0.0f, segDispQueue = 0.0f;
    bool hasSegments = false;
    bool statsDirty = false; // LoadStats changed something on screen (redraw gate)
};
extern AppState g_app;
extern HWND g_hwnd;
extern DWORD g_mainThreadId;
extern ImFont *g_fontUI;
extern ImFont *g_fontMono;

// 字号/间距整体缩小一档(用户偏好:面板在小屏也放得下);1.0 = 跟随 DPI 原尺寸
inline constexpr float kUiFontScale = 0.80f;
// 自绘标题栏几何(96dpi 基准,乘 uiScale)——DrawUi 画与 WM_NCHITTEST 判定
// 共用同一组常量,改动不会留下一条拖不动的死区。
inline constexpr float kTitleBarH = 38.0f;
inline constexpr float kCloseBtnSize = 30.0f;
inline constexpr float kCloseBtnPad = 10.0f;
// 窗宽(96dpi 基准):648 = 2*18 + 2*(94+200) + 24,96dpi 下 518px。
inline constexpr float kPanelW = 648.0f;

// ---- panel_app.cpp ----
bool BasePath(wchar_t *path, size_t len) noexcept;
void PanelLog(const char *fmt, ...) noexcept;
bool WriteIniNow() noexcept;
bool WriteFgOptimizedIni(int v) noexcept;
void WritePayload() noexcept;

// ---- panel_mpv_ipc.cpp ----
bool TriggerMpvReseek() noexcept;
DWORD WINAPI HdrTagProc(LPVOID) noexcept; // HDR 打标线程入口(wWinMain 拉起)

// ---- panel_stats.cpp ----
// Stats 映射一拍快照(seq 门校验)。返回:2=有效(已拷入 *st);1=映射在
// 但快照未稳(seq 翻转中/未发布),*badMagic 带回 magic 是否失配;0=映射
// 不存在(插件未运行)。LoadStats(主线程)与 HDR 打标线程共用。
int ReadStatsSnapshot(StatsPayload *st, bool *badMagic) noexcept;
void ClearSessionState() noexcept;
void LoadStats() noexcept;

// ---- panel_ui.cpp ----
void DrawUi() noexcept;
