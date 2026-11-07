#pragma once

// ============================================================================
// mock/SoftRender.h
//
// 软件 3D 光栅化器。
//
// 原 MockRender 只会画线（Bresenham），没有深度概念 —— 那是「取证」的形状，
// 不是「3D 软件」的形状。这里补上一条完整的光栅化管线：
//
//   世界坐标
//     -> 相机空间（右/上/前三个基向量点乘，Z-up 左手的 UE 约定）
//     -> 近平面裁剪（Sutherland-Hodgman，只裁 z）
//     -> 屏幕空间 + 1/z
//     -> 深度测试 + 填充
//
// 三个关键决定：
//
// 1) 用 1/z 而不用 z 做深度。透视投影下 1/z 在屏幕空间是线性函数，
//    所以每个三角形可以预解出一个平面 invz(x,y) = a*x + b*y + c，
//    内循环只剩两次加法。换成插值 z 就必须逐像素除法，1080p 直接卡死。
//
// 2) 不做背面剔除。PhysX 三角网的绕序来自游戏内编译期数据，
//    我们没有任何理由相信它一致；剔除一旦判反，模型就变成空壳。
//    改为「法线朝相机翻转」的双面着色，代价是看不到内壁，但绝不会破洞。
//
// 3) 平面着色（per-triangle）而不是 Gouraud。物理网格本来就是拼出来的
//    多面体，平滑法线会让箱子和墙面互相糊在一起；保持每个三角形一个
//    亮度，反而让结构更易读 —— 也是这类技术可视化该有的质感。
// ============================================================================

