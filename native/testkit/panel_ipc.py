# -*- coding: utf-8 -*-
"""面板 <-> 插件 IPC 的 python 侧镜像(与 native/src/panel_ipc.h 对齐)。

panel_ipc.h 是唯一权威;本文件是测试用的字节级镜像 —— 改头文件必须同步这里。
历史教训:各脚本手抄 struct 布局错位时,断言会"空心通过"(存活但什么都没
发生),所以布局集中在此并附 inputResolution 偏移自检。

通道:
  - 参数通道 PARAMS_MAPPING(面板写 -> 插件 40ms 轮询,seq 门控)
  - stats 通道 STATS_MAPPING(插件写定长 struct -> 面板 0.1s 读,每帧发布;
    v27 起 body 为二进制结构体,原 JSON 文本退役)
  - ALIVE_EVENT:滤镜生命周期标记(面板 watchdog 据此自动退出)
"""
import ctypes
import re
import struct

PAYLOAD_SIZE = 2048
PAYLOAD_MAGIC = 0x514C5344  # "DSLQ" (v25: saveRequest/uiCorrection 删除;v29: v22-24 键全量;
                            #  v30: src_hdr —— 源传输函数探针,插件 HDR 源压制消费)
STATS_MAGIC = 0x424C5344  # "DSLB" (v29: gateSkips 删除 + rtx 实效位/数值尺寸;
                          #  v30: src_hdr 位 —— HDR 源旁路判定)

PARAMS_MAPPING = "vs_dlssnr_panel_params"
STATS_MAPPING = "vs_dlssnr_stats"
ALIVE_EVENT = "vs_dlssnr_bridge_alive"

# Stats 字段名镜像(panel_ipc.h StatsPayload 字段单一权威的 python 侧;
# 历史上是 JSON 键名,断言脚本从本表取键,勿手抄字面量)。
SK = {
    "gpu_last": "gpu_last",
    "pack_last": "pack_last",
    "eval_cpu_last": "eval_cpu_last",
    "fg_last": "fg_last",
    "rtxvsr_last": "rtxvsr_last",
    "rtxhdr_last": "rtxhdr_last",
    "conv_last": "conv_last",
    "queue_last": "queue_last",
    "of_last": "of_last",
    "unpack_last": "unpack_last",
    "internal_w": "internal_w",
    "internal_h": "internal_h",
    "width": "width",
    "height": "height",
    "scaling": "scaling",
    "fps": "fps",
    "gpu_name": "gpu_name",
    "model_dll": "model_dll",
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
    "gate_expired": "gate_expired",
    "gate_resets": "gate_resets",
    "eval_active": "eval_active",
    "of_active": "of_active",
    "scaling_active": "scaling_active",
    "rtx_vsr_active": "rtx_vsr_active",
    "rtx_hdr_active": "rtx_hdr_active",
    "rtx_out_w": "rtx_out_w",
    "rtx_out_h": "rtx_out_h",
    "src_hdr": "src_hdr",
    "temporal": "temporal",
    "temporal_route": "temporal_route",
    "temporal_w": "temporal_w",
}
assert len(set(SK.values())) == len(SK), "SK 键名表内有重复值"

