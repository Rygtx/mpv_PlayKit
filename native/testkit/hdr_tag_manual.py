# -*- coding: utf-8 -*-
"""hdr_tag_manual:对实机 mpv 手动打/摘 PQ 元数据标签(面板自动同步的备用手段)。

用法: <部署根>\\python.exe hdr_tag_manual.py [add|remove|status]
  add     追加 @dlssnr-hdr-tag(lavfi setparams 打 BT.2020 PQ)
  remove  摘除该标签
  status  只打印当前 video-out-params 色彩标签
"""
import json
import os
import re
import sys
import time

# 管道名默认 mpvpipe(面板 mpv.conf 解析的常用名),可用 VSDLSSNR_MPV_PIPE 覆盖。
PIPE = os.environ.get("VSDLSSNR_MPV_PIPE") or r"\\.\pipe\mpvpipe"


def ipc(cmd):
    with open(PIPE, "r+b", buffering=0) as p:
        p.write((json.dumps({"command": cmd}) + "\n").encode("utf-8"))
        time.sleep(0.5)
        try:
            return p.read(65536).decode("utf-8", "replace").strip()
        except OSError:
            return "(no reply)"


op = ipc(["get_property", "video-out-params"])
m = re.search(r'"colormatrix":"([a-z0-9.-]+)","colorlevels":"([a-z]+)",'
              r'"primaries":"([a-z0-9.-]+)","gamma":"([a-z0-9.]+)"', op)
if m:
    print("当前标签: matrix=%s levels=%s primaries=%s gamma=%s" % m.groups())
else:
    print("out-params:", op[:150])

action = sys.argv[1] if len(sys.argv) > 1 else "status"
if action == "add":
    print("add:", ipc(["vf", "add",
          "@dlssnr-hdr-tag:lavfi=[setparams=colorspace=bt2020nc:color_primaries=bt2020:color_trc=smpte2084]"])[:60])
elif action == "remove":
    print("remove:", ipc(["vf", "remove", "@dlssnr-hdr-tag"])[:60])

op = ipc(["get_property", "video-out-params"])
m = re.search(r'"colormatrix":"([a-z0-9.-]+)","colorlevels":"([a-z]+)",'
              r'"primaries":"([a-z0-9.-]+)","gamma":"([a-z0-9.]+)"', op)
if m:
    print("操作后标签: matrix=%s levels=%s primaries=%s gamma=%s" % m.groups())
