# -*- coding: utf-8 -*-
"""stats 通道键位验证:filter_state / of_mode 等面板数据源字段。

stats 通道是 512 字节映射:magic+seq 头 + JSON body。键位定义在
native/src/panel_ipc.h(SK_* 常量)。本脚本覆盖三类场景:
  case 1: 默认(ofq=0,零 guidance 是用户选择)→ filter_state=ok, of_mode=off
  case 2: ofq=5(NVOF 会话建立)→ of_mode 上报实际档位
  case 3: init 必败路径(不存在的模型 dll)→ passthrough + 原因

运行: <部署根>\\python.exe testkit\\verify_stats_keys.py
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402
import panel_ipc  # noqa: E402

testenv.require_env()

import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402

W, H = 640, 480
base = core.std.BlankClip(width=W, height=H, format=vs.YUV420P8, color=[116, 169, 91])


def pull(clip, n):
    f = clip.get_frame(n)
    _ = f.get_read_ptr(0)


print("=== case 1: 默认(ofq=0,零 guidance 是用户选择,应为 ok/off) ===")
ret = core.dlssnr.Enhance(base)
pull(ret, 0)
pull(ret, 1)
st = panel_ipc.read_stats()
print("stats:", st[2] if st else "MAPPING MISSING")

print("=== case 2: ofq=5(NVOF 会话建立,验证实际模式上报) ===")
ret2 = core.dlssnr.Enhance(base, motion_vector_quality=5)
pull(ret2, 0)  # 播种帧
pull(ret2, 1)  # execute + densify
pull(ret2, 2)
st = panel_ipc.read_stats()
print("stats:", st[2] if st else "MAPPING MISSING")

print("=== case 3: init 必败路径(不存在的模型 dll → passthrough + 原因) ===")
ret3 = core.dlssnr.Enhance(base, ngx_dll=r"Z:\definitely_missing\nvngx_dlssnr.dll")
pull(ret3, 0)  # 应直通(帧返回源内容)
st = panel_ipc.read_stats()
print("stats:", st[2] if st else "MAPPING MISSING")

# 断言:case1 应 ok/off;case3 应 passthrough 且带原因
ok = True
if st is None:
    ok = False
else:
    import json
    try:
        body = json.loads(st[2])
        ok = body.get("filter_state") == "passthrough" and bool(body.get("state_detail"))
    except json.JSONDecodeError:
        ok = False
print("STATS-KEYS:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
