// ============================================================================
// mock/main_mock.cpp
//
// 桌面透明叠层版：合成 PhysX 场景 -> 原版加载器 -> 原版射线 -> 全息线框叠层。
//
//   MockScene               按 Physx.h 的真实布局摆出 NpScene / Pruner / Shape / Body
//        |
//        v
//   physx::LoadShapeByRange                （原版，未改一行）  -> 静态 triangle mesh
//   physx::RefreshDynamicLoadHeightField 的同款过滤谓词        -> heightfield
//   physx::LoadDynamicRigidShape           （原版，未改一行）  -> 动态刚体
//        |
//        v
//   Physics::VisibleScene   （原版，未改一行）
//        |  用 src/mock/EmbreeCompat.h 提供的同名 Embree4 API（自写 BVH）
//        v
//   LineTrace::getNextHint  （原版，未改一行）  -> 屏幕中心射线命中的网格
//        |
//        v
//   HoloOverlay：深度预通道 + 特征边 + 抗锯齿线段 + 预乘 alpha 叠层窗口
//
// 用法：
//   PhysxPUBGMock.exe                         透明叠层，实时（默认全息风格）
//   PhysxPUBGMock.exe --style wire            纯线框（不填充面）
//   PhysxPUBGMock.exe --preset 1              切视角（0 环绕 / 1 全景 / 2 俯视 / 3 平视 / 4 近景）
//   PhysxPUBGMock.exe --size 1600x900         指定画布尺寸
//   PhysxPUBGMock.exe --headless 90 out.bmp   无窗口渲染并落盘（取证）
//   PhysxPUBGMock.exe --obj                   把探到的网格导出成 OBJ
//
// 视角：右键拖拽旋转 · 右键+Q/E 拉远拉近 · 右键+R 复位 · F9 键盘常驻接管
//       --look yaw,pitch[,dist] 可以直接给一个初始轨道偏移（headless 取证也能用）
//
// 退出：F8
// ============================================================================

#include <windows.h>

#include "mock/DMALibrary/Memory/Memory.h"
#include "mock/MockScene.h"
#include "mock/MockRender.h"
#include "mock/HoloOverlay.h"
#include "mock/ViewOrbit.h"

#include "util/Data.h"
#include "util/VectorHelper.h"     // FMatrix / FRotator / FTransform
#include "util/VisibleScene.h"
#include "util/LineTrace.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Data.h 里是 extern，这里给定义
FGameData GameData{};

using namespace physx;

namespace
{
    constexpr float kFov = 90.0f;             // 与 LineTrace 的判定保持一致（水平角）
    constexpr float kRoomX = 2000.0f;         // 合成场景房间中心
    constexpr float kRoomY = -2000.0f;

    const uint32_t kInk    = 0xE9F2FF;        // 主文字
    const uint32_t kDim    = 0x93A9C4;        // 次要文字
    const uint32_t kAccent = 0x5FB4FF;        // 强调
    const uint32_t kWarn   = 0xFF6A50;        // 命中
    const uint32_t kOk     = 0x5FE0A0;

    // ------------------------------------------------------------------
    // 相机
    // ------------------------------------------------------------------
    struct CamRig
    {
        Vector3 eye;
        Vector3 target;
        float   fovX = kFov;
    };

    CamRig MakeCamera(int preset, float t)
    {
        CamRig r;
        switch (preset)
        {
        case 1:   // 全景：东南方向、略高，房间占画面主体
            r.eye = Vector3(kRoomX + 1150.0f, kRoomY - 1150.0f, 880.0f);
            r.target = Vector3(kRoomX, kRoomY, 150.0f);
            break;
        case 2:   // 俯视
            r.eye = Vector3(kRoomX + 140.0f, kRoomY - 140.0f, 2650.0f);
            r.target = Vector3(kRoomX, kRoomY, 40.0f);
            break;
        case 3:   // 平视：从东侧开口往里看
            r.eye = Vector3(kRoomX + 760.0f, kRoomY - 90.0f, 205.0f);
            r.target = Vector3(kRoomX - 200.0f, kRoomY + 60.0f, 150.0f);
            break;
        case 4:   // 近景：从敞开的东侧看箱子（原来的对角线机位正对墙角，一条边劈在屏幕中间）
            r.eye = Vector3(kRoomX + 760.0f, kRoomY - 150.0f, 330.0f);
            r.target = Vector3(kRoomX + 140.0f, kRoomY - 150.0f, 185.0f);
            break;
        default:  // 环绕（默认）：从东侧缓慢扫过，房间东面是敞开的
            {
                const float ang = t * 0.30f;
                const float R = 1420.0f;
                r.eye = Vector3(kRoomX + std::cos(ang) * R,
                                kRoomY + std::sin(ang) * R,
                                620.0f);
                r.target = Vector3(kRoomX, kRoomY, 145.0f);
            }
            break;
        }
        return r;
    }

    void CamToUE(const CamRig& r, FVector& loc, FRotator& rot)
    {
        loc = FVector(r.eye.x, r.eye.y, r.eye.z);

        const Vector3 d = r.target - r.eye;
        const float yaw = std::atan2(d.y, d.x) * 57.2957795f;
        const float horiz = std::sqrt(d.x * d.x + d.y * d.y);
        const float pitch = std::atan2(d.z, horiz) * 57.2957795f;

        rot = FRotator(pitch, yaw, 0.0f);
    }

    // 原版 RefreshDynamicLoadHeightField 里的过滤谓词，逐字照抄。
    // 不能直接调那个函数：它有 `if (newObjects.size() < 1500) return {}`
    // 的硬闸门（Physx.h:1184），小场景永远进不去。
    auto HeightFieldFilter = [](const ShapeDataT& shape_data)
    {
        PxGeometryType type = shape_data.mType;
        if (type == PxGeometryType::eHEIGHTFIELD)
        {
            PxHeightFieldGeometryT field_geometry = (PxHeightFieldGeometryT&)shape_data.Shape.GetGeometry();
            if (field_geometry.RowScale != 100.f) return true;   // 模糊 heightfield 丢掉
            return false;                                        // RowScale==100 才是真地形
        }
        return true;
    };

