# -*- coding: utf-8 -*-
"""YUV 原生化数值验收:ConvertIn / ConvertOut 两段转换 vs python 参考。

依赖插件 dump(VSDLSSNR_DUMP=1 时首帧落盘,文件写在宿主 exe 旁):
  dump_yuvin_y/u/v.bin  R8/R16_UNORM 平面(YUV 输入)
  dump_input.bin        BGRA8(YUV->RGB 转换结果)
  dump_output.bin       BGRA8(NGX 输出)
  dump_y/u/v_plane.bin  R8/R16_UNORM 平面(RGB->YUV 转换结果)

python 参考按 709-limited 重算,容差 <=2 LSB(float32 GPU vs numpy;
UNORM 往返舍入含在内)。

用法:
  <部署根>\\python.exe testkit\\check_yuv_convert.py [dump目录] [W] [H]
  dump 目录缺省 = 部署根(插件默认落盘位置)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402

import numpy as np  # noqa: E402

d = sys.argv[1] if len(sys.argv) > 1 else testenv.HOST_DIR
W, H = (int(x) for x in (sys.argv[2], sys.argv[3])) if len(sys.argv) > 3 else (1920, 1080)
CW, CH = (W + 1) // 2, (H + 1) // 2


def read_plane(path, w, h, bpp=1):
    raw = open(path, "rb").read()
    stride = w * bpp  # DumpTextureToFile 逐行紧凑 fwrite,文件内无 pitch 间隙
    arr = np.frombuffer(raw[: stride * h], dtype=np.uint8).reshape(h, stride)
    return arr[:, :w].astype(np.float32)


def read_bgra(path, w, h):
    raw = open(path, "rb").read()
    stride = w * 4
    arr = np.frombuffer(raw[: stride * h], dtype=np.uint8).reshape(h, w, 4)
    return arr.astype(np.float32)


KR, KB = np.float32(0.2126), np.float32(0.0722)
KG = np.float32(1.0) - KR - KB
Y_LO, Y_SPAN = np.float32(16.0), np.float32(219.0)
C_MID, C_SPAN = np.float32(128.0), np.float32(224.0)

yuv_y = read_plane(os.path.join(d, "dump_yuvin_y.bin"), W, H)
yuv_u = read_plane(os.path.join(d, "dump_yuvin_u.bin"), CW, CH)
yuv_v = read_plane(os.path.join(d, "dump_yuvin_v.bin"), CW, CH)
bgra_in = read_bgra(os.path.join(d, "dump_input.bin"), W, H)
rgb_out = read_bgra(os.path.join(d, "dump_output.bin"), W, H)
out_y = read_plane(os.path.join(d, "dump_y_plane.bin"), W, H)
out_u = read_plane(os.path.join(d, "dump_u_plane.bin"), CW, CH)
out_v = read_plane(os.path.join(d, "dump_v_plane.bin"), CW, CH)

# ---- 1) ConvertIn: YUV(709 limited) -> RGB -> BGRA8 ----
nY = (yuv_y - Y_LO) / Y_SPAN
# 色度双线性上采(与 shader 逐式一致:fc=(px+0.5)*0.5-0.5,4 邻域 clamp)
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
fc_y = (yy + np.float32(0.5)) * np.float32(0.5) - np.float32(0.5)
fc_x = (xx + np.float32(0.5)) * np.float32(0.5) - np.float32(0.5)
c0y = np.floor(fc_y).astype(np.int32)
c0x = np.floor(fc_x).astype(np.int32)
wy = fc_y - c0y.astype(np.float32)
wx = fc_x - c0x.astype(np.float32)
s0y = np.clip(c0y, 0, CH - 1)
s0x = np.clip(c0x, 0, CW - 1)
s1y = np.clip(c0y + 1, 0, CH - 1)
s1x = np.clip(c0x + 1, 0, CW - 1)
w00 = (1 - wx) * (1 - wy)
w10 = wx * (1 - wy)
w01 = (1 - wx) * wy
w11 = wx * wy


def bilinear(plane):
    return (plane[s0y, s0x] * w00 + plane[s0y, s1x] * w10
            + plane[s1y, s0x] * w01 + plane[s1y, s1x] * w11)


nU = (bilinear(yuv_u) - C_MID) / C_SPAN
nV = (bilinear(yuv_v) - C_MID) / C_SPAN
r = nY + nV * 2 * (1 - KR)
b = nY + nU * 2 * (1 - KB)
g = (nY - KR * r - KB * b) / KG
ref_b = np.clip(b, 0, 1)
ref_g = np.clip(g, 0, 1)
ref_r = np.clip(r, 0, 1)
gpu_b = bgra_in[:, :, 0] / 255.0
gpu_g = bgra_in[:, :, 1] / 255.0
gpu_r = bgra_in[:, :, 2] / 255.0
diffs = []
for name, ref, gpu in (("B", ref_b, gpu_b), ("G", ref_g, gpu_g), ("R", ref_r, gpu_r)):
    dd = np.abs(np.round(ref * 255) - np.round(gpu * 255))
    diffs.append((name, int(dd.max()), int((dd > 2).sum())))
alpha_ok = bool(np.all(bgra_in[:, :, 3] == 255))
print("== ConvertIn(YUV->RGB,709 limited 8bit)==")
for name, mx, bad in diffs:
    print(f"  {name}: max diff={mx} LSB, >2 LSB 点数={bad}")
print(f"  alpha 全 255: {alpha_ok}")
in_ok = all(mx <= 2 and bad == 0 for _, mx, bad in diffs) and alpha_ok

# ---- 2) ConvertOut: BGRA8 -> Y/U/V(709 limited;luma 直算,chroma 2x2 box)----
rgbN = rgb_out[:, :, :3] / 255.0  # .0=B .1=G .2=R(内存序)
rN, gN, bN = rgbN[:, :, 2], rgbN[:, :, 1], rgbN[:, :, 0]
nYo = rN * KR + gN * KG + bN * KB
ref_y8 = np.round(np.clip(nYo, 0, 1) * Y_SPAN + Y_LO)
# chroma: 2x2 box 平均后转 cb/cr(shader 先平均后转换,仿射等价);
# cb/cr ∈ [-0.5,0.5] 不做 [0,1] 钳制(负色度合法,偏移在映射里加)
rA = rN.reshape(H // 2, 2, W // 2, 2).mean(axis=(1, 3))
gA = gN.reshape(H // 2, 2, W // 2, 2).mean(axis=(1, 3))
bA = bN.reshape(H // 2, 2, W // 2, 2).mean(axis=(1, 3))
nYa = rA * KR + gA * KG + bA * KB
ref_u8 = np.round((bA - nYa) * (0.5 / (1 - KB)) * C_SPAN + C_MID)
ref_v8 = np.round((rA - nYa) * (0.5 / (1 - KR)) * C_SPAN + C_MID)
d_y = np.abs(ref_y8 - np.round(out_y))
d_u = np.abs(ref_u8 - np.round(out_u))
d_v = np.abs(ref_v8 - np.round(out_v))
print("== ConvertOut(RGB->YUV,709 limited 8bit)==")
print(f"  Y: max diff={int(d_y.max())} LSB, >2 LSB 点数={int((d_y > 2).sum())}")
print(f"  U: max diff={int(d_u.max())} LSB, >2 LSB 点数={int((d_u > 2).sum())}")
print(f"  V: max diff={int(d_v.max())} LSB, >2 LSB 点数={int((d_v > 2).sum())}")
out_ok = int(d_y.max()) <= 2 and int(d_u.max()) <= 2 and int(d_v.max()) <= 2

print("PASS" if (in_ok and out_ok) else "FAIL", ":两段转换均在 <=2 LSB")
sys.exit(0 if (in_ok and out_ok) else 1)
