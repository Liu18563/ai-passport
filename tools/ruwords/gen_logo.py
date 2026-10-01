#!/usr/bin/env python3
"""生成俄语单词斩的像素护耳毛帽(ушанка)Logo。

32x32 字符矩阵 -> 每像素 x3 -> 96x96 RGB565,输出:
  assets/images/ruw_logo.c   LVGL9 lv_image_dsc_t(不压缩、无 alpha)
  tools/ruwords/logo_preview.png  288x288 预览

颜色 <= 8,背景 = COL_BG(0xF6F1E7)。不要红星/镰刀锤子/任何政治符号。
改图只需改下面 MATRIX,每字符一个颜色。
"""
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT_C = os.path.join(ROOT, "assets", "images", "ruw_logo.c")
OUT_PNG = os.path.join(ROOT, "tools", "ruwords", "logo_preview.png")

SCALE = 3
W = H = 32

# 字符 -> RGB565 (r,g,b)
#   .  背景 COL_BG
#   D  深色描边
#   F  毛皮主色(棕)
#   L  毛皮高光
#   V  翻起帽檐(皮)
#   B  黄铜小扣(装饰,非政治符号)
PALETTE = {
    ".": (0xF6, 0xF1, 0xE7),
    "D": (0x3A, 0x24, 0x13),
    "F": (0x7A, 0x4A, 0x26),
    "L": (0xA6, 0x73, 0x42),
    "V": (0x54, 0x35, 0x1F),
    "B": (0xC9, 0xA2, 0x27),
}

# 32 列 x 32 行。护耳放下、正面帽檐翻起的正面像。每行不足 32 列右侧补背景。
MATRIX = [
    "................................",  # 0
    "................................",  # 1
    "................................",  # 2
    "................................",  # 3
    "................................",  # 4
    "..........DDDDDDDD............",  # 5  帽顶
    "........DDLLLLLLLLDD..........",  # 6
    ".......DLLFFFFFFLLD...........",  # 7
    "......DLFFFFFFFFFFLD..........",  # 8
    ".....DLFFFFFFFFFFFFLD..........",  # 9
    ".....DLFFFFFFFFFFFFLD..........",  # 10
    "....DLFFFFFFFFFFFFFFLD.........",  # 11
    "....DLFFFFFFFFFFFFFFLD.........",  # 12
    "...DLFFFFFFFFFFFFFFFFLD.......",  # 13
    "...DLFFFFFFFFFFFFFFFFLD.......",  # 14
    "..DFFFDDDDDDDDDDDDDDDDFFF.....",  # 15  冠底
    ".DFFF..................FFF....",  # 16
    ".DFF..DDDDDDDDDDDDDD..FFD....",  # 17  帽檐上沿
    ".DFF..DVVVVVVVVVVVVD..FFD....",  # 18
    ".DFF..DVVBBDVVVVVVVD..FFD....",  # 19  黄铜扣
    ".DFF..DVVBBDVVVVVVVD..FFD....",  # 20
    ".DFF..DVVVVVVVVVVVVD..FFD....",  # 21
    ".DFF..DDDDDDDDDDDDDD..FFD....",  # 22  帽檐下沿
    ".DFF...................FFD...",  # 23
    ".DFF...................FFD...",  # 24
    ".DFF...................FFD...",  # 25
    "..DD...................DD....",  # 26
    "................................",  # 27
    "................................",  # 28
    "................................",  # 29
    "................................",  # 30
    "................................",  # 31
]


def to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def build():
    assert len(MATRIX) == H, f"rows={len(MATRIX)}"
    rows = []
    for row in MATRIX:
        row = row.ljust(W, ".")[:W]
        rows.append(row)
    # 96x96 像素,RGB565 小端
    px = []
    for y in range(H * SCALE):
        sy = y // SCALE
        for x in range(W * SCALE):
            sx = x // SCALE
            ch = rows[sy][sx]
            px.append(to_rgb565(*PALETTE[ch]))
    return px


def write_c(px):
    os.makedirs(os.path.dirname(OUT_C), exist_ok=True)
    n = len(px)
    lines = ["// 这个文件由 tools/ruwords/gen_logo.py 生成,不要手改。",
             "// 护耳毛帽(ушанка)像素 Logo,96x96 RGB565 不压缩无 alpha。",
             '#include "lvgl.h"',
             ""]
    body = ["const uint16_t ruw_logo_map[" + str(n) + "] = {"]
    for i in range(0, n, 16):
        chunk = ", ".join("0x%04X" % v for v in px[i:i + 16])
        body.append("    " + chunk + ",")
    body.append("};")
    body.append("")
    body.append("const lv_image_dsc_t ruw_logo = {")
    body.append("    .header.magic = LV_IMAGE_HEADER_MAGIC,")
    body.append("    .header.cf = LV_COLOR_FORMAT_RGB565,")
    body.append("    .header.flags = 0,")
    body.append("    .header.w = 96,")
    body.append("    .header.h = 96,")
    body.append("    .header.stride = 96 * 2,")
    body.append("    .data_size = sizeof(ruw_logo_map),")
    body.append("    .data = (const uint8_t *)ruw_logo_map,")
    body.append("};")
    with open(OUT_C, "w") as f:
        f.write("\n".join(lines + body) + "\n")


def write_png(px):
    # 纯 stdlib 写个最小 PNG(96x96 再放大到 288x288 预览)。
    import zlib
    S = 3  # 96 -> 288
    pw, ph = 96 * S, 96 * S
    raw = bytearray()
    for y in range(ph):
        raw.append(0)  # filter type 0
        sy = (y // S)
        for x in range(pw):
            sx = (x // S)
            v = px[sy * 96 + sx]
            r = ((v >> 11) & 0x1F) << 3
            g = ((v >> 5) & 0x3F) << 2
            b = (v & 0x1F) << 3
            raw += bytes((r, g, b))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", pw, ph, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))
    with open(OUT_PNG, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    px = build()
    write_c(px)
    write_png(px)
    print("wrote", OUT_C)
    print("wrote", OUT_PNG)
