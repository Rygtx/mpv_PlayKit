# -*- coding: utf-8 -*-
"""面板 <-> 插件 IPC 的 python 侧镜像(与 native/src/panel_ipc.h 对齐)。

panel_ipc.h 是唯一权威;本文件是测试用的字节级镜像 —— 改头文件必须同步这里。
历史教训:各脚本手抄 struct 布局错位时,断言会"空心通过"(存活但什么都没
发生),所以布局集中在此并附 inputResolution 偏移自检。

通道:
  - 参数通道 PARAMS_MAPPING(面板写 -> 插件 40ms 轮询,seq 门控)
  - stats 通道 STATS_MAPPING(插件写 JSON -> 面板 0.1s 读,每帧发布)
  - ALIVE_EVENT:滤镜生命周期标记(面板 watchdog 据此自动退出)
"""
import ctypes
import struct

PAYLOAD_SIZE = 1024
PAYLOAD_MAGIC = 0x4E4C5344  # "DSLN" (v23, 版本位走 hex: 9 之后是 A/B/C/D/E/F)
STATS_MAGIC = 0x354C5344  # "DSL5" (stats v24)

PARAMS_MAPPING = "vs_dlssnr_panel_params"
STATS_MAPPING = "vs_dlssnr_stats"
ALIVE_EVENT = "vs_dlssnr_bridge_alive"

# Stats JSON schema 键名镜像(panel_ipc.h SK_* 单一权威的 python 侧;
# 2026-09-25 收敛 —— 此前 verify_stats_keys/test_* 手抄键名字符串,头文件
# 加键/改名时 python 侧无同步防线)。断言一律从本表取键,勿手抄字面量。
SK = {
    "gpu_last": "gpu_last",
    "pack_last": "pack_last",
    "eval_cpu_last": "eval_cpu_last",
    "fg_last": "fg_last",
    "rtxvsr_last": "rtxvsr_last",
    "rtxhdr_last": "rtxhdr_last",
    "conv_last": "conv_last",
    "queue_last": "queue_last",
    "nvof_last": "nvof_last",
    "unpack_last": "unpack_last",
    "internal_w": "internal_w",
    "internal_h": "internal_h",
    "width": "width",
    "height": "height",
    "scaling": "scaling",
    "fps": "fps",
    "gpu_name": "gpu_name",
    "gpu_hang": "gpu_hang",
    "removed_reason": "removed_reason",
    "filter_state": "filter_state",
    "state_detail": "state_detail",
    "of_mode": "of_mode",
    "fg": "fg",
    "fg_mult": "fg_mult",
    "fg_route_eff": "fg_route_eff",
    "fg_mult_create": "fg_mult_create",
    "fg_mult_max": "fg_mult_max",
    "fg_detail": "fg_detail",
    "of_detail": "of_detail",
    "rtx": "rtx",
    "rtx_detail": "rtx_detail",
    "slot_wait": "slot_wait",
    "lock_wait": "lock_wait",
    "gate_skips": "gate_skips",
    "gate_expired": "gate_expired",
    "gate_resets": "gate_resets",
}
assert len(set(SK.values())) == len(SK), "SK 键名表内有重复值"
kStateNrSeekInit = "NR on; seek to initialize"

# mirror of PanelPayload (#pragma pack push, 全 4 字节字段无对齐缝隙):
# 3I magic,seq,generation | 2i preset,style | 4f intensity,localTone,
# localStructure,skinStructure | 3i useAutoMask,uiCorrection(保留,恒 1),
# inputResolution | 5f residualMultiplier,residualSaturation,residualLightness,
# shadowStructure,reflectionGlow | 12i scalingEnabled,saveRequest,logEnabled,
# motionVectorQuality,ffxQuality,nvofFollowScaling,fgEnabled,fgMultiplier,
# fgRoute,nrEnabled,debugView,ofBackend | i f(vsrMode,vsrScale)| 7i
# vsrStrength,hdrEnabled,hdrContrast,hdrSaturation,hdrMiddleGray,
# hdrMaxLuminance,fgHdrInterp(v23 实验性补帧 HDR 域插值)
_STRUCT = struct.Struct("<3I2i4f3i5f12iif7i")
assert _STRUCT.size == 152, "PanelPayload 布局与 panel_ipc.h 不一致"

DEFAULTS = dict(
    preset=0, style=0,
    intensity=1.0, localTone=1.0, localStructure=1.0, skinStructure=-1.0,
    useAutoMask=1, uiCorrection=1, inputResolution=100,
    residualMultiplier=1.0, residualSaturation=1.0, residualLightness=1.0,
    shadowStructure=1.0, reflectionGlow=1.0,
    scalingEnabled=1, saveRequest=0, logEnabled=1,
    motionVectorQuality=0, ffxQuality=2, nvofFollowScaling=0,
    fgEnabled=0, fgMultiplier=2, fgRoute=0, nrEnabled=1,
    debugView=0, ofBackend=0,
    vsrMode=0, vsrScale=2.0, vsrStrength=2,
    hdrEnabled=0, hdrContrast=100, hdrSaturation=100,
    hdrMiddleGray=50, hdrMaxLuminance=1000,
    fgHdrInterp=0,
)