#include "WinPre.h"
#include "math/math.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace soft3d
{
    // ------------------------------------------------------------------
    // 颜色
    // ------------------------------------------------------------------
    inline int Clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

    inline uint32_t PackRGB(int r, int g, int b)
    {
        return 0xFF000000u
             | ((uint32_t)Clamp255(r) << 16)
             | ((uint32_t)Clamp255(g) << 8)
             | (uint32_t)Clamp255(b);
    }

    inline uint32_t PackF(float r, float g, float b)
    {
        return PackRGB((int)(r * 255.0f + 0.5f),
                       (int)(g * 255.0f + 0.5f),
                       (int)(b * 255.0f + 0.5f));
    }

    inline void Unpack(uint32_t c, int& r, int& g, int& b)
    {
        r = (c >> 16) & 0xFF;
        g = (c >> 8) & 0xFF;
        b = c & 0xFF;
    }

    inline uint32_t LerpColor(uint32_t a, uint32_t b, float t)
    {
        int ar, ag, ab, br, bg, bb;
        Unpack(a, ar, ag, ab);
        Unpack(b, br, bg, bb);
        return PackRGB(
            (int)(ar + (br - ar) * t),
            (int)(ag + (bg - ag) * t),
            (int)(ab + (bb - ab) * t));
    }

    inline uint32_t ScaleColor(uint32_t c, float k)
    {
        int r, g, b;
        Unpack(c, r, g, b);
        return PackRGB((int)(r * k), (int)(g * k), (int)(b * k));
    }

    // ------------------------------------------------------------------
    // 帧缓冲
    // ------------------------------------------------------------------
    struct Framebuffer
    {
        int w = 0, h = 0;
        std::vector<uint32_t> color;

        // 深度：存 1/z，值越大越靠近相机；0 表示该像素还没有任何东西写入。
        // 因为 z >= zNear > 0，1/z 恒为正，用 0 当「远平面」是安全的。
        std::vector<float> invz;

        void Resize(int W, int H)
        {
            if (W <= 0 || H <= 0) return;
            if (W == w && H == h && !color.empty()) return;
            w = W;
            h = H;
            color.assign((size_t)w * h, 0xFF000000u);
            invz.assign((size_t)w * h, 0.0f);
        }

        inline void ClearDepth()
        {
            std::fill(invz.begin(), invz.end(), 0.0f);
        }

        // 竖直渐变背景。dist 参数让地平线位置随俯仰变化，看起来才像天空。
        void Gradient(uint32_t top, uint32_t mid, uint32_t bottom, float horizon)
        {
            if (w <= 0 || h <= 0) return;
            horizon = std::min(1.0f, std::max(0.0f, horizon));

            const int hz = (int)(horizon * (h - 1));

            for (int y = 0; y < h; y++)
            {
                uint32_t c;
                if (y <= hz && hz > 0)
                    c = LerpColor(top, mid, (float)y / (float)hz);
                else if (hz < h - 1)
                    c = LerpColor(mid, bottom, (float)(y - hz) / (float)(h - 1 - hz));
                else
                    c = mid;

                uint32_t* row = &color[(size_t)y * w];
                for (int x = 0; x < w; x++) row[x] = c;
            }
        }

        inline void Fill(uint32_t c)
        {
            std::fill(color.begin(), color.end(), c);
        }

        // 暗角。作用不是「好看」，而是把视线压回画面中心 ——
        // 满屏均匀亮度的 3D 图会显得廉价，因为人的视觉没有落点。
        void Vignette(float strength = 0.42f)
        {
            if (w <= 0 || h <= 0) return;
            const float hw = w * 0.5f, hh = h * 0.5f;
            const float invhw = 1.0f / hw, invhh = 1.0f / hh;

            for (int y = 0; y < h; y++)
            {
                const float ny = (y - hh) * invhh;
                uint32_t* row = &color[(size_t)y * w];
                for (int x = 0; x < w; x++)
                {
                    const float nx = (x - hw) * invhw;
                    // 稍微压扁成椭圆，宽屏才自然
                    float r2 = nx * nx * 0.55f + ny * ny * 0.75f;
                    if (r2 <= 0.0f) continue;
                    if (r2 > 1.0f) r2 = 1.0f;

                    const float k = 1.0f - strength * r2 * r2;
                    const uint32_t c = row[x];
                    const int r = (int)(((c >> 16) & 0xFF) * k);
                    const int g = (int)(((c >> 8) & 0xFF) * k);
                    const int b = (int)((c & 0xFF) * k);
                    row[x] = 0xFF000000u
                           | ((uint32_t)Clamp255(r) << 16)
                           | ((uint32_t)Clamp255(g) << 8)
                           | (uint32_t)Clamp255(b);
                }
            }
        }
    };

    // ------------------------------------------------------------------
    // 相机
    // ------------------------------------------------------------------
    struct Camera
    {
        Vector3 eye{ 0, 0, 0 };
        Vector3 target{ 0, 0, 0 };
        Vector3 worldUp{ 0, 0, 1 };      // UE 是 Z-up

        float fovY = 55.0f;              // 垂直视角（度）
        float zNear = 8.0f;

        // LookAt 之后有效
        Vector3 fwd, right, upv;
        float focal = 1.0f;
        int   vw = 0, vh = 0;
        float halfW = 0.0f, halfH = 0.0f;

        void LookAt(int W, int H)
        {
            vw = W;
            vh = H;
            halfW = W * 0.5f;
            halfH = H * 0.5f;

            Vector3 d = target - eye;
            float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (len < 1e-5f) { d = Vector3(0, 0, -1); len = 1.0f; }
            fwd = d * (1.0f / len);

            // right = up x fwd  （Z-up 下 fwd=+X 得到 right=+Y，与 UE 一致）
            right = worldUp.cross(fwd);
            float rl = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
            if (rl < 1e-5f)
            {
                // 视线与上方向平行（垂直俯视），换一个参考轴
                right = Vector3(1, 0, 0).cross(fwd);
                rl = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
                if (rl < 1e-5f) right = Vector3(1, 0, 0), rl = 1.0f;
            }
            right = right * (1.0f / rl);

            upv = fwd.cross(right);

            const float rad = fovY * 3.14159265358979323846f / 180.0f;
            focal = 1.0f / std::tan(rad * 0.5f);
        }

        // 相机空间：x 右、y 上、z 前（不是 OpenGL 的 -z，少一次取反）
        inline void ToView(const Vector3& p, float& xc, float& yc, float& zc) const
        {
            const float dx = p.x - eye.x;
            const float dy = p.y - eye.y;
            const float dz = p.z - eye.z;
            xc = dx * right.x + dy * right.y + dz * right.z;
            yc = dx * upv.x + dy * upv.y + dz * upv.z;
            zc = dx * fwd.x + dy * fwd.y + dz * fwd.z;
        }

        // 世界点 -> 屏幕。返回 false 表示在近平面之后。
        inline bool Project(const Vector3& p, float& sx, float& sy, float& invz) const
        {
            float xc, yc, zc;
            ToView(p, xc, yc, zc);
            if (zc < zNear) return false;

            invz = 1.0f / zc;
            const float k = focal * halfH;
            sx = halfW + xc * invz * k;
            sy = halfH - yc * invz * k;
            return true;
        }

        // 直接投到「已经算好 1/z」的形式，供裁剪后使用
        inline void ProjectWithInvz(float xc, float yc, float zc,
                                    float& sx, float& sy) const
        {
            const float inv = 1.0f / zc;
            const float k = focal * halfH;
            sx = halfW + xc * inv * k;
            sy = halfH - yc * inv * k;
        }
    };

    // ------------------------------------------------------------------
    // 三角形填充
    // ------------------------------------------------------------------
    struct Tri
    {
        float x[3];
        float y[3];
        float iz[3];
    };

    inline void RasterTriangle(Framebuffer& fb, const Tri& t, uint32_t color, int alpha = 255)
    {
        const float x0 = t.x[0], y0 = t.y[0];
        const float x1 = t.x[1], y1 = t.y[1];
        const float x2 = t.x[2], y2 = t.y[2];

        // 有向面积（叉积的两倍）。判定绕序 + 判断退化。
        const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (!std::isfinite(area) || std::fabs(area) < 1e-7f) return;

        // --- 包围盒 ---
        float fxmin = std::min(x0, std::min(x1, x2));
        float fxmax = std::max(x0, std::max(x1, x2));
        float fymin = std::min(y0, std::min(y1, y2));
        float fymax = std::max(y0, std::max(y1, y2));

        // 投影后的坐标可能极大（三角形横穿近平面），提前夹住
        const float LIMIT = 1e6f;
        if (!std::isfinite(fxmin) || !std::isfinite(fxmax) ||
            !std::isfinite(fymin) || !std::isfinite(fymax)) return;
        fxmin = std::max(fxmin, -LIMIT);
        fxmax = std::min(fxmax, LIMIT);
        fymin = std::max(fymin, -LIMIT);
        fymax = std::min(fymax, LIMIT);

        int x0i = std::max(0, (int)std::floor(fxmin));
        int x1i = std::min(fb.w - 1, (int)std::ceil(fxmax));
        int y0i = std::max(0, (int)std::floor(fymin));
        int y1i = std::min(fb.h - 1, (int)std::ceil(fymax));
        if (x0i > x1i || y0i > y1i) return;

        // --- 三条边的直线方程 w = A*x + B*y + C ---
        // w0 对应边 v1->v2，w1 对应 v2->v0，w2 对应 v0->v1
        const float A0 = y1 - y2, B0 = x2 - x1, C0 = x1 * y2 - x2 * y1;
        const float A1 = y2 - y0, B1 = x0 - x2, C1 = x2 * y0 - x0 * y2;
        const float A2 = y0 - y1, B2 = x1 - x0, C2 = x0 * y1 - x1 * y0;

        const float sign = (area > 0.0f) ? 1.0f : -1.0f;

        // --- 深度平面：invz(x,y) = Da*x + Db*y + Dc ---
        float Da = 0.0f, Db = 0.0f, Dc = 0.0f;
        {
            const float det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            if (std::fabs(det) > 1e-9f)
            {
                const float iz0 = t.iz[0], iz1 = t.iz[1], iz2 = t.iz[2];
                Da = ((iz1 - iz0) * (y2 - y0) - (iz2 - iz0) * (y1 - y0)) / det;
                Db = ((iz2 - iz0) * (x1 - x0) - (iz1 - iz0) * (x2 - x0)) / det;
                Dc = iz0 - Da * x0 - Db * y0;
            }
            else
            {
                return;
            }
        }

        uint32_t* cbuf = fb.color.data();
        float*    zbuf = fb.invz.data();

        for (int py = y0i; py <= y1i; py++)
        {
            const float fy = (float)py + 0.5f;

            // 逐行起点
            const float px0 = (float)x0i + 0.5f;
            float w0 = A0 * px0 + B0 * fy + C0;
            float w1 = A1 * px0 + B1 * fy + C1;
            float w2 = A2 * px0 + B2 * fy + C2;
            float iz = Da * px0 + Db * fy + Dc;

            uint32_t* row = cbuf + (size_t)py * fb.w;
            float*    zrow = zbuf + (size_t)py * fb.w;

            for (int px = x0i; px <= x1i; px++)
            {
                // 三个边函数同号（按 sign 归一化后全非负）才在三角形内
                if ((w0 * sign) >= 0.0f && (w1 * sign) >= 0.0f && (w2 * sign) >= 0.0f)
                {
                    if (iz > zrow[px])
                    {
                        zrow[px] = iz;

                        if (alpha >= 255)
                        {
                            row[px] = color;
                        }
                        else
                        {
                            // 半透明（接触阴影用）。深度仍然写满，
                            // 这样阴影不会因为自己的边缘互相重复叠深。
                            const uint32_t dst = row[px];
                            const int ia = 255 - alpha;
                            const int sr = (color >> 16) & 0xFF;
                            const int sg = (color >> 8) & 0xFF;
                            const int sb = color & 0xFF;
                            const int dr = (dst >> 16) & 0xFF;
                            const int dg = (dst >> 8) & 0xFF;
                            const int db = dst & 0xFF;
                            const int nr = (sr * alpha + dr * ia) / 255;
                            const int ng = (sg * alpha + dg * ia) / 255;
                            const int nb = (sb * alpha + db * ia) / 255;
                            row[px] = 0xFF000000u
                                    | ((uint32_t)nr << 16)
                                    | ((uint32_t)ng << 8)
                                    | (uint32_t)nb;
                        }
                    }
                }
                w0 += A0; w1 += A1; w2 += A2; iz += Da;
            }
        }
    }

    // ------------------------------------------------------------------
    // 3D 线段（深度测试，供网格线 / 坐标轴 / 高亮框使用）
    // ------------------------------------------------------------------
    inline void Line3D(Framebuffer& fb, const Camera& cam,
                       const Vector3& a, const Vector3& b,
                       uint32_t color, float depthBias = 1.0f,
                       int width = 1)
    {
        float xc0, yc0, zc0, xc1, yc1, zc1;
        cam.ToView(a, xc0, yc0, zc0);
        cam.ToView(b, xc1, yc1, zc1);

        // 近平面裁剪（只处理一段线段）
        const float zn = cam.zNear;
        if (zc0 < zn && zc1 < zn) return;
        if (zc0 < zn)
        {
            const float t = (zn - zc0) / (zc1 - zc0);
            xc0 += (xc1 - xc0) * t;
            yc0 += (yc1 - yc0) * t;
            zc0 = zn;
        }
        else if (zc1 < zn)
        {
            const float t = (zn - zc1) / (zc0 - zc1);
            xc1 += (xc0 - xc1) * t;
            yc1 += (yc0 - yc1) * t;
            zc1 = zn;
        }

        float sx0, sy0, sx1, sy1;
        cam.ProjectWithInvz(xc0, yc0, zc0, sx0, sy0);
        cam.ProjectWithInvz(xc1, yc1, zc1, sx1, sy1);

        if (!std::isfinite(sx0) || !std::isfinite(sy0) ||
            !std::isfinite(sx1) || !std::isfinite(sy1)) return;

        // 完全在屏幕外就丢（避免巨大的 DDA 循环）
        const int SW = fb.w, SH = fb.h;
        if ((sx0 < -50 && sx1 < -50) || (sx0 > SW + 50 && sx1 > SW + 50) ||
            (sy0 < -50 && sy1 < -50) || (sy0 > SH + 50 && sy1 > SH + 50)) return;

        const float iz0 = 1.0f / zc0;
        const float iz1 = 1.0f / zc1;

        const float dx = sx1 - sx0, dy = sy1 - sy0;
        const int steps = (int)std::max(std::fabs(dx), std::fabs(dy)) + 1;
        if (steps <= 0 || steps > 20000) return;

        const float dIz = (iz1 - iz0) / (float)steps;

        float sx = sx0, sy = sy0, iz = iz0;
        for (int i = 0; i <= steps; i++)
        {
            const int px = (int)(sx + 0.5f);
            const int py = (int)(sy + 0.5f);

            for (int oy = 0; oy < width; oy++)
            {
                const int yy = py + oy;
                if (px < 0 || yy < 0 || px >= SW || yy >= SH) continue;
                const size_t idx = (size_t)yy * SW + px;
                if (iz * depthBias > fb.invz[idx])
                {
                    fb.invz[idx] = iz * depthBias;
                    fb.color[idx] = color;
                }
            }

            sx += dx / (float)steps;
            sy += dy / (float)steps;
            iz += dIz;
        }
    }

    // 屏幕空间线段（端点已投影、带 1/z），用于三角形描边。
    // 单独于 Line3D 存在，是因为填充后的描边只需要屏幕坐标，
    // 不必把世界坐标再投一遍 —— 一根边要投两次是纯浪费。
    inline void LineScreen(Framebuffer& fb, float x0, float y0, float iz0,
                           float x1, float y1, float iz1,
                           uint32_t color, float depthBias = 1.0015f)
    {
        if (!std::isfinite(x0) || !std::isfinite(y0) ||
            !std::isfinite(x1) || !std::isfinite(y1)) return;

        const float dx = x1 - x0, dy = y1 - y0;
        const int steps = (int)std::max(std::fabs(dx), std::fabs(dy)) + 1;
        if (steps <= 0 || steps > 8000) return;

        const float sxStep = dx / (float)steps;
        const float syStep = dy / (float)steps;
        const float izStep = (iz1 - iz0) / (float)steps;

        float sx = x0, sy = y0, iz = iz0;
        const int SW = fb.w, SH = fb.h;

        for (int i = 0; i <= steps; i++)
        {
            const int px = (int)(sx + 0.5f);
            const int py = (int)(sy + 0.5f);
            if (px >= 0 && py >= 0 && px < SW && py < SH)
            {
                const size_t idx = (size_t)py * SW + px;
                const float z = iz * depthBias;
                if (z > fb.invz[idx])
                {
                    fb.invz[idx] = z;
                    fb.color[idx] = color;
                }
            }
            sx += sxStep;
            sy += syStep;
            iz += izStep;
        }
    }

    // ------------------------------------------------------------------
    // 光照
    // ------------------------------------------------------------------
    struct Shading
    {
        // 两盏方向光 + 半球环境光。Z-up，所以「上」是 +Z。
        Vector3 keyDir{ 0.42f, -0.55f, 0.72f };      // 主光：右前上
        Vector3 fillDir{ -0.62f, 0.45f, 0.35f };     // 补光：左后上（较暗）

        float ambient = 0.20f;
        float keyStrength = 1.18f;
        float fillStrength = 0.30f;
        float rimStrength = 0.16f;

        // 头灯：从相机方向补的那盏。它不是物理，是可视化手段 ——
        // 一个四面朝内的房间（墙的可见面全背着主光）如果没有头灯，
        // 内部会黑成一团，什么结构都看不出来。
        float headlight = 0.30f;

        // 雾：让远处的几何淡出到背景，制造纵深
        float fogStart = 1700.0f;
        float fogEnd = 8200.0f;
        uint32_t fogColor = 0xFF0A0F1E;

        void Normalize()
        {
            auto nrm = [](Vector3& v)
            {
                const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
                if (l > 1e-6f) { v.x /= l; v.y /= l; v.z /= l; }
            };
            nrm(keyDir);
            nrm(fillDir);
        }

        // base: 材质色。n: 世界法线（已双面定向）。toEye: 从表面指向相机（单位）。
        uint32_t Evaluate(uint32_t base, const Vector3& n, const Vector3& toEye,
                          float distance) const
        {
            float ndl = n.x * keyDir.x + n.y * keyDir.y + n.z * keyDir.z;
            if (ndl < 0.0f) ndl = 0.0f;

            float ndf = n.x * fillDir.x + n.y * fillDir.y + n.z * fillDir.z;
            if (ndf < 0.0f) ndf = 0.0f;

            // 半球环境光：朝上的面偏亮，朝下的面偏暗
            const float hemi = 0.5f + 0.5f * n.z;

            const float ndv0 = n.x * toEye.x + n.y * toEye.y + n.z * toEye.z;
            const float head = (ndv0 > 0.0f) ? ndv0 : 0.0f;

            float lum = ambient * (0.55f + 0.75f * hemi)
                      + keyStrength * ndl
                      + fillStrength * ndf
                      + headlight * head;

            int r, g, b;
            Unpack(base, r, g, b);

            float fr = (float)r * lum;
            float fg = (float)g * lum;
            float fb2 = (float)b * lum;

            // 边缘光：掠射角提亮一点，让轮廓从背景里分离出来
            if (rimStrength > 0.0f)
            {
                float ndv = n.x * toEye.x + n.y * toEye.y + n.z * toEye.z;
                if (ndv < 0.0f) ndv = 0.0f;
                const float rim = std::pow(1.0f - ndv, 4.0f) * rimStrength;
                fr += 255.0f * rim * 0.6f;
                fg += 255.0f * rim * 0.85f;
                fb2 += 255.0f * rim * 1.0f;
            }

            // 雾
            if (fogEnd > fogStart && distance > fogStart)
            {
                float f = (distance - fogStart) / (fogEnd - fogStart);
                if (f > 1.0f) f = 1.0f;
                f *= 0.85f;
                int fogR, fogG, fogB;
                Unpack(fogColor, fogR, fogG, fogB);
                fr = fr + (fogR - fr) * f;
                fg = fg + (fogG - fg) * f;
                fb2 = fb2 + (fogB - fb2) * f;
            }

            return PackRGB((int)fr, (int)fg, (int)fb2);
        }
    };

    // ------------------------------------------------------------------
    // 近平面裁剪：把相机空间的三角形切成 0..2 个
    // ------------------------------------------------------------------
    struct CVert { float x, y, z; };

    // 返回裁剪后的顶点数（0..4），写入 out
    inline int ClipNear(const CVert in[3], CVert out[4], float zNear)
    {
        int n = 0;
        for (int i = 0; i < 3; i++)
        {
            const CVert& a = in[i];
            const CVert& b = in[(i + 1) % 3];
            const bool ain = a.z >= zNear;
            const bool bin = b.z >= zNear;

            if (ain) out[n++] = a;

            if (ain != bin)
            {
                const float t = (zNear - a.z) / (b.z - a.z);
                CVert v;
                v.x = a.x + (b.x - a.x) * t;
                v.y = a.y + (b.y - a.y) * t;
                v.z = zNear;
                out[n++] = v;
            }
            if (n >= 4) break;
        }
        return n;
    }

} // namespace soft3d
