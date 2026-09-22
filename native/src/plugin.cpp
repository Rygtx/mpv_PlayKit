// vs_dlssnr - NVIDIA DLSSNR (NGX Feature 18) filter for VapourSynth.
// Ported from Magpie experimental (github.com/SAOG0721/Magpie).
// Model file: nvngx_dlssnr.dll (310.9.0), zero-guidance mode (Magpie
// guidanceMode=1 Force Zero), same-resolution processing.

#include "bridge.h"
#include "d3d12_context.h"
#include "dlssnr_context.h"
#include "dlssnr_ini.h" // LoadRtxVideoIni([rtxvideo] 节)
#include "dlssnr_params.h"
#include "panel_ipc.h"
#include "shared_params.h"

#include "VapourSynth4.h"
#include "VSHelper4.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <windows.h>

using vsdlssnr::kFgGenSlots; // FG 插值槽数上界(d3d12_context.h,kFgMultMax-1)

namespace {

constexpr char PLUGIN_IDENTIFIER[] = "dev.rygtx.vsdlssnr";
constexpr char PLUGIN_NAMESPACE[] = "dlssnr";
constexpr char PLUGIN_NAME[] = "NVIDIA DLSSNR filter (Magpie port)";
constexpr char SNIPPET_DLL_NAME[] = "nvngx_dlssnr.dll";
constexpr char FG_PROXY_DLL_NAME[] = "version.dll"; // dlssg_for_sm86 0.3.x hook 型代理

// vpy explicit arguments overwrite the DlssnrParams member initializers —
// the struct defaults in dlssnr_params.h are the single authority, so a
// default change cannot drift between the vpy layer and the ini/panel layers.
// Missing keys leave the field untouched; present keys are clamped at the
// boundary against the same dlssnr_params.h range constants.
void ApplyIntArg(const VSMap *in, const VSAPI *vsapi, const char *key,
                 int &field, int lo, int hi) noexcept {
    int err = 0;
    const long long v = vsapi->mapGetInt(in, key, 0, &err);
    if (!err) field = static_cast<int>(std::clamp<long long>(v, lo, hi));
}

void ApplyFloatArg(const VSMap *in, const VSAPI *vsapi, const char *key,
                   float &field, float lo, float hi) noexcept {
    int err = 0;
    const double v = vsapi->mapGetFloat(in, key, 0, &err);
    if (!err) field = vsh::doubleToFloatS(std::clamp(v, static_cast<double>(lo), static_cast<double>(hi)));
}

void ApplyFlagArg(const VSMap *in, const VSAPI *vsapi, const char *key, int &field) noexcept {
    int err = 0;
    const long long v = vsapi->mapGetInt(in, key, 0, &err);
    if (!err) field = v != 0;
}

struct FilterData {
    VSNode *node = nullptr;
    // Runtime-mutable parameters shared with the tray panel (initial values
    // come from the .vpy call).
    std::unique_ptr<vsdlssnr::SharedParams> params;
    // RTX Video(VSR/TrueHDR)创建时参数(vpy args ← [rtxvideo] ini;面板
    // payload 不携带)。vsrAutoHeight 由显示器探测填入。
    RtxVideoParams rtx;

    // Eagerly initialized in DlssnrCreate (before playback starts); VS may
    // call getFrame on several threads under fmParallel, but each frame runs
    // on its own D3D12 slot, so no further locking is needed here. Both are
    // unique_ptr because they move into the process-level hot context on
    // Free and move back out on the next Create (mpv re-runs the whole VS
    // script on every seek; without the hot context that pays the ~1s
    // D3D12+NGX bring-up each time).
    std::unique_ptr<vsdlssnr::D3D12Context> d3d12;
    std::unique_ptr<vsdlssnr::DlssnrContext> ngx;
    std::wstring ngxDllPath;
    bool initOk = false;
    int width = 0;
    int height = 0;
    int depth = 0; // YUV 位深(8/10);同尺寸换深度必须走冷重建(hotMatch 拦截)
    // RTX Video 输出几何(init 后从 context 读回;输出帧按此建)。
    int outW = 0;
    int outH = 0;
    bool hdrOut = false;       // 输出 = P10 BT.2020 PQ(HDR props 随帧写)
    bool rtxActive = false;    // RTX 在管线(输出格式/尺寸可能 ≠ 源)
    VSVideoFormat outFi{};     // 输出帧格式(hdr → YUV420P10,否则沿用源)
    // Failure-log latch: a wedged context fails every frame at frame rate —
    // log the first failure only, re-arm on the next success.
    std::atomic<bool> failureLogged{ false };
    // NR 总开关 stats 发布边沿(-1 = 未发布):开关翻转时向面板发一次
    // filter_state 状态行(live 关 = 直通原因;live 开但未初始化 = 提示
    // 需要 seek)。开着且已初始化时不发布 —— 常规逐帧 stats 接管。
    std::atomic<int> nrPubState{ -1 };

    // ---- DLSS FG 多帧输出 ----
    // fgActive = 创建时 FG 激活(官方链初始化成功):每源帧产出 M0 帧
    // (M0 = fgCreateMult,创建时定格 —— 输出帧数/fps 元数据/每槽时长随之
    // 与 mpv 定约,会话内恒定)。输出索引 n → (源帧 k, 槽位) 为无状态闭式
    // 映射(见 DlssnrGetFrame):序列 = R0, [G(k-1,k)×(M0-1), R_k] ... ——
    // 处理源帧 k 产出的插值帧落在 k-1 与 k 之间(DLSS-G 语义;Magpie
    // Renderer::_CompleteBackendFrame 同款发布序:gen 先、真实帧后),
    // 显示在 R_{k-1} 之后、R_k 之前。旧版"real 在前 gen 在后"把每个插值
    // 帧晚放一个源帧位,运动画面持续前后抖动(用户实测)。
    // 会话内有效密度 = min(live 倍数, M0):live 关闭/降档即时生效(多余
    // 槽位回落真实帧引用);升档超过 M0 无槽可填,需下个 seek(重建即新
    // M0)。输出帧数/节奏契约因此恒定 —— 旧版随 live 倍数改组尺寸会撕裂
    // 契约(提前 EOF / 尾部冻结)。
    // 注:曾有按 n 单调推进的顺序状态机,实测 VS core(fmParallel)的评估
    // 启动顺序不保证单调(timing log "fg out-of-order" 行)—— 首个乱序
    // 请求即失位,此后全部请求走闭式回退且 fgM 冻结在创建值(live 倍数
    // 静默失效)。已删;闭式映射对请求顺序天然免疫。
    // fgMutex 只串行缓存与处理(同源帧一次处理、各槽从缓存出;FG 链的
    // NVOF/DLSSG 历史按处理序推进,帧序门自愈乱序)。
    std::mutex fgMutex;
    bool fgActive = false;
    std::wstring fgDllPath;
    int fgCreateMult = 2;                     // 创建时倍数(vi.fps/帧数元数据 + 组槽位结构)
    int srcFrames = 0;                        // 源帧数(vi.numFrames;0 = 未知,尾部钳制禁用)
    int fgCacheK = -1;                        // 缓存命中 = 同源帧的后继槽位请求
    int fgCacheM = 0;                         // 有效缓存条目上界(1 + 插值槽数;空位判 null)
    const VSFrame *fgCache[kFgMultMax] = {};  // [0]=真实帧 [1..]=插值帧;持引用
};

// Process-lifetime hot context. mpv's vf_vapoursynth tears down and
// re-creates the whole VS script on every seek; keeping the D3D12 device,
// NGX feature and slot pool alive across filter instances turns that from a
// ~1s reload into a near-free Rebind. Single-threaded by construction: VS
// serializes filter create/free, and the plugin is process-level single
// instance (IAT hook).
//
// Deliberately leaked (never destroyed): tearing the kept-warm context down
// from static teardown / DllMain(process exit) would run NGX shutdown under
// the loader lock and deadlock; the OS reclaims everything at exit anyway.
struct HotContext {
    std::unique_ptr<vsdlssnr::D3D12Context> d3d12;
    std::unique_ptr<vsdlssnr::DlssnrContext> ngx;
    std::wstring ngxDllPath;
    int width = 0;
    int height = 0;
    int depth = 0;
    // FG 状态参与 hotMatch:开关/代理路径变化 = 槽资源形态变化(FG 纹理
    // 有无),必须冷重建。路由(route)进程级,重启生效,不参与。
    bool fgEnabled = false;
    std::wstring fgDllPath;
    // RTX Video 参与 hotMatch:模式/目标/强度/HDR 参数变化 = 管线几何或
    // 输出格式变化,冷重建。
    RtxVideoParams rtx{};
    bool valid = false;
};
HotContext &Hot() {
    static HotContext *inst = new HotContext();
    return *inst;
}

} // namespace

