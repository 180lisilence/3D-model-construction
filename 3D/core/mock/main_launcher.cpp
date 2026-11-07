// ============================================================================
// mock/main_launcher.cpp  -  启动器的启动动画界面
//
// 和 main_mock.cpp（桌面透明叠层）的关系：
//   两个 exe 共用同一套场景、同一套软件光栅器，区别只在"壳"：
//     main_mock      透明穿透窗口，叠在桌面上，可交互（转视角/射线/HUD）
//     main_launcher  不透明全屏窗口，铺极光天空，无人值守播放完自动退出
//
// 启动动画的时间轴：
//   0.0s            黑场起步，相机在很远处（R=2750）
//   0.0 ~ 2.4s      淡入 + 相机推近（ease-out cubic），仰角略降
//   2.4 ~ N-1.4s    缓慢环绕，极光持续漂移
//   N-1.4s ~ N      淡出
//   ESC / 空格 / 点击 立即进入 0.35s 快速淡出
//
// 为什么用 UpdateLayeredWindow 而不是普通窗口 + BitBlt：
//   普通窗口每帧 BitBlt 会闪（先清背景再画），全屏尤其明显。
//   分层窗口把整张位图连同 alpha 一次性交给 DWM 合成，不闪，
//   而且 SourceConstantAlpha 正好可以拿来做全局淡入淡出。
// ============================================================================

#include <windows.h>
#include <mmsystem.h>

#include "mock/DMALibrary/Memory/Memory.h"
#include "mock/MockScene.h"
#include "mock/MockRender.h"
#include "mock/HoloOverlay.h"
#include "mock/Aurora.h"

#include "util/Data.h"
#include "util/VectorHelper.h"
#include "util/VisibleScene.h"
#include "util/LineTrace.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Data.h 里是 extern，这里给定义
FGameData GameData{};

using namespace physx;

namespace
{
    constexpr float kRoomX = 2000.0f;
    constexpr float kRoomY = -2000.0f;

    using Clock = std::chrono::high_resolution_clock;

    bool ParseSize(const char* s, int& w, int& h)
    {
        int a = 0, b = 0;
        if (sscanf_s(s, "%dx%d", &a, &b) != 2) return false;
        if (a < 64 || b < 64) return false;
        w = a; h = b;
        return true;
    }

    // ------------------------------------------------------------------
    // 相机
    // ------------------------------------------------------------------
    struct CamRig
    {
        Vector3 eye;
        Vector3 target;
        float   fovX = 74.0f;
    };

    // 入场：从远处推近，同时把仰角压低一点（高处俯视 -> 与人眼接近）
    CamRig MakeIntroCamera(float t)
    {
        const float intro = 2.4f;
        float k = t / intro;
        if (k > 1.0f) k = 1.0f;
        if (k < 0.0f) k = 0.0f;

        // ease-out cubic：起步快、收尾慢，像镜头被稳稳停住
        const float e = 1.0f - std::pow(1.0f - k, 3.0f);

        const float R = 2650.0f - 1120.0f * e;    // 2650 -> 1530
        const float z = 1000.0f - 420.0f * e;     // 1000 -> 580
        const float ang = -0.62f + t * 0.085f;    // 缓慢右移环绕

        CamRig r;
        r.eye = Vector3(kRoomX + std::cos(ang) * R, kRoomY + std::sin(ang) * R, z);
        r.target = Vector3(kRoomX, kRoomY, 155.0f);
        r.fovX = 68.0f;
        return r;
    }

    // ------------------------------------------------------------------
    // 启动动画窗口：不透明全屏分层窗口
    // ------------------------------------------------------------------
    class AnimWindow
    {
    public:
        bool Create(const wchar_t* title, int w, int h, bool fullscreen)
        {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_OWNDC;
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"AuroraLauncherWindow";
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
            if (!RegisterClassExW(&wc)) return false;

            int x = 0, y = 0;
            if (fullscreen)
            {
                w = GetSystemMetrics(SM_CXSCREEN);
                h = GetSystemMetrics(SM_CYSCREEN);
            }
            else
            {
                x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
                y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
            }

            // 不带 WS_EX_TRANSPARENT：启动动画要能接住 ESC / 点击
            hwnd = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TOPMOST,
                wc.lpszClassName, title,
                WS_POPUP,
                x, y, w, h,
                nullptr, nullptr, wc.hInstance, nullptr);

