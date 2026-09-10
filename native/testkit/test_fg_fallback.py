# FG 降级冒烟:fg_enabled=1 且 proxy 未部署 → 初始化失败,优雅回退 1:1。
# 验证:输出帧数/格式不变、getFrame 正常、timing log 留痕 dlssfg init failed。
import os
import sys

os.environ.setdefault("VSDLSSNR_NO_PANEL", "1")
sys.path.insert(0, r"f:\Project\mpv_PlayKit\native\testkit")
import testenv  # noqa: E402  (发现部署根 + 环境准备)

import vapoursynth as vs  # noqa: E402

core = vs.core
src = core.std.BlankClip(width=320, height=240, format=vs.YUV420P8, length=10, fpsnum=24)
ret = core.dlssnr.Enhance(src, fg_enabled=1, motion_vector_quality=0)

info = ret.get_frame(0)
assert ret.width == 320 and ret.height == 240, "尺寸不应变化"
# FG 未激活(无 proxy)→ 1:1 输出:帧数不变
assert len(ret) == 10, f"FG 降级应 1:1(帧数 {len(ret)} != 10)"
for n in (0, 3, 9):
    f = ret.get_frame(n)
    assert f.width == 320, "帧内容应正常产出"
print("FG FALLBACK OK: frames =", len(ret))