# mirror of StatsPayload (panel_ipc.h #pragma pack push,8;v27 起定长 struct,
# 旧 JSON 键名保留为 dict 键)。布局:2I magic,seq | 14f 计时/权重 |
# 20I 尺寸/倍数/门累计/实效位(DSL9 三位 + DSLA rtx 四项 + DSLB src_hdr;
# gate_skips 已删)| 13 段定长字符串(v28 起含 model_dll)。错位 = 读出
# 乱码,断言即炸 —— 尺寸断言是同步防线(与 _STRUCT 同一教训)。
# 1098 = python pack 尺寸;C++ sizeof(pack(8),尾部对齐到 4)= 1100。
_STATS_STRUCT = struct.Struct("<II14f20I160s64s200s16s40s16s16s128s96s96s96s10s16s")
assert _STATS_STRUCT.size == 1098, "StatsPayload 布局与 panel_ipc.h 不一致(C++ sizeof=1100)"
_STATS_FIELDS = (
    "magic", "seq",
    "gpu_last", "pack_last", "eval_cpu_last", "unpack_last",
    "of_last", "fg_last", "rtxvsr_last", "rtxhdr_last",
    "conv_last", "queue_last", "slot_wait", "lock_wait", "fps", "temporal_w",
    "internal_w", "internal_h", "width", "height", "scaling",
    "fg_mult", "fg_mult_create", "fg_mult_max",
    "gate_expired", "gate_resets", "gpu_hang", "temporal_route",
    "eval_active", "of_active", "scaling_active",
    "rtx_vsr_active", "rtx_hdr_active", "rtx_out_w", "rtx_out_h", "src_hdr",
    "gpu_name", "model_dll", "state_detail", "filter_state", "of_mode", "fg",
    "fg_route_eff", "fg_detail", "of_detail", "rtx", "rtx_detail", "temporal",
    "removed_reason",
)
assert len(_STATS_FIELDS) == 2 + 14 + 20 + 13 == len(set(_STATS_FIELDS))
_STATS_STR_START = 2 + 14 + 20  # 首个字符串字段在字段元组中的下标
_STATS_STR_WIDTHS = [int(w) for w in re.findall(r"(\d+)s", _STATS_STRUCT.format)]
assert len(_STATS_STR_WIDTHS) == 13
assert sum(_STATS_STR_WIDTHS) == _STATS_STRUCT.size - 8 - 14 * 4 - 20 * 4


def pack_stats(stats, seq=1):
    """stats dict(旧 JSON 键名)→ StatsPayload 定长字节。

    数字缺省 0;字符串缺省空串,超长按格宽截断(UTF-8 字节截断,可能切在
    多字节中间 —— 测试数据自守,不追求字符边界)。fake_panel_env(合成
    stats 喂面板)与断言脚本共用,布局只此一处。
    """
    vals = []
    for i, name in enumerate(_STATS_FIELDS):
        if name == "magic":
            vals.append(STATS_MAGIC)
        elif name == "seq":
            vals.append(seq)
        elif i >= _STATS_STR_START:
            w = _STATS_STR_WIDTHS[i - _STATS_STR_START]
            vals.append(str(stats.get(name, "")).encode("utf-8")[: w - 1])
        else:
            vals.append(stats.get(name, 0))
    return _STATS_STRUCT.pack(*vals)

# mirror of PanelPayload (#pragma pack push, 全 4 字节字段无对齐缝隙):
# 3I magic,seq,generation | 2i preset,style | 4f intensity,localTone,
# localStructure,skinStructure | 2i useAutoMask,inputResolution | 5f
# residualMultiplier,residualSaturation,residualLightness,shadowStructure,
# reflectionGlow | 11i scalingEnabled,logEnabled,motionVectorQuality,
# ffxQuality,nvofFollowScaling,fgEnabled,fgMultiplier,fgRoute,nrEnabled,
# debugView,ofBackend | i f(vsrMode,vsrScale)| 8i vsrStrength,hdrEnabled,
# hdrContrast,hdrSaturation,hdrMiddleGray,hdrMaxLuminance,fgHdrInterp,
# antiFlicker | 1i srcHdr(v30)
# (v24 增 antiFlicker;v25 删 uiCorrection/saveRequest;v30 增 src_hdr。)
_STRUCT = struct.Struct("<3I2i4f2i5f11iif9i")
assert _STRUCT.size == 152, "PanelPayload 布局与 panel_ipc.h 不一致"

