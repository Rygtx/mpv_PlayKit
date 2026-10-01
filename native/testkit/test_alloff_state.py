# -*- coding: utf-8 -*-
"""test_alloff_state:全关直通实例的 stats 状态回归。

2026-09-24 nrPubState{-1} 首帧边沿曾让全关直通实例(initOk=false)在首帧
误发 "NR off (panel)"(该串本意只允许已初始化会话的 live 关发布),面板 NR
开关按前缀判定 "live 可恢复" 跳过重建 → 全关实例上开 NR 永远无效(4K 片源
日志零条 nr=1 fg=0 create;FG 开关捎带重建才生效)。该边沿体已随管线解耦
整体删除(边沿发布只剩 kStateNrSeekInit 一种)。
断言:全关实例的 state_detail 恒为 "NR+FG+RTX disabled (panel/vpy)",
"NR off (panel)" 永久不得再现。

用法: <部署根>\\python.exe <仓库>\\native\\testkit\\test_alloff_state.py
"""
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import panel_ipc  # noqa: E402
import testenv  # noqa: E402
import testmedia  # noqa: E402


def main():
    testenv.require_env()
    media = testmedia.ensure(names=["hdr_hint_sync_test.y4m"])["hdr_hint_sync_test.y4m"]

    # 现场保护:全关状态由 ini 裁决(无面板 payload);退出时整文件恢复。
    testenv.ini_restore_on_exit()

    testenv.kill_panel()
    testenv.kill_mpv()
    time.sleep(1.5)
    testenv.set_ini("dlssnr", "nr_enabled", "0")
    testenv.set_ini("dlssnr", "fg_enabled", "0")
    testenv.set_rtxvideo_ini(vsr_mode="0", hdr_enabled="0")

    ok = True
    try:
        vf = '--vf=vapoursynth="~~/vs/DLSSNR_NV.vpy"'
        errf = open(testenv.MPV_STDERR, "a", encoding="utf-8")
        mpv = subprocess.Popen([
            testenv.MPV_EXE, f"--input-ipc-server={testenv.MPV_IPC_SERVER}",
            "--really-quiet", "--volume=0", "--start=2", "--loop=inf",
            "--geometry=640x360", vf, media,
        ], cwd=testenv.ROOT, stderr=errf, stdout=subprocess.DEVNULL)
        time.sleep(6)

        st = None
        for _ in range(10):
            st = panel_ipc.read_stats_json()
            if st and st.get("filter_state"):
                break
            time.sleep(0.5)
        detail = (st or {}).get(panel_ipc.SK["state_detail"], "")
        fstate = (st or {}).get(panel_ipc.SK["filter_state"], "")
        print(f"[全关] filter_state={fstate!r} state_detail={detail!r}")
        if fstate != "passthrough" or "NR+FG+RTX disabled" not in detail:
            print("FAIL: 全关实例状态不是 NR+FG+RTX disabled(状态未达或语义漂移)")
            ok = False
        if "NR off" in detail:
            print("FAIL: 全关实例误发 NR off (panel) —— nrPubState 门控未生效")
            ok = False

        testenv.kill_mpv()
        try:
            mpv.wait(timeout=10)
        except Exception:
            testenv.kill_mpv_tree(mpv.pid)
        errf.close()
    finally:
        testenv.kill_mpv()

    print("PASS: 全关实例状态正确(NR off 误标已消除)" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
