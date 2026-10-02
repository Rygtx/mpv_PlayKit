# -*- coding: utf-8 -*-
"""① 复验:DLSSG ColorBuffersHDR 路径压高光(2026-09-24 实测 276→70 nits)。

A/B:ini fg_hdr_interp 0(修复形态,SDR 域插帧)/ 1(HDR 域插帧,实验)。
HDR 会话 + FG 4x,移动条纹源 + 固定亮带(恒有高光可压)。逐输出帧解 PQ
码 → nits,按 gen 分组(gen 0=真实帧,1..3=插值帧)。判据:
  压高光成立  = 插值帧 max-nits 中位数 ≪ 真实帧(复现 ~25% 比值)
  未复现      = 两组中位数相近(±10% 量级)

运行: <部署根>\\python.exe testkit\\hdr_interp_nits.py(先设好 ini)
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402

testenv.require_env()
testenv.require_nvidia()

import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402

M1, M2 = 2610 / 16384, 2523 / 4096 * 128
C1, C2, C3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32


def code_to_nits(c):
    """PQ 码值(0..1)→ 绝对亮度 nits(ST 2084 逆 EOTF,向量化)。"""
    c = np.clip(c, 1e-6, 1.0)
    p = np.power(c, 1.0 / M2)
    n = np.power(np.clip((p - C1) / (C2 - C3 * p), 0.0, None), 1.0 / M1)
    return n * 10000.0


SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "media",
                   "hdr_hint_sync_test.y4m")
NSRC = 96

clip = core.bs.VideoSource(SRC)[:NSRC]
ret = core.dlssnr.Enhance(clip)
n = ret.num_frames
gen_max = {g: [] for g in range(4)}
for i in range(n):
    f = ret.get_frame(i)
    y = np.asarray(f[0]).astype(np.float64) / 1023.0  # P10 右对齐 word → PQ 码
    nits = code_to_nits(y)
    gen_max[i % 4].append((float(nits.max()), float(np.percentile(nits, 99.9))))

interp = "?"
print(f"HDR-INTERP: out_frames={n} src={NSRC}")
for g in range(4):
    mx = np.array([v[0] for v in gen_max[g]])
    p = np.array([v[1] for v in gen_max[g]])
    name = "real" if g == 0 else f"gen{g}"
    print(f"HDR-INTERP: {name:>4} max-med={np.median(mx):7.1f}nits "
          f"p999-med={np.median(p):7.1f}nits frames={len(mx)}")
real_med = np.median([v[0] for v in gen_max[0]])
interp_med = np.median([v[0] for v in gen_max[1] + gen_max[2] + gen_max[3]])
if real_med > 1.0:
    ratio = interp_med / real_med
    print(f"HDR-INTERP: interp/real max-nits ratio = {ratio:.2f} -> "
          + ("CRUSHED (claim reproduced)" if ratio < 0.6 else "NOT REPRODUCED"))
else:
    print("HDR-INTERP: content lacks highlights, inconclusive")
