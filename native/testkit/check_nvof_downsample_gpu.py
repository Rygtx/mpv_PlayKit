# -*- coding: utf-8 -*-
"""GPU 光流输入降采样数值验证(CPU->GPU 改造的验收)。

对比 dump_nvof_input.bin(GPU 降采样输出,NVOF 注册输入纹理,BGRA8 紧凑行)
与 python 参考实现:从 dump_input.bin(本帧 upload 的整帧 BGRA8)按与
NVOF_DOWNSAMPLE_HLSL/CPU PackNvofInput 相同的双线性公式重算。

参考与 GPU 同源(都对 PackInput 已量化的 0-255 值 lerp,核公式逐式一致),
期望基本逐位一致;f32(GPU) vs f64/numpy 累加在 round-half-up 边界可能
差 1 LSB。断言:逐通道(B/G/R)最大差 <=1,alpha 全 255。

尺寸:--src/--dst 对照 timing log 的 "nvof session mode=... WxH" 行
(dst = 会话输入尺寸,follow 模式 = 内部尺寸)与源帧尺寸。

用法:
  <部署根>\\python.exe testkit\\check_nvof_downsample_gpu.py \\
      --src 1920x1080 --dst 960x540 \\
      --input <dump目录>\\dump_input.bin --gpu <dump目录>\\dump_nvof_input.bin
"""
import argparse
import sys

import numpy as np


def read_bgra(path, w, h):
    raw = np.fromfile(path, dtype=np.uint8)
    expect = w * h * 4
    if raw.size != expect:
        sys.exit(f"FAIL: {path} size {raw.size} != {w}x{h}x4 = {expect}")
    return raw.reshape(h, w, 4)


def bilinear_reference(src, sw, sh, dw, dh):
    """与 HLSL/CPU 逐式一致的双线性:fy=(dy+0.5)*sy-0.5、(int) 截断、
    clamp、+1 邻域取 min、0-255 域 lerp、round-half-up。"""
    sx = np.float32(sw) / np.float32(dw)
    sy = np.float32(sh) / np.float32(dh)

    fx = (np.arange(dw, dtype=np.float32) + np.float32(0.5)) * sx - np.float32(0.5)
    x0 = fx.astype(np.int32)  # trunc toward zero, 与 C/CPU 一致
    wx = fx - x0.astype(np.float32)
    x0 = np.clip(x0, 0, sw - 1)
    x1 = np.minimum(x0 + 1, sw - 1)

    fy = (np.arange(dh, dtype=np.float32) + np.float32(0.5)) * sy - np.float32(0.5)
    y0 = fy.astype(np.int32)
    wy = fy - y0.astype(np.float32)
    y0 = np.clip(y0, 0, sh - 1)
    y1 = np.minimum(y0 + 1, sh - 1)

    w00 = (1.0 - wx)[None, :] * (1.0 - wy)[:, None]
    w10 = wx[None, :] * (1.0 - wy)[:, None]
    w01 = (1.0 - wx)[None, :] * wy[:, None]
    w11 = wx[None, :] * wy[:, None]

    ix0 = x0[None, :]
    ix1 = x1[None, :]
    iy0 = y0[:, None]
    iy1 = y1[:, None]

    out = np.empty((dh, dw, 3), dtype=np.float32)
    for c in range(3):  # BGRA: c=0->B, 1->G, 2->R
        ch = src[:, :, c].astype(np.float32)
        p00 = ch[iy0, ix0]
        p10 = ch[iy0, ix1]
        p01 = ch[iy1, ix0]
        p11 = ch[iy1, ix1]
        out[:, :, c] = p00 * w00 + p10 * w10 + p01 * w01 + p11 * w11
    return np.floor(out + np.float32(0.5))  # round-half-up, 0-255


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="WxH,源帧尺寸")
    ap.add_argument("--dst", required=True, help="WxH,NVOF 会话输入尺寸")
    ap.add_argument("--input", required=True, help="dump_input.bin 路径")
    ap.add_argument("--gpu", required=True, help="dump_nvof_input.bin 路径")
    a = ap.parse_args()

    sw, sh = (int(v) for v in a.src.lower().split("x"))
    dw, dh = (int(v) for v in a.dst.lower().split("x"))
    if not (dw <= sw and dh <= sh):
        sys.exit("FAIL: 会话尺寸必须 <= 源尺寸(follow 只缩窄)")

    src = read_bgra(a.input, sw, sh)
    gpu = read_bgra(a.gpu, dw, dh)

    ref = bilinear_reference(src, sw, sh, dw, dh)
    gpu_rgb = gpu[:, :, :3].astype(np.float32)

    diff = np.abs(ref - gpu_rgb)
    max_diff = int(diff.max())
    nonzero = int((diff > 0).sum())
    over1 = int((diff > 1).sum())
    alpha_bad = int((gpu[:, :, 3] != 255).sum())
    total = dw * dh * 3

    print(f"对比 {sw}x{sh} -> {dw}x{dh}")
    print(f"  采样点        : {total} (3 通道 x {dw * dh})")
    print(f"  逐位一致      : {total - nonzero} ({100.0 * (total - nonzero) / total:.4f}%)")
    print(f"  最大差        : {max_diff} LSB")
    print(f"  差 1 LSB 点数 : {int((diff == 1).sum())}")
    print(f"  差 >1 点数    : {over1}")
    print(f"  alpha 非全 1  : {alpha_bad} 像素")

    if max_diff <= 1 and over1 == 0 and alpha_bad == 0:
        print("PASS: GPU 降采样与参考 <=1 LSB(alpha 恒 255)")
        return 0
    print("FAIL: 超出 1 LSB 容差或 alpha 异常 —— 检查 HLSL 公式/坐标/分量序")
    return 1


if __name__ == "__main__":
    sys.exit(main())
