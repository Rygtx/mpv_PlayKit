// stats 共享内存消费(2026-10-03 自 panel_app.cpp 拆出):进程级视图缓存、
// seq 门快照、死亡态清零与字段直读。HDR 打标线程(本件外)经
// ReadStatsSnapshot 共用同一份快照实现。

#include "panel_shared.h"

#include <cstring>
#include <cstdio>
#include <mutex>

// Stats 映射句柄/视图进程级缓存(2026-09-25):两个消费方(UI 10Hz + 打标
// 线程 ~4Hz)此前每拍 Open/Map/Unmap/Close 全套内核往返 + 页表工作;与插件
// 侧协议(映射句柄进程级保留,panel_ipc.h 注释明示)对齐。插件重启 = 同名
// 新 section(旧对象随创建者退出改名),靠 1Hz 新鲜度探针检出句柄失配后
// 重开 —— CompareObjectHandles 动态加载(Win10 1607+,拿不到则视为恒匹配,
// 退化为老语义:重启后等面板重启恢复)。
namespace {
HANDLE g_statsMapping = nullptr;
void *g_statsView = nullptr;
LARGE_INTEGER g_statsProbeQpc{};
bool g_statsCmpHandlesOk = true;
std::mutex g_statsCacheMutex; // 缓存初建/换新互斥(LoadStats 主线程 + 打标线程并发)

bool StatsCmpSameObject(HANDLE a, HANDLE b) noexcept {
    if (!g_statsCmpHandlesOk) return true; // 探针不可用:退化为恒匹配
    using Fn = BOOL(WINAPI *)(HANDLE, HANDLE);
    static Fn fn = []() -> Fn {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        return k ? reinterpret_cast<Fn>(GetProcAddress(k, "CompareObjectHandles")) : nullptr;
    }();
    if (!fn) {
        g_statsCmpHandlesOk = false;
        return true;
    }
    return fn(a, b) != FALSE;
}

// 调用方必须持 g_statsCacheMutex(ReadStatsSnapshot 全程持锁)。
bool EnsureStatsViewLocked() noexcept {
    if (g_statsView) {
        // 1Hz 探针:插件重启后同名 section 是新对象,旧视图会永久读到冻结
        // 快照 —— 检出句柄失配即换新。
        LARGE_INTEGER now{}, tf{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&tf);
        if (now.QuadPart - g_statsProbeQpc.QuadPart < tf.QuadPart) return true;
        g_statsProbeQpc = now;
        HANDLE fresh = OpenFileMappingW(FILE_MAP_READ, FALSE, STATS_MAPPING);
        if (!fresh) return true; // 暂时打不开:沿用旧视图(老语义也如此)
        if (StatsCmpSameObject(fresh, g_statsMapping)) {
            CloseHandle(fresh);
            return true;
        }
        UnmapViewOfFile(g_statsView);
        g_statsView = nullptr;
        CloseHandle(g_statsMapping);
        g_statsMapping = fresh;
    }
    if (!g_statsMapping) {
        g_statsMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, STATS_MAPPING);
        if (!g_statsMapping) return false;
    }
    if (!g_statsView) {
        g_statsView = MapViewOfFile(g_statsMapping, FILE_MAP_READ, 0, 0, PAYLOAD_SIZE);
        if (!g_statsView) {
            CloseHandle(g_statsMapping);
            g_statsMapping = nullptr;
            return false;
        }
        QueryPerformanceCounter(&g_statsProbeQpc);
    }
    return true;
}
} // namespace

