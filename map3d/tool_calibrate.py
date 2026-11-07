"""分析 PUBG 地图截图：网格标定 + 建筑候选 + 地形亮度分布。

输出一张控制台报告，供后续建模脚本取参数。
"""
import sys
from PIL import Image, ImageFilter

SRC = r"C:\Users\19513\.workbuddy\clipboard-images\clipboard-2026-10-23T16-54-04-126Z-eab95c49.png"

im = Image.open(SRC).convert("RGB")
W, H = im.size
print(f"size = {W} x {H}")

px = im.load()


def gray(x, y):
    r, g, b = px[x, y]
    return (r * 299 + g * 587 + b * 114) // 1000


# ---- 1) 全局亮度统计 -----------------------------------------------------
hist = [0] * 256
for y in range(0, H, 2):
    for x in range(0, W, 2):
        hist[gray(x, y)] += 1
tot = sum(hist)
print("\n亮度分布（每 16 级）:")
for lo in range(0, 256, 16):
    c = sum(hist[lo:lo + 16])
    print(f"  {lo:3d}-{lo+15:3d}  {100.0*c/tot:5.1f}%  {'#' * int(60.0*c/tot)}")

# ---- 2) 网格线检测 -------------------------------------------------------
# 地图网格是贯穿全图的一条细线，一列（或一行）的整体亮度会与邻居不同。
colmean = []
for x in range(W):
    s = 0
    for y in range(0, H, 3):
        s += gray(x, y)
    colmean.append(s / ((H + 2) // 3))

rowmean = []
for y in range(H):
    s = 0
    for x in range(0, W, 3):
        s += gray(x, y)
    rowmean.append(s / ((W + 2) // 3))


def find_lines(vals, label):
    n = len(vals)
    med = sorted(vals)[n // 2]
    cand = []
    for i in range(2, n - 2):
        # 相对邻域中位数的偏离
        nb = sorted(vals[i - 2:i + 3])
        base = nb[2]
        d = vals[i] - base
        if abs(d) > 6.0:
            cand.append((i, d))
    # 合并相邻
    groups = []
    for i, d in cand:
        if groups and i - groups[-1][-1][0] <= 3:
            groups[-1].append((i, d))
        else:
            groups.append([(i, d)])
    lines = []
    for g in groups:
        best = max(g, key=lambda t: abs(t[1]))
        lines.append(best)
    print(f"\n{label} 候选线 (位置, 相对亮度):")
    for i, d in lines:
        print(f"   {i:4d}  {d:+.1f}")
    if len(lines) >= 2:
        gaps = [lines[i + 1][0] - lines[i][0] for i in range(len(lines) - 1)]
        print(f"   间距: {gaps}")
    return lines


find_lines(colmean, "竖线")
find_lines(rowmean, "横线")

# ---- 3) 亮块（白色屋顶）检测 --------------------------------------------
# 白色屋顶：亮度高、RGB 三通道接近（低饱和）
mask = Image.new("L", (W, H), 0)
mp = mask.load()
for y in range(H):
    for x in range(W):
        r, g, b = px[x, y]
        mx, mn = max(r, g, b), min(r, g, b)
        if mx > 195 and (mx - mn) < 42:
            mp[x, y] = 255

# 去噪
mask = mask.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.MaxFilter(3))
mask.save(r"E:\workaddress\31\_mask_roof.png")

# 连通域
seen = [[False] * W for _ in range(H)]
maskp = mask.load()
comps = []
for y0 in range(H):
    for x0 in range(W):
        if maskp[x0, y0] == 0 or seen[y0][x0]:
            continue
        stack = [(x0, y0)]
        seen[y0][x0] = True
        minx = maxx = x0
        miny = maxy = y0
        cnt = 0
        while stack:
            x, y = stack.pop()
            cnt += 1
            if x < minx: minx = x
            if x > maxx: maxx = x
            if y < miny: miny = y
            if y > maxy: maxy = y
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx2, ny2 = x + dx, y + dy
                if 0 <= nx2 < W and 0 <= ny2 < H and not seen[ny2][nx2] and maskp[nx2, ny2]:
                    seen[ny2][nx2] = True
                    stack.append((nx2, ny2))
        if cnt >= 40:
            comps.append((cnt, minx, miny, maxx, maxy))

comps.sort(reverse=True)
print(f"\n亮块连通域（>=40px）共 {len(comps)} 个:")
for cnt, x0, y0, x1, y1 in comps[:28]:
    w = x1 - x0 + 1
    h = y1 - y0 + 1
    fill = 100.0 * cnt / (w * h)
    print(f"   px={cnt:5d}  box=({x0:3d},{y0:3d})-({x1:3d},{y1:3d})  {w:3d}x{h:3d}  填充{fill:4.1f}%")
