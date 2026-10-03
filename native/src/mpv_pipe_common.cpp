// mpv.conf input-ipc-server 解析(实现;契约见 mpv_pipe_common.h)。
#include "mpv_pipe_common.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vsdlssnr {

wchar_t *MpvParseIpcServerName(const wchar_t *confPath) noexcept {
    if (!confPath) return nullptr;
    FILE *f = nullptr;
    if (_wfopen_s(&f, confPath, L"rb") != 0 || !f) return nullptr;
    char buf[16384]{};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    // 逐行找未注释的 input-ipc-server = <名>(值可带引号,行尾 # 截断)。
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
                        return _wcsdup(parsed);
                    }
                }
                break; // 键命中但值非法:不再扫后续行
            }
        }
        pos = eol + 1;
    }
    return nullptr;
}

void MpvPipeDefaultNames(const wchar_t *candidates[kMpvPipeMaxCandidates]) noexcept {
    candidates[0] = nullptr;
    candidates[1] = L"mpvpipe";
    candidates[2] = L"mpvsocket";
    candidates[3] = L"umpv";
}

HANDLE MpvPipeOpen(const wchar_t *const *candidates, bool overlapped,
                   wchar_t *nameOut, size_t nameLen) noexcept {
    for (int i = 0; i < kMpvPipeMaxCandidates; ++i) {
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
