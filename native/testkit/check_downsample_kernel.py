# -*- coding: utf-8 -*-
"""降采样核判别:高频图案下内部分辨率档位必须产生不同输出。

低频平滑图案下 area-average 与 Lanczos2 会量化到相同 RGBA8(对低频内容
都近似恒等),无法区分核实现。高频图案必须让 res100/res50/mult1.5 分歧,
并真正踩到核的负瓣。这是 sanity 测试,不是与旧二进制的 A/B。

运行: <部署根>\\python.exe testkit\\check_downsample_kernel.py
"""
import ctypes
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402

testenv.require_env()
testenv.require_nvidia()

if os.path.exists(testenv.INI):
    os.remove(testenv.INI)
# 面板残留清场:面板 payload 会覆盖 vpy kwargs(#37 优先级),配置差异被抹平
testenv.kill_panel()
import time  # noqa: E402
time.sleep(1)
# 同进程多实例共享 NGX 时域历史(见 validate_params.py 注释):哈希断言
# 只在"每配置一个进程"下可靠。本脚本用 driver+worker 模式跑。
os.environ["VSDLSSNR_NO_PANEL"] = "1"

WORKER = r'''
import ctypes, hashlib, json, sys
import vapoursynth as vs
from vapoursynth import core
kw = json.loads(sys.argv[1])
clip = core.std.BlankClip(width=1280, height=720, length=100000,
                          format=vs.YUV420P8, color=[122,142,116])
# 高频 luma: ~40px 周期正弦 + 二次谐波细 checker
clip = core.std.Expr(clip, ["x 8 + 20 sin 5 * + 40 sin 3 * +", "", ""])
ret = core.dlssnr.Enhance(clip, **kw)
n = 100
for _ in range(5):
    ret.get_frame(n); n += 1
f = ret.get_frame(n); n += 1
hsh = hashlib.md5()
for row in range(0, 720, 8):
    addr = ctypes.cast(f.get_read_ptr(0), ctypes.c_void_p).value + f.get_stride(0) * row
    hsh.update(ctypes.string_at(addr, 1280 * 4))
print(hsh.hexdigest()[:10])
'''

import json  # noqa: E402
import subprocess  # noqa: E402


def capture(**kw):
    r = subprocess.run([sys.executable, "-c", WORKER, json.dumps(kw)],
                       capture_output=True, text=True)
    out = r.stdout.strip().splitlines()
    return out[-1] if out else f"(worker failed: {r.stderr[-200:]})"


a = capture()
print(f"hifreq default res100: {a}")
a2 = capture()
print(f"hifreq res100 again:   {a2}  {'DETERMINISTIC' if a2 == a else 'NONDET!'}")
b = capture(input_resolution=50)
print(f"hifreq res50:          {b}  {'DIFF' if b != a else 'SAME!'}")
c = capture(residual_multiplier=1.5)
print(f"hifreq res100 mult1.5: {c}  {'DIFF' if c != a else 'SAME!'}")

ok = (a2 == a) and (b != a) and (c != a)
print("DOWNSAMPLE-KERNEL:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
