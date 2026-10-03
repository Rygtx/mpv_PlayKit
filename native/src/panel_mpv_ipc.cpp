// mpv 管道 IPC(2026-10-03 自 panel_app.cpp 拆出):一次性命令(reseek)与
// HDR 打标持久连接两条路径此前各写一份候选连接循环,本件连同连接层一起
// 收口(OpenMpvPipe)。

#include "panel_shared.h"

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
    candidates[0] = nullptr;
    candidates[1] = L"mpvpipe";
    candidates[2] = L"mpvsocket";
    candidates[3] = L"umpv";
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

    wchar_t *parsedName = nullptr; // _wcsdup;末尾 free(nullptr) 恒安全
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
                                parsedName = _wcsdup(parsed);
                                candidates[0] = parsedName;
                            }
                        }
                        break;
                    }
                }
                pos = eol + 1;
            }
        }
    }

    if (parsedName) {
        g_pipeResolved = _wcsdup(parsedName); // 命中缓存(进程期有效)
    } else {
        QueryPerformanceCounter(&g_pipeFailQpc);
        g_pipeFailStamped = true; // 失败戳:1s 内调用方短路
    }
    return parsedName;
}

// 候选管道连接(一次性命令与打标持久连接两客户端共用;此前各写一份
// 候选循环):按序尝试 CreateFileW,命中即返回。overlapped = 命令路径
// (500ms 有界 IO);打标走阻塞形态(线程私有连接,生命周期语义自管)。
// nameOut 可空;调用方传单一候选指针即得"逐候选"语义。
static HANDLE OpenMpvPipe(const wchar_t *const *candidates, bool overlapped,
                          wchar_t *nameOut, size_t nameLen) noexcept {
    for (int i = 0; i < 4; ++i) {
        if (!candidates[i] || !candidates[i][0]) continue;
        wchar_t pipePath[MAX_PATH];
        swprintf_s(pipePath, L"\\\\.\\pipe\\%s", candidates[i]);
        HANDLE pipe = CreateFileW(pipePath, GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING,
                                  overlapped ? FILE_FLAG_OVERLAPPED : 0,
                                  nullptr);
        if (pipe == INVALID_HANDLE_VALUE) continue;
        if (nameOut && nameLen) swprintf_s(nameOut, nameLen, L"%ls", candidates[i]);
        return pipe;
    }
    return nullptr;
}
} // namespace

// 面板→mpv IPC 单条命令:候选管道逐个尝试,写命令 + 读回执。cmd 须自带
// 行尾 \n。命中管道名经 hitOut 带回。
// 写/读均为 overlapped + 500ms 有界等待(2026-09-25):原实现同步无超时,
// mpv 活着但不回包(渲染挂起)时 ReadFile 永久阻塞 —— 主线程 reseek 路径
// 与打标线程共用本函数,主线程被拖死 = 面板 UI 整体冻结。
bool MpvIpcSendCmd(const char *cmd, wchar_t *hitOut, size_t hitLen) noexcept {
    const wchar_t *candidates[4];
    wchar_t *parsedName = ResolveMpvPipeCandidates(candidates);

    bool ok = false;
    for (int i = 0; i < 4 && !ok; ++i) {
        if (!candidates[i] || !candidates[i][0]) continue;
        // 逐候选连接(单一候选指针 = 逐候选语义;写失败继续下一候选)。
        HANDLE pipe = OpenMpvPipe(&candidates[i], /*overlapped=*/true, nullptr, 0);
        if (!pipe) continue;
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ev) {
            CloseHandle(pipe);
            continue;
        }
        OVERLAPPED ov{};
        ov.hEvent = ev;
        const DWORD cmdLen = static_cast<DWORD>(strlen(cmd));
        DWORD written = 0;
        // 写入有界等待:ERROR_IO_PENDING 后等事件,超时视为该候选失败。
        const bool wrote =
            WriteFile(pipe, cmd, cmdLen, &written, &ov) ||
            (GetLastError() == ERROR_IO_PENDING &&
             WaitForSingleObject(ev, 500) == WAIT_OBJECT_0 &&
             GetOverlappedResult(pipe, &ov, &written, FALSE));
        ok = wrote && written == cmdLen;
        if (ok) {
            // 回执只做排空,成败不影响 ok(原语义);超时按无回执放行。
            ResetEvent(ev);
            char ack[128]{};
            DWORD got = 0;
            if (!ReadFile(pipe, ack, sizeof(ack) - 1, &got, &ov) &&
                GetLastError() == ERROR_IO_PENDING &&
                WaitForSingleObject(ev, 500) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &ov);
                GetOverlappedResult(pipe, &ov, &got, TRUE); // 收割中止态,防悬悬
            }
        } else {
            CancelIoEx(pipe, &ov);
            DWORD got = 0;
            GetOverlappedResult(pipe, &ov, &got, TRUE);
        }
        CloseHandle(ev);
        CloseHandle(pipe);
        if (ok && hitOut && hitLen) {
            swprintf_s(hitOut, hitLen, L"%ls", candidates[i]);
        }
    }
    free(parsedName);
    return ok;
}

bool TriggerMpvReseek() noexcept {
    // 原地微 seek(1ms 向前,exact)触发 vf_vapoursynth 整脚本重建,
    // 新实例在 create 时采纳刚发布的 payload create-time 三元组
    // (vsrMode/scale/hdr)。实测播放/暂停两态均稳定重建(2026-09-23
    // 4/4);曾误判"seek 不重建"(23:02 风暴零新实例)—— 对照测试
    // 推翻,该次异常归因于管道归属的环境性歧义,机制本身有效。
    wchar_t hit[64];
    if (MpvIpcSendCmd("{\"command\":[\"seek\",\"0.001\",\"relative+exact\"]}\n",
                      hit, 64)) {
        PanelLog("panel: mpv reseek via IPC pipe %ls", hit);
        return true;
    }
    PanelLog("panel: mpv IPC reseek unavailable (input-ipc-server off? mpv closed?); falling back to in-session live");
    return false;
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
        c.pipe = OpenMpvPipe(candidates, /*overlapped=*/false,
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
        // rtx 字段为空 ≠ 死 body:NR+FG+RTX 皆关时插件发布 passthrough 简体,
        // 本身不带 rtx(2026-09-24 实锤:关 HDR 后标签残留)—— 空即 SDR
        // 实态,发 remove 摘标。
        HdrTagTick(conn, strstr(st.rtx, "hdr") ? 1 : 0);
    }
    if (conn.pipe) CloseHandle(conn.pipe);
    return 0;
}
