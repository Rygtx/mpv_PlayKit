# -*- coding: utf-8 -*-
"""smoke:插件加载 -> NGX 初始化 -> 单帧往返,读回平面字节验证。

最低门槛的数值测试,不需要 mpv、面板与共享内存,只要求部署根有插件与模型。
验证点:
  - core.dlssnr 插件自动加载(mpv-lazy 的 vs-plugins 机制)
  - Enhance(YUV420P8 -> 同格式)首帧成功返回
  - 输出平面可读、plugin 日志经 add_log_handler 可见

运行: <部署根>\\python.exe testkit\\test_smoke.py
"""
import ctypes
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402

testenv.require_env()

import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402

seen = []
core.add_log_handler(lambda lvl, msg: seen.append((lvl, msg)))

print("VapourSynth:", str(core)[:80])
print("dlssnr plugin:", core.dlssnr)

W, H = 640, 480
clip = core.std.BlankClip(width=W, height=H, format=vs.YUV420P8, color=[116, 169, 91])
# 两值条纹让量化差异可见(luma 只有 40/200 两个值)
clip = core.std.Expr(clip, ["x 128 < 40 200 ?", "", ""])

ret = core.dlssnr.Enhance(clip)
print("out format:", ret.format.name, ret.width, ret.height)

f = ret.get_frame(0)
for p in range(3):
    stride = f.get_stride(p)
    row = ctypes.string_at(f.get_read_ptr(p), stride)
    if ret.format.bytes_per_sample == 4:
        vals = [round(ctypes.cast(ctypes.c_char_p(row[i:i + 4]),
                 ctypes.POINTER(ctypes.c_float)).contents.value, 4) for i in range(0, 24, 4)]
    else:
        vals = list(row[:16])
    print("plane", p, "first values:", vals)

print("--- VS logs ---")
for lvl, msg in seen:
    print(lvl, msg.strip())
print("SMOKE OK")
sys.exit(0)
