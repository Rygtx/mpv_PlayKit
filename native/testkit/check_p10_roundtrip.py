# -*- coding: utf-8 -*-
"""P10 色块往返:SKIP_EVAL 纯转换往返 vs zimg 同款转换(8 色块,P10 码值)。

8 色块条带(每块 Y/U/V 恒定 -> RGB 域可表示,无钳制损失),P10 源 ->
VSDLSSNR_SKIP_EVAL=1 纯转换往返 -> 逐块中心对比。判据:色度块内
mine == zimg 同款转换(<=2 LSB);明确排除"RGB 中间域钳制"的固有损失
场景(U/V 超出 [Y 所允许的色域] 的块不参与断言 —— zimg 对这类块同样不可逆)。

前置:设 VSDLSSNR_SKIP_EVAL=1 运行(纯转换,跳过 NGX eval)。

运行: VSDLSSNR_SKIP_EVAL=1 <部署根>\\python.exe testkit\\check_p10_roundtrip.py
"""
import ctypes
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402

import numpy as np  # noqa: E402
import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402

if os.path.exists(testenv.INI):
    os.remove(testenv.INI)
testenv.kill_panel()
time.sleep(1)

W, H = 1280, 720
NB = 8
BH = H // NB  # 90
CW, CH = W // 2, H // 2
CBH = CH // NB

# 709 limited 10-bit 码值(Y 64-940, C 64-960),全部在 RGB 可表示域内:
# 白/灰/黑三中性 + 品红/青/黄/橙/蓝五个高饱和(色度未超 RGB 色域)。
# yuv709 直接以 10-bit 码值推导(Cb10 = (B8-Y8)/255*(1/0.9278)*224/255*4 ->
# 统一按归一域算再乘 4)。
def yuv709_10(r8, g8, b8):
    r, g, b = r8 / 255, g8 / 255, b8 / 255
    nY = 0.2126 * r + 0.7152 * g + 0.0722 * b
    nCb = (b - nY) * 0.5 / (1 - 0.0722)
    nCr = (r - nY) * 0.5 / (1 - 0.2126)
    y10 = round(nY * 876 + 64)
    cb10 = round(nCb * 896 + 512)
    cr10 = round(nCr * 896 + 512)
    return (max(64, min(940, y10)), max(64, min(960, cb10)), max(64, min(960, cr10)))


colors = [(255, 0, 255), (0, 255, 255), (255, 255, 0), (255, 128, 0),
          (0, 128, 255), (0, 200, 0), (255, 0, 0), (128, 128, 128)]
yuv = [yuv709_10(*c) for c in colors]

strip = None
for (y10, cb10, cr10) in yuv:
    a = core.std.BlankClip(width=W, height=BH, format=vs.YUV420P10, color=[y10, cb10, cr10])
    strip = a if strip is None else core.std.StackVertical([strip, a])
src = strip


def get_plane(f, p):
    stride = f.get_stride(p)
    w = (W if p == 0 else CW) * 2
    h = H if p == 0 else CH
    a = np.frombuffer(ctypes.string_at(
        ctypes.cast(f.get_read_ptr(p), ctypes.c_void_p).value,
        stride * (h - 1) + w), np.uint16).reshape(h, stride // 2)
    return a[:, :(W if p == 0 else CW)]


env = os.environ.get("VSDLSSNR_SKIP_EVAL", "")
print(f"SKIP_EVAL={env!r}")
mine = core.dlssnr.Enhance(src)
zimg = src.resize.Bilinear(format=vs.RGBS, matrix_in_s="709")
zimg = zimg.resize.Bilinear(format=src.format.id, matrix_s="709")

n = 20
keep = []
for _ in range(6):
    keep.append(mine.get_frame(n))
    keep.append(zimg.get_frame(n))
    n += 1
fm = mine.get_frame(n)
fz = zimg.get_frame(n)
fs = src.get_frame(n)
keep += [fm, fz, fs]

# 逐色块中心(取块中部行避免边界),Y/U/V 三平面 mine vs zimg vs src
worst = 0
print("--- 色块中心对比(P10 码值)---")
for i, ((r8, g8, b8), (y10, cb10, cr10)) in enumerate(zip(colors, yuv)):
    yrow = i * BH + BH // 2
    crow = i * CBH + CBH // 2
    vals = []
    for p, (row, col) in enumerate([(yrow, W // 2), (crow, CW // 2), (crow, CW // 2)]):
        m = int(get_plane(fm, p)[row, col])
        z = int(get_plane(fz, p)[row, col])
        s = int(get_plane(fs, p)[row, col])
        vals.append((m, z, s))
    tag = colors[i]
    ok = all(abs(m - z) <= 2 for m, z, _ in vals)
    print(f"{str(tag):15s} Y {vals[0][0]:4d}/{vals[0][1]:4d}  "
          f"U {vals[1][0]:4d}/{vals[1][1]:4d}  V {vals[2][0]:4d}/{vals[2][1]:4d}  "
          f"src Y{vals[0][2]} U{vals[1][2]} V{vals[2][2]}  {'OK' if ok else '<-- 分歧'}")
    for m, z, _ in vals:
        worst = max(worst, abs(m - z))

print(f"max |mine-zimg| = {worst} LSB")
print("P10 CONVERT:", "PASS" if worst <= 2 else "FAIL")
sys.exit(0 if worst <= 2 else 1)