// FG 多帧输出的时长语义:mpv 的 vf_vapoursynth 输出节奏只认输出帧 props
// 的整数对 _DurationNum/_DurationDen(喂数时按 pkt_duration*1e6 写入源帧,
// 收帧时读回算 nominal_fps;VS 的 vi.fps 只是元数据不参与节拍)。倍数 M 的
// 每个输出帧各占源帧 1/M:_DurationDen ×= M(分子不变)—— 同源帧 M 个输出
// 时长求和恒等于源时长,倍数变化不漂移。整数对缺失(非 mpv 宿主)时退
// 回浮点 _Duration(存在才写)。
static void ScaleOutputDuration(VSFrame *frame, const VSAPI *vsapi, int m) noexcept {
    VSMap *props = vsapi->getFramePropertiesRW(frame);
    if (!props) return;
    int err = 0;
    const int64_t num = vsapi->mapGetInt(props, "_DurationNum", 0, &err);
    int err2 = 0;
    const int64_t den = err ? 0 : vsapi->mapGetInt(props, "_DurationDen", 0, &err2);
    if (!err && !err2 && den > 0 && num > 0) {
        vsapi->mapSetInt(props, "_DurationNum", num, maReplace);
        vsapi->mapSetInt(props, "_DurationDen", den * m, maReplace);
        return;
    }
    err = 0;
    const double dur = vsapi->mapGetFloat(props, "_Duration", 0, &err);
    if (!err) {
        vsapi->mapSetFloat(props, "_Duration", dur / m, maReplace);
    }
}

// 从源帧复制三平面(YUV420)到 dst —— 帧失败的降级路径。
static void CopyPlanes(const VSFrame *src, VSFrame *dst, const VSAPI *vsapi,
                       int width, int height) noexcept {
    const VSVideoFormat *fi = vsapi->getVideoFrameFormat(dst);
    const int bpp = fi->bytesPerSample;
    const int cw = (width + 1) >> 1, ch = (height + 1) >> 1;
    for (int p = 0; p < 3; ++p) {
        const int pw = p == 0 ? width : cw;
        const int ph = p == 0 ? height : ch;
        vsh::bitblt(vsapi->getWritePtr(dst, p), vsapi->getStride(dst, p),
                    vsapi->getReadPtr(src, p), vsapi->getStride(src, p),
                    static_cast<size_t>(pw) * bpp, static_cast<size_t>(ph));
    }
}

// ---- RTX Video:目标尺寸的"跟随播放器"落点 ----
// 探测本进程可见顶层窗口(即 mpv 的 vo 窗)所在显示器,取其原生高度。
// 链创建粒度:mpv 在 seek/换片时重建整条 VS 链 → 重新探测;窗口换屏/
// 改尺寸后的生效点是下一次链重建(与 VS constant-format 契约一致,无法
// 做到每帧跟随 —— Magpie 式逐帧跟随在 mpv 架构下不存在落点)。
// 探测失败(无窗口/枚举失败)回落主显示器。
struct DisplayPick {
    int width = 0;
    int height = 0;
};

static DisplayPick DetectDisplaySize() noexcept {
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
        // mpv vo 窗口 = 本进程可见、带标题栏/边框的主窗口;排除工具窗/
        // 无边框隐藏辅助窗(面板另有进程)。
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
    HMONITOR mon = ctx.hwnd
                       ? MonitorFromWindow(ctx.hwnd, MONITOR_DEFAULTTONEAREST)
                       : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        return { mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top };
    }
    return { 0, 0 };
}

// RTX 参数裁决:vpy args(已应用在 rtx 上)← [rtxvideo] ini 覆盖 ←
// mode=1 的显示器探测。返回最终参数(plugin.cpp 是 vsrAutoHeight 的唯一
// 写入点)。
static void ResolveRtxParams(RtxVideoParams &rtx) noexcept {
    wchar_t iniPath[MAX_PATH]{};
    HMODULE self = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ResolveRtxParams), &self)) {
        wchar_t dllPath[MAX_PATH]{};
        if (GetModuleFileNameW(self, dllPath, MAX_PATH)) {
            const std::filesystem::path dir =
                std::filesystem::path(dllPath).parent_path() / "dlssnr_ui.ini";
            const std::wstring ws = dir.wstring();
            if (ws.size() < MAX_PATH) {
                wcscpy_s(iniPath, ws.c_str());
            }
        }
    }
    if (iniPath[0]) vsdlssnr::LoadRtxVideoIni(rtx, iniPath);
    if (rtx.vsrMode == 1) {
        const DisplayPick disp = DetectDisplaySize();
        if (disp.height > 0) {
            rtx.vsrAutoHeight = disp.height;
        }
    } else {
        rtx.vsrAutoHeight = rtx.vsrHeight;
    }
}

// 降级路径的缩放拷贝(OUT ≠ 源 或 位深不同:处理失败帧的兜底,画质次要,
// 可用性第一)。近邻采样;8/10bit 同构(按样本粒度)。VS 平面 stride 对齐。
static void CopyPlanesScaled(const VSFrame *src, VSFrame *dst, const VSAPI *vsapi,
                             int srcW, int srcH, int dstW, int dstH) noexcept {
    const VSVideoFormat *fi = vsapi->getVideoFrameFormat(dst);
    const int bpp = fi->bytesPerSample;
    const int srcCw = (srcW + 1) >> 1, srcCh = (srcH + 1) >> 1;
    const int dstCw = (dstW + 1) >> 1, dstCh = (dstH + 1) >> 1;
    for (int p = 0; p < 3; ++p) {
        const int sw = p == 0 ? srcW : srcCw;
        const int sh = p == 0 ? srcH : srcCh;
        const int dw = p == 0 ? dstW : dstCw;
        const int dh = p == 0 ? dstH : dstCh;
        const uint8_t *sp = vsapi->getReadPtr(src, p);
        const int64_t sstride = vsapi->getStride(src, p);
        uint8_t *dp = vsapi->getWritePtr(dst, p);
        const int64_t dstride = vsapi->getStride(dst, p);
        const size_t sampleBytes = static_cast<size_t>(bpp);
        for (int y = 0; y < dh; ++y) {
            const int sy = (std::min)(sh - 1, static_cast<int>(static_cast<int64_t>(y) * sh / dh));
            const uint8_t *srow = sp + sstride * sy;
            uint8_t *drow = dp + dstride * y;
            for (int x = 0; x < dw; ++x) {
                const int sx = (std::min)(sw - 1, static_cast<int>(static_cast<int64_t>(x) * sw / dw));
                memcpy(drow + static_cast<size_t>(x) * sampleBytes,
                       srow + static_cast<size_t>(sx) * sampleBytes, sampleBytes);
            }
        }
    }
}

