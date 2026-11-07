#pragma once

// ============================================================================
// mock/Aurora.h  -  极光天空
//
// ---- 配色为什么是这几个颜色 ----
// 极光的颜色不是随便调的，它来自高层大气里几种原子/分子的电子跃迁。
// 按高度从低到高（也是从下往上看到的顺序）：
//
//   557.7 nm  氧原子 O(1S -> 1D)     亮黄绿   ~100-150km  ← 最常见，极光"绿"
//   427.8 nm  氮分子 N2+ 第一负带     蓝紫     ~100km 以下  ← 帘幕底部边缘
//   630.0 nm  氧原子 O(1D -> 3P)     暗红     ~200-400km  ← 高处，弱，强活动才明显
//   红/品红   氮分子 + 氧的高能态     品红紫   高纬度强活动
//
// 所以真实的极光帘幕是：底边一条偏蓝紫的锐利边 -> 往上整片黄绿/翠绿 ->
// 再往上淡成青 -> 顶部泛紫品红。照片里那种"纯绿一片"是相机过曝的结果。
//
// ---- 形态为什么这么做 ----
// 极光帘幕（auroral curtain）有三个标志性特征，少一个就不像：
//   1. 下边缘锐利，而且是一条缓慢起伏的波浪线（不是渐变）
//   2. 向上发散、越往上越淡，顶部散成丝状
//   3. 竖直的细密"丝缕"（rays），像被风吹皱的窗帘 —— 这是最关键的一条
//
// ---- 性能 ----
// 强度和颜色做成 256 级 LUT（一次构建，全屏复用）。
// 内层像素循环里只有查表 + 一次混合，没有 sin / pow。
// sin 只在外层每列算一次（1920 列 ≈ 0.3ms）。
// ============================================================================

