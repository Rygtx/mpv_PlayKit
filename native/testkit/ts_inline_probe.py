# -*- coding: utf-8 -*-
"""TS-INLINE 探针(2026-10-02 NOTE 复核协议):timestamp 内联 NGX 同 CL 的 A/B。

A/B 由环境变量切:VSDLSSNR_TS_INLINE=1 = 内联组(被验证的危险组合);
不设 = 对照组(现行独立括号 CL)。每组跑 N 帧,计数证据:
  - dlssnr_timing.log 的 "TS-INLINE: ... ok" 行 = 内联组合存活帧数
  - "SEH"/设备移除行 = 故障留痕;脚本非零退出 = 帧路径失败

运行: <部署根>\\python.exe testkit\\ts_inline_probe.py [帧数,默认 120]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402

testenv.require_env()
testenv.require_nvidia()

import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402

n = int(sys.argv[1]) if len(sys.argv) > 1 else 120  # 目标 eval 次数(源帧)
inline = os.environ.get("VSDLSSNR_TS_INLINE") == "1"
# FG 开启时胶水层 1 次 ProcessFrame 产出 4 个输出帧(输出 fps ×4):
# 要触发 n 次 eval 须取 4n 个输出帧(2026-10-02 实测:取 121 帧只得 31 eval)。
n_out = n * 4
clip = core.std.BlankClip(width=640, height=360, format=vs.YUV420P8,
                          color=[138, 169, 91], length=n_out + 1)
ret = core.dlssnr.Enhance(clip)
ret.get_frame(0)  # warmup:首帧含 init(~1s),不计
t0 = time.perf_counter()
for i in range(1, n_out + 1):
    ret.get_frame(i)
print(f"PROBE-RUN: inline={int(inline)} out_frames={n_out}/{n_out} ok "
      f"(~{n} evals) in {time.perf_counter() - t0:.1f}s -> NO CRASH, NO DEVICE LOST")