// HDR 输出帧色彩签名(BT.2020 PQ limited;mpv 按 props 上屏/色调映射)。
static void SetHdrFrameProps(VSFrame *frame, const VSAPI *vsapi) noexcept {
    VSMap *props = vsapi->getFramePropertiesRW(frame);
    if (!props) return;
    vsapi->mapSetInt(props, "_Matrix", 9, maReplace);      // BT.2020 NCL
    vsapi->mapSetInt(props, "_Transfer", 16, maReplace);   // ST 2084 (PQ)
    vsapi->mapSetInt(props, "_Primaries", 9, maReplace);   // BT.2020
    vsapi->mapSetInt(props, "_ColorRange", 1, maReplace);  // limited
}

// 从源帧 props 解析色彩矩阵/范围(缺失/未知回落 709 limited);值变化时
// 留痕一行(atomic 边沿去重)。VS _Matrix:1=BT.709,5(BT470BG)/6(SMPTE170M)
// =601 族,4(XYZ)=601 近似(D65 录制内容的可用降级);9(BT.2020)/0(GBR)
// 等超范围按 709 处理(vpy 层已有 HDR 直通守卫,到这里的多半是属性缺失
// 的裸流)。
static void ParseColorProps(const VSMap *props, const VSAPI *vsapi,
                            vsdlssnr::ColorMatrix &matrix,
                            vsdlssnr::ColorRange &range) noexcept {
    int perr = 0;
    const int matrixProp = vsapi->mapGetInt(props, "_Matrix", 0, &perr);
    const bool matrixKnown = perr == 0;
    perr = 0;
    const int rangeProp = vsapi->mapGetInt(props, "_ColorRange", 0, &perr);
    const bool rangeKnown = perr == 0;
    matrix = vsdlssnr::ColorMatrix::BT709;
    if (matrixKnown && (matrixProp == 4 || matrixProp == 5 || matrixProp == 6)) {
        matrix = vsdlssnr::ColorMatrix::BT601;
    }
    range = vsdlssnr::ColorRange::Limited;
    if (rangeKnown && rangeProp == 0) range = vsdlssnr::ColorRange::Full;
    const int sig = (matrixKnown ? matrixProp : -1) * 16 + (rangeKnown ? rangeProp : -2);
    static std::atomic<int> lastSig{ INT_MIN };
    if (lastSig.exchange(sig, std::memory_order_relaxed) != sig) {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: frame props matrix=%s(%d) range=%s(%d) -> %s/%s",
                      matrixKnown ? "yes" : "missing", matrixProp,
                      rangeKnown ? "yes" : "missing", rangeProp,
                      matrix == vsdlssnr::ColorMatrix::BT709 ? "709" : "601",
                      range == vsdlssnr::ColorRange::Limited ? "limited" : "full");
        vsdlssnr::TimingStatusLine(msg);
    }
}

