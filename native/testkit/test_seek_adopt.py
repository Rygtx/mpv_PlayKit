# -*- coding: utf-8 -*-
"""seek-adopt 回归:seek 拆 VS core 后新滤镜实例必须在 create 时 ADOPT 面板
当前 payload(曾经回退到过期 ini/vpy 值,因为 bridge 轮询跳过历史)。

判定:
  实例 1:冷 create,面板 payload res=50(vpy 默认 100)→ adopt 必须赢
  实例 2:丢实例 1 后重建(同进程热交接 = seek 路径)→ 保持 res=50,
          零 "feature recreated"
  后续 payload 编辑(seq 递增)→ 仍恰好一次 recreate 到 res=75

运行: <部署根>\\python.exe testkit\\test_seek_adopt.py
"""
import gc
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
    os.remove(testenv.INI)  # isolate from saved profiles

ch = panel_ipc.ParamsChannel()
ch.push(seq=1, inputResolution=50)

clip = core.std.BlankClip(width=640, height=480, length=100000,
                          format=vs.YUV420P8, color=[122, 142, 116])

# --- instance 1: cold create, panel says res=50, vpy default is 100 ---
off1 = testenv.log_size()
ret = core.dlssnr.Enhance(clip)  # no vpy args: defaults 100, adopt must win
ret.get_frame(0)
ret.get_frame(1)
time.sleep(0.4)
l1 = testenv.new_lines(off1)
print("instance1 log:")
for l in l1:
    print("   ", l)
i1_ok = any("res=50%" in l for l in l1) and not any("res=100%" in l for l in l1)
print("instance1 adopted res=50:", i1_ok)

# --- instance 2: seek simulation (drop instance 1 -> hot handoff, same process) ---
off2 = testenv.log_size()
ret = None
gc.collect()
time.sleep(0.6)
ret = core.dlssnr.Enhance(clip)  # fresh SharedParams: adopt must keep 50
ret.get_frame(2)
ret.get_frame(3)
time.sleep(0.4)
l2 = testenv.new_lines(off2)
print("instance2 log:")
for l in l2:
    print("   ", l)
i2_ok = (any("kept feature" in l and "res=50%" in l for l in l2)
         and not any("recreated" in l for l in l2))
print("instance2 hot-kept res=50 (zero rebuild):", i2_ok)

# --- live edit still works after adopt ---
off3 = testenv.log_size()
ch.push(seq=2, inputResolution=75)
for i in range(5):
    ret.get_frame(4 + i)
    time.sleep(0.2)
time.sleep(0.4)
l3 = testenv.new_lines(off3)
print("after push res=75:")
for l in l3:
    print("   ", l)
i3_ok = sum(1 for l in l3 if "recreated" in l) == 1 and any("res=75%" in l for l in l3)
print("live edit recreated once at res=75:", i3_ok)

ok = i1_ok and i2_ok and i3_ok
print("SEEK-ADOPT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
