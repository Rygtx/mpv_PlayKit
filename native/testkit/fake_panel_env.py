# -*- coding: utf-8 -*-
r"""假 stats 喂面板 + 诊断页截图 —— 面板 UI 改动的快速回归工具。

不经过 mpv/插件,直接往 stats 共享内存写合成 JSON,让面板渲染出完整的
诊断页(滤镜状态/请求 vs 实际/排队细分),供人工核对或截图留档。
真插件链路的验收走 test_e2e_panel.py,本工具只管 UI 呈现。

踩坑记录(2026-10-02 固化,改本脚本前先读):
  1. stats JSON 必须紧凑( separators=(",", ":") )且 ensure_ascii=False:
     面板 JsonGetString 精确匹配 `"key":"` —— json.dumps 默认的 `": "`
     空格让所有字符串键静默失配;默认 ASCII 转义让中文 detail 显示成
     \\uXXXX。
  2. params 预置的映射句柄必须保持到面板启动之后:python 进程退出 =
     共享映射对象销毁,面板启动时 CreateFileMappingW 拿到的是全新空映射,
     adopt(magic/seq 门)静默失败,面板回落 ini/零值 —— "明明 push 了
     nr=1 面板却全关"。本脚本把句柄持有到喂循环结束,天然覆盖。
  3. 截图进程必须 SetProcessDPIAware:unaware 进程的屏幕坐标被虚拟化,
     CopyFromScreen 只能截到高 DPI 面板窗口的左上角一小块,表现为
     "内容被右/下边缘裁掉"的假象。PrintWindow 对 D3D 自绘窗口恒黑,别用。
  4. 面板是托盘优先启动(窗口默认隐藏),必须 ShowWindow + 置顶后才可截。

用法:
  python fake_panel_env.py --list                     # 列出场景
  python fake_panel_env.py                            # 默认场景 nr
  python fake_panel_env.py --scene vsr_scaling --shot diag.png
  python fake_panel_env.py --scene vsr --keep         # 不杀面板,人工核对

面板 exe 发现:VSDLSSNR_TEST_ROOT\vs-plugins\dlssnr_panel.exe,缺省回退
仓库 native\bin\dlssnr_panel.exe。脚本管理面板生命周期:已在跑的面板会
被杀掉重启(单实例,验证需要确定性的初始状态);--keep 保留运行实例。
页签记忆:面板 exe 同目录 dlssnr_ui.ini 写入 [panel] page=3(诊断页),
原文件自动备份、退出恢复。
"""
import argparse
import ctypes
import json
import os
import shutil
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import panel_ipc as ipc  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
INI_NAME = "dlssnr_ui.ini"
PANEL_CLASS = "vs_dlssnr_panel_app"

# ---------------------------------------------------------------- 场景表 ---
# 每场景 = (stats 覆盖, params 覆盖)。字段语义见 panel_ipc.py 的 SK 表与
# DEFAULTS;未覆盖字段用 STATS_BASE / DEFAULTS 兜底。

STATS_BASE = {
    "gpu_last": 2.1, "pack_last": 0.3, "eval_cpu_last": 3.4, "fg_last": 1.8,
    "rtxvsr_last": 1.2, "rtxhdr_last": 0.0, "conv_last": 1.5, "queue_last": 0.2,
    "of_last": 2.0, "unpack_last": 0.1,
    "internal_w": 0, "internal_h": 0, "width": 1920, "height": 1080,
    "scaling": 0, "fps": 60.0,
    "gpu_name": "NVIDIA GeForce RTX 4070",
    "gpu_hang": 0, "removed_reason": "",
    "filter_state": "ok", "state_detail": "",
    "of_mode": "both", "of_detail": "FFX 质量(全分辨率)",
    "fg": 0, "fg_mult": 0, "fg_route_eff": "off",
    "fg_mult_create": 0, "fg_mult_max": 5,
    "fg_detail": "",
    "rtx": "off", "rtx_detail": "",
    "slot_wait": 1.23, "lock_wait": 0.45,
    "gate_skips": 3, "gate_expired": 1, "gate_resets": 2,
}

