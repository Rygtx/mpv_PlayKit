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
testenv.ini_restore_on_exit()  # 部署 ini 现场保护(2026-09-25 收敛)
import panel_ipc  # noqa: E402

testenv.require_env()
# 真实面板清场(2026-09-25 修连跑 flake):前序测试(未设 NO_PANEL 的
# verify_stats_keys 等)经 BridgeStart 拉起过真实面板时,它会与本项目
# ParamsChannel 互写同一映射的 generation(seq 基线反复重置),recreate
# 计数断言随机失败。本测试的通道语义必须独占。
testenv.kill_panel()
os.environ.setdefault("VSDLSSNR_NO_PANEL", "1")
time.sleep(1)

import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402

if os.path.exists(testenv.INI):
    os.remove(testenv.INI)

ch = panel_ipc.ParamsChannel()

clip = core.std.BlankClip(width=640, height=480, length=100000,
                          format=vs.YUV420P8, color=[122, 142, 116])
# 显式开 NR:出厂默认 nr=0(2026-09-23 改档)会让全关实例走纯直通
# (ProcessFrame/ConsumeRebuild 不再执行)—— payload 的 create-time 参数
# 无人消费,rebuild 契约就测不到了。
ret = core.dlssnr.Enhance(clip, nr_enabled=1)  # scaling 默认 ON at res=100
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
