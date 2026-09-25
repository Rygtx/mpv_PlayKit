# -*- coding: utf-8 -*-
"""test_hdrfix_vf:面板驱动 HDR 打标(PQ 元数据)端到端验证。

链路:mpv 挂裸 vapoursynth(mpv.conf 同名管道 mpvpipe)→ 面板轮询 SK_RTX
→ rtxState 含 hdr → IPC `vf add @dlssnr-hdr-tag`(lavfi setparams 打
BT.2020 PQ 元数据)→ mpv 正确解读 → 截图。
运行前提:环境变量 VSDLSSNR_TEST_MEDIA = 任一真片源路径(mpv 播放用)。
运行: <部署根>\\python.exe testkit\\test_hdrfix_vf.py
"""
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402
testenv.ini_restore_on_exit()  # 部署 ini 现场保护(2026-09-25 收敛)

testenv.require_env()
testenv.kill_panel()
testenv.kill_mpv()
time.sleep(2)

import ctypes  # noqa: E402

k32 = ctypes.windll.kernel32
for k, v in {"vsr_mode": "2", "vsr_scale_x100": "200", "vsr_strength": "2",
             "hdr_enabled": "1"}.items():
    assert k32.WritePrivateProfileStringW("rtxvideo", k, v, testenv.INI), k

MEDIA = os.environ.get("VSDLSSNR_TEST_MEDIA")
testenv.require(MEDIA and os.path.isfile(MEDIA),
                "未设 VSDLSSNR_TEST_MEDIA(任一真片源路径,mpv 播放用)")

shot = os.path.join(testenv.ROOT, "hdrfix_panel.png")
if os.path.exists(shot):
    os.remove(shot)
errf = open(os.path.join(testenv.ROOT, "mpv_stderr.txt"), "w", encoding="utf-8")

mpv = subprocess.Popen([
    testenv.MPV_EXE,
    "--input-ipc-server=mpvpipe",
    "--screenshot-format=png",
    "--screenshot-png-compression=0",
    "--volume=0",
    "--start=300",
    "--geometry=1280x720",
    '--vf=vapoursynth="~~/vs/DLSSNR_NV.vpy"',
    MEDIA,
], cwd=testenv.ROOT, stderr=errf)
panel = subprocess.Popen([os.path.join(testenv.PLUGIN_DIR, "dlssnr_panel.exe")],
                         cwd=testenv.ROOT)


def ipc(cmd, retries=20):
    """管道连接重试(mpv 启动/前次残名释放均有竞态)。"""
    for _ in range(retries):
        if mpv.poll() is not None:
            return "(mpv dead)"
        try:
            with open(r"\\.\pipe\mpvpipe", "r+b", buffering=0) as p:
                p.write((json.dumps({"command": cmd}) + "\n").encode("utf-8"))
                time.sleep(0.4)
                try:
                    return p.read(65536).decode("utf-8", "replace").strip()
                except OSError:
                    return "(no reply)"
        except FileNotFoundError:
            time.sleep(1)
    return "(pipe never appeared)"


time.sleep(12)
op = ipc(["get_property", "video-out-params"])
import re  # noqa: E402
m = re.search(r'"colormatrix":"([a-z0-9.-]+)","colorlevels":"([a-z]+)",'
              r'"primaries":"([a-z0-9.-]+)","gamma":"([a-z0-9.]+)"', op)
if m:
    print("TAGS: matrix=%s primaries=%s gamma=%s" % m.groups())
else:
    print("out-params:", op[:200])

print("shot:", ipc(["screenshot-to-file", shot.replace("\\", "/"), "video"])[:60])
time.sleep(2)
testenv.kill_mpv()
mpv.wait(timeout=10)
testenv.kill_panel()
panel.wait(timeout=10)
errf.close()

if os.path.exists(shot):
    print("SHOT OK:", shot)
else:
    print("SHOT MISSING")
sys.exit(0)
