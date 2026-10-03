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

} // namespace vsdlssnr
