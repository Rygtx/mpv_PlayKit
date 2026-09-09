# -*- coding: utf-8 -*-
"""生成 dlssnr_panel 托盘/应用图标(多尺寸 .ico)。

设计:DLSSNR 是 AI 画质增强(NVIDIA NGX),面板主色是深色底 + 蓝
(gpu 段 IM_COL32(30,136,229))。图标 = 圆角深色底 + 中心四角星
(sparkle,AI 增强的通用符号)+ 两条对角光轨(残差增强),蓝青渐变。
托盘最小 16px 也要可读:星形主体占画布 ~60%,粗笔画,无细碎细节。

输出: native/src/panel.ico (16/20/24/32/48/64/128/256)
用法: python tools/make_panel_icon.py  (系统 python,需 Pillow)
"""
from PIL import Image, ImageDraw

# 面板配色:gpu 段蓝 (30,136,229) + eval 段靛蓝 (63,81,181),提亮做渐变
C_BG_TOP = (32, 36, 46, 255)      # 深色底(与 ImGui 深色风格一致)
C_BG_BOT = (18, 20, 28, 255)
C_BLUE = (56, 152, 244, 255)      # 主蓝(提亮自 30,136,229)
C_CYAN = (86, 220, 255, 255)      # 高光青
C_INDIGO = (88, 108, 246, 255)    # 靛蓝(渐变另一端)

SIZES = [16, 20, 24, 32, 48, 64, 128, 256]


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(4))


def draw_icon(size):
    s = size
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # --- 圆角方形底(Windows 托盘惯例:方形带圆角,非圆形)---
    radius = max(2, s * 22 // 100)
    # 垂直渐变底:逐行画圆角矩形裁剪
    grad = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    gd = ImageDraw.Draw(grad)
    for y in range(s):
        gd.line([(0, y), (s, y)], fill=lerp(C_BG_TOP, C_BG_BOT, y / max(1, s - 1)))
    mask = Image.new("L", (s, s), 0)
    md = ImageDraw.Draw(mask)
    md.rounded_rectangle([0, 0, s - 1, s - 1], radius=radius, fill=255)
    img.paste(grad, (0, 0), mask)
    # 细描边(1px 高光,深底上勾出轮廓)
    edge = lerp(C_BLUE, C_BG_BOT, 0.55)
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=radius, outline=edge, width=1)

    # --- 中心四角星(sparkle):两支长对角 + 两支短对角,凹边菱形 ---
    cx = cy = s / 2.0
    R = s * 0.34          # 长臂半径
    r = s * 0.13          # 短臂半径
    waist = s * 0.055     # 臂根收腰(星形凹边)
    # 45° 旋转坐标系:长臂指向四角,短臂指向上下左右
    import math
    pts = []
    for i in range(4):
        a_long = math.radians(45 + 90 * i)
        a_short = math.radians(90 * i)
        # 长臂尖 -> 收腰点 -> 短臂尖 -> 收腰点
        pts.append((cx + R * math.cos(a_long), cy + R * math.sin(a_long)))
        w1 = (cx + waist * math.cos(a_long + math.radians(45)),
              cy + waist * math.sin(a_long + math.radians(45)))
        pts.append(w1)
        pts.append((cx + r * math.cos(a_short), cy + r * math.sin(a_short)))
        w2 = (cx + waist * math.cos(a_long - math.radians(45)),
              cy + waist * math.sin(a_long - math.radians(45)))
        pts.append(w2)
    # 星形渐变:左上青 -> 右下蓝
    star = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    sd = ImageDraw.Draw(star)
    sd.polygon(pts, fill=C_BLUE)
    # 渐变裁剪:对角线性渐变蒙版
    grad2 = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    g2 = ImageDraw.Draw(grad2)
    for y in range(s):
        for step in range(1):
            pass
    # 简化:用小图渐变 + paste with star mask
    gmask = Image.new("L", (s, s), 0)
    gm = ImageDraw.Draw(gmask)
    gm.polygon(pts, fill=255)
    grad2 = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    g2 = ImageDraw.Draw(grad2)
    for y in range(s):
        for x in range(0, s, max(1, s // 64)):
            t = (x + y) / (2.0 * s)
            g2.rectangle([x, y, min(x + max(1, s // 64), s), y + 1],
                         fill=lerp(C_CYAN, C_INDIGO, t))
    img.paste(grad2, (0, 0), gmask)

    # --- 中心亮点(星核高光)---
    core_r = max(1, s * 0.055)
    d.ellipse([cx - core_r, cy - core_r, cx + core_r, cy + core_r], fill=(235, 250, 255, 255))

    # --- 16/20px 极小尺寸简化:去掉描边和短臂,只留粗星 ---
    if s <= 20:
        img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        d2 = ImageDraw.Draw(img)
        d2.rounded_rectangle([0, 0, s - 1, s - 1], radius=max(2, s * 22 // 100), fill=C_BG_BOT)
        R2 = s * 0.38
        w2 = s * 0.07
        pts2 = []
        for i in range(4):
            a = math.radians(45 + 90 * i)
            pts2.append((cx + R2 * math.cos(a), cy + R2 * math.sin(a)))
            pts2.append((cx + w2 * math.cos(a + math.radians(45)), cy + w2 * math.sin(a + math.radians(45))))
            pts2.append((cx + w2 * 0.6 * math.cos(a), cy + w2 * 0.6 * math.sin(a)))
            pts2.append((cx + w2 * math.cos(a - math.radians(45)), cy + w2 * math.sin(a - math.radians(45))))
        d2.polygon(pts2, fill=C_CYAN)
        cr = max(1, s * 0.07)
        d2.ellipse([cx - cr, cy - cr, cx + cr, cy + cr], fill=(235, 250, 255, 255))

    return img


imgs = [draw_icon(s) for s in SIZES]
imgs[-1].save("native/src/panel.ico", format="ICO",
              sizes=[(s, s) for s in SIZES], append_images=imgs[:-1])
print("panel.ico written:", SIZES)
