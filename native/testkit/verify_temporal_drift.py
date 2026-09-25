# -*- coding: utf-8 -*-
"""verify_temporal_drift:连续帧采样 —— TrueHDR 时域模型随帧数的退化检查。

实机 = 连续播放(数百帧);此前 testkit 只看第 0 帧。本脚本顺序解码推进,
逐点采样输出 Y 码分布:若随帧数 rails(→1023/→64 二值化),即复现实机病灶。
运行前提:环境变量 VSDLSSNR_TEST_MEDIA = 任一真片源路径。
运行: <部署根>\\python.exe testkit\\verify_temporal_drift.py
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

k32 = ctypes.windll.kernel32
for k, v in {"vsr_mode": "2", "vsr_scale_x100": "200", "vsr_strength": "2",
             "hdr_enabled": "1"}.items():
    assert k32.WritePrivateProfileStringW("rtxvideo", k, v, testenv.INI), k

import numpy as np  # noqa: E402
import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402

MEDIA = os.environ.get("VSDLSSNR_TEST_MEDIA")
testenv.require(MEDIA and os.path.isfile(MEDIA),
                "未设 VSDLSSNR_TEST_MEDIA(任一真片源路径,连续解码 600 帧用)")
src = core.bs.VideoSource(MEDIA)
ret = core.dlssnr.Enhance(src, vsr_mode=2, vsr_scale=2.0, hdr_enabled=1)
print("out:", ret.format.name, ret.width, ret.height)

SAMPLES = [0, 10, 30, 60, 100, 150, 200, 300, 400, 600]
print(f"{'frame':>6} {'Ymin':>6} {'Ymax':>6} {'Ymean':>7} {'Yuniq':>6} {'Umin':>5} {'Umax':>5} {'>=990':>6}")
bad = False
for n in SAMPLES:
    f = ret.get_frame(n)
    y = np.asarray(f[0]).astype(np.float64) / 64.0
    u = np.asarray(f[1]).astype(np.float64) / 64.0
    uniq = len(np.unique((np.asarray(f[0])[::8, ::8] // 64)))
    rail = (y >= 990).mean() * 100
    print(f"{n:>6} {y.min():>6.0f} {y.max():>6.0f} {y.mean():>7.0f} {uniq:>6} "
          f"{u.min():>5.0f} {u.max():>5.0f} {rail:>5.1f}%")
    if uniq <= 4 and n > 0:
        bad = True
print("RESULT:", "RAIL(复现实机病灶)" if bad else "稳定(时域无退化)")
sys.exit(0)
