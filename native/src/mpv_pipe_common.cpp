// mpv.conf input-ipc-server 解析(实现;契约见 mpv_pipe_common.h)。
#include "mpv_pipe_common.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace vsdlssnr {

wchar_t *MpvParseIpcServerName(const wchar_t *confPath) noexcept {
    if (!confPath) return nullptr;
    FILE *f = nullptr;
    if (_wfopen_s(&f, confPath, L"rb") != 0 || !f) return nullptr;
    // 整文件读取(上限 1MB,与 dlssnr_ini 的 Prune 上限同量级):此前固定
    // 16KB 截断会把"同名键取最后命中"偷换成"缓冲区内最后命中",且截断点
    // 切在值中间时解析出半截名字(2026-10-05 评审修)。
    __int64 sz64 = -1;
    if (_fseeki64(f, 0, SEEK_END) == 0) {
        sz64 = _ftelli64(f);
        _fseeki64(f, 0, SEEK_SET);
    }
    if (sz64 <= 0) { fclose(f); return nullptr; }
    if (sz64 > 1024 * 1024) sz64 = 1024 * 1024;
    const size_t n = static_cast<size_t>(sz64);
    std::unique_ptr<char[]> buf(new (std::nothrow) char[n + 1]{});
    if (!buf) { fclose(f); return nullptr; }
    const size_t got = fread(buf.get(), 1, n, f);
    fclose(f);
    buf[got] = '\0';
    // 逐行找未注释的 input-ipc-server = <名>(值可带引号,行尾 # 截断)。
    // 同名键取**最后**一个命中(2026-10-04 评审修):mpv 语义是同名 option
    // 后值覆盖前值,此前取首个 —— 重复键(追加修改残留)时两侧同源地连到
    // 已废弃的旧管道名,reseek/打标静默退化。
    wchar_t last[64]{};
    bool have = false;
    size_t pos = 0;
    while (pos < got) {
        const size_t eol = pos + strcspn(buf.get() + pos, "\r\n");
        size_t s = pos;
        while (s < eol && (buf[s] == ' ' || buf[s] == '\t')) ++s;
        if (s + 16 <= eol && _strnicmp(buf.get() + s, "input-ipc-server", 16) == 0) {
            size_t eq = s + 16;
            while (eq < eol && (buf[eq] == ' ' || buf[eq] == '\t')) ++eq;
            if (eq < eol && buf[eq] == '=') {
                ++eq;
                size_t e = eol;
                for (size_t h = eq; h < e; ++h) {
                    if (buf[h] == '#') { e = h; break; }
                }
                // 两端剥空白,再剥一对配对引号(2026-10-05 评审修:此前行首
                // 从不剥引号,"foo" 解析出 "foo,连接恒败且无迹可查)。mpv
                // conf 语义同:整值被同种引号包住才剥。
                while (eq < e && (buf[eq] == ' ' || buf[eq] == '\t')) ++eq;
                while (e > eq && (buf[e - 1] == ' ' || buf[e - 1] == '\t')) --e;
                if (e > eq + 1 &&
                    ((buf[eq] == '"' && buf[e - 1] == '"') ||
                     (buf[eq] == '\'' && buf[e - 1] == '\''))) {
                    ++eq;
                    --e;
                }
                if (e > eq && e - eq < 64) {
                    wchar_t parsed[64]{};
                    const int cw = MultiByteToWideChar(
                        CP_UTF8, 0, buf.get() + eq, static_cast<int>(e - eq),
                        parsed, 63);
                    if (cw > 0) {
                        parsed[cw] = L'\0';
                        wcscpy_s(last, parsed); // 覆盖语义:后值胜
                        have = true;
                    }
                }
            }
        }
        pos = eol + 1;
    }
    return have ? _wcsdup(last) : nullptr;
}

void MpvPipeDefaultNames(const wchar_t *candidates[kMpvPipeMaxCandidates]) noexcept {
    candidates[0] = nullptr;
    candidates[1] = L"mpvpipe";
    candidates[2] = L"mpvsocket";
    candidates[3] = L"umpv";
}

HANDLE MpvPipeOpen(const wchar_t *const *candidates, int candidateCount,
                   bool overlapped, wchar_t *nameOut, size_t nameLen) noexcept {
    for (int i = 0; i < candidateCount; ++i) {
        if (!candidates[i] || !candidates[i][0]) continue;
        wchar_t pipePath[MAX_PATH];
        // 全路径形态(input-ipc-server 官方文档示例即 \\.\pipe\mpvsocket)
        // 原样使用,不重拼前缀 —— 双重前缀 CreateFileW 必败(2026-10-05 评审修)。
        if (_wcsnicmp(candidates[i], L"\\\\.\\pipe\\", 9) == 0) {
            swprintf_s(pipePath, L"%s", candidates[i]);
        } else {
            swprintf_s(pipePath, L"\\\\.\\pipe\\%s", candidates[i]);
        }
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

bool MpvPipeSendBounded(HANDLE pipe, const char *cmd, size_t len) noexcept {
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return false;
    OVERLAPPED ov{};
    ov.hEvent = ev;
    const DWORD cmdLen = static_cast<DWORD>(len);
    DWORD written = 0;
    // 写入有界等待:ERROR_IO_PENDING 后等事件,超时视为失败。
    bool ok = WriteFile(pipe, cmd, cmdLen, &written, &ov) ||
              (GetLastError() == ERROR_IO_PENDING &&
               WaitForSingleObject(ev, 500) == WAIT_OBJECT_0 &&
               GetOverlappedResult(pipe, &ov, &written, FALSE));
    ok = ok && written == cmdLen;
    if (ok) {
        // 回执只排空,成败不影响 ok(两侧同语义);超时按无回执放行。
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
    return ok;
}

} // namespace vsdlssnr
