# -*- coding: utf-8 -*-
"""稳态性能基准:YUV 原生直吃 -> NGX -> YUV 输出完整往返的帧耗时(各分辨率)。

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
# 前提钉死(2026-10-09):用户 ini nr_enabled=0 会覆盖下方 nr_enabled=1
# vpy 参数(插件顺序 vpy→ini→payload),基准静默退化为 memcpy 计时。
testenv.solo_panel()
testenv.ini_isolated()  # 清空 + 退出恢复(统一配方,防用户 ini 现场)

import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402


def bench(w, h, n=12):
    clip = core.std.BlankClip(width=w, height=h, format=vs.YUV420P8, color=[138, 169, 91])
    # nr 钉 1(2026-10-05 评审修):缺省 nr=0 走零 GPU 直通,实测会把 memcpy
    # 耗时当 NGX 稳态数据报(虚低一个量级)。
    ret = core.dlssnr.Enhance(clip, nr_enabled=1)
    ret.get_frame(0)  # warmup: plugin/NGX init 只发生在首帧
    t0 = time.perf_counter()
    for i in range(1, n + 1):
        ret.get_frame(i)
    dt = time.perf_counter() - t0
    print(f"{w}x{h}: {n} frames in {dt:.2f}s -> {n/dt:.1f} fps ({dt/n*1000:.1f} ms/frame steady-state)")


bench(1280, 720)
bench(1920, 1080)
