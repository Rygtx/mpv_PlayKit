// mpv 管道 IPC(2026-10-03 自 panel_app.cpp 拆出):一次性命令(reseek)与
// HDR 打标持久连接两条路径此前各写一份候选连接循环,连接/发送层已收口
// mpv_pipe_common(MpvPipeOpen/MpvPipeSendBounded;2026-10-05 起与插件
// resize watcher 同一份实现)。

#include "panel_shared.h"
#include "mpv_pipe_common.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>

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
// 候选管道名解析(mpv.conf 的 input-ipc-server 优先,默认名/umpv 兜底),
// reseek 与 HDR 打标两条 IPC 路径共用。返回 _wcsdup 的解析名(可 nullptr,
// free(nullptr) 恒安全),candidates[4] 就绪;返回值须活过使用期。
// 解析结果缓存(2026-09-25):成功命中后恒定缓存(同名命中不再重读);
// 失败(未启用 IPC 等)留 1s 失败戳 —— 打标线程未连接时 ~4Hz 全量重读并
// 解析 16KB mpv.conf 是永久性文件 IO,降到 ≤1Hz。conf 中途改动需面板重启
// 才被看到(此前是下拍生效 —— 差异可接受,该场景实为"面板先于 mpv 启动"
// 的主路径,首次失败 1s 后照常重读,覆盖 mpv 晚起)。
namespace {
wchar_t *g_pipeResolved = nullptr; // 成功命中缓存
LARGE_INTEGER g_pipeFailQpc{};
bool g_pipeFailStamped = false;

wchar_t *ResolveMpvPipeCandidates(const wchar_t **candidates) noexcept {
    // 默认兜底名收口 mpv_pipe_common(与插件 resize watcher 同一份,
    // 2026-10-05 —— 候选列表/连接/有界发送三条轴此前各写一份)。
    MpvPipeDefaultNames(candidates);
    if (g_pipeResolved) {
        candidates[0] = g_pipeResolved;
        return _wcsdup(g_pipeResolved);
    }
    {
        LARGE_INTEGER now{}, tf{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&tf);
        if (g_pipeFailStamped &&
            now.QuadPart - g_pipeFailQpc.QuadPart < tf.QuadPart) {
            candidates[1] = candidates[2] = candidates[3] = nullptr;
            return nullptr; // 失败后 1s 内连默认名都跳过(整拍零 IO)
        }
    }
    wchar_t base[MAX_PATH];
    if (!BasePath(base, MAX_PATH)) return nullptr;

    // conf 解析收口到 mpv_pipe_common(与插件 resize watcher 共用,2026-10-04
    // —— 此前两份手写解析,插件侧压根没有,自定义管道名下静默分叉)。
    wchar_t confPath[MAX_PATH];
    swprintf_s(confPath, L"%s\\..\\portable_config\\mpv.conf", base);
    wchar_t *parsedName = MpvParseIpcServerName(confPath);
    if (parsedName) candidates[0] = parsedName;

    if (parsedName) {
        g_pipeResolved = _wcsdup(parsedName); // 命中缓存(进程期有效)
    } else {
        QueryPerformanceCounter(&g_pipeFailQpc);
        g_pipeFailStamped = true; // 失败戳:1s 内调用方短路
    }
    return parsedName;
}

// 候选管道连接/有界发送已收口 mpv_pipe_common(MpvPipeOpen/MpvPipeSendBounded,
// 2026-10-05;与插件 resize watcher 同一份实现)。
} // namespace

// 面板→mpv 原地重载请求(唯一消费者 = reseek;原"通用 MpvIpcSendCmd"单
// 调用者壳已内联,2026-10-04)。候选管道逐个尝试,写命令 + 排空回执。
// 写/读均为 overlapped + 500ms 有界等待(2026-09-25):原实现同步无超时,
// mpv 活着但不回包(渲染挂起)时 ReadFile 永久阻塞 —— 主线程被拖死 =
// 面板 UI 整体冻结。
bool TriggerMpvReseek() noexcept {
    // 原地微 seek(1ms 向前,exact)触发 vf_vapoursynth 整脚本重建,
    // 新实例在 create 时采纳刚发布的 payload create-time 三元组
    // (vsrMode/scale/hdr)。实测播放/暂停两态均稳定重建(2026-09-23
    // 4/4);曾误判"seek 不重建"(23:02 风暴零新实例)—— 对照测试
    // 推翻,该次异常归因于管道归属的环境性歧义,机制本身有效。
    static const char kSeekCmd[] = "{\"command\":[\"seek\",\"0.001\",\"relative+exact\"]}\n";
    const wchar_t *candidates[4];
    wchar_t *parsedName = ResolveMpvPipeCandidates(candidates);

    bool ok = false;
    for (int i = 0; i < 4 && !ok; ++i) {
        if (!candidates[i] || !candidates[i][0]) continue;
        // 逐候选连接(单一候选指针 = 逐候选语义;写失败继续下一候选)。
        HANDLE pipe = MpvPipeOpen(&candidates[i], /*overlapped=*/true, nullptr, 0);
        if (!pipe) continue;
        ok = MpvPipeSendBounded(pipe, kSeekCmd, sizeof(kSeekCmd) - 1);
        CloseHandle(pipe);
        if (ok) {
            PanelLog("panel: mpv reseek via IPC pipe %ls", candidates[i]);
        }
    }
    free(parsedName);
    if (!ok) {
        PanelLog("panel: mpv IPC reseek unavailable (input-ipc-server off? mpv closed?); falling back to in-session live");
    }
    return ok;
}

