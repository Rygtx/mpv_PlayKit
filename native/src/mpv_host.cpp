// mpv 宿主探测与 resize 跟随实现(契约见 mpv_host.h;2026-10-04 自
// plugin.cpp 拆出)。
#include "mpv_host.h"
#include "mpv_pipe_common.h"
#include "status_line.h" // TimingStatusLine(前置声明收拢件)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>

namespace vsdlssnr {

// ---- RTX Video:目标尺寸的"跟随播放器"落点 ----
// 探测本进程 mpv vo 窗口:**客户区尺寸 = mpv 的 osd-dimensions 等价**
// (插件 DLL 活在 mpv 进程内,直读窗口零桥接),显示器原生分辨率仅作
// 上限 clamp(窗口理论上不会大于所在屏)。链创建粒度:mpv 在 seek/换片
// 时重建整条 VS 链 → 重新探测;窗口换屏/改尺寸后的生效点是下一次链重建
// (VS constant-format 契约,每帧跟随在 mpv 架构下不存在落点)。
// 探测失败(无窗口/枚举失败)回落主显示器。
MpvDisplayPick MpvDetectTargetSize(int srcW, int srcH) noexcept {
    struct Ctx {
        DWORD pid;
        HWND hwnd;
        LONG area;
    } ctx{ GetCurrentProcessId(), nullptr, 0 };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<Ctx *>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != c->pid || !IsWindowVisible(hwnd)) return TRUE;
        // mpv vo 窗口 = 本进程可见的主窗口;排除工具窗/无边框辅助窗
        //(面板另有进程)。
        const LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
        if (exStyle & WS_EX_TOOLWINDOW) return TRUE;
        RECT rc{};
        if (!GetWindowRect(hwnd, &rc)) return TRUE;
        const LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
        if (area > c->area) {
            c->area = area;
            c->hwnd = hwnd;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    // 找不到可见窗口(无窗音频/全屏独占被排除等)= 探测失败,返回 {0,0}
    // 由调用方按 VSR 旁路处理;窗口出现后的链重建会重新探到并启用。
    // 与显示器无关:有窗口时客户区即实际显示大小(宿主 per-monitor DPI
    // aware,物理像素),无窗口时无目标 —— 不存在"拿显示器凑"的场景。
    if (!ctx.hwnd) return { 0, 0 };
    int w = 0, h = 0;
    // 最小化窗口的 GetClientRect 是任务栏代表尺寸(~152x20,低于源)——
    // 不做特例:探测目标低于源 = 旁路,正是"低于源分辨率直通"的设计语义;
    // 还原时 resize watcher 探到高度变化,自动 seek 重建回全尺寸目标。
    RECT rc{};
    if (GetClientRect(ctx.hwnd, &rc)) {
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
    }
    if (w <= 0 || h <= 0) return { 0, 0 };
    // 视频实际显示矩形 = 源宽高比在客户区内 min-fit(mpv letterbox 语义;
    // panscan/zoom 覆盖不追,近似足够)—— 黑边不计入目标,避免"宽窗放
    // 窄视频"时目标虚大。
    if (srcW > 0 && srcH > 0) {
        const double s = (std::min)(static_cast<double>(w) / srcW,
                                    static_cast<double>(h) / srcH);
        w = (std::max)(1, static_cast<int>(std::lround(srcW * s)));
        h = (std::max)(1, static_cast<int>(std::lround(srcH * s)));
    }
    return { w, h };
}

// ---- 窗口 resize 跟随(mode=1)----
// 窗口尺寸变化不触发 mpv 的 video-reconfig(纯 VO 事件),链不重建、
// 探测不重跑 —— VSR 目标停留在创建时尺寸,必须 seek 才跟随。轻量
// watcher:每 400ms 复用 DetectTargetSize 探一次,高度连续两拍稳定偏离
// 创建值(迟滞 max(8px, 2%))后经 mpv IPC 发一次 1ms seek 触发链重建;
// 重建出的新实例带新探测值与新 watcher,本线程随即退出(存活事件关闭
// = 滤镜释放,同样退出)。IPC 缺失则优雅降级为旧行为(需手动 seek)。
namespace {

struct ResizeWatchCtx {
    int srcW, srcH, refH;
    HANDLE stop; // 匿名停事件(FilterData 持句柄,Free 时 SetEvent+Close)
};

std::atomic<int> g_resizeWatchers{ 0 };

bool ResizeWatchSendSeek() noexcept {
    // 管道名发现 = mpv.conf input-ipc-server 优先(与面板同源解析,
    // mpv_pipe_common;2026-10-04 补齐 —— 此前只试三个默认名,自定义管道
    // 名部署下面板 reseek 正常而本线程静默失效,双实现分叉的实害)。
    // conf 按 mpv.exe 目录(宿主进程可执行文件)定位。解析结果进程级缓存
    // (conf 中途改动需重启 mpv,与面板侧同语义)。
    // 候选列表/连接/写读 overlapped + 500ms 有界均已收口 mpv_pipe_common
    // (2026-10-05,与面板 reseek 同一份实现;原同步 ReadFile 无超时,mpv
    // 挂起时本线程永久滞留 —— watcher 名额上限 2,滞留 = 自动跟随静默失效)。
    const wchar_t *candidates[kMpvPipeMaxCandidates];
    MpvPipeDefaultNames(candidates);
    // conf 解析进程级缓存(magic static:watcher 名额上限 2,两线程可能
    // 并发首调 —— 手写 probed 旗标有初始化竞态 + 双解析泄漏一条,静态
    // 初始化一次性的语义正是这里的合同,2026-10-04 评审修)。
    static const wchar_t *g_confPipe = [] {
        wchar_t exe[MAX_PATH];
        if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return static_cast<const wchar_t *>(nullptr);
        const std::filesystem::path confPath =
            std::filesystem::path(exe).parent_path() / L"portable_config" / L"mpv.conf";
        return static_cast<const wchar_t *>(MpvParseIpcServerName(confPath.c_str()));
    }();
    candidates[0] = g_confPipe;
    static constexpr char kSeekCmd[] =
        "{\"command\":[\"seek\",\"0.001\",\"relative+exact\"]}\n";
    for (const wchar_t *name : candidates) {
        HANDLE pipe = MpvPipeOpen(&name, 1, /*overlapped=*/true, nullptr, 0);
        if (!pipe) continue;
        if (!MpvPipeSendBounded(pipe, kSeekCmd, sizeof(kSeekCmd) - 1)) {
            CloseHandle(pipe);
            continue;
        }
        CloseHandle(pipe);
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: resize reload seek via IPC pipe %ls", name);
        TimingStatusLine(msg);
        return true;
    }
    return false;
}

DWORD WINAPI ResizeWatchProc(LPVOID param) noexcept {
    std::unique_ptr<ResizeWatchCtx> ctx(static_cast<ResizeWatchCtx *>(param));
    int drift = 0;
    // 停靠匿名事件(2026-09-25):此前等 ALIVE_EVENT 的 WAIT_OBJECT_0 分支
    // 是死代码(该事件 manual-reset 初始非信号、全仓无 SetEvent),watcher
    // 只能靠自己触发 seek 或进程退出结束,期间 2.5Hz 全量 EnumWindows 空转,
    // 且线程生命周期不受实例释放约束。现在 FilterData 持停事件句柄,Free
    // 时 SetEvent —— 实例与线程生命周期对齐。
    for (;;) {
        const DWORD w = WaitForSingleObject(ctx->stop, 400);
        // 非 WAIT_TIMEOUT 一律停:SetEvent 是正常停旗;WAIT_FAILED = Free 的
        // CloseHandle 撞上扫描窗口(不在 wait 中),句柄已关,继续循环会
        // 全速空转(每圈全量 EnumWindows)。
        if (w != WAIT_TIMEOUT) break; // 滤镜已释放(重建/热停泊/关停)
        const MpvDisplayPick d = MpvDetectTargetSize(ctx->srcW, ctx->srcH);
        if (d.height <= 0) { drift = 0; continue; } // 窗口暂不可见,不积累
        const int thresh = (std::max)(8, ctx->refH / 50);
        if (std::abs(d.height - ctx->refH) <= thresh) { drift = 0; continue; }
        if (++drift < 2) continue; // 连续两拍稳定偏离才触发(拖动防抖)
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: window target %d -> %d, triggering reload",
                      ctx->refH, d.height);
        vsdlssnr::TimingStatusLine(msg);
        if (!ResizeWatchSendSeek()) {
            vsdlssnr::TimingStatusLine("DLSSNR STATUS: resize watch: mpv IPC unavailable, auto-follow off");
        }
        break; // 重建带来新探测值与新 watcher
    }
    g_resizeWatchers.fetch_sub(1, std::memory_order_relaxed);
    return 0;
}

} // namespace

