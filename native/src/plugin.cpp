// vs_dlssnr - NVIDIA DLSSNR (NGX Feature 18) filter for VapourSynth.
// Ported from Magpie experimental (github.com/SAOG0721/Magpie).
// Model file: nvngx_dlssnr.dll (310.9.0), zero-guidance mode (Magpie
// guidanceMode=1 Force Zero), same-resolution processing.

#include "bridge.h"
#include "d3d12_context.h"
#include "dlssnr_context.h"
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
constexpr char FG_PROXY_DLL_NAME[] = "version.dll"; // dlssg_for_sm86 原生代理

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

// ---------------------------------------------------------------------------
// FG proxy INI 同步:Router 面板选项的落点。dlssg_for_sm86 从 proxy DLL 同
// 目录读 dlssg_sm86.ini(五键,见其 docs/NATIVE_INI.md);mpv_PlayKit 拥有
// 该文件的 Router 键 —— 面板改路由后这里在 proxy 加载前自动同步,用户不
// 接触 INI。保留策略:Router 行按目标值原位替换,文件其余字节(用户自行
// 添加的 [Diagnostics] 排查段、注释、CRLF/LF 行尾风格)逐字节保留 —— 官方
// 文档(NATIVE_INI.md)推荐排查时手改此文件,切路由不得清空;文件缺失或无
// Router 键时才落地整套上游 0.2.4 默认模板(Router 键挂在 [Compatibility]
// 段内,无法安全外插)。时机:每次滤镜创建、proxy LoadLibrary 之前 —— 冷
// 路径当场生效;proxy 模块进程内钉住,已加载会话的改动按面板提示重启后生效。
// ---------------------------------------------------------------------------
void SyncProxyRouterIni(const std::wstring &fgDllPath, int fgRouter) noexcept {
    std::error_code ec;
    const std::filesystem::path proxyDll(fgDllPath);
    if (fgDllPath.empty() || !std::filesystem::exists(proxyDll, ec)) {
        return; // proxy 未部署:FG 本就不可用,不产生孤儿 INI
    }
    const char *router = fgRouter == 1 ? "SM75" : "SM86";
    const std::filesystem::path iniPath = proxyDll.parent_path() / "dlssg_sm86.ini";

    // 整文件按字节读入,扫描定位 Router 行(只认行首(允许空白)的 Router
    // 键:上游注释 "; SM86 for Ampere ..." 无 Router 前缀,天然跳过)。
    std::string content;
    {
        std::ifstream in(iniPath, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            content = ss.str();
        }
    }
    bool found = false;     // Router 键存在且带 '='(值待比对)
    size_t lineStart = 0, lineEnd = 0; // 行区间 [lineStart, lineEnd),不含行尾符
    size_t pos = 0;
    while (pos < content.size()) {
        const size_t eol = content.find('\n', pos);
        const size_t stop = (eol == std::string::npos) ? content.size() : eol;
        size_t len = stop - pos;
        if (len && content[pos + len - 1] == '\r') --len; // 行尾 \r 归入 EOL,替换时保留
        const size_t s = content.find_first_not_of(" \t", pos);
        if (s != std::string::npos && s < pos + len &&
            content.compare(s, 6, "Router") == 0) {
            const bool boundary = (s + 6 == pos + len) || content[s + 6] == '=' ||
                                  content[s + 6] == ' ' || content[s + 6] == '\t';
            const size_t eq = boundary ? content.find('=', s + 6) : std::string::npos;
            if (eq != std::string::npos && eq < pos + len) {
                found = true;
                lineStart = pos;
                lineEnd = pos + len; // 行内容末尾(不含 \r):替换后原 CRLF/LF 行尾原样保留
                const size_t v = content.find_first_not_of(" \t", eq + 1);
                if (v < pos + len && content.compare(v, 4, router) == 0) {
                    return; // 已是目标值:整文件不动,用户排查段原样
                }
                break; // Router 键存在但值不同 -> 仅替换该行
            }
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }

    std::string out;
    if (found) {
        out = content;
        out.replace(lineStart, lineEnd - lineStart, std::string("Router=") + router);
    } else {
        // 文件缺失 / 空 / 无 Router 键:落地整套上游默认模板。
        static const char *kIniFmt =
            "; Native proxy config maintained by mpv_PlayKit (FG Router panel option).\n"
            "; Other keys mirror dlssg_for_sm86 0.2.4 defaults; restart mpv after switching.\n"
            "[Compatibility]\n"
            "; SM86 for Ampere (RTX 30); SM75 for Turing (RTX 20) with KernelImage=PTX.\n"
            "Router=%s\n"
            "; PTX uses driver JIT. Cubin requires an exact GPU/Router match.\n"
            "KernelImage=PTX\n"
            "; 0 = exact output (default); 1 = optional approximate sampling, SM86 only.\n"
            "HardwareBilinear=0\n"
            "\n"
            "[FrameGeneration]\n"
            "; Capability limit: 1=2X, 2=3X, 3=4X. The filter requests the actual multiplier.\n"
            "MaxGeneratedFrames=3\n"
            "\n"
            "[Logging]\n"
            "; 0=off, 1=errors, 2=diagnostics, 3=verbose.\n"
            "Level=1\n";
        char buf[768];
        std::snprintf(buf, sizeof(buf), kIniFmt, router);
        out = buf;
    }
    std::ofstream file(iniPath, std::ios::binary | std::ios::trunc);
    if (!file) {
        vsdlssnr::TimingStatusLine(
            "DLSSNR STATUS: fg proxy ini sync FAILED (write); Router stays as on disk");
        return;
    }
    file << out;
    char msg[128];
    std::snprintf(msg, sizeof(msg), "DLSSNR STATUS: fg proxy ini synced Router=%s", router);
    vsdlssnr::TimingStatusLine(msg);
}

struct FilterData {
    VSNode *node = nullptr;
    // Runtime-mutable parameters shared with the tray panel (initial values
    // come from the .vpy call).
    std::unique_ptr<vsdlssnr::SharedParams> params;

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
    // Failure-log latch: a wedged context fails every frame at frame rate —
    // log the first failure only, re-arm on the next success.
    std::atomic<bool> failureLogged{ false };
    // NR 总开关 stats 发布边沿(-1 = 未发布):开关翻转时向面板发一次
    // filter_state 状态行(live 关 = 直通原因;live 开但未初始化 = 提示
    // 需要 seek)。开着且已初始化时不发布 —— 常规逐帧 stats 接管。
    std::atomic<int> nrPubState{ -1 };

    // ---- DLSS FG 多帧输出 ----
    // fgActive = 创建时 FG 激活(proxy 初始化成功):每源帧产出 M 帧
    // (1 真实 + M-1 插值;M = live 倍数,源帧边界生效)。输出索引 n 由
    // 状态机映射(fgNextN/fgCurK/fgSlot/fgM,fgMutex 内推进):序列 =
    // real0, gen(1..M-1), real1, gen(1..M-1), ... —— gen(k) 由源帧 k 的
    // 一次处理产出(插值落在 k-1 与 k 之间,与 Magpie 发布序一致)。
    // fgMutex 串行处理与缓存(FG 链的 NVOF/DLSSG 历史本质按源帧顺序,
    // 缓存防止同源帧被并发双跑)。
    std::mutex fgMutex;
    bool fgActive = false;
    std::wstring fgDllPath;
    int fgBackend = 0;                        // 创建时 FG 后端(hotMatch 比对用)
    int fgCreateMult = 2;                     // 创建时倍数(vi.fps 元数据用)
    int fgNextN = 0;                          // 下一个待映射的输出索引
    int fgCurK = -1;                          // 当前源帧(首个 slot0 请求时 ++)
    int fgSlot = 0;                           // 当前源帧的待发槽位(0=真实)
    int fgM = 2;                              // 当前源帧的倍数(触及时定格)
    int fgCacheK = -1;                        // 缓存命中 = 同源帧的后继请求
    int fgCacheM = 0;
    const VSFrame *fgCache[kFgMultMax] = {};  // [0]=真实 [1..M-1]=插值;持引用
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
    // FG 状态参与 hotMatch:开关/后端/代理路径变化 = 槽资源形态或后端选择
    // 变化(FG 纹理有无、官方 NGX vs proxy),必须冷重建。
    bool fgEnabled = false;
    std::wstring fgDllPath;
    int fgBackend = 0;
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
            // 输出索引 → (源帧, 槽位) 状态机(fgMutex 内推进;结果经
            // frameData 带到 arAllFramesReady —— VS 的 per-request 存储,
            // fmParallel 多帧在飞时各 n 的映射互不串扰)。序列:real0,
            // gen(1..M-1), real1, ... 倍数 M 在源帧边界取当前参数快照
            // (live 生效点);同源帧的 M 触及时定格,时长求和恒等于源时长。
            std::lock_guard<std::mutex> lock(d->fgMutex);
            if (n != d->fgNextN) {
                // 乱序请求(mpv 顺序拉流不应发生):按当前倍数闭式回退,
                // 只服务本帧不推进状态机。映射必须与状态机同款 floor
                // (k = n/M)——ceil 会让 gen 槽偏到 k+1,且尾帧
                // (n = 源帧数×M-1)算出 k = 源帧数,requestFrameFilter 越界。
                static std::atomic<bool> warned{ false };
                if (!warned.exchange(true)) {
                    vsdlssnr::TimingStatusLine("DLSSNR STATUS: fg out-of-order frame request; closed-form fallback");
                }
                const int k = n / d->fgM;
                const int slot = n % d->fgM;
                *frameData = reinterpret_cast<void *>(static_cast<intptr_t>((k << 4) | slot));
                vsapi->requestFrameFilter(k, d->node, frameCtx);
                return nullptr;
            }
            if (d->fgSlot == 0) {
                ++d->fgCurK;
                DlssnrParams snap = d->params->Snapshot();
                d->fgM = std::clamp(snap.fgMultiplier, kFgMultMin, kFgMultMax);
            }
            const int k = d->fgCurK;
            const int slot = d->fgSlot;
            d->fgSlot = (d->fgSlot + 1) % d->fgM;
            ++d->fgNextN;
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
        std::unique_lock<std::mutex> fgLock(d->fgMutex, std::defer_lock);
        fgLock.lock();
        if (k == d->fgCacheK && slot < d->fgCacheM && d->fgCache[0] && d->fgCache[slot]) {
            // 缓存命中:同源帧已产出全部 M 帧,交出一个新引用(缓存自留)。
            const VSFrame *out = vsapi->addFrameRef(d->fgCache[slot]);
            fgLock.unlock();
            return out;
        }

        const VSFrame *src = vsapi->getFrameFilter(k, d->node, frameCtx);
        const int m = d->fgM; // 本源帧倍数(arInitial 时定格)
        if (!d->initOk) {
            // passthrough on setup failure(防御:fgActive 恒蕴含 initOk;
            // NR 关不在此列 —— ProcessFrame 内部门控跳过降噪评估,补帧以
            // 直通帧为 backbuffer 照常插值)。FG 输出计数仍是 M(帧率已
            // ×M):源帧单次复制入缓存,各槽交新引用,时长按 1/M 摊分。
            VSFrame *dup = vsapi->copyFrame(src, core);
            ScaleOutputDuration(dup, vsapi, m);
            d->fgCacheK = k;
            d->fgCacheM = m;
            for (int i = 0; i < kFgMultMax; ++i) {
                if (d->fgCache[i]) vsapi->freeFrame(d->fgCache[i]);
                d->fgCache[i] = i == 0 ? dup : vsapi->addFrameRef(dup);
            }
            const VSFrame *ret = vsapi->addFrameRef(d->fgCache[slot]);
            vsapi->freeFrame(src);
            fgLock.unlock();
            return ret;
        }

        const VSVideoFormat *fi = vsapi->getVideoFrameFormat(src);
        VSFrame *out = vsapi->newVideoFrame(fi, d->width, d->height, src, core);
        // M-1 个插值输出帧(eval 失败/降级槽由下方复制真实帧填充)。
        VSFrame *genFrame[kFgGenSlots] = {};
        for (int g = 0; g < m - 1; ++g) {
            genFrame[g] = vsapi->newVideoFrame(fi, d->width, d->height, src, core);
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
        for (int g = 0; g < m - 1; ++g) {
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
        // only the frame index is forwarded here. 倍数 m 与 genPlanes 布局
        // ([gen][plane] 扁平)即 ProcessFrame 的多帧输出契约。
        bool fgGenOk[kFgGenSlots] = {};
        const bool procOk =
            d->ngx->ProcessFrame(srcPlanes, srcStrides, dstPlanes, dstStrides,
                                 m, genPlanes, genStrides, fgGenOk,
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
            // the output planes are never left uninitialized. YUV420:色度半尺寸。
            CopyPlanes(src, out, vsapi, d->width, d->height);
        } else {
            d->failureLogged.store(false);
        }
        // 插值帧内容:对应槽 eval 成功 = ProcessFrame 已回读;否则复制真实帧
        // (复位/零光流/面板关/eval 降级 —— 重复帧优于垃圾插值)。
        for (int g = 0; g < m - 1; ++g) {
            if (genFrame[g] && (!procOk || !fgGenOk[g])) {
                CopyPlanes(out, genFrame[g], vsapi, d->width, d->height);
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

        // M 帧入缓存(FG 链按源帧序,同源帧后继请求直接命中);各输出帧
        // 时长 = 源时长/M(_DurationNum/_DurationDen 整数对 —— mpv 唯一
        // 认的节奏来源;vi.fps 只是元数据)。
        for (int i = 0; i < m; ++i) {
            ScaleOutputDuration(i == 0 ? out : genFrame[i - 1], vsapi, m);
        }
        for (int i = 0; i < kFgMultMax; ++i) {
            if (d->fgCache[i]) {
                vsapi->freeFrame(d->fgCache[i]);
                d->fgCache[i] = nullptr; // 置空:live 下调 m 后高位槽不再回填,
                                         // 残留旧指针会被下一次填充/Free 双重释放
            }
        }
        d->fgCacheK = k;
        d->fgCacheM = m;
        d->fgCache[0] = out; // 接管 newVideoFrame 的引用
        for (int g = 0; g < m - 1; ++g) {
            d->fgCache[g + 1] = genFrame[g];
        }
        const VSFrame *ret = vsapi->addFrameRef(d->fgCache[slot]);
        vsapi->freeFrame(src);
        fgLock.unlock();
        return ret;
    }

    // ---- 非 FG 路径(1:1,与旧管线一致) ----
    const VSFrame *src = vsapi->getFrameFilter(n, d->node, frameCtx);
    if (!d->initOk || !nrLive) return src; // passthrough(NR 关 = 原帧零拷贝);引用移交调用方

    const VSVideoFormat *fi = vsapi->getVideoFrameFormat(src);
    VSFrame *out = vsapi->newVideoFrame(fi, d->width, d->height, src, core);
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
        CopyPlanes(src, out, vsapi, d->width, d->height);
    } else {
        d->failureLogged.store(false);
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
        Hot().fgBackend = d->fgBackend;
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
    ApplyFlagArg(in, vsapi, "ui_correction", initial.uiCorrection);
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
    // 光流输入跟随内部降采样(scaling 启用时 NVOF 按内部尺寸计算)
    ApplyFlagArg(in, vsapi, "nvof_follow_scaling", initial.nvofFollowScaling);
    // DLSS 帧生成(0/1):激活时每源帧产出 M 帧(1 真实 + M-1 插值),
    // 输出帧时长 = 源时长/M(mpv vapoursynth 契约),失败优雅回退 1:1。
    // 挂 DLSSNR 之后 —— backbuffer = NR 输出。
    ApplyFlagArg(in, vsapi, "fg_enabled", initial.fgEnabled);
    // 插帧倍数 2-4(live 参数,源帧边界生效;创建值定 vi.fps 元数据)
    ApplyIntArg(in, vsapi, "fg_multiplier", initial.fgMultiplier, kFgMultMin, kFgMultMax);
    // FG 路由 0=SM86/1=SM75(进程级,重启生效;proxy INI 由插件自动同步)
    ApplyIntArg(in, vsapi, "fg_router", initial.fgRouter, kFgRouterMin, kFgRouterMax);
    // FG 后端 0=自动/1=仅官方 NGX/2=仅 proxy(下个 seek 生效;hotMatch 拦截)
    ApplyIntArg(in, vsapi, "fg_backend", initial.fgBackend, kFgBackendMin, kFgBackendMax);
    // Panel-saved profile (dlssnr_ui.ini) overrides .vpy values when present;
    // the panel's CURRENT payload (last live state) overrides the ini. Without
    // the adopt step a seek rebuilds the filter from stale ini/vpy values —
    // the bridge poll skips the existing payload (history), so the panel's
    // parameters only came back after touching the panel again.
    const bool iniLoaded = vsdlssnr::BridgeLoadIni(initial);
    const bool payloadAdopted = vsdlssnr::BridgeAdoptPanelPayload(initial);
    // 探针:三层参数源(vpy 默认 → ini → 面板 payload)的最终裁决值。
    // "参数没生效/拖进度条回去了"类问题(#37)一行定位:ini/payload 哪层
    // 参与了、create-time 三元组最终是什么,一眼可查。
    {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      "DLSSNR STATUS: create params %dx%dd%d ini=%d payload=%d -> nr=%d preset=%d res=%d%% scaling=%d of=%d follow=%d fg=%d mult=%d router=%d backend=%d",
                      d->width, d->height, d->depth, iniLoaded ? 1 : 0, payloadAdopted ? 1 : 0,
                      initial.nrEnabled ? 1 : 0, initial.preset, initial.inputResolutionPercent,
                      initial.scalingEnabled ? 1 : 0, initial.motionVectorQuality,
                      initial.nvofFollowScaling ? 1 : 0, initial.fgEnabled ? 1 : 0,
                      initial.fgMultiplier, initial.fgRouter, initial.fgBackend);
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

    // NR+FG 皆关时也照常同步 Router INI(重开时即最新值);热/冷初始化
    // 由下方各守卫跳过 —— 零设备、零显存、零 GPU。仅 NR 关而 FG 开:
    // 初始化照常,降噪评估在 ProcessFrame 内部跳过。

    // FG proxy DLL(dlssg_for_sm86 的 version.dll;用户自备部署,与模型
    // DLL 同目录约定)。默认 <plugin dir>/ngx/version.dll。
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

    // FG 路由面板选项落点:proxy LoadLibrary 之前同步其同目录 INI(冷路径
    // 当场生效;模块已钉住的会话按面板提示重启后生效)。
    d->fgBackend = initial.fgBackend;
    SyncProxyRouterIni(d->fgDllPath, initial.fgRouter);

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
    // FG 状态(开关 + 后端 + 代理路径)同样参与:槽资源形态(FG 纹理有无)
    // 与后端选择(官方 NGX vs proxy)随之变化,必须冷重建 —— 面板切后端
    // 因此在下个 seek 当场生效,无需重启 mpv。
    // NR+FG 皆关 = 跳过热复用(实例纯直通,停泊上下文原样保留,重开秒回);
    // 仅 NR 关而 FG 开仍需热复用(设备与上下文都在用)。
    const bool hotMatch = (initial.nrEnabled || initial.fgEnabled) && Hot().valid &&
                          Hot().ngxDllPath == d->ngxDllPath &&
                          Hot().width == d->width && Hot().height == d->height &&
                          Hot().depth == d->depth &&
                          Hot().fgEnabled == (initial.fgEnabled != 0) &&
                          Hot().fgDllPath == d->fgDllPath &&
                          Hot().fgBackend == d->fgBackend;
    if (hotMatch) {
        d->d3d12 = std::move(Hot().d3d12);
        d->ngx = std::move(Hot().ngx);
        Hot().valid = false;
        Hot().ngxDllPath.clear();
        if (d->ngx->Rebind(d->params.get(), d->width, d->height, d->depth, err, sizeof(err))) {
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
    if ((initial.nrEnabled || initial.fgEnabled) && !d->initOk && !d->d3d12) {
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
                               d->width, d->height, d->depth, d->params.get(), err, sizeof(err))) {
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
    if (!initial.nrEnabled && !initial.fgEnabled) {
        // NR + FG 皆关:热/冷初始化全部跳过 —— 零设备、零显存、零 GPU,
        // 滤镜纯直通(getFrame 原帧交还)。仅 NR 关而 FG 开时不走此分支:
        // 初始化照常(补帧/光流需要设备与 NVOF),降噪评估由 ProcessFrame
        // 内部门控跳过。桥接照常启动:面板仍被拉起并可实时控制 —— 已激活
        // 会话 live 重开立即恢复;创建即全关的实例重开需下个 seek(停泊热
        // 上下文原样保留,同参数重开走秒回的热复用)。
        char body[192];
        std::snprintf(body, sizeof(body), "{\"%s\":\"passthrough\",\"%s\":\"NR+FG disabled (panel/vpy)\"}",
                      vsdlssnr::SK_FILTER_STATE, vsdlssnr::SK_STATE_DETAIL);
        vsdlssnr::PublishStatsJson(body);
        if (!vsdlssnr::BridgeStart(d->params.get())) {
            vsapi->logMessage(mtWarning,
                              "vs_dlssnr: parameter bridge failed to start; panel edits will not apply",
                              core);
            vsdlssnr::TimingStatusLine("DLSSNR STATUS: bridge start FAILED; panel edits will not apply");
        }
        vsdlssnr::TimingStatusLine("DLSSNR STATUS: NR+FG disabled; passthrough (zero GPU)");
    }

    // FG 多帧输出判定(两条初始化路径汇合):会话激活 = 每源帧产出 M 帧
    // (输出帧时长 = 源时长/M,mpv 逐帧认 _DurationNum/_DurationDen);
    // 否则 1:1(FG 失败的降级语义)。vi 副本按创建值倍增 fps —— 纯元数据
    // (mpv 不据此节拍,但下游工具/时长估算消费它)。
    d->fgActive = d->initOk && d->ngx && d->ngx->FgActive();
    VSVideoInfo viOut = *vi;
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
        "ui_correction:int:opt;"
        "residual_multiplier:float:opt;"
        "residual_saturation:float:opt;"
        "residual_lightness:float:opt;"
        "shadow_structure:float:opt;"
        "reflection_glow:float:opt;"
        "scaling_enabled:int:opt;"
        "input_resolution:int:opt;"
        "motion_vector_quality:int:opt;"
        "nvof_follow_scaling:int:opt;"
        "fg_enabled:int:opt;"
        "fg_multiplier:int:opt;"
        "fg_router:int:opt;"
        "fg_backend:int:opt;"
        "fg_dll:data:opt;",
        "clip:vnode;",
        DlssnrCreate, nullptr, plugin);
}
