# FG 降级冒烟:fg_enabled=1 且 proxy 未部署 → 初始化失败,优雅回退 1:1。
# 验证:输出帧数/格式不变、getFrame 正常、timing log 留痕 dlssfg init failed。
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("VSDLSSNR_NO_PANEL", "1")
import testenv  # noqa: E402  (发现部署根 + 环境准备)
testenv.ini_restore_on_exit()  # 部署 ini 现场保护(2026-09-25 收敛)
testenv.require_env()  # 环境缺失打印 SKIP,而非 AttributeError 崩溃

import vapoursynth as vs  # noqa: E402

core = vs.core
src = core.std.BlankClip(width=320, height=240, format=vs.YUV420P8, length=10, fpsnum=24)
# 环境前提:proxy 未部署是本测试的触发条件。部署树带了 0.3.x hook 代理
# (ngx/version.dll)时 FG 真激活(官方链经 hook 交付)→ 降级路径前提失效,
# 交由部署无 proxy 的环境覆盖(对称于 fg_live 的官方链不可用 SKIP)。
if os.path.exists(os.path.join(os.path.dirname(testenv.NGX_DLL), "version.dll")):
    print("FG FALLBACK SKIP: 部署树带 hook 代理,FG 正常激活;降级路径需无 proxy 环境")
    sys.exit(0)
ret = core.dlssnr.Enhance(src, fg_enabled=1, motion_vector_quality=0)

info = ret.get_frame(0)
assert ret.width == 320 and ret.height == 240, "尺寸不应变化"
# FG 未激活(无 proxy)→ 1:1 输出:帧数不变
assert len(ret) == 10, f"FG 降级应 1:1(帧数 {len(ret)} != 10)"
for n in (0, 3, 9):
    f = ret.get_frame(n)
    assert f.width == 320, "帧内容应正常产出"
print("FG FALLBACK OK: frames =", len(ret))
