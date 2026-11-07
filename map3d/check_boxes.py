"""把检测结果画回原图，核对建筑框和网格标定是否准确。"""
import json
from PIL import Image, ImageDraw

OUT = r"E:\workaddress\31\map3d"
SRC = r"C:\Users\19513\.workbuddy\clipboard-images\clipboard-2026-10-23T16-54-04-126Z-eab95c49.png"

meta = json.load(open(f"{OUT}\\meta.json", encoding="utf-8"))
im = Image.open(SRC).convert("RGB")
W, H = im.size
d = ImageDraw.Draw(im)

# 网格线（实测位置）用青色标出
for x in (34, 142, 250, 358, 466, 574):
    d.line([(x, 0), (x, H)], fill=(0, 220, 220), width=1)
for y in (108, 216, 324, 432):
    d.line([(0, y), (W, y)], fill=(0, 220, 220), width=1)

# 建筑框
for b in meta["buildings"]:
    x0, y0, x1, y1 = b["box_px"]
    d.rectangle([x0, y0, x1, y1], outline=(255, 40, 40), width=2)
    d.text((x0 + 2, max(0, y0 - 11)), str(b["id"]), fill=(255, 240, 0))

im.save(f"{OUT}\\check_boxes.png")
print(f"已保存 {OUT}\\check_boxes.png  建筑 {len(meta['buildings'])} 个")
for b in meta["buildings"]:
    print(f"  #{b['id']:2d} {b['roi']:7s} {b['box_px']}  {b['size_m']}m  h={b['height_m']}m")
