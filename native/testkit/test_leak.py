# -*- coding: utf-8 -*-
"""泄漏检测:工作集必须平台化,不能随帧数线性增长。

150 帧采样 5 次,报告净增速率;>0.5 MB/帧判 LEAK。
内存探针用 psapi.GetProcessMemoryInfo,无需外部依赖。

运行: <部署根>\\python.exe testkit\\test_leak.py
"""
import ctypes
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402

testenv.require_env()
testenv.require_nvidia()

import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402


class PMC(ctypes.Structure):
    _fields_ = [("cb", ctypes.c_uint), ("PageFaultCount", ctypes.c_uint),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]


def ws_mb():
    pmc = PMC()
    pmc.cb = ctypes.sizeof(PMC)
    k32 = ctypes.WinDLL("kernel32")
    psapi = ctypes.WinDLL("psapi")
    psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.POINTER(PMC), ctypes.c_uint]
    psapi.GetProcessMemoryInfo.restype = ctypes.c_int
    h = k32.GetCurrentProcess()
    if not psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb):
        raise OSError("GetProcessMemoryInfo failed")
    return pmc.WorkingSetSize / 1048576


W, H, N = 1280, 720, 150
clip = core.std.BlankClip(width=W, height=H, format=vs.YUV420P8, color=[138, 169, 91])
ret = core.dlssnr.Enhance(clip)
ret.get_frame(0)  # warmup: init

samples = []
for i in range(1, N + 1):
    ret.get_frame(i)
    if i % 30 == 0:
        samples.append((i, ws_mb()))

base = samples[0][1]
print("frame | workingset_MB | delta_vs_30f_MB")
for i, m in samples:
    print(f"{i:5d} | {m:8.1f} | {m - base:+8.1f}")

growth = (samples[-1][1] - samples[0][1]) / (samples[-1][0] - samples[0][0])
print(f"net growth: {growth * 1024:.1f} KB/frame ({growth * 1000:.2f} MB per 1000 frames)")
print("LEAK" if growth > 0.5 else "OK (plateau)")