#include "HoloOverlay.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace aurora
{
    struct Params
    {
        float intensity = 1.00f;   // 总亮度，0 关闭
        float speed     = 1.00f;   // 漂移速度
        float horizon   = 0.66f;   // 天空下边界在屏幕高度的比例
        float band      = 0.55f;   // 帘幕高度（屏幕高度的比例）
        float edge      = 0.85f;   // 帘幕下边缘的基础位置（相对天空高度）
        float rays      = 1.00f;   // 丝缕强度
        bool  background = true;   // true: 先铺不透明深空（全屏启动动画）
                                   // false: 只叠极光，背景保持透明（桌面叠层）
        bool  stars      = true;   // 星点
    };

    namespace detail
    {
        // 真实极光的发射色标（s 是"从帘幕底边到顶部"的归一化高度）
        struct Stop { float s, r, g, b; };

        static const Stop kStops[6] = {
            { 0.00f, 150.0f, 255.0f, 120.0f },   // 底边：黄绿
            { 0.16f,  40.0f, 255.0f, 150.0f },   // 翠绿
            { 0.38f,   0.0f, 225.0f, 205.0f },   // 青
            { 0.60f,  40.0f, 150.0f, 255.0f },   // 蓝紫（427.8nm）
            { 0.80f, 130.0f,  90.0f, 250.0f },   // 紫
            { 1.00f, 210.0f,  70.0f, 220.0f },   // 品红
        };

        inline void RampColor(float s, float& r, float& g, float& b)
        {
            if (s < 0.0f) s = 0.0f;
            if (s > 1.0f) s = 1.0f;

            int i = 0;
            while (i < 4 && s > kStops[i + 1].s) i++;

            const Stop& a = kStops[i];
            const Stop& c = kStops[i + 1];
            const float span = c.s - a.s;
            const float f = (span > 1.0e-5f) ? (s - a.s) / span : 0.0f;

            r = a.r + (c.r - a.r) * f;
            g = a.g + (c.g - a.g) * f;
            b = a.b + (c.b - a.b) * f;

            // 贴地平线的那一圈常带一点红色（630nm 氧原子，强活动才明显），
            // 只给最底下 8%，多了就变成火灾。
            if (s < 0.08f)
            {
                const float k = 1.0f - s / 0.08f;
                r += 110.0f * k;
                g -= 40.0f * k;
            }
        }

        struct Lut
        {
            float a[256];
            float r[256], g[256], b[256];

            void Build()
            {
                for (int i = 0; i < 256; i++)
                {
                    const float s = (float)i / 255.0f;

                    // 底边强、向上衰减。指数从 1.55 降到 1.12：原来的曲线
                    // 在 s>0.8 处几乎归零，画面上就只剩绿色，看不到顶部的
                    // 蓝紫和品红 —— 而那几个颜色才是"真实极光"和"绿色光晕"
                    // 的分界线。
                    float v = std::pow(1.0f - s, 1.12f);
                    v *= (1.0f - 0.22f * s * s);
                    a[i] = v;

                    float rr, gg, bb;
                    RampColor(s, rr, gg, bb);
                    r[i] = rr;
                    g[i] = gg;
                    b[i] = bb;
                }
            }
        };
    } // namespace detail

    namespace detail
    {
        // 背景已经是不透明深空时，极光可以直接加色：
        // 少一次 alpha 回写和一次乘法，实测比 Blend 快约 40%。
        // 桌面叠层模式（background=false）没有底板，只能走 Blend。
        inline void AddPx(holo::Target& t, int x, int y, int r, int g, int b, float a)
        {
            uint32_t* p = t.px + (size_t)y * (size_t)t.w + x;
            const uint32_t d = *p;
            int nr = (int)((d >> 16) & 0xFF) + (int)((float)r * a);
            int ng = (int)((d >> 8) & 0xFF) + (int)((float)g * a);
            int nb = (int)(d & 0xFF) + (int)((float)b * a);
            if (nr > 255) nr = 255;
            if (ng > 255) ng = 255;
            if (nb > 255) nb = 255;
            *p = 0xFF000000u | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb;
        }
    } // namespace detail

    // ------------------------------------------------------------------
    // 把极光铺到 target 上。
    // 必须在 3D 几何之前调用 —— 之后画的线和面自然压在极光上面。
    // ------------------------------------------------------------------
    inline void Draw(holo::Target& t, float time, const Params& p)
    {
        if (!t.px || t.w <= 0 || t.h <= 0) return;
        if (p.intensity <= 0.001f) return;

        static detail::Lut lut;
        static bool lutReady = false;
        if (!lutReady) { lut.Build(); lutReady = true; }

        const int   W = t.w;
        const int   H = t.h;
        const float skyBottomF = (float)H * p.horizon;
        int         skyBottom = (int)skyBottomF;
        if (skyBottom > H) skyBottom = H;
        if (skyBottom < 1) skyBottom = 1;

        const float bandPx = (float)H * p.band;
        const float tsec   = time * p.speed;

        // ---------------- 1) 深空背景 ----------------
        if (p.background)
        {
            for (int y = 0; y < H; y++)
            {
                int r, g, b;
                if (y < skyBottom)
                {
                    const float k = (float)y / skyBottomF;      // 0 顶 / 1 地平线
                    r = (int)(7.0f + 16.0f * k);
                    g = (int)(10.0f + 21.0f * k);
                    b = (int)(26.0f + 40.0f * k);
                }
                else
                {
                    // 地平线以下：夜里的地面。比天空还暗，但留一点冷色，
                    // 免得 3D 网格画上去之后像悬空在纯黑里。
                    const float k = ((float)y - skyBottomF) / (float)(H - skyBottom);
                    r = (int)(10.0f - 5.0f * k);
                    g = (int)(13.0f - 6.0f * k);
                    b = (int)(20.0f - 8.0f * k);
                }
                if (r < 0) r = 0;
                if (g < 0) g = 0;
                if (b < 0) b = 0;

                const uint32_t c = 0xFF000000u
                                 | ((uint32_t)r << 16)
                                 | ((uint32_t)g << 8)
                                 | (uint32_t)b;

                uint32_t* row = t.px + (size_t)y * (size_t)W;
                for (int x = 0; x < W; x++) row[x] = c;
            }

            // 星点：坐标哈希，不占内存，也不会随时间乱跳
            if (p.stars)
            {
                const int yEnd = (int)(skyBottomF * 0.55f);
                for (int y = 1; y < yEnd; y += 3)
                {
                    for (int x = 1; x < W; x += 3)
                    {
                        const uint32_t hsh = ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u);
                        if ((hsh & 1023u) >= 5u) continue;

                        const float b0 = 0.30f + 0.55f * (float)((hsh >> 10) & 255u) / 255.0f;
                        const float tw = 0.72f + 0.28f * std::sin(tsec * 1.7f + (float)(hsh & 63u));
                        t.Blend(x, y, 205, 222, 255, b0 * tw * 0.85f);
                    }
                }
            }
        }

        // ---------------- 2) 极光帘幕 ----------------
        // 两条帘幕：一条低、一条高。真实极光经常是分层出现的，
        // 单层会显得太"平"。
        struct Curtain
        {
            float heightMul;   // 高度倍率
            float edgeMul;     // 下边缘位置倍率（1 = 靠下）
            float alpha;       // 整体强度
            float phase;       // 时间相位偏移
            float freqMul;     // 波浪频率倍率
            float colorShift;  // 色标偏移：正值往紫红端推
        };

        static const Curtain kCurtains[2] = {
            { 1.00f, 1.00f, 0.78f, 0.00f, 1.00f,   0.00f },  // 主帘幕（低、绿）
            { 1.10f, 0.52f, 0.55f, 2.30f, 0.62f, 105.00f },  // 上层帘幕（高、偏紫）
        };

        for (int ci = 0; ci < 2; ci++)
        {
            const Curtain& cu = kCurtains[ci];
            const float bandH = bandPx * cu.heightMul;
            const float ts    = tsec + cu.phase;

            for (int x = 0; x < W; x++)
            {
                const float u = (float)x / (float)W;

                // --- 下边缘的波浪（三个不可公约频率，看不出周期）---
                float wave = 0.0f;
                wave += 0.62f * std::sin(u * 5.83f * cu.freqMul + ts * 0.29f);
                wave += 0.28f * std::sin(u * 13.70f * cu.freqMul - ts * 0.44f + 1.7f);
                wave += 0.10f * std::sin(u * 31.10f * cu.freqMul + ts * 0.71f + 3.4f);

                const float edgeBase = skyBottomF * p.edge * cu.edgeMul;
                const float edge = edgeBase + wave * bandH * 0.26f;
                const float top  = edge - bandH * (0.82f + 0.18f * std::sin(u * 7.3f + ts * 0.17f));

                if (top >= edge) continue;

                // --- 帘幕疏密：低频强弱，形成一片一片的明暗 ---
                float dens = 0.40f + 0.60f * (0.5f + 0.5f * std::sin(u * 4.10f + ts * 0.23f + 0.9f));

                // --- 丝缕：两层，宽的做褶皱、窄的做细丝 ---
                float rayWide = 0.5f + 0.5f * std::sin(u * 46.0f + ts * 0.9f + wave * 2.0f);
                rayWide = 0.35f + 0.65f * rayWide * rayWide;
                // 相位上叠一层低频扰动，条纹间距就不会整齐得像百叶窗
                float rayFine = 0.5f + 0.5f * std::sin(u * 213.0f
                                    + std::sin(u * 7.3f + ts * 0.41f) * 1.9f
                                    + ts * 1.6f);
                // 原来收到 7 次方，条纹细得像下雨；3 次方是"光幕褶皱"的量级
                rayFine = std::pow(rayFine, 3.0f);

                float rays = (0.62f + 0.38f * rayWide) * (0.70f + 0.30f * rayFine);
                rays = 1.0f - (1.0f - rays) * p.rays;            // p.rays=0 时变成均匀发光

                const float gain = cu.alpha * p.intensity * dens * rays;
                if (gain <= 0.002f) continue;

                const float invSpan = 255.0f / (edge - top);

                int y0 = (int)top;
                int y1 = (int)edge;
                if (y0 < 0) y0 = 0;
                if (y1 > skyBottom) y1 = skyBottom;
                if (y0 >= y1) continue;

                for (int y = y0; y < y1; y++)
                {
                    int si = (int)((edge - (float)y) * invSpan);
                    if (si < 0) si = 0;
                    if (si > 255) si = 255;

                    int sc = si + (int)cu.colorShift;
                    if (sc < 0) sc = 0;
                    if (sc > 255) sc = 255;

                    float a = lut.a[si] * gain;

                    // 迫近地平线时淡出，否则极光会糊到地面网格上，
                    // 看起来像地面上铺了一层绿光。
                    const float hk = (float)y / skyBottomF;
                    if (hk > 0.88f) a *= (1.0f - (hk - 0.88f) / 0.12f);

                    if (a <= 0.003f) continue;
                    if (a > 1.0f) a = 1.0f;

                    if (p.background)
                        detail::AddPx(t, x, y, (int)lut.r[sc], (int)lut.g[sc], (int)lut.b[sc], a);
                    else
                        t.Blend(x, y, (int)lut.r[sc], (int)lut.g[sc], (int)lut.b[sc], a);
                }
            }
        }

        // ---------------- 3) 地平线辉光 ----------------
        // 极光打到地面/雾气上的一层冷光，让天空和地面之间有过渡，
        // 否则极光的底边会像被刀切过一样。
        if (p.background)
        {
            const int gh = (int)((float)H * 0.16f);
            for (int i = 0; i < gh; i++)
            {
                const int y = skyBottom - gh + i;
                if (y < 0 || y >= H) continue;

                const float k = (float)i / (float)gh;      // 0 上 1 下
                const float a = 0.14f * (1.0f - k) * (1.0f - k) * p.intensity;

                uint32_t* row = t.px + (size_t)y * (size_t)W;
                for (int x = 0; x < W; x++)
                {
                    // 预乘背景，直接加一层冷绿
                    const uint32_t d = row[x];
                    const int dr = (int)((d >> 16) & 0xFF);
                    const int dg = (int)((d >> 8) & 0xFF);
                    const int db = (int)(d & 0xFF);

                    int nr = dr + (int)(40.0f * a);
                    int ng = dg + (int)(120.0f * a);
                    int nb = db + (int)(95.0f * a);
                    if (nr > 255) nr = 255;
                    if (ng > 255) ng = 255;
                    if (nb > 255) nb = 255;

                    row[x] = 0xFF000000u | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb;
                }
            }
        }
    }
} // namespace aurora
