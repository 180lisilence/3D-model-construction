"""make_compare.py — 把输入截图与三个视角的渲染结果拼成一张对比图。

布局  2 x 2
    [原始截图]        [俯视还原]
    [斜视 3D]         [等轴测 3D]
"""
from PIL import Image, ImageDraw, ImageFont

OUT = r"E:\workaddress\31\map3d"
SRC = r"C:\Users\19513\.workbuddy\clipboard-images\clipboard-2026-10-23T16-54-04-126Z-eab95c49.png"

CELL_W, CELL_H = 660, 480
PAD = 10
LABEL_H = 30
BG = (16, 18, 24)
FG = (208, 214, 226)

cells = [
    ("输入：PUBG 地图俯视截图", SRC),
    ("俯视还原（几何位置校验）", f"{OUT}\\preview_top.png"),
    ("斜视透视（地形起伏 + 建筑）", f"{OUT}\\preview_persp.png"),
    ("等轴测（建筑分布全貌）", f"{OUT}\\preview_iso.png"),
]

W = CELL_W * 2 + PAD * 3
H = (CELL_H + LABEL_H) * 2 + PAD * 3
canvas = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(canvas)

try:
    font = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 17)
except Exception:
    font = ImageFont.load_default()

for k, (label, path) in enumerate(cells):
    col = k % 2
    row = k // 2
    x = PAD + col * (CELL_W + PAD)
    y = PAD + row * (CELL_H + LABEL_H + PAD)

    d.text((x + 4, y + 4), label, fill=FG, font=font)

    try:
        im = Image.open(path).convert("RGB")
    except Exception as e:
        print(f"跳过 {path}: {e}")
        continue
    # 等比缩放到单元格内
    sc = min(CELL_W / im.width, CELL_H / im.height)
    im2 = im.resize((int(im.width * sc), int(im.height * sc)), Image.LANCZOS)
    ox = x + (CELL_W - im2.width) // 2
    oy = y + LABEL_H + (CELL_H - im2.height) // 2
    canvas.paste(im2, (ox, oy))
    d.rectangle([x, y + LABEL_H, x + CELL_W, y + LABEL_H + CELL_H],
                outline=(48, 52, 62), width=1)

canvas.save(f"{OUT}\\compare.png")
print(f"已保存 {OUT}\\compare.png  ({W}x{H})")
