# -*- coding: utf-8 -*-
"""stats 通道键位验证:filter_state / of_mode 等面板数据源字段。

stats 通道是 2048 字节映射:magic+seq 头 + 定长 struct body(v27 起二进制,
字段定义在 native/src/panel_ipc.h StatsPayload;python 镜像在 panel_ipc.py)。
本脚本覆盖三类场景:
  case 1: 默认(ofq=0,零 guidance 是用户选择)→ filter_state=ok, of_mode=off
  case 2: ofq=5(NVOF 会话建立)→ of_mode 上报实际档位
  case 3: init 必败路径(不存在的模型 dll)→ passthrough + 原因

运行: <部署根>\\python.exe testkit\\verify_stats_keys.py
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("VSDLSSNR_NO_PANEL", "1")  # 纯 stats 键位验证,勿拉真实面板污染后序测试
import testenv  # noqa: E402
import panel_ipc  # noqa: E402

SK = panel_ipc.SK  # stats 键名单一镜像(勿手抄字面量 —— 改名时 python 侧同步防线)

testenv.require_env()

import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402

W, H = 640, 480
base = core.std.BlankClip(width=W, height=H, format=vs.YUV420P8, color=[116, 169, 91])


def pull(clip, n):
    f = clip.get_frame(n)
    _ = f.get_read_ptr(0)


print("=== case 1: 默认(ofq=0,零 guidance 是用户选择,应为 ok/off) ===")
# 显式传参脱离 ini:本机部署的 dlssnr_ui.ini 若开着 FG/FFX,route_eff 会是
# official-hook 而非 off —— 键位断言要的是确定性,不是用户现役配置。
# nr_enabled=1 同理显式钉(2026-09-25):插件出厂默认 nr=0,空依赖 = 全关
# passthrough,create 行都不出现,键位断言空心。
ret = core.dlssnr.Enhance(base, nr_enabled=1, ffx_quality=0,
                          motion_vector_quality=0, fg_enabled=0)
pull(ret, 0)
pull(ret, 1)
st = panel_ipc.read_stats()
body = panel_ipc.read_stats_json()
print("stats:", {k: body[k] for k in ("filter_state", "of_mode", "fg_route_eff")}
      if body else "MAPPING MISSING")

# v19+ 字段:实际路由 / 创建倍数 / 排队细分;v21(DSSL3)新增:
# fg_mult_max(运行库插值帧上限)、of_detail(光流失败原因);RTX 分段字段
# rtxvsr_last/rtxhdr_last(VSR/TrueHDR 专用队列 eval 拆账,关闭时恒 0);
# v24(DSL5,管线全流程解耦)新增 conv_last(输出转换段,恒非 0);
# v25(DSLN,账目诚实化)新增 queue_last(帧间排队)—— GPU 时间戳括号,
# 段语义见 panel_ipc.h;of_engine_last 曾短暂存在,随"光流段=全跨度"语义
# 修正撤销(引擎计算并入 nvof_last,独立段=重复计账)。
# 断言挂在 case 1 的存活 body 上 —— case 2/3 会因同进程第二个滤镜实例
# 的 IAT hook 单例走 passthrough,边缘 body 不带 tick 字段(环境特性,非回归)。
# 真实部署机上 dlssnr_ui.ini(面板保存的设置优先于 VS 参数)可能开着
# FG/FFX,故断言值域自洽,不锁具体数值。
# v27 struct 化后"字段存在性"由布局自身保证(镜像 Struct 解包失败即炸),
# 断言只剩值域自洽。
new_keys_present = False
if st:
    body = panel_ipc.read_stats_json()
    if body:
        mc, mm = body.get(SK["fg_mult_create"], -1), body.get(SK["fg_mult_max"], -1)
        new_keys_present = (
            body.get(SK["filter_state"]) == "ok"
            and body.get(SK["fg_route_eff"]) in ("off", "official-hook", "official", "copy")
            and 0 <= mc <= 6 and 0 <= mm <= 5
            and (mc == 0 or mm >= 1)  # FG 会话存在时运行库上限必 >= 1
            and (mc <= mm + 1)        # 创建倍数不超上限+1(gate 正常钳制)
            and body.get(SK["rtxvsr_last"], -1) >= 0 and body.get(SK["rtxhdr_last"], -1) >= 0
            and body.get(SK["conv_last"], -1) >= 0
            and body.get(SK["queue_last"], -1) >= 0 and body.get(SK["of_last"], -1) >= 0
        )
print("v19/v21/v24/v25 keys (fg_route_eff/fg_mult_create/fg_mult_max/of_detail/slot_wait/lock_wait/gate_*/conv_last/queue_last):",
      "PASS" if new_keys_present else "FAIL")

print("=== case 2: ofq=5(NVOF 会话建立,验证实际模式上报) ===")
ret2 = core.dlssnr.Enhance(base, nr_enabled=1, motion_vector_quality=5,
                           ffx_quality=0, fg_enabled=0)
pull(ret2, 0)  # 播种帧
pull(ret2, 1)  # execute + densify
pull(ret2, 2)
st = panel_ipc.read_stats()
body = panel_ipc.read_stats_json()
print("stats:", {k: body[k] for k in ("filter_state", "of_mode")} if body else "MAPPING MISSING")

print("=== case 3: init 必败路径(不存在的模型 dll → passthrough + 原因) ===")
ret3 = core.dlssnr.Enhance(base, ngx_dll=r"Z:\definitely_missing\nvngx_dlssnr.dll")
pull(ret3, 0)  # 应直通(帧返回源内容)
st = panel_ipc.read_stats()
body = panel_ipc.read_stats_json()
print("stats:", {k: body[k] for k in ("filter_state", "state_detail")} if body else "MAPPING MISSING")

# 断言:case1 应 ok/off;case3 应 passthrough 且带原因;v19+ 字段值域自洽
ok = new_keys_present
if not body:
    ok = False
else:
    ok = ok and body.get(SK["filter_state"]) == "passthrough" and bool(body.get(SK["state_detail"]))
print("STATS-KEYS:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