HANDLE MpvResizeWatchStart(int srcW, int srcH, int refH, bool vsrAutoMode) noexcept {
    if (!vsrAutoMode || g_resizeWatchers.load(std::memory_order_relaxed) >= 2) return nullptr;
    HANDLE stop = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset 停旗
    if (!stop) return nullptr;
    g_resizeWatchers.fetch_add(1, std::memory_order_relaxed);
    // nothrow new:本函数 noexcept,抛式 new 的分配失败会变 std::terminate
    // 杀掉整个 mpv 进程 —— 按本函数既有契约优雅降级(nullptr = 跟随关闭)。
    auto *ctx = new (std::nothrow) ResizeWatchCtx{ srcW, srcH, refH, stop };
    if (!ctx) {
        CloseHandle(stop);
        g_resizeWatchers.fetch_sub(1, std::memory_order_relaxed);
        return nullptr;
    }
    HANDLE th = CreateThread(nullptr, 0, ResizeWatchProc, ctx, 0, nullptr);
    if (th) {
        CloseHandle(th);
        return stop; // 句柄归调用方,Free 时 MpvResizeWatchStop
    }
    delete ctx;
    CloseHandle(stop);
    g_resizeWatchers.fetch_sub(1, std::memory_order_relaxed);
    return nullptr;
}

void MpvResizeWatchStop(HANDLE stop) noexcept {
    if (!stop) return;
    SetEvent(stop);
    CloseHandle(stop);
}

} // namespace vsdlssnr