// ---- HDR 输出打标同步(独立后台线程)-------------------------------------
// vf_vapoursynth 不透传 VS 帧 props(mpv 源码实锤:vs_frame_done 只认
// _DurationNum/_Den),插件 HDR 输出(YUV420P10 BT.2020 PQ)到 mpv 手里
// 仍无标签 → 按 bt.709/bt.1886 解读 = 白/品红二色画面。插件 HDR 实态时给
// mpv vf 链追加 @dlssnr-hdr-tag(lavfi setparams 打 BT.2020 PQ 元数据),
// SDR 实态移除(8bit 输出不得标成 PQ)。标签参数 = 插件输出契约的镜像,
// 固定不可配置。
//
// 同拍同步 target-colorspace-hint:打标只声明"数据是 PQ",显示端切 HDR
// 由该属性决定(0.41 起 auto/no/yes 三档;no = 恒色调映射进 SDR,不上
// HDR 亮度)。加标拍设 yes,摘标拍还原连接时读回的 mpv 实值(get 实测
// 序列化:auto→"auto" 字符串,no/yes→JSON 布尔,三态都接;读不到按 auto
// 兜底)。mpv.conf 零配置。
//
// 2026-09-24 从主循环 stats tick 独立成线程:面板隐藏进托盘后主循环
// WaitMessage 全休眠,stats tick 不可达 —— 曾表现为"开 HDR 不点托盘
// 图标就永远打不上标"。worker 每 250ms 自读 stats 快照(不经 g_app:
// 主线程只在窗口可见时刷新它,隐藏期间是陈旧值),快照无效(插件未跑)
// 则本拍跳过,不闩锁。
//
// mpv 重启感知 = 持久 IPC 连接的句柄生命周期:管道实例随 mpv 进程销毁,
// 任何 Write/Read 失败 → 关连接、lastTagState 打回 -1(未知);重连成功
// 后必发一次(即使 want 未变)= 新实例补标。不需要轮询 vf / 实例 ID。
// 已知残留:mpv 卡死时 ReadFile 等 ack 会挂住本线程(独立线程,不连坐
// UI);用户手动 vf remove 不感知(覆盖需对账 ack,协议面不值),
// hdr_tag_manual.py 是手动兜底。

