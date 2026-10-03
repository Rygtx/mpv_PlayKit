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
    // 直拷(2026-10-04:原 VirtualQuery 钳制分支恒假 —— 映射区域页粒度
    // ≥4096,恒覆盖 sizeof(StatsPayload)≈1.1K;混部署错配由下方 magic
    // 校验拒读,防的是"读到错版本数据"而非"读到映射外")。
    memcpy(st, view, sizeof(*st));
    *badMagic = st->magic != 0 && st->magic != STATS_MAGIC;
    const bool valid = st->magic == STATS_MAGIC && SeqStable(view, *st);
    return valid ? 2 : 1;
}

// 会话状态统一清零(断连 / GPU 挂起两条路径共用):残留旧值会让状态行
// 说谎。整快照清(2026-10-04 AppState 快照化:逐字段手抄清单随 StatsPayload
// 增字段必然失序)。标题行(statsBig/statsRes)与分段显示值语义随路径
// 不同,由各调用点自行处理。
void ClearSessionState() noexcept {
    g_app.snap = StatsPayload{};
}

// Read the plugin's per-frame stats from named shared memory (zero disk IO).
// Mapping absent = no live filter: clear the stats line. The mapping object
// dies with the plugin process, so re-open each refresh (no stale handles)
// —— 快照读取本体在 ReadStatsSnapshot(HDR 打标线程共用同一份实现)。
void LoadStats() noexcept {
    StatsPayload st{};
    bool badMagic = false;
    const int snap = ReadStatsSnapshot(&st, &badMagic);
    const AppState before = g_app; // redraw gate: entry snapshot, compared below
    if (snap == 0) {
        // 无映射 = 无存活滤镜:清标题 + 整快照清,重画门统一判定。
        g_app.statsBig[0] = 0;
        g_app.statsRes[0] = 0;
        ClearSessionState();
        g_app.connState = 0;
    } else if (snap != 2) {
        // magic 不符 = 面板与插件版本未成对更新:不再静默冻结旧显示,
        // 连接指示红显("看起来活着但调参无效"的最短诊断路径)。
        if (badMagic) g_app.connState = 2;
    } else {
        g_app.connState = 1; // 0→1 跳变进下方重画门
        // 整快照落账(2026-10-04):body 未携带的字段本就是发布侧零值,
        // 整拷 = 旧 JSON"缺键即清"语义。HDR 打标不在此:HdrTagProc 自读
        // (本函数只在窗口可见时被调,曾把打标一并拖进"隐藏即休眠"门)。
        g_app.snap = st;
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
                // 分辨率展示:VSR 生效(rtxVsrActive 实效位 + rtxOut 数值尺寸,
                // DSLA 起不再 parse 人读串)→ 源 → VSR 输出;未开启缩放 ->
                // 原生分辨率;开启 -> 处理分辨率 → 回源分辨率。width/height 恒
                // 为源尺寸,不含 VSR 输出。
                // 内部评估档真参与(DSL9 实效位直读;原按面板意图镜像"NR 参与
                // 才为真"的推导已删 —— 插件改直连条件,面板自动跟上)。
                const int iw = static_cast<int>(st.internalW);
                const int ih = static_cast<int>(st.internalH);
                const int w = static_cast<int>(st.width);
                const int h = static_cast<int>(st.height);
                const bool scalingLive = st.scalingActive != 0 && iw > 0 && ih > 0;
                const int rw = static_cast<int>(st.rtxOutW);
                const int rh = static_cast<int>(st.rtxOutH);
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
                g_app.segDispPack = segSmooth(g_app.segDispPack, st.packLast);
                g_app.segDispEval = segSmooth(g_app.segDispEval, st.evalCpuLast);
                g_app.segDispGpu = segSmooth(g_app.segDispGpu, st.gpuLast);
                g_app.segDispUnpack = segSmooth(g_app.segDispUnpack, st.unpackLast);
                g_app.segDispNvof = segSmooth(g_app.segDispNvof, st.ofLast);
                g_app.segDispFg = segSmooth(g_app.segDispFg, st.fgLast);
                g_app.segDispRtxVsr = segSmooth(g_app.segDispRtxVsr, st.rtxVsrLast);
                g_app.segDispRtxHdr = segSmooth(g_app.segDispRtxHdr, st.rtxHdrLast);
                g_app.segDispConv = segSmooth(g_app.segDispConv, st.convLast);
                g_app.segDispQueue = segSmooth(g_app.segDispQueue, st.queueLast);
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
            } else {
                // 死亡 body(passthrough / ngx_faulted):清掉冻结的旧统计与
                // 分段,让状态行成为唯一内容。不得清 g_app.snap:整快照直读
                // 已把会话字段同步成本 body 的实值(passthrough body 携带
                // filterState/stateDetail)—— 此处再清空会把 "passthrough"
                // 抹成 "",面板勾 NR 的 needsReseek 判据与 kStateNrSeekInit
                // 闭环双双失明,全关实例勾 NR 永不自动重建(2026-10-03 实锤:
                // 直通会话勾选瞬间 snap.filterState=='')。fps 同理:死亡
                // body 携带 0,整拷已清。
                g_app.statsBig[0] = 0;
                g_app.statsRes[0] = 0;
                g_app.segDispPack = g_app.segDispEval = g_app.segDispGpu = g_app.segDispUnpack =
                g_app.segDispNvof = g_app.segDispFg = g_app.segDispRtxVsr = g_app.segDispRtxHdr =
                g_app.segDispConv = g_app.segDispQueue = 0.0f;
                g_app.hasSegments = false;
            }
        }
    }
    // 重画门:before 是 LoadStats 入口的全结构快照,期间只写展示字段 ——
    // 整体比较一次即可(AppState 平凡可拷贝,快照含 padding 逐位一致;
    // seq 翻转中/无变化的拍 = 恒等 → 不重画)。
    g_app.statsDirty = memcmp(&before, &g_app, sizeof(AppState)) != 0;
}
