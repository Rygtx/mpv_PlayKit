# -*- coding: utf-8 -*-
"""test_shape_hotswap: RTX/FG 形态热重建(hotMatch 放宽)回归。

2026-09-27 解耦收尾:vsr_mode/scale/hdr_enabled/fg_enabled/fg_hdr_interp
不再参与 hotMatch —— 切档经 DlssnrContext::Rebind → RecreateFeature 形态段
热重建(槽资源 + VSR/HDR/FG 会话 + NR feature),不再付 D3D12/NGX 冷启动
(~1s → 百 ms 级)。

断言(真实 mpv + ini 裁决 + IPC seek 触发链重建;日志水位做断言):
  基线  同形态 seek → "hot rebind kept feature"(老热交接路径不回归)
  HDR 开→关     → "hot rebind shape ... hdr=0" + 输出几何回 8bpp hdr=0
  VSR off→mode2 → shape 行 vsr=1 + out=960x540(640x360 ×1.5 偶收口)
  FG  off→on    → shape 段热运行 + 会话落定:激活 "fg active, output fps
                  x2"(640x480 等达标源)或优雅降级记因(DLSSG 有最小
                  分辨率约束,640x360 官方链 0xBAD0000B —— 两条皆合法,
                  共同点 = 无冷回落、无风暴)
  FG  on→off    → 输出 1:1 无 "fg active" 新行;落定 = shape fg=0(激活
                  成功路径)或 kept-feature(降级已清请求,请求与实态
                  一致)—— FgActive() 请求感知语义
  全窗口禁出现 "hot rebind failed" / "re-initializing"(任何一步冷回落)

用法: <部署根>\\python.exe <仓库>\\native\\testkit\\test_shape_hotswap.py
"""
import ctypes  # noqa: E402
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402
import testmedia  # noqa: E402

k32 = ctypes.windll.kernel32
TIMING_LOG = testenv.MPV_TIMING_LOG  # 宿主 = mpv.exe → 部署根


def ipc(cmd, retries=20):
    """管道连接重试(mpv 启动/前次残名释放均有竞态;同 hdr_hint_sync)。"""
    for _ in range(retries):
        try:
            with open(testenv.MPV_PIPE, "r+b", buffering=0) as p:
                p.write((json.dumps({"command": cmd}) + "\n").encode("utf-8"))
                time.sleep(0.3)
                try:
                    return p.read(65536).decode("utf-8", "replace")
                except OSError:
                    return "(no reply)"
        except FileNotFoundError:
            if cur_mpv[0] and subprocess.run(
                    ["tasklist", "/FI", f"PID eq {cur_mpv[0]}"],
                    capture_output=True, text=True).stdout.count("mpv.exe") == 0:
                return "(mpv dead)"
            time.sleep(1)
    return "(pipe never appeared)"


cur_mpv = [0]


def seek():
    """1ms exact seek → mpv 重建整条 VS 链(= 面板切档的 reseek 等价物)。"""
    r = ipc(["seek", "0.001", "relative+exact"])
    if "(mpv dead)" in r or "(pipe never appeared)" in r:
        raise RuntimeError(f"seek failed: {r}")
    time.sleep(0.5)


def wait_log(off, pred, timeout=25):
    """轮询日志新增行直到 pred(全量新行)为真;返回命中时的新行列表。"""
    deadline = time.time() + timeout
    lines = []
    while time.time() < deadline:
        lines = testenv.new_lines(off, TIMING_LOG)
        if pred(lines):
            return lines
        time.sleep(0.4)
    return lines