    struct Stats
    {
        int heightFieldMeshes = 0;
        int staticMeshes = 0;
        int dynamicMeshes = 0;
        size_t totalTriangles = 0;
        size_t totalVertices = 0;
        size_t edgeCount = 0;
        double buildMs = 0.0;
        double frameMs = 0.0;
        double loadMs = 0.0;
        int frames = 0;
        TriangleMeshData* hit = nullptr;
        float hitDistance = 0.0f;
        std::vector<float> history;      // 帧耗时曲线
    };

    const char* TypeName(PxGeometryType t)
    {
        switch (t)
        {
        case PxGeometryType::eBOX:          return "box";
        case PxGeometryType::eCONVEXMESH:   return "convex";
        case PxGeometryType::eTRIANGLEMESH: return "triangle mesh";
        case PxGeometryType::eHEIGHTFIELD:  return "heightfield";
        case PxGeometryType::eSPHERE:       return "sphere";
        case PxGeometryType::eCAPSULE:      return "capsule";
        default:                            return "other";
        }
    }

    // getNextHint 只返回网格指针，不返回距离。用同一套参数再打三条射线，
    // 取最小 tfar 就得到真正的命中距离（顺带再验一次 VisibleScene::Raycast）。
    float RayHitDistance(const FVector& loc, const FRotator& rot)
    {
        FVector forward;
        forward.X = std::cos(rot.Yaw * 3.14159265f / 180.0f) * std::cos(rot.Pitch * 3.14159265f / 180.0f);
        forward.Y = std::sin(rot.Yaw * 3.14159265f / 180.0f) * std::cos(rot.Pitch * 3.14159265f / 180.0f);
        forward.Z = std::sin(rot.Pitch * 3.14159265f / 180.0f);

        const FVector target = loc + forward * 100000.0f;

        float best = -1.0f;
        Physics::VisibleScene<PrunerPayload, PrunerPayloadHash>* scenes[] = {
            GameData.DynamicLoadScene, GameData.DynamicRigidScene
        };
        for (auto* scene : scenes)
        {
            if (!scene) continue;
            auto rh = scene->Raycast(const_cast<FVector&>(loc), const_cast<FVector&>(target));
            if (rh.hit.geomID != RTC_INVALID_GEOMETRY_ID && (best < 0.0f || rh.ray.tfar < best))
                best = rh.ray.tfar;
        }
        if (GameData.HeightFieldScene)
        {
            auto rh = GameData.HeightFieldScene->Raycast(const_cast<FVector&>(loc), const_cast<FVector&>(target));
            if (rh.hit.geomID != RTC_INVALID_GEOMETRY_ID && (best < 0.0f || rh.ray.tfar < best))
                best = rh.ray.tfar;
        }
        return best < 0.0f ? 0.0f : best;
    }

    // ------------------------------------------------------------------
    // 射线可视化：虚线 + 命中点标记
    // ------------------------------------------------------------------
    void DrawRay(holo::Target& t, const soft3d::Camera& cam,
                 const Vector3& from, const Vector3& to,
                 uint32_t color, float alpha, const holo::DrawOptions& opt)
    {
        const Vector3 d = to - from;
        const int N = 40;

        for (int i = 0; i < N; i++)
        {
            // 只画偶数段 -> 虚线；越靠近终点越密，暗示射线的方向
            if (i % 2) continue;

            const float t0 = (float)i / N;
            const float t1 = (float)(i + 1) / N;

            const Vector3 p0 = from + d * t0;
            const Vector3 p1 = from + d * t1;

            float x0, y0, iz0, x1, y1, iz1;
            if (!holo::ProjectSegment(cam, p0, p1, x0, y0, iz0, x1, y1, iz1)) continue;

            const float fade = 1.0f - 0.55f * t0;
            holo::Segment(t, x0, y0, iz0, x1, y1, iz1, color, alpha * fade, 1.25f, opt);
        }
    }

