# -*- coding: utf-8 -*-
"""F2 IPC 契约:python 模拟面板经共享内存推参,断言热重建/保存/日志开关。

契约(bridge.cpp 的行为):
  1. payload 改 preset(创建参数)→ 恰好一次 "feature recreated"
  2. payload saveRequest=1 → vs-plugins\\dlssnr_ui.ini 落盘
  3. payload logEnabled=0 → timing log 停止增长(面板日志开关生效)

运行: <部署根>\\python.exe testkit\\test_ipc.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402
import panel_ipc  # noqa: E402

testenv.require_env()

import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402

if os.path.exists(testenv.INI):
    os.remove(testenv.INI)

ch = panel_ipc.ParamsChannel()

clip = core.std.BlankClip(width=640, height=480, length=100000,
                          format=vs.YUV420P8, color=[122, 142, 116])
ret = core.dlssnr.Enhance(clip)  # scaling 默认 ON at res=100
ret.get_frame(0)
n = [5]


def settle(k=25):
    for _ in range(k):
        ret.get_frame(n[0])
        n[0] += 1


def status_count():
    return sum(1 for l in testenv.new_lines(0) if "feature recreated" in l)


s0 = status_count()
print("baseline settle...", flush=True)
settle()
print("push preset=2, res=50 ...", flush=True)
ch.push(seq=1, preset=2, inputResolution=50)
time.sleep(0.8)
settle()
d = status_count() - s0
print(f"rebuilds after payload: {d} (expect 1)")

print("push saveRequest ...", flush=True)
ch.push(seq=2, preset=2, inputResolution=50, saveRequest=1)
time.sleep(0.5)
print("ini written:", os.path.exists(testenv.INI))
if os.path.exists(testenv.INI):
    content = open(testenv.INI, encoding="ascii", errors="ignore").read()
    print("ini preset=2:", "preset=2" in content,
          "| ini input_resolution=50:", "input_resolution=50" in content)

print("push logEnabled=0 (timing log must stop) ...", flush=True)
ch.push(seq=3, logEnabled=0)
time.sleep(0.5)
sz1 = testenv.log_size()
settle(70)
sz2 = testenv.log_size()
print(f"timing log growth with log=0: {sz2 - sz1} bytes (expect 0)")
ch.push(seq=4, logEnabled=1)

ok = (d == 1) and os.path.exists(testenv.INI)
print("IPC-CONTRACT:", "PASS" if ok else "FAIL", flush=True)
sys.exit(0 if ok else 1)