            if (!hwnd) return false;

            fullscreen_ = fullscreen;
            ShowWindow(hwnd, SW_SHOWNORMAL);
            UpdateWindow(hwnd);
            SetForegroundWindow(hwnd);
            return true;
        }

        // alpha: 0..255 全局透明度，用来做淡入淡出
        // tw/th:  目标窗口尺寸。比画布大时由系统拉伸 —— 软件光栅器按
        //         1600x900 渲染再拉到 1920x1080，帧时间直接省三分之一，
        //         而启动画面本来就是柔和的光，拉伸的软化反而是加分项。
        void Present(mockrender::Canvas& canvas, int alpha, int tw, int th)
        {
            if (!hwnd) return;
            if (alpha < 0) alpha = 0;
            if (alpha > 255) alpha = 255;

            HDC screen = GetDC(nullptr);
            POINT src{ 0, 0 };
            SIZE  size{ tw, th };

            // 全屏固定贴 (0,0)；窗口化传 nullptr，位置保持不变 ——
            // 否则每帧都被拽回屏幕左上角，拖不动。
            POINT pt{ 0, 0 };
            POINT* dst = fullscreen_ ? &pt : nullptr;

            BLENDFUNCTION blend{};
            blend.BlendOp = AC_SRC_OVER;
            blend.SourceConstantAlpha = (BYTE)alpha;
            blend.AlphaFormat = AC_SRC_ALPHA;

            UpdateLayeredWindow(hwnd, screen, dst, &size,
                                canvas.Dc(), &src, 0, &blend, ULW_ALPHA);
            ReleaseDC(nullptr, screen);
        }

