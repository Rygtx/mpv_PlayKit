# -*- coding: utf-8 -*-
"""运行时环境发现 —— 测试脚本零硬编码路径的基础设施。

约定(详见 README.md):
  1. 首选用 mpv-lazy 自带的 python.exe 运行本目录脚本:
       <部署根>\\python.exe <仓库>\\native\\testkit\\test_xxx.py
     mpv-lazy 发行包里 python.exe 与 mpv.exe 同目录(部署根),
     VapourSynth 运行时与 vs-plugins 插件自动加载都按该根解析,
     因此 sys.executable 所在目录即部署根。
  2. 非标准布局:设环境变量 VSDLSSNR_TEST_ROOT 指向部署根。

产物路径全部由运行时事实推导(与插件源码的解析规则一致):
  - dlssnr_timing.log:插件写在宿主 exe 旁(GetModuleFileNameW(nullptr))。
    测试进程内跑滤镜时宿主 = python.exe → TIMING_LOG;
    由本脚本拉起 mpv 验证时宿主 = mpv.exe → MPV_TIMING_LOG。
  - dlssnr_ui.ini / dlssnr_panel.exe / ngx\\nvngx_dlssnr.dll:vs-plugins\\ 下。
"""
import os
import subprocess
import sys

ROOT = os.environ.get("VSDLSSNR_TEST_ROOT") or os.path.dirname(sys.executable)
HOST_DIR = os.path.dirname(sys.executable)
PLUGIN_DIR = os.path.join(ROOT, "vs-plugins")

TIMING_LOG = os.path.join(HOST_DIR, "dlssnr_timing.log")
MPV_TIMING_LOG = os.path.join(ROOT, "dlssnr_timing.log")
INI = os.path.join(PLUGIN_DIR, "dlssnr_ui.ini")
PANEL_EXE = os.path.join(PLUGIN_DIR, "dlssnr_panel.exe")
NGX_DLL = os.path.join(PLUGIN_DIR, "ngx", "nvngx_dlssnr.dll")
MPV_EXE = os.path.join(ROOT, "mpv.exe")
MPV_COM = os.path.join(ROOT, "mpv.com")

PANEL_PROCESS = "dlssnr_panel.exe"
MPV_PROCESS = "mpv.exe"


def require(cond, msg):
    """前置条件不满足时打印 SKIP 并以 0 退出(可复用脚本不能拿环境缺失当失败)。"""
    if not cond:
        print(f"SKIP: {msg}")
        sys.exit(0)


def require_env():
    """VS 插件测试的基本前提:部署根 + 插件 DLL 就位。"""
    require(os.path.isfile(os.path.join(PLUGIN_DIR, "vs_dlssnr.dll")),
            f"部署根未找到 vs-plugins\\vs_dlssnr.dll(当前解析: {ROOT})\n"
            "  用 mpv-lazy 自带 python.exe 运行,或设 VSDLSSNR_TEST_ROOT 指向部署根")


def require_nvidia():
    """NGX/DLSSNR 相关数值断言的前提:没有 N 卡时全链直通,哈希断言必然空转。"""
    import shutil
    require(shutil.which("nvidia-smi") is not None, "未检测到 nvidia-smi(N 卡),NGX 数值断言无意义")


def kill_panel():
    subprocess.run(["taskkill", "/F", "/IM", PANEL_PROCESS], capture_output=True)


def kill_mpv():
    # mpv.com 是控制台壳,只杀壳会留 mpv.exe 孤儿;两个名字都要处理。
    subprocess.run(["taskkill", "/F", "/IM", MPV_PROCESS], capture_output=True)
    subprocess.run(["taskkill", "/F", "/IM", "mpv.com"], capture_output=True)


def kill_mpv_tree(pid):
    subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)], capture_output=True)


def panel_running():
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {PANEL_PROCESS}"],
                         capture_output=True, text=True).stdout
    return out.count(PANEL_PROCESS)


def log_size(path=None):
    p = path or TIMING_LOG
    return os.path.getsize(p) if os.path.exists(p) else 0


def new_lines(off, path=None):
    """读取 timing log 自水位 off 起的新增行(timing log 跨进程可读是设计契约)。"""
    p = path or TIMING_LOG
    if not os.path.exists(p):
        return []
    with open(p, "r", encoding="utf-8", errors="ignore") as f:
        f.seek(off)
        return [l.strip() for l in f.readlines() if l.strip()]