// Stats 映射一拍快照(seq 门校验)。返回:2=有效(已拷入 *st);1=映射在
// 但快照未稳(seq 翻转中/未发布),*badMagic 带回 magic 是否失配;0=映射
// 不存在(插件未运行)。多读者安全:LoadStats(主线程,UI)与 HdrTagProc
// (打标线程)各自调用,seq 协议只保证"拷贝期间计数器不动";ensure+拷贝+
// 校验整体在 g_statsCacheMutex 内 —— 防止本线程拷贝期间另一线程的插件
// 重启换新路径 unmap 同一视图。10Hz+4Hz 的节拍下锁竞争可忽略。
int ReadStatsSnapshot(StatsPayload *st, bool *badMagic) noexcept {
    *badMagic = false;
    std::lock_guard<std::mutex> lock(g_statsCacheMutex);
    if (!EnsureStatsViewLocked()) return 0;
    const StatsPayload *view = static_cast<const StatsPayload *>(g_statsView);
    // 混部署(新面板 + 旧插件的小映射)时按映射实际区域钳制拷贝量:
    // 直接 sizeof(*st) 是标准意义上的越界读,分页粒度通常掩盖但不该赌。
    MEMORY_BASIC_INFORMATION mbi{};
    size_t copy = sizeof(*st);
    if (VirtualQuery(view, &mbi, sizeof(mbi)) && mbi.RegionSize > 0 &&
        mbi.RegionSize < sizeof(*st)) {
        copy = mbi.RegionSize;
    }
    memcpy(st, view, copy);
    *badMagic = st->magic != 0 && st->magic != STATS_MAGIC;
    const bool valid = st->magic == STATS_MAGIC && SeqStable(view, *st);
    return valid ? 2 : 1;
}

// 会话状态统一清零(断连 / GPU 挂起 / 死亡 body 三条路径共用):残留旧值
// 会让状态行说谎。标题行(statsBig/statsRes)、分段显示值与帧率语义随路径
// 不同,由各调用点自行处理。
void ClearSessionState() noexcept {
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
    g_app.evalActive = g_app.ofActive = g_app.scalingActive = 0;
    g_app.gateSkips = g_app.gateExpired = g_app.gateResets = 0;
    g_app.temporalState[0] = 0;
    g_app.temporalRoute = 0;
    g_app.temporalW = 0.0f;
}

