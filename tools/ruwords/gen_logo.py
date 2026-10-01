#!/usr/bin/env python3
"""从套娃原图生成俄语单词斩 Logo 的五帧素材。

输入: assets/images/source/matryoshka_source.png (1254x1254, 原创套娃)
输出:
  assets/images/ruw_logo.c   5 个 LVGL9 lv_image_dsc_t (88x88 RGB565 不压缩无 alpha)
  tools/ruwords/logo_preview.png  5 帧横排、每帧 3 倍预览

处理流程(原生分辨率处理,最后才 LANCZOS 缩到 88):
  圆形裁切(圆外填 COL_BG,边缘抗锯齿) -> 旋转 0/5/10/15 度(逆时针) -> 88x88
  闭眼帧: 在原图脸部区域(R,G,B<60)取眼睛,膨胀6px后用邻近肤色填平,
          再沿眼睛原方向画一条略弯的闭眼线,最后同样转 15 度。
"""
import math
import os
import struct
import zlib

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "assets", "images", "source", "matryoshka_source.png")
OUT_C = os.path.join(ROOT, "assets", "images", "ruw_logo.c")
OUT_PNG = os.path.join(ROOT, "tools", "ruwords", "logo_preview.png")

COL_BG = (0xF6, 0xF1, 0xE7)
N = 88                      # 输出边长
FACE = (300, 330, 1050, 800)  # 原图脸部区域 (x0,y0,x1,y1)
DILATE = 6                   # 眼睛膨胀像素
ANGLES = [0, 5, 10, 15]      # r0,r5,r10,r15


def circular_crop(im):
    """以中心为圆心、直径=边长做圆形裁切,圆外填 COL_BG,边缘抗锯齿。"""
    w, h = im.size
    scale = 2
    mask = Image.new("L", (w * scale, h * scale), 0)
    md = ImageDraw.Draw(mask)
    md.ellipse([0, 0, w * scale - 1, h * scale - 1], fill=255)
    mask = mask.resize((w, h), Image.LANCZOS)  # 边缘抗锯齿
    bg = Image.new("RGB", (w, h), COL_BG)
    return Image.composite(im, bg, mask)


