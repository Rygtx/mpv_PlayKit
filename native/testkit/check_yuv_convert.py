# -*- coding: utf-8 -*-
"""YUV 原生化数值验收:ConvertIn / ConvertOut 两段转换 vs python 参考。

依赖插件 dump(VSDLSSNR_DUMP=1 时首帧落盘,文件写在宿主 exe 旁):
  dump_yuvin_y/u/v.bin  R8/R16_UNORM 平面(YUV 输入)
  dump_input.bin        BGRA8(YUV->RGB 转换结果)
  dump_output.bin       BGRA8(NGX 输出)
  dump_y/u/v_plane.bin  R8/R16_UNORM 平面(RGB->YUV 转换结果)

python 参考矩阵/范围可选(2026-10-08 起支持 2020/full —— 此前钉死
709-limited,新系数分支零数值验收,正是 full-range 色度 ×2 事故的
翻车模式)。容差 <=2 LSB(float32 GPU vs numpy;UNORM 往返舍入含在内)。
跨矩阵/范围数值基准(8bit):limited 16/219/128/224,full 0/255/127.5/255。

用法:
  <部署根>\\python.exe testkit\\check_yuv_convert.py [dump目录] [W] [H] [matrix] [range]
  dump 目录缺省 = 部署根(插件默认落盘位置)
  matrix ∈ {709,601,2020}(缺省 709),range ∈ {limited,full}(缺省 limited)
  自动化驱动见 test_yuv2020.py(dump 进程级闩锁一次,每配置独立子进程)。
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402

import numpy as np  # noqa: E402

_KR_KB = {"709": (0.2126, 0.0722), "601": (0.299, 0.114), "2020": (0.2627, 0.0593)}


def check(d, W, H, matrix="709", rng="limited"):
    """跑两段转换对比,返回 (ok, 报告行列表)。参考跨度按 8bit 域计;
    16bit dump 在 read_plane 归一到 8bit 域(65535/257=255 整除)后走同
    一套参考 —— 此前 16bit 平面直读会与 8bit 参考差 ~256x LSB,必然
    假 FAIL(2026-10-09 评审修,潜在:现役配置全 8bit)。"""
    if W <= 0 or H <= 0 or W % 2 or H % 2:
        return False, [f"FAIL: W/H 须为正偶数(色度 2x2 与半宽 reshape 前提),got {W}x{H}"]
    CW, CH = (W + 1) // 2, (H + 1) // 2
    kr, kb = (np.float32(x) for x in _KR_KB[matrix])
    kg = np.float32(1.0) - kr - kb
    if rng == "limited":
        y_lo, y_span = np.float32(16.0), np.float32(219.0)
        c_mid, c_span = np.float32(128.0), np.float32(224.0)
    else:
        y_lo, y_span = np.float32(0.0), np.float32(255.0)
        c_mid, c_span = np.float32(127.5), np.float32(255.0)

    def read_plane(path, w, h):
        # bpp 自动探测(2026-10-05 评审修):头注声明 dump 可为 R8/R16_UNORM,
        # 此前固定按 1 字节/采样读,16-bit dump 整体错读。按文件实际大小判:
        # w*h*2 → 16bit(uint16 视图直读,右对齐采样字,÷257 落 8bit 域);
        # w*h → 8bit。DumpTextureToFile 逐行紧凑 fwrite,文件内无 pitch 间隙
        #(同旧注)。尺寸两不符 → ValueError;文件缺失 → OSError(2026-10-09
        # 评审修:此前只捕 ValueError,插件中途崩掉的缺 dump 直接裸 traceback
        # —— check 捕获转 (False, 报告):本模块被 CLI 与 test_yuv2020 进程内
        # import,SystemExit 会杀死父驱动)。
        raw = open(path, "rb").read()
        if len(raw) == w * h * 2:
            return np.frombuffer(raw, dtype=np.uint16).reshape(h, w).astype(np.float32) / np.float32(257.0)
        if len(raw) == w * h:
            return np.frombuffer(raw, dtype=np.uint8).reshape(h, w).astype(np.float32)
        raise ValueError(f"{path}: 大小 {len(raw)} 与 {w}x{h} 的 8/16bit 紧凑形态均不符")

    def read_bgra(path, w, h):
        raw = open(path, "rb").read()
        stride = w * 4
        arr = np.frombuffer(raw[: stride * h], dtype=np.uint8).reshape(h, w, 4)
        return arr.astype(np.float32)

    try:
        yuv_y = read_plane(os.path.join(d, "dump_yuvin_y.bin"), W, H)
        yuv_u = read_plane(os.path.join(d, "dump_yuvin_u.bin"), CW, CH)
        yuv_v = read_plane(os.path.join(d, "dump_yuvin_v.bin"), CW, CH)
        bgra_in = read_bgra(os.path.join(d, "dump_input.bin"), W, H)
        rgb_out = read_bgra(os.path.join(d, "dump_output.bin"), W, H)
        out_y = read_plane(os.path.join(d, "dump_y_plane.bin"), W, H)
        out_u = read_plane(os.path.join(d, "dump_u_plane.bin"), CW, CH)
        out_v = read_plane(os.path.join(d, "dump_v_plane.bin"), CW, CH)
    except (OSError, ValueError) as e:
        return False, [f"FAIL: {e}(缺失/截断/半写 dump?复跑对应配置子进程)"]

    report = []

    # ---- 1) ConvertIn: YUV -> RGB -> BGRA8 ----
    nY = (yuv_y - y_lo) / y_span
    # 色度双线性上采(与 shader 逐式一致:fc=(px+0.5)*0.5-0.5,4 邻域 clamp)
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    fc_y = (yy + np.float32(0.5)) * np.float32(0.5) - np.float32(0.5)
    fc_x = (xx + np.float32(0.5)) * np.float32(0.5) - np.float32(0.5)
    c0y = np.floor(fc_y).astype(np.int32)
    c0x = np.floor(fc_x).astype(np.int32)
    wy = fc_y - c0y.astype(np.float32)
    wx = fc_x - c0x.astype(np.float32)
    s0y = np.clip(c0y, 0, CH - 1)
    s0x = np.clip(c0x, 0, CW - 1)
    s1y = np.clip(c0y + 1, 0, CH - 1)
    s1x = np.clip(c0x + 1, 0, CW - 1)
    w00 = (1 - wx) * (1 - wy)
    w10 = wx * (1 - wy)
    w01 = (1 - wx) * wy
    w11 = wx * wy

    def bilinear(plane):
        return (plane[s0y, s0x] * w00 + plane[s0y, s1x] * w10
                + plane[s1y, s0x] * w01 + plane[s1y, s1x] * w11)

    nU = (bilinear(yuv_u) - c_mid) / c_span
    nV = (bilinear(yuv_v) - c_mid) / c_span
    r = nY + nV * 2 * (1 - kr)
    b = nY + nU * 2 * (1 - kb)
    g = (nY - kr * r - kb * b) / kg
    ref_b = np.clip(b, 0, 1)
    ref_g = np.clip(g, 0, 1)
    ref_r = np.clip(r, 0, 1)
    gpu_b = bgra_in[:, :, 0] / 255.0
    gpu_g = bgra_in[:, :, 1] / 255.0
    gpu_r = bgra_in[:, :, 2] / 255.0
    diffs = []
    for name, ref, gpu in (("B", ref_b, gpu_b), ("G", ref_g, gpu_g), ("R", ref_r, gpu_r)):
        dd = np.abs(np.round(ref * 255) - np.round(gpu * 255))
        diffs.append((name, int(dd.max()), int((dd > 2).sum())))
    alpha_ok = bool(np.all(bgra_in[:, :, 3] == 255))
    report.append(f"== ConvertIn(YUV->RGB,{matrix} {rng} 8bit)==")
    for name, mx, bad in diffs:
        report.append(f"  {name}: max diff={mx} LSB, >2 LSB 点数={bad}")
    report.append(f"  alpha 全 255: {alpha_ok}")
    in_ok = all(mx <= 2 and bad == 0 for _, mx, bad in diffs) and alpha_ok

    # ---- 2) ConvertOut: BGRA8 -> Y/U/V(luma 直算,chroma 2x2 box)----
    rgbN = rgb_out[:, :, :3] / 255.0  # .0=B .1=G .2=R(内存序)
    rN, gN, bN = rgbN[:, :, 2], rgbN[:, :, 1], rgbN[:, :, 0]
    nYo = rN * kr + gN * kg + bN * kb
    ref_y8 = np.round(np.clip(nYo, 0, 1) * y_span + y_lo)
    # chroma: 2x2 box 平均后转 cb/cr(shader 先平均后转换,仿射等价);
    # cb/cr ∈ [-0.5,0.5] 不做 [0,1] 钳制(负色度合法,偏移在映射里加)
    rA = rN.reshape(H // 2, 2, W // 2, 2).mean(axis=(1, 3))
    gA = gN.reshape(H // 2, 2, W // 2, 2).mean(axis=(1, 3))
    bA = bN.reshape(H // 2, 2, W // 2, 2).mean(axis=(1, 3))
    nYa = rA * kr + gA * kg + bA * kb
    ref_u8 = np.round((bA - nYa) * (0.5 / (1 - kb)) * c_span + c_mid)
    ref_v8 = np.round((rA - nYa) * (0.5 / (1 - kr)) * c_span + c_mid)
    d_y = np.abs(ref_y8 - np.round(out_y))
    d_u = np.abs(ref_u8 - np.round(out_u))
    d_v = np.abs(ref_v8 - np.round(out_v))
    report.append(f"== ConvertOut(RGB->YUV,{matrix} {rng} 8bit)==")
    report.append(f"  Y: max diff={int(d_y.max())} LSB, >2 LSB 点数={int((d_y > 2).sum())}")
    report.append(f"  U: max diff={int(d_u.max())} LSB, >2 LSB 点数={int((d_u > 2).sum())}")
    report.append(f"  V: max diff={int(d_v.max())} LSB, >2 LSB 点数={int((d_v > 2).sum())}")
    out_ok = int(d_y.max()) <= 2 and int(d_u.max()) <= 2 and int(d_v.max()) <= 2

    return in_ok and out_ok, report


def main() -> None:
    argv = sys.argv[1:]
    d = argv[0] if argv else testenv.HOST_DIR
    if len(argv) == 2:
        raise SystemExit("W 与 H 须成对给出(只给 W 会被静默按缺省 1920x1080 读,方向误导)")
    try:
        W, H = (int(x) for x in argv[1:3]) if len(argv) >= 3 else (1920, 1080)
    except ValueError:
        raise SystemExit(f"W/H 须为整数,got {argv[1:3]!r}")
    matrix = argv[3] if len(argv) >= 4 else "709"
    rng = argv[4] if len(argv) >= 5 else "limited"
    if matrix not in _KR_KB or rng not in ("limited", "full"):
        raise SystemExit(f"matrix ∈ {sorted(_KR_KB)}, range ∈ [limited, full];"
                         f"got {matrix!r}/{rng!r}")
    ok, report = check(d, W, H, matrix, rng)
    for line in report:
        print(line)
    print("PASS" if ok else "FAIL", f":两段转换均在 <=2 LSB({matrix} {rng})")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
