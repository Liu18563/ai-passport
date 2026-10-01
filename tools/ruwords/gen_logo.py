#!/usr/bin/env python3
"""从套娃 12 帧动画原图生成 SamL 首页 Logo 素材。

输入: assets/images/source/matryoshka_sheet.png
      1254x1254,4 列 x 3 行共 12 帧,格子之间是白色分隔线(原创,作者刘晓,AI 生成)
输出:
  assets/images/ruw_logo.c         12 个 LVGL9 lv_image_dsc_t(ruw_logo_f0..f11),
                                   72x96 RGB565 不压缩无 alpha,四角圆角外填 COL_BG
  tools/ruwords/logo_preview.png   12 帧按原排布、每帧 3 倍放大的预览

帧含义(与 main/ruwords_logic.h 的 RUW_LOGO_* 一致):
  f0-f3  从左侧探头滑入    f4-f7  歪头、上下弹动
  f8     睁眼静止          f9     半闭眼
  f10    闭眼              f11    睁眼静止(首页常驻帧)
"""
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "assets", "images", "source", "matryoshka_sheet.png")
OUT_C = os.path.join(ROOT, "assets", "images", "ruw_logo.c")
OUT_PNG = os.path.join(ROOT, "tools", "ruwords", "logo_preview.png")

COL_BG = (0xF6, 0xF1, 0xE7)
COLS, ROWS = 4, 3
W, H = 72, 96          # 输出尺寸,与原格子 3:4 一致
RADIUS = 10            # 输出圆角半径(像素)
INSET = 3              # 格子四边各内缩,避开白线的抗锯齿边


def white_runs(profile, threshold=0.9):
    """返回整列/整行几乎全白的连续区间,用来定位分隔线。"""
    runs, start = [], None
    for i, v in enumerate(profile):
        if v > threshold and start is None:
            start = i
        elif v <= threshold and start is not None:
            runs.append((start, i - 1))
            start = None
    if start is not None:
        runs.append((start, len(profile) - 1))
    return runs


def cells(sheet):
    """按白色分隔线切出 12 个格子,返回 [(x0, y0, x1, y1), ...],按行优先。"""
    w, h = sheet.size
    px = sheet.load()

    def is_white(x, y):
        return min(px[x, y]) > 235

    col_profile = [sum(is_white(x, y) for y in range(0, h, 4)) / len(range(0, h, 4)) for x in range(w)]
    row_profile = [sum(is_white(x, y) for x in range(0, w, 4)) / len(range(0, w, 4)) for y in range(h)]
    xs, ys = white_runs(col_profile), white_runs(row_profile)
    if len(xs) != COLS + 1 or len(ys) != ROWS + 1:
        raise SystemExit(f"分隔线数量不对:列 {len(xs)},行 {len(ys)},应为 {COLS + 1} 和 {ROWS + 1}")
    # 所有格子统一用最小的宽高,保证各帧尺寸一致、不跳动。
    cw = min(xs[i + 1][0] - xs[i][1] - 1 for i in range(COLS)) - 2 * INSET
    ch = min(ys[i + 1][0] - ys[i][1] - 1 for i in range(ROWS)) - 2 * INSET
    out = []
    for r in range(ROWS):
        for c in range(COLS):
            x0 = xs[c][1] + 1 + INSET
            y0 = ys[r][1] + 1 + INSET
            out.append((x0, y0, x0 + cw, y0 + ch))
    return out


def rounded(im):
    """四角圆角,圆角外填 COL_BG;在 4 倍尺寸画遮罩再缩小,边缘抗锯齿。"""
    s = 4
    mask = Image.new("L", (W * s, H * s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W * s - 1, H * s - 1], radius=RADIUS * s, fill=255)
    mask = mask.resize((W, H), Image.LANCZOS)
    return Image.composite(im, Image.new("RGB", (W, H), COL_BG), mask)


def to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def write_c(frames):
    lines = [
        "// 这个文件由 tools/ruwords/gen_logo.py 生成,不要手改。",
        f"// 套娃 Logo 12 帧,{W}x{H} RGB565 不压缩无 alpha。",
        '#include "lvgl.h"',
        "",
    ]
    for i, im in enumerate(frames):
        px = [to_rgb565(*im.getpixel((x, y))) for y in range(H) for x in range(W)]
        lines.append(f"static const uint16_t ruw_logo_f{i}_map[{len(px)}] = {{")
        for k in range(0, len(px), 16):
            lines.append("    " + ", ".join(f"0x{v:04X}" for v in px[k:k + 16]) + ",")
        lines += ["};", ""]
    for i in range(len(frames)):
        lines += [
            f"const lv_image_dsc_t ruw_logo_f{i} = {{",
            "    .header.magic = LV_IMAGE_HEADER_MAGIC,",
            "    .header.cf = LV_COLOR_FORMAT_RGB565,",
            "    .header.flags = 0,",
            f"    .header.w = {W},",
            f"    .header.h = {H},",
            f"    .header.stride = {W} * 2,",
            f"    .data_size = sizeof(ruw_logo_f{i}_map),",
            f"    .data = (const uint8_t *)ruw_logo_f{i}_map,",
            "};",
            "",
        ]
    with open(OUT_C, "w") as f:
        f.write("\n".join(lines))


def write_preview(frames):
    gap, z = 6, 3
    sheet = Image.new("RGB", (COLS * (W * z + gap) + gap, ROWS * (H * z + gap) + gap), COL_BG)
    for i, im in enumerate(frames):
        r, c = divmod(i, COLS)
        sheet.paste(im.resize((W * z, H * z), Image.NEAREST), (gap + c * (W * z + gap), gap + r * (H * z + gap)))
    sheet.save(OUT_PNG)


def main():
    sheet = Image.open(SRC).convert("RGB")
    frames = [rounded(sheet.crop(box).resize((W, H), Image.LANCZOS)) for box in cells(sheet)]
    write_c(frames)
    write_preview(frames)
    print(f"wrote {OUT_C}: {len(frames)} frames, {len(frames) * W * H * 2} bytes")


if __name__ == "__main__":
    main()
