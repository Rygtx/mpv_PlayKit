# -*- coding: utf-8 -*-
"""verify_chroma_forward:U/V 平面前向对照(shader 数学 vs 实测平面)。

同一帧:dump hdrColor → 精确按 PqToYuv shader 数学算期望 U/V → 与
Enhance 实测平面逐像素对照。匹配 = 编码忠实(青色另有来源);
不匹配 = shader/cbuffer 有 bug。
运行: <部署根>\\python.exe testkit\\verify_chroma_forward.py
"""
import ctypes
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402
testenv.ini_restore_on_exit()  # 部署 ini 现场保护(2026-09-25 收敛)

testenv.require_env()
testenv.kill_panel()
testenv.kill_mpv()

os.environ["VSDLSSNR_DUMP"] = "1"
os.environ["VSDLSSNR_DUMP_SKIP"] = "5"  # dump 与对比帧同帧号

k32 = ctypes.windll.kernel32
testenv.set_rtxvideo_ini(vsr_mode="2", vsr_scale_x100="200",
                    vsr_strength="2", hdr_enabled="1")

import time  # noqa: E402
import numpy as np  # noqa: E402
import vapoursynth as vs  # noqa: E402
from vapoursynth import core

import pq2020  # noqa: E402
from pq2020 import KR, KB, to_pq2020  # noqa: E402  # 前向数学单一权威(共享)  # noqa: E402

dark = core.std.BlankClip(width=640, height=480, format=vs.YUV420P8, color=[40,128,128])
bright = core.std.BlankClip(width=640, height=480, format=vs.YUV420P8, color=[200,128,128])
src = core.std.StackHorizontal([dark, bright])

# 陈旧 dump 先删(2026-10-04,与 verify_pq_roundtrip 同族同步):必须在
# Enhance/get_frame 之前 —— 色度 dump 在首帧 Finish 内同步执行完(进程内
# 一次锁存),get_frame 之后删的就是刚写好的文件,锁存已置位永不重写;
# 不删则上一会话的残留 dump 直接过大小门 → 前向对照跑在陈旧帧上,
# PASS/FAIL 均不可信。
dump = os.path.join(testenv.HOST_DIR, "dump_hdrcolor.bin")
if os.path.exists(dump):
    os.remove(dump)

ret = core.dlssnr.Enhance(src, vsr_mode=2, vsr_scale=2.0, hdr_enabled=1)
N = 5
f = ret.get_frame(N)
y_meas = np.asarray(f[0]).astype(np.float64) / 1023.0
u_meas = np.asarray(f[1]).astype(np.float64) / 1023.0
v_meas = np.asarray(f[2]).astype(np.float64) / 1023.0

for _ in range(80):
    if os.path.exists(dump) and os.path.getsize(dump) > 1000000:
        break
    time.sleep(0.1)
# dump 是 dump-site 帧的(与 N 同帧:第一真运动帧;逐位对齐靠同帧号)
hdr = (np.fromfile(dump, dtype=np.uint16)
       .view(np.float16).astype(np.float64).reshape(ret.height, ret.width, 4))



# shader 2x2 box:色度输出 tid → 4 个 luma 域采样点(同尺寸 = tid*2 + 0.5/1.5)
grp = np.stack([hdr[0::2, 0::2, :3], hdr[0::2, 1::2, :3],
                hdr[1::2, 0::2, :3], hdr[1::2, 1::2, :3]])
pq4 = to_pq2020(grp)
pq_avg = pq4.mean(axis=0)
y_exp = pq_avg @ np.array([KR, 1 - KR - KB, KB])
cb_exp = (pq_avg[:, :, 2] - y_exp) * (0.5 / (1 - KB))
cr_exp = (pq_avg[:, :, 0] - y_exp) * (0.5 / (1 - KR))
LO_C, SPAN_C = 512.0 / 1023.0, 896.0 / 1023.0
u_exp = np.clip(cb_exp * SPAN_C + LO_C, 0, 1) * 1023.0
v_exp = np.clip(cr_exp * SPAN_C + LO_C, 0, 1) * 1023.0

u_m = u_meas[:u_exp.shape[0], :u_exp.shape[1]] * 1023.0
v_m = v_meas[:v_exp.shape[0], :v_exp.shape[1]] * 1023.0
print("U: meas mean=%.1f exp mean=%.1f max|Δ|=%.1f" % (
    u_m.mean(), u_exp.mean(), np.abs(u_m - u_exp).max()))
print("V: meas mean=%.1f exp mean=%.1f max|Δ|=%.1f" % (
    v_m.mean(), v_exp.mean(), np.abs(v_m - v_exp).max()))
print("V<500 占比: meas=%.1f%% exp=%.1f%%" % (
    (v_m < 500).mean() * 100, (v_exp < 500).mean() * 100))
print("pq.r vs y(编码域): mean(pq.r - y) = %.4f" % (pq_avg[:, :, 0] - y_exp).mean())
print("pq.b vs y: mean(pq.b - y) = %.4f" % (pq_avg[:, :, 2] - y_exp).mean())

# 判定(2026-10-05 评审修:此前只打印测量值恒 exit 0)。判定只锁 U/V 均值
# 偏差 —— 参考是"4 tap 平均后再转换",shader 是逐 tap 转换后平均,PQ 非线性
# 下 dark/bright 硬边界处存在合法逐像素偏差(参照 verify_pq_roundtrip 判例
# 不锁 max),均值判定只抓系统性错位(cMid/cSpan/矩阵错)。容差取宁松量级。
CHROMA_MEAN_TOL = 8.0  # 0-1023 code 域
du_mean = abs(u_m.mean() - u_exp.mean())
dv_mean = abs(v_m.mean() - v_exp.mean())
ok = du_mean <= CHROMA_MEAN_TOL and dv_mean <= CHROMA_MEAN_TOL
print("RESULT:", "PASS" if ok else "FAIL",
      "|dU|mean=%.1f |dV|mean=%.1f (tol=%.1f)" % (du_mean, dv_mean, CHROMA_MEAN_TOL))
sys.exit(0 if ok else 1)