    // ------------------------------------------------------------------
    // HUD
    // ------------------------------------------------------------------
    void DrawHud(holo::Target& t, holo::TextAA& tx, const Stats& st,
                 const CamRig& rig, const vieworbit::State& orb, float elevDeg,
                 int W, int H, const holo::DrawOptions& opt)
    {
        (void)rig;

        // HUD 以 900p 为设计基准，整体随分辨率缩放：
        // 直接把像素写死，在 1440p / 4K 上字会小得像灰尘。
        const float s = std::min(1.7f, std::max(0.8f, (float)H / 900.0f));
        auto P = [&](float v) { return (int)(v * s + 0.5f); };
        auto F = [&](int px) { return std::max(9, (int)(px * s + 0.5f)); };
        auto T = [&](int tr) { return std::max(0, (int)(tr * s + 0.5f)); };

        const int M = P(44);
        wchar_t buf[256];
        wchar_t val[128];

        // ---- 标题 ----
        // 左侧一条竖向强调线，给标题一个左边界；越往下越淡，像全息投影的底光
        for (int i = 0; i < P(52); i++)
        {
            const float k = 1.0f - (float)i / (float)P(52);
            t.Blend(M, P(36) + i, 0x5F, 0xB4, 0xFF, 0.85f * k);
            t.Blend(M + 1, P(36) + i, 0x5F, 0xB4, 0xFF, 0.45f * k);
            if (s > 1.0f) t.Blend(M + 2, P(36) + i, 0x5F, 0xB4, 0xFF, 0.18f * k);
        }

        tx.Draw(t, M + P(18), P(34), L"PHYSX  RAYCAST  TRACE", F(12), 0x86B6E8, 0.92f, true, T(3), true);
        tx.Draw(t, M + P(18), P(58), L"合成场景 · 桌面叠层", F(30), kInk, 1.0f, true, 0, true);
        tx.Draw(t, M + P(20), P(104), L"无 DMA · 无 embree4.dll · 无 vmm.dll · 数据由 MockScene 合成",
                F(13), kDim, 0.95f, false, 0, true);

        // ---- 左：场景 ----
        {
            const int px = M, py = P(148), pw = P(400), ph = P(206);
            holo::ui::Panel(t, px, py, pw, ph, 0.62f, P(12));

            int y = py + P(16);
            tx.Draw(t, px + P(18), y, L"场景", F(13), 0x7E9BBE, 0.95f, true, T(1));
            y += P(26);

            auto Row = [&](const wchar_t* k, const wchar_t* v, uint32_t vc)
            {
                tx.Draw(t, px + P(18), y, k, F(14), 0x9FB4CC, 0.92f);
                const int vw = tx.Measure(v, F(14), true);
                tx.Draw(t, px + pw - P(18) - vw, y, v, F(14), vc, 1.0f, true);
                y += P(25);
            };

            swprintf_s(val, L"%d / %d / %d", st.staticMeshes, st.dynamicMeshes, st.heightFieldMeshes);
            Row(L"静态 / 动态 / 地形", val, kInk);

            swprintf_s(val, L"%zu / %zu", st.totalTriangles, st.totalVertices);
            Row(L"三角形 / 顶点", val, kInk);

            swprintf_s(val, L"%zu 条", st.edgeCount);
            Row(L"特征边", val, kInk);

            swprintf_s(val, L"%.1f / %.1f ms", st.buildMs, st.frameMs);
            Row(L"加载 / 渲染", val, st.frameMs > 33.0 ? kWarn : kOk);

            y += P(4);
            const float fill = (float)std::min(1.0, st.frameMs / 33.0);
            holo::ui::HBar(t, px + P(18), y, pw - P(36), std::max(3, P(4)), fill,
                           st.frameMs > 33.0 ? kWarn : kAccent, 0.20f);
            y += P(12);

            tx.Draw(t, px + P(18), y, L"16.7 ms 预算", F(11), 0x6E86A4, 0.9f);
            swprintf_s(buf, L"%.0f fps", st.frameMs > 0.0 ? 1000.0 / st.frameMs : 0.0);
            const int fw = tx.Measure(buf, F(11), true);
            tx.Draw(t, px + pw - P(18) - fw, y, buf, F(11), kDim, 0.95f, true);
        }

        // ---- 右：射线 ----
        {
            const int pw = P(372), ph = P(168);
            const int px = W - M - pw, py = M;
            holo::ui::Panel(t, px, py, pw, ph, 0.62f, P(12));

            tx.Draw(t, px + P(18), py + P(14), L"RAYCAST", F(11), 0x86B6E8, 0.92f, true, T(3));

            if (st.hit)
            {
                swprintf_s(buf, L"%.2f m", st.hitDistance / 100.0f);
                tx.Draw(t, px + P(18), py + P(32), buf, F(34), kWarn, 1.0f, true, 0, true);

                swprintf_s(buf, L"%hs · %zu 顶点 / %zu 面",
                           TypeName(st.hit->Type),
                           st.hit->Vertices.size(),
                           st.hit->Indices.size() / 3);
                tx.Draw(t, px + P(18), py + P(86), buf, F(13), 0xE0C0B8, 0.95f);

                swprintf_s(buf, L"query (%u,%u,%u,%u)",
                           st.hit->QueryFilterData.word0, st.hit->QueryFilterData.word1,
                           st.hit->QueryFilterData.word2, st.hit->QueryFilterData.word3);
                tx.Draw(t, px + P(18), py + P(108), buf, F(12), 0x8E7E86, 0.95f);

                swprintf_s(buf, L"flags 0x%02X", st.hit->Flags);
                tx.Draw(t, px + P(18), py + P(128), buf, F(12), 0x8E7E86, 0.95f);
            }
            else
            {
                tx.Draw(t, px + P(18), py + P(40), L"未命中", F(30), kDim, 1.0f, true, 0, true);
                tx.Draw(t, px + P(18), py + P(86), L"视线指向空处", F(13), 0x7E93AC, 0.95f);
            }
        }

        // ---- 左下：图例 ----
        {
            const int pw = P(268), ph = P(128);
            const int px = M, py = H - M - ph;
            holo::ui::Panel(t, px, py, pw, ph, 0.55f, P(12));

            tx.Draw(t, px + P(18), py + P(14), L"图层", F(11), 0x7E9BBE, 0.95f, true, T(2));

            int y = py + P(40);
            struct Item { holo::Layer layer; int count; };
            const Item items[3] = {
                { holo::Layer::Static,  st.staticMeshes },
                { holo::Layer::Dynamic, st.dynamicMeshes },
                { holo::Layer::Terrain, st.heightFieldMeshes },
            };

            for (const auto& it : items)
            {
                const auto ls = holo::StyleOf(it.layer);

                // 色块：一小段带发光的线，和画面里的边同色
                const int r = (ls.edge >> 16) & 0xFF;
                const int g = (ls.edge >> 8) & 0xFF;
                const int b = ls.edge & 0xFF;
                const int sw = P(22);
                for (int i = 0; i < sw; i++)
                {
                    t.Blend(px + P(18) + i, y + P(7), r, g, b, 0.95f);
                    t.Blend(px + P(18) + i, y + P(6), r, g, b, 0.28f);
                    t.Blend(px + P(18) + i, y + P(8), r, g, b, 0.28f);
                }

                tx.Draw(t, px + P(50), y - P(3), ls.label, F(13), ls.edge, 0.95f);

                swprintf_s(buf, L"%d", it.count);
                const int cw = tx.Measure(buf, F(13), true);
                tx.Draw(t, px + pw - P(18) - cw, y - P(3), buf, F(13), 0x8FA6C2, 0.9f, true);

                y += P(26);
            }
        }

        // ---- 右下：视角读数 + 操作提示 ----
        {
            // 读数：没介入时告诉你怎么介入，介入后显示实时角度
            if (orb.engaged || orb.keyMode)
            {
                wchar_t look[192];
                swprintf_s(look, L"方位 %+.0f\u00B0   仰角 %+.0f\u00B0   距离 %.2fx%s",
                           orb.yaw, elevDeg, orb.distMul,
                           orb.keyMode ? L"   [KEY]" : L"");
                const int lw = tx.Measure(look, F(12), true);
                tx.Draw(t, W - M - lw - P(8), H - M - P(46), look, F(12), kAccent, 0.92f, true, 0, true);
            }
            else
            {
                const wchar_t* look = L"按住中键拖拽 \u2192 转视角";
                const int lw = tx.Measure(look, F(12), true);
                tx.Draw(t, W - M - lw - P(8), H - M - P(46), look, F(12), 0x9FB8D6, 0.85f, false, 0, true);
            }

            const wchar_t* hint = L"中键/右键 + Q/E 远近  \u00B7  + R 复位  \u00B7  F9 键盘接管  \u00B7  F8 退出";
            const int hw = tx.Measure(hint, F(12));
            tx.Draw(t, W - M - hw - P(8), H - M - P(22), hint, F(12), 0x7E93AC, 0.9f, false, 0, true);
        }

        // ---- 准星 ----
        {
            const float cx = W * 0.5f;
            const float cy = H * 0.5f;
            const uint32_t cc = st.hit ? kWarn : 0xCFE0F2;
            const float ca = 0.85f;

            holo::ui::Ring(t, cx, cy, (float)P(13), cc, ca * 0.55f, std::max(1.0f, s));
            holo::ui::Ring(t, cx, cy, (float)P(4), cc, ca * 0.35f, std::max(1.0f, s * 0.8f));

            holo::ui::Line2D(t, cx - P(22), cy, cx - P(8), cy, cc, ca);
            holo::ui::Line2D(t, cx + P(8), cy, cx + P(22), cy, cc, ca);
            holo::ui::Line2D(t, cx, cy - P(22), cx, cy - P(8), cc, ca);
            holo::ui::Line2D(t, cx, cy + P(8), cx, cy + P(22), cc, ca);

            // 四个方向各一个刻度点，读起来像取景器
            for (int i = 0; i < 4; i++)
            {
                const float ang = i * 3.14159265f * 0.5f;
                const float dx = std::cos(ang) * (float)P(34);
                const float dy = std::sin(ang) * (float)P(34);
                t.Blend((int)(cx + dx), (int)(cy + dy),
                        (cc >> 16) & 0xFF, (cc >> 8) & 0xFF, cc & 0xFF, ca * 0.5f);
            }

            if (st.hit)
            {
                swprintf_s(buf, L"%.2f m", st.hitDistance / 100.0f);
                const int bw = tx.Measure(buf, F(12), true);
                tx.Draw(t, (int)(cx - bw * 0.5f), (int)(cy + P(26)), buf, F(12), kWarn, 0.95f, true, 0, true);
            }
        }
    }

