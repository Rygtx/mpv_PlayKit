# -*- coding: utf-8 -*-
"""test_hdr_hint_sync:面板 HDR 打标 + target-colorspace-hint 同步端到端验证。

链路:插件 SK_RTX 实态含 hdr → 面板 IPC `vf add @dlssnr-hdr-tag` 且同拍
`set target-colorspace-hint=yes`(上屏 HDR);SDR 实态 → 标签移除 + hint
还原连接时读回的 mpv 实值(mpv.conf 为 no 时即还原 no,不写死)。
两阶段断言(值域,不依赖 ini 回读):
  A. hdr_enabled=1 → vf 链含标签 且 get hint == true
  B. hdr_enabled=0 → vf 链无标签 且 get hint == false(还原 conf 实值)
媒体:VSDLSSNR_TEST_MEDIA 优先;缺失时合成 y4m(640x360@24 移动条纹,
15 秒,%TEMP% 生成;--start=5 + --loop=inf,不依赖真片源与播放不中断)。
运行: <部署根>\\python.exe testkit\\test_hdr_hint_sync.py
"""
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402

testenv.require_env()
testenv.kill_panel()
testenv.kill_mpv()
time.sleep(2)

import ctypes  # noqa: E402

k32 = ctypes.windll.kernel32


def set_ini(**kv):
    for k, v in kv.items():
        assert k32.WritePrivateProfileStringW("rtxvideo", k, v, testenv.INI), k


def gen_y4m(path, w=640, h=360, fps=24, secs=15):
    """移动竖条纹(全亮范围扫)+ 静态中灰色度;纯 bytes 旋转,无 numpy。"""
    pat = bytes(16 + (x % 64) * 219 // 63 for x in range(64))
    row0 = (pat * (w // 64 + 2))[:w]
    csize = (w // 2) * (h // 2)
    chroma = b"\x80" * csize
    with open(path, "wb") as f:
        f.write(f"YUV4MPEG2 W{w} H{h} F{fps}:1 Ip A1:1 C420jpeg\n".encode())
        for n in range(fps * secs):
            off = (n * 3) % w
            row = row0[w - off:] + row0[:w - off]
            f.write(b"FRAME\n" + row * h + chroma + chroma)


MEDIA = os.environ.get("VSDLSSNR_TEST_MEDIA")
if MEDIA and os.path.isfile(MEDIA):
    print("MEDIA:", MEDIA)
else:
    MEDIA = os.path.join(os.environ.get("TEMP", "."), "hdr_hint_sync_test.y4m")
    if not os.path.exists(MEDIA):
        print("合成媒体:", MEDIA)
        gen_y4m(MEDIA)
    assert os.path.getsize(MEDIA) > 0


def launch(hdr):
    set_ini(vsr_mode="2", vsr_scale_x100="200", vsr_strength="2",
            hdr_enabled="1" if hdr else "0")
    errf = open(os.path.join(testenv.ROOT, "mpv_stderr.txt"), "a", encoding="utf-8")
    mpv = subprocess.Popen([
        testenv.MPV_EXE,
        "--input-ipc-server=mpvpipe",
        "--really-quiet",
        "--volume=0",
        "--start=5",
        "--loop=inf",
        "--geometry=1280x720",
        '--vf=vapoursynth="~~/vs/DLSSNR_NV.vpy"',
        MEDIA,
    ], cwd=testenv.ROOT, stderr=errf, stdout=subprocess.DEVNULL)
    panel = subprocess.Popen([testenv.PANEL_EXE], cwd=testenv.ROOT)
    return mpv, panel, errf


def ipc(cmd, retries=20):
    """管道连接重试(mpv 启动/前次残名释放均有竞态)。"""
    for _ in range(retries):
        try:
            with open(r"\\.\pipe\mpvpipe", "r+b", buffering=0) as p:
                p.write((json.dumps({"command": cmd}) + "\n").encode("utf-8"))
                time.sleep(0.3)
                try:
                    return p.read(65536).decode("utf-8", "replace")
                except OSError:
                    return "(no reply)"
        except FileNotFoundError:
            if subprocess.run(["tasklist", "/FI", f"PID eq {cur_mpv[0]}"],
                              capture_output=True, text=True).stdout.count("mpv.exe") == 0:
                return "(mpv dead)"
            time.sleep(1)
    return "(pipe never appeared)"


cur_mpv = [0]


def wait_until(desc, pred, timeout=40):
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        last = pred()
        if last:
            print(f"  [ok {int(timeout - (deadline - time.time())):02d}s] {desc}")
            return last
        time.sleep(1)
    print(f"  [TIMEOUT] {desc} last={last[:200]}")
    return None


def vf_has_tag():
    # `vf` 裸命令需要操作子命令,列链走属性:get vf → [{"name":..,"label":..}]
    return "dlssnr-hdr-tag" in ipc(["get_property", "vf"])


def hint_is(expected):
    r = ipc(["get_property", "target-colorspace-hint"])
    return f'"data":{expected}' in r


print("== A: hdr_enabled=1 → 标签 + hint=yes ==")
mpv, panel, errf = launch(hdr=True)
cur_mpv[0] = mpv.pid
time.sleep(10)  # 链重建 + NGX 初始化 + 面板 250ms tick
ok_tag = wait_until("vf 含 @dlssnr-hdr-tag", vf_has_tag)
ok_hint = wait_until("target-colorspace-hint == true", lambda: hint_is("true"))
op = ipc(["get_property", "video-out-params"])
import re  # noqa: E402
m = re.search(r'"colormatrix":"([a-z0-9.-]+)".{0,80}?"gamma":"([a-z0-9.]+)"', op)
if m:
    print("  out-params: matrix=%s gamma=%s" % m.groups())
testenv.kill_mpv()
testenv.kill_panel()
mpv.wait(timeout=10)
panel.wait(timeout=10)
errf.close()

print("== B: hdr_enabled=0 → 摘标 + hint 还原 conf 实值(no)==")
mpv, panel, errf = launch(hdr=False)
cur_mpv[0] = mpv.pid
time.sleep(10)
ok_untag = wait_until("vf 无 @dlssnr-hdr-tag", lambda: not vf_has_tag())
ok_restore = wait_until("target-colorspace-hint == false", lambda: hint_is("false"))
testenv.kill_mpv()
testenv.kill_panel()
mpv.wait(timeout=10)
panel.wait(timeout=10)
errf.close()

ok = all([ok_tag, ok_hint, ok_untag, ok_restore])
print("PASS" if ok else "FAIL",
      {k: bool(v) for k, v in [("tag_added", ok_tag), ("hint_true", ok_hint),
                               ("tag_removed", ok_untag), ("hint_restored", ok_restore)]})
sys.exit(0 if ok else 1)
