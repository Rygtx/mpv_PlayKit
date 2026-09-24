# -*- coding: utf-8 -*-
"""testmedia:testkit 合成测试媒体统一生成器 + 集中存放(media\\)。

testkit 全部测试用到的合成媒体(y4m/PNG)由此模块生成,统一落在
native/testkit/media/(gitignore,可整目录删掉重生成):

    python testmedia.py              # 生成缺失的媒体到 ..\\testkit\\media\\
    python testmedia.py <目录>       # 生成到指定目录
    python testmedia.py --force      # 已存在也重新生成
    python testmedia.py --list      # 只列清单与体积

为什么生成而非入库:y4m 无压缩,单片 11–124MB,二进制不入库;生成器是
确定性纯函数(固定图案、无随机),重生成逐位一致,断言稳定。测试代码
不要自写生成器 —— 从 CLIPS 取定义、用 ensure() 取路径。
"""
import os
import struct
import sys
import zlib

MEDIA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "media")

W, H, FPS, SECS = 320, 180, 24, 8  # 格式收编系列片(320x180 短片,创 `create` 行可断言即可)


def _row8(w, off):
    return bytes(((x * 2 + off) & 0xFF) for x in range(w))


def _row10(w, off):
    return b"".join(struct.pack(">H", ((x * 4 + off) & 0x3FF)) for x in range(w))


def gen_y4m(path, tag, pack, w=W, h=H, fps=FPS, secs=SECS):
    with open(path, "wb") as f:
        f.write(f"YUV4MPEG2 W{w} H{h} F{fps}:1 Ip A1:1 {tag}\n".encode())
        for n in range(fps * secs):
            f.write(b"FRAME\n")
            f.write(pack(n))


def _pack444p8(n):
    return (_row8(W, n * 5) * H) + (_row8(W, 96 + n * 3) * H) + (_row8(W, 160 + n * 3) * H)


def _pack444p10(n):
    return (b"".join(struct.pack(">H", ((x * 4 + n * 5) & 0x3FF)) for x in range(W)) * H) + \
           (b"".join(struct.pack(">H", ((x * 4 + 384 + n * 3) & 0x3FF)) for x in range(W)) * H) + \
           (b"".join(struct.pack(">H", ((x * 4 + 640 + n * 3) & 0x3FF)) for x in range(W)) * H)


def _pack_mono8(n):
    return _row8(W, n * 5) * H


def gen_png_rgb24(path):
    """单帧 PNG(mpv 按 image2 解码,--loop=inf 连续供帧)。"""
    pw = ph = 96
    raw = bytearray()
    for y in range(ph):
        raw += b"\x00"  # filter: none
        for x in range(pw):
            raw += bytes((((x * 2) & 0xFF), ((y * 2) & 0xFF), ((x + y) & 0xFF)))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", pw, ph, 8, 2, 0, 0, 0)  # 8bit truecolor
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
                + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def gen_y4m_420jpeg_moving(path, w=640, h=360, fps=24, secs=15):
    """移动竖条纹(全亮范围扫)+ 静态中灰色度;纯 bytes 旋转,无 numpy。"""
    pat = bytes(16 + (x % 64) * 219 // 63 for x in range(64))
    row0 = (pat * (w // 64 + 2))[:w]
    csize = (w // 2) * (h // 2)
    chroma = b"\x80" * csize
    with open(path, "wb") as f:
        f.write(f"YUV4MPEG2 W{w} H{h} F{fps}:1 Ip A1:1 C420jpeg\n".encode())
        for n in range(fps * secs):
            off = (n * 3) % w
            row = row0[w - off:] + row0[:w - off]
            f.write(b"FRAME\n" + row * h + chroma + chroma)


# (文件名, 生成函数 path->None, 断言宽, 断言高, create 位深, 用途说明)
CLIPS = [
    ("dlssnr_fmt_444.y4m", lambda p: gen_y4m(p, "C444", _pack444p8),
     W, H, 8, "YUV444P8 直吃"),
    ("dlssnr_fmt_444p10.y4m", lambda p: gen_y4m(p, "C444p10", _pack444p10),
     W, H, 10, "YUV444P10 直吃"),
    ("dlssnr_fmt_mono.y4m", lambda p: gen_y4m(p, "Cmono", _pack_mono8),
     W, H, 8, "GRAY8 -> vpy 转 444P8"),
    ("dlssnr_fmt_rgb.png", gen_png_rgb24,
     96, 96, 8, "RGB24 -> vpy 709 矩阵转 444P8"),
    ("hdr_hint_sync_test.y4m", gen_y4m_420jpeg_moving,
     640, 360, 8, "HDR hint/打标与全关态等通用 420P8 载体"),
]


def ensure(dir=MEDIA_DIR, names=None, force=False):
    """生成缺失媒体(已存在且非 force 则跳过),返回 {文件名: 路径}。"""
    os.makedirs(dir, exist_ok=True)
    out = {}
    for fname, gen, _w, _h, _d, _note in CLIPS:
        if names and fname not in names:
            continue
        path = os.path.join(dir, fname)
        if force or not os.path.isfile(path) or os.path.getsize(path) == 0:
            print(f"合成媒体: {path}")
            gen(path)
        assert os.path.isfile(path) and os.path.getsize(path) > 0, fname
        out[fname] = path
    return out


def main():
    argv = [a for a in sys.argv[1:]]
    force = "--force" in argv
    if "--list" in argv:
        for fname, _gen, w, h, depth, note in CLIPS:
            path = os.path.join(MEDIA_DIR, fname)
            size = os.path.getsize(path) if os.path.isfile(path) else 0
            print(f"{fname:32s} {w}x{h} d{depth}  {size / 1e6:7.1f} MB  {note}")
        return 0
    argv = [a for a in argv if not a.startswith("--")]
    where = argv[0] if argv else MEDIA_DIR
    ensure(dir=where, force=force)
    total = sum(os.path.getsize(os.path.join(where, f[0])) for f in CLIPS)
    print(f"完成:{len(CLIPS)} 个媒体 @ {where}(合计 {total / 1e6:.1f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