        bool Pump()
        {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT) return false;
                if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) return false;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            return IsWindow(hwnd) != 0;
        }

    private:
        HWND hwnd = nullptr;
        bool fullscreen_ = true;
    };

} // namespace

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetProcessDPIAware();

    float seconds     = 9.0f;
    bool  fullscreen  = true;
    int   headlessFrames = -1;
    const char* outBmp = nullptr;
    int   forcedW = 0, forcedH = 0;

    float renderScale = 0.833f;   // 画布相对屏幕的比例
    float auroraGain  = 0.88f;
    float auroraSpeed = 1.00f;
    bool  noStars     = false;

    holo::DrawOptions opt = holo::DefaultOptions();

    // 启动动画的背景是满屏极光，比桌面亮得多。默认的 hardAlpha=0.80 /
    // glowAlpha=0.16 是给深色桌面调的，套在极光上房子的线会被背景吃掉，
    // 看起来像"半透明的玻璃盒"。这里单独提亮一档。
    opt.hardAlpha = 0.95f;
    opt.glowAlpha = 0.26f;
    opt.fillAlpha = 0.075f;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--headless") == 0 && i + 2 < argc)
        {
            headlessFrames = atoi(argv[++i]);
            outBmp = argv[++i];
        }
        else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc)
        {
            if (!ParseSize(argv[++i], forcedW, forcedH))
                printf("[!] --size 格式应为 1600x900，忽略\n");
        }
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
        {
            seconds = (float)atof(argv[++i]);
            if (seconds < 1.0f) seconds = 1.0f;
        }
        else if (strcmp(argv[i], "--windowed") == 0)
        {
            fullscreen = false;
        }
        else if (strcmp(argv[i], "--aurora") == 0 && i + 1 < argc)
        {
            auroraGain = (float)atof(argv[++i]);
        }
        else if (strcmp(argv[i], "--aurora-speed") == 0 && i + 1 < argc)
        {
            auroraSpeed = (float)atof(argv[++i]);
        }
        else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
        {
            renderScale = (float)atof(argv[++i]);
            if (renderScale < 0.4f) renderScale = 0.4f;
            if (renderScale > 1.0f) renderScale = 1.0f;
        }
        else if (strcmp(argv[i], "--no-stars") == 0)
        {
            noStars = true;
        }
        else if (strcmp(argv[i], "--style") == 0 && i + 1 < argc)
        {
            const char* s = argv[++i];
            if (strcmp(s, "wire") == 0)
            {
                opt.fill = false;
                opt.softAlpha = 0.13f;
                opt.glowAlpha = 0.10f;
            }
        }
        else if (strcmp(argv[i], "--help") == 0)
        {
            printf("用法:\n");
            printf("  AuroraLauncher.exe                        全屏启动动画（默认 9 秒）\n");
            printf("  AuroraLauncher.exe --seconds 6            指定时长（秒）\n");
            printf("  AuroraLauncher.exe --windowed --size 1280x720   窗口模式\n");
            printf("  AuroraLauncher.exe --aurora 0.6           极光亮度 0..1\n");
            printf("  AuroraLauncher.exe --aurora-speed 2       极光漂移速度\n");
            printf("  AuroraLauncher.exe --no-stars             关掉星点\n");
            printf("  AuroraLauncher.exe --style wire           纯线框\n");
            printf("  AuroraLauncher.exe --headless 120 out.bmp 无窗口渲染并落盘\n");
            return 0;
        }
    }

    printf("=== Aurora Launcher · 启动动画 ===\n");
    printf("构建合成 PhysX 场景...\n");

    auto demo = mockscene::BuildDemo();
    GameData.PhysxInstancePtr = demo.physxInstance;
    GameData.WorldOriginLocation = FVector(0, 0, 0);
    GameData.LoadRadius = 5000.0f;
    GameData.PhysxRefreshLimit = 0;

    GameData.DynamicLoadScene  = new Physics::VisibleScene<PrunerPayload, PrunerPayloadHash>(Physics::prunerPayloadExtractor);
    GameData.DynamicRigidScene = new Physics::VisibleScene<PrunerPayload, PrunerPayloadHash>(Physics::prunerPayloadExtractor);
    GameData.HeightFieldScene  = new Physics::VisibleScene<uint64_t, Int64Hash>(Physics::int64Extractor);

    // ---- 地形：一次性加载 ----
    auto HeightFieldFilter = [](const ShapeDataT& shape_data)
    {
        PxGeometryType type = shape_data.mType;
        if (type == PxGeometryType::eHEIGHTFIELD)
        {
            PxHeightFieldGeometryT field_geometry = (PxHeightFieldGeometryT&)shape_data.Shape.GetGeometry();
            if (field_geometry.RowScale != 100.f) return true;
            return false;
        }
        return true;
    };
    {
        auto hfMeshes = physx::GetMeshData(demo.staticObjects, HeightFieldFilter, false);
        GameData.HeightFieldScene->UpdateMesh(hfMeshes, {});
        printf("  地形网格 %zu 个\n", hfMeshes.size());
    }

    // ---- 画布 / 目标 ----
    // screenW/H 是窗口尺寸，W/H 是实际渲染尺寸（可能更小，靠系统拉伸）
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    int W = forcedW, H = forcedH;
    if (W <= 0 || H <= 0)
    {
        W = (int)((float)screenW * renderScale);
        H = (int)((float)screenH * renderScale);
        if (W < 640) W = 640;
        if (H < 360) H = 360;
    }

    mockrender::Canvas canvas;
    if (!canvas.Create(W, H))
    {
        printf("创建 %dx%d 画布失败\n", W, H);
        return 1;
    }

    holo::Target target;
    target.Attach(canvas.Pixels(), W, H);

    soft3d::Camera cam;
    soft3d::Shading shading;

    AnimWindow window;
    bool windowed = (headlessFrames < 0);
    if (windowed)
    {
        if (!window.Create(L"Aurora Launcher", W, H, fullscreen))
        {
            printf("[!] 创建窗口失败，转入无窗口模式\n");
            windowed = false;
            headlessFrames = 60;
        }
        else
        {
            printf("  窗口 %dx%d %s\n", W, H, fullscreen ? "全屏" : "窗口化");
        }
    }

    printf("  画布 %dx%d  时长 %.1fs  极光 %.2f\n", W, H, seconds, auroraGain);

    // ---- 逐帧状态（原版加载器的差值缓存）----
    struct SceneState
    {
        uint32_t lastTimestamp = 0;
        std::unordered_map<PrunerPayload, PxTransformT, PrunerPayloadHash> staticCache;
        std::unordered_map<PrunerPayload, PxTransformT, PrunerPayloadHash> dynamicCache;
        std::unordered_map<PrunerPayload, uint64_t, PrunerPayloadHash> ptrCache;
        std::unordered_map<PrunerPayload, uint64_t, PrunerPayloadHash> alwaysCheckShape;
        std::set<PrunerPayload> staticInScene;
        std::set<PrunerPayload> dynamicInScene;
        std::set<PrunerPayload> staticRemove;
        std::set<PrunerPayload> dynamicRemove;
    } state;

    std::vector<TriangleMeshData*> terrainMeshes;
    std::vector<TriangleMeshData*> staticMeshes;
    std::vector<TriangleMeshData*> dynamicMeshes;

    aurora::Params ap;
    ap.intensity  = auroraGain;
    ap.speed      = auroraSpeed;
    ap.background = true;
    ap.stars      = !noStars;

    // 提高 Sleep 精度，否则帧率会被 15.6ms 的时钟粒度锁死在 30~60 之间跳
    timeBeginPeriod(1);

    const auto t0 = Clock::now();
    float endTime = seconds;
    bool  skipLatched = false;

    int frame = 0;
    for (;;)
    {
        const auto frameStart = Clock::now();

        // 窗口模式用真实时间（掉帧也不会跳）；headless 用帧号换算成虚拟时间，
        // 否则机器一快，60 帧只跑到 0.8 秒，截图全是淡入中的黑场。
        const float t = windowed
            ? std::chrono::duration<float>(Clock::now() - t0).count()
            : (float)frame / 60.0f;

        if (windowed)
        {
            if (!window.Pump()) break;

            // 跳过：ESC / 空格 / 鼠标左键
            if (!skipLatched &&
                ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) ||
                 (GetAsyncKeyState(VK_SPACE) & 0x8000) ||
                 (GetAsyncKeyState(VK_LBUTTON) & 0x8000)))
            {
                skipLatched = true;
                endTime = std::min(endTime, t + 0.40f);
            }
        }

        if (t >= endTime) break;

        demo.Tick(t);

        const CamRig rig = MakeIntroCamera(t);

        // ---- 加载 ----
        {
            auto meshes = physx::LoadShapeByRange(
                state.lastTimestamp, state.staticCache, state.staticInScene,
                state.staticRemove, state.alwaysCheckShape,
                rig.eye, GameData.LoadRadius, 0);
            if (!meshes.empty() || !state.staticRemove.empty())
                GameData.DynamicLoadScene->UpdateMesh(meshes, state.staticRemove);
            state.staticRemove.clear();
        }
        {
            auto meshes = physx::LoadDynamicRigidShape(
                state.dynamicInScene, state.dynamicCache, state.ptrCache,
                state.dynamicRemove, rig.eye, GameData.LoadRadius);
            if (!meshes.empty() || !state.dynamicRemove.empty())
                GameData.DynamicRigidScene->UpdateMesh(meshes, state.dynamicRemove);
            state.dynamicRemove.clear();
        }

        {
            const FVector eyePos(rig.eye.x, rig.eye.y, rig.eye.z);
            terrainMeshes = GameData.HeightFieldScene->GetNearMesh(eyePos, 1e12);
            staticMeshes  = GameData.DynamicLoadScene->GetNearMesh(eyePos, 1e12);
            dynamicMeshes = GameData.DynamicRigidScene->GetNearMesh(eyePos, 1e12);
        }

        opt.terrainEdgeRange = std::min(2400.0f, std::max(760.0f, rig.eye.z * 1.55f));

        const float aspect = (float)W / (float)H;
        const float fovY = 2.0f * std::atan(std::tan(rig.fovX * 3.14159265f / 360.0f) / aspect) * 57.2957795f;
        cam.eye = rig.eye;
        cam.target = rig.target;
        cam.fovY = std::min(120.0f, std::max(20.0f, fovY));
        cam.zNear = 10.0f;
        cam.LookAt(W, H);

        // ---- 渲染 ----
        canvas.Clear();
        target.Begin();

        // 1) 极光天空（铺满画面，作为不透明底）
        aurora::Draw(target, t, ap);

        // 2) 深度预通道
        for (auto* m : terrainMeshes) if (m) holo::DrawMeshDepth(target, cam, *m);
        for (auto* m : staticMeshes)  if (m) holo::DrawMeshDepth(target, cam, *m);
        for (auto* m : dynamicMeshes) if (m) holo::DrawMeshDepth(target, cam, *m);

        // 3) 面
        if (opt.fill)
        {
            if (opt.fillTerrain)
                for (auto* m : terrainMeshes) if (m) holo::DrawMeshFill(target, cam, *m, holo::Layer::Terrain, opt, false, shading);
            for (auto* m : staticMeshes)  if (m) holo::DrawMeshFill(target, cam, *m, holo::Layer::Static,  opt, false, shading);
            for (auto* m : dynamicMeshes) if (m) holo::DrawMeshFill(target, cam, *m, holo::Layer::Dynamic, opt, false, shading);
        }

        // 4) 线
        for (auto* m : terrainMeshes) if (m) holo::DrawMeshEdges(target, cam, *m, holo::Layer::Terrain, opt, false);
        for (auto* m : staticMeshes)  if (m) holo::DrawMeshEdges(target, cam, *m, holo::Layer::Static,  opt, false);
        for (auto* m : dynamicMeshes) if (m) holo::DrawMeshEdges(target, cam, *m, holo::Layer::Dynamic, opt, false);

        // ---- 全局淡入淡出 ----
        float ga = 1.0f;
        const float fadeIn = 0.9f;
        const float fadeOut = skipLatched ? 0.40f : 1.4f;

        if (t < fadeIn) ga = t / fadeIn;
        if (t > endTime - fadeOut) ga = std::min(ga, (endTime - t) / fadeOut);
        if (ga < 0.0f) ga = 0.0f;
        if (ga > 1.0f) ga = 1.0f;

        if (windowed)
        {
            // 全屏：把 1600x900 的画布拉伸铺满屏幕
            // 窗口化：目标和画布一样大，否则 UpdateLayeredWindow 会把窗口
            //         直接拉到全屏（它的 psize 参数会改窗口尺寸）
            window.Present(canvas, (int)(ga * 255.0f),
                           fullscreen ? screenW : W,
                           fullscreen ? screenH : H);
        }
        else if (outBmp && (frame == headlessFrames - 1 || frame == headlessFrames / 2))
        {
            wchar_t wpath[512];
            MultiByteToWideChar(CP_UTF8, 0, outBmp, -1, wpath, 512);

            if (frame != headlessFrames - 1)
            {
                std::wstring mid = wpath;
                if (mid.size() > 4) mid.insert(mid.size() - 4, L"_mid");
                canvas.SaveBMP(mid.c_str());
            }
            else
            {
                canvas.SaveBMP(wpath);
                printf("已保存: %s\n", outBmp);
            }
        }

        frame++;
        if (!windowed && frame >= headlessFrames) break;

        // 帧率控制：60fps
        if (windowed)
        {
            const double spentMs = std::chrono::duration<double, std::milli>(
                Clock::now() - frameStart).count();
            const double target = 16.6667;
            if (spentMs < target) Sleep((DWORD)(target - spentMs));
        }
    }

    timeEndPeriod(1);
    printf("启动动画结束（%d 帧）\n", frame);
    return 0;
}
