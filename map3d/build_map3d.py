"""
build_map3d.py — 从 PUBG 地图俯视截图重建 3D 模型，输出标准 OBJ。

原理
----
输入是一张正交俯视图。要变成 3D，需要三样东西：
  1. 平面位置  -> 像素坐标直接线性映射到世界 XY（比例尺由网格标定）
  2. 高度      -> 用亮度作海拔代理（沙漠里亮=砂石高地，暗=沟谷/植被洼地）
  3. 建筑      -> 屋顶是"亮且暖"的规则块，检测出 bbox 后挤出成盒体

两个关键坑
----------
* 坑 1：屋顶本身很亮。若直接拿亮度算高度，每栋房子都会隆起成一座山，
  房子再叠在上面就变成"悬空楼阁"。必须先做 mask + 迭代扩散填补（inpaint），
  把屋顶和 UI 图形从灰度图里抹掉，再算高度。
* 坑 2：房屋地基必须是平的。算出高度场后要按建筑 bbox 逐个压平到
  该 bbox 外围一圈地面的中位高度，否则房子会半陷进坡里。

比例尺
------
网格间距实测精确为 108px（竖线 34/142/250/358/466/574，横线 108/216/324/432，
零误差）。PUBG 地图一格通常代表 100m，故 1px = 0.9259m。
这个绝对比例无法从单张截图严格证实，属书面假设，可由 --cell-m 覆盖。

用法
----
    python build_map3d.py                # 默认参数
    python build_map3d.py --cell-m 100   # 改比例尺
    python build_map3d.py --step 6       # 更粗的地形网格
"""
import argparse
import json
import math
import os
import shutil
from PIL import Image, ImageFilter

# --------------------------------------------------------------------------
# 默认参数
# --------------------------------------------------------------------------
DEFAULT_SRC = r"C:\Users\19513\.workbuddy\clipboard-images\clipboard-2026-10-23T16-54-04-126Z-eab95c49.png"
DEFAULT_OUT = r"E:\workaddress\31\map3d"
# 剪贴板目录会被系统清理，本地常留一份副本作回退
LOCAL_TEX = r"E:\workaddress\31\map3d\miramar_tex.png"

# 网格：108px 一格，来自实测
CELL_PX = 108.0

# 建筑检测：亮度 + 暖色饱和度双判据
ROOF_MIN_LUM = 145      # 屋顶最暗也在这个之上（沙地约 70）
ROOF_SAT_LO = 18        # 纯白 UI 图标 sat≈0，靠下界排除
ROOF_SAT_HI = 80        # 地面为灰绿/灰棕，sat 也低；上界防过饱和

# 建筑只能在已知聚落范围内找 —— 否则沙漠里的亮砂地会被当成房子。
# 坐标来自肉眼核对裁剪图：中央 2x2 房群 / 右上聚落 / 底部村庄 / 左下角
BUILDING_ROIS = [
    ("center", 100, 275, 200, 355),
    ("right",  415,   0, 515,  65),
    ("bottom", 165, 435, 340, 481),
    ("corner",   0, 455,  58, 481),
]

MIN_BLD_AREA = 150      # 连通域最小像素数
MIN_BLD_SIDE = 8        # 最小边长
MAX_BLD_SIDE = 95       # 最大边长（超过就是连片砂地误检）

# 高度映射
H_LARGE = 0.55          # 大尺度海拔系数（灰度偏离均值 -> 米）
H_FINE = 0.40           # 细节起伏系数（突出沟壑）
BLUR_LARGE = 10         # 低分辨率图上的大尺度模糊半径
BLUR_MID = 2.5

# 建筑
BLD_H_RATIO = 0.30      # 高度 = 宽度 * 该比例
BLD_H_MIN = 6.0
BLD_H_MAX = 15.0
BLD_BASE_SINK = 3.0     # 底部向下多延伸，防止坡地上露缝
ROOF_INSET = 0.35       # 屋顶收进，做出檐口
RIDGE_RATIO = 0.16      # 四坡顶脊高 = 建筑高 * 该比例（与 render_preview 保持一致）


# --------------------------------------------------------------------------
# 图像工具（纯 PIL，不依赖 numpy）
# --------------------------------------------------------------------------
def to_gray(px, W, H):
    """返回灰度二维列表（0..255 整数）。"""
    g = [[0] * W for _ in range(H)]
    for y in range(H):
        row = g[y]
        for x in range(W):
            r, gg, b = px[x, y]
            row[x] = (r * 299 + gg * 587 + b * 114) // 1000
    return g


