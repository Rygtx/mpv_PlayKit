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
import contextlib
import os
import shutil
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
    require(shutil.which("nvidia-smi") is not None, "未检测到 nvidia-smi(N 卡),NGX 数值断言无意义")


def ini_restore_on_exit():
    """进程退出时把 ini 恢复为调用时刻现场(2026-09-25 收敛)。

    动 ini 的测试在 import testenv 后立即调用一行即可:

        import testenv
        testenv.ini_restore_on_exit()

    测试脚本多为模块级流程(无统一 main/with 结构),atexit 恢复是侵入
    最小的保护 —— 无论 PASS/FAIL/异常退出,部署 ini 都回到调用时刻现场
    (硬崩溃跳过 atexit,属尽力而为)。返回恢复函数,需要中途显式固定
    现场的场景可留存调用。
    """
    import atexit
    backup = None
    if os.path.isfile(INI):
        with open(INI, "rb") as f:
            backup = f.read()

    def _restore():
        if backup is not None:
            with open(INI, "wb") as f:
                f.write(backup)
        elif os.path.isfile(INI):
            os.remove(INI)

    atexit.register(_restore)
    return _restore


@contextlib.contextmanager
def ini_snapshot(clear=False):
    """部署树真实 dlssnr_ui.ini 的备份/恢复上下文(2026-09-25 收敛)。

    部署根即用户 mpv-lazy 目录,ini 是用户"保存设置"的真实面板配置。
    此前约 10 个测试直接改写/删除该文件且不恢复 —— 跑一遍测试丢一次
    "保存为默认值"。所有动 ini 的测试必须经本上下文:

        with testenv.ini_snapshot():        # 现场原样备份,finally 恢复
            os.remove(testenv.INI)          # 需要空场(无 ini)的测试
            ...

    clear=True = 进入即清空文件内容(语义等价"删除"但保留文件属性;
    面板/插件的 WritePrivateProfileStringW 在空文件上照常建节)。
    备份按原始字节整文件进行,任意编码(UTF-8/UTF-16)无损恢复。
    """
    backup = None
    if os.path.isfile(INI):
        with open(INI, "rb") as f:
            backup = f.read()
    if clear:
        open(INI, "w").close()
    try:
        yield
    finally:
        if backup is not None:
            with open(INI, "wb") as f:
                f.write(backup)
        elif os.path.isfile(INI):
            # 进入时无 ini:恢复为"无 ini"(测试期间生成的清掉)。
            os.remove(INI)


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
