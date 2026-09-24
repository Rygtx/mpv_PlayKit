# -*- coding: utf-8 -*-
"""test_format_coverage:非 420 格式收编冒烟 —— 444 直吃 / 高位深 / GRAY / RGB。

2026-09-24 撤直通守卫后:插件原生收编 YUV 计划族 {420,422,444} × 8-16bit
(直传,零预转换);vpy `_to_native` 只兜底非直收格式(GRAY/packed RGB 经
709 矩阵转 YUV444)。本测试用真实 mpv 播放路径逐条断言四条分支:
  444P8  (y4m C444)    -> 直吃 YUV444P8 (d8 create)
  444P10 (y4m C444p10) -> 直吃 YUV444P10 (d10 create)
  GRAY8  (y4m Cmono)   -> _to_native 转 YUV444P8 (d8 create)
  RGB    (PNG rgb24)   -> _to_native 709 矩阵转 YUV444P8 (d8 create)
(y4m 的 Crgb 标记 ffmpeg 不认 —— RGB 载体只能用 PNG 这类图像容器。)
断言:mpv 存活 + timing log 出现对应分辨率/位深且 nr=1 的 create 行
(直通或 vpy 崩溃 = create 行缺失)。
媒体:testmedia.CLIPS 统一定义,生成到 testkit/media/(共享,不删)。

用法: <部署根>\\python.exe <仓库>\\native\\testkit\\test_format_coverage.py
运行会临时改 vs-plugins\\dlssnr_ui.ini(nr_enabled=1, fg/rtx 关),结束恢复原值。
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402
import testmedia  # noqa: E402

VF = '--vf=vapoursynth="~~/vs/DLSSNR_NV.vpy"'


def main():
    import ctypes
    k32 = ctypes.windll.kernel32

    # 备份将被触碰的 ini 键,结束恢复(部署 ini 是用户的真实配置)
    touched = [("dlssnr", "nr_enabled"), ("dlssnr", "fg_enabled"),
               ("rtxvideo", "vsr_mode"), ("rtxvideo", "hdr_enabled")]
    backup = {}
    for sec, key in touched:
        buf = ctypes.create_unicode_buffer(64)
        k32.GetPrivateProfileStringW(sec, key, None, buf, 64, testenv.INI)
        backup[(sec, key)] = buf.value

    def set_ini(sec, key, val):
        assert k32.WritePrivateProfileStringW(sec, key, val, testenv.INI), f"{sec}.{key}"

    testenv.kill_panel()
    testenv.kill_mpv()
    time.sleep(1.5)
    # 无面板(payload=0):ini 直接裁决。NR 开、FG/RTX 关 —— create 行
    # 应为 nr=1 fg=0,正是"单独开 NR"这次被修直的路径。
    set_ini("dlssnr", "nr_enabled", "1")
    set_ini("dlssnr", "fg_enabled", "0")
    set_ini("rtxvideo", "vsr_mode", "0")
    set_ini("rtxvideo", "hdr_enabled", "0")

    media = testmedia.ensure(names=[c[0] for c in testmedia.CLIPS
                                    if c[0].startswith("dlssnr_fmt_")])
    failures = []
    try:
        for fname, _gen, aw, ah, depth, note in testmedia.CLIPS:
            if not fname.startswith("dlssnr_fmt_"):
                continue  # hdr_hint_sync_test.y4m = 420 基线载体,归各自的测试
            use_start = not fname.endswith(".png")  # 单帧 PNG 用 --loop=inf 供帧即可
            wm = testenv.log_size(testenv.MPV_TIMING_LOG)
            args = [testenv.MPV_EXE, "--input-ipc-server=mpvpipe", "--really-quiet",
                    "--volume=0", "--loop=inf", "--geometry=640x360"]
            if use_start:
                args.append("--start=2")
            args += [VF, media[fname]]
            errf = open(os.path.join(testenv.ROOT, "mpv_stderr.txt"), "a", encoding="utf-8")
            mpv = subprocess.Popen(args, cwd=testenv.ROOT, stderr=errf, stdout=subprocess.DEVNULL)
            time.sleep(10)
            alive = mpv.poll() is None
            testenv.kill_mpv()
            try:
                mpv.wait(timeout=10)
            except Exception:
                testenv.kill_mpv_tree(mpv.pid)
            errf.close()

            lines = testenv.new_lines(wm, testenv.MPV_TIMING_LOG)
            pat = re.compile(rf"create params {aw}x{ah}d{depth}\b.*\bnr=1\b")
            hit = [l for l in lines if pat.search(l)]
            ok = alive and hit
            print(f"{fname} ({note}): mpv {'存活' if alive else '提前退出!'}; "
                  f"create(nr=1, {aw}x{ah}d{depth}) {'命中' if hit else '未命中'} "
                  f"(新增 {len(lines)} 行)")
            if not ok:
                failures.append(fname)
                for l in lines[:6]:
                    print(f"    | {l[:140]}")
    finally:
        testenv.kill_mpv()
        for (sec, key), val in backup.items():
            k32.WritePrivateProfileStringW(sec, key, val, testenv.INI)

    if failures:
        print(f"FAIL: {', '.join(failures)}")
        return 1
    print("PASS: 四条格式路径均进滤镜(直吃 + 兜底转换)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
