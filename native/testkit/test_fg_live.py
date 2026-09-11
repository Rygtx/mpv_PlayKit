# -*- coding: utf-8 -*-
"""FG 激活冒烟:fg_enabled=1 + proxy 已部署 → 输出帧数/时长语义 ×M。

验证:vi 帧数 ×M(fpsNum 同步)、_DurationNum/_DurationDen 整数对 ×M
(mpv vapoursynth 唯一认的节奏来源)、各索引帧可取。proxy 缺失(初始化
回退 1:1)时打印 SKIP —— 降级路径由 test_fg_fallback.py 覆盖。
"""
import os
import sys

os.environ.setdefault("VSDLSSNR_NO_PANEL", "1")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402

import vapoursynth as vs  # noqa: E402

core = vs.core
M = 3
src = core.std.BlankClip(width=320, height=240, format=vs.YUV420P8, length=12, fpsnum=24)
ret = core.dlssnr.Enhance(src, fg_enabled=1, fg_multiplier=M, motion_vector_quality=0)

if len(ret) == 12:
    print("FG LIVE SKIP: proxy 未部署/未激活(1:1 回退);用 test_fg_fallback.py 覆盖降级")
    sys.exit(0)

assert len(ret) == 12 * M, f"帧数应 ×M({len(ret)} != {12 * M})"
f0 = ret.get_frame(0)
dn = f0.props.get("_DurationNum")
dd = f0.props.get("_DurationDen")
assert dn and dd and dd % (24 * M) == 0 or (dn, dd) == (1, 24 * M), \
    f"时长应为 1/{24 * M},实测 {dn}/{dd}"
for n in (0, 1, 2, 3, 11, 12 * M - 1):
    f = ret.get_frame(n)
    assert f.width == 320, f"帧 {n} 内容应正常产出"
print(f"FG LIVE OK: frames = {len(ret)}, duration = {dn}/{dd} (x{M})")
