# -*- coding: utf-8 -*-
"""F3 端到端:真实面板 <-> 插件,双向共享内存通道 + 自动拉起。

验证点:
  - 滤镜加载后面板经 LaunchPanelSilently 自动启动(仅托盘)
  - stats 通道(插件 -> 面板)出现合法 JSON
  - 参数通道(面板 -> 插件)magic=0x364C5344、seq>=1、generation!=0

运行: <部署根>\\python.exe testkit\\test_e2e_panel.py
"""
import ctypes
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402
import panel_ipc  # noqa: E402

testenv.require_env()

import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402

clip = core.std.BlankClip(width=640, height=480, length=100000,
                          format=vs.YUV420P8, color=[122, 142, 116])
clip = core.std.Expr(clip, ["x 8 + 10 sin 5 * +", "", ""])
ret = core.dlssnr.Enhance(clip)
ret.get_frame(0)
print("filter loaded; panel should auto-launch", flush=True)

n = [1]
t0 = time.time()
while time.time() - t0 < 12:
    for _ in range(20):
        ret.get_frame(n[0])
        n[0] += 1
    time.sleep(0.2)

# stats channel (plugin -> panel)
st = panel_ipc.read_stats()
if st:
    print("STATS:", st[2][:160], flush=True)
else:
    print("STATS: MAPPING MISSING", flush=True)

# params channel (panel -> plugin)
h, v = panel_ipc.open_params_mapping_readonly()
if v:
    buf = (ctypes.c_uint32 * 128)()
    ctypes.memmove(buf, ctypes.c_void_p(v), 512)
    magic, seq, gen = buf[0], buf[1], buf[2]
    print(f"PARAMS: magic=0x{magic:08X} (expect 0x{panel_ipc.PAYLOAD_MAGIC:08X}) "
          f"seq={seq} gen={gen}", flush=True)
    ok = magic == panel_ipc.PAYLOAD_MAGIC and seq >= 1 and gen != 0
    print("PARAMS-OK:", ok, flush=True)
else:
    print("PARAMS: MAPPING MISSING (panel not running?)", flush=True)
    ok = False

print("E2E-PANEL:", "PASS" if ok else "FAIL", flush=True)
sys.exit(0 if ok else 1)
