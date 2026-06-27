#!/usr/bin/env python3
# 把 PNG/JPG/GIF 烤成模拟太「电子吧唧」媒体页格式 .m8g —— 静态=1帧、GIF=多帧；设备零解码直接播。
# 用法: python3 tools/mkbadge.py 你的图.gif badge.m8g   再 scp 到路由器 /www/badge.m8g
# 格式: "M8G1" + u16LE(w,h,nframes,0) + 每帧[ u16LE delay_ms + w*h × u16BE RGB565 ]
#   像素大端：ESP32(小端)读进来即面板要的 sw16(c) 字节序，直接 blit。
import sys, struct
from PIL import Image

src  = sys.argv[1] if len(sys.argv) > 1 else "badge.gif"
out  = sys.argv[2] if len(sys.argv) > 2 else "badge.m8g"
size = int(sys.argv[3]) if len(sys.argv) > 3 else 0   # 可选：强制输出边长（动图省 PSRAM 用）

im = Image.open(src)
frames, durs = [], []           # 逐帧抽取（seek 让 PIL 自动合成 GIF 的部分帧/disposal）
i = 0
while True:
    try: im.seek(i)
    except EOFError: break
    frames.append(im.convert("RGB"))
    durs.append(im.info.get("duration", 100))
    i += 1
nf = len(frames)

W = H = size if size else (466 if nf == 1 else 300)   # 指定优先；否则静态铺满 466、动图 300

def to565(fr):
    f = fr.copy(); f.thumbnail((W, H), Image.LANCZOS)
    canvas = Image.new("RGB", (W, H), (0, 0, 0))
    canvas.paste(f, ((W - f.width) // 2, (H - f.height) // 2))
    buf = bytearray()
    for (r, g, b) in canvas.getdata():
        buf += struct.pack(">H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return buf

data = bytearray(b"M8G1")
data += struct.pack("<HHHH", W, H, nf, 0)
for fr, d in zip(frames, durs):
    delay = 60000 if nf == 1 else max(20, min(int(d), 65535))   # 静态给超长延时（只画一次）
    data += struct.pack("<H", delay) + to565(fr)

open(out, "wb").write(data)
kb = len(data) // 1024
print(f"{src} → {out}  {nf} 帧 {W}x{H}  {kb} KB")
if kb > 3000:
    print("⚠ 偏大，建议用更短/更小的 GIF（PSRAM 与传输考虑）")
print(f"部署: cat {out} | ssh root@192.168.2.254 'cat >/www/badge.m8g'")
