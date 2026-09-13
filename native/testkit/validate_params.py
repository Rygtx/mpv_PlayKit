# -*- coding: utf-8 -*-
"""参数面数值验收 —— driver + worker 双进程模式。

名字不带版本号:断言的是「参数语义面」(每个参数确实改变输出、钳制生效、
同配置确定),对齐上游新版本后直接复跑,不用改名。

为什么每个配置一个进程:NGX DLSSNR 的时域历史**跨热复用实例延续**
(PARAM_RESET 不完全复位),同进程的"独立滤镜实例"共享 feature 历史,
哈希被历史位置污染(NONDETERMINISTIC 假象)。新进程 = 新 NGX core = 真冷启,
哈希只由配置决定。

断言(driver 本文件):
  det   : 同配置两个 worker 哈希一致(确定性)
  res50 : input_resolution=50 触发 horizontal pass -> DIFF
  mult2 : residual_multiplier=2 -> DIFF
  fine  : saturation=0 lightness=0.5 -> DIFF
  clamp : intensity=1.5 钳到 kStrengthMax=1.0 -> SAME as default
  -- 2026-09-14 交叉验证补的契约面(AIO 72 用例矩阵 + RTX 3080 A/B,
     换 nvngx_dlssnr.dll 版本后复跑,防参数行为静默漂移):
  style : style=1 / style=2 各自 -> DIFF(Style 是真模型选择器;
          AIO 矩阵:preset 1-3 输出不变,Style 0/1/2 三个不同输出)
  am0   : use_auto_mask=0 -> DIFF(自动蒙版键有效性哨兵)
  nr0   : nr_enabled=0 -> DIFF(直通路径健康基线)
  preset: preset=1 / preset=3 -> SAME(哨兵断言,记录当前 DLL 不消费
          Hint.Render.Preset 的事实;哪天变 DIFF = preset 被新 DLL 激活,
          此时应恢复面板"预设"下拉,FAIL 不是 bug)

运行: <部署根>\\python.exe testkit\\validate_params.py
"""
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402

testenv.require_env()
testenv.require_nvidia()

if os.path.exists(testenv.INI):
    os.remove(testenv.INI)
# 面板残留清场:面板 payload 经 BridgeAdoptPanelPayload 覆盖 worker 的
# vpy kwargs(#37 优先级设计),配置差异全部被抹平。
testenv.kill_panel()
time.sleep(1)
# worker 全程无面板
os.environ["VSDLSSNR_NO_PANEL"] = "1"

WORKER = r'''
import ctypes, hashlib, json, sys
import vapoursynth as vs
from vapoursynth import core
kw = json.loads(sys.argv[1])
clip = core.std.BlankClip(width=1280, height=720, length=100000,
                          format=vs.YUV420P10, color=[488,568,464])
clip = core.std.Expr(clip, ["x 32 + 40 sin 20 * +", "", ""])
ret = core.dlssnr.Enhance(clip, **kw)
n = 100
for _ in range(5):
    ret.get_frame(n); n += 1
f = ret.get_frame(n); n += 1
hsh = hashlib.md5()
plane_w = (1280, 640, 640)
plane_h = (720, 360, 360)
for p in range(3):
    stride = f.get_stride(p)
    addr = ctypes.cast(f.get_read_ptr(p), ctypes.c_void_p).value
    row_bytes = plane_w[p] * f.format.bytes_per_sample
    for row in range(0, plane_h[p], 8):
        hsh.update(ctypes.string_at(addr + stride * row, row_bytes))
print(hsh.hexdigest()[:10])
'''


def capture(kw):
    r = subprocess.run([sys.executable, "-c", WORKER, json.dumps(kw)],
                       capture_output=True, text=True)
    out = r.stdout.strip().splitlines()
    return out[-1] if out else f"(worker failed: {r.stderr[-200:]})"


a = capture({})
print(f"default res100:            {a}")
a2 = capture({})
print(f"default res100 (again):    {a2}  {'DETERMINISTIC' if a2 == a else 'NONDETERMINISTIC!'}")
b = capture({"input_resolution": 50})
print(f"res50 (horizontal pass):   {b}  {'DIFF' if b != a else 'SAME!'}")
d = capture({"residual_multiplier": 2.0})
print(f"res100 multiplier=2:       {d}  {'DIFF' if d != a else 'SAME!'}")
e = capture({"residual_saturation": 0.0, "residual_lightness": 0.5})
print(f"saturation=0 light=0.5:    {e}  {'DIFF' if e != a else 'SAME!'}")
g = capture({"intensity": 1.5})
print(f"intensity=1.5 (clamp 1.0): {g}  {'CLAMPED-OK' if g == a else 'NOT CLAMPED!'}")
s = capture({"shadow_structure": 0.0, "reflection_glow": 1.8})
print(f"shadow=0 glow=1.8:         {s}  {'DIFF' if s != a else 'SAME!'}")

# -- 契约面(2026-09-14):Style / AutoMask / NR 总开关 / Preset 哨兵 --
st1 = capture({"style": 1})
print(f"style=1:                   {st1}  {'DIFF' if st1 != a else 'SAME!'}")
st2 = capture({"style": 2})
print(f"style=2:                   {st2}  {'DIFF' if st2 != a else 'SAME!'}")
am = capture({"use_auto_mask": 0})
print(f"use_auto_mask=0:           {am}  {'DIFF' if am != a else 'SAME!'}")
nr0 = capture({"nr_enabled": 0})
print(f"nr_enabled=0 (passthrough):{nr0}  {'DIFF' if nr0 != a else 'SAME!'}")
p1 = capture({"preset": 1})
p3 = capture({"preset": 3})
p_ok = (p1 == a) and (p3 == a)
print(f"preset=1:                  {p1}  {'SAME (预期:DLL 不消费 preset)' if p1 == a else 'DIFF (preset 被激活!)'}")
print(f"preset=3:                  {p3}  {'SAME (预期:DLL 不消费 preset)' if p3 == a else 'DIFF (preset 被激活!)'}")
if not p_ok:
    print("  ^ SENTINEL TRIPPED: 新 DLL 消费 Hint.Render.Preset 了 —— "
          "恢复面板\"预设\"下拉(对照 2026-09-14 的移除提交)")

ok = (a2 == a) and (b != a) and (d != a) and (e != a) and (g == a) and (s != a)
ok = ok and (st1 != a) and (st2 != a) and (am != a) and (nr0 != a) and p_ok
print("VALIDATE-PARAMS:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