static const VSFrame *VS_CC DlssnrGetFrame(
    int n, int activationReason, void *instanceData, void **frameData,
    VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    auto *d = static_cast<FilterData *>(instanceData);

    if (activationReason == arInitial) {
        if (d->fgActive) {
            // 输出索引 → (源帧, 槽位) 无状态闭式映射。VS core(fmParallel)
            // 的评估启动顺序不保证 n 单调,任何依赖请求顺序的状态机都会
            // 失位 —— 每个输出索引独立换算,天然免疫乱序:
            //   n == 0    → R0(播种源帧,仅真实帧)
            //   n >= 1    → 组 g = (n-1)/M0;源帧 k = g+1;组内位 p = (n-1)%M0
            //               p < M0-1 → 槽 p+1(插值帧,随处理源帧 k 产出)
            //               p == M0-1 → 槽 0(真实帧 R_k,组末尾)
            // 尾部(k 越过最后一源帧)= 重复 R_{S-1} 占位(无后续源帧可
            // 插值,首尾帧保持优于越界请求)。
            const int m0 = d->fgCreateMult;
            int k = 0, slot = 0;
            if (n > 0) {
                k = (n - 1) / m0 + 1;
                if (d->srcFrames > 0 && k >= d->srcFrames) {
                    k = d->srcFrames - 1;
                    slot = 0;
                } else {
                    const int p = (n - 1) % m0;
                    slot = (p == m0 - 1) ? 0 : p + 1;
                }
            }
            *frameData = reinterpret_cast<void *>(static_cast<intptr_t>((k << 4) | slot));
            vsapi->requestFrameFilter(k, d->node, frameCtx);
            return nullptr;
        }
        vsapi->requestFrameFilter(n, d->node, frameCtx);
        return nullptr;
    }
    if (activationReason != arAllFramesReady) return nullptr;

    static const bool timingEnabled = GetEnvironmentVariableA("VSDLSSNR_TIMING", nullptr, 0) != 0;

    // NR 总开关 live 门(shared_lock 快照,fmParallel 并发安全)。FG 激活
    // 的会话:NR 关由 ProcessFrame 内部门控(跳过降噪评估,补帧/光流照常
    // —— 输出仍是增强管线的产物),不走此直通。开关边沿向面板发一次状态
    // (见 nrPubState 注释)。
    const bool nrLive = d->params->Snapshot().nrEnabled != 0;
    const int nrPub = nrLive ? 1 : 0;
    if (d->nrPubState.exchange(nrPub) != nrPub) {
        char body[224];
        if (!nrLive && !d->fgActive) {
            std::snprintf(body, sizeof(body), "{\"%s\":\"passthrough\",\"%s\":\"NR off (panel)\"}",
                          vsdlssnr::SK_FILTER_STATE, vsdlssnr::SK_STATE_DETAIL);
        } else if (nrLive && !d->initOk) {
            std::snprintf(body, sizeof(body), "{\"%s\":\"passthrough\",\"%s\":\"NR on; seek to initialize\"}",
                          vsdlssnr::SK_FILTER_STATE, vsdlssnr::SK_STATE_DETAIL);
        } else {
            body[0] = '\0'; // 已初始化 / FG 仍在跑:常规逐帧 stats 接管,无需发布
        }
        if (body[0]) vsdlssnr::PublishStatsJson(body);
    }

    if (d->fgActive) {
        const intptr_t fd = reinterpret_cast<intptr_t>(*frameData);
        const int k = static_cast<int>(fd >> 4);
        const int slot = static_cast<int>(fd & 0xF);
        // fgMutex 串行处理与缓存(同源帧一次处理,各槽从缓存出;FG 链的
        // NVOF/DLSSG 历史按处理序推进,乱序由 NVOF 帧序门自愈)。
        std::lock_guard<std::mutex> fgLock(d->fgMutex);
        // 有效密度 = min(live 倍数, 结构倍数 M0)。M0(帧数/节奏契约)创建时
        // 定格;live 倍数只决定组内多少槽位产出真插值 —— 关闭/降档即时生效
        // (多余槽位回落真实帧引用),升档超过 M0 无槽可填(需下个 seek)。
        // 播种源帧(k=0)仅真实帧:无消费槽,不带插值平面(eval 门随之关闭,
        // 历史重置留给首个插值组,与其对齐 Magpie 的 reset 帧不发布插值)。
        const int m0 = d->fgCreateMult;
        const DlssnrParams snap = d->params->Snapshot();
        int effM = snap.fgEnabled
                       ? (std::min)(std::clamp(snap.fgMultiplier, kFgMultMin, kFgMultMax), m0)
                       : 1;
        int effGens = effM - 1;
        if (k == 0) {
            effM = 1;
            effGens = 0;
        }
        if (k == d->fgCacheK && d->fgCache[0]) {
            // 缓存命中:同源帧已处理,任意槽位交一个新引用(缓存自留)。
            // 无缓存内容的槽位(密度下调的多余槽/播种帧/eval 降级)回落
            // 真实帧 —— 槽位照常占位,输出节奏不变。
            const VSFrame *out =
                (slot >= 1 && slot < d->fgCacheM && d->fgCache[slot])
                    ? vsapi->addFrameRef(d->fgCache[slot])
                    : vsapi->addFrameRef(d->fgCache[0]);
            return out;
        }

        const VSFrame *src = vsapi->getFrameFilter(k, d->node, frameCtx);
        if (!d->initOk) {
            // passthrough on setup failure(防御:fgActive 恒蕴含 initOk;
            // NR 关不在此列 —— ProcessFrame 内部门控跳过降噪评估,补帧以
            // 直通帧为 backbuffer 照常插值)。FG 输出计数仍是 M0(帧率已
            // ×M0):源帧单次复制入缓存,各槽回落该帧,时长按 1/M0 摊分。
            // RTX 几何 ≠ 源时走缩放拷贝(输出帧尺寸契约不破)。
            VSFrame *dup = vsapi->newVideoFrame(&d->outFi, d->outW, d->outH, src, core);
            if (d->rtxActive) {
                CopyPlanesScaled(src, dup, vsapi, d->width, d->height, d->outW, d->outH);
            } else {
                CopyPlanes(src, dup, vsapi, d->width, d->height);
            }
            if (d->hdrOut) SetHdrFrameProps(dup, vsapi);
            ScaleOutputDuration(dup, vsapi, m0);
            d->fgCacheK = k;
            d->fgCacheM = 1;
            for (int i = 0; i < kFgMultMax; ++i) {
                if (d->fgCache[i]) vsapi->freeFrame(d->fgCache[i]);
                d->fgCache[i] = nullptr;
            }
            d->fgCache[0] = dup;
            const VSFrame *ret = vsapi->addFrameRef(d->fgCache[0]);
            vsapi->freeFrame(src);
            return ret;
        }

        VSFrame *out = vsapi->newVideoFrame(&d->outFi, d->outW, d->outH, src, core);
        // effGens 个插值输出帧(仅 eval 成功槽进缓存;失败/降级槽直接释放,
        // 槽位在出帧时回落真实帧引用 —— 零复制优于再拷一份重复帧)。
        VSFrame *genFrame[kFgGenSlots] = {};
        for (int g = 0; g < effGens; ++g) {
            genFrame[g] = vsapi->newVideoFrame(&d->outFi, d->outW, d->outH, src, core);
        }

        const uint8_t *srcPlanes[3]{};
        int64_t srcStrides[3]{};
        uint8_t *dstPlanes[3]{};
        int64_t dstStrides[3]{};
        for (int p = 0; p < 3; ++p) {
            srcPlanes[p] = vsapi->getReadPtr(src, p);
            srcStrides[p] = vsapi->getStride(src, p);
            dstPlanes[p] = vsapi->getWritePtr(out, p);
            dstStrides[p] = vsapi->getStride(out, p);
        }
        uint8_t *genPlanes[kFgGenSlots * 3]{};
        int64_t genStrides[kFgGenSlots * 3]{};
        for (int g = 0; g < effGens; ++g) {
            for (int p = 0; p < 3; ++p) {
                genPlanes[g * 3 + p] = vsapi->getWritePtr(genFrame[g], p);
                genStrides[g * 3 + p] = vsapi->getStride(genFrame[g], p);
            }
        }

        char err[256]{};
        char timing[128]{};
        vsdlssnr::ColorMatrix matrix = vsdlssnr::ColorMatrix::BT709;
        vsdlssnr::ColorRange range = vsdlssnr::ColorRange::Limited;
        ParseColorProps(vsapi->getFramePropertiesRO(src), vsapi, matrix, range);
        // NGX history-reset policy (frame-gap heuristic) lives in DlssnrContext;
        // only the frame index is forwarded here. 有效密度 effM 与 genPlanes
        // 布局([gen][plane] 扁平)即 ProcessFrame 的多帧输出契约。
        bool fgGenOk[kFgGenSlots] = {};
        const bool procOk =
            d->ngx->ProcessFrame(srcPlanes, srcStrides, dstPlanes, dstStrides,
                                 effM, effGens > 0 ? genPlanes : nullptr,
                                 effGens > 0 ? genStrides : nullptr, fgGenOk,
                                 d->width, d->height, k, matrix, range, err, sizeof(err),
                                 timingEnabled ? timing : nullptr, timingEnabled ? sizeof(timing) : 0);
        if (!procOk) {
            if (!d->failureLogged.exchange(true)) {
                char msg[512];
                std::snprintf(msg, sizeof(msg), "vs_dlssnr frame %d failed: %s", k, err);
                vsapi->logMessage(mtWarning, msg, core);
                // 探针:首帧失败进 timing log(GUI mpv 完全看不到 logMessage)。
                vsdlssnr::TimingStatusLine(msg);
            }
            // Failed frames fall back to a plain copy of the source content so
            // the output planes are never left uninitialized. YUV420:色度半尺寸;
            // RTX 几何 ≠ 源时缩放拷贝兜底。
            if (d->rtxActive) {
                CopyPlanesScaled(src, out, vsapi, d->width, d->height, d->outW, d->outH);
            } else {
                CopyPlanes(src, out, vsapi, d->width, d->height);
            }
        } else {
            d->failureLogged.store(false);
            if (d->hdrOut) SetHdrFrameProps(out, vsapi);
        }
        // 失败/未评插值槽:释放帧(出帧时槽位回落真实帧引用)。
        for (int g = 0; g < effGens; ++g) {
            if (genFrame[g] && (!procOk || !fgGenOk[g])) {
                vsapi->freeFrame(genFrame[g]);
                genFrame[g] = nullptr;
            }
        }

        if (timingEnabled && timing[0]) {
            // Throttle: log every 30th frame (fmParallel: order irrelevant).
            static std::atomic<int> timingFrameCount{ 0 };
            if (timingFrameCount.fetch_add(1, std::memory_order_relaxed) % 30 == 1) {
                char msg[192];
                std::snprintf(msg, sizeof(msg), "vs_dlssnr timing[%d]: %s", k, timing);
                vsapi->logMessage(mtInformation, msg, core);
            }
        }

        // 时长契约:每输出帧 = 源时长/M0(_DurationNum/_DurationDen 整数对
        // —— mpv 逐帧读回累加 pts、nominal_fps 也由它重算;vi.fps 只是元
        // 数据)。密度变化不改变节奏 —— 同源帧 M0 个输出时长求和恒等于源
        // 时长。缓存帧各设一次(出帧交引用,props 随帧)。
        ScaleOutputDuration(out, vsapi, m0);
        for (int g = 0; g < effGens; ++g) {
            if (genFrame[g]) {
                if (d->hdrOut) SetHdrFrameProps(genFrame[g], vsapi);
                ScaleOutputDuration(genFrame[g], vsapi, m0);
            }
        }
        // 缓存替换:[0]=真实帧,[1..effGens]=成功插值帧(slot g+1)。
        for (int i = 0; i < kFgMultMax; ++i) {
            if (d->fgCache[i]) {
                vsapi->freeFrame(d->fgCache[i]);
                d->fgCache[i] = nullptr; // 置空:密度下调后高位槽不再回填,
                                         // 残留旧指针会被下一次填充/Free 双重释放
            }
        }
        d->fgCacheK = k;
        d->fgCacheM = effGens + 1;
        d->fgCache[0] = out; // 接管 newVideoFrame 的引用
        for (int g = 0; g < effGens; ++g) {
            d->fgCache[g + 1] = genFrame[g];
        }
        const VSFrame *ret =
            (slot >= 1 && slot < d->fgCacheM && d->fgCache[slot])
                ? vsapi->addFrameRef(d->fgCache[slot])
                : vsapi->addFrameRef(d->fgCache[0]);
        vsapi->freeFrame(src);
        return ret;
    }

    // ---- 非 FG 路径(1:1,与旧管线一致;RTX 开 = OUT 几何)----
    const VSFrame *src = vsapi->getFrameFilter(n, d->node, frameCtx);
    // passthrough(NR 关 = 原帧零拷贝;RTX 开时仍要处理 —— VSR/HDR 独立
    // 于 NR 开关)。引用移交调用方。
    if (!d->initOk || (!nrLive && !d->rtxActive)) return src;

    VSFrame *out = vsapi->newVideoFrame(&d->outFi, d->outW, d->outH, src, core);
    const uint8_t *srcPlanes[3]{};
    int64_t srcStrides[3]{};
    uint8_t *dstPlanes[3]{};
    int64_t dstStrides[3]{};
    for (int p = 0; p < 3; ++p) {
        srcPlanes[p] = vsapi->getReadPtr(src, p);
        srcStrides[p] = vsapi->getStride(src, p);
        dstPlanes[p] = vsapi->getWritePtr(out, p);
        dstStrides[p] = vsapi->getStride(out, p);
    }
    char err[256]{};
    char timing[128]{};
    vsdlssnr::ColorMatrix matrix = vsdlssnr::ColorMatrix::BT709;
    vsdlssnr::ColorRange range = vsdlssnr::ColorRange::Limited;
    ParseColorProps(vsapi->getFramePropertiesRO(src), vsapi, matrix, range);
    if (!d->ngx->ProcessFrame(srcPlanes, srcStrides, dstPlanes, dstStrides,
                              0, nullptr, nullptr, nullptr,
                              d->width, d->height, n, matrix, range, err, sizeof(err),
                              timingEnabled ? timing : nullptr, timingEnabled ? sizeof(timing) : 0)) {
        if (!d->failureLogged.exchange(true)) {
            char msg[512];
            std::snprintf(msg, sizeof(msg), "vs_dlssnr frame %d failed: %s", n, err);
            vsapi->logMessage(mtWarning, msg, core);
            vsdlssnr::TimingStatusLine(msg);
        }
        if (d->rtxActive) {
            CopyPlanesScaled(src, out, vsapi, d->width, d->height, d->outW, d->outH);
        } else {
            CopyPlanes(src, out, vsapi, d->width, d->height);
        }
    } else {
        d->failureLogged.store(false);
        if (d->hdrOut) SetHdrFrameProps(out, vsapi);
    }
    if (timingEnabled && timing[0]) {
        static std::atomic<int> timingFrameCount{ 0 };
        if (timingFrameCount.fetch_add(1, std::memory_order_relaxed) % 30 == 1) {
            char msg[192];
            std::snprintf(msg, sizeof(msg), "vs_dlssnr timing[%d]: %s", n, timing);
            vsapi->logMessage(mtInformation, msg, core);
        }
    }
    // getFrameFilter handed us a reference to src; release it or every source
    // frame leaks (~24MB per 1080p frame).
    vsapi->freeFrame(src);
    return out;
}

