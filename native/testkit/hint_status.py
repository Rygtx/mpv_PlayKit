# -*- coding: utf-8 -*-
"""hint_status:一行命令看当前 mpv 的 HDR/色彩空间实态(四层中的 1-2 层)。

用法: <部署根>\\python.exe testkit\\hint_status.py   (mpv 播放中运行)
读数: target-colorspace-hint(面板设的意图)、video-out-params(滤镜输出解读)、
      video-target-params(mpv 实际渲染进什么 —— 显示端被 hint 后 gamma/max-luma
      会变)、vf 链(打标在不在)。
"""
import json
import sys
import time

PIPE = r"\\.\pipe\mpvpipe"


def main():
    lines = []
    for attempt in range(3):
        try:
            with open(PIPE, "r+b", buffering=0) as p:
                for cmd in (["get_property", "target-colorspace-hint"],
                            ["get_property", "video-out-params"],
                            ["get_property", "video-target-params"],
                            ["get_property", "vf"],
                            ["get_property", "filename"]):
                    p.write((json.dumps({"command": cmd}) + "\n").encode())
                    time.sleep(0.25)
                    lines.append(p.read(65536).decode("utf-8", "replace"))
            break
        except FileNotFoundError:
            if attempt == 2:
                print("mpv 未在跑或 IPC 管道不存在(mpv.conf: input-ipc-server)")
                return 1
            time.sleep(1)
    hint, out, tgt, vf, name = lines
    print(f"file  : {name.strip()[:120]}")
    print(f"hint  : {hint.strip()}")
    print(f"vf-tag: {'@dlssnr-hdr-tag 在链' if 'dlssnr-hdr-tag' in vf else '无打标(插件非 HDR 实态?)'}")
    for label, blob in (("out-params(滤镜输出)", out), ("target-params(渲染目标)", tgt)):
        keep = [k for k in ("colormatrix", "primaries", "gamma", "max-luma", "min-luma",
                            "sig-peak", "light", "colorspace") if f'"{k}":"' in blob]
        vals = {k: blob.split(f'"{k}":"')[1].split('"')[0] for k in keep}
        print(f"{label}: {vals}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