namespace {

// 对已连接管道发一条命令并吞 ack。任一步失败 = 管道已断(mpv 死亡/重启),
// 调用方关连接下拍重连。cmd 须自带行尾 \n。ok 判定只看写入+读回,不解析
// ack 内容(维持原语义)。
bool HdrTagSendOnPipe(HANDLE pipe, const char *cmd) noexcept {
    DWORD written = 0, got = 0;
    char ack[128]{};
    return WriteFile(pipe, cmd, static_cast<DWORD>(strlen(cmd)), &written, nullptr) &&
           written == strlen(cmd) &&
           ReadFile(pipe, ack, sizeof(ack) - 1, &got, nullptr) != FALSE;
}

// 读回 target-colorspace-hint 实值("auto"/"yes"/"no")到 out。入口先写
// "auto" 兜底,故返回时 out 恒非空(调用方无需判空)。
void HdrTagReadHint(HANDLE pipe, char *out, size_t cap) noexcept {
    static const char kCmd[] =
        "{\"command\":[\"get_property\",\"target-colorspace-hint\"]}\n";
    strcpy_s(out, cap, "auto"); // mpv 0.41+ 默认档
    char ack[256]{};
    DWORD written = 0, got = 0;
    if (!WriteFile(pipe, kCmd, static_cast<DWORD>(sizeof(kCmd) - 1), &written, nullptr) ||
        written != sizeof(kCmd) - 1 ||
        ReadFile(pipe, ack, sizeof(ack) - 1, &got, nullptr) == FALSE)
        return;
    const char *d = strstr(ack, "\"data\":");
    if (!d) return;
    d += 7;
    if (strncmp(d, "true", 4) == 0) strcpy_s(out, cap, "yes");
    else if (strncmp(d, "false", 5) == 0) strcpy_s(out, cap, "no");
    else if (*d == '"') {
        const char *e = strchr(d + 1, '"');
        if (e && static_cast<size_t>(e - d - 1) < cap) {
            memcpy(out, d + 1, static_cast<size_t>(e - d - 1));
            out[e - d - 1] = '\0';
        }
    }
}

struct HdrTagConn {
    HANDLE pipe = nullptr; // 持久连接:句柄存活 = 同一 mpv 会话
    wchar_t name[64]{};    // 当前管道名(conf 解析命中值,仅日志)
    int lastTagState = -1; // -1 未知(连接建立前/断开后)→ 重连必发
    char initialHint[8]{}; // 连接时读回的 target-colorspace-hint 实值,摘标还原用
};

// 连接(若无)并把打标状态推到 want。返回 true = 连接存活且状态已对齐。
bool HdrTagTick(HdrTagConn &c, int want) noexcept {
    if (!c.pipe) {
        const wchar_t *candidates[4];
        wchar_t *parsedName = ResolveMpvPipeCandidates(candidates);
        c.pipe = MpvPipeOpen(candidates, /*overlapped=*/false,
                             c.name, std::size(c.name));
        free(parsedName);
        if (!c.pipe) return false; // mpv 不在/IPC 未起:下拍重试,不闩锁
        c.lastTagState = -1;       // 新会话一律视为未知 → 必发
        HdrTagReadHint(c.pipe, c.initialHint, sizeof(c.initialHint));
        PanelLog("panel: hdr tag worker connected to pipe %ls (colorspace-hint=%hs)",
                 c.name, c.initialHint);
    }
    if (c.lastTagState == want) return true;
    char cmd[256];
    if (want) {
        snprintf(cmd, sizeof(cmd),
                 "{\"command\":[\"vf\",\"add\","
                 "\"@dlssnr-hdr-tag:lavfi=[setparams=colorspace=bt2020nc:"
                 "color_primaries=bt2020:color_trc=smpte2084]\"]}\n");
    } else {
        snprintf(cmd, sizeof(cmd),
                 "{\"command\":[\"vf\",\"remove\",\"@dlssnr-hdr-tag\"]}\n");
    }
    if (HdrTagSendOnPipe(c.pipe, cmd)) {
        c.lastTagState = want;
        // 打标同拍把显示端拉到位:加标 = yes(上屏 HDR),摘标 = 还原连接
        // 时读回的 mpv 实值(不写死 no —— conf 设了 auto 的用户不被降档)。
        // 失败 = 管道断裂,走下方统一重连路径,下拍整组重发。
        char hintCmd[160];
        snprintf(hintCmd, sizeof(hintCmd),
                 "{\"command\":[\"set_property\",\"target-colorspace-hint\",\"%s\"]}\n",
                 want ? "yes" : (c.initialHint[0] ? c.initialHint : "auto"));
        if (!HdrTagSendOnPipe(c.pipe, hintCmd)) {
            PanelLog("panel: hdr tag pipe broken (hint sync) -> reconnect next tick");
            CloseHandle(c.pipe);
            c.pipe = nullptr;
            c.lastTagState = -1;
            return false;
        }
        PanelLog("panel: hdr vf tag %ls + colorspace-hint via %ls",
                 want ? L"added" : L"removed", c.name);
        return true;
    }
    PanelLog("panel: hdr tag pipe broken -> reconnect next tick");
    CloseHandle(c.pipe);
    c.pipe = nullptr;
    c.lastTagState = -1;
    return false;
}
} // namespace

// 打标 worker:want 来自插件本体的 StatsPayload.rtx 实态,与 UI 可见性完全解耦。
// 退出轮询 g_quit(与看门狗同款);进程退出会硬杀本线程,残留管道句柄
// 随进程回收,无需 join。(外链:wWinMain 在本件外拉起线程。)
DWORD WINAPI HdrTagProc(LPVOID) noexcept {
    HdrTagConn conn;
    for (;;) {
        Sleep(250);
        if (g_quit) break;
        StatsPayload st{};
        bool badMagic = false;
        if (ReadStatsSnapshot(&st, &badMagic) != 2) continue; // 插件未跑/快照未稳
        // HDR 打标直读实效位(DSLA;原 strstr 人读串 parse 在插件改显示
        // 格式时会静默错)。rtxHdrActive 缺省 0(passthrough 简体 body 不带)
        // = SDR 实态,发 remove 摘标 —— 与旧"rtx 字段为空"判据同语义。
        HdrTagTick(conn, st.rtxHdrActive ? 1 : 0);
    }
    if (conn.pipe) CloseHandle(conn.pipe);
    return 0;
}