static void VS_CC DlssnrFree(void *instanceData, VSCore * /*core*/, const VSAPI *vsapi) {
    auto *d = static_cast<FilterData *>(instanceData);
    if (d->node) vsapi->freeNode(d->node);
    // Stop the bridge before tearing down the contexts it observes. The D3D12
    // + NGX contexts themselves move into the hot context instead of being
    // destroyed: mpv re-runs the VS script on every seek, and a warm context
    // makes the next filter instance near-free to create.
    vsdlssnr::BridgeStop(d->params.get());
    // FG 缓存帧:最后一个引用(缓存自留),先于 filter 释放。
    for (int i = 0; i < kFgMultMax; ++i) {
        if (d->fgCache[i]) vsapi->freeFrame(d->fgCache[i]);
    }
    if (d->initOk && d->d3d12 && d->ngx) {
        Hot().d3d12 = std::move(d->d3d12);
        Hot().ngx = std::move(d->ngx);
        Hot().ngxDllPath = std::move(d->ngxDllPath);
        Hot().width = d->width;
        Hot().height = d->height;
        Hot().depth = d->depth;
        Hot().fgEnabled = d->fgActive;
        Hot().fgDllPath = std::move(d->fgDllPath);
        Hot().rtx = d->rtx;
        Hot().valid = true;
        // 探针:停放行 —— 与下一次 create 的 "hot rebind kept"/"re-init"
        // 行配对,seek 生命周期序列(bridge stopped → freed → started →
        // rebind)在 timing log 里闭环;mpv 在停放后退出也有尾行可查。
        char msg[128];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: filter freed (hot parked %dx%dd%d fg=%d)",
                      d->width, d->height, d->depth, d->fgActive ? 1 : 0);
        vsdlssnr::TimingStatusLine(msg);
    }
    delete d;
}

