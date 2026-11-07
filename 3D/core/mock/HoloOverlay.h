#pragma once

// ============================================================================
// mock/HoloOverlay.h
//
// 桌面透明叠层专用的「线框全息」渲染器。
//
// 和 SoftRender.h 的分工：
//   SoftRender.h  实心光栅化（不透明帧缓冲，给独立窗口用）
//   HoloOverlay.h 透明叠层光栅化（预乘 alpha 帧缓冲，给桌面叠层用）
//
// 这一版相对最早的 Bresenham 线框，加了四件东西：
//
// 1) 深度预通道。先只写深度不写颜色，把整个场景的 1/z 存下来；
//    之后所有线段逐像素比较深度。于是背面边被正面遮住 —— 这是
//    隐藏线消除，线框才第一次读得出「前后」。没有这一步，
//    一个箱子 12 条边全部可见，看起来就是一堆交叉的线。
//
// 2) 特征边提取。每条边先看它相邻两个三角形的法线夹角：
//    夹角大 = 折痕边（箱子棱、墙角），画亮；夹角小 = 同面内的
//    三角剖分痕迹（一个矩形被切成两个三角形的那条对角线），画暗；
//    只属于一个三角形 = 边界边，画亮。三角形数不变，噪点少一半。
//
// 3) 亚像素抗锯齿。逐像素按覆盖率混合，而不是非 0 即 1。
//    细线在 1080p 上不再闪，也不用开 MSAA。
//
// 4) 距离雾 + 近端淡出。远的线变暗，贴脸的线不炸屏。
//
// 坐标系：直接复用 SoftRender.h 的 soft3d::Camera（Z-up 左手，UE 约定），
// 这样叠层和独立窗口两套渲染的视角完全一致，可以对拍。
// ============================================================================

#include "WinPre.h"

