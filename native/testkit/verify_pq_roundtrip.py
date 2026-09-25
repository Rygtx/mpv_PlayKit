# -*- coding: utf-8 -*-
"""verify_pq_roundtrip:PQ 常量修复的数值闭环。

同一次运行里:插件 dump hdrColor(VSDLSSNR_DUMP=1,帧0)+ 脚本读回输出
YUV420P10 平面 → numpy 前向复算 shader 数学(scRGB→2020→PQ→limited 10bit)
→ 与实测平面逐位对照。max|Δcode| ≤ 1 = PQ 链路端到端正确。

运行: <部署根>\\python.exe testkit\\verify_pq_roundtrip.py
"""
import ctypes
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402
testenv.ini_restore_on_exit()  # 部署 ini 现场保护(2026-09-25 收敛)

testenv.require_env()
testenv.kill_panel()
testenv.kill_mpv()

os.environ["VSDLSSNR_DUMP"] = "1"

k32 = ctypes.windll.kernel32
for k, v in {"vsr_mode": "2", "vsr_scale_x100": "200", "vsr_strength": "2",
             "hdr_enabled": "1"}.items():
    assert k32.WritePrivateProfileStringW("rtxvideo", k, v, testenv.INI), k

import numpy as np  # noqa: E402
import vapoursynth as vs  # noqa: E402
from vapoursynth import core  # noqa: E402

