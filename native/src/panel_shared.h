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

using namespace vsdlssnr;

struct ImFont;

// 进程退出旗标:主循环(WM_QUIT)、看门狗与 HDR 打标线程轮询同一位。
extern bool g_quit;
struct AppState {
    DlssnrParams params{};
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
    int page = 0;              // 功能页签:0=神经渲染,1=帧生成(ini [panel] page 记忆)
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
    char status[160]{};
    char statsBig[64]{};
    char statsRes[96]{};
    char gpuName[128]{};
    char modelDll[64]{};     // StatsPayload.modelDll: 加载的模型 dll 文件名(诊断页)
    char filterState[16]{};  // StatsPayload.filterState: ok / nvof_zero / passthrough / ngx_faulted
    char stateDetail[208]{}; // StatsPayload.stateDetail: 死亡状态的原因串
                             // (生产端 %.200s 封顶,缓冲须容 200+NUL,见 plugin.cpp)
    char ofMode[40]{};       // StatsPayload.ofMode: off / zero / 后端能力串(最长
                             // "fxof q5 qual 1920x1080" = 23+1;与插件 _ofModeBuf 同尺寸)
    char fgState[16]{};      // StatsPayload.fgState: on / dup / off / unavailable
    int fgMult = 0;          // StatsPayload.fgMult: 当前插帧倍数(未激活 = 0)
    char fgRouteEff[16]{};   // StatsPayload.fgRouteEff: off/official-hook/official/copy
    int fgMultCreate = 0;    // StatsPayload.fgMultCreate: 会话创建倍数(FG 未激活 = 0)
    char fgDetail[128]{};    // StatsPayload.fgDetail: FG 最近一次初始化失败原因(成功 = 空)
    int fgMultMax = 0;       // StatsPayload.fgMultMax: 运行库插值帧上限(FG 未激活 = 0)。
                             // gate 解锁失败回落 2x 的唯一面板侧信号源
    char ofDetail[96]{};     // StatsPayload.ofDetail: 光流会话创建失败原因(成功 = 空)
    char rtxState[32]{};     // StatsPayload.rtx: off / vsr / hdr / vsr+hdr + 实际输出分辨率
                             // ("vsr+hdr 15360x8640" = 18+NUL;原 16 字节把分辨率
                             // 截成 "7680x43" —— 诊断页"实际"显示不全的根因)
    char rtxDetail[96]{};    // StatsPayload.rtxDetail: VSR/TrueHDR 最近失败原因(成功 = 空)
    int connState = 0;       // stats 通道连接态:0=未检测到插件 1=已连接
                             // 2=magic 不匹配(面板/插件版本未成对更新)
    int fgOptimized = 1;     // dlssg_for_sm86 [FrameGeneration] Optimized 0-3
                             // (存储单点 = 代理 ini;面板启动回读,重启 mpv 生效)
    float slotWait = 0.0f;   // StatsPayload.slotWait: 槽池等待 last(诊断页)
    float lockWait = 0.0f;   // StatsPayload.lockWait: evaluate 互斥等待 last(诊断页)
    int evalActive = 0;      // StatsPayload.evalActive: 本帧 NGX/NR 真评估(DSL9 实效位)
    int ofActive = 0;        // StatsPayload.ofActive: 本帧光流消费门开
    int scalingActive = 0;   // StatsPayload.scalingActive: 本帧内部缩放档真参与
    int gateSkips = 0;       // StatsPayload.gateSkips: 光流帧序门跳帧累计(诊断页)
    int gateExpired = 0;     // StatsPayload.gateExpired: 过期帧累计(诊断页)
    int gateResets = 0;      // StatsPayload.gateResets: 历史重置累计(诊断页)
    char temporalState[10]{}; // StatsPayload.temporal: off / seed / steady / failed(诊断页)
    int temporalRoute = 0;   // StatsPayload.temporalRoute: 实际生效档 0-4(诊断页)
    float temporalW = 0.0f;  // StatsPayload.temporalW: 最近一帧混合权重(诊断页)
    double fps = 0.0;
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
void WritePayload(bool saveRequest = false) noexcept;

// ---- panel_mpv_ipc.cpp ----
bool MpvIpcSendCmd(const char *cmd, wchar_t *hitOut, size_t hitLen) noexcept;
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