    // 诊断：box / convex 这两个分支在现在的加载链路里到不了，
    // 因为两个过滤谓词把非 triangle mesh 全部删掉了（Physx.h:1397 / 1140）。
    // 这里绕过 pruner 直接把形状喂给 GetMeshData，看分支代码本身对不对。
    void DiagnoseMeshes(const std::vector<PrunerPayload>& objects)
    {
        printf("\n=== 分支诊断（绕过 pruner，直接喂 GetMeshData）===\n");
        if (objects.empty())
        {
            printf("  没有诊断对象\n");
            return;
        }

        auto meshes = physx::GetMeshData(
            const_cast<std::vector<PrunerPayload>&>(objects),
            [](const ShapeDataT&) { return false; },
            true);

        if (meshes.empty())
        {
            printf("  GetMeshData 没有产出任何网格\n");
            return;
        }

        for (const auto& m : meshes)
        {
            printf("\n  [%s] 顶点 %zu, 索引 %zu (%zu 三角形)\n",
                TypeName(m.Type), m.Vertices.size(), m.Indices.size(), m.Indices.size() / 3);

            if (m.Vertices.empty())
            {
                printf("    [!] 顶点为空\n");
                continue;
            }

            size_t uniquePos = 0;
            for (size_t i = 0; i < m.Vertices.size(); i++)
            {
                bool dup = false;
                for (size_t j = 0; j < i; j++)
                    if (m.Vertices[i].IsNearlyEqual(m.Vertices[j], 1e-4f)) { dup = true; break; }
                if (!dup) uniquePos++;
            }

            Vector3 mn(FLT_MAX, FLT_MAX, FLT_MAX), mx(-FLT_MAX, -FLT_MAX, -FLT_MAX);
            for (const auto& v : m.Vertices)
            {
                mn.x = std::min(mn.x, v.x); mn.y = std::min(mn.y, v.y); mn.z = std::min(mn.z, v.z);
                mx.x = std::max(mx.x, v.x); mx.y = std::max(mx.y, v.y); mx.z = std::max(mx.z, v.z);
            }
            printf("    去重位置数 %zu, AABB 尺寸 (%.1f, %.1f, %.1f)\n",
                uniquePos, mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
            printf("    QueryFilter.word3 = %u（> 100 才入列，闸门在 Physx.h:1001）\n",
                m.QueryFilterData.word3);

            if (uniquePos <= 1)
            {
                printf("    [!] 全部顶点被压到同一点 —— convex 分支用了从未赋值的 ShapeData.Scale。\n");
                printf("        Physx.h:959 读 ShapeDataItem.Scale，而 Scale 只在 triangle mesh 分支\n");
                printf("        （Physx.h:713）被写。PxMeshScale 默认 Vector3 是 (0,0,0)，\n");
                printf("        ScaleVal.transform() 于是把每个顶点乘成零向量。\n");
                printf("        修法见 patch\\Physx_convex_scale.patch（3 行）。\n");
            }
            else
            {
                printf("    [OK] 顶点正常，box 分支可用\n");
            }
        }

        auto boxOnly = physx::GetMeshData(
            const_cast<std::vector<PrunerPayload>&>(objects),
            [](const ShapeDataT& sd) { return sd.mType != PxGeometryType::eTRIANGLEMESH; },
            true);
        printf("\n  用 LoadDynamicRigidShape 同款谓词再跑一次: 产出 %zu 个网格（预期 0）\n",
            boxOnly.size());
        printf("  → box / convex 在现有加载器里是死分支，只有直接调 GetMeshData 才会走到。\n");
    }

    void ExportObj(const std::vector<TriangleMeshData*>& meshes, const char* dir)
    {
        int n = 0;
        for (const auto* m : meshes)
        {
            if (!m || m->Vertices.empty()) continue;

            char path[512];
            char typeName[32];
            snprintf(typeName, sizeof(typeName), "%s", TypeName(m->Type));
            for (char* c = typeName; *c; ++c) if (*c == ' ') *c = '_';

            snprintf(path, sizeof(path), "%s/mesh_%03d_%s_%zu_%zu.obj",
                dir, n++, typeName, m->Vertices.size(), m->Indices.size());

            FILE* f = nullptr;
            if (fopen_s(&f, path, "wb") != 0 || !f) continue;
            for (const auto& v : m->Vertices)
                fprintf(f, "v %.4f %.4f %.4f\n", v.x, v.y, v.z);
            for (size_t i = 0; i + 2 < m->Indices.size(); i += 3)
                fprintf(f, "f %u %u %u\n", m->Indices[i] + 1, m->Indices[i + 1] + 1, m->Indices[i + 2] + 1);
            fclose(f);
            printf("  导出 %s\n", path);
        }
    }

    // 画布尺寸：默认跟主显示器一致（DPI 感知后拿到的是物理像素）
    bool ParseSize(const char* s, int& w, int& h)
    {
        int a = 0, b = 0;
        if (sscanf_s(s, "%dx%d", &a, &b) != 2) return false;
        if (a < 320 || b < 240 || a > 7680 || b > 4320) return false;
        w = a;
        h = b;
        return true;
    }
}

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    // 先声明 DPI 感知，否则 GetSystemMetrics 返回的是缩放前的逻辑像素，
    // 叠层窗口会盖不满屏幕（125% 缩放下只盖左上 1.6 屏）
    SetProcessDPIAware();