static void VS_CC DlssnrCreate(
    const VSMap *in, VSMap *out, void * /*userData*/, VSCore *core, const VSAPI *vsapi) {
    VSNode *node = vsapi->mapGetNode(in, "clip", 0, nullptr);
    if (!node) {
        vsapi->mapSetError(out, "dlssnr.Enhance: missing clip argument");
        return;
    }
    const VSVideoInfo *vi = vsapi->getVideoInfo(node);

    // YUV 原生(全切 RGBS):仅 YUV420P8/P10,输出同格式。其它格式由 vpy
    // 直通守卫(防不支持格式打断播放)。
    const bool is420P8 = vi->format.colorFamily == cfYUV && vi->format.sampleType == stInteger &&
                         vi->format.bitsPerSample == 8 && vi->format.subSamplingW == 1 &&
                         vi->format.subSamplingH == 1;
    const bool is420P10 = vi->format.colorFamily == cfYUV && vi->format.sampleType == stInteger &&
                          vi->format.bitsPerSample == 10 && vi->format.subSamplingW == 1 &&
                          vi->format.subSamplingH == 1;
    if (!vsh::isConstantVideoFormat(vi) || (!is420P8 && !is420P10)) {
        vsapi->mapSetError(out, "dlssnr.Enhance: clip must be YUV420P8 or YUV420P10 (constant format)");
        vsapi->freeNode(node);
        return;
    }

    auto *d = new FilterData();
    d->node = node;
    d->width = vi->width;
    d->height = vi->height;
    d->depth = vi->format.bitsPerSample;
    d->srcFrames = vi->numFrames; // FG 尾部映射;0 = 未知长度(尾部钳制禁用)

    DlssnrParams initial{}; // member initializers are the default authority
    // NR 总开关(0/1):0 = 跳过 D3D12/NGX 全部初始化,滤镜纯直通零开销,
    // FG 一并不可用;面板开关(live)与 ini/vpy 均可控制。
    ApplyFlagArg(in, vsapi, "nr_enabled", initial.nrEnabled);
    ApplyIntArg(in, vsapi, "preset", initial.preset, kPresetMin, kPresetMax);
    ApplyIntArg(in, vsapi, "style", initial.style, kStyleMin, kStyleMax);
    ApplyFloatArg(in, vsapi, "intensity", initial.intensity, kStrengthMin, kStrengthMax);
    ApplyFloatArg(in, vsapi, "local_tone", initial.localToneStrength, kStrengthMin, kStrengthMax);
    ApplyFloatArg(in, vsapi, "local_structure", initial.localStructureStrength, kStrengthMin, kStrengthMax);
    ApplyFloatArg(in, vsapi, "skin_structure", initial.skinStructureStrength, kSkinMin, kSkinMax);
    ApplyFlagArg(in, vsapi, "use_auto_mask", initial.useAutoMask);
    ApplyFloatArg(in, vsapi, "residual_multiplier", initial.residualMultiplier, kResidualMultMin, kResidualMultMax);
    ApplyFloatArg(in, vsapi, "residual_saturation", initial.residualSaturation, kResidualFineMin, kResidualFineMax);
    ApplyFloatArg(in, vsapi, "residual_lightness", initial.residualLightness, kResidualFineMin, kResidualFineMax);
    ApplyFloatArg(in, vsapi, "shadow_structure", initial.shadowStructureMultiplier, kResidualFineMin, kResidualFineMax);
    ApplyFloatArg(in, vsapi, "reflection_glow", initial.reflectionGlowMultiplier, kResidualFineMin, kResidualFineMax);
    ApplyIntArg(in, vsapi, "input_resolution", initial.inputResolutionPercent, kResPctMin, kResPctMax);
    // scaling_enabled=0 drops the residual pipeline entirely (input_resolution ignored)
    ApplyFlagArg(in, vsapi, "scaling_enabled", initial.scalingEnabled);
    // NVOF 光流质量 0-5(0 = 零 guidance);>0 且驱动支持时启用真运动矢量
    ApplyIntArg(in, vsapi, "motion_vector_quality", initial.motionVectorQuality, kOfQualityMin, kOfQualityMax);
    // FFX 光流质量 0-2(0 = 无;1 = 性能,2 = 质量;两后端档位分字段,
    // 面板单下拉按当前后端读写)
    ApplyIntArg(in, vsapi, "ffx_quality", initial.ffxQuality, kFfxQualityMin, kFfxQualityMax);
    // 光流输入跟随内部降采样(scaling 启用时 NVOF 按内部尺寸计算)
    ApplyFlagArg(in, vsapi, "nvof_follow_scaling", initial.nvofFollowScaling);
    // DLSS 帧生成(0/1):激活时每源帧产出 M 帧(1 真实 + M-1 插值),
    // 输出帧时长 = 源时长/M(mpv vapoursynth 契约),失败优雅回退 1:1。
    // 挂 DLSSNR 之后 —— backbuffer = NR 输出。
    ApplyFlagArg(in, vsapi, "fg_enabled", initial.fgEnabled);
    // 插帧倍数 2-6(live 参数,源帧边界生效;创建值定 vi.fps 元数据)
    ApplyIntArg(in, vsapi, "fg_multiplier", initial.fgMultiplier, kFgMultMin, kFgMultMax);
    // FG 路由 0=自动(预载 0.3.x hook 代理)/1=纯官方(不预载;进程级,
    // 重启生效)
    ApplyIntArg(in, vsapi, "fg_route", initial.fgRoute, kFgRouteMin, kFgRouteMax);
    ApplyIntArg(in, vsapi, "of_backend", initial.ofBackend, kOfBackendMin, kOfBackendMax);
    // Panel-saved profile (dlssnr_ui.ini) overrides .vpy values when present;
    // the panel's CURRENT payload (last live state) overrides the ini. Without
    // the adopt step a seek rebuilds the filter from stale ini/vpy values —
    // the bridge poll skips the existing payload (history), so the panel's
    // parameters only came back after touching the panel again.
    const bool iniLoaded = vsdlssnr::BridgeLoadIni(initial);
    const bool payloadAdopted = vsdlssnr::BridgeAdoptPanelPayload(initial);
    // RTX Video(vpy args 已在上面 Apply 到 d->rtx):[rtxvideo] ini 覆盖
    // + mode=1 显示器探测。三层里没有面板 payload —— RTX 参数暂不走面板
    // (PanelPayload ABI 未动),以 ini/vpy 为准。
    ApplyIntArg(in, vsapi, "vsr_mode", d->rtx.vsrMode, kVsrModeMin, kVsrModeMax);
    ApplyIntArg(in, vsapi, "vsr_height", d->rtx.vsrHeight, kVsrHeightMin, kVsrHeightMax);
    ApplyIntArg(in, vsapi, "vsr_strength", d->rtx.vsrStrength, kVsrStrengthMin, kVsrStrengthMax);
    ApplyIntArg(in, vsapi, "hdr_enabled", d->rtx.hdrEnabled, 0, 1);
    ApplyIntArg(in, vsapi, "hdr_contrast", d->rtx.hdrContrast, kHdrContrastMin, kHdrContrastMax);
    ApplyIntArg(in, vsapi, "hdr_saturation", d->rtx.hdrSaturation, kHdrSaturationMin, kHdrSaturationMax);
    ApplyIntArg(in, vsapi, "hdr_middle_gray", d->rtx.hdrMiddleGray, kHdrMiddleGrayMin, kHdrMiddleGrayMax);
    ApplyIntArg(in, vsapi, "hdr_peak_nits", d->rtx.hdrMaxLuminance, kHdrMaxLumMin, kHdrMaxLumMax);
    ResolveRtxParams(d->rtx);
    // 探针:三层参数源(vpy 默认 → ini → 面板 payload)的最终裁决值。
    // "参数没生效/拖进度条回去了"类问题(#37)一行定位:ini/payload 哪层
    // 参与了、create-time 三元组最终是什么,一眼可查。
    {
        char msg[288];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: create params %dx%dd%d ini=%d payload=%d -> nr=%d preset=%d res=%d%% scaling=%d of=%d ffx=%d follow=%d fg=%d mult=%d route=%d",
                      d->width, d->height, d->depth, iniLoaded ? 1 : 0, payloadAdopted ? 1 : 0,
                      initial.nrEnabled ? 1 : 0, initial.preset, initial.inputResolutionPercent,
                      initial.scalingEnabled ? 1 : 0, initial.motionVectorQuality,
                      initial.ffxQuality,
                      initial.nvofFollowScaling ? 1 : 0,
                      initial.fgEnabled ? 1 : 0,
                      initial.fgMultiplier, initial.fgRoute);
        vsdlssnr::TimingStatusLine(msg);
    }
    d->params = std::make_unique<vsdlssnr::SharedParams>(initial);

    int dllErr = 0;
    const char *dllArg = vsapi->mapGetData(in, "ngx_dll", 0, &dllErr);
    if (!dllErr && dllArg && dllArg[0]) {
        try {
            // VS map strings are UTF-8; the path(const char*) ctor would
            // decode them as ANSI and mojibake non-ASCII paths.
            d->ngxDllPath = std::filesystem::path(reinterpret_cast<const char8_t *>(dllArg)).wstring();
        } catch (...) {
            d->ngxDllPath.clear();
        }
    }
    // Default: <plugin dir>/ngx/nvngx_dlssnr.dll
    if (d->ngxDllPath.empty()) {
        const VSPlugin *self = vsapi->getPluginByNamespace(PLUGIN_NAMESPACE, core);
        const char *selfPath = self ? vsapi->getPluginPath(self) : nullptr;
        if (selfPath) {
            try {
                // getPluginPath is UTF-8 as well (see above)
                const std::filesystem::path dir =
                    std::filesystem::path(reinterpret_cast<const char8_t *>(selfPath)).parent_path() / "ngx";
                d->ngxDllPath = (dir / SNIPPET_DLL_NAME).wstring();
            } catch (...) {
                d->ngxDllPath.clear();
            }
        }
    }

    // NR+FG 皆关时也照常解析 FG DLL 路径(重开时即最新值);热/冷初始化
    // 由下方各守卫跳过 —— 零设备、零显存、零 GPU。仅 NR 关而 FG 开:
    // 初始化照常,降噪评估在 ProcessFrame 内部跳过。

    // FG hook 代理 DLL(dlssg_for_sm86 0.3.x 的 version.dll;用户自备部署,
    // 与模型 DLL 同目录约定)。默认 <plugin dir>/ngx/version.dll。
    {
        int fgDllErr = 0;
        const char *fgDllArg = vsapi->mapGetData(in, "fg_dll", 0, &fgDllErr);
        if (!fgDllErr && fgDllArg && fgDllArg[0]) {
            try {
                d->fgDllPath = std::filesystem::path(reinterpret_cast<const char8_t *>(fgDllArg)).wstring();
            } catch (...) {
                d->fgDllPath.clear();
            }
        }
        if (d->fgDllPath.empty()) {
            const VSPlugin *self = vsapi->getPluginByNamespace(PLUGIN_NAMESPACE, core);
            const char *selfPath = self ? vsapi->getPluginPath(self) : nullptr;
            if (selfPath) {
                try {
                    const std::filesystem::path dir =
                        std::filesystem::path(reinterpret_cast<const char8_t *>(selfPath)).parent_path() / "ngx";
                    d->fgDllPath = (dir / FG_PROXY_DLL_NAME).wstring();
                } catch (...) {
                    d->fgDllPath.clear();
                }
            }
        }
    }

    // Eager init: the D3D12/NGX bring-up costs ~1s (165MB snippet DLL load +
    // CreateFeature + first-evaluate warm-up). Doing it here — script
    // execution, before mpv starts the playback clock — keeps that whole
    // warm-up period out of playback. A hot context from the previous filter
    // instance (freed on the last seek) skips the bring-up entirely —
    // including for a DIFFERENT video size: Rebind rebuilds the frame
    // resources + feature inside one pool-sealed pass. Only a changed
    // snippet DLL forces the cold path. Failure keeps the passthrough
    // fallback semantics below.
    char err[256]{};
    // depth 参与 hotMatch:同尺寸换深度(P8↔P10)走冷重建 —— 热上下文的
    // 槽纹理按旧位深建,R8 纹理遇 P10 打包 = 数据撕裂(#40-① 同族)。
    // FG 状态(开关 + 代理路径)同样参与:槽资源形态(FG 纹理有无)随之
    // 变化,必须冷重建。路由(route)进程级,重启生效,不参与。
    // NR+FG 皆关 = 跳过热复用(实例纯直通,停泊上下文原样保留,重开秒回);
    // 仅 NR 关而 FG 开仍需热复用(设备与上下文都在用)。
    // RTX 请求开 = vsr_mode>0 或 hdr 开(初始化守卫参与;皆关 + NR/FG 皆关
    // = 纯直通零 GPU)。
    const bool rtxRequested = d->rtx.vsrMode > 0 || d->rtx.hdrEnabled != 0;
    const bool hotMatch = (initial.nrEnabled || initial.fgEnabled || rtxRequested) &&
                          Hot().valid &&
                          Hot().ngxDllPath == d->ngxDllPath &&
                          Hot().width == d->width && Hot().height == d->height &&
                          Hot().depth == d->depth &&
                          Hot().fgEnabled == (initial.fgEnabled != 0) &&
                          Hot().fgDllPath == d->fgDllPath &&
                          Hot().rtx == d->rtx;
    if (hotMatch) {
        d->d3d12 = std::move(Hot().d3d12);
        d->ngx = std::move(Hot().ngx);
        Hot().valid = false;
        Hot().ngxDllPath.clear();
        if (d->ngx->Rebind(d->params.get(), d->width, d->height, d->depth, d->rtx, err, sizeof(err))) {
            d->initOk = true;
            // Filter is live: start the mpv-side parameter bridge
            if (!vsdlssnr::BridgeStart(d->params.get())) {
                vsapi->logMessage(mtWarning,
                                  "vs_dlssnr: parameter bridge failed to start; panel edits will not apply",
                                  core);
                vsdlssnr::TimingStatusLine("DLSSNR STATUS: bridge start FAILED; panel edits will not apply");
            }
            char msg[128];
            std::snprintf(msg, sizeof(msg), "vs_dlssnr ready from hot context (%dx%d)", d->width, d->height);
            vsapi->logMessage(mtInformation, msg, core);
        } else {
            // The warm context is wedged (NGX state lost): drop it and fall
            // through to a full re-initialization below.
            char msg[512];
            std::snprintf(msg, sizeof(msg), "vs_dlssnr hot rebind failed, re-initializing: %s", err);
            vsapi->logMessage(mtWarning, msg, core);
            vsdlssnr::TimingStatusLine(msg); // GUI mpv 不透传 logMessage,失败必须进 timing log
            d->ngx.reset();
            d->d3d12.reset();
        }
    }
    if ((initial.nrEnabled || initial.fgEnabled || rtxRequested) && !d->initOk && !d->d3d12) {
        // Any parked context left over (different snippet DLL) must be torn
        // down BEFORE a cold init: the IAT hook and the NGX core are
        // process-global singletons. A second full Initialize could never
        // install the hook (its owner CAS is held by the parked context) and
        // its eventual Shutdown1 would tear down the shared core under the
        // parked feature — silently degrading every later resolution.
        if (Hot().valid) {
            Hot().ngx->Shutdown();
            Hot().ngx.reset();
            Hot().d3d12.reset();
            Hot().valid = false;
            Hot().ngxDllPath.clear();
        }
        d->d3d12 = std::make_unique<vsdlssnr::D3D12Context>();
        d->ngx = std::make_unique<vsdlssnr::DlssnrContext>();
        if (d->d3d12->Initialize(err, sizeof(err)) &&
            d->ngx->Initialize(*d->d3d12, d->ngxDllPath.c_str(), d->fgDllPath.c_str(),
                               d->width, d->height, d->depth, d->params.get(), d->rtx,
                               err, sizeof(err))) {
            d->initOk = true;
            // Filter is live: start the mpv-side parameter bridge
            if (!vsdlssnr::BridgeStart(d->params.get())) {
                vsapi->logMessage(mtWarning,
                                  "vs_dlssnr: parameter bridge failed to start; panel edits will not apply",
                                  core);
                vsdlssnr::TimingStatusLine("DLSSNR STATUS: bridge start FAILED; panel edits will not apply");
            }
            char msg[128];
            std::snprintf(msg, sizeof(msg), "vs_dlssnr ready (%dx%d)", d->width, d->height);
            vsapi->logMessage(mtInformation, msg, core);
        } else {
            char msg[512];
            std::snprintf(msg, sizeof(msg),
                          "vs_dlssnr init failed, falling back to passthrough: %s", err);
            vsapi->logMessage(mtWarning, msg, core);
            vsdlssnr::TimingStatusLine(msg); // GUI mpv 不透传 logMessage,失败必须进 timing log
            OutputDebugStringA("vs_dlssnr: init failed: ");
            OutputDebugStringA(err);
            OutputDebugStringA("\n");
            // 面板可见状态:D3D12/NGX 初始化失败 = 本实例整体直通。原因串
            // 可能含 D3D12 debug-layer 文本(引号),消毒后再进 stats。
            char safe[288];
            vsdlssnr::SanitizeJsonDetail(err, safe, sizeof(safe));
            char body[384];
            std::snprintf(body, sizeof(body), "{\"%s\":\"passthrough\",\"%s\":\"%.200s\"}",
                          vsdlssnr::SK_FILTER_STATE, vsdlssnr::SK_STATE_DETAIL, safe);
            vsdlssnr::PublishStatsJson(body);
        }
    }
    if (!initial.nrEnabled && !initial.fgEnabled && !rtxRequested) {
        // NR + FG + RTX 皆关:热/冷初始化全部跳过 —— 零设备、零显存、零 GPU,
        // 滤镜纯直通(getFrame 原帧交还)。仅 NR 关而 FG/RTX 开时不走此分支:
        // 初始化照常,降噪评估由 ProcessFrame 内部门控跳过。桥接照常启动:
        // 面板仍被拉起并可实时控制 —— 已激活会话 live 重开立即恢复;创建即
        // 全关的实例重开需下个 seek(停泊热上下文原样保留,同参数重开走
        // 秒回的热复用)。
        char body[192];
        std::snprintf(body, sizeof(body), "{\"%s\":\"passthrough\",\"%s\":\"NR+FG+RTX disabled (panel/vpy)\"}",
                      vsdlssnr::SK_FILTER_STATE, vsdlssnr::SK_STATE_DETAIL);
        vsdlssnr::PublishStatsJson(body);
        if (!vsdlssnr::BridgeStart(d->params.get())) {
            vsapi->logMessage(mtWarning,
                              "vs_dlssnr: parameter bridge failed to start; panel edits will not apply",
                              core);
            vsdlssnr::TimingStatusLine("DLSSNR STATUS: bridge start FAILED; panel edits will not apply");
        }
        vsdlssnr::TimingStatusLine("DLSSNR STATUS: NR+FG+RTX disabled; passthrough (zero GPU)");
    }

    // FG 多帧输出判定(两条初始化路径汇合):会话激活 = 每源帧产出 M 帧
    // (输出帧时长 = 源时长/M,mpv 逐帧认 _DurationNum/_DurationDen);
    // 否则 1:1(FG 失败的降级语义)。vi 副本按创建值倍增 fps —— 纯元数据
    // (mpv 不据此节拍,但下游工具/时长估算消费它)。
    d->fgActive = d->initOk && d->ngx && d->ngx->FgActive();
    // RTX 输出几何(init 后从 context 读回 —— pipe/out 的最终裁决在
    // Initialize 内含 capability/倍率旁路)。未初始化实例 = 源几何直通。
    d->outW = d->width;
    d->outH = d->height;
    d->hdrOut = false;
    d->rtxActive = false;
    d->outFi = vi->format; // VS4:VSVideoInfo.format 为内嵌值(VS4 API 形态)
    if (d->initOk && d->ngx) {
        d->outW = d->ngx->OutWidth();
        d->outH = d->ngx->OutHeight();
        d->hdrOut = d->ngx->HdrActive();
        d->rtxActive = d->ngx->RtxActive();
        if (d->hdrOut) {
            // HDR 输出契约 = YUV420P10(BT.2020 PQ limited;props 逐帧写)。
            VSVideoFormat p10{};
            if (!vsapi->getVideoFormatByID(&p10, pfYUV420P10, core)) {
                vsapi->mapSetError(out, "dlssnr.Enhance: getVideoFormatByID(YUV420P10) failed");
                delete d;
                return;
            }
            d->outFi = p10;
        }
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: output geometry %dx%d fmt=%dbpp hdr=%d rtx=%d (src %dx%dd%d)",
                      d->outW, d->outH, d->outFi.bitsPerSample, d->hdrOut ? 1 : 0,
                      d->rtxActive ? 1 : 0, d->width, d->height, d->depth);
        vsdlssnr::TimingStatusLine(msg);
    }
    VSVideoInfo viOut = *vi;
    viOut.width = d->outW;
    viOut.height = d->outH;
    viOut.format = d->outFi; // VS4:VSVideoInfo.format 为内嵌值
    if (d->fgActive) {
        d->fgCreateMult = std::clamp(d->params->Snapshot().fgMultiplier, kFgMultMin, kFgMultMax);
        viOut.fpsNum *= d->fgCreateMult;
        // 帧数同步 ×M:VS4 里 n >= numFrames 的请求会被核心拒为越界 ——
        // mpv 顺序拉流到尾帧时会提前 EOF(尾段丢插值帧)。
        if (viOut.numFrames > 0) viOut.numFrames *= d->fgCreateMult;
        char msg[160];
        std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: fg active, output fps x%d (%I64d/%I64d -> %I64d/%I64d)",
                      d->fgCreateMult, vi->fpsNum, vi->fpsDen, viOut.fpsNum, viOut.fpsDen);
        vsdlssnr::TimingStatusLine(msg);
    }

    VSFilterDependency deps[]{ { node, rpStrictSpatial } };
    // fmParallel: mpv keeps multiple frame requests in flight; each getFrame
    // runs on its own D3D12 slot (slot pool caps the concurrency at
    // kSlotCount), which overlaps CPU pack/unpack with the GPU work of other
    // slots — the old fmUnordered path idled the GPU between frames.
    // FG 模式下 getFrame 内部经 fgMutex 串行(FG 链按源帧序),缓存条目
    // 保证同源帧的两条输出只处理一次。
    vsapi->createVideoFilter(out, "Enhance", &viOut, DlssnrGetFrame, DlssnrFree,
                             fmParallel, deps, 1, d, core);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
    // Pin the DLL: mpv's vf_vapoursynth tears down and recreates the whole VS
    // core on every seek, which unloads and reloads every plugin DLL. Without
    // the pin, the process-level hot context (device + NGX feature + slot
    // pool) would die with our CRT heap at unload and every seek would pay
    // the ~1s bring-up again. Pinned modules never unload (FreeLibrary is
    // ignored); the OS reclaims them at process exit.
    HMODULE self = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&VapourSynthPluginInit2), &self);
    vspapi->configPlugin(PLUGIN_IDENTIFIER, PLUGIN_NAMESPACE, PLUGIN_NAME,
                         VS_MAKE_VERSION(0, 1), VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction(
        "Enhance",
        "clip:vnode;"
        "ngx_dll:data:opt;"
        "nr_enabled:int:opt;"
        "preset:int:opt;"
        "style:int:opt;"
        "intensity:float:opt;"
        "local_tone:float:opt;"
        "local_structure:float:opt;"
        "skin_structure:float:opt;"
        "use_auto_mask:int:opt;"
        "residual_multiplier:float:opt;"
        "residual_saturation:float:opt;"
        "residual_lightness:float:opt;"
        "shadow_structure:float:opt;"
        "reflection_glow:float:opt;"
        "scaling_enabled:int:opt;"
        "input_resolution:int:opt;"
        "motion_vector_quality:int:opt;"
        "ffx_quality:int:opt;"
        "nvof_follow_scaling:int:opt;"
        "fg_enabled:int:opt;"
        "fg_multiplier:int:opt;"
        "fg_route:int:opt;"
        "of_backend:int:opt;"
        "vsr_mode:int:opt;"
        "vsr_height:int:opt;"
        "vsr_strength:int:opt;"
        "hdr_enabled:int:opt;"
        "hdr_contrast:int:opt;"
        "hdr_saturation:int:opt;"
        "hdr_middle_gray:int:opt;"
        "hdr_peak_nits:int:opt;"
        "fg_dll:data:opt;",
        "clip:vnode;",
        DlssnrCreate, nullptr, plugin);
}
