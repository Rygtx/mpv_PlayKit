# -*- coding: utf-8 -*-
"""rtx_hdr_diag:TrueHDR 输出恒黑排查(黑盒;场景由命令行选择)。

背景:排查期的独立 D3D12 探针(单队列/跨队列/VSR→fence→跨队列 HDR 场景)
在本机全部正常 —— 插件内仍有差异。
本脚本在插件本体上拿可迭代复现:Enhance(vsr+hdr) 读回帧做恒黑判定。

场景:
  hdr    仅 TrueHDR(vsr_mode=0, hdr_enabled=1)—— 隔离 TrueHDR 本体
  vsrhdr VSR(2x)+ TrueHDR —— 用户实际档位
  vsr    仅 VSR(2x)—— 已知好基线(输出应有内容)

流程:杀面板/残留 mpv → 写部署 ini [rtxvideo](部署 ini 优先于 VS 参数,
BridgeLoadIni 在 create 覆盖)→ Enhance 读回帧 → luma/chroma 统计 +
timing STATUS 尾部。

运行: <部署根>\\python.exe testkit\\test_rtx_hdr_diag.py [hdr|vsrhdr|vsr]
"""
import ctypes
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402

testenv.require_env()

import numpy as np  # noqa: E402
import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402

scenario = sys.argv[1] if len(sys.argv) > 1 else "hdr"

testenv.kill_panel()
testenv.kill_mpv()

# [rtxvideo] 场景写入:BridgeLoadIni 在 create 覆盖 VS 参数,ini 是唯一入口。
k32 = ctypes.windll.kernel32


def wr(key, val):
    assert k32.WritePrivateProfileStringW("rtxvideo", key, val, testenv.INI), key


scenes = {
    "hdr":    {"vsr_mode": "0", "vsr_scale_x100": "200", "vsr_strength": "2", "hdr_enabled": "1"},
    "vsrhdr": {"vsr_mode": "2", "vsr_scale_x100": "200", "vsr_strength": "2", "hdr_enabled": "1"},
    "vsr":    {"vsr_mode": "2", "vsr_scale_x100": "200", "vsr_strength": "2", "hdr_enabled": "0"},
}
for k, v in scenes[scenario].items():
    wr(k, v)

# 2nd arg "min" = 最小管线(nr/fg 全关):排除 feature 共存因素。
# 写回 [dlssnr] 节(插件落盘回写在 teardown,脚本每次启动重写,以脚本为准)。
if len(sys.argv) > 2 and sys.argv[2] == "min":
    for k, v in {"nr_enabled": "0", "fg_enabled": "0"}.items():
        assert k32.WritePrivateProfileStringW("dlssnr", k, v, testenv.INI), k
    print("[min] nr_enabled=0 fg_enabled=0")

W, H = 640, 480
# 确定性两值图案:左半 Y=40,右半 Y=200(不依赖 std.Expr 坐标语义)
dark = core.std.BlankClip(width=W // 2, height=H, format=vs.YUV420P8, color=[40, 128, 128])
bright = core.std.BlankClip(width=W // 2, height=H, format=vs.YUV420P8, color=[200, 128, 128])
clip = core.std.StackHorizontal([dark, bright])

kw = {
    "hdr":    dict(vsr_mode=0, vsr_scale=2.0, hdr_enabled=1),
    "vsrhdr": dict(vsr_mode=2, vsr_scale=2.0, hdr_enabled=1),
    "vsr":    dict(vsr_mode=2, vsr_scale=2.0, hdr_enabled=0),
}[scenario]

ret = core.dlssnr.Enhance(clip, **kw)
print(f"[{scenario}] out:", ret.format.name, ret.width, ret.height)

for n in (0, 5):
    f = ret.get_frame(n)
    a = np.asarray(f[0])
    ys = a[::16, ::16]
    print(f"  frame{n} luma: min={ys.min()} max={ys.max()} uniq={len(np.unique(ys))} mean={ys.mean():.1f}")
    if ret.format.num_planes > 1:
        c = np.asarray(f[1])[::8, ::8]
        print(f"         chroma: min={c.min()} max={c.max()} uniq={len(np.unique(c))} mean={c.mean():.1f}")

# timing STATUS 尾部(几何/rtx ready/失败原因都在这)
lines = []
if os.path.exists(testenv.TIMING_LOG):
    with open(testenv.TIMING_LOG, "r", encoding="utf-8", errors="ignore") as fh:
        lines = [l.strip() for l in fh.readlines() if l.strip()]
print("--- timing tail ---")
for l in lines[-12:]:
    print(" ", l)
sys.exit(0)
