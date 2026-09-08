# -*- coding: utf-8 -*-
"""稳态性能基准:RGBS->NGX->RGBS 完整往返的帧耗时(各分辨率)。

验证点:
  - 首帧含 NGX init(~1s),warmup 后测稳态
  - 输出各分辨率 ms/frame,可与 README 的性能数据(RTX 3080: 720p 18ms /
    1080p 33ms)对照

运行: <部署根>\\python.exe testkit\\test_perf.py
"""
import time
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402

testenv.require_env()
testenv.require_nvidia()

import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402


def bench(w, h, n=12):
    clip = core.std.BlankClip(width=w, height=h, format=vs.YUV420P8, color=[138, 169, 91])
    ret = core.dlssnr.Enhance(clip)
    ret.get_frame(0)  # warmup: plugin/NGX init 只发生在首帧
    t0 = time.perf_counter()
    for i in range(1, n + 1):
        ret.get_frame(i)
    dt = time.perf_counter() - t0
    print(f"{w}x{h}: {n} frames in {dt:.2f}s -> {n/dt:.1f} fps ({dt/n*1000:.1f} ms/frame steady-state)")


bench(1280, 720)
bench(1920, 1080)