def build_roof_mask(px, W, H):
    """亮 + 暖色 -> 屋顶候选。返回 (mask, stats)。"""
    mask = [[False] * W for _ in range(H)]
    hit = 0
    for y in range(H):
        mrow = mask[y]
        for x in range(W):
            r, g, b = px[x, y]
            mx = max(r, g, b)
            sat = mx - min(r, g, b)
            if mx > ROOF_MIN_LUM and ROOF_SAT_LO <= sat <= ROOF_SAT_HI:
                mrow[x] = True
                hit += 1
    return mask, hit


def dilate(mask, W, H, r=1):
    """方框膨胀。"""
    out = [[False] * W for _ in range(H)]
    for y in range(H):
        orow = out[y]
        for x in range(W):
            if not mask[y][x]:
                continue
            for dy in range(-r, r + 1):
                yy = y + dy
                if yy < 0 or yy >= H:
                    continue
                nrow = out[yy]
                for dx in range(-r, r + 1):
                    xx = x + dx
                    if 0 <= xx < W:
                        nrow[xx] = True
    return out


# --------------------------------------------------------------------------
# 建筑检测
# --------------------------------------------------------------------------
def components(mask, W, H, x0, y0, x1, y1, min_area):
    """在 ROI 内做 8 邻域连通域，返回 [(area, minx, miny, maxx, maxy)]。"""
    seen = [[False] * W for _ in range(H)]
    comps = []
    for sy in range(max(0, y0), min(H, y1)):
        for sx in range(max(0, x0), min(W, x1)):
            if not mask[sy][sx] or seen[sy][sx]:
                continue
            stack = [(sx, sy)]
            seen[sy][sx] = True
            minx = maxx = sx
            miny = maxy = sy
            cnt = 0
            while stack:
                x, y = stack.pop()
                cnt += 1
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y
                for dy in (-1, 0, 1):
                    yy = y + dy
                    if yy < 0 or yy >= H:
                        continue
                    for dx in (-1, 0, 1):
                        xx = x + dx
                        if xx < 0 or xx >= W:
                            continue
                        if mask[yy][xx] and not seen[yy][xx]:
                            seen[yy][xx] = True
                            stack.append((xx, yy))
            if cnt >= min_area:
                comps.append((cnt, minx, miny, maxx, maxy))
    return comps


def detect_buildings(px, W, H):
    """检测建筑体块。返回 [{'roi','area','box','w','h','px_w','px_h'}]。"""
    raw, hit = build_roof_mask(px, W, H)
    print(f"  屋顶候选像素 {hit}（{100.0*hit/(W*H):.1f}%）")
    mask = dilate(raw, W, H, 1)

    out = []
    for name, x0, y0, x1, y1 in BUILDING_ROIS:
        comps = components(mask, W, H, x0, y0, x1, y1, MIN_BLD_AREA)
        comps.sort(key=lambda c: -c[0])
        kept = 0
        for area, ax0, ay0, ax1, ay1 in comps:
            w = ax1 - ax0 + 1
            h = ay1 - ay0 + 1
            if not (MIN_BLD_SIDE <= w <= MAX_BLD_SIDE and MIN_BLD_SIDE <= h <= MAX_BLD_SIDE):
                continue
            out.append({
                "roi": name, "area": area,
                "box": [ax0, ay0, ax1, ay1],
                "px_w": w, "px_h": h,
                "fill": round(100.0 * area / (w * h), 1),
            })
            kept += 1
        print(f"  [{name}] 体块 {kept} 个")
    return out