SCENES = {
    # 面板参数全关:滤镜状态应显示"直通(画面未增强):面板全关"灰字
    "alloff": (
        {},
        {"nrEnabled": 0, "fgEnabled": 0, "vsrMode": 0, "hdrEnabled": 0},
    ),
    # 只开 NR:增强中(NGX 推理运行);光流行 每帧:算(NR)
    "nr": (
        {},
        {"nrEnabled": 1},
    ),
    # FG 开:帧生成行走 official-hook 路由显示
    "fg": (
        {"fg": 1, "fg_mult": 4, "fg_route_eff": "official-hook",
         "fg_mult_create": 4, "fg_detail": "官方 NGX(0.3.x 代理接管)"},
        {"nrEnabled": 1, "fgEnabled": 1, "fgMultiplier": 4},
    ),
    # 只开 VSR:分辨率行 1920x1080 → 3840x2160
    "vsr": (
        {"rtx": "vsr 3840x2160"},
        {"nrEnabled": 1, "vsrMode": 1},
    ),
    # NR 内部缩放 + VSR 并存:1280x720 → 1920x1080 → 3840x2160
    "vsr_scaling": (
        {"rtx": "vsr 3840x2160", "internal_w": 1280, "internal_h": 720,
         "scaling": 1},
        {"nrEnabled": 1, "scalingEnabled": 1, "vsrMode": 1},
    ),
    # RTX 请求了但实际 off:RTX 行红显(降级场景)
    "vsr_broken": (
        {"rtx": "off", "rtx_detail": "capability 不可用"},
        {"nrEnabled": 1, "vsrMode": 1},
    ),
}


def find_panel_exe():
    root = os.environ.get("VSDLSSNR_TEST_ROOT")
    if root:
        p = os.path.join(root, "vs-plugins", "dlssnr_panel.exe")
        if os.path.exists(p):
            return p
    p = os.path.join(REPO, "native", "bin", "dlssnr_panel.exe")
    if os.path.exists(p):
        return p
    raise SystemExit("panel exe not found (set VSDLSSNR_TEST_ROOT?)")


def kill_panel():
    subprocess.run(["taskkill", "/IM", "dlssnr_panel.exe", "/F"],
                   capture_output=True)
    time.sleep(1)


class IniGuard:
    """面板 exe 同目录 ini 的 page=3 预置 + 退出恢复。"""

    def __init__(self, panel_exe):
        self.ini = os.path.join(os.path.dirname(panel_exe), INI_NAME)
        self.backup = None
        if os.path.exists(self.ini):
            self.backup = self.ini + ".fakeenv.bak"
            shutil.copy2(self.ini, self.backup)
        with open(self.ini, "w", encoding="ascii") as f:
            f.write("[panel]\npage=3\n")

    def restore(self):
        if self.backup:
            shutil.move(self.backup, self.ini)
        elif os.path.exists(self.ini):
            os.remove(self.ini)


class StatsFeed:
    """持续写 stats 映射(句柄存活 = 映射存活,见文件头坑 2)。"""

    def __init__(self, stats):
        body = json.dumps(stats, separators=(",", ":"),
                          ensure_ascii=False).encode("utf-8")
        assert 8 + len(body) < ipc.PAYLOAD_SIZE, "stats body overflow"
        self._payload_body = body
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CreateFileMappingW.restype = ctypes.c_void_p
        k32.MapViewOfFile.restype = ctypes.c_void_p
        self._k32 = k32
        self._h = k32.CreateFileMappingW(
            ipc.INVALID_HANDLE_VALUE, None, ipc.PAGE_READWRITE,
            0, ipc.PAYLOAD_SIZE, ipc.STATS_MAPPING)
        assert self._h, "CreateFileMappingW(stats) failed"
        self._v = k32.MapViewOfFile(self._h, 0xF, 0, 0, ipc.PAYLOAD_SIZE)
        assert self._v, "MapViewOfFile(stats) failed"
        self._seq = 0
        self._stop = False

    def loop(self, duration):
        t0 = time.time()
        while not self._stop and time.time() - t0 < duration:
            self._seq += 1
            data = struct.pack("<II", ipc.STATS_MAGIC, self._seq) + self._payload_body
            ctypes.memmove(ctypes.c_void_p(self._v), data, len(data))
            time.sleep(0.1)

    def stop(self):
        self._stop = True