// Read the plugin's per-frame stats from named shared memory (zero disk IO).
// Mapping absent = no live filter: clear the stats line. The mapping object
// dies with the plugin process, so re-open each refresh (no stale handles)
// —— 快照读取本体在 ReadStatsSnapshot(HDR 打标线程共用同一份实现)。
void LoadStats() noexcept {
    StatsPayload st{};
    bool badMagic = false;
    const int snap = ReadStatsSnapshot(&st, &badMagic);
    if (snap == 0) {
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
        ClearSessionState();
        g_app.connState = 0; // 连接指示随之变化(statsDirty 已置)
        return;
    }
    if (snap != 2) {
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
    // 字段直读。空/零字段 = 本 body 未携带(死亡/简体 body 只填部分字段),
    // 等价旧 JSON 的"缺键即清"语义:残留旧值会让状态行说谎。
    CopyStatStr(g_app.filterState, st.filterState);
    CopyStatStr(g_app.stateDetail, st.stateDetail);
    CopyStatStr(g_app.ofMode, st.ofMode);
    CopyStatStr(g_app.fgState, st.fgState);
    g_app.fgMult = static_cast<int>(st.fgMult);
    // FG 实际路由/创建倍数/失败原因 + 排队细分(死亡 body 不带这些字段,
    // 零/空即清:与 filterState 同款规则)。
    CopyStatStr(g_app.fgRouteEff, st.fgRouteEff);
    g_app.fgMultCreate = static_cast<int>(st.fgMultCreate);
    CopyStatStr(g_app.fgDetail, st.fgDetail);
    // v21 字段:运行库上限 + 光流失败原因(与 fgDetail 同款规则)。
    g_app.fgMultMax = static_cast<int>(st.fgMultMax);
    CopyStatStr(g_app.ofDetail, st.ofDetail);
    // v22 字段:RTX Video 实态 + 失败原因(同款规则)。
    CopyStatStr(g_app.rtxState, st.rtx);
    CopyStatStr(g_app.rtxDetail, st.rtxDetail);
    // HDR 打标不再走这里:已独立为 HdrTagProc 线程(本函数只在窗口可见时
    // 被调,曾把打标一并拖进"隐藏即休眠"的门里)。
    g_app.slotWait = st.slotWait;
    g_app.lockWait = st.lockWait;
    // 每帧实效位(DSL9;死亡/简体 body 不带 = 清零,与 slotWait 同规则)。
    g_app.evalActive = static_cast<int>(st.evalActive);
    g_app.ofActive = static_cast<int>(st.ofActive);
    g_app.scalingActive = static_cast<int>(st.scalingActive);
    g_app.gateSkips = static_cast<int>(st.gateSkips);
    g_app.gateExpired = static_cast<int>(st.gateExpired);
    g_app.gateResets = static_cast<int>(st.gateResets);
    // 抗闪烁三字段(v26):状态串 + 实际生效档 + 最近混合权重。
    CopyStatStr(g_app.temporalState, st.temporal);
    g_app.temporalRoute = static_cast<int>(st.temporalRoute);
    g_app.temporalW = st.temporalW;
    if (st.gpuHang != 0) {
        // The hang body has no gpuLast (sentinel -1), so the gate below
        // would keep showing frozen pre-hang stats forever; surface it —
        // with the device-removal reason the plugin publishes alongside.
        snprintf(g_app.statsBig, sizeof(g_app.statsBig), "GPU 挂起/设备移除(滤镜已回退)");
        if (st.removedReason[0]) {
            snprintf(g_app.statsRes, sizeof(g_app.statsRes), "移除原因 %s", st.removedReason);
        }
        // hang 有自己的展示行,清掉状态字段防上一 body 的残留
        ClearSessionState();
    } else {
        const double gpuLast = st.gpuLast;
        if (gpuLast >= 0) {
            snprintf(g_app.statsBig, sizeof(g_app.statsBig), "NGX 延迟 %.1f ms", gpuLast);
            // 分辨率展示:VSR 生效(rtx 字段携带 "vsr WxH"/"vsr+hdr WxH")
            // -> 源 → VSR 输出;未开启缩放 -> 原生分辨率;开启 -> 处理分辨率
            // → 回源分辨率。width/height 恒为源尺寸,不含 VSR 输出。
            // 内部评估档真参与(DSL9 实效位直读;原按面板意图镜像"NR 参与
            // 才为真"的推导已删 —— 插件改直连条件,面板自动跟上)。
            const int iw = static_cast<int>(st.internalW);
            const int ih = static_cast<int>(st.internalH);
            const int w = static_cast<int>(st.width);
            const int h = static_cast<int>(st.height);
            const bool scalingLive = g_app.scalingActive != 0 && iw > 0 && ih > 0;
            // rtx 字段格式(dlssnr_context.cpp):"vsr WxH"/"vsr+hdr WxH"/
            // "hdr WxH"/"off" —— 跳到首个数字 sscanf WxH;off 无数字得 0。
            int rw = 0, rh = 0;
            {
                const char *p = st.rtx;
                while (*p && (*p < '0' || *p > '9')) ++p;
                if (*p) sscanf(p, "%dx%d", &rw, &rh);
            }
            if (rw > 0 && rh > 0 && (rw != w || rh != h)) {
                // VSR 生效:内部评估 → 回源 → VSR 输出,箭头链
                if (scalingLive) {
                    snprintf(g_app.statsRes, sizeof(g_app.statsRes),
                             "分辨率 %dx%d → %dx%d → %dx%d",
                             iw, ih, w, h, rw, rh);
                } else {
                    snprintf(g_app.statsRes, sizeof(g_app.statsRes),
                             "分辨率 %dx%d → %dx%d", w, h, rw, rh);
                }
            } else if (scalingLive) {
                snprintf(g_app.statsRes, sizeof(g_app.statsRes),
                         "分辨率 %dx%d → %dx%d", iw, ih, w, h);
            } else {
                snprintf(g_app.statsRes, sizeof(g_app.statsRes),
                         "分辨率 %dx%d(原生)", w, h);
            }
            // 八段读每帧 last 值(与 NGX 延迟同语义):EMA 稳态冻结,
            // last 随帧呼吸(见 panel_ipc.h StatsPayload 计时字段注释)。
            // **裸 last 恒存 segX(真实数据不平滑);EMA 只推进显示值
            // segDispX(用户裁定)**:分段计时锚存在 CPU 唤醒竞争 —— 极快的
            // 段(vsr ~0.5ms)会因 CPU 迟到测得瞬时 0,时间线忽隐忽现。
            // α=0.35 ≈ 3-4 个更新周期收敛;持续为零的段(关开关)数秒内
            // 衰减回隐藏阈值之下,过渡平滑。
            constexpr float kSegAlpha = 0.35f;
            auto segSmooth = [kSegAlpha](float prev, float v) {
                return prev + (v - prev) * kSegAlpha;
            };
            const float rawPack = st.packLast;
            const float rawEval = st.evalCpuLast;
            const float rawGpu = st.gpuLast;
            const float rawUnpack = st.unpackLast;
            const float rawNvof = st.ofLast;
            const float rawFg = st.fgLast;
            const float rawRtxVsr = st.rtxVsrLast;
            const float rawRtxHdr = st.rtxHdrLast;
            const float rawConv = st.convLast;
            const float rawQueue = st.queueLast;
            g_app.segDispPack = segSmooth(g_app.segDispPack, rawPack);
            g_app.segDispEval = segSmooth(g_app.segDispEval, rawEval);
            g_app.segDispGpu = segSmooth(g_app.segDispGpu, rawGpu);
            g_app.segDispUnpack = segSmooth(g_app.segDispUnpack, rawUnpack);
            g_app.segDispNvof = segSmooth(g_app.segDispNvof, rawNvof);
            g_app.segDispFg = segSmooth(g_app.segDispFg, rawFg);
            g_app.segDispRtxVsr = segSmooth(g_app.segDispRtxVsr, rawRtxVsr);
            g_app.segDispRtxHdr = segSmooth(g_app.segDispRtxHdr, rawRtxHdr);
            g_app.segDispConv = segSmooth(g_app.segDispConv, rawConv);
            g_app.segDispQueue = segSmooth(g_app.segDispQueue, rawQueue);
            // 时间线可见性 = 任一段有处理时间(平滑值合计 > 0):EMA 衰减
            // 期内仍算"插件在运作",过渡平滑;全关直通/死亡 body 走下方
            // 清零分支归 false。原判据 segGpu>0 与 9729c84 冲突 —— NR 关
            // 直连帧 gpu 段恒 0(空栅栏等待不是 NR 处理时间),把仍真实
            // 非零的 fg/conv/pack/unpack 连坐收起,表现为"NR 一关,其他
            // 功能的处理用时全没了"。渲染侧无下限门:非 0ms 段全显示。
            g_app.hasSegments = (g_app.segDispPack + g_app.segDispEval + g_app.segDispGpu +
                                 g_app.segDispUnpack + g_app.segDispNvof + g_app.segDispFg +
                                 g_app.segDispRtxVsr + g_app.segDispRtxHdr + g_app.segDispConv +
                                 g_app.segDispQueue) > 0.0f;
            g_app.fps = st.fps;
            CopyStatStr(g_app.gpuName, st.gpuName);
            CopyStatStr(g_app.modelDll, st.modelDll);
        } else {
            // 死亡 body(passthrough / ngx_faulted):清掉冻结的旧统计与
            // 分段,让状态行成为唯一内容。不得调 ClearSessionState():
            // 上方字段直读已把会话字段同步成本 body 的实值(passthrough
            // body 携带 filterState/stateDetail)—— 此处再清空会把
            // "passthrough" 抹成 "",面板勾 NR 的 needsReseek 判据与
            // kStateNrSeekInit 闭环双双失明,全关实例勾 NR 永不自动重建
            // (2026-10-03 实锤:直通会话勾选瞬间 g_app.filterState=='')。
            g_app.statsBig[0] = 0;
            g_app.statsRes[0] = 0;
            g_app.segDispPack = g_app.segDispEval = g_app.segDispGpu = g_app.segDispUnpack =
            g_app.segDispNvof = g_app.segDispFg = g_app.segDispRtxVsr = g_app.segDispRtxHdr =
            g_app.segDispConv = g_app.segDispQueue = 0.0f;
            g_app.hasSegments = false;
            g_app.fps = 0.0;
        }
    }
    // 重画门:before 是 LoadStats 入口的全结构快照,期间只写展示字段 ——
    // 整体比较一次即可(AppState 平凡可拷贝,快照含 padding 逐位一致)。
    g_app.statsDirty = memcmp(&before, &g_app, sizeof(AppState)) != 0;
}
