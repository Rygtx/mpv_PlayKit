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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402
import panel_ipc  # noqa: E402

testenv.require_env()
testenv.require_nvidia()
testenv.solo_panel()  # 面板清场:幸存面板重推 payload 会盖掉本测试 ch.push 的 OF 配置
testenv.ini_isolated()  # 用户 ini nr_enabled=0 会让实例全关直通,vpy kwargs 被盖(合并序 vpy→ini→payload)→ 假 FAIL(2026-10-09 评审修,与 test_perf/verify_stats_keys 同批收敛)

ch = panel_ipc.ParamsChannel()

W, H = 640, 480
import numpy as np  # noqa: E402  模块级(2026-10-09 评审修:回调内逐帧 sys.modules 查找,依赖还藏出 linter 视线;同 check_yuv_convert/test_yuv2020 配方)
import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402

off = testenv.log_size()

# 10-bit YUV + 真实帧间运动:宽底图画空间梯度(ModifyFrame + numpy;本部署
# R73 的 Expr 无坐标访问,原注释"空间梯度"实为点变换),FrameEval 逐帧在
# 底图上平移取样窗口 = 真空间平移(此前静态帧五帧 bit 级相同,NVOF 零运动
# —— 断言碰巧仍成立,但运动路径从未被测到,2026-10-08 评审修)。
BW = W + 128
base = core.std.BlankClip(width=BW, height=H, format=vs.YUV420P10, color=[116, 169, 91])

def _paint(n, f):
    cf = f.copy()
    yy, xx = np.mgrid[0:H, 0:BW].astype(np.float32)
    np.asarray(cf[0])[:] = (xx * 0.5 + yy * 0.25 + 32)
    return cf

base = core.std.ModifyFrame(base, base, _paint)
clip = core.std.FrameEval(
    core.std.CropAbs(base, W, H, 0, 0),
    lambda n, f: core.std.CropAbs(base, W, H, (n * 16) % 128, 0), base)

ch.push(seq=1, motionVectorQuality=2, ofBackend=1)
ret = core.dlssnr.Enhance(clip, scaling_enabled=0, motion_vector_quality=2,
                          of_backend=1)
for n in range(5):
    ret.get_frame(n)

lines = testenv.new_lines(off)
fails = sum(1 for l in lines if "copy submit failed" in l)
disabled = sum(1 for l in lines if "nvof disabled" in l)
session = sum(1 for l in lines if "of session created backend=1" in l)
# 证明确实走了 10-bit 管线(RGBA16F),没测错路径。环境强制 NR_FORMAT 会改
# 管线色,前提不成立时按仓库约定 SKIP,不静默放宽断言(2026-10-08 评审修:
# 此前 or fmt_forced 让 fp16 门形同虚设)。
testenv.require(not os.environ.get("VSDLSSNR_NR_FORMAT"),
                "VSDLSSNR_NR_FORMAT 强制覆盖使 10-bit 管线前提不成立")
fp16 = sum(1 for l in lines if "color buffer RGBA16F" in l)
print(f"nvof sessions(NVOF): {session}, RGBA16F pipeline: {fp16}, "
      f"copy failed: {fails}, disabled: {disabled}")

ok = session > 0 and fails == 0 and disabled == 0 and fp16 > 0
print("NVOF-YUV10:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