def main():
    testenv.require_env()
    testenv.require_nvidia()
    media = testmedia.ensure(names=["hdr_hint_sync_test.y4m"])["hdr_hint_sync_test.y4m"]

    touched = [("dlssnr", "nr_enabled"), ("dlssnr", "fg_enabled"),
               ("rtxvideo", "vsr_mode"), ("rtxvideo", "vsr_scale_x100"),
               ("rtxvideo", "hdr_enabled")]
    backup = {}
    for sec, key in touched:
        buf = ctypes.create_unicode_buffer(64)
        k32.GetPrivateProfileStringW(sec, key, None, buf, 64, testenv.INI)
        backup[(sec, key)] = buf.value

    testenv.kill_panel()
    testenv.kill_mpv()
    time.sleep(1.5)

    # 初始形态:NR only + HDR(源 640x360;HDR-only 会话 pipe/out=源,P10 输出)
    testenv.set_ini("dlssnr", "nr_enabled", "1")
    testenv.set_ini("dlssnr", "fg_enabled", "0")
    testenv.set_ini("rtxvideo", "vsr_mode", "0")
    testenv.set_ini("rtxvideo", "vsr_scale_x100", "150")
    testenv.set_ini("rtxvideo", "hdr_enabled", "1")

    ok = True
    mpv = None
    errf = open(testenv.MPV_STDERR, "a", encoding="utf-8")
    try:
        env = dict(os.environ, VSDLSSNR_NO_PANEL="1")
        vf = '--vf=vapoursynth="~~/vs/DLSSNR_NV.vpy"'
        mpv = subprocess.Popen([
            testenv.MPV_EXE, f"--input-ipc-server={testenv.MPV_IPC_SERVER}", "--really-quiet",
            "--volume=0", "--start=2", "--loop=inf", "--geometry=640x360",
            vf, media,
        ], cwd=testenv.ROOT, stderr=errf, stdout=subprocess.DEVNULL, env=env)
        cur_mpv[0] = mpv.pid
        time.sleep(6)

        def check(name, cond, hint=""):
            nonlocal ok
            print(f"  [{'ok' if cond else 'FAIL'}] {name}" + (f" | {hint}" if hint and not cond else ""))
            if not cond:
                ok = False

        def window(off, *preds, timeout=25):
            """等待所有谓词;返回 (全命中?, 命中窗口新行)。"""
            lines = wait_log(off, lambda ls: all(p(ls) for p in preds), timeout)
            return all(p(lines) for p in preds), lines

        # ---- 基线:同形态 seek = 老热交接(kept feature),不进形态段 ----
        off = testenv.log_size(TIMING_LOG)
        seek()
        hit, lines = window(off,
                            lambda ls: any("hot rebind kept feature" in l for l in ls))
        check("baseline: same-shape seek hot-keeps feature", hit)
        check("baseline: no shape segment", not any("hot rebind shape" in l for l in lines))

        def assert_warm(lines, tag):
            check(f"{tag}: shape segment ran",
                  any("hot rebind shape" in l for l in lines))
            check(f"{tag}: no cold fallback",
                  not any("hot rebind failed" in l or "re-initializing" in l for l in lines))

        # ---- HDR 开→关:输出回 SDR 源位深 ----
        off = testenv.log_size(TIMING_LOG)
        testenv.set_ini("rtxvideo", "hdr_enabled", "0")
        seek()
        hit, lines = window(off,
                            lambda ls: any("hot rebind shape" in l and "hdr=0" in l for l in ls),
                            lambda ls: any("output geometry" in l and "hdr=0" in l
                                           and "fmt=8bpp" in l for l in ls))
        check("hdr on->off: geometry back to SDR 8bpp", hit)
        assert_warm(lines, "hdr on->off")

        # ---- VSR off→mode2 1.5x:960x540(偶收口)----
        off = testenv.log_size(TIMING_LOG)
        testenv.set_ini("rtxvideo", "vsr_mode", "2")
        seek()
        hit, lines = window(off,
                            lambda ls: any("hot rebind shape" in l and "vsr=1" in l for l in ls),
                            lambda ls: any("output geometry" in l and "960x540" in l for l in ls))
        check("vsr mode2 1.5x: out=960x540", hit)
        assert_warm(lines, "vsr mode2")

        # ---- VSR off + FG on:会话落定(激活 ×2 或优雅降级记因)----
        # DLSSG 有最小分辨率约束(640x360 上官方链 0xBAD0000B,640x480 可
        # 用):本机/媒体组合下激活与降级都合法,共同点 = shape 段热运行 +
        # 无冷回落 + 结果有迹可查。
        off = testenv.log_size(TIMING_LOG)
        testenv.set_ini("rtxvideo", "vsr_mode", "0")
        testenv.set_ini("dlssnr", "fg_enabled", "1")
        seek()
        hit, lines = window(off,
                            lambda ls: any("hot rebind shape" in l for l in ls)
                            and any(("fg active, output fps x2" in l)
                                    or ("dlssfg official init failed" in l)
                                    or ("dlssfg nvngx_dlssg.dll missing" in l)
                                    for l in ls),
                            timeout=15)
        fg_on = any("fg active, output fps x2" in l for l in lines)
        if fg_on:
            check("fg on: session activated x2", True)
        else:
            check("fg on: degraded with reason (shape fg=0)",
                  any("hot rebind shape" in l and "fg=0" in l for l in lines))
        assert_warm(lines, "fg on")

        # ---- FG on→off:输出 1:1(FgActive 请求感知),无 fg active 新行 ----
        # FG 激活成功时本步是 shape 段(请求翻转);上一步降级已把 _fgRequested
        # 清 0 时,请求与实态一致 → 走 kept-feature。两者皆合法。
        off = testenv.log_size(TIMING_LOG)
        testenv.set_ini("dlssnr", "fg_enabled", "0")
        seek()
        hit, lines = window(off,
                            lambda ls: any(("hot rebind shape" in l and "fg=0" in l)
                                           or ("hot rebind kept feature" in l)
                                           for l in ls),
                            timeout=15)
        check("fg off: settled (shape fg=0 or hot-keep)", hit)
        check("fg off: no new fg active line (1:1 output)",
              not any("fg active" in l for l in lines))
        # 两条合法落定路径:激活成功 → shape 段;激活降级已清请求 → kept。
        check("fg off: warm path (kept or shape)",
              any(("hot rebind kept feature" in l) or ("hot rebind shape" in l)
                  for l in lines))
        check("fg off: no cold fallback",
              not any("hot rebind failed" in l or "re-initializing" in l for l in lines))

        testenv.kill_mpv()
        try:
            mpv.wait(timeout=10)
        except Exception:
            testenv.kill_mpv_tree(mpv.pid)
        mpv = None
    finally:
        testenv.kill_mpv()
        errf.close()
        for (sec, key), val in backup.items():
            testenv.set_ini(sec, key, val)

    print("SHAPE-HOTSWAP: PASS" if ok else "SHAPE-HOTSWAP: FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