# --------------------------------------------------------------------------
# 高度场
# --------------------------------------------------------------------------
def build_heightfield(gray, W, H, step, block_mask):
    """由灰度推高度场。block_mask 为 True 的点先做扩散填补。

    返回 (h, GX, GY)，h[j][i] 单位米。
    """
    GX = (W + step - 1) // step + 1
    GY = (H + step - 1) // step + 1

    # 降采样到网格
    gl = [[0.0] * GX for _ in range(GY)]
    ml = [[False] * GX for _ in range(GY)]
    for j in range(GY):
        y = min(H - 1, j * step)
        for i in range(GX):
            x = min(W - 1, i * step)
            gl[j][i] = float(gray[y][x])
            ml[j][i] = block_mask[y][x]

    nblock = sum(1 for j in range(GY) for i in range(GX) if ml[j][i])
    print(f"  待填补格子 {nblock} / {GX*GY}")

    # 迭代扩散：每轮把边缘一圈填掉，由外向内渗透
    for it in range(60):
        updates = []
        for j in range(GY):
            for i in range(GX):
                if not ml[j][i]:
                    continue
                s = 0.0
                n = 0
                for dj in (-1, 0, 1):
                    jj = j + dj
                    if jj < 0 or jj >= GY:
                        continue
                    for di in (-1, 0, 1):
                        ii = i + di
                        if ii < 0 or ii >= GX:
                            continue
                        if not ml[jj][ii]:
                            s += gl[jj][ii]
                            n += 1
                if n:
                    updates.append((j, i, s / n))
        if not updates:
            break
        for j, i, v in updates:
            gl[j][i] = v
            ml[j][i] = False

    # 多尺度：大尺度当海拔，中尺度减大尺度当细节（沟壑在这里是负值）
    img = Image.new("L", (GX, GY))
    img.putdata([max(0, min(255, int(round(gl[j][i])))) for j in range(GY) for i in range(GX)])
    large = list(img.filter(ImageFilter.GaussianBlur(BLUR_LARGE)).getdata())
    mid = list(img.filter(ImageFilter.GaussianBlur(BLUR_MID)).getdata())

    mean_large = sum(large) / len(large)
    print(f"  大尺度灰度均值 {mean_large:.1f}")

    h = [[0.0] * GX for _ in range(GY)]
    for j in range(GY):
        for i in range(GX):
            k = j * GX + i
            h[j][i] = (large[k] - mean_large) * H_LARGE + (mid[k] - large[k]) * H_FINE

    flat = [h[j][i] for j in range(GY) for i in range(GX)]
    print(f"  高度范围 {min(flat):.1f} .. {max(flat):.1f} m")
    return h, GX, GY


