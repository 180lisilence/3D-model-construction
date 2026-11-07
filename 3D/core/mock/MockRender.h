#pragma once

// ============================================================================
// mock/MockRender.h
//
// 极简软件渲染层，替代 原版 main.cpp 的 D3D11 + ImGui。
//
// 为什么不用 ImGui：原版那套要 d3d11 + imgui_impl_dx11 + swapchain，
// 而我们要证明的是「物理数据 -> 三角形 -> 屏幕坐标」这条链，
// 中间的 GPU 管线换成 CPU 逐像素反而更可控、也更好截图取证。
//
// 组成：
//   Canvas       32 位预乘 alpha 的 DIB，自己画线，GDI 画字
//   OverlayWin   WS_EX_LAYERED 全屏透明窗 + UpdateLayeredWindow
//   SaveBMP      无窗口模式落盘（用来在没有交互桌面的环境里取证）
//
// 关于 alpha：UpdateLayeredWindow 要求预乘 alpha，所以 Blend() 里
//   dst = src*a + dst*(1-a)
// 全程按预乘算。GDI 画字不会写 alpha（写成 0），所以画完字要跑一遍
// AlphaFix()：把「alpha==0 但 rgb!=0」的像素补成不透明。
// ============================================================================

#include "WinPre.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace mockrender
{
    // ---------------------------------------------------------------------
    // 画布
    // ---------------------------------------------------------------------
    class Canvas
    {
    public:
        Canvas() = default;
        ~Canvas() { Release(); }

        bool Create(int width, int height)
        {
            Release();
            w = width;
            h = height;

            HDC screen = GetDC(nullptr);
            dc = CreateCompatibleDC(screen);
            ReleaseDC(nullptr, screen);
            if (!dc) return false;

            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;          // 负 = 自上而下，行序与屏幕一致
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;

            bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!bmp) return false;

            oldBmp = SelectObject(dc, bmp);
            Clear();
            return true;
        }

        void Release()
        {
            if (dc && oldBmp) SelectObject(dc, oldBmp);
            oldBmp = nullptr;
            if (bmp) { DeleteObject(bmp); bmp = nullptr; }
            if (dc)  { DeleteDC(dc); dc = nullptr; }
            bits = nullptr;
        }

        void Clear()
        {
            if (bits) std::memset(bits, 0, (size_t)w * h * 4);
        }

        // 预乘 alpha 混合
        inline void Blend(int x, int y, int r, int g, int b, int a)
        {
            if (a <= 0 || x < 0 || y < 0 || x >= w || y >= h) return;
            uint32_t* row = reinterpret_cast<uint32_t*>(bits) + (size_t)y * w;
            uint32_t dst = row[x];
            const int ia = 255 - a;
            int dr = (dst >> 16) & 0xFF;
            int dg = (dst >> 8) & 0xFF;
            int db = dst & 0xFF;
            int nr = (r * a + dr * ia) / 255;
            int ng = (g * a + dg * ia) / 255;
            int nb = (b * a + db * ia) / 255;
            int na = a + (int)((dst >> 24) & 0xFF) * ia / 255;
            if (na > 255) na = 255;
            row[x] = ((uint32_t)na << 24) | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb;
        }

        void Line(float x0, float y0, float x1, float y1, int r, int g, int b, int a)
        {
            if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1))
                return;
            // 防止近平面投影爆炸画出贯穿全屏的长线
            const float LIMIT = 20000.0f;
            if (std::fabs(x0) > LIMIT || std::fabs(y0) > LIMIT ||
                std::fabs(x1) > LIMIT || std::fabs(y1) > LIMIT)
                return;

            int ix0 = (int)std::lround(x0), iy0 = (int)std::lround(y0);
            int ix1 = (int)std::lround(x1), iy1 = (int)std::lround(y1);

            int dx = std::abs(ix1 - ix0), sx = ix0 < ix1 ? 1 : -1;
            int dy = -std::abs(iy1 - iy0), sy = iy0 < iy1 ? 1 : -1;
            int err = dx + dy;

            for (;;)
            {
                Blend(ix0, iy0, r, g, b, a);
                if (ix0 == ix1 && iy0 == iy1) break;
                int e2 = err * 2;
                if (e2 >= dy) { err += dy; ix0 += sx; }
                if (e2 <= dx) { err += dx; iy0 += sy; }
            }
        }

        void Rect(int x, int y, int rw, int rh, int r, int g, int b, int a)
        {
            for (int i = y; i < y + rh; i++)
                for (int j = x; j < x + rw; j++)
                    Blend(j, i, r, g, b, a);
        }

        // 圆角矩形。四角用 1px 软边（按覆盖率混合），免得锯齿扎眼。
        // 只用于 HUD 面板，面积小，逐像素判定足够快。
        void RoundRect(int x, int y, int rw, int rh, int radius,
                       int r, int g, int b, int a)
        {
            if (rw <= 0 || rh <= 0) return;
            if (radius < 0) radius = 0;
            if (radius > rw / 2) radius = rw / 2;
            if (radius > rh / 2) radius = rh / 2;

            for (int i = 0; i < rh; i++)
            {
                for (int j = 0; j < rw; j++)
                {
                    if (radius > 0)
                    {
                        // 判断是否落在四角的圆外
                        int cxp = j, cyp = i;
                        int ox = 0, oy = 0;
                        if (j < radius)                     ox = radius - j;
                        else if (j >= rw - radius)           ox = j - (rw - radius - 1);
                        if (i < radius)                     oy = radius - i;
                        else if (i >= rh - radius)           oy = i - (rh - radius - 1);

                        if (ox > 0 && oy > 0)
                        {
                            const float d = std::sqrt((float)(ox * ox + oy * oy));
                            if (d > radius) continue;
                        }
                    }
                    Blend(x + j, y + i, r, g, b, a);
                }
            }
        }

        // GDI 画字（DIB 上直接画，画完补 alpha）
        void Text(int x, int y, const wchar_t* text, int px,
                  int r, int g, int b, int a = 255, bool bold = false)
        {
            if (!dc || !text || !*text) return;

            HFONT font = CreateFontW(
                -px, 0, 0, 0,
                bold ? FW_SEMIBOLD : FW_NORMAL,
                FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                L"Microsoft YaHei");
            HGDIOBJ oldFont = SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(r, g, b));

            // a 只影响最终 alpha，GDI 不知道 alpha，画完由 AlphaFix 统一补
            TextOutW(dc, x, y, text, (int)wcslen(text));
            (void)a;

            SelectObject(dc, oldFont);
            DeleteObject(font);
        }

        // alpha==0 但 rgb!=0 的像素 = GDI 刚写上去的字，补成不透明
        void AlphaFix()
        {
            uint32_t* p = reinterpret_cast<uint32_t*>(bits);
            const size_t n = (size_t)w * h;
            for (size_t i = 0; i < n; i++)
            {
                uint32_t v = p[i];
                if ((v & 0xFF000000u) == 0 && (v & 0x00FFFFFFu) != 0)
                    p[i] = v | 0xFF000000u;
            }
        }

        // 按预乘转成普通 alpha（BMP 用不到，调试查看时有用）
        bool SaveBMP(const wchar_t* path)
        {
            if (!bits) return false;

            const int rowBytes = w * 4;
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            ih.biSize = sizeof(ih);
            ih.biWidth = w;
            ih.biHeight = -h;
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            ih.biCompression = BI_RGB;
            ih.biSizeImage = (DWORD)rowBytes * h;

            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + ih.biSizeImage;

            FILE* f = nullptr;
            if (_wfopen_s(&f, path, L"wb") != 0 || !f) return false;
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&ih, sizeof(ih), 1, f);
            fwrite(bits, 1, ih.biSizeImage, f);
            fclose(f);
            return true;
        }

        int Width() const { return w; }
        int Height() const { return h; }
        HDC Dc() const { return dc; }

        // 直接暴露像素缓冲，供 software rasterizer 整体拷入。
        // 布局是 32bpp 的 0xAARRGGBB（小端下内存顺序为 B,G,R,A），与 DIB 一致。
        uint32_t* Pixels() { return reinterpret_cast<uint32_t*>(bits); }
        const uint32_t* Pixels() const { return reinterpret_cast<const uint32_t*>(bits); }
        size_t PixelCount() const { return (size_t)w * h; }

    private:
        int w = 0, h = 0;
        HDC dc = nullptr;
        HBITMAP bmp = nullptr;
        void* bits = nullptr;
        HGDIOBJ oldBmp = nullptr;
    };

    // ---------------------------------------------------------------------
    // 透明叠层窗口
    // ---------------------------------------------------------------------
    class OverlayWindow
    {
    public:
        bool Create(const wchar_t* title, int width, int height)
        {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"PhysxMockOverlay";
            RegisterClassExW(&wc);

            hwnd = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                wc.lpszClassName, title,
                WS_POPUP,
                0, 0, width, height,
                nullptr, nullptr, wc.hInstance, nullptr);

            if (!hwnd) return false;

            ShowWindow(hwnd, SW_SHOW);
            UpdateWindow(hwnd);
            return true;
        }

        void Present(Canvas& canvas)
        {
            if (!hwnd) return;

            HDC screen = GetDC(nullptr);
            POINT src{ 0, 0 };
            POINT dst{ 0, 0 };
            SIZE size{ canvas.Width(), canvas.Height() };
            BLENDFUNCTION blend{};
            blend.BlendOp = AC_SRC_OVER;
            blend.SourceConstantAlpha = 255;
            blend.AlphaFormat = AC_SRC_ALPHA;

            UpdateLayeredWindow(hwnd, screen, &dst, &size,
                canvas.Dc(), &src, 0, &blend, ULW_ALPHA);
            ReleaseDC(nullptr, screen);
        }

        // 处理窗口消息；返回 false 表示该退出了
        bool Pump()
        {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT) return false;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (GetAsyncKeyState(VK_F8) & 0x8000) return false;
            return true;
        }

        HWND Handle() const { return hwnd; }

    private:
        HWND hwnd = nullptr;
    };
} // namespace mockrender