#include "MockRender.h"      // Canvas / OverlayWindow（DIB + UpdateLayeredWindow）
#include "SoftRender.h"      // soft3d::Camera / Tri / ClipNear / 颜色工具
#include "util/Data.h"       // TriangleMeshData

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace holo
{
    // ------------------------------------------------------------------
    // 图层
    //
    // 按「这个网格是从哪个场景取出来的」分类，不按 PxGeometryType。
    // 实测过：GetMeshData 把 box / convex 全部摊成三角形之后，
    // TriangleMeshData.Type 一律是 eTRIANGLEMESH，照类型配色会把
    // 会动的箱子和不动的墙面刷成一个颜色。
    // ------------------------------------------------------------------
    enum class Layer
    {
        Terrain,
        Static,
        Dynamic,
    };

    struct LayerStyle
    {
        uint32_t edge;        // 边色
        uint32_t fill;        // 面填充色
        const wchar_t* label; // 图例文案
        float softGain;       // 软边强度系数（地形压低，否则一片网）
        float hardGain;       // 硬边强度系数
    };

    inline LayerStyle StyleOf(Layer l)
    {
        switch (l)
        {
        case Layer::Terrain:
            // 地形的软边只要 0.2 的强度：一张 3200 三角形的网铺满整个
            // 下半屏，如果和静态几何同亮度，房间就淹在网格里了。
            // 地形的硬边也要压：heightfield 是带噪声的高度场，
            // 相邻三角形的法线经常差 20 度以上，于是「折痕边」占了绝大多数 ——
            // 按静态几何的亮度画，整片地面会亮成一张绿网。
            return { soft3d::PackRGB(0x46, 0xD2, 0x8C), soft3d::PackRGB(0x10, 0x3A, 0x2A), L"地形  heightfield", 0.14f, 0.26f };
        case Layer::Static:
            return { soft3d::PackRGB(0x64, 0xB8, 0xFF), soft3d::PackRGB(0x12, 0x2E, 0x4A), L"静态几何  triangle mesh", 1.0f, 1.0f };
        case Layer::Dynamic:
            return { soft3d::PackRGB(0xFF, 0xC2, 0x4F), soft3d::PackRGB(0x4A, 0x32, 0x10), L"动态刚体  rigid body", 1.0f, 1.0f };
        }
        return { soft3d::PackRGB(0xC8, 0xC8, 0xC8), soft3d::PackRGB(0x20, 0x20, 0x20), L"", 1.0f, 1.0f };
    }

    // ------------------------------------------------------------------
    // 目标缓冲：预乘 alpha 的 ARGB + 深度
    //
    // 深度存 1/z：透视投影下 1/z 在屏幕空间线性，所以三角形可以
    // 预解一个平面方程，内循环只剩加法；线段也能直接线性插值。
    // 值越大越靠近相机，0 表示该像素还没有任何几何。
    // ------------------------------------------------------------------
    struct Target
    {
        uint32_t* px = nullptr;
        int w = 0, h = 0;

        // 深度缓冲按 1/2 分辨率存。
        //
        // 理由是纯成本：线段本来只有 1~2px 宽，深度只需要回答
        // 「这个位置前面有没有东西挡着」，不需要全分辨率精度。
        // 半分辨率把深度预通道的像素数降到 1/4，视觉上的唯一代价是
        // 遮挡边缘会溢出 1~2px —— 那反而像一圈描边，更好看。
        int dw = 0, dh = 0;
        std::vector<float> depth;

        void Attach(uint32_t* p, int W, int H)
        {
            px = p;
            w = W;
            h = H;
            dw = (W + 1) / 2;
            dh = (H + 1) / 2;
            const size_t n = (size_t)dw * (size_t)dh;
            if (depth.size() != n) depth.assign(n, 0.0f);
        }

        void Begin() { std::fill(depth.begin(), depth.end(), 0.0f); }

        inline float DepthAt(int x, int y) const
        {
            return depth[(size_t)(y >> 1) * (size_t)dw + (x >> 1)];
        }

        // 线段专用：在 3x3 半分辨率邻域里取「最远」的那个深度值（1/z 最小）。
        // 半分辨率深度是有损的，一条躺在地形表面上的边，它的采样点
        // 很可能落在旁边那个更高的三角形上，于是被判成「被挡住」，
        // 整条线就被切成虚线。取邻域最小值等于放宽到「只要有任何一个
        // 邻格是空的/更远，就允许画」，虚线就没了。
        inline float DepthAtSoft(int x, int y) const
        {
            const int cx = x >> 1, cy = y >> 1;
            const float* row = depth.data() + (size_t)cy * (size_t)dw;

            float m = row[cx];
            if (cx > 0)      m = std::min(m, row[cx - 1]);
            if (cx + 1 < dw) m = std::min(m, row[cx + 1]);
            if (cy > 0)
            {
                const float* up = row - dw;
                m = std::min(m, up[cx]);
                if (cx > 0)      m = std::min(m, up[cx - 1]);
                if (cx + 1 < dw) m = std::min(m, up[cx + 1]);
            }
            if (cy + 1 < dh)
            {
                const float* dn = row + dw;
                m = std::min(m, dn[cx]);
                if (cx > 0)      m = std::min(m, dn[cx - 1]);
                if (cx + 1 < dw) m = std::min(m, dn[cx + 1]);
            }
            return m;
        }

        // 预乘 alpha 混合：dst.rgb 已经是「颜色 * 自身 alpha」。
        // 所以合成公式是 out = src*sa + dst*(1-sa)，不需要再乘一遍 dy。
        inline void Blend(int x, int y, int r, int g, int b, float a)
        {
            if (x < 0 || y < 0 || x >= w || y >= h) return;
            if (a <= 0.0015f) return;
            if (a > 1.0f) a = 1.0f;

            uint32_t* p = px + (size_t)y * (size_t)w + x;
            const uint32_t d = *p;
            const float ia = 1.0f - a;

            const float dr = (float)((d >> 16) & 0xFF);
            const float dg = (float)((d >> 8) & 0xFF);
            const float db = (float)(d & 0xFF);
            const float da = (float)((d >> 24) & 0xFF);

            float nr = (float)r * a + dr * ia;
            float ng = (float)g * a + dg * ia;
            float nb = (float)b * a + db * ia;
            float na = a * 255.0f + da * ia;

            if (nr > 255.0f) nr = 255.0f;
            if (ng > 255.0f) ng = 255.0f;
            if (nb > 255.0f) nb = 255.0f;
            if (na > 255.0f) na = 255.0f;

            *p = ((uint32_t)(na + 0.5f) << 24)
               | ((uint32_t)(nr + 0.5f) << 16)
               | ((uint32_t)(ng + 0.5f) << 8)
               | (uint32_t)(nb + 0.5f);
        }
    };

    // ------------------------------------------------------------------
    // 绘制参数
    // ------------------------------------------------------------------
    struct DrawOptions
    {
        bool  fill = true;
        bool  fillTerrain = false;  // 地形铺满下半屏，填一遍要 8ms 而几乎看不见
        float fillAlpha = 0.055f;   // 面的半透明度：给线框一点体积感，但仍是叠层
        float hardAlpha = 0.80f;    // 折痕/边界边
        float softAlpha = 0.170f;   // 同面内剖分边
        float glowAlpha = 0.16f;    // 硬边的外发光
        float widthNear = 1.55f;    // 近处线宽
        float widthFar = 1.05f;     // 远处线宽
        float creaseCos = 0.93f;    // 法线夹角 > ~21.6° 算折痕
        float fogStart = 1500.0f;
        float fogEnd = 8200.0f;
        // 地形边只画到这个距离。heightfield 铺满几十米见方，
        // 全画出来下半屏全是网格，房间反而看不清；截断之后
        // 近处有网、远处干净，也更像「扫描范围」。
        float terrainEdgeRange = 2700.0f;
        float glowDist = 3000.0f;   // 超过这个距离不再画发光（省一半像素）
        uint32_t highlight = 0xFF3C2E;
    };

    inline DrawOptions DefaultOptions()
    {
        return DrawOptions{};
    }

    // ------------------------------------------------------------------
    // 近平面裁剪 + 投影：一条线段
    // ------------------------------------------------------------------
    inline bool ProjectSegment(const soft3d::Camera& cam,
                               const Vector3& a, const Vector3& b,
                               float& x0, float& y0, float& iz0,
                               float& x1, float& y1, float& iz1)
    {
        float xc0, yc0, zc0, xc1, yc1, zc1;
        cam.ToView(a, xc0, yc0, zc0);
        cam.ToView(b, xc1, yc1, zc1);

        const float zn = cam.zNear;
        if (zc0 < zn && zc1 < zn) return false;

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

        cam.ProjectWithInvz(xc0, yc0, zc0, x0, y0);
        cam.ProjectWithInvz(xc1, yc1, zc1, x1, y1);
        iz0 = 1.0f / zc0;
        iz1 = 1.0f / zc1;

        return std::isfinite(x0) && std::isfinite(y0) &&
               std::isfinite(x1) && std::isfinite(y1);
    }

    // ------------------------------------------------------------------
    // 三角形投影（含近平面裁剪，裁完是 1~2 个三角形）
    // 返回写入 out 的三角形数量，outDist 给出去心距离（供着色/雾）
    // ------------------------------------------------------------------
    inline int ProjectTriangle(const soft3d::Camera& cam,
                               const Vector3& a, const Vector3& b, const Vector3& c,
                               soft3d::Tri out[2], float& outDist)
    {
        soft3d::CVert in[3], cl[4];
        cam.ToView(a, in[0].x, in[0].y, in[0].z);
        cam.ToView(b, in[1].x, in[1].y, in[1].z);
        cam.ToView(c, in[2].x, in[2].y, in[2].z);

        outDist = (in[0].z + in[1].z + in[2].z) * (1.0f / 3.0f);

        const int n = soft3d::ClipNear(in, cl, cam.zNear);
        if (n < 3) return 0;

        float sx[4], sy[4], iz[4];
        for (int i = 0; i < n; i++)
        {
            cam.ProjectWithInvz(cl[i].x, cl[i].y, cl[i].z, sx[i], sy[i]);
            iz[i] = 1.0f / cl[i].z;
        }

        int count = 0;
        for (int i = 1; i + 1 < n; i++)
        {
            soft3d::Tri& t = out[count++];
            t.x[0] = sx[0];      t.y[0] = sy[0];      t.iz[0] = iz[0];
            t.x[1] = sx[i];      t.y[1] = sy[i];      t.iz[1] = iz[i];
            t.x[2] = sx[i + 1];  t.y[2] = sy[i + 1];  t.iz[2] = iz[i + 1];
        }
        return count;
    }

    // ------------------------------------------------------------------
    // 深度预通道：只写 depth，不碰颜色
    //
    // 这就是隐藏线消除的全部秘密：场景先「静默地填一遍」，
    // 后面画线时逐像素问一句「这个位置前面有没有东西挡着」。
    // ------------------------------------------------------------------
    inline void RasterDepth(Target& t, const soft3d::Tri& tri)
    {
        const float x0 = tri.x[0], y0 = tri.y[0];
        const float x1 = tri.x[1], y1 = tri.y[1];
        const float x2 = tri.x[2], y2 = tri.y[2];

        const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (!std::isfinite(area) || std::fabs(area) < 1e-7f) return;

        float fxmin = std::min(x0, std::min(x1, x2));
        float fxmax = std::max(x0, std::max(x1, x2));
        float fymin = std::min(y0, std::min(y1, y2));
        float fymax = std::max(y0, std::max(y1, y2));
        if (!std::isfinite(fxmin) || !std::isfinite(fxmax) ||
            !std::isfinite(fymin) || !std::isfinite(fymax)) return;

        const float LIMIT = 1e6f;
        fxmin = std::max(fxmin, -LIMIT);
        fxmax = std::min(fxmax, LIMIT);
        fymin = std::max(fymin, -LIMIT);
        fymax = std::min(fymax, LIMIT);

        // 采样点落在半分辨率格子的中心（全分辨率坐标 = 2*i+1）
        const int i0 = std::max(0, (int)std::floor(fxmin * 0.5f));
        const int i1 = std::min(t.dw - 1, (int)std::ceil(fxmax * 0.5f));
        const int j0 = std::max(0, (int)std::floor(fymin * 0.5f));
        const int j1 = std::min(t.dh - 1, (int)std::ceil(fymax * 0.5f));
        if (i0 > i1 || j0 > j1) return;

        const float A0 = y1 - y2, B0 = x2 - x1, C0 = x1 * y2 - x2 * y1;
        const float A1 = y2 - y0, B1 = x0 - x2, C1 = x2 * y0 - x0 * y2;
        const float A2 = y0 - y1, B2 = x1 - x0, C2 = x0 * y1 - x1 * y0;
        const float sign = (area > 0.0f) ? 1.0f : -1.0f;

        const float det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (std::fabs(det) < 1e-9f) return;

        const float iz0 = tri.iz[0], iz1 = tri.iz[1], iz2 = tri.iz[2];
        const float Da = ((iz1 - iz0) * (y2 - y0) - (iz2 - iz0) * (y1 - y0)) / det;
        const float Db = ((iz2 - iz0) * (x1 - x0) - (iz1 - iz0) * (x2 - x0)) / det;
        const float Dc = iz0 - Da * x0 - Db * y0;

        for (int j = j0; j <= j1; j++)
        {
            const float fy = (float)(j * 2) + 1.0f;
            const float pxs = (float)(i0 * 2) + 1.0f;

            float w0 = A0 * pxs + B0 * fy + C0;
            float w1 = A1 * pxs + B1 * fy + C1;
            float w2 = A2 * pxs + B2 * fy + C2;
            float iz = Da * pxs + Db * fy + Dc;

            float* zrow = t.depth.data() + (size_t)j * (size_t)t.dw;

            for (int i = i0; i <= i1; i++)
            {
                if ((w0 * sign) >= 0.0f && (w1 * sign) >= 0.0f && (w2 * sign) >= 0.0f)
                {
                    if (iz > zrow[i]) zrow[i] = iz;
                }
                w0 += A0 * 2.0f; w1 += A1 * 2.0f; w2 += A2 * 2.0f; iz += Da * 2.0f;
            }
        }
    }

    // ------------------------------------------------------------------
    // 半透明面填充（不写深度，只按已有深度做剔除）
    //
    // 目的不是「画成实心」，而是让线框有体积：面只贡献 5% 不透明度，
    // 桌面照样透过来，但背面的线会被这层面压暗，前后关系立刻清楚。
    // ------------------------------------------------------------------
    inline void RasterFill(Target& t, const soft3d::Tri& tri,
                           int r, int g, int b, float alpha,
                           float fogStart, float fogEnd, float dist)
    {
        if (alpha <= 0.002f) return;

        const float x0 = tri.x[0], y0 = tri.y[0];
        const float x1 = tri.x[1], y1 = tri.y[1];
        const float x2 = tri.x[2], y2 = tri.y[2];

        const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (!std::isfinite(area) || std::fabs(area) < 1e-7f) return;

        float fxmin = std::min(x0, std::min(x1, x2));
        float fxmax = std::max(x0, std::max(x1, x2));
        float fymin = std::min(y0, std::min(y1, y2));
        float fymax = std::max(y0, std::max(y1, y2));
        if (!std::isfinite(fxmin) || !std::isfinite(fxmax) ||
            !std::isfinite(fymin) || !std::isfinite(fymax)) return;

        const float LIMIT = 1e6f;
        fxmin = std::max(fxmin, -LIMIT);
        fxmax = std::min(fxmax, LIMIT);
        fymin = std::max(fymin, -LIMIT);
        fymax = std::min(fymax, LIMIT);

        const int x0i = std::max(0, (int)std::floor(fxmin));
        const int x1i = std::min(t.w - 1, (int)std::ceil(fxmax));
        const int y0i = std::max(0, (int)std::floor(fymin));
        const int y1i = std::min(t.h - 1, (int)std::ceil(fymax));
        if (x0i > x1i || y0i > y1i) return;

        const float A0 = y1 - y2, B0 = x2 - x1, C0 = x1 * y2 - x2 * y1;
        const float A1 = y2 - y0, B1 = x0 - x2, C1 = x2 * y0 - x0 * y2;
        const float A2 = y0 - y1, B2 = x1 - x0, C2 = x0 * y1 - x1 * y0;
        const float sign = (area > 0.0f) ? 1.0f : -1.0f;

        const float det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (std::fabs(det) < 1e-9f) return;

        const float iz0 = tri.iz[0], iz1 = tri.iz[1], iz2 = tri.iz[2];
        const float Da = ((iz1 - iz0) * (y2 - y0) - (iz2 - iz0) * (y1 - y0)) / det;
        const float Db = ((iz2 - iz0) * (x1 - x0) - (iz1 - iz0) * (x2 - x0)) / det;
        const float Dc = iz0 - Da * x0 - Db * y0;

        // 距离雾作用在面上，远近才有层次
        float a = alpha;
        if (fogEnd > fogStart && dist > fogStart)
        {
            float f = (dist - fogStart) / (fogEnd - fogStart);
            if (f > 1.0f) f = 1.0f;
            a *= (1.0f - f * 0.75f);
        }
        if (a <= 0.002f) return;

        // 面的深度判定放宽一点点，避免和预通道写入的值互相排斥
        const float bias = 0.9995f;

        for (int py = y0i; py <= y1i; py++)
        {
            const float fy = (float)py + 0.5f;
            const float pxs = (float)x0i + 0.5f;

            float w0 = A0 * pxs + B0 * fy + C0;
            float w1 = A1 * pxs + B1 * fy + C1;
            float w2 = A2 * pxs + B2 * fy + C2;
            float iz = Da * pxs + Db * fy + Dc;

            for (int pxi = x0i; pxi <= x1i; pxi++)
            {
                if ((w0 * sign) >= 0.0f && (w1 * sign) >= 0.0f && (w2 * sign) >= 0.0f)
                {
                    if (iz * bias >= t.DepthAt(pxi, py)) t.Blend(pxi, py, r, g, b, a);
                }
                w0 += A0; w1 += A1; w2 += A2; iz += Da;
            }
        }
    }

    // ------------------------------------------------------------------
    // 线段 -> 屏幕矩形裁剪（Liang-Barsky）
    //
    // 为什么必须有：从相机旁边擦过去的墙边可以投影到 -8000..+8000，
    // 逐像素跑完再被 bounds check 丢掉，一条线就是上万次空循环。
    // 地形那种横贯全屏的长边尤其致命 —— 实测占掉了线渲染的一半时间。
    // ------------------------------------------------------------------
    inline bool ClipSegmentRect(float& x0, float& y0, float& iz0,
                                float& x1, float& y1, float& iz1,
                                float xmin, float ymin, float xmax, float ymax)
    {
        const float dx = x1 - x0;
        const float dy = y1 - y0;

        float t0 = 0.0f, t1 = 1.0f;

        auto test = [&](float p, float q) -> bool
        {
            if (std::fabs(p) < 1e-9f) return q >= 0.0f;
            const float r = q / p;
            if (p < 0.0f)
            {
                if (r > t1) return false;
                if (r > t0) t0 = r;
            }
            else
            {
                if (r < t0) return false;
                if (r < t1) t1 = r;
            }
            return true;
        };

        if (!test(-dx, x0 - xmin)) return false;
        if (!test(dx, xmax - x0)) return false;
        if (!test(-dy, y0 - ymin)) return false;
        if (!test(dy, ymax - y0)) return false;

        if (t1 <= t0) return false;

        if (t0 > 0.0f || t1 < 1.0f)
        {
            const float nx0 = x0 + dx * t0;
            const float ny0 = y0 + dy * t0;
            const float nx1 = x0 + dx * t1;
            const float ny1 = y0 + dy * t1;
            const float niz0 = iz0 + (iz1 - iz0) * t0;
            const float niz1 = iz0 + (iz1 - iz0) * t1;

            x0 = nx0; y0 = ny0; iz0 = niz0;
            x1 = nx1; y1 = ny1; iz1 = niz1;
        }
        return true;
    }

    // ------------------------------------------------------------------
    // 抗锯齿线段（深度测试 + 距离雾 + 线宽）
    //
    // 用 1/z 沿主线轴线性插值 —— 这是透视投影下严格正确的插值量，
    // 换成插值 z 会在近处粗、远处细，看起来很脏。
    // ------------------------------------------------------------------
    inline void Segment(Target& t,
                        float x0, float y0, float iz0,
                        float x1, float y1, float iz1,
                        uint32_t color, float alpha, float width,
                        const DrawOptions& opt)
    {
        if (!std::isfinite(x0) || !std::isfinite(y0) ||
            !std::isfinite(x1) || !std::isfinite(y1)) return;
        if (iz0 <= 1e-6f || iz1 <= 1e-6f) return;
        if (alpha <= 0.002f) return;

        // 先裁到屏幕外扩 2px 的矩形里，再谈栅格化
        if (!ClipSegmentRect(x0, y0, iz0, x1, y1, iz1,
                             -2.0f, -2.0f, (float)t.w + 2.0f, (float)t.h + 2.0f))
            return;

        const float dx = x1 - x0;
        const float dy = y1 - y0;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-4f) return;

        const int r = (color >> 16) & 0xFF;
        const int g = (color >> 8) & 0xFF;
        const int b = color & 0xFF;

        // 线宽 < 1.35 时只跑一遍（靠 alpha 表现粗细），否则两遍。
        // 三遍以上肉眼分不出来，成本却线性涨。
        const int passes = (width > 1.35f) ? 2 : 1;
        const float passAlpha = alpha * std::min(1.0f, width / (float)passes);

        const float nx = -dy / len;
        const float ny = dx / len;

        for (int p = 0; p < passes; p++)
        {
            const float off = (passes == 1) ? 0.0f : ((float)p - (float)(passes - 1) * 0.5f);

            float ax = x0 + nx * off, ay = y0 + ny * off;
            float bx = x1 + nx * off, by = y1 + ny * off;

            const bool steep = std::fabs(by - ay) > std::fabs(bx - ax);

            float X0 = steep ? ay : ax;
            float Y0 = steep ? ax : ay;
            float X1 = steep ? by : bx;
            float Y1 = steep ? bx : by;
            float Z0 = iz0, Z1 = iz1;

            if (X0 > X1)
            {
                std::swap(X0, X1);
                std::swap(Y0, Y1);
                std::swap(Z0, Z1);
            }

            const float span = X1 - X0;
            if (span < 1e-4f) continue;

            const float gradY = (Y1 - Y0) / span;
            const float gradZ = (Z1 - Z0) / span;

            const int iStart = (int)std::floor(X0);
            const int iEnd = (int)std::ceil(X1);

            float yy = Y0 + ((float)iStart - X0) * gradY;
            float zz = Z0 + ((float)iStart - X0) * gradZ;

            for (int i = iStart; i <= iEnd; i++)
            {
                const int iyFloor = (int)std::floor(yy);
                const float frac = yy - (float)iyFloor;

                for (int k = 0; k < 2; k++)
                {
                    const float cov = (k == 0) ? (1.0f - frac) : frac;
                    if (cov <= 0.004f) continue;

                    const int gx = steep ? (iyFloor + k) : i;
                    const int gy = steep ? i : (iyFloor + k);
                    if (gx < 0 || gy < 0 || gx >= t.w || gy >= t.h) continue;

                    // 深度：稍微往相机方向偏一点，否则边会和自己所在的
                    // 面打成平手，整条线被吃掉
                    const float ztest = zz * 1.0060f;
                    if (ztest < t.DepthAtSoft(gx, gy)) continue;

                    const float z = 1.0f / zz;
                    float f = (z - opt.fogStart) / (opt.fogEnd - opt.fogStart);
                    if (f < 0.0f) f = 0.0f;
                    if (f > 1.0f) f = 1.0f;

                    float a = passAlpha * cov * (1.0f - 0.88f * f);
                    if (z < 90.0f) a *= z / 90.0f;      // 贴脸淡出

                    if (a > 0.002f) t.Blend(gx, gy, r, g, b, a);
                }

                yy += gradY;
                zz += gradZ;
            }
        }
    }

    // ------------------------------------------------------------------
    // 特征边缓存
    //
    // 键是 TriangleMeshData 的地址 + 索引数：动态刚体每帧重建缓冲区时
    // 地址不变（场景缓存复用），所以这份缓存跨帧有效。
    // ------------------------------------------------------------------
    struct EdgeList
    {
        std::vector<uint32_t> hard;   // 折痕边 + 边界边
        std::vector<uint32_t> soft;   // 同面内剖分边
        size_t indexCount = 0;
        size_t vertexCount = 0;
        float  creaseCos = -1.0f;     // 这份缓存是按哪个阈值算的
    };

    inline std::unordered_map<const void*, EdgeList>& EdgeCache()
    {
        static std::unordered_map<const void*, EdgeList> cache;
        return cache;
    }

    inline const EdgeList& EdgesOf(const TriangleMeshData& mesh, float creaseCos)
    {
        auto& cache = EdgeCache();

        auto it = cache.find(&mesh);
        if (it != cache.end() &&
            it->second.indexCount == mesh.Indices.size() &&
            it->second.vertexCount == mesh.Vertices.size() &&
            it->second.creaseCos == creaseCos)
            return it->second;

        if (cache.size() > 4096) cache.clear();

        EdgeList el;
        el.indexCount = mesh.Indices.size();
        el.vertexCount = mesh.Vertices.size();
        el.creaseCos = creaseCos;

        const size_t vcount = mesh.Vertices.size();
        const size_t triCount = mesh.Indices.size() / 3;
        if (triCount == 0 || vcount == 0)
        {
            cache[&mesh] = std::move(el);
            return cache[&mesh];
        }

        // 每个三角形的单位法线
        std::vector<Vector3> nrm(triCount, Vector3(0, 0, 0));
        for (size_t f = 0; f < triCount; f++)
        {
            const uint32_t i0 = mesh.Indices[f * 3 + 0];
            const uint32_t i1 = mesh.Indices[f * 3 + 1];
            const uint32_t i2 = mesh.Indices[f * 3 + 2];
            if (i0 >= vcount || i1 >= vcount || i2 >= vcount) continue;

            const Vector3 e1 = mesh.Vertices[i1] - mesh.Vertices[i0];
            const Vector3 e2 = mesh.Vertices[i2] - mesh.Vertices[i0];
            Vector3 n = e1.cross(e2);
            const float l = std::sqrt(n.dot(n));
            if (l > 1e-6f) n = n * (1.0f / l);
            nrm[f] = n;
        }

        std::unordered_map<uint64_t, uint32_t> seen;
        seen.reserve(triCount * 3 + 16);

        el.hard.reserve(triCount);
        el.soft.reserve(triCount * 2);

        for (size_t f = 0; f < triCount; f++)
        {
            for (int e = 0; e < 3; e++)
            {
                const uint32_t a = mesh.Indices[f * 3 + (size_t)e];
                const uint32_t b = mesh.Indices[f * 3 + (size_t)((e + 1) % 3)];
                if (a == b) continue;

                const uint32_t lo = (a < b) ? a : b;
                const uint32_t hi = (a < b) ? b : a;
                const uint64_t key = ((uint64_t)lo << 32) | (uint64_t)hi;

                auto hit = seen.find(key);
                if (hit == seen.end())
                {
                    seen.emplace(key, (uint32_t)f);
                    continue;
                }

                // 用 |dot| 而不是 dot：高度场的三角形绕序不一定一致，
                // 反向绕序会让相邻共面三角形算出 -1，被误判成折痕边。
                const float d = std::fabs(nrm[f].dot(nrm[hit->second]));
                if (d < creaseCos)
                {
                    el.hard.push_back(a);
                    el.hard.push_back(b);
                }
                else
                {
                    el.soft.push_back(a);
                    el.soft.push_back(b);
                }
                seen.erase(hit);
            }
        }

        // 只被一个三角形用到的边 = 网格边界，必须画亮，否则模型开口
        for (const auto& kv : seen)
        {
            el.hard.push_back((uint32_t)(kv.first >> 32));
            el.hard.push_back((uint32_t)(kv.first & 0xFFFFFFFFu));
        }

        cache[&mesh] = std::move(el);
        return cache[&mesh];
    }

    // ------------------------------------------------------------------
    // 网格绘制：拆成三步，主循环里按「全场景深度 -> 全场景填充 ->
    // 全场景线」的顺序调用，才能保证遮挡关系正确。
    //
    // 如果按网格逐个「深度+填充+线」地画，先画的网格会被后画的
    // 网格的深度预通道挡住 —— 线就消失了。
    // ------------------------------------------------------------------
    inline void DrawMeshDepth(Target& t, const soft3d::Camera& cam,
                              const TriangleMeshData& mesh)
    {
        const size_t vcount = mesh.Vertices.size();
        if (vcount < 3 || vcount > 400000) return;

        soft3d::Tri tris[2];
        for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
        {
            const uint32_t i0 = mesh.Indices[i], i1 = mesh.Indices[i + 1], i2 = mesh.Indices[i + 2];
            if (i0 >= vcount || i1 >= vcount || i2 >= vcount) continue;

            float dist = 0.0f;
            const int n = ProjectTriangle(cam, mesh.Vertices[i0], mesh.Vertices[i1], mesh.Vertices[i2], tris, dist);
            for (int k = 0; k < n; k++) RasterDepth(t, tris[k]);
        }
    }

    inline void DrawMeshFill(Target& t, const soft3d::Camera& cam,
                             const TriangleMeshData& mesh, Layer layer,
                             const DrawOptions& opt, bool highlight,
                             const soft3d::Shading& shading)
    {
        if (!opt.fill || opt.fillAlpha <= 0.002f) return;

        const size_t vcount = mesh.Vertices.size();
        if (vcount < 3 || vcount > 400000) return;

        const LayerStyle st = StyleOf(layer);
        int br, bg, bb;
        soft3d::Unpack(highlight ? opt.highlight : st.fill, br, bg, bb);

        soft3d::Tri tris[2];
        for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
        {
            const uint32_t i0 = mesh.Indices[i], i1 = mesh.Indices[i + 1], i2 = mesh.Indices[i + 2];
            if (i0 >= vcount || i1 >= vcount || i2 >= vcount) continue;

            const Vector3& a = mesh.Vertices[i0];
            const Vector3& b = mesh.Vertices[i1];
            const Vector3& c = mesh.Vertices[i2];

            // 面法线（双面定向：永远朝向相机），物理网格绕序不可信
            Vector3 e1 = b - a;
            Vector3 e2 = c - a;
            Vector3 n = e1.cross(e2);
            const float nl = std::sqrt(n.dot(n));
            if (nl < 1e-6f) continue;
            n = n * (1.0f / nl);

            Vector3 toEye = cam.eye - a;
            const float dl = std::sqrt(toEye.dot(toEye));
            if (dl < 1e-4f) continue;
            toEye = toEye * (1.0f / dl);

            if (n.dot(toEye) < 0.0f) n = n * -1.0f;

            // 面罩暗处理：直接拿材质色 + 一点方向感，不做完整光照 ——
            // 叠层要的是「结构」，光比过强反而看不清线。
            const float hemi = 0.5f + 0.5f * n.z;
            const float key = std::max(0.0f, n.dot(shading.keyDir));
            const float lum = 0.55f + 0.45f * hemi + 0.35f * key;

            const int fr = std::min(255, (int)(br * lum + 0.5f));
            const int fg = std::min(255, (int)(bg * lum + 0.5f));
            const int fb = std::min(255, (int)(bb * lum + 0.5f));

            float dist = 0.0f;
            const int cnt = ProjectTriangle(cam, a, b, c, tris, dist);
            for (int k = 0; k < cnt; k++)
                RasterFill(t, tris[k], fr, fg, fb, opt.fillAlpha, opt.fogStart, opt.fogEnd, dist);
        }
    }

    inline void DrawMeshEdges(Target& t, const soft3d::Camera& cam,
                              const TriangleMeshData& mesh, Layer layer,
                              const DrawOptions& opt, bool highlight)
    {
        const size_t vcount = mesh.Vertices.size();
        if (vcount < 3 || vcount > 400000) return;

        const LayerStyle st = StyleOf(layer);
        const uint32_t edgeColor = highlight ? opt.highlight : st.edge;

        // heightfield 是带噪声的高度场：相邻三角形的法线经常差 20° 以上，
        // 用静态几何的 21° 阈值会把几乎所有格子边判成折痕边，而折痕边
        // 是不受距离限制的 —— 结果整片地面亮成一张绿网。
        // 地形单独放宽到 36°，只留下真正的坡折和边界。
        const float crease = (layer == Layer::Terrain) ? 0.80f : opt.creaseCos;
        const EdgeList& el = EdgesOf(mesh, crease);

        const float hardAlpha = (highlight ? 0.95f : opt.hardAlpha) * st.hardGain;
        const float softAlpha = (highlight ? 0.34f : opt.softAlpha) * st.softGain;

        // 先画软边（同面剖分），再画硬边，硬边压在上面。
        //
        // 软边只给地形：heightfield 的三角剖分就是它的"格网"，少了这层地面是空的。
        // box 之类简单几何的软边只有每个面那条对角线 —— 投影出来是横贯画面的
        // 斜线状虚线。地板上 25 块瓷砖就是 25 条，整片地面看起来像碎玻璃。
        const size_t softPairs = (layer == Layer::Terrain) ? el.soft.size() / 2 : 0;
        for (size_t i = 0; i < softPairs; i++)
        {
            const uint32_t i0 = el.soft[i * 2], i1 = el.soft[i * 2 + 1];
            if (i0 >= vcount || i1 >= vcount) continue;

            float x0, y0, iz0, x1, y1, iz1;
            if (!ProjectSegment(cam, mesh.Vertices[i0], mesh.Vertices[i1], x0, y0, iz0, x1, y1, iz1))
                continue;

            float a = softAlpha;
            if (layer == Layer::Terrain)
            {
                const float z = 2.0f / (iz0 + iz1);
                if (z > opt.terrainEdgeRange) continue;
                // 截断前 900 单位内线性淡出，否则地形边会在地面上
                // 切出一圈生硬的弧线
                const float k = (z - (opt.terrainEdgeRange - 900.0f)) / 900.0f;
                if (k > 0.0f) a *= (1.0f - std::min(1.0f, k));
                if (a <= 0.002f) continue;
            }

            Segment(t, x0, y0, iz0, x1, y1, iz1, edgeColor, a,
                    opt.widthFar, opt);
        }

        const size_t hardPairs = el.hard.size() / 2;
        for (size_t i = 0; i < hardPairs; i++)
        {
            const uint32_t i0 = el.hard[i * 2], i1 = el.hard[i * 2 + 1];
            if (i0 >= vcount || i1 >= vcount) continue;

            float x0, y0, iz0, x1, y1, iz1;
            if (!ProjectSegment(cam, mesh.Vertices[i0], mesh.Vertices[i1], x0, y0, iz0, x1, y1, iz1))
                continue;

            const float zAvg = 2.0f / (iz0 + iz1);

            // 注意：软边受距离限制，硬边不受 —— 硬边是地形边界和坡折，
            // 数量少、信息量大，全画出来才能让地面有个轮廓
            // 外发光只给动态刚体和命中网格。
            // 静态墙面 + 地形加起来上千条硬边，每条 3~4 遍平行线，
            // 实测这一项能吃掉 8ms —— 而它在深色背景下几乎看不出来。
            const bool wantGlow = highlight || (layer == Layer::Dynamic);
            if (wantGlow && opt.glowAlpha > 0.002f && zAvg < opt.glowDist)
            {
                const float k = 1.0f - zAvg / opt.glowDist;
                Segment(t, x0, y0, iz0, x1, y1, iz1, edgeColor,
                        opt.glowAlpha * k, opt.widthNear * 3.4f, opt);
            }

            // 近处线粗、远处线细
            const float zt = std::min(1.0f, zAvg / 2600.0f);
            const float width = opt.widthNear + (opt.widthFar - opt.widthNear) * zt;

            Segment(t, x0, y0, iz0, x1, y1, iz1, edgeColor, hardAlpha, width, opt);
        }
    }

    // ------------------------------------------------------------------
    // UI 基础件（透明底上画面板，全部带 AA 边）
    // ------------------------------------------------------------------
    namespace ui
    {
        inline float SdRoundBox(float px, float py, float hx, float hy, float r)
        {
            const float qx = std::fabs(px) - hx + r;
            const float qy = std::fabs(py) - hy + r;
            const float ox = std::max(qx, 0.0f);
            const float oy = std::max(qy, 0.0f);
            const float outside = std::sqrt(ox * ox + oy * oy);
            const float inside = std::min(std::max(qx, qy), 0.0f);
            return outside + inside - r;
        }

        inline void Panel(Target& t, int x, int y, int w, int h,
                          float alpha = 0.60f, int radius = 12)
        {
            if (w <= 0 || h <= 0) return;

            const float cx = x + w * 0.5f;
            const float cy = y + h * 0.5f;
            const float hx = w * 0.5f;
            const float hy = h * 0.5f;

            // 填充和描边在同一遍里算完：SDF 只求一次。
            // 分两遍写会把这个函数的时间直接翻倍，而面板占了 HUD 的大头。
            for (int py = y - 2; py < y + h + 2; py++)
            {
                for (int px = x - 2; px < x + w + 2; px++)
                {
                    const float d = SdRoundBox((float)px + 0.5f - cx, (float)py + 0.5f - cy, hx, hy, (float)radius);

                    const float cov = std::min(1.0f, std::max(0.0f, 0.5f - d));
                    if (cov <= 0.0f) continue;

                    t.Blend(px, py, 0x08, 0x0E, 0x1A, alpha * cov);

                    // 内圈 1px 亮边，做出玻璃片的厚度
                    const float band = std::min(1.0f, std::max(0.0f, 1.0f - std::fabs(d + 0.5f)));
                    if (band > 0.0f)
                        t.Blend(px, py, 0x7C, 0xA6, 0xD8, 0.20f * band * cov);
                }
            }
        }

        // HUD 专用线段：不做深度测试，也不做雾。
        // HUD 在 3D 之上，用 Segment 会被深度缓冲挡掉。
        inline void Line2D(Target& t, float x0, float y0, float x1, float y1,
                           uint32_t color, float a, float w = 1.0f)
        {
            const int r = (color >> 16) & 0xFF;
            const int g = (color >> 8) & 0xFF;
            const int b = color & 0xFF;

            const float dx = x1 - x0, dy = y1 - y0;
            const float len = std::sqrt(dx * dx + dy * dy);
            const bool steep = std::fabs(dy) > std::fabs(dx);

            float X0 = steep ? y0 : x0, Y0 = steep ? x0 : y0;
            float X1 = steep ? y1 : x1, Y1 = steep ? x1 : y1;
            if (X0 > X1) { std::swap(X0, X1); std::swap(Y0, Y1); }

            const float span = X1 - X0;
            if (span < 1e-4f || len > 8000.0f) return;

            const float grad = (Y1 - Y0) / span;
            const int iStart = (int)std::floor(X0);
            const int iEnd = (int)std::ceil(X1);

            float yy = Y0 + ((float)iStart - X0) * grad;

            for (int i = iStart; i <= iEnd; i++)
            {
                const int iy = (int)std::floor(yy);
                const float frac = yy - (float)iy;

                for (int k = 0; k < 2; k++)
                {
                    const float cov = (k == 0) ? (1.0f - frac) : frac;
                    if (cov <= 0.004f) continue;
                    const int gx = steep ? (iy + k) : i;
                    const int gy = steep ? i : (iy + k);
                    t.Blend(gx, gy, r, g, b, a * cov * w);
                }
                yy += grad;
            }
        }

        inline void Rule(Target& t, int x, int y, int w, uint32_t color, float a = 0.5f)
        {
            for (int i = 0; i < w; i++)
            {
                const float k = 1.0f - (float)i / (float)std::max(1, w - 1);
                t.Blend(x + i, y, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF, a * k);
            }
        }

        inline void HBar(Target& t, int x, int y, int w, int h, float fill,
                         uint32_t color, float bgA = 0.18f)
        {
            for (int py = y; py < y + h; py++)
                for (int px = x; px < x + w; px++)
                    t.Blend(px, py, 0x9A, 0xB6, 0xD8, bgA);

            const int fw = (int)(fill * (float)w + 0.5f);
            for (int py = y; py < y + h; py++)
                for (int px = x; px < x + fw; px++)
                    t.Blend(px, py, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF, 0.85f);
        }

        // 圆环（准星、命中点标记）
        inline void Ring(Target& t, float cx, float cy, float radius,
                         uint32_t color, float a, float thickness = 1.6f)
        {
            const int r = (int)(radius + thickness + 2.0f);
            const int x0 = (int)cx - r, x1 = (int)cx + r;
            const int y0 = (int)cy - r, y1 = (int)cy + r;

            for (int py = y0; py <= y1; py++)
            {
                for (int px = x0; px <= x1; px++)
                {
                    const float dx = (float)px + 0.5f - cx;
                    const float dy = (float)py + 0.5f - cy;
                    const float d = std::sqrt(dx * dx + dy * dy);
                    const float cov = std::min(1.0f, std::max(0.0f,
                        thickness * 0.5f + 0.5f - std::fabs(d - radius)));
                    if (cov <= 0.0f) continue;
                    t.Blend(px, py, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF, a * cov);
                }
            }
        }

        // 方括号（命中包围盒的 8 个角，比整圈线框干净）
        inline void Brackets(Target& t, const soft3d::Camera& cam,
                             const Vector3& mn, const Vector3& mx,
                             uint32_t color, float a, const DrawOptions& opt)
        {
            const float k = 0.11f;
            const float ex = (mx.x - mn.x) * k;
            const float ey = (mx.y - mn.y) * k;
            const float ez = (mx.z - mn.z) * k;

            Vector3 pts[8] = {
                Vector3(mn.x, mn.y, mn.z), Vector3(mx.x, mn.y, mn.z),
                Vector3(mn.x, mx.y, mn.z), Vector3(mx.x, mx.y, mn.z),
                Vector3(mn.x, mn.y, mx.z), Vector3(mx.x, mn.y, mx.z),
                Vector3(mn.x, mx.y, mx.z), Vector3(mx.x, mx.y, mx.z),
            };

            for (int i = 0; i < 8; i++)
            {
                const float dx = (i & 1) ? -ex : ex;
                const float dy = (i & 2) ? -ey : ey;
                const float dz = (i & 4) ? -ez : ez;
                const Vector3& p = pts[i];

                const Vector3 q[3] = {
                    Vector3(p.x + dx, p.y, p.z),
                    Vector3(p.x, p.y + dy, p.z),
                    Vector3(p.x, p.y, p.z + dz),
                };

                for (int j = 0; j < 3; j++)
                {
                    float x0, y0, iz0, x1, y1, iz1;
                    if (!ProjectSegment(cam, p, q[j], x0, y0, iz0, x1, y1, iz1)) continue;
                    Segment(t, x0, y0, iz0, x1, y1, iz1, color, a, 1.4f, opt);
                }
            }
        }

        inline void ProjectedAABB(const TriangleMeshData& m, Vector3& mn, Vector3& mx)
        {
            mn = Vector3( 1e30f,  1e30f,  1e30f);
            mx = Vector3(-1e30f, -1e30f, -1e30f);
            for (const auto& v : m.Vertices)
            {
                mn.x = std::min(mn.x, v.x); mn.y = std::min(mn.y, v.y); mn.z = std::min(mn.z, v.z);
                mx.x = std::max(mx.x, v.x); mx.y = std::max(mx.y, v.y); mx.z = std::max(mx.z, v.z);
            }
        }
    } // namespace ui

    // ------------------------------------------------------------------
    // 抗锯齿文字
    //
    // 做法：先把字画在一张黑色离屏 DIB 上（GDI 灰度抗锯齿），
    // 再把每个像素的亮度当作覆盖率，混合进叠层。
    //
    // 为什么不直接用 Canvas::Text：GDI 往透明 DIB 上写字时只写 RGB、
    // alpha 写成 0，边缘像素是「和黑色混合」的结果，直接显示就是
    // 一圈黑边。取亮度当覆盖率，边缘才是真正透明的。
    // ------------------------------------------------------------------
    class TextAA
    {
    public:
        ~TextAA() { Release(); }

        void Draw(Target& t, int x, int y, const wchar_t* s, int px,
                  uint32_t color, float alpha = 1.0f, bool bold = false,
                  int tracking = 0, bool shadow = false)
        {
            if (!s || !*s || alpha <= 0.004f) return;

            const int len = (int)wcslen(s);
            if (!Ensure(px * (len * 2 + 2) + 32, px * 2 + 20)) return;

            HFONT font = MakeFont(px, bold);
            if (!font) return;

            HGDIOBJ oldFont = SelectObject(dc_, font);

            SIZE sz{};
            GetTextExtentPoint32W(dc_, s, len, &sz);

            // 字间距是逐字画出来的，GetTextExtentPoint32W 不知道这件事。
            // 不补上这段宽度，最后一个字就会被 buf 裁掉一半
            // （之前 "RAYCAST" 的 T 只剩一根竖线，就是这个原因）。
            if (tracking != 0) sz.cx += tracking * (len - 1);

            if (sz.cx + 8 > w_ || sz.cy + 8 > h_)
            {
                // 估算不够（多数是 CJK 或长串），按实测再扩一次
                const int needW = sz.cx + 8, needH = sz.cy + 8;
                if (!Ensure(needW, needH))
                {
                    SelectObject(dc_, oldFont);
                    DeleteObject(font);
                    return;
                }
                SelectObject(dc_, font);
            }

            const int bw = std::min<int>(w_, (int)sz.cx + 8);
            const int bh = std::min<int>(h_, (int)sz.cy + 8);

            // 只清文字框：清整个缓冲（最长可能 2400x300）在 20 条字符串上
            // 就是 700 万次写入，实测占 HUD 的一半
            RECT rc{ 0, 0, bw, bh };
            FillRect(dc_, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));

            SetBkMode(dc_, TRANSPARENT);
            SetTextColor(dc_, RGB(255, 255, 255));

            if (tracking == 0)
            {
                TextOutW(dc_, 4, 4, s, len);
            }
            else
            {
                int cx = 4;
                for (int i = 0; i < len; i++)
                {
                    wchar_t ch[2] = { s[i], 0 };
                    TextOutW(dc_, cx, 4, ch, 1);
                    SIZE csz{};
                    GetTextExtentPoint32W(dc_, ch, 1, &csz);
                    cx += csz.cx + tracking;
                }
            }

            SelectObject(dc_, oldFont);

            const int r = (color >> 16) & 0xFF;
            const int g = (color >> 8) & 0xFF;
            const int b = color & 0xFF;

            const uint32_t* bits = (const uint32_t*)bits_;
            for (int j = 0; j < bh; j++)
            {
                const uint32_t* row = bits + (size_t)j * (size_t)w_;
                for (int i = 0; i < bw; i++)
                {
                    const uint32_t v = row[i];
                    const int lum = (int)(v & 0xFF);      // 白字黑底，B 通道即覆盖率
                    if (lum == 0) continue;

                    const float cov = (lum / 255.0f) * alpha;

                    // 描影：叠层底下是什么内容我们控制不了（可能是白色的网页），
                    // 纯色字压上去就糊了。右下 1px 的暗影能救回来。
                    if (shadow) t.Blend(x + i - 3, y + j - 3, 0, 0, 0, cov * 0.55f);

                    t.Blend(x + i - 4, y + j - 4, r, g, b, cov);
                }
            }
        }

        // 量宽度（HUD 右对齐用）
        int Measure(const wchar_t* s, int px, bool bold = false)
        {
            if (!s || !*s) return 0;
            const int len = (int)wcslen(s);
            if (!Ensure(px * (len * 2 + 2) + 32, px * 2 + 20)) return 0;

            HFONT font = MakeFont(px, bold);
            if (!font) return 0;

            HGDIOBJ oldFont = SelectObject(dc_, font);
            SIZE sz{};
            GetTextExtentPoint32W(dc_, s, len, &sz);
            SelectObject(dc_, oldFont);

            return (int)sz.cx;
        }

    private:
        // 字体缓存：每帧 20 条字符串各 CreateFont 一次是白花钱，
        // GDI 建字体要查字体库，比画字本身还慢
        HFONT MakeFont(int px, bool bold)
        {
            const int key = px * 2 + (bold ? 1 : 0);
            auto it = fonts_.find(key);
            if (it != fonts_.end()) return it->second;

            HFONT f = CreateFontW(
                -px, 0, 0, 0,
                bold ? FW_SEMIBOLD : FW_NORMAL,
                FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                L"Microsoft YaHei");

            fonts_.emplace(key, f);
            return f;
        }

        bool Ensure(int needW, int needH)
        {
            if (needW < 16) needW = 16;
            if (needH < 16) needH = 16;
            if (needW > 2400) needW = 2400;
            if (needH > 300) needH = 300;

            if (dc_ && needW <= w_ && needH <= h_) return true;

            Release();

            w_ = needW;
            h_ = needH;

            HDC screen = GetDC(nullptr);
            dc_ = CreateCompatibleDC(screen);
            ReleaseDC(nullptr, screen);
            if (!dc_) return false;

            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w_;
            bi.bmiHeader.biHeight = -h_;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;

            bmp_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
            if (!bmp_) { Release(); return false; }

            old_ = SelectObject(dc_, bmp_);
            return true;
        }

        void Release()
        {
            for (auto& kv : fonts_)
                if (kv.second) DeleteObject(kv.second);
            fonts_.clear();

            if (dc_ && old_) SelectObject(dc_, old_);
            old_ = nullptr;
            if (bmp_) { DeleteObject(bmp_); bmp_ = nullptr; }
            if (dc_) { DeleteDC(dc_); dc_ = nullptr; }
            bits_ = nullptr;
            w_ = h_ = 0;
        }

        std::unordered_map<int, HFONT> fonts_;
        HDC dc_ = nullptr;
        HBITMAP bmp_ = nullptr;
        void* bits_ = nullptr;
        HGDIOBJ old_ = nullptr;
        int w_ = 0, h_ = 0;
    };

} // namespace holo