_FIELDS = ("magic", "seq", "generation", "preset", "style",
           "intensity", "localTone", "localStructure", "skinStructure",
           "useAutoMask", "uiCorrection", "inputResolution",
           "residualMultiplier", "residualSaturation", "residualLightness",
           "shadowStructure", "reflectionGlow",
           "scalingEnabled", "saveRequest", "logEnabled",
           "motionVectorQuality", "ffxQuality", "nvofFollowScaling",
           "fgEnabled", "fgMultiplier", "fgRoute", "nrEnabled", "debugView",
           "ofBackend",
           "vsrMode", "vsrScale", "vsrStrength", "hdrEnabled", "hdrContrast",
           "hdrSaturation", "hdrMiddleGray", "hdrMaxLuminance", "fgHdrInterp")

PAGE_READWRITE = 0x04
FILE_MAP_READ = 0x0004
FILE_MAP_WRITE = 0x0002
FILE_MAP_ALL = 0xF
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1)


class ParamsChannel:
    """面板角色:创建/打开参数映射并下发 payload(模拟面板推参)。"""

    def __init__(self):
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CreateFileMappingW.restype = ctypes.c_void_p
        k32.MapViewOfFile.restype = ctypes.c_void_p
        self._k32 = k32
        self._handle = k32.CreateFileMappingW(
            INVALID_HANDLE_VALUE, None, PAGE_READWRITE, 0, PAYLOAD_SIZE, PARAMS_MAPPING)
        assert self._handle, "CreateFileMappingW failed"
        self._view = k32.MapViewOfFile(self._handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, PAYLOAD_SIZE)
        assert self._view, "MapViewOfFile failed"

    def push(self, seq, generation=1234, **kw):
        vals = dict(DEFAULTS)
        vals.update(kw)
        data = _STRUCT.pack(
            PAYLOAD_MAGIC, seq, generation,
            vals["preset"], vals["style"],
            vals["intensity"], vals["localTone"], vals["localStructure"], vals["skinStructure"],
            vals["useAutoMask"], vals["uiCorrection"], vals["inputResolution"],
            vals["residualMultiplier"], vals["residualSaturation"],
            vals["residualLightness"], vals["shadowStructure"], vals["reflectionGlow"],
            vals["scalingEnabled"], vals["saveRequest"], vals["logEnabled"],
            vals["motionVectorQuality"], vals["ffxQuality"], vals["nvofFollowScaling"],
            vals["fgEnabled"], vals["fgMultiplier"], vals["fgRoute"],
            vals["nrEnabled"], vals["debugView"], vals["ofBackend"],
            vals["vsrMode"], vals["vsrScale"], vals["vsrStrength"],
            vals["hdrEnabled"], vals["hdrContrast"], vals["hdrSaturation"],
            vals["hdrMiddleGray"], vals["hdrMaxLuminance"], vals["fgHdrInterp"])
        assert len(data) == _STRUCT.size, "PanelPayload pack 布局与 panel_ipc.h 不一致"
        ctypes.memmove(ctypes.c_void_p(self._view), data, len(data))

    def read(self):
        buf = ctypes.string_at(ctypes.c_void_p(self._view), PAYLOAD_SIZE)
        magic, seq = struct.unpack_from("<II", buf, 0)
        if magic != PAYLOAD_MAGIC:
            return None
        vals = dict(zip(_FIELDS, _STRUCT.unpack(buf[:_STRUCT.size])))
        return vals


def open_params_mapping_readonly():
    """插件已建立通道时的只读访问(验证真实面板写入了 payload)。"""
    k32 = ctypes.WinDLL("kernel32")
    k32.OpenFileMappingW.restype = ctypes.c_void_p
    k32.MapViewOfFile.restype = ctypes.c_void_p
    h = k32.OpenFileMappingW(FILE_MAP_ALL, False, PARAMS_MAPPING)
    if not h:
        return None, None
    v = k32.MapViewOfFile(h, FILE_MAP_ALL, 0, 0, PAYLOAD_SIZE)
    return h, v


def read_stats(require_magic=True):
    """读 stats 通道 JSON(插件 -> 面板),无映射时返回 None。

    返回 (magic, seq, body)。require_magic=True(默认)时 magic 失配即
    返回 None —— 面板/插件版本位不配对(成对部署被破坏)不该被当成
    "stats 缺键"消音;旧行为(不校验)用 require_magic=False 取得。
    2026-09-25 修:kernel32 函数补 restype/argtypes(此前 64 位下句柄
    截断侥幸可用),magic 校验默认开启。
    """
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenFileMappingW.restype = ctypes.c_void_p
    k32.OpenFileMappingW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]
    k32.MapViewOfFile.restype = ctypes.c_void_p
    k32.MapViewOfFile.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                  ctypes.c_uint32, ctypes.c_uint32, ctypes.c_size_t]
    k32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
    k32.CloseHandle.argtypes = [ctypes.c_void_p]
    h = k32.OpenFileMappingW(FILE_MAP_READ, False, STATS_MAPPING)
    if not h:
        return None
    p = k32.MapViewOfFile(h, FILE_MAP_READ, 0, 0, PAYLOAD_SIZE)
    if not p:
        k32.CloseHandle(ctypes.c_void_p(h))
        return None
    raw = ctypes.string_at(p, PAYLOAD_SIZE)
    k32.UnmapViewOfFile(ctypes.c_void_p(p))
    k32.CloseHandle(ctypes.c_void_p(h))
    magic, seq = struct.unpack_from("<II", raw, 0)
    if require_magic and magic != STATS_MAGIC:
        return None
    body = raw[8:].split(b"\0")[0].decode("utf-8", "replace")
    return magic, seq, body
