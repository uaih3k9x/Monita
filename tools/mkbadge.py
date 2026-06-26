#!/usr/bin/env python3
# 把任意图片(PNG/JPG/…)烤成 466×466 RGB565 原始数据，供模拟太「电子吧唧」媒体页直接 blit。
# 用法: python3 tools/mkbadge.py 月詠.png badge.565
#   再 scp badge.565 到路由器 /www/，板子翻到媒体页就拉取显示。
# 设备零解码：文件按「大端 RGB565」存，ESP32(小端)读进来即面板要的 sw16(c) 字节序。
import sys, struct
from PIL import Image

W = H = 466
src = sys.argv[1] if len(sys.argv) > 1 else "badge.png"
out = sys.argv[2] if len(sys.argv) > 2 else "badge.565"

im = Image.open(src).convert("RGB")
im.thumbnail((W, H), Image.LANCZOS)                 # 等比缩进 466 内
canvas = Image.new("RGB", (W, H), (0, 0, 0))        # 黑底（圆屏角外/留边都黑）
canvas.paste(im, ((W - im.width) // 2, (H - im.height) // 2))

buf = bytearray()
for (r, g, b) in canvas.getdata():
    c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)   # RGB565
    buf += struct.pack(">H", c)                            # 大端：设备小端读即 sw16(c)

with open(out, "wb") as f:
    f.write(buf)
print(f"{src} → {out}  {len(buf)} 字节  ({W}x{H} RGB565)")
print(f"部署: cat {out} | ssh root@192.168.2.254 'cat >/www/badge.565'")
