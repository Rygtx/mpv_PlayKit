# -*- coding: utf-8 -*-
"""生命周期:滤镜加载自动拉起面板;杀 mpv 后面板自行退出(watchdog)。

面板退出机制:命名事件 vs_dlssnr_bridge_alive 是滤镜生命周期标记,
面板 watchdog 检测到事件消失后自动退出(托盘图标消失)。

运行: <部署根>\\python.exe testkit\\test_lifecycle.py
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402

testenv.require_env()
testenv.require_nvidia()

VPY = "~~/vs/DLSSNR_NV.vpy"


def panel_pid():
    return testenv.panel_running()


mpv = subprocess.Popen(
    [testenv.MPV_COM, "av://lavfi:testsrc2", "--frames=600",
     f"--vf=vapoursynth={VPY}", "--vo=null", "--no-terminal", "--really-quiet"],
    cwd=testenv.ROOT)
time.sleep(8)
panel_up = panel_pid() == 1
print("after filter load, panel running:", panel_up)

# mpv.com 是启动壳:必须按名杀真正的 mpv.exe,否则 alive 事件仍在,
# 面板正确地继续运行(这不是 bug)。
testenv.kill_mpv()
deadline = time.time() + 8
gone = False
while time.time() < deadline:
    time.sleep(1)
    if panel_pid() == 0:
        gone = True
        break
print("after mpv kill, panel exited:", gone)
# 两段契约都要验:面板被拉起(panel_up)+ 杀 mpv 后 watchdog 退出(gone)。
# 此前最终判定只看 gone —— 面板从未启动(dlssnr_panel.exe 缺失/拉起失败)
# 时 panel_pid() 恒 0,循环第一拍即 gone=True 照样 PASS,空心通过
# (2026-10-05 评审修)。
ok = gone and panel_up
print("LIFECYCLE", "OK" if ok else "FAIL")
sys.exit(0 if ok else 1)
