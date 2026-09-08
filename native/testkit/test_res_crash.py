# -*- coding: utf-8 -*-
"""res 崩溃扫描:逐档下发内部分辨率 payload,断言 mpv 每档都存活。

历史背景:某档内部分辨率曾触发 GPU 崩溃;本测试遍历 100/75/50/33/25/100
并验证 mpv 存活 + 切换真实发生(空心测试防线:payload 布局错位时存活断言
会空转,所以必须数 timing log 里的 recreate 行)。

播放源用 --loop-file=inf + 脚本端控时:测试时长与处理 fps 完全解耦
(4K res=100 只有 ~5fps,固定 --frames 会把时长耦合到处理速度上)。

用法:
  <部署根>\\python.exe testkit\\test_res_crash.py <视频文件>
  视频文件缺省用 av://lavfi:testsrc2 合成源(无真实文件时)。
"""
import ctypes
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402
import panel_ipc  # noqa: E402

testenv.require_env()
testenv.require_nvidia()

source = sys.argv[1] if len(sys.argv) > 1 else "av://lavfi:testsrc2"
VPY = "~~/vs/DLSSNR_NV.vpy"

mpv = subprocess.Popen(
    [testenv.MPV_COM, source, "--loop-file=inf",
     f"--vf=vapoursynth={VPY}", "--vo=null", "--no-terminal", "--really-quiet"],
    cwd=testenv.ROOT)
time.sleep(10)  # init + first frames
print("mpv alive after init:", mpv.poll() is None)

# create/open the mapping (panel role)
ch = panel_ipc.ParamsChannel()

seq = 1
off = testenv.log_size(testenv.MPV_TIMING_LOG)
alive = mpv.poll() is None
for res in (100, 75, 50, 33, 25, 100):
    seq += 1
    ch.push(seq, inputResolution=res)
    time.sleep(4)
    alive = mpv.poll() is None
    print(f"res {res}%: mpv alive={alive}")
    if not alive:
        print(f"CRASH at res={res}")
        break

# 树杀:mpv.com 是控制台壳,kill() 只杀壳会留 mpv.exe 孤儿锁 dll。
testenv.kill_mpv_tree(mpv.pid)
testenv.kill_mpv()
time.sleep(1)

# 空心测试防线:切换必须真实发生 —— create 在 res=100,窗口内 75/50/33/25/100
# 共 5 次 recreate(mpv 已杀,日志可读)。
recreates = 0
try:
    with open(testenv.MPV_TIMING_LOG, "r", encoding="utf-8", errors="ignore") as f:
        f.seek(off)
        recreates = sum(1 for l in f if "feature recreated" in l)
except FileNotFoundError:
    pass
print(f"窗口内 recreate 次数: {recreates} (expect >= 4)")
print("RES-CRASH-SURVIVAL:", "PASS" if (alive and recreates >= 4) else "FAIL")
sys.exit(0 if (alive and recreates >= 4) else 1)