    int  headlessFrames = -1;
    const char* headlessOut = nullptr;
    bool exportObj = false;
    int  preset = 0;
    int  forcedW = 0, forcedH = 0;
    float lookYaw = 0.0f, lookPitch = 0.0f, lookDist = 1.0f;   // --look 初始轨道偏移

    holo::DrawOptions opt = holo::DefaultOptions();

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--headless") == 0 && i + 2 < argc)
        {
            headlessFrames = atoi(argv[++i]);
            headlessOut = argv[++i];
        }
        else if (strcmp(argv[i], "--obj") == 0)
        {
            exportObj = true;
        }
        else if (strcmp(argv[i], "--preset") == 0 && i + 1 < argc)
        {
            preset = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc)
        {
            if (!ParseSize(argv[++i], forcedW, forcedH))
                printf("[!] --size 格式应为 1600x900，忽略\n");
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
            else if (strcmp(s, "holo") == 0)
            {
                opt.fill = true;
            }
            else
            {
                printf("[!] --style 只支持 holo / wire\n");
            }
        }
        else if (strcmp(argv[i], "--no-glow") == 0)
        {
            opt.glowAlpha = 0.0f;
        }
        else if (strcmp(argv[i], "--look") == 0 && i + 1 < argc)
        {
            const char* s = argv[++i];
            float y = 0.0f, p = 0.0f, d = 1.0f;
            const int n = sscanf_s(s, "%f,%f,%f", &y, &p, &d);
            if (n >= 2)
            {
                lookYaw   = y;
                lookPitch = p;
                lookDist  = (n >= 3) ? d : 1.0f;
            }
            else
            {
                printf("[!] --look 格式应为 yaw,pitch[,dist]，例如 --look 35,10\n");
            }
        }
        else if (strcmp(argv[i], "--help") == 0)
        {
            printf("用法:\n");
            printf("  PhysxPUBGMock.exe                          透明叠层实时渲染\n");
            printf("  PhysxPUBGMock.exe --style wire             纯线框（不填充面）\n");
            printf("  PhysxPUBGMock.exe --preset 1               切视角 0..4\n");
            printf("  PhysxPUBGMock.exe --size 1600x900          指定画布尺寸\n");
            printf("  PhysxPUBGMock.exe --look 35,10             初始视角偏移 yaw,pitch[,dist]\n");
            printf("  PhysxPUBGMock.exe --headless 90 out.bmp    无窗口渲染并落盘\n");
            printf("  PhysxPUBGMock.exe --obj                    导出 OBJ\n");
            return 0;
        }
    }

    printf("=== PhysX Raycast Trace · HOLO OVERLAY ===\n");
    printf("构建合成 PhysX 场景...\n");

    auto demo = mockscene::BuildDemo();
    auto& space = mockmem::Space();
    printf("  模拟地址空间: %zu 块 / %zu 字节 / %zu 页\n",
        space.BlockCount(), space.AllocatedBytes(), space.PagesUsed());
    printf("  静态对象 %zu 个，动态对象 %zu 个\n",
        demo.staticObjects.size(), demo.dynamicObjects.size());

    GameData.PhysxInstancePtr = demo.physxInstance;
    GameData.WorldOriginLocation = FVector(0, 0, 0);
    GameData.LoadRadius = 5000.0f;
    GameData.PhysxRefreshLimit = 0;

    GameData.DynamicLoadScene = new Physics::VisibleScene<PrunerPayload, PrunerPayloadHash>(Physics::prunerPayloadExtractor);
    GameData.DynamicRigidScene = new Physics::VisibleScene<PrunerPayload, PrunerPayloadHash>(Physics::prunerPayloadExtractor);
    GameData.HeightFieldScene = new Physics::VisibleScene<uint64_t, Int64Hash>(Physics::int64Extractor);

    Stats stats;

    // ---- 地形：一次性加载 ----
    {
        auto start = std::chrono::high_resolution_clock::now();
        auto hfMeshes = physx::GetMeshData(demo.staticObjects, HeightFieldFilter, false);
        GameData.HeightFieldScene->UpdateMesh(hfMeshes, {});
        auto end = std::chrono::high_resolution_clock::now();
        stats.buildMs = std::chrono::duration<double, std::milli>(end - start).count();

        size_t tris = 0, verts = 0;
        for (auto& m : hfMeshes) { tris += m.Indices.size() / 3; verts += m.Vertices.size(); }
        stats.heightFieldMeshes = (int)hfMeshes.size();
        printf("  地形: %zu 个网格, %zu 三角形, %zu 顶点 (%.2f ms)\n",
            hfMeshes.size(), tris, verts, stats.buildMs);
        if (hfMeshes.empty())
            printf("  [!] 地形为空 —— heightfield 过滤或布局有问题\n");
    }

    DiagnoseMeshes(demo.diagnosticObjects);

    // ---- 画布尺寸 ----
    int W = forcedW, H = forcedH;
    if (W <= 0 || H <= 0)
    {
        W = GetSystemMetrics(SM_CXSCREEN);
        H = GetSystemMetrics(SM_CYSCREEN);
        if (W <= 0 || H <= 0) { W = 1920; H = 1080; }
    }

    mockrender::Canvas canvas;
    if (!canvas.Create(W, H))
    {
        printf("创建 %dx%d 画布失败\n", W, H);
        return 1;
    }

    holo::Target target;
    target.Attach(canvas.Pixels(), W, H);

    holo::TextAA text;
    soft3d::Camera cam;
    soft3d::Shading shading;

    mockrender::OverlayWindow overlay;
    bool windowed = (headlessFrames < 0);
    if (windowed)
    {
        if (!overlay.Create(L"PhysX Raycast Trace - HOLO", W, H))
        {
            printf("创建透明叠层窗口失败（转入无窗口模式）\n");
            windowed = false;
            headlessFrames = 0;
        }
    }

    printf("  画布 %dx%d，模式: %s，风格: %s\n", W, H,
        windowed ? "透明叠层" : "无窗口",
        opt.fill ? "holo（面 + 线）" : "wire（纯线框）");

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
    };
    SceneState state;

    const int totalFrames = windowed ? INT32_MAX : headlessFrames;

    // 分段计时：光栅化里凭直觉猜瓶颈十次有九次是错的
    struct Prof
    {
        double depth = 0, fill = 0, edge = 0, ray = 0, hud = 0;
        int samples = 0;
    } prof;

    std::vector<TriangleMeshData*> terrainMeshes;
    std::vector<TriangleMeshData*> staticMeshes;
    std::vector<TriangleMeshData*> dynamicMeshes;

    auto runStart = std::chrono::high_resolution_clock::now();

    // ---- 视角交互 ----
    vieworbit::State  orbit;
    vieworbit::Config orbitCfg;
    orbit.yaw     = lookYaw;
    orbit.pitch   = lookPitch;
    orbit.distMul = lookDist;
    if (lookYaw != 0.0f || lookPitch != 0.0f || lookDist != 1.0f)
        orbit.engaged = true;

    float  orbitFreezeT = lookYaw != 0.0f ? 0.0f : -1.0f;   // 0 号机位自动环绕的冻结时刻
    float  orbitElevDeg = 0.0f;                              // 实际仰角，HUD 用
    double prevFrameSec = 0.0;

    for (int frame = 0; frame < totalFrames; frame++)
    {
        auto frameStart = std::chrono::high_resolution_clock::now();
        const float t = frame * (1.0f / 60.0f);

        // 帧间隔：键盘转速按秒算，掉帧时不会一下甩半圈
        const double nowSec = std::chrono::duration<double>(frameStart - runStart).count();
        const float  dt = (float)std::min(0.100, std::max(0.0, nowSec - prevFrameSec));
        prevFrameSec = nowSec;

        demo.Tick(t);

        // 只在窗口模式接管输入。headless 去读真实鼠标会让截图不可复现。
        if (windowed) vieworbit::Update(orbit, orbitCfg, dt);

        // 用户一旦介入，0 号机位的自动环绕就冻结在介入那一刻，否则自动转会跟手转打架
        if (orbit.engaged && orbitFreezeT < 0.0f) orbitFreezeT = t;
        const float camT = (orbitFreezeT >= 0.0f) ? orbitFreezeT : t;

        CamRig rig = MakeCamera(preset, camT);
        {
            float e3[3] = { rig.eye.x, rig.eye.y, rig.eye.z };
            const float p3[3] = { rig.target.x, rig.target.y, rig.target.z };
            orbitElevDeg = vieworbit::Apply(orbit, orbitCfg, e3, p3);
            rig.eye = Vector3(e3[0], e3[1], e3[2]);
        }

        // ---- 加载：原版 LoadShapeByRange + LoadDynamicRigidShape ----
        auto loadStart = std::chrono::high_resolution_clock::now();
        {
            auto meshes = physx::LoadShapeByRange(
                state.lastTimestamp,
                state.staticCache,
                state.staticInScene,
                state.staticRemove,
                state.alwaysCheckShape,
                rig.eye,
                GameData.LoadRadius,
                0);

            if (!meshes.empty() || !state.staticRemove.empty())
                GameData.DynamicLoadScene->UpdateMesh(meshes, state.staticRemove);
            state.staticRemove.clear();
        }
        {
            auto meshes = physx::LoadDynamicRigidShape(
                state.dynamicInScene,
                state.dynamicCache,
                state.ptrCache,
                state.dynamicRemove,
                rig.eye,
                GameData.LoadRadius);

            if (!meshes.empty() || !state.dynamicRemove.empty())
                GameData.DynamicRigidScene->UpdateMesh(meshes, state.dynamicRemove);
            state.dynamicRemove.clear();
        }
        stats.loadMs = std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - loadStart).count();

        // ---- 射线：原版 LineTrace::getNextHint ----
        FVector loc;
        FRotator rot;
        CamToUE(rig, loc, rot);

        TriangleMeshData* hit = LineTrace::getNextHint(rot, loc, kFov);
        const float hitDist = hit ? RayHitDistance(loc, rot) : 0.0f;

        // ---- 取网格 ----
        {
            // 注意：变量名不能叫 near —— Windows.h 里有 #define near / far（16 位遗留）
            const FVector eyePos(rig.eye.x, rig.eye.y, rig.eye.z);
            terrainMeshes = GameData.HeightFieldScene->GetNearMesh(eyePos, 1e12);
            staticMeshes = GameData.DynamicLoadScene->GetNearMesh(eyePos, 1e12);
            dynamicMeshes = GameData.DynamicRigidScene->GetNearMesh(eyePos, 1e12);
        }

        // ---- 相机 ----
        // 地形网格的作用范围跟相机高度挂钩：站得低，网格只铺到附近；
        // 俯视时相机本身就离地 2000+，网格自动退出，画面反而干净
        opt.terrainEdgeRange = std::min(2400.0f, std::max(760.0f, rig.eye.z * 1.55f));

        float aspect = (float)W / (float)H;
        const float fovY = 2.0f * std::atan(std::tan(rig.fovX * 3.14159265f / 360.0f) / aspect) * 57.2957795f;
        cam.eye = rig.eye;
        cam.target = rig.target;
        cam.fovY = std::min(120.0f, std::max(20.0f, fovY));
        cam.zNear = 10.0f;
        cam.LookAt(W, H);

        // ---- 渲染 ----
        canvas.Clear();
        target.Begin();

        // 1) 全场景深度预通道（隐藏线消除的前提）
        auto tDepth = std::chrono::high_resolution_clock::now();
        for (auto* m : terrainMeshes) if (m) holo::DrawMeshDepth(target, cam, *m);
        for (auto* m : staticMeshes)  if (m) holo::DrawMeshDepth(target, cam, *m);
        for (auto* m : dynamicMeshes) if (m) holo::DrawMeshDepth(target, cam, *m);
        prof.depth += std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - tDepth).count();
        auto tFill = std::chrono::high_resolution_clock::now();

        // 2) 面（半透明，只给体积感）
        if (opt.fill)
        {
            if (opt.fillTerrain)
                for (auto* m : terrainMeshes) if (m) holo::DrawMeshFill(target, cam, *m, holo::Layer::Terrain, opt, false, shading);
            for (auto* m : staticMeshes)  if (m) holo::DrawMeshFill(target, cam, *m, holo::Layer::Static,  opt, false, shading);
            for (auto* m : dynamicMeshes) if (m) holo::DrawMeshFill(target, cam, *m, holo::Layer::Dynamic, opt, false, shading);
        }

        prof.fill += std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - tFill).count();

        // 3) 线（先软后硬，硬边压在上面）
        auto tEdge = std::chrono::high_resolution_clock::now();
        for (auto* m : terrainMeshes) if (m) holo::DrawMeshEdges(target, cam, *m, holo::Layer::Terrain, opt, false);
        for (auto* m : staticMeshes)  if (m) holo::DrawMeshEdges(target, cam, *m, holo::Layer::Static,  opt, false);
        for (auto* m : dynamicMeshes)
        {
            if (!m) continue;
            holo::DrawMeshEdges(target, cam, *m, holo::Layer::Dynamic, opt, m == hit);
        }

        prof.edge += std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - tEdge).count();

        // 4) 射线
        auto tRay = std::chrono::high_resolution_clock::now();
        {
            FVector fwd;
            fwd.X = std::cos(rot.Yaw * 3.14159265f / 180.0f) * std::cos(rot.Pitch * 3.14159265f / 180.0f);
            fwd.Y = std::sin(rot.Yaw * 3.14159265f / 180.0f) * std::cos(rot.Pitch * 3.14159265f / 180.0f);
            fwd.Z = std::sin(rot.Pitch * 3.14159265f / 180.0f);

            const float reach = hit ? (hitDist > 0.0f ? hitDist : 3000.0f) : 3000.0f;
            const Vector3 rayEnd = rig.eye + Vector3(fwd.X, fwd.Y, fwd.Z) * reach;

            DrawRay(target, cam, rig.eye, rayEnd,
                    kWarn, 0.55f, opt);

            // 命中点：一个屏幕空间的小环 + 十字
            if (hit)
            {
                float hx, hy, hiz;
                if (cam.Project(rayEnd, hx, hy, hiz))
                {
                    const float z = 1.0f / hiz;
                    const float a = 1.0f - 0.7f * std::min(1.0f, std::max(0.0f,
                        (z - opt.fogStart) / (opt.fogEnd - opt.fogStart)));

                    holo::ui::Ring(target, hx, hy, 7.0f, kWarn, a, 1.4f);
                    holo::ui::Ring(target, hx, hy, 13.0f, kWarn, a * 0.35f, 1.0f);
                    holo::ui::Line2D(target, hx - 16.0f, hy, hx - 10.0f, hy, kWarn, a * 0.8f);
                    holo::ui::Line2D(target, hx + 10.0f, hy, hx + 16.0f, hy, kWarn, a * 0.8f);
                    holo::ui::Line2D(target, hx, hy - 16.0f, hx, hy - 10.0f, kWarn, a * 0.8f);
                    holo::ui::Line2D(target, hx, hy + 10.0f, hx, hy + 16.0f, kWarn, a * 0.8f);
                }

                // 命中网格的包围盒：只画 8 个角，比整圈线框干净
                // 地形不画包围盒：3200 个三角形的 AABB 就是整个世界，
                // 那两个巨大的红色括号会横扫整屏
                if (hit->Type != PxGeometryType::eHEIGHTFIELD &&
                    hit->Indices.size() <= 3 * 4000 && !hit->Vertices.empty())
                {
                    Vector3 mn, mx;
                    holo::ui::ProjectedAABB(*hit, mn, mx);
                    holo::ui::Brackets(target, cam, mn, mx, kWarn, 0.55f, opt);
                }
            }
        }

        // ---- 统计 ----
        stats.frames = frame + 1;
        stats.staticMeshes = (int)staticMeshes.size();
        stats.dynamicMeshes = (int)dynamicMeshes.size();
        stats.totalTriangles = 0;
        stats.totalVertices = 0;
        stats.edgeCount = 0;
        {
            auto acc = [&](const std::vector<TriangleMeshData*>& v)
            {
                for (auto* m : v)
                {
                    if (!m) continue;
                    stats.totalTriangles += m->Indices.size() / 3;
                    stats.totalVertices += m->Vertices.size();
                    const float crease = (m->Type == PxGeometryType::eHEIGHTFIELD) ? 0.80f : opt.creaseCos;
                    const auto& el = holo::EdgesOf(*m, crease);
                    stats.edgeCount += (el.hard.size() + el.soft.size()) / 2;
                }
            };
            acc(terrainMeshes); acc(staticMeshes); acc(dynamicMeshes);

            if (frame == 0)
            {
                for (auto* m : terrainMeshes)
                {
                    if (!m) continue;
                    const auto& el = holo::EdgesOf(*m, 0.80f);
                    printf("  [地形边] 硬边 %zu 条 / 软边 %zu 条\n",
                        el.hard.size() / 2, el.soft.size() / 2);
                }
            }
        }
        stats.hit = hit;
        stats.hitDistance = hitDist;

        auto frameEnd = std::chrono::high_resolution_clock::now();
        stats.frameMs = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();

        stats.history.push_back((float)stats.frameMs);
        if (stats.history.size() > 96) stats.history.erase(stats.history.begin());

        prof.ray += std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - tRay).count();

        // ---- HUD ----
        auto tHud = std::chrono::high_resolution_clock::now();
        DrawHud(target, text, stats, rig, orbit, orbitElevDeg, W, H, opt);
        prof.hud += std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - tHud).count();
        prof.samples++;

        // ---- 呈现 ----
        if (windowed)
        {
            overlay.Present(canvas);
            if (!overlay.Pump()) break;
        }
        else if (headlessOut && (frame == totalFrames - 1 || frame == totalFrames / 2))
        {
            wchar_t wpath[512];
            MultiByteToWideChar(CP_UTF8, 0, headlessOut, -1, wpath, 512);

            if (frame != totalFrames - 1)
            {
                std::wstring mid = wpath;
                if (mid.size() > 4) mid.insert(mid.size() - 4, L"_mid");
                canvas.SaveBMP(mid.c_str());
            }
            else
            {
                canvas.SaveBMP(wpath);
                printf("已保存: %s\n", headlessOut);
            }
        }
    }

    if (exportObj)
    {
        printf("\n导出 OBJ...\n");
        std::vector<TriangleMeshData*> all = terrainMeshes;
        all.insert(all.end(), staticMeshes.begin(), staticMeshes.end());
        all.insert(all.end(), dynamicMeshes.begin(), dynamicMeshes.end());
        ExportObj(all, ".");
    }

    auto runEnd = std::chrono::high_resolution_clock::now();
    const double totalMs = std::chrono::duration<double, std::milli>(runEnd - runStart).count();

    if (prof.samples > 0)
    {
        const double n = (double)prof.samples;
        printf("  分段耗时: 深度 %.2f / 填充 %.2f / 线 %.2f / 射线 %.2f / HUD %.2f ms\n",
            prof.depth / n, prof.fill / n, prof.edge / n, prof.ray / n, prof.hud / n);
    }

    printf("\n=== 运行统计 ===\n");
    printf("  帧数 %d  平均 %.2f ms/帧\n", stats.frames, totalMs / (stats.frames ? stats.frames : 1));
    printf("  静态网格 %d  动态网格 %d  地形网格 %d\n",
        stats.staticMeshes, stats.dynamicMeshes, stats.heightFieldMeshes);
    printf("  三角形 %zu  顶点 %zu  特征边 %zu\n",
        stats.totalTriangles, stats.totalVertices, stats.edgeCount);
    if (stats.hit)
        printf("  射线命中: %s  verts=%zu faces=%zu flags=0x%02X  距离 %.0f cm\n",
            TypeName(stats.hit->Type), stats.hit->Vertices.size(),
            stats.hit->Indices.size() / 3, stats.hit->Flags, stats.hitDistance);
    else
        printf("  射线命中: 无\n");

    return 0;
}