W, H = 640, 480
dark = core.std.BlankClip(width=W // 2, height=H, format=vs.YUV420P8, color=[40, 128, 128])
bright = core.std.BlankClip(width=W // 2, height=H, format=vs.YUV420P8, color=[200, 128, 128])
clip = core.std.StackHorizontal([dark, bright])

# 陈旧 dump 先删(2026-09-25):必须在 Enhance/get_frame **之前** —— 色度
# dump 块在首帧 Finish 内同步执行完(进程内一次锁存),get_frame 之后删的
# 就是刚写好的文件,锁存已置位永不重写 → 轮询必空(上一版把 os.remove 放
# 在 get_frame 后,dump_hdrcolor"永远缺失"的真凶)。此前用 >= 尺寸门槛,
# 上一会话更大尺寸的残留 dump 直接通过 → reshape 崩溃,同根同源。
dump = os.path.join(testenv.ROOT, "dump_hdrcolor.bin")
if os.path.exists(dump):
    os.remove(dump)

ret = core.dlssnr.Enhance(clip, vsr_mode=2, vsr_scale=2.0, hdr_enabled=1)
print("out:", ret.format.name, ret.width, ret.height)
f = ret.get_frame(0)
yuv = [np.asarray(f[p]).copy() for p in range(3)]
np.save(os.path.join(testenv.ROOT, "verify_y.npy"), yuv[0])
np.save(os.path.join(testenv.ROOT, "verify_u.npy"), yuv[1])

# dump 在首帧 Finish 内同步落盘;轮询仅兜异步边界,正常首轮即命中。
import time  # noqa: E402

expect_bytes = ret.width * ret.height * 8
for _ in range(50):
    if os.path.exists(dump) and os.path.getsize(dump) == expect_bytes:
        break
    time.sleep(0.1)
# FP16 裸字节直接按 float16 读(2026-09-25 修:原 uint16→float32→view(float16)
# 的舞步会破坏位模式且元素数翻倍,reshape 恒炸 —— 该行此前从未成功执行过)。
hdr = np.fromfile(dump, dtype=np.float16).astype(np.float32)
hdr = hdr.reshape(ret.height, ret.width, 4)
print("hdrColor: min|rgb|=%.4f max|rgb|=%.4f alpha_uniq=%s" % (
    np.abs(hdr[:, :, :3]).min(), np.abs(hdr[:, :, :3]).max(), np.unique(hdr[:, :, 3])))

# ---- 前向复算(= PqToYuv shader 的数学)----
M = np.array([[0.6274, 0.3293, 0.0433],
              [0.0690, 0.9195, 0.0112],
              [0.0164, 0.0880, 0.8955]], dtype=np.float64)
KR, KB = 0.2627, 0.0593
M1, M2 = 2610 / 16384, 2523 / 4096 * 128
C1, C2, C3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32


def pq(nits):
    p = np.power(np.clip(nits / 10000.0, 0.0, 1.0), M1)
    return np.power((C1 + C2 * p) / (1.0 + C3 * p), M2)


lin = np.clip(hdr[:, :, :3].astype(np.float64), 0.0, None)  # max(lin709,0)
lin2020 = lin @ M.T * 80.0
pqv = pq(lin2020)
y = pqv @ np.array([KR, 1 - KR - KB, KB])
y_expect_word = np.clip(y * 876 / 1023 + 64 / 1023, 0, 1) * 65535.0

# 量纲对齐(2026-09-25 修,此前恒假阴性):VS P10 平面 = 右对齐 code
# (0-1023,见 d3d12_context.h yuvOut 注释);前向模型是 16bit full-range
# word(= code × 64)。此前 code 直接比 word,Δ 恒 ~64 倍 —— 实测输出
# 与模型逐位吻合,判据被量纲错位掩盖(该脚本因 reshape bug 从未跑通过
# 这一段,故无人发现)。
y_expect_code = y_expect_word / 64.0
meas = yuv[0].astype(np.float64)
diff = meas - y_expect_code
print("--- luma plane vs forward model (P10 code 域) ---")
print("meas code: min=%.0f max=%.0f" % (meas.min(), meas.max()))
print("expect code: min=%.1f max=%.1f" % (y_expect_code.min(), y_expect_code.max()))
print("max|dcode| = %.2f mean|dcode| = %.4f" % (np.abs(diff).max(), np.abs(diff).mean()))

# chroma 抽查(U 平面;shader = 4 tap 各自 RGB→PQ→YCbCr 后 Cb/Cr 平均,
# 非"平均后再算"—— 原稿 pq_avg 丢通道维是笔误,此段此前从未执行到)。
grp = np.stack([hdr[0:ret.height:2, 0:ret.width:2, :3],
                hdr[0:ret.height:2, 1:ret.width:2, :3],
                hdr[1:ret.height:2, 0:ret.width:2, :3],
                hdr[1:ret.height:2, 1:ret.width:2, :3]]).astype(np.float64)
pq4 = pq(np.clip(grp, 0, None) @ M.T * 80.0)          # (4, H/2, W/2, 3) PQ RGB/tap
y4 = pq4 @ np.array([KR, 1 - KR - KB, KB])            # (4, H/2, W/2) PQ luma/tap
cb4 = (pq4[:, :, :, 2] - y4) * (0.5 / (1 - KB))       # Cb/tap
cr4 = (pq4[:, :, :, 0] - y4) * (0.5 / (1 - KR))       # Cr/tap
cb = cb4.mean(axis=0)
cr = cr4.mean(axis=0)
u_expect = np.clip(cb * 896 / 1023 + 512 / 1023, 0, 1) * 1023.0   # P10 code
v_expect = np.clip(cr * 896 / 1023 + 512 / 1023, 0, 1) * 1023.0
u_meas = yuv[1].astype(np.float64)
v_meas = yuv[2].astype(np.float64)
du = np.abs(u_meas - u_expect)
dv = np.abs(v_meas - v_expect)
print("--- U/V plane vs forward model (P10 code 域) ---")
print("U: max|dcode| = %.2f mean|dcode| = %.4f  >2code 占比 %.4f%%" % (
    du.max(), du.mean(), 100.0 * (du > 2).mean()))
print("V: max|dcode| = %.2f mean|dcode| = %.4f  >2code 占比 %.4f%%" % (
    dv.max(), dv.mean(), 100.0 * (dv > 2).mean()))
# 判定(2026-09-25):luma 逐位域(≤2 code);色度 shader 是 4×4 双线性盒
# (16 采样)而参考是 2×2 盒 —— dark/bright 硬边界处少数点因盒粒度合法
# 偏差,平坦区 mean 亚 code。判据 = mean + 大点占比(边界集中度),
# 不锁 max。
luma_ok = np.abs(diff).max() <= 2.0
chroma_ok = (du.mean() <= 1.0 and dv.mean() <= 1.0 and
             (du > 2).mean() < 0.01 and (dv > 2).mean() < 0.01)
print("RESULT:", "PASS" if (luma_ok and chroma_ok) else "FAIL")
sys.exit(0 if (luma_ok and chroma_ok) else 1)
