# -*- coding: utf-8 -*-
"""verify_chroma_forward:U/V 平面前向对照(shader 数学 vs 实测平面)。

同一帧:dump hdrColor → 精确按 PqToYuv shader 数学算期望 U/V → 与
Enhance 实测平面逐像素对照。匹配 = 编码忠实(青色另有来源);
不匹配 = shader/cbuffer 有 bug。
运行: <部署根>\\python.exe testkit\\verify_chroma_forward.py
"""
import ctypes
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402
testenv.ini_restore_on_exit()  # 部署 ini 现场保护(2026-09-25 收敛)

testenv.require_env()
testenv.kill_panel()
testenv.kill_mpv()

os.environ["VSDLSSNR_DUMP"] = "1"
os.environ["VSDLSSNR_DUMP_SKIP"] = "5"  # dump 与对比帧同帧号

k32 = ctypes.windll.kernel32
for k, v in {"vsr_mode": "2", "vsr_scale_x100": "200", "vsr_strength": "2",
             "hdr_enabled": "1"}.items():
    assert k32.WritePrivateProfileStringW("rtxvideo", k, v, testenv.INI), k

import time  # noqa: E402
import numpy as np  # noqa: E402
import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402

dark = core.std.BlankClip(width=640, height=480, format=vs.YUV420P8, color=[40,128,128])
bright = core.std.BlankClip(width=640, height=480, format=vs.YUV420P8, color=[200,128,128])
src = core.std.StackHorizontal([dark, bright])
ret = core.dlssnr.Enhance(src, vsr_mode=2, vsr_scale=2.0, hdr_enabled=1)
N = 5
f = ret.get_frame(N)
y_meas = np.asarray(f[0]).astype(np.float64) / 1023.0
u_meas = np.asarray(f[1]).astype(np.float64) / 1023.0
v_meas = np.asarray(f[2]).astype(np.float64) / 1023.0

dump = os.path.join(testenv.ROOT, "dump_hdrcolor.bin")
for _ in range(80):
    if os.path.exists(dump) and os.path.getsize(dump) > 1000000:
        break
    time.sleep(0.1)
# dump 是 dump-site 帧的(与 N 同帧:第一真运动帧;逐位对齐靠同帧号)
hdr = (np.fromfile(dump, dtype=np.uint16)
       .view(np.float16).astype(np.float64).reshape(ret.height, ret.width, 4))

M = np.array([[0.6274, 0.3293, 0.0433], [0.0690, 0.9195, 0.0112], [0.0164, 0.0880, 0.8955]])
KR, KB = 0.2627, 0.0593
M1, M2 = 2610 / 16384, 2523 / 4096 * 128
C1, C2, C3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32


def pq(nits):
    p = np.power(np.clip(nits / 10000.0, 0, 1), M1)
    return np.power((C1 + C2 * p) / (1 + C3 * p), M2)


def to_pq2020(lin709):
    return pq(np.clip(lin709, 0, None) @ M.T * 80.0)


# shader 2x2 box:色度输出 tid → 4 个 luma 域采样点(同尺寸 = tid*2 + 0.5/1.5)
grp = np.stack([hdr[0::2, 0::2, :3], hdr[0::2, 1::2, :3],
                hdr[1::2, 0::2, :3], hdr[1::2, 1::2, :3]])
pq4 = to_pq2020(grp)
pq_avg = pq4.mean(axis=0)
y_exp = pq_avg @ np.array([KR, 1 - KR - KB, KB])
cb_exp = (pq_avg[:, :, 2] - y_exp) * (0.5 / (1 - KB))
cr_exp = (pq_avg[:, :, 0] - y_exp) * (0.5 / (1 - KR))
LO_C, SPAN_C = 512.0 / 1023.0, 896.0 / 1023.0
u_exp = np.clip(cb_exp * SPAN_C + LO_C, 0, 1) * 1023.0
v_exp = np.clip(cr_exp * SPAN_C + LO_C, 0, 1) * 1023.0

u_m = u_meas[:u_exp.shape[0], :u_exp.shape[1]] * 1023.0
v_m = v_meas[:v_exp.shape[0], :v_exp.shape[1]] * 1023.0
print("U: meas mean=%.1f exp mean=%.1f max|Δ|=%.1f" % (
    u_m.mean(), u_exp.mean(), np.abs(u_m - u_exp).max()))
print("V: meas mean=%.1f exp mean=%.1f max|Δ|=%.1f" % (
    v_m.mean(), v_exp.mean(), np.abs(v_m - v_exp).max()))
print("V<500 占比: meas=%.1f%% exp=%.1f%%" % (
    (v_m < 500).mean() * 100, (v_exp < 500).mean() * 100))
print("pq.r vs y(编码域): mean(pq.r − y) = %.4f" % (pq_avg[:, :, 0] - y_exp).mean())
print("pq.b vs y: mean(pq.b − y) = %.4f" % (pq_avg[:, :, 2] - y_exp).mean())
sys.exit(0)