DEFAULTS = dict(
    preset=0, style=0,
    intensity=1.0, localTone=1.0, localStructure=1.0, skinStructure=1.0,
    useAutoMask=1, inputResolution=100,
    residualMultiplier=1.0, residualSaturation=1.0, residualLightness=1.0,
    shadowStructure=1.0, reflectionGlow=1.0,
    scalingEnabled=1, logEnabled=1,
    motionVectorQuality=0, ffxQuality=2, nvofFollowScaling=0,
    fgEnabled=0, fgMultiplier=2, fgRoute=0, nrEnabled=1,
    debugView=0, ofBackend=0,
    vsrMode=0, vsrScale=2.0, vsrStrength=2,
    hdrEnabled=0, hdrContrast=100, hdrSaturation=100,
    hdrMiddleGray=50, hdrMaxLuminance=1000,
    fgHdrInterp=0, antiFlicker=0,
    src_hdr=0,
)

_FIELDS = ("magic", "seq", "generation", "preset", "style",
           "intensity", "localTone", "localStructure", "skinStructure",
           "useAutoMask", "inputResolution",
           "residualMultiplier", "residualSaturation", "residualLightness",
           "shadowStructure", "reflectionGlow",
           "scalingEnabled", "logEnabled",
           "motionVectorQuality", "ffxQuality", "nvofFollowScaling",
           "fgEnabled", "fgMultiplier", "fgRoute", "nrEnabled", "debugView",
           "ofBackend",
           "vsrMode", "vsrScale", "vsrStrength", "hdrEnabled", "hdrContrast",
           "hdrSaturation", "hdrMiddleGray", "hdrMaxLuminance", "fgHdrInterp",
           "antiFlicker", "src_hdr")

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
            vals["useAutoMask"], vals["inputResolution"],
            vals["residualMultiplier"], vals["residualSaturation"],
            vals["residualLightness"], vals["shadowStructure"], vals["reflectionGlow"],
            vals["scalingEnabled"], vals["logEnabled"],
            vals["motionVectorQuality"], vals["ffxQuality"], vals["nvofFollowScaling"],
            vals["fgEnabled"], vals["fgMultiplier"], vals["fgRoute"],
            vals["nrEnabled"], vals["debugView"], vals["ofBackend"],
            vals["vsrMode"], vals["vsrScale"], vals["vsrStrength"],
            vals["hdrEnabled"], vals["hdrContrast"], vals["hdrSaturation"],
            vals["hdrMiddleGray"], vals["hdrMaxLuminance"], vals["fgHdrInterp"],
            vals["antiFlicker"], vals["src_hdr"])
        assert len(data) == _STRUCT.size, "PanelPayload pack 布局与 panel_ipc.h 不一致"
        ctypes.memmove(ctypes.c_void_p(self._view), data, len(data))


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
    """读 stats 通道(插件 -> 面板),无映射时返回 None。

    返回 (magic, seq, raw)。raw = StatsPayload 的原始字节(定长 struct,
    v27 起不再是 JSON 文本;解码走 read_stats_json)。require_magic=True
    (默认)时 magic 失配即返回 None —— 面板/插件版本位不配对(成对部署
    被破坏)不该被当成"stats 缺键"消音;旧行为(不校验)用
    require_magic=False 取得。
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
    return magic, seq, raw


def read_stats_json():
    """read_stats 的 dict 封装(无映射/魔数不符/未发布 → None)。

    断言脚本用本函数拿 stats 字典(键 = 旧 JSON 时代的 SK 名,延续不断
    旧脚本);需要 magic/seq 原始三元组时仍走 read_stats。
    """
    st = read_stats()
    if not st:
        return None
    raw = st[2][: _STATS_STRUCT.size]
    vals = _STATS_STRUCT.unpack(raw)
    d = dict(zip(_STATS_FIELDS, vals))
    for name in _STATS_FIELDS[_STATS_STR_START:]:
        d[name] = d[name].split(b"\0")[0].decode("utf-8", "replace")
    return d