class PanelWindow:
    """定位/显示面板窗口。单实例 mutex 保证同名的只有一个,按类名匹配。"""

    def __init__(self):
        user32 = ctypes.windll.user32
        user32.SetProcessDPIAware()  # 文件头坑 3
        self.user32 = user32
        self.hwnd = None
        CB = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)

        def on_window(h, _):
            buf = ctypes.create_unicode_buffer(128)
            user32.GetClassNameW(h, buf, 128)
            if buf.value == PANEL_CLASS:
                self.hwnd = h
                return 0
            return 1

        user32.EnumWindows(CB(on_window), None)
        if not self.hwnd:
            raise SystemExit("panel window not found (is it running?)")

    def show_topmost(self):
        # 坑 4:托盘优先启动,窗口默认隐藏;置顶 + 钉到 (0,0) 再截
        self.user32.ShowWindow(self.hwnd, 5)  # SW_SHOW
        HWND_TOPMOST = ctypes.c_void_p(-1)
        NOSIZE_SHOW = 0x0041  # SWP_NOSIZE | SWP_SHOWWINDOW
        self.user32.SetWindowPos(self.hwnd, HWND_TOPMOST, 0, 0, 0, 0, NOSIZE_SHOW)

    def rect(self):
        class RECT(ctypes.Structure):
            _fields_ = [("L", ctypes.c_long), ("T", ctypes.c_long),
                        ("R", ctypes.c_long), ("B", ctypes.c_long)]
        r = RECT()
        self.user32.GetWindowRect(self.hwnd, ctypes.byref(r))
        return r.L, r.T, r.R, r.B


def capture_shot(shot_path):
    win = PanelWindow()
    win.show_topmost()
    time.sleep(1.5)  # 置顶后渲染一帧
    from PIL import ImageGrab  # noqa: 延迟导入,PIL 仅截图路径需要
    l, t, r, b = win.rect()
    ImageGrab.grab(bbox=(l, t, r, b)).save(shot_path)
    return r - l, b - t


def main():
    ap = argparse.ArgumentParser(
        description="假 stats 喂面板 + 诊断页截图(面板 UI 回归工具)")
    ap.add_argument("--scene", default="nr", choices=sorted(SCENES))
    ap.add_argument("--list", action="store_true", help="列出场景并退出")
    ap.add_argument("--shot", metavar="PNG", help="截图输出路径")
    ap.add_argument("--duration", type=float, default=12, help="喂 stats 秒数")
    ap.add_argument("--keep", action="store_true", help="退出时不杀面板")
    args = ap.parse_args()
    if args.list:
        for name in sorted(SCENES):
            print(name)
        return

    stats_ov, params_ov = SCENES[args.scene]
    panel_exe = find_panel_exe()
    print(f"panel: {panel_exe}")
    print(f"scene: {args.scene}")

    kill_panel()
    ini_guard = IniGuard(panel_exe)

    # params 预置:句柄由 params_ch 持有到 main 结束(文件头坑 2)
    params_ch = None
    if params_ov:
        params_ch = ipc.ParamsChannel()
        params_ch.push(1, generation=999, **params_ov)

    subprocess.Popen([panel_exe])
    time.sleep(3)

    stats = dict(STATS_BASE)
    stats.update(stats_ov)
    feed = StatsFeed(stats)
    feed_thread = threading.Thread(target=feed.loop, args=(args.duration,),
                                   daemon=True)
    feed_thread.start()

    time.sleep(3.5)  # 面板读取 stats + 渲染稳定

    if args.shot:
        w, h = capture_shot(args.shot)
        print(f"shot: {args.shot} ({w}x{h})")

    rest = args.duration - 6.5
    if rest > 0:
        time.sleep(rest)
    feed.stop()
    feed_thread.join(timeout=2)

    params_ch = None  # 显式释放,先于恢复 ini / 杀面板
    ini_guard.restore()
    if not args.keep:
        kill_panel()
        print("panel: killed (use --keep to leave it running)")
    else:
        print("panel: left running")


if __name__ == "__main__":
    main()
