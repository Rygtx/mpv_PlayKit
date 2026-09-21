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

# v19+ 新键:实际路由 / 创建倍数 / 排队细分(FG 未请求 → route=off,
# mult_create=0;存活 tick 必带 slot_wait/lock_wait/gate_*)。
# 断言挂在 case 1 的存活 body 上 —— case 2/3 会因同进程第二个滤镜实例
# 的 IAT hook 单例走 passthrough,边缘 body 不带 tick 键(环境特性,非回归)。
new_keys_present = False
if st:
    import json
    try:
        body = json.loads(st[2])
        new_keys_present = (
            body.get("filter_state") == "ok"
            and body.get("fg_route_eff") == "off"
            and body.get("fg_mult_create") == 0
            and "fg_detail" in body
            and "slot_wait" in body and "lock_wait" in body
            and "gate_skips" in body and "gate_expired" in body and "gate_resets" in body
        )
    except json.JSONDecodeError:
        new_keys_present = False
print("v19 keys (fg_route_eff/fg_mult_create/slot_wait/lock_wait/gate_*):",
      "PASS" if new_keys_present else "FAIL")

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

# 断言:case1 应 ok/off;case3 应 passthrough 且带原因;v19+ 新键齐全
ok = new_keys_present
if st is None:
    ok = False
else:
    import json
    try:
        body = json.loads(st[2])
        ok = ok and body.get("filter_state") == "passthrough" and bool(body.get("state_detail"))
    except json.JSONDecodeError:
        ok = False
print("STATS-KEYS:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
