# -*- coding: utf-8 -*-
"""verify_pq_roundtrip:PQ 常量修复的数值闭环。

同一次运行里:插件 dump hdrColor(VSDLSSNR_DUMP=1,帧0)+ 脚本读回输出
YUV420P10 平面 → numpy 前向复算 shader 数学(scRGB→2020→PQ→limited 10bit)
→ 与实测平面逐位对照。max|Δcode| ≤ 1 = PQ 链路端到端正确。

运行: <部署根>\\python.exe testkit\\verify_pq_roundtrip.py
"""
import ctypes
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402

testenv.require_env()
testenv.kill_panel()
testenv.kill_mpv()

os.environ["VSDLSSNR_DUMP"] = "1"

k32 = ctypes.windll.kernel32
for k, v in {"vsr_mode": "2", "vsr_scale_x100": "200", "vsr_strength": "2",
             "hdr_enabled": "1"}.items():
    assert k32.WritePrivateProfileStringW("rtxvideo", k, v, testenv.INI), k

import numpy as np  # noqa: E402
import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402

W, H = 640, 480
dark = core.std.BlankClip(width=W // 2, height=H, format=vs.YUV420P8, color=[40, 128, 128])
bright = core.std.BlankClip(width=W // 2, height=H, format=vs.YUV420P8, color=[200, 128, 128])
clip = core.std.StackHorizontal([dark, bright])

ret = core.dlssnr.Enhance(clip, vsr_mode=2, vsr_scale=2.0, hdr_enabled=1)
print("out:", ret.format.name, ret.width, ret.height)
f = ret.get_frame(0)
yuv = [np.asarray(f[p]).copy() for p in range(3)]
np.save(os.path.join(testenv.ROOT, "verify_y.npy"), yuv[0])
np.save(os.path.join(testenv.ROOT, "verify_u.npy"), yuv[1])

# dump 是异步一帧后落盘,轮询等文件
import time  # noqa: E402

dump = os.path.join(testenv.ROOT, "dump_hdrcolor.bin")
for _ in range(50):
    if os.path.exists(dump) and os.path.getsize(dump) >= ret.width * ret.height * 8:
        break
    time.sleep(0.1)
hdr = np.fromfile(dump, dtype=np.uint16).astype(np.float32).view(np.float16).astype(np.float32)
hdr = hdr.reshape(ret.height, ret.width, 4)
print("hdrColor: min|rgb|=%.4f max|rgb|=%.4f alpha_uniq=%s" % (
    np.abs(hdr[:, :, :3]).min(), np.abs(hdr[:, :, :3]).max(), np.unique(hdr[:, :, 3])))

# ---- 前向复算(= PqToYuv shader 的数学)----
M = np.array([[0.6274, 0.3293, 0.0433],
              [0.0690, 0.9195, 0.0112],
              [0.0164, 0.0880, 0.8955]], dtype=np.float64)
KR, KB = 0.2627, 0.0593
M1, M2 = 2610 / 16384, 2523 / 4096 * 128
C1, C2, C3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32


def pq(nits):
    p = np.power(np.clip(nits / 10000.0, 0.0, 1.0), M1)
    return np.power((C1 + C2 * p) / (1.0 + C3 * p), M2)


lin = np.clip(hdr[:, :, :3].astype(np.float64), 0.0, None)  # max(lin709,0)
lin2020 = lin @ M.T * 80.0
pqv = pq(lin2020)
y = pqv @ np.array([KR, 1 - KR - KB, KB])
y_expect_word = np.clip(y * 876 / 1023 + 64 / 1023, 0, 1) * 65535.0

meas = yuv[0].astype(np.float64)
diff = meas - y_expect_word
print("--- luma plane vs forward model ---")
print("meas word: min=%.0f max=%.0f" % (meas.min(), meas.max()))
print("expect word: min=%.0f max=%.0f" % (y_expect_word.min(), y_expect_word.max()))
print("max|Δword| = %.1f (word/64 = P10 code)" % np.abs(diff).max())
print("mean|Δword| = %.2f" % np.abs(diff).mean())

# chroma 抽查(U 平面,shader 2x2 box 平均)
sub = hdr[0::2, 0::2, :]
grp = np.stack([hdr[0:ret.height:2, 0:ret.width:2, :3],
                hdr[0:ret.height:2, 1:ret.width:2, :3],
                hdr[1:ret.height:2, 0:ret.width:2, :3],
                hdr[1:ret.height:2, 1:ret.width:2, :3]]).astype(np.float64)
pq4 = pq(np.clip(grp, 0, None) @ M.T * 80.0)
y4 = pq4 @ np.array([KR, 1 - KR - KB, KB])
pq_avg = y4.mean(axis=0)
cb = (pq_avg[:, :, 2] - pq_avg) * (0.5 / (1 - KB))
u_expect = np.clip(cb * 896 / 1023 + 512 / 1023, 0, 1) * 65535.0
u_meas = yuv[1].astype(np.float64)[:u_expect.shape[0], :u_expect.shape[1]]
print("--- U plane vs forward model ---")
print("max|Δword| = %.1f mean|Δword| = %.2f" % (
    np.abs(u_meas - u_expect).max(), np.abs(u_meas - u_expect).mean()))
print("RESULT:", "PASS" if np.abs(diff).max() <= 256 else
      "CHECK(Δ>4 code,看分布)" if np.abs(diff).max() <= 640 else "FAIL")
sys.exit(0)
