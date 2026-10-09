# -*- coding: utf-8 -*-
"""BT.2020 / 全范围系数数值验收(2026-10-08 评审修:2020 系数分支此前
零数值验收 —— 与 full-range 色度 ×2 事故同款翻车模式,check_yuv_convert
当时只钉 709-limited)。

驱动插件落 VSDLSSNR_DUMP(_ColorSpace=9 走 _ColorSpace fallback 路径 /
_ColorRange=0 走 full-range cSpan 路径),numpy 参考按同矩阵/范围重算,
两段转换均须 <=2 LSB。dump 落盘闩锁进程级一次 → 每配置独立子进程;
参考实现单一权威 = check_yuv_convert.check。

运行: <部署根>\\python.exe testkit\\test_yuv2020.py
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import testenv  # noqa: E402
import check_yuv_convert  # noqa: E402

testenv.require_env()
testenv.require_nvidia()
# 幸存面板重推 payload(nr=0)会让子进程走直通:dump 照落、check 恒等
# 往返 —— 全 PASS 但系数分支从未被真实覆盖。
testenv.solo_panel()
# 环境强制管线色同理(2026-10-09 评审修,与 test_nvof_yuv10 的 require 同款):
# VSDLSSNR_NR_FORMAT 会改子进程管线色路径,系数分支照旧测不到,绿灯是
# 覆盖空洞。子进程 env 继承本进程,模块级拦一次即可。
testenv.require(not os.environ.get("VSDLSSNR_NR_FORMAT"),
                "VSDLSSNR_NR_FORMAT 强制覆盖使本测试的系数分支前提不成立")

DUMP_NAMES = ["dump_yuvin_y.bin", "dump_yuvin_u.bin", "dump_yuvin_v.bin",
              "dump_input.bin", "dump_output.bin",
              "dump_y_plane.bin", "dump_u_plane.bin", "dump_v_plane.bin"]

# (标签, 额外 frame props, 期望矩阵/范围):2020 配置故意只设 _ColorSpace
# 不设 _Matrix —— 测的正是 _Matrix 缺失时的 fallback 映射(插件消费路径)。
CONFIGS = [
    ("2020-limited", "clip = core.std.SetFrameProps(clip, _ColorSpace=9)", "2020", "limited"),
    ("709-full", "clip = core.std.SetFrameProps(clip, _ColorRange=0)", "709", "full"),
]

W, H = 320, 240

CHILD_TMPL = """
import sys
import numpy as np
import vapoursynth as vs
from vapoursynth import core
# 空间梯度(色度上采双线性路径才有判别力;本部署 R73 的 Expr 无坐标访问,
# 用 ModifyFrame + numpy 直画)。图案钉在色域菱形内(全部 |RGB|<1 有余量,
# 无任何钳制触发):系数验收不测钳制顺序 —— 出三角区的 g 计算依赖 shader
# 的 saturate 次序,与矩阵系数无关,卷进来只会假 FAIL。
W, H = {w}, {h}
base = core.std.BlankClip(width=W, height=H, format=vs.YUV420P8,
                          color=[116, 128, 128])
def _paint(n, f):
    cf = f.copy()
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    np.asarray(cf[0])[:] = 64 + (xx + yy) * 0.272      # Y ∈ [64,216]
    cy, cx = np.mgrid[0:H // 2, 0:W // 2].astype(np.float32)
    np.asarray(cf[1])[:] = 128 + (cx - 80) * 0.125     # U ∈ [118,138]
    np.asarray(cf[2])[:] = 128 + (cy - 60) * 0.125     # V ∈ [121,135]
    return cf
clip = core.std.ModifyFrame(base, base, _paint)
{props}
ret = core.dlssnr.Enhance(clip, nr_enabled=1)
for n in range(3):
    ret.get_frame(n)
"""

results = []
with testenv.ini_snapshot(clear=True):  # 用户 ini 不参与(vsr/vsr_mode 等会改管线)
    for label, props, matrix, rng in CONFIGS:
        dumps = {n: os.path.join(testenv.HOST_DIR, n) for n in DUMP_NAMES}
        for p in dumps.values():
            if os.path.exists(p):
                os.remove(p)
        env = dict(os.environ, VSDLSSNR_DUMP="1", VSDLSSNR_NO_PANEL="1")
        try:
            proc = subprocess.run(
                [sys.executable, "-c", CHILD_TMPL.format(w=W, h=H, props=props)],
                env=env, timeout=300)
            rc = proc.returncode
        except subprocess.TimeoutExpired:
            rc = -1  # NV.init 卡死是已知失败形态:按失败计,继续下一配置
        missing = [n for n, p in dumps.items() if not os.path.exists(p)]
        if rc != 0 or missing:
            # 单配置失败记 FAIL 不短路(2026-10-09 评审修:此前 sys.exit(1)
            # 让 709-full 的 full-range cSpan 路径零验收,运维还以为跑全了)。
            print(f"{label}: SKIP 子进程失败 rc={rc} 缺 dump={missing}")
            results.append((label, False))
            continue
        ok, report = check_yuv_convert.check(testenv.HOST_DIR, W, H, matrix, rng)
        print(f"== {label} ==")
        for line in report:
            print(line)
        results.append((label, ok))

ok = all(r for _, r in results)
for label, r in results:
    print(f"  {label}: {'PASS' if r else 'FAIL'}")
print("YUV2020:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