def flatten_under_buildings(h, GX, GY, step, buildings):
    """把每栋建筑脚下压平到外围地面中位高度，返回压平后的 h。"""
    for b in buildings:
        bx0, by0, bx1, by1 = b["box"]
        i0 = max(0, bx0 // step)
        i1 = min(GX - 1, bx1 // step)
        j0 = max(0, by0 // step)
        j1 = min(GY - 1, by1 // step)
        # 采集外围一圈（bbox 外扩 2 格、内部排除）的地面高度
        ring = []
        for j in range(max(0, j0 - 2), min(GY, j1 + 3)):
            for i in range(max(0, i0 - 2), min(GX, i1 + 3)):
                if i0 <= i <= i1 and j0 <= j <= j1:
                    continue
                ring.append(h[j][i])
        if not ring:
            continue
        ring.sort()
        base = ring[len(ring) // 2]
        b["ground_z"] = round(base, 2)
        for j in range(j0, j1 + 1):
            for i in range(i0, i1 + 1):
                h[j][i] = base
    return h


# --------------------------------------------------------------------------
# OBJ 导出
# --------------------------------------------------------------------------
def box_side_faces(v0, v1, v2, v3, v4, v5, v6, v7):
    """盒体 4 个侧面（8 个三角），绕序统一为从外部看逆时针。

    顶点：0..3 底面(-,-)(+,-)(+,+)(-,+)，4..7 顶面同序。
    底面不导出（被地形埋住，看不见）；顶面由四坡屋顶替代。
    """
    return [
        (v0, v1, v5), (v0, v5, v4),   # -Y
        (v1, v2, v6), (v1, v6, v5),   # +X
        (v2, v3, v7), (v2, v7, v6),   # +Y
        (v3, v0, v4), (v3, v4, v7),   # -X
    ]


def export_obj(path, tex_name, h, GX, GY, step, buildings, W, H, px2m):
    verts = []      # (x,y,z)
    uvs = []        # (u,v)
    faces_earth = []   # 索引（1-based，稍后加偏移）
    faces_wall = []
    faces_roof = []

    # ---- 地形 ----
    for j in range(GY):
        for i in range(GX):
            x = i * step * px2m
            y = j * step * px2m
            verts.append((x, y, h[j][i]))
            u = (i * step) / float(W)
            v = 1.0 - (j * step) / float(H)
            uvs.append((min(1.0, u), max(0.0, v)))

    def vid(i, j):
        return j * GX + i + 1   # 1-based

    for j in range(GY - 1):
        for i in range(GX - 1):
            # 两个三角形，法线朝 +Z：(i,j)->(i+1,j+1)->(i,j+1)
            faces_earth.append((vid(i, j), vid(i + 1, j + 1), vid(i, j + 1)))
            faces_earth.append((vid(i, j), vid(i + 1, j), vid(i + 1, j + 1)))

    # ---- 建筑 ----
    bld_info = []
    for idx, b in enumerate(buildings):
        bx0, by0, bx1, by1 = b["box"]
        x0 = bx0 * px2m
        x1 = (bx1 + 1) * px2m
        y0 = by0 * px2m
        y1 = (by1 + 1) * px2m
        bw = x1 - x0
        bh = y1 - y0

        hgt = max(BLD_H_MIN, min(BLD_H_MAX, bw * BLD_H_RATIO))
        gz = b.get("ground_z", 0.0)
        z0 = gz - BLD_BASE_SINK
        z1 = gz + hgt

        base = len(verts) + 1
        # 底面 4 角
        for (px_, py_) in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
            verts.append((px_, py_, z0))
            uvs.append((0.0, 0.0))
        # 顶面 4 角（收进 ROOF_INSET）
        ix0, ix1 = x0 + ROOF_INSET, x1 - ROOF_INSET
        iy0, iy1 = y0 + ROOF_INSET, y1 - ROOF_INSET
        for (px_, py_) in ((ix0, iy0), (ix1, iy0), (ix1, iy1), (ix0, iy1)):
            verts.append((px_, py_, z1))
            uvs.append((0.0, 0.0))

        v0, v1_, v2, v3 = base, base + 1, base + 2, base + 3
        v4, v5, v6, v7 = base + 4, base + 5, base + 6, base + 7
        for tri in box_side_faces(v0, v1_, v2, v3, v4, v5, v6, v7):
            faces_wall.append(tri)

        # 四坡顶：顶面中心抬高一个脊点，4 个三角面。PUBG 沙漠房基本都是坡顶，
        # 平顶会让模型看起来像纸盒。
        verts.append(((ix0 + ix1) * 0.5, (iy0 + iy1) * 0.5, z1 + hgt * RIDGE_RATIO))
        uvs.append((0.0, 0.0))
        vr = len(verts)
        top4 = (v4, v5, v6, v7)
        for k in range(4):
            faces_roof.append((top4[k], top4[(k + 1) % 4], vr))

        bld_info.append({
            "id": idx, "roi": b["roi"],
            "box_px": [bx0, by0, bx1, by1],
            "size_m": [round(bw, 1), round(bh, 1)],
            "height_m": round(hgt, 1),
            "ground_z": round(gz, 2),
        })

    # ---- 写文件 ----
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# reconstructed from PUBG map screenshot\n")
        f.write(f"# grid: {CELL_PX:.0f}px per cell, 1px = {px2m:.4f} m\n")
        f.write(f"# extent: {W*px2m:.1f} x {H*px2m:.1f} m\n")
        f.write(f"# terrain verts: {GX*GY}, buildings: {len(buildings)}\n\n")
        f.write(f"mtllib {os.path.basename(path).replace('.obj', '.mtl')}\n\n")

        for (x, y, z) in verts:
            f.write(f"v {x:.3f} {y:.3f} {z:.3f}\n")
        for (u, v) in uvs:
            f.write(f"vt {u:.5f} {v:.5f}\n")

        f.write("\no terrain\nusemtl earth\ns 1\n")
        for (a, b_, c) in faces_earth:
            f.write(f"f {a}/{a} {b_}/{b_} {c}/{c}\n")

        if faces_wall:
            f.write("\no buildings_wall\nusemtl wall\ns off\n")
            for (a, b_, c) in faces_wall:
                f.write(f"f {a} {b_} {c}\n")

        if faces_roof:
            f.write("\no buildings_roof\nusemtl roof\ns off\n")
            for (a, b_, c) in faces_roof:
                f.write(f"f {a} {b_} {c}\n")

    # ---- MTL ----
    mtl = path.replace(".obj", ".mtl")
    with open(mtl, "w", encoding="utf-8", newline="\n") as f:
        f.write("newmtl earth\n")
        f.write("Ka 1.000 1.000 1.000\n")
        f.write("Kd 1.000 1.000 1.000\n")
        f.write("Ks 0.000 0.000 0.000\n")
        f.write(f"map_Kd {tex_name}\n\n")
        f.write("newmtl wall\n")
        f.write("Ka 0.300 0.280 0.240\n")
        f.write("Kd 0.720 0.680 0.580\n")
        f.write("Ks 0.000 0.000 0.000\n\n")
        f.write("newmtl roof\n")
        f.write("Ka 0.350 0.330 0.290\n")
        f.write("Kd 0.840 0.810 0.700\n")
        f.write("Ks 0.050 0.050 0.050\n")

    return {
        "verts": len(verts),
        "tris_earth": len(faces_earth),
        "tris_wall": len(faces_wall),
        "tris_roof": len(faces_roof),
        "buildings": bld_info,
    }


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=DEFAULT_SRC)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--cell-px", type=float, default=CELL_PX)
    ap.add_argument("--cell-m", type=float, default=100.0)
    ap.add_argument("--step", type=int, default=4)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    px2m = args.cell_m / args.cell_px

    src_path = args.src
    if not os.path.exists(src_path) and os.path.exists(LOCAL_TEX):
        src_path = LOCAL_TEX
        print(f"[提示] 原截图路径不存在，改用本地副本 {LOCAL_TEX}")

    print("=" * 62)
    print("build_map3d  —  从地图截图重建 3D 模型")
    print("=" * 62)

    im = Image.open(src_path).convert("RGB")
    W, H = im.size
    px = im.load()
    print(f"\n[1] 输入 {W}x{H}   比例 {args.cell_px:.0f}px = {args.cell_m:.0f}m")
    print(f"    1px = {px2m:.4f} m   场景范围 {W*px2m:.0f} x {H*px2m:.0f} m")

    print("\n[2] 灰度化")
    gray = to_gray(px, W, H)

    print("\n[3] 建筑检测")
    buildings = detect_buildings(px, W, H)
    print(f"    合计 {len(buildings)} 个体块")

    # 用于地形填补的 mask = 所有建筑 bbox 外扩 4px
    block = [[False] * W for _ in range(H)]
    for b in buildings:
        bx0, by0, bx1, by1 = b["box"]
        for y in range(max(0, by0 - 4), min(H, by1 + 5)):
            row = block[y]
            for x in range(max(0, bx0 - 4), min(W, bx1 + 5)):
                row[x] = True

    print("\n[4] 高度场（先剔除屋顶再算，避免房子隆起成山）")
    h, GX, GY = build_heightfield(gray, W, H, args.step, block)
    print(f"    网格 {GX} x {GY} = {GX*GY} 顶点")

    print("\n[5] 建筑地基压平")
    h = flatten_under_buildings(h, GX, GY, args.step, buildings)

    print("\n[6] 导出 OBJ")
    tex_name = "miramar_tex.png"
    obj_path = os.path.join(args.out, "miramar.obj")
    info = export_obj(obj_path, tex_name, h, GX, GY, args.step, buildings, W, H, px2m)
    shutil.copyfile(src_path, os.path.join(args.out, tex_name))

    size = os.path.getsize(obj_path)
    print(f"    {obj_path}  ({size/1024/1024:.2f} MB)")
    print(f"    顶点 {info['verts']}   地形面 {info['tris_earth']}   "
          f"墙面 {info['tris_wall']}   顶面 {info['tris_roof']}")

    # 记录中间数据，供预览/核对
    meta = {
        "src": src_path,
        "image_wh": [W, H],
        "cell_px": args.cell_px,
        "cell_m": args.cell_m,
        "px_to_m": round(px2m, 6),
        "extent_m": [round(W * px2m, 1), round(H * px2m, 1)],
        "grid": [GX, GY],
        "step_px": args.step,
        "terrain_z_range": [
            round(min(h[j][i] for j in range(GY) for i in range(GX)), 2),
            round(max(h[j][i] for j in range(GY) for i in range(GX)), 2),
        ],
        "heightfield": [[round(h[j][i], 3) for i in range(GX)] for j in range(GY)],
        "buildings": info["buildings"],
    }
    with open(os.path.join(args.out, "meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=1)

    print("\n[7] 建筑清单")
    print("   id roi      bbox(px)                尺寸(m)        高(m)  地面z")
    for b in info["buildings"]:
        bx0, by0, bx1, by1 = b["box_px"]
        print(f"   {b['id']:2d} {b['roi']:7s} ({bx0:3d},{by0:3d})-({bx1:3d},{by1:3d})  "
              f"{b['size_m'][0]:6.1f} x {b['size_m'][1]:5.1f}  {b['height_m']:5.1f}  {b['ground_z']:6.2f}")

    print("\n完成。")


if __name__ == "__main__":
    main()
