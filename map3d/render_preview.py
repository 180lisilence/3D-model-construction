"""
render_preview.py — 把 meta.json 的几何渲染成图片，用于验证模型正确性。

两种视图
--------
top   : 正交俯视。屏幕坐标 = 原图像素坐标，因此渲染结果应几乎等于原图。
        这是最强验证 —— 俯视能还原截图，就说明位置映射和 UV 没错。
persp : 透视斜视。带朗伯光照，画家算法做隐藏面消除。

美术处理（让"灰盒"看起来像个模型而不是悬浮的布）
------------------------------------------------
* 地形四周生成垂直裙边，向下延伸到最低点以下，形成沙盘/地形块的剖面
* 建筑做四坡顶（hip roof），PUBG 沙漠房基本都是这种
* 超采样渲染再降采样，消除锯齿
* 天空用垂直渐变，地平线附近略亮

用法
----
    python render_preview.py top
    python render_preview.py persp
    python render_preview.py all
"""
import json
import math
import os
import sys
import time
from PIL import Image, ImageDraw, ImageFilter, ImageEnhance

OUT = r"E:\workaddress\31\map3d"
SRC = r"C:\Users\19513\.workbuddy\clipboard-images\clipboard-2026-10-23T16-54-04-126Z-eab95c49.png"

APRON_M = 30.0      # 裙边向下延伸深度（做厚，让地形像个"块"而不是"布"）
RIDGE_RATIO = 0.16  # 四坡顶脊高 = 建筑高 * 该比例


def norm(v):
    L = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    return (0.0, 0.0, 1.0) if L < 1e-9 else (v[0] / L, v[1] / L, v[2] / L)


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def load():
    meta = json.load(open(f"{OUT}\\meta.json", encoding="utf-8"))
    # 剪贴板原图可能已被清理，回退到本目录下的贴图副本
    src_path = SRC if os.path.exists(SRC) else f"{OUT}\\miramar_tex.png"
    src = Image.open(src_path).convert("RGB")
    return meta, src


def terrain_shade(h, i, j, GX, GY, cell_m, light=True):
    if not light:
        return 1.0
    dhdx = (h[j][min(GX - 1, i + 1)] - h[j][max(0, i - 1)]) / (2 * cell_m)
    dhdy = (h[min(GY - 1, j + 1)][i] - h[max(0, j - 1)][i]) / (2 * cell_m)
    n = norm((-dhdx, -dhdy, 1.0))
    L = norm((-0.40, -0.50, 0.77))
    return 0.52 + 0.72 * max(0.0, dot(n, L))


def building_solids(meta):
    """重建建筑：返回 (lo4, hi4, ridge_top)。四坡顶由 hi4 + 脊点构成。"""
    px2m = meta["px_to_m"]
    INS, SINK = 0.35, 3.0
    out = []
    for b in meta["buildings"]:
        x0p, y0p, x1p, y1p = b["box_px"]
        x0, x1 = x0p * px2m, (x1p + 1) * px2m
        y0, y1 = y0p * px2m, (y1p + 1) * px2m
        gz = b["ground_z"]
        z0 = gz - SINK
        z1 = gz + b["height_m"]
        lo = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)]
        hi = [(x0 + INS, y0 + INS, z1), (x1 - INS, y0 + INS, z1),
              (x1 - INS, y1 - INS, z1), (x0 + INS, y1 - INS, z1)]
        ridge = (((hi[0][0] + hi[2][0]) * 0.5),
                 ((hi[0][1] + hi[2][1]) * 0.5),
                 z1 + b["height_m"] * RIDGE_RATIO)
        out.append((lo, hi, ridge))
    return out


# --------------------------------------------------------------------------
def render_top(meta, src, path):
    """正交俯视，关光照，用于和原图逐像素比对。"""
    h = meta["heightfield"]
    GX, GY = meta["grid"]
    step = meta["step_px"]
    W, H = meta["image_wh"]
    sp = src.load()

    im = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(im)
    n = 0
    for j in range(GY - 1):
        for i in range(GX - 1):
            x0, y0 = i * step, j * step
            x1, y1 = x0 + step, y0 + step
            if x1 > W - 1 or y1 > H - 1:
                continue
            r, g, b = sp[min(W - 1, x0), min(H - 1, y0)]
            col = (r, g, b)
            d.polygon([(x0, y0), (x1, y1), (x0, y1)], fill=col)
            d.polygon([(x0, y0), (x1, y0), (x1, y1)], fill=col)
            n += 2

    px2m = meta["px_to_m"]
    for bi in meta["buildings"]:
        x0, y0, x1, y1 = bi["box_px"]
        d.rectangle([x0, y0, x1, y1], fill=(198, 190, 165))
        d.rectangle([x0 + 1, y0 + 1, x1 - 1, y1 - 1], outline=(85, 78, 62))

    im.save(path)
    print(f"  俯视: 三角 {n} -> {path}")


