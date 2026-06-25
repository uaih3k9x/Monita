#!/usr/bin/env python3
# 烤中文点阵字库 → fw/main/font_cjk.h
from PIL import Image, ImageFont, ImageDraw
import sys

CELL = 24          # 点阵格子（正方）
FONTPX = 23        # 字号（略小于格子留边）
THRESH = 96
FONT = "/System/Library/Fonts/Hiragino Sans GB.ttc"
OUT = "fw/main/font_cjk.h"

font = ImageFont.truetype(FONT, FONTPX)
asc, desc = font.getmetrics()
ytop = max(0, (CELL - (asc + desc)) // 2)   # 垂直大致居中（统一基线）

# 字符集：ASCII + GB2312（区 1-9 符号 + 16-55 一级常用）
cps = set(range(0x20, 0x7f))
for qu in list(range(1, 10)) + list(range(16, 88)):   # 区1-9符号 + 16-87 一级&二级常用字
    for wei in range(1, 95):
        try:
            ch = bytes([0xa0 + qu, 0xa0 + wei]).decode('gb2312')
            if ch.strip():
                cps.add(ord(ch))
        except Exception:
            pass
cps = sorted(cps)
ROWBYTES = (CELL + 7) // 8
BYTES = ROWBYTES * CELL

def render(cp):
    ch = chr(cp)
    im = Image.new('L', (CELL, CELL), 0)
    d = ImageDraw.Draw(im)
    try:
        adv = int(round(font.getlength(ch)))
    except Exception:
        adv = CELL
    if adv <= 0:
        adv = CELL
    if adv > CELL:
        adv = CELL
    ox = 0 if cp < 128 else max(0, (CELL - adv) // 2)
    d.text((ox, ytop), ch, font=font, fill=255)
    px = im.load()
    glyph = bytearray()
    for y in range(CELL):
        for bx in range(ROWBYTES):
            b = 0
            for bit in range(8):
                x = bx * 8 + bit
                if x < CELL and px[x, y] > THRESH:
                    b |= (1 << (7 - bit))
            glyph.append(b)
    return adv, glyph

advs = []
bmp = bytearray()
for cp in cps:
    a, g = render(cp)
    advs.append(a)
    bmp += g

# 自检：把 满/网 打成字符画
def ascii_art(cp):
    a, g = render(cp)
    print(f"--- U+{cp:04X} {chr(cp)} (adv={a}) ---")
    for y in range(CELL):
        row = ""
        for x in range(CELL):
            byte = g[y * ROWBYTES + (x // 8)]
            row += "##" if (byte >> (7 - (x % 8))) & 1 else "  "
        print(row)
for ch in "满网咦":
    ascii_art(ord(ch))

with open(OUT, 'w') as f:
    f.write("// 自动生成：GB2312 一级常用字 + 符号 + ASCII，%d×%d 点阵\n" % (CELL, CELL))
    f.write("#pragma once\n#include <stdint.h>\n")
    f.write("#define FONT_W %d\n#define FONT_H %d\n#define FONT_ROWBYTES %d\n#define FONT_BYTES %d\n#define FONT_N %d\n"
            % (CELL, CELL, ROWBYTES, BYTES, len(cps)))
    f.write("static const uint16_t FONT_CP[FONT_N]={")
    f.write(",".join(str(c) for c in cps))
    f.write("};\n")
    f.write("static const uint8_t FONT_ADV[FONT_N]={")
    f.write(",".join(str(a) for a in advs))
    f.write("};\n")
    f.write("static const uint8_t FONT_BMP[]={")
    f.write(",".join(str(b) for b in bmp))
    f.write("};\n")

print(f"\n字符数 N={len(cps)}  每字 {BYTES}B  位图共 {len(bmp)//1024}KB  → {OUT}")
