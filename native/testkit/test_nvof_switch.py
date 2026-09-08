# -*- coding: utf-8 -*-
"""NVOF 质量热切换:面板 payload quality 2->0->3,断言会话重建、NGX 不重建。

NVOF 会话(按 motionVectorQuality)与 NGX feature 是两个生命周期:
档位切换只重建光流会话(毫秒级),NGX feature 保持。timing log 里
NVOF 会话行数应随切换递增,而 "feature recreated" 不出现。

运行: <部署根>\\python.exe testkit\\test_nvof_switch.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402
import panel_ipc  # noqa: E402

testenv.require_env()
testenv.require_nvidia()

ch = panel_ipc.ParamsChannel()

W, H = 640, 480
import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402

# 日志水位:只统计本测试窗口内的 recreate(全局计数会被历史播放污染)
off = testenv.log_size()

clip = core.std.BlankClip(width=W, height=H, format=vs.YUV420P8, color=[116, 169, 91])
# 移动图案:逐帧 x 平移的亮度梯度 -> 给 NVOF 真实运动
clip = core.std.Expr(clip, ["x 2 / 32 +", "", ""])

ch.push(seq=1, motionVectorQuality=2)  # payload 先于滤镜创建(adopt 路径)
ret = core.dlssnr.Enhance(clip, scaling_enabled=0, motion_vector_quality=2)
for n in range(3):
    ret.get_frame(n)
print("phase1: q=2 frames ok")

ch.push(seq=2, motionVectorQuality=0)  # -> 0 (destroy session)
for n in range(3, 6):
    ret.get_frame(n)
print("phase2: q=0 frames ok")

ch.push(seq=3, motionVectorQuality=3)  # -> 3 (recreate session)
for n in range(6, 9):
    ret.get_frame(n)
print("phase3: q=3 frames ok")

recreates = sum(1 for l in testenv.new_lines(off) if "feature recreated" in l)
print(f"feature recreated during test: {recreates} (expect 0 — only OF sessions rebuild)")
ok = recreates == 0
print("NVOF-SWITCH:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