def bilinear(h, GX, GY, fi, fj):
    """在高度场上做双线性采样（fi/fj 为浮点格坐标）。"""
    i0 = max(0, min(GX - 1, int(fi)))
    j0 = max(0, min(GY - 1, int(fj)))
    i1 = min(GX - 1, i0 + 1)
    j1 = min(GY - 1, j0 + 1)
    ti = fi - i0
    tj = fj - j0
    return (h[j0][i0] * (1 - ti) * (1 - tj) + h[j0][i1] * ti * (1 - tj)
            + h[j1][i0] * (1 - ti) * tj + h[j1][i1] * ti * tj)


def render_persp(meta, src, path, az_deg=-76.0, el_deg=26.0, fov_deg=33.0,
                 tw=1400, th=820, ss=2, sub=2):
    t0 = time.time()
    h0 = meta["heightfield"]
    GX0, GY0 = meta["grid"]
    step = meta["step_px"]
    W, H = meta["image_wh"]
    px2m = meta["px_to_m"]

    # 渲染用加密网格：几何形状由双线性插值保持，但颜色采样点更密，
    # 能显著削弱「每格一个色块」的马赛克。
    GX = (GX0 - 1) * sub + 1
    GY = (GY0 - 1) * sub + 1
    cell_px = step / float(sub)
    cell_m = cell_px * px2m
    h = [[bilinear(h0, GX0, GY0, i / float(sub), j / float(sub))
          for i in range(GX)] for j in range(GY)]
    sp = src.load()

    ex, ey = W * px2m, H * px2m
    zmin = min(h[j][i] for j in range(GY) for i in range(GX))
    zmax = max(h[j][i] for j in range(GY) for i in range(GX))
    target = (ex * 0.5, ey * 0.56, (zmin + zmax) * 0.5)

    az, el = math.radians(az_deg), math.radians(el_deg)
    R = max(ex, ey) * 1.38
    C = (target[0] + R * math.cos(el) * math.cos(az),
         target[1] + R * math.cos(el) * math.sin(az),
         target[2] + R * math.sin(el))

    zaxis = norm((C[0] - target[0], C[1] - target[1], C[2] - target[2]))
    xaxis = norm(cross((0.0, 0.0, 1.0), zaxis))
    yaxis = cross(zaxis, xaxis)

    SW, SH = tw * ss, th * ss
    f = (SH * 0.5) / math.tan(math.radians(fov_deg) * 0.5)

    def project(p):
        dx, dy, dz = p[0] - C[0], p[1] - C[1], p[2] - C[2]
        cx = dx * xaxis[0] + dy * xaxis[1] + dz * xaxis[2]
        cy = dx * yaxis[0] + dy * yaxis[1] + dz * yaxis[2]
        cz = dx * zaxis[0] + dy * zaxis[1] + dz * zaxis[2]
        if cz > -1e-3:
            return None
        inv = f / (-cz)
        return (SW * 0.5 + cx * inv, SH * 0.5 - cy * inv, -cz)

    faces = []

    def push(tri, col, cull=True):
        if cull:
            nrm = cross((tri[1][0] - tri[0][0], tri[1][1] - tri[0][1], tri[1][2] - tri[0][2]),
                        (tri[2][0] - tri[0][0], tri[2][1] - tri[0][1], tri[2][2] - tri[0][2]))
            if dot(nrm, (C[0] - tri[0][0], C[1] - tri[0][1], C[2] - tri[0][2])) <= 0:
                return
        pa, pb, pc = project(tri[0]), project(tri[1]), project(tri[2])
        if pa is None or pb is None or pc is None:
            return
        if max(pa[0], pb[0], pc[0]) < 0 or min(pa[0], pb[0], pc[0]) > SW:
            return
        if max(pa[1], pb[1], pc[1]) < 0 or min(pa[1], pb[1], pc[1]) > SH:
            return
        faces.append(((pa[2] + pb[2] + pc[2]) / 3.0,
                      [(pa[0], pa[1]), (pb[0], pb[1]), (pc[0], pc[1])], col))

    # ---- 地形 ----
    for j in range(GY - 1):
        for i in range(GX - 1):
            x0, y0 = i * cell_px * px2m, j * cell_px * px2m
            x1, y1 = (i + 1) * cell_px * px2m, (j + 1) * cell_px * px2m
            p00 = (x0, y0, h[j][i])
            p11 = (x1, y1, h[j + 1][i + 1])
            p01 = (x0, y1, h[j + 1][i])
            p10 = (x1, y0, h[j][i + 1])
            r, g, b = sp[min(W - 1, int(i * cell_px)), min(H - 1, int(j * cell_px))]
            s = min(1.35, terrain_shade(h, i, j, GX, GY, cell_m))
            col = (min(255, int(r * s)), min(255, int(g * s)), min(255, int(b * s)))
            push((p00, p11, p01), col)
            push((p00, p10, p11), col)

    # ---- 地形裙边（四条边，做成剖面感）----
    # 裙边不做背面剔除：绕序方向随视角变化，剔除会漏掉整条边
    # 颜色统一成土色 —— 若按格取地形色，剖面会出现密集竖条纹
    zb = zmin - APRON_M
    APRON_COL = (52, 45, 36)
    for i in range(GX - 1):
        x0, x1 = i * cell_px * px2m, (i + 1) * cell_px * px2m

        push(((x0, 0.0, h[0][i]), (x1, 0.0, h[0][i + 1]), (x1, 0.0, zb)), APRON_COL, False)
        push(((x0, 0.0, h[0][i]), (x1, 0.0, zb), (x0, 0.0, zb)), APRON_COL, False)

        yb = ey
        push(((x1, yb, h[GY - 1][i + 1]), (x0, yb, h[GY - 1][i]), (x0, yb, zb)), APRON_COL, False)
        push(((x1, yb, h[GY - 1][i + 1]), (x0, yb, zb), (x1, yb, zb)), APRON_COL, False)
    for j in range(GY - 1):
        y0, y1 = j * cell_px * px2m, (j + 1) * cell_px * px2m

        push(((0.0, y1, h[j + 1][0]), (0.0, y0, h[j][0]), (0.0, y0, zb)), APRON_COL, False)
        push(((0.0, y1, h[j + 1][0]), (0.0, y0, zb), (0.0, y1, zb)), APRON_COL, False)

        push(((ex, y0, h[j][GX - 1]), (ex, y1, h[j + 1][GX - 1]), (ex, y1, zb)), APRON_COL, False)
        push(((ex, y0, h[j][GX - 1]), (ex, y1, zb), (ex, y0, zb)), APRON_COL, False)

    # ---- 建筑投影（沿光方向落到地形上，让房子"坐"实）----
    LOFF = (0.26, 0.33)   # 偏移量 = 建筑高 * 该系数，近似光方向 (0.40,0.50,-0.77)
    SHADOW = (58, 52, 41)
    for lo, hi, ridge in building_solids(meta):
        x0b, y0b = lo[0][0], lo[0][1]
        x1b, y1b = lo[2][0], lo[2][1]
        hgt = hi[0][2] - lo[0][2]
        ox, oy = hgt * LOFF[0], hgt * LOFF[1]
        N = 3
        pts = []
        for vv in range(N + 1):
            row = []
            for uu in range(N + 1):
                wx = x0b + (x1b - x0b) * (uu / N) + ox
                wy = y0b + (y1b - y0b) * (vv / N) + oy
                wz = bilinear(h0, GX0, GY0,
                              wx / px2m / step, wy / px2m / step) + 0.35
                row.append((wx, wy, wz))
            pts.append(row)
        for vv in range(N):
            for uu in range(N):
                a, b_ = pts[vv][uu], pts[vv][uu + 1]
                c, d_ = pts[vv + 1][uu + 1], pts[vv + 1][uu]
                push((a, b_, c), SHADOW)
                push((a, c, d_), SHADOW)

    # ---- 建筑 ----
    LO = norm((-0.40, -0.50, 0.77))
    WALL = (150, 141, 120)
    ROOF = (178, 170, 145)
    for lo, hi, ridge in building_solids(meta):
        # 四面墙
        for k in range(4):
            a, b_ = lo[k], lo[(k + 1) % 4]
            c, d_ = hi[(k + 1) % 4], hi[k]
            nrm = cross((b_[0] - a[0], b_[1] - a[1], b_[2] - a[2]),
                        (d_[0] - a[0], d_[1] - a[1], d_[2] - a[2]))
            if dot(nrm, (C[0] - a[0], C[1] - a[1], C[2] - a[2])) <= 0:
                continue
            s = min(1.2, 0.50 + 0.58 * max(0.0, dot(norm(nrm), LO)))
            col = (min(255, int(WALL[0] * s)), min(255, int(WALL[1] * s)), min(255, int(WALL[2] * s)))
            push((a, b_, d_), col)
            push((a, d_, c), col)
        # 四坡顶
        for k in range(4):
            a, b_ = hi[k], hi[(k + 1) % 4]
            tri = (a, b_, ridge)
            nrm = cross((b_[0] - a[0], b_[1] - a[1], b_[2] - a[2]),
                        (ridge[0] - a[0], ridge[1] - a[1], ridge[2] - a[2]))
            if dot(nrm, (C[0] - a[0], C[1] - a[1], C[2] - a[2])) <= 0:
                continue
            s = min(1.25, 0.55 + 0.60 * max(0.0, dot(norm(nrm), LO)))
            col = (min(255, int(ROOF[0] * s)), min(255, int(ROOF[1] * s)), min(255, int(ROOF[2] * s)))
            push(tri, col)

    print(f"  透视: 可见面 {len(faces)}  收集耗时 {time.time()-t0:.1f}s")

    faces.sort(key=lambda t: -t[0])

    # 天空渐变
    im = Image.new("RGB", (SW, SH), (0, 0, 0))
    d = ImageDraw.Draw(im)
    # 天空渐变：三段插值，接缝处连续，避免出现横向硬边
    horizon = int(SH * 0.55)
    TOPC = (14, 18, 32)
    MIDC = (40, 46, 66)
    BOTC = (22, 21, 24)
    for y in range(SH):
        if y < horizon:
            t = y / float(max(1, horizon))
            col = (int(TOPC[0] + (MIDC[0] - TOPC[0]) * t),
                   int(TOPC[1] + (MIDC[1] - TOPC[1]) * t),
                   int(TOPC[2] + (MIDC[2] - TOPC[2]) * t))
        else:
            t = (y - horizon) / float(max(1, SH - horizon))
            col = (int(MIDC[0] + (BOTC[0] - MIDC[0]) * t),
                   int(MIDC[1] + (BOTC[1] - MIDC[1]) * t),
                   int(MIDC[2] + (BOTC[2] - MIDC[2]) * t))
        d.line([(0, y), (SW, y)], fill=col)

    for dep, pts, col in faces:
        d.polygon(pts, fill=col)

    im = im.resize((tw, th), Image.LANCZOS)
    im = im.filter(ImageFilter.UnsharpMask(radius=1.4, percent=45, threshold=2))
    im = ImageEnhance.Brightness(im).enhance(1.12)
    im.save(path)
    print(f"  已保存 {path}   总耗时 {time.time()-t0:.1f}s")
    print(f"  相机 C=({C[0]:.0f},{C[1]:.0f},{C[2]:.0f})  az={az_deg} el={el_deg} fov={fov_deg} ss={ss}")


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    meta, src = load()
    print(f"场景 {meta['extent_m']} m   网格 {meta['grid']}   建筑 {len(meta['buildings'])}")
    if what in ("top", "all"):
        render_top(meta, src, f"{OUT}\\preview_top.png")
    if what in ("persp", "all"):
        print("[透视]")
        render_persp(meta, src, f"{OUT}\\preview_persp.png")
    if what in ("iso", "all"):
        print("[等轴测]")
        render_persp(meta, src, f"{OUT}\\preview_iso.png",
                     az_deg=-118.0, el_deg=44.0, fov_deg=30.0)


if __name__ == "__main__":
    main()
