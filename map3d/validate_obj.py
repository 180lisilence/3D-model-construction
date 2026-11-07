"""validate_obj.py — 校验导出的 OBJ 结构是否合法。

检查项
  * v / vt / f 计数
  * 面索引是否越界（最常见的导出 bug）
  * 三角面退化（三点共线 / 面积为 0）
  * 法线方向是否朝外（地形应朝 +Z）
  * 包围盒
"""
import math
import sys

path = sys.argv[1] if len(sys.argv) > 1 else r"E:\workaddress\31\map3d\miramar.obj"

verts = []
uvs = []
groups = {}
cur_mat = None
cur_faces = None

with open(path, encoding="utf-8") as f:
    for ln, line in enumerate(f, 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        t = line.split()
        if t[0] == "v":
            verts.append((float(t[1]), float(t[2]), float(t[3])))
        elif t[0] == "vt":
            uvs.append((float(t[1]), float(t[2])))
        elif t[0] == "usemtl":
            cur_mat = t[1]
            groups.setdefault(cur_mat, [])
            cur_faces = groups[cur_mat]
        elif t[0] == "f":
            idx = []
            for tok in t[1:]:
                idx.append(int(tok.split("/")[0]))
            if cur_faces is None:
                groups.setdefault("_default", [])
                cur_faces = groups["_default"]
            cur_faces.append(idx)

print(f"文件 {path}")
print(f"  顶点 v  : {len(verts)}")
print(f"  UV   vt : {len(uvs)}")
tot = 0
for mat, faces in groups.items():
    tris = sum(1 for fc in faces if len(fc) == 3)
    quads = sum(1 for fc in faces if len(fc) == 4)
    others = sum(1 for fc in faces if len(fc) not in (3, 4))
    tot += len(faces)
    print(f"  材质 {mat:8s} 面 {len(faces):6d}  (三角 {tris}, 四边形 {quads}, 其他 {others})")
print(f"  面总计   : {tot}")

# ---- 索引越界 ----
bad = 0
for mat, faces in groups.items():
    for fc in faces:
        for i in fc:
            if i < 1 or i > len(verts):
                bad += 1
print(f"\n[索引越界] {bad} 处 {'OK' if bad == 0 else '<-- 异常'}")

# ---- 包围盒 ----
xs = [v[0] for v in verts]
ys = [v[1] for v in verts]
zs = [v[2] for v in verts]
print(f"[包围盒] X {min(xs):.1f}..{max(xs):.1f}   "
      f"Y {min(ys):.1f}..{max(ys):.1f}   Z {min(zs):.1f}..{max(zs):.1f}")

# ---- 退化面 & 法线朝向（按材质分别统计）----
deg = 0
orient = {}
for mat, faces in groups.items():
    up = down = flat = 0
    for fc in faces:
        if len(fc) != 3:
            continue
        a, b, c = verts[fc[0] - 1], verts[fc[1] - 1], verts[fc[2] - 1]
        u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        v = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        nx = u[1] * v[2] - u[2] * v[1]
        ny = u[2] * v[0] - u[0] * v[2]
        nz = u[0] * v[1] - u[1] * v[0]
        L = math.sqrt(nx * nx + ny * ny + nz * nz)
        if L < 1e-9:
            deg += 1
            continue
        nzs = nz / L
        if nzs > 0.25:
            up += 1
        elif nzs < -0.25:
            down += 1
        else:
            flat += 1
    orient[mat] = (up, down, flat)

print(f"\n[退化面] {deg} 个 {'OK' if deg == 0 else '<-- 异常'}")
print("[法线朝向]  材质      朝上   朝下   竖直")
for mat, (up, down, flat) in orient.items():
    print(f"            {mat:8s} {up:6d} {down:6d} {flat:6d}")

ok = (bad == 0 and deg == 0
      and orient.get("earth", (0, 1, 0))[1] == 0    # 地形不能翻面
      and orient.get("roof", (0, 0, 0))[1] == 0)    # 屋顶不能朝下
print(f"\n结论: {'通过' if ok else '存在问题'}")
