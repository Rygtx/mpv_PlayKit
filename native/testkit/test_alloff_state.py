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
import ctypes
import json
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv

STATS_MAPPING = "vs_dlssnr_stats"
STATS_MAGIC = 0x354C5344  # "DSL5" (v24) — panel_ipc.h STATS_MAGIC
PAYLOAD_SIZE = 1024


def read_stats():
    """与面板 LoadStats 同款:只读打开 stats 映射,seq==0 = 写入进行中。"""
    k32 = ctypes.windll.kernel32
    # 64 位指针必须显式 restype/argtypes,否则句柄被截断成 32 位 → AV
    k32.OpenFileMappingW.restype = ctypes.c_void_p
    k32.MapViewOfFile.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
                                  ctypes.c_uint32, ctypes.c_size_t]
    k32.MapViewOfFile.restype = ctypes.c_void_p
    k32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
    k32.CloseHandle.argtypes = [ctypes.c_void_p]
    h = k32.OpenFileMappingW(0x0004, False, STATS_MAPPING)  # FILE_MAP_READ
    if not h:
        return None
    try:
        view = k32.MapViewOfFile(h, 0x0004, 0, 0, PAYLOAD_SIZE)
        if not view:
            return None
        try:
            blob = ctypes.string_at(view, PAYLOAD_SIZE)
            magic, seq = struct.unpack_from("<II", blob, 0)
            if magic != STATS_MAGIC or seq == 0:
                return None
            body = blob[8:].split(b"\x00", 1)[0].decode("utf-8", "replace")
            return json.loads(body)
        finally:
            k32.UnmapViewOfFile(view)
    finally:
        k32.CloseHandle(h)


def main():
    k32 = ctypes.windll.kernel32
    media = os.path.join(os.environ.get("TEMP", "."), "hdr_hint_sync_test.y4m")
    if not os.path.isfile(media):
        print(f"SKIP: 找不到合成测试媒体 {media}(先跑 test_hdr_hint_sync.py 生成)")
        return 0
    testenv.require_env()

    # 备份/恢复 ini(全关状态由 ini 裁决:无面板 payload)
    touched = [("dlssnr", "nr_enabled"), ("dlssnr", "fg_enabled"),
               ("rtxvideo", "vsr_mode"), ("rtxvideo", "hdr_enabled")]
    backup = {}
    for sec, key in touched:
        buf = ctypes.create_unicode_buffer(64)
        k32.GetPrivateProfileStringW(sec, key, None, buf, 64, testenv.INI)
        backup[(sec, key)] = buf.value

    def set_ini(sec, key, val):
        k32.WritePrivateProfileStringW(sec, key, val, testenv.INI)

    testenv.kill_panel()
    testenv.kill_mpv()
    time.sleep(1.5)
    set_ini("dlssnr", "nr_enabled", "0")
    set_ini("dlssnr", "fg_enabled", "0")
    set_ini("rtxvideo", "vsr_mode", "0")
    set_ini("rtxvideo", "hdr_enabled", "0")

    ok = True
    try:
        vf = '--vf=vapoursynth="~~/vs/DLSSNR_NV.vpy"'
        errf = open(os.path.join(testenv.ROOT, "mpv_stderr.txt"), "a", encoding="utf-8")
        mpv = subprocess.Popen([
            testenv.MPV_EXE, "--input-ipc-server=mpvpipe", "--really-quiet",
            "--volume=0", "--start=2", "--loop=inf", "--geometry=640x360",
            vf, media,
        ], cwd=testenv.ROOT, stderr=errf, stdout=subprocess.DEVNULL)
        time.sleep(6)

        st = None
        for _ in range(10):
            st = read_stats()
            if st and st.get("filter_state"):
                break
            time.sleep(0.5)
        detail = (st or {}).get("state_detail", "")
        fstate = (st or {}).get("filter_state", "")
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
        for (sec, key), val in backup.items():
            set_ini(sec, key, val)

    print("PASS: 全关实例状态正确(NR off 误标已消除)" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