def find_eyes(im):
    """脸部区域内 R,G,B 都<60 的像素 = 眼睛。返回左右两个点列表与肤色。"""
    px = im.load()
    x0, y0, x1, y1 = FACE
    left, right = [], []
    skin_samples = []
    all_dark = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = px[x, y]
            if r < 60 and g < 60 and b < 60:
                all_dark.append((x, y))
            # 取样肤色:亮而偏暖、非红兜帽、非花。
            elif r > 200 and g > 180 and b > 150 and r > b + 25 and g >= b - 10:
                skin_samples.append((r, g, b))
    if not all_dark:
        raise SystemExit("未找到眼睛像素")
    xs = sorted(p[0] for p in all_dark)
    split = xs[len(xs) // 2]
    for p in all_dark:
        (left if p[0] <= split else right).append(p)
    sr = sum(p[0] for p in skin_samples) // len(skin_samples)
    sg = sum(p[1] for p in skin_samples) // len(skin_samples)
    sb = sum(p[2] for p in skin_samples) // len(skin_samples)
    return left, right, (sr, sg, sb), all_dark


def dilate_mask(points, w, h, r):
    """给定像素集合,返回膨胀 r 像素后的 set。"""
    out = set()
    for (x, y) in points:
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                if dx * dx + dy * dy <= r * r:
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < w and 0 <= ny < h:
                        out.add((nx, ny))
    return out


def make_blink_source(im):
    """在原图上把眼睛填成肤色,并画闭眼弧线。"""
    im = im.convert("RGB")
    w, h = im.size
    left, right, skin, _ = find_eyes(im)

    # 膨胀后的眼睛区域全部用肤色填平(不留眼眶)。
    fill = dilate_mask(left + right, w, h, DILATE)
    px = im.load()
    for (x, y) in fill:
        px[x, y] = skin

    draw = ImageDraw.Draw(im)
    for eye in (left, right):
        n = len(eye)
        cx = sum(p[0] for p in eye) / n
        cy = sum(p[1] for p in eye) / n
        # 主方向(协方差)= 睁眼胶囊的长轴。
        sxx = sum((p[0] - cx) ** 2 for p in eye) / n
        syy = sum((p[1] - cy) ** 2 for p in eye) / n
        sxy = sum((p[0] - cx) * (p[1] - cy) for p in eye) / n
        ang = 0.5 * math.atan2(2 * sxy, sxx - syy)
        ux, uy = math.cos(ang), math.sin(ang)        # 睁眼长轴
        vx, vy = -uy, ux                            # 闭眼线方向(短轴)
        # 闭眼线沿短轴,长度≈短轴跨度,粗细≈短轴跨度/3。
        tproj = [p[0] * vx + p[1] * vy for p in eye]
        short_span = max(tproj) - min(tproj)
        half = short_span * 0.62
        thick = thich_safe(short_span / 3)
        sag = short_span * 0.12                      # 中间微向下弯
        p0 = (cx - vx * half, cy - vy * half)
        p2 = (cx + vx * half, cy + vy * half)
        p1 = (cx + ux * sag, cy + uy * sag)
        draw.line([p0, p1, p2], fill=(28, 26, 26), width=thick)
    return im


def thich_safe(v):
    return max(2, min(v, 10))


def to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def encode(im):
    im = im.convert("RGB").resize((N, N), Image.LANCZOS)
    data = im.load()
    out = []
    for y in range(N):
        for x in range(N):
            out.append(to_rgb565(*data[x, y]))
    return out


def write_c(frames):
    """frames: [(name, pixels88), ...];保留 ruw_logo = r15 兼容旧 app.c。"""
    os.makedirs(os.path.dirname(OUT_C), exist_ok=True)
    lines = [
        "// 这个文件由 tools/ruwords/gen_logo.py 生成,不要手改。",
        "// 套娃(матрёшка)Logo 五帧,88x88 RGB565 不压缩无 alpha。",
        "// r0/r5/r10/r15 = 逆时针 0/5/10/15 度;blink = 15 度闭眼帧。",
        '#include "lvgl.h"',
        "",
    ]
    for name, px in frames:
        n = len(px)
        lines.append("static const uint16_t %s_map[%d] = {" % (name, n))
        for i in range(0, n, 16):
            lines.append("    " + ", ".join("0x%04X" % v for v in px[i:i + 16]) + ",")
        lines.append("};")
        lines.append("")
    for name, _ in frames:
        lines += [
            "const lv_image_dsc_t %s = {" % name,
            "    .header.magic = LV_IMAGE_HEADER_MAGIC,",
            "    .header.cf = LV_COLOR_FORMAT_RGB565,",
            "    .header.flags = 0,",
            "    .header.w = %d," % N,
            "    .header.h = %d," % N,
            "    .header.stride = %d * 2," % N,
            "    .data_size = sizeof(%s_map)," % name,
            "    .data = (const uint8_t *)%s_map," % name,
            "};",
            "",
        ]
    # 兼容旧首页:ruw_logo 即静止帧 r15,提交2 改版后可移除。
    lines += [
        "// 兼容符号:旧首页直接用 ruw_logo,即静止 15 度帧。",
        "const lv_image_dsc_t ruw_logo = ruw_logo_r15;",
        "",
    ]
    with open(OUT_C, "w") as f:
        f.write("\n".join(lines))


def write_preview(frames):
    """五帧横排,每帧放大 3 倍,背景 COL_BG。"""
    s = 3
    strip = Image.new("RGB", (N * s * len(frames), N * s), COL_BG)
    d = ImageDraw.Draw(strip)
    for i, (_, px) in enumerate(frames):
        im = Image.new("RGB", (N, N))
        im.putdata([(((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3)
                    for v in px])
        im = im.resize((N * s, N * s), Image.NEAREST)
        strip.paste(im, (i * N * s, 0))
        d.text((i * N * s + 6, 6), ["r0", "r5", "r10", "r15", "blink"][i], fill=(0, 0, 0))
    strip.save(OUT_PNG)


def main():
    src = Image.open(SRC).convert("RGB")
    base = circular_crop(src)
    closed = circular_crop(make_blink_source(src))

    frames = []
    for ang in ANGLES:
        rot = base.rotate(ang, expand=False, fillcolor=COL_BG, resample=Image.BICUBIC)
        frames.append(("ruw_logo_r%d" % ang, encode(rot)))
    rot = closed.rotate(15, expand=False, fillcolor=COL_BG, resample=Image.BICUBIC)
    frames.append(("ruw_logo_blink", encode(rot)))

    write_c(frames)
    write_preview(frames)
    total = sum(len(p) * 2 for _, p in frames)
    print("wrote", OUT_C, "frames=%d  total=%d bytes (%.1f KiB)" % (len(frames), total, total / 1024))
    print("wrote", OUT_PNG)


if __name__ == "__main__":
    main()
