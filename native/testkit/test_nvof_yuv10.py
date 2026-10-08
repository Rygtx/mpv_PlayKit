# -*- coding: utf-8 -*-
"""10-bit 源 NVOF 回归:管线色 RGBA16F 时输入须走 shader 直写。

历史 bug(2026-10-08 用户报告):>8bit 且无 RTX 时管线色 = RGBA16F,
非 follow(降采样关)走 CopyTextureRegion 裸拷贝 RGBA16F→BGRA8 注册输入,
格式不匹配 → nvof CL Close 恒败 ×3 → NVOF 自禁用 → 补帧退化复制帧。
修复:非 BGRA8 管线色改走 NVOF_DOWNSAMPLE_HLSL 1:1 直写。

断言:NVOF 会话建立(backend=1)后无 "copy submit failed"/"nvof disabled",
且日志窗口确认走了 RGBA16F 管线(证明测的是出 bug 的那条路)。

运行: <部署根>\\python.exe testkit\\test_nvof_yuv10.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402
import panel_ipc  # noqa: E402

testenv.require_env()
testenv.require_nvidia()
testenv.kill_panel()
os.environ.setdefault("VSDLSSNR_NO_PANEL", "1")
time.sleep(1)

ch = panel_ipc.ParamsChannel()

W, H = 640, 480
import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402

off = testenv.log_size()

# 10-bit YUV + 逐帧平移梯度(给 NVOF 真实运动)
clip = core.std.BlankClip(width=W, height=H, format=vs.YUV420P10, color=[116, 169, 91])
clip = core.std.Expr(clip, ["x 2 / 32 +", "", ""])

ch.push(seq=1, motionVectorQuality=2, ofBackend=1)
ret = core.dlssnr.Enhance(clip, scaling_enabled=0, motion_vector_quality=2,
                          of_backend=1)
for n in range(5):
    ret.get_frame(n)

lines = testenv.new_lines(off)
fails = sum(1 for l in lines if "copy submit failed" in l)
disabled = sum(1 for l in lines if "nvof disabled" in l)
session = sum(1 for l in lines if "of session created backend=1" in l)
# 证明确实走了 10-bit 管线(RGBA16F),没测错路径
fp16 = sum(1 for l in lines if "color buffer RGBA16F" in l)
print(f"nvof sessions(NVOF): {session}, RGBA16F pipeline: {fp16}, "
      f"copy failed: {fails}, disabled: {disabled}")

# 环境强制 NR_FORMAT 会改管线色,此时 RGBA16F 前提不成立
fmt_forced = os.environ.get("VSDLSSNR_NR_FORMAT", "").lower() in ("fp16", "bgra8", "rgb10a2")

ok = session > 0 and fails == 0 and disabled == 0 and (fp16 > 0 or fmt_forced)
print("NVOF-YUV10:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
