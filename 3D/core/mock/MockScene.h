#pragma once

// ============================================================================
// mock/MockScene.h
//
// 在 mockmem::Memory 里，按 Physx.h 的结构体布局搭一个「看起来像真的」
// PhysX 场景，然后让原版 physx::LoadShapeByRange / LoadDynamicRigidShape /
// GetMeshData 去读它 —— 一行源码都不改。
//
// 为什么要这么做：
//   Physx.h 里那些偏移（ShapeT::ShapeCore 在 +0x20、PxShapeCoreT::geometry 在
//   +0x28、BodyT::mRigid 在 +0x20、TriangleMeshT::Vertices 在 +0x28 ...）在这份
//   数据里是被显式写出来的。布局一旦对不上，读出来立刻是垃圾值（0 或野指针），
//   而不是像真实环境那样只表现为「屏幕上什么都没有」。
//
// 场景内容（世界坐标 Z 向上，单位 cm，与 UE 一致）：
//   地面       heightfield  41x41 采样，RowScale=ColumnScale=100（必须 100，
//                            见 RefreshDynamicLoadHeightField 的过滤条件）
//   房间       4 面墙 + 地板 + 斜坡 + 立柱，全部 triangle mesh
//   可破坏墙   带 QueryFilterData.word0/word2 > 0，用来走 alwaysCheckShape 分支
//   动态箱子   3 个 box，bodyToActor 非单位阵，用来验证 getInverse 链
//   圆桶       convex mesh（会暴露 GetMeshData 的 Scale 未初始化问题）
// ============================================================================

#include "MockMemory.h"
#include "WinPre.h"
#include "../visible/Physx.h"
#include "../util/Data.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <vector>

namespace mockscene
{
    using namespace physx;

    // ---------------------------------------------------------------------
    // 布局核对：这些数字来自 probe_offsets.exe 的实测输出（MSVC 14.44 x64）。
    // 换编译器/换标准库、或者有人改了 Physx.h 的字段顺序，这里立刻炸，
    // 而不是等到运行时读出一屏垃圾。
    // ---------------------------------------------------------------------
    namespace layout
    {
        static_assert(offsetof(ShapeT, mControlState) == 0x08, "ShapeT::mControlState");
        static_assert(offsetof(ShapeT, mStreamPtr)    == 0x10, "ShapeT::mStreamPtr");
        static_assert(offsetof(ShapeT, ShapeCore)     == 0x20, "ShapeT::ShapeCore");

        static_assert(offsetof(ShapeCoreT, mCore)     == 0x20, "ShapeCoreT::mCore");
        static_assert(offsetof(PxShapeCoreT, mShapeFlags) == 0x20, "PxShapeCoreT::mShapeFlags");
        static_assert(offsetof(PxShapeCoreT, geometry)    == 0x28, "PxShapeCoreT::geometry");

        static_assert(offsetof(BodyT, mRigid)             == 0x20, "BodyT::mRigid");
        static_assert(offsetof(BodyCoreT, mCore)          == 0x10, "BodyCoreT::mCore");
        static_assert(offsetof(PxsRigidCoreT, mBodyToWorld) == 0x00, "PxsRigidCoreT::mBodyToWorld");
        static_assert(offsetof(BodyCoreT, mBodyToActor)   == 0x30, "BodyCoreT::mBodyToActor");

        static_assert(offsetof(TriangleMeshT, mNbVertices) == 0x20, "TriangleMeshT::mNbVertices");
        static_assert(offsetof(TriangleMeshT, Vertices)    == 0x28, "TriangleMeshT::Vertices");
        static_assert(offsetof(TriangleMeshT, mTriangles)  == 0x30, "TriangleMeshT::mTriangles");
        static_assert(offsetof(TriangleMeshT, Flags)       == 0x5C, "TriangleMeshT::Flags");

        static_assert(offsetof(HeightFieldT, mData)        == 0x20, "HeightFieldT::mData");
        static_assert(offsetof(ConvexMeshT, HullData)      == 0x20, "ConvexMeshT::HullData");
        static_assert(offsetof(ConvexHullDataT, mPolygons) == 0x28, "ConvexHullDataT::mPolygons");

        static_assert(offsetof(NpSceneT, exts)             == 0x2450, "NpSceneT::exts");
        static_assert(offsetof(PruningPoolT, mObjects)     == 0x10, "PruningPoolT::mObjects");
        static_assert(offsetof(PrunerExtT, mTimestamp)     == 0x2C, "PrunerExtT::mTimestamp");

        static_assert(sizeof(PxTransformT) == 28, "PxTransformT (Vector4 + Vector3，无 alignas)");
        static_assert(sizeof(HullPolygonDataT) == 20, "HullPolygonDataT");
        static_assert(sizeof(PxBoxGeometry) <= sizeof(GeometryUnionT::mGeometry), "box overflow");
        static_assert(sizeof(PxTriangleMeshGeometryT) <= sizeof(GeometryUnionT::mGeometry), "mesh overflow");
        static_assert(sizeof(PxHeightFieldGeometryT) <= sizeof(GeometryUnionT::mGeometry), "hf overflow");
        static_assert(sizeof(PxConvexMeshGeometryT) <= sizeof(GeometryUnionT::mGeometry), "convex overflow");
    }

    // ---------------------------------------------------------------------
    // 变换工具
    // ---------------------------------------------------------------------
    inline Vector4 QuatAxisAngle(const Vector3& axis, float radians)
    {
        float len = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
        if (len < 1e-8f) return Vector4(0, 0, 0, 1);
        float s = std::sin(radians * 0.5f) / len;
        return Vector4(axis.x * s, axis.y * s, axis.z * s, std::cos(radians * 0.5f));
    }

    inline Vector4 QuatIdentity() { return Vector4(0, 0, 0, 1); }

    inline PxTransformT MakeTransform(const Vector3& pos, const Vector4& rot)
    {
        PxTransformT t;
        t.mRotation = rot;
        t.mPosition = pos;
        return t;
    }

    // 立方体三角形网格（12 个三角形，8 个顶点）
    inline void AppendBox(
        std::vector<Vector3>& verts,
        std::vector<uint32_t>& indices,
        const Vector3& center,
        const Vector3& half)
    {
        uint32_t base = (uint32_t)verts.size();
        const float sx = half.x, sy = half.y, sz = half.z;
        verts.push_back(Vector3(center.x - sx, center.y - sy, center.z - sz)); // 0
        verts.push_back(Vector3(center.x + sx, center.y - sy, center.z - sz)); // 1
        verts.push_back(Vector3(center.x + sx, center.y + sy, center.z - sz)); // 2
        verts.push_back(Vector3(center.x - sx, center.y + sy, center.z - sz)); // 3
        verts.push_back(Vector3(center.x - sx, center.y - sy, center.z + sz)); // 4
        verts.push_back(Vector3(center.x + sx, center.y - sy, center.z + sz)); // 5
        verts.push_back(Vector3(center.x + sx, center.y + sy, center.z + sz)); // 6
        verts.push_back(Vector3(center.x - sx, center.y + sy, center.z + sz)); // 7

        static const uint32_t faces[36] = {
            0,1,2, 0,2,3,   // -Z
            4,6,5, 4,7,6,   // +Z
            0,4,5, 0,5,1,   // -Y
            3,2,6, 3,6,7,   // +Y
            0,3,7, 0,7,4,   // -X
            1,5,6, 1,6,2    // +X
        };
        for (int i = 0; i < 36; i++) indices.push_back(base + faces[i]);
    }

    // ---------------------------------------------------------------------
    // 任意两点之间的方梁（截面 2*halfW），用来做横杆和斜撑。
    //
    // 拓扑和 AppendBox 完全一致，只是把局部 Z 轴换成 a->b 这条方向：
    //   u = normalize(cross(d, ref))，v = cross(d, u) —— (u, v, d) 是右手系，
    //   所以 36 个索引的绕序可以直接照抄，背面剔除/法线都不用另外处理。
    //
    // 顶点建在以 a 为原点的局部坐标里，世界位置交给 body 的 mBodyToWorld，
    // 和 addBoxSpec 的做法保持一致（否则 GetNearMesh 的距离筛选会失效）。
    // ---------------------------------------------------------------------
    inline void AppendBeam(
        std::vector<Vector3>& verts,
        std::vector<uint32_t>& indices,
        const Vector3& a,
        const Vector3& b,
        float halfW)
    {
        Vector3 d = b - a;
        const float len = std::sqrt(d.dot(d));
        if (len < 1.0e-4f) return;
        d = d * (1.0f / len);

        // 参考轴不能和 d 平行，否则叉乘退化成零向量
        Vector3 ref(0.0f, 0.0f, 1.0f);
        if (std::fabs(d.z) > 0.95f) ref = Vector3(1.0f, 0.0f, 0.0f);

        Vector3 u = d.cross(ref).normalized();
        Vector3 v = d.cross(u);

        const uint32_t base = (uint32_t)verts.size();
        const Vector3 cu = u * halfW;
        const Vector3 cv = v * halfW;

        verts.push_back(a - cu - cv);   // 0
        verts.push_back(a + cu - cv);   // 1
        verts.push_back(a + cu + cv);   // 2
        verts.push_back(a - cu + cv);   // 3
        verts.push_back(b - cu - cv);   // 4
        verts.push_back(b + cu - cv);   // 5
        verts.push_back(b + cu + cv);   // 6
        verts.push_back(b - cu + cv);   // 7

        static const uint32_t faces[36] = {
            0,1,2, 0,2,3,   // a 端
            4,6,5, 4,7,6,   // b 端
            0,4,5, 0,5,1,
            3,2,6, 3,6,7,
            0,3,7, 0,7,4,
            1,5,6, 1,6,2
        };
        for (int i = 0; i < 36; i++) indices.push_back(base + faces[i]);
    }

    // ---------------------------------------------------------------------
    // 场景描述：给 main 用的句柄集合
    // ---------------------------------------------------------------------
    struct AnimBody
    {
        uint64_t  addr = 0;          // BodyT 在模拟内存里的地址
        BodyT     body{};            // 当前内容（写回用）
        Vector3   center{};          // 旋转/浮动中心
        Vector3   axis{ 0, 0, 1 };   // 自转轴
        float     amp = 0.0f;        // 浮点幅度
        float     speed = 1.0f;      // 角速度 / 浮动频率
        float     phase = 0.0f;
        int       mode = 0;          // 0=自转 1=上下浮动 2=摆动
    };

    struct DemoScene
    {
        uint64_t physxInstance = 0;   // 写进 GameData.PhysxInstancePtr
        uint64_t sceneAddr     = 0;   // NpSceneT
        std::vector<PrunerPayload> staticObjects;   // exts[0]
        std::vector<PrunerPayload> dynamicObjects;  // exts[1]

        // 只给诊断用：box / convex 这两种几何在生产链路上会被过滤掉，
        // 所以故意不挂进任何 pruner，单独拿出来直接喂 GetMeshData。
        std::vector<PrunerPayload> diagnosticObjects;

        // 用来动态改 timestamp（LoadShapeByRange 要求两次间隔 >= 1000）
        uint32_t timestamp = 0;
        uint64_t staticExtAddr = 0;
        uint64_t dynamicExtAddr = 0;

        std::vector<AnimBody> anim;

        // 每帧推进动态刚体的位姿 + timestamp
        void Tick(float timeSeconds)
        {
            for (auto& a : anim)
            {
                Vector4 rot = QuatIdentity();
                Vector3 pos = a.center;

                if (a.mode == 0)
                {
                    rot = QuatAxisAngle(a.axis, timeSeconds * a.speed + a.phase);
                }
                else if (a.mode == 1)
                {
                    pos.z = a.center.z + std::sin(timeSeconds * a.speed + a.phase) * a.amp;
                }
                else if (a.mode == 2)
                {
                    rot = QuatAxisAngle(Vector3(0, 1, 0), std::sin(timeSeconds * a.speed + a.phase) * a.amp);
                }

                a.body.mRigid.mCore.mBodyToWorld = MakeTransform(pos, rot);
                mockmem::Space().Write(a.addr, &a.body, sizeof(BodyT));
            }

            // 注意：LoadShapeByRange 读的是 scene.exts->mTimestamp，
            // 也就是 NpSceneT 里那份「副本」，不是 PrunerExt 独立块里的值。
            // 两处都写，省得以后有人只改一处又踩一遍。
            timestamp += 1500;
            mockmem::Space().Write(staticExtAddr + offsetof(PrunerExtT, mTimestamp), &timestamp, sizeof(timestamp));
            mockmem::Space().Write(dynamicExtAddr + offsetof(PrunerExtT, mTimestamp), &timestamp, sizeof(timestamp));

            if (sceneAddr)
            {
                uint64_t base = sceneAddr + offsetof(NpSceneT, exts);
                mockmem::Space().Write(base + offsetof(PrunerExtT, mTimestamp), &timestamp, sizeof(timestamp));
                mockmem::Space().Write(base + sizeof(PrunerExtT) + offsetof(PrunerExtT, mTimestamp), &timestamp, sizeof(timestamp));
            }
        }
    };

    // ---------------------------------------------------------------------
    // 构建器
    // ---------------------------------------------------------------------
    class Builder
    {
    public:
        explicit Builder(mockmem::Memory& m) : mem_(m) {}

        // --- Shape ---
        uint64_t AddShape(
            PxGeometryType type,
            const void* geomData,
            size_t geomSize,
            const PxTransformT& shapeToActor,
            uint8_t shapeFlags,
            const FilterDataT& queryFilter,
            const FilterDataT& simFilter,
            const char* tag)
        {
            uint64_t addr = mem_.Alloc(sizeof(ShapeT), tag);

            ShapeT s{};
            s.mScene = 0;
            s.mControlState = 1;
            s.mStreamPtr = 0;

            s.ShapeCore.QueryFilterData = queryFilter;
            s.ShapeCore.SimulationFilterData = simFilter;
            s.ShapeCore.mCore.transform = shapeToActor;
            s.ShapeCore.mCore.contactOffset = 0.02f;
            s.ShapeCore.mCore.mShapeFlags = shapeFlags;
            s.ShapeCore.mCore.mOwnsMaterialIdxMemory = 0;
            s.ShapeCore.mCore.materialIndex = 0xFFFF;

            uint8_t* unionPtr = reinterpret_cast<uint8_t*>(&s.ShapeCore.mCore.geometry.mGeometry);
            std::memset(unionPtr, 0, sizeof(s.ShapeCore.mCore.geometry.mGeometry));
            if (geomData && geomSize > 0)
            {
                size_t copySize = geomSize;
                if (copySize > sizeof(s.ShapeCore.mCore.geometry.mGeometry))
                    copySize = sizeof(s.ShapeCore.mCore.geometry.mGeometry);
                std::memcpy(unionPtr, geomData, copySize);
            }
            // 无论 geomData 是否带 mType，都以前面显式传入的为准
            *reinterpret_cast<PxGeometryType*>(unionPtr) = type;

            mem_.Write(addr, &s, sizeof(s));
            return addr;
        }

        // --- Body ---
        uint64_t AddBody(const PxTransformT& bodyToWorld, const PxTransformT& bodyToActor,
                         PxRigidBodyFlag flags, const char* tag, BodyT* outCopy = nullptr)
        {
            uint64_t addr = mem_.Alloc(sizeof(BodyT), tag);
            BodyT b{};
            b.mScene = 0;
            b.mControlState = 0;
            b.mStreamPtr = 0;
            b.mRigid.mCore.mBodyToWorld = bodyToWorld;
            b.mRigid.mCore.Flags = flags;
            b.mRigid.mCore.m_idt_body_to_actor = 0;
            b.mRigid.mCore.m_solver_iteration_counts = 0;
            b.mRigid.mBodyToActor = bodyToActor;
            mem_.Write(addr, &b, sizeof(b));
            if (outCopy) *outCopy = b;
            return addr;
        }

        // --- Triangle mesh ---
        uint64_t AddTriangleMesh(
            const std::vector<Vector3>& verts,
            const std::vector<uint32_t>& indices,
            bool sixteenBit,
            const char* tag)
        {
            uint64_t meshAddr = mem_.Alloc(sizeof(TriangleMeshT), tag);

            uint64_t vertAddr = mem_.Alloc(std::max<size_t>(verts.size(), 1) * sizeof(Vector3), nullptr);
            if (!verts.empty())
                mem_.Write(vertAddr, verts.data(), verts.size() * sizeof(Vector3));

            TriangleMeshT tm{};
            tm.mType = PxConcreteType::eTRIANGLE_MESH_BVH34;
            tm.mBaseFlags = PxBaseFlag::eOWNS_MEMORY;
            tm.mRefCountableVfptr = 0;
            tm.mRefCount = 1;
            tm.mNbVertices = (uint32_t)verts.size();
            tm.mNbTriangles = (uint32_t)(indices.size() / 3);
            tm.Vertices = reinterpret_cast<Vector3*>(vertAddr);

            uint64_t idxAddr = 0;
            if (sixteenBit)
            {
                std::vector<uint16_t> small(indices.begin(), indices.end());
                idxAddr = mem_.Alloc(small.size() * sizeof(uint16_t), nullptr);
                mem_.Write(idxAddr, small.data(), small.size() * sizeof(uint16_t));
                tm.Flags = 2;            // bit1 = 16-bit indices
            }
            else
            {
                idxAddr = mem_.Alloc(indices.size() * sizeof(uint32_t), nullptr);
                mem_.Write(idxAddr, indices.data(), indices.size() * sizeof(uint32_t));
                tm.Flags = 0;
            }
            tm.mTriangles = reinterpret_cast<void*>(idxAddr);

            Vector3 mn(FLT_MAX, FLT_MAX, FLT_MAX), mx(-FLT_MAX, -FLT_MAX, -FLT_MAX);
            for (const auto& v : verts)
            {
                mn.x = std::min(mn.x, v.x); mn.y = std::min(mn.y, v.y); mn.z = std::min(mn.z, v.z);
                mx.x = std::max(mx.x, v.x); mx.y = std::max(mx.y, v.y); mx.z = std::max(mx.z, v.z);
            }
            tm.mAABB.mCenter = Vector3((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f);
            tm.mAABB.mExtents = Vector3((mx.x - mn.x) * 0.5f, (mx.y - mn.y) * 0.5f, (mx.z - mn.z) * 0.5f);
            tm.mGeomEpsilon = 0.0001f;

            mem_.Write(meshAddr, &tm, sizeof(tm));
            return meshAddr;
        }

        // --- Height field ---
        uint64_t AddHeightField(
            uint32_t rows, uint32_t cols,
            const std::vector<PxHeightFieldSampleT>& samples,
            const char* tag)
        {
            uint64_t hfAddr = mem_.Alloc(sizeof(HeightFieldT), tag);
            uint64_t sampleAddr = mem_.Alloc(std::max<size_t>(samples.size(), 1) * sizeof(PxHeightFieldSampleT), nullptr);
            mem_.Write(sampleAddr, samples.data(), samples.size() * sizeof(PxHeightFieldSampleT));

            HeightFieldT hf{};
            hf.mType = PxConcreteType::eHEIGHTFIELD;
            hf.mBaseFlags = PxBaseFlag::eOWNS_MEMORY;
            hf.mRefCount = 1;
            hf.mData.Rows = rows;
            hf.mData.Columns = cols;
            hf.mData.mRowLimit = (float)(rows - 1);
            hf.mData.mColumnLimit = (float)(cols - 1);
            hf.mData.mNbColumns = (float)cols;
            hf.mData.mSamples = reinterpret_cast<PxHeightFieldSampleT*>(sampleAddr);
            hf.mData.mThickness = 10.0f;
            hf.mData.mConvexEdgeThreshold = 0.1f;
            hf.mData.mFlags = 0;
            hf.mData.mFormat = 0;
            hf.mSampleStride = sizeof(PxHeightFieldSampleT);
            hf.mNbSamples = (uint32_t)samples.size();
            hf.mMinHeight = -1000.0f;
            hf.mMaxHeight = 1000.0f;

            mem_.Write(hfAddr, &hf, sizeof(hf));
            return hfAddr;
        }

        // --- Convex mesh ---
        // 内存布局严格按 ConvexHullDataT 的访问器来排：
        //   [0             .. 20*P)  HullPolygonDataT[]
        //   [20*P          .. +12*V) hull vertices
        //   [+2*E)                   facesByEdges (uint8)
        //   [+3*V)                   facesByVertices (uint8)
        //   [getVertexData8() ...)   每个面的顶点索引（uint8）
        uint64_t AddConvexMesh(
            const std::vector<Vector3>& hullVerts,
            const std::vector<std::vector<uint8_t>>& faces,
            const char* tag,
            uint32_t* outIdxCount = nullptr)
        {
            uint64_t meshAddr = mem_.Alloc(sizeof(ConvexMeshT), tag);

            const uint32_t polyCount = (uint32_t)faces.size();
            const uint32_t vertCount = (uint32_t)hullVerts.size();
            const uint32_t edgeCount = vertCount + polyCount - 2;   // 凸多面体欧拉公式

            uint32_t idxCount = 0;
            for (const auto& f : faces) idxCount += (uint32_t)f.size();

            const size_t polygonsBytes = sizeof(HullPolygonDataT) * polyCount;
            const size_t vertsBytes    = sizeof(Vector3) * vertCount;
            const size_t edgeBytes     = sizeof(uint8_t) * edgeCount * 2;
            const size_t vertTriBytes  = sizeof(uint8_t) * vertCount * 3;
            const size_t idxOffset     = polygonsBytes + vertsBytes + edgeBytes + vertTriBytes;
            const size_t total         = idxOffset + idxCount;

            uint64_t blobAddr = mem_.Alloc(total, nullptr);
            std::vector<uint8_t> blob(total, 0);

            auto* polys = reinterpret_cast<HullPolygonDataT*>(blob.data());
            uint16_t vref = 0;
            for (uint32_t i = 0; i < polyCount; i++)
            {
                const auto& f = faces[i];
                polys[i].mPlane.n = Vector3(0, 1, 0);
                polys[i].mPlane.d = 0.0f;
                polys[i].mVRef8 = vref;
                polys[i].mNbVerts = (uint8_t)f.size();
                polys[i].mMinIndex = 0;
                vref += (uint16_t)f.size();
            }

            auto* hv = reinterpret_cast<Vector3*>(blob.data() + polygonsBytes);
            for (uint32_t i = 0; i < vertCount; i++) hv[i] = hullVerts[i];

            uint8_t* idxBase = blob.data() + idxOffset;
            uint32_t w = 0;
            for (const auto& f : faces)
                for (uint8_t vi : f)
                    idxBase[w++] = vi;

            mem_.Write(blobAddr, blob.data(), blob.size());

            ConvexMeshT cm{};
            cm.mType = PxConcreteType::eCONVEX_MESH;
            cm.mBaseFlags = PxBaseFlag::eOWNS_MEMORY;
            cm.mRefCount = 1;
            cm.HullData.mAABB.mCenter = Vector3(0, 0, 0);
            cm.HullData.mAABB.mExtents = Vector3(1, 1, 1);
            cm.HullData.m_center_of_mass = Vector3(0, 0, 0);
            cm.HullData.mNbEdges = PxBitAndWord((unsigned short)edgeCount);
            cm.HullData.HullVerticesNb = (uint8_t)vertCount;
            cm.HullData.PolygonsNb = (uint8_t)polyCount;
            cm.HullData.mPolygons = reinterpret_cast<HullPolygonDataT*>(blobAddr);

            mem_.Write(meshAddr, &cm, sizeof(cm));
            if (outIdxCount) *outIdxCount = idxCount;
            return meshAddr;
        }

        // --- 挂到 pruner ---
        void Attach(uint64_t shapeAddr, uint64_t actorAddr, bool dynamic)
        {
            PrunerPayload p{ shapeAddr, actorAddr };
            if (dynamic) dynamicObjects_.push_back(p);
            else         staticObjects_.push_back(p);
        }

        const std::vector<PrunerPayload>& StaticObjects() const  { return staticObjects_; }
        const std::vector<PrunerPayload>& DynamicObjects() const { return dynamicObjects_; }

        // --- 收尾：建 pruner / NpScene / PhysxInstance ---
        uint64_t BuildScene(uint64_t* outStaticExt, uint64_t* outDynamicExt)
        {
            uint64_t staticPool  = BuildPool(staticObjects_,  "pruner_static");
            uint64_t dynamicPool = BuildPool(dynamicObjects_, "pruner_dynamic");

            uint64_t ext0 = BuildPrunerExt(staticPool,  "PrunerExt_static");
            uint64_t ext1 = BuildPrunerExt(dynamicPool, "PrunerExt_dynamic");
            if (outStaticExt)  *outStaticExt  = ext0;
            if (outDynamicExt) *outDynamicExt = ext1;

            uint64_t sceneAddr = mem_.Alloc(sizeof(NpSceneT), "NpScene");
            NpSceneT sc{};
            sc.exts[0].mPruner = ext0;
            sc.exts[0].mTimestamp = 0;
            sc.exts[1].mPruner = ext1;
            sc.exts[1].mTimestamp = 0;
            mem_.Write(sceneAddr, &sc, sizeof(sc));

            // 读取方写法：
            //   px_scene_arr_ptr = Read<uint64>(PhysxInstancePtr + 0x8)
            //   px_scene_ptr     = Read<uint64>(px_scene_arr_ptr)
            // 所以 +0x8 放「指针数组」，数组[0] 指向 NpSceneT
            uint64_t arrAddr = mem_.Alloc(sizeof(uint64_t), "scene_array");
            uint64_t scenePtrVal = sceneAddr;
            mem_.Write(arrAddr, &scenePtrVal, sizeof(scenePtrVal));

            uint64_t instAddr = mem_.Alloc(0x40, "PhysxInstance");
            mem_.Write(instAddr + 0x8, &arrAddr, sizeof(arrAddr));

            lastSceneAddr_ = sceneAddr;
            return instAddr;
        }

    private:
        mockmem::Memory& mem_;
        std::vector<PrunerPayload> staticObjects_;
        std::vector<PrunerPayload> dynamicObjects_;
        uint64_t lastSceneAddr_ = 0;

        uint64_t BuildPool(const std::vector<PrunerPayload>& objs, const char* tag)
        {
            uint64_t objectsAddr = 0;
            if (!objs.empty())
            {
                objectsAddr = mem_.Alloc(objs.size() * sizeof(PrunerPayload), nullptr);
                mem_.Write(objectsAddr, objs.data(), objs.size() * sizeof(PrunerPayload));
            }

            uint64_t boundsAddr = mem_.Alloc(std::max<size_t>(objs.size(), 1) * sizeof(PxBounds3), nullptr);

            uint64_t poolAddr = mem_.Alloc(sizeof(PruningPoolT), tag);
            PruningPoolT pool{};
            pool.mNbObjects = (uint32_t)objs.size();
            pool.mMaxNbObjects = (uint32_t)objs.size();
            pool.mWorldBoxes = reinterpret_cast<PxBounds3*>(boundsAddr);
            pool.mObjects = reinterpret_cast<PrunerPayload*>(objectsAddr);
            mem_.Write(poolAddr, &pool, sizeof(pool));
            return poolAddr;
        }

        // 读取方是 mem.Read<PruningPoolT>(scene.exts[i].mPruner + 0x1A0)
        // —— 注意是内联，不是指针解引用。所以 PruningPoolT 必须整块写在 +0x1A0。
        uint64_t BuildPrunerExt(uint64_t poolAddr, const char* tag)
        {
            uint64_t base = mem_.Alloc(0x1A0 + sizeof(PruningPoolT) + 0x40, tag);

            PrunerExtT ext{};
            ext.mPruner = base;
            ext.mTimestamp = 0;
            mem_.Write(base, &ext, sizeof(ext));

            PruningPoolT pool{};
            mem_.Read(poolAddr, &pool, sizeof(pool));
            mem_.Write(base + 0x1A0, &pool, sizeof(pool));
            return base;
        }
    };

    // ---------------------------------------------------------------------
    // 演示场景
    // ---------------------------------------------------------------------
    inline DemoScene BuildDemo()
    {
        Builder b(mockmem::Space());
        DemoScene demo;

        // 房间中心（heightfield 变换后落在 x∈[0,4000], y∈[-4000,0]）
        const float CX = 2000.0f, CY = -2000.0f;

        // 房间整体架空高度。
        //
        // 地形起伏 ±70cm（见下面 heightfield 的 sin/cos 公式），而原来是
        // 把 10cm 厚的地板直接坐在 z=0，结果地形从地板里顶出来一片。
        //
        // 压平地形虽然能消除穿插，但会在房子四周留下一圈被削平的台面，
        // 从开口往外看地形是断的 —— 看着比穿模还假。所以改成不动地形，
        // 把整座房子抬到地形最高点之上：架空地板 + 木柱支撑，地形从下面
        // 自然穿过，起伏完整保留。
        //
        // 140 的取值：地形峰值 +70（房间 footprint 内实测 +59），地板底面落在
        // 135，净空约 76cm —— 能看清架空木构和斜撑，又不至于像在飞。
        const float LIFT = 140.0f;

        // 地板顶面标高（瓷砖上表面），下面多处要按它对齐
        const float FLOOR_TOP = LIFT + 10.0f;

        // ---------------- 地面：heightfield ----------------
        // 采样点 x = row*RowScale, y = height, z = col*ColumnScale
        // shape 变换绕 X 轴 +90°，把 (x, h, z) 映射成 (x, -z, h) —— 变成 Z 向上的世界
        const uint32_t HF_ROWS = 41, HF_COLS = 41;
        std::vector<PxHeightFieldSampleT> samples(HF_ROWS * HF_COLS);
        for (uint32_t r = 0; r < HF_ROWS; r++)
        {
            for (uint32_t c = 0; c < HF_COLS; c++)
            {
                float x = (float)r * 100.0f;
                float z = (float)c * 100.0f;
                float h = std::sin(x * 0.004f) * 40.0f + std::cos(z * 0.005f) * 30.0f;
                PxHeightFieldSampleT& s = samples[r * HF_COLS + c];
                s.mHeight = (int16_t)h;
                s.mMaterialIndex0 = PxBitAndByte((unsigned char)0);
                s.mMaterialIndex1 = PxBitAndByte((unsigned char)0);
            }
        }

        uint64_t hfMesh = b.AddHeightField(HF_ROWS, HF_COLS, samples, "HeightField");

        PxHeightFieldGeometryT hfGeom{};
        hfGeom.mType = PxGeometryType::eHEIGHTFIELD;
        hfGeom.mHeightField = reinterpret_cast<HeightFieldT*>(hfMesh);
        hfGeom.HeightScale = 1.0f;
        hfGeom.RowScale = 100.0f;      // 必须 100，见 RefreshDynamicLoadHeightField
        hfGeom.ColumnScale = 100.0f;
        hfGeom.Flags = 0;

        uint64_t hfShape = b.AddShape(
            PxGeometryType::eHEIGHTFIELD, &hfGeom, sizeof(hfGeom),
            MakeTransform(Vector3(0, 0, 0), QuatAxisAngle(Vector3(1, 0, 0), 1.57079633f)),
            0, FilterDataT{ 0, 0, 0, 0 }, FilterDataT{ 0, 0, 0, 0 }, "Shape_heightfield");

        uint64_t hfBody = b.AddBody(
            MakeTransform(Vector3(0, 0, 0), QuatIdentity()),
            MakeTransform(Vector3(0, 0, 0), QuatIdentity()),
            PxRigidBodyFlag::eKINEMATIC, "Body_heightfield");

        b.Attach(hfShape, hfBody, false);

        // ---------------- 房间：triangle mesh ----------------
        // 顶点一律建在局部坐标（原点为中心），世界位置交给 body 的 mBodyToWorld。
        // 这样 TriangleMeshData.Transform.mPosition 才有意义 —— GetNearMesh 用它
        // 做距离筛选，HUD 也用它显示位置；如果顶点直接写成世界坐标而 body 留在
        // 原点，所有静态网格的 Transform 都会是 (0,0,0)。
        struct MeshSpec
        {
            std::vector<Vector3> verts;
            std::vector<uint32_t> indices;
            bool sixteenBit;
            uint8_t shapeFlags;
            FilterDataT queryFilter;
            PxTransformT shapeTransform;
            Vector3 bodyPos;
            // 用 std::string 而不是 const char*：瓷砖是循环里批量生成的，
            // 名字要拼数字，传字面量指针会悬空。
            std::string tag;
        };

        std::vector<MeshSpec> specs;

        auto addBoxSpec = [&](Vector3 center, Vector3 half, bool sixteenBit,
                              uint8_t flags, FilterDataT qf, const char* tag)
        {
            MeshSpec spec;
            AppendBox(spec.verts, spec.indices, Vector3(0, 0, 0), half);
            spec.sixteenBit = sixteenBit;
            spec.shapeFlags = flags;
            spec.queryFilter = qf;
            spec.shapeTransform = MakeTransform(Vector3(0, 0, 0), QuatIdentity());
            spec.bodyPos = center;
            spec.tag = tag;
            specs.push_back(std::move(spec));
        };

        // 两点之间的方梁/斜撑。顶点建在局部坐标（以 a 为原点），
        // body 摆到 a 上 —— 和 addBoxSpec 同一条链路。
        auto addBeamSpec = [&](Vector3 a, Vector3 b, float halfW, const std::string& tag)
        {
            MeshSpec spec;
            AppendBeam(spec.verts, spec.indices, Vector3(0, 0, 0), b - a, halfW);
            spec.sixteenBit = true;
            spec.shapeFlags = 1;
            spec.queryFilter = FilterDataT{0,0,0,0};
            spec.shapeTransform = MakeTransform(Vector3(0, 0, 0), QuatIdentity());
            spec.bodyPos = a;
            spec.tag = tag;
            specs.push_back(std::move(spec));
        };

        // 四面墙（16 位索引）。墙底 LIFT，坐在楼板上
        addBoxSpec(Vector3(CX, CY - 500, 200 + LIFT), Vector3(500, 10, 200), true, 1, FilterDataT{0,0,0,0}, "Mesh_wall_S");
        addBoxSpec(Vector3(CX, CY + 500, 200 + LIFT), Vector3(500, 10, 200), true, 1, FilterDataT{0,0,0,0}, "Mesh_wall_N");
        addBoxSpec(Vector3(CX - 500, CY, 200 + LIFT), Vector3(10, 500, 200), true, 1, FilterDataT{0,0,0,0}, "Mesh_wall_W");
        // 东侧故意不砌墙 —— 相机从东侧看进来，否则近处那面墙会糊满整个屏幕
        // （真实对局里也一样：贴墙站着只能看到墙）。

        // 楼板（木构）：z∈[LIFT-5, LIFT+5]
        addBoxSpec(Vector3(CX, CY, LIFT), Vector3(500, 500, 5), true, 1, FilterDataT{0,0,0,0}, "Mesh_floor");

        // 瓷地板砖：楼板顶面铺一层 5×5 大板砖，砖缝 2cm。
        // 缝里露出的是楼板顶面，填充只叠一次所以缝比砖暗 —— 那一圈圈暗线
        // 就是瓷砖格。砖本身 z∈[LIFT+5, FLOOR_TOP]，跟楼板错开半个面，
        // 各条棱都是独立几何的边界，硬边照常画得出来。
        {
            const int   NT     = 5;
            const float cell   = 1000.0f / NT;              // 每格 200cm
            const float gap    = 2.0f;                      // 砖缝宽
            const float tileHz = cell * 0.5f - gap * 0.5f;  // 砖半宽
            const float tileZc = (LIFT + 5.0f + FLOOR_TOP) * 0.5f;
            const float tileZs = (FLOOR_TOP - (LIFT + 5.0f)) * 0.5f;

            for (int iy = 0; iy < NT; iy++)
            {
                for (int ix = 0; ix < NT; ix++)
                {
                    const float px = CX - 500.0f + cell * ((float)ix + 0.5f);
                    const float py = CY - 500.0f + cell * ((float)iy + 0.5f);

                    char name[48];
                    sprintf_s(name, "Mesh_tile_%d_%d", ix, iy);
                    addBoxSpec(Vector3(px, py, tileZc), Vector3(tileHz, tileHz, tileZs),
                               true, 1, FilterDataT{0,0,0,0}, name);
                }
            }
        }

        // ---------------- 架空木构：四柱 + 圈梁 + 倒三角斜撑 ----------------
        //
        // 为什么加斜撑：矩形框架（两根柱 + 上下横杆）在侧向力下是可变形的，
        // 加上一条斜边之后变成三角形，几何上就被锁死了 —— 这就是"三角形
        // 具有稳定性"的来历。这里每一面放一个倒三角（宽边在上 = 圈梁，
        // 尖角在下 = 落到地面），四面各一个，从任何角度看都是三角桁架。
        //
        // 柱底压到 -80（地形最低点 -70 以下），任何起伏处都插在土里，
        // 不会出现某根柱子悬空。位置贴地板边缘（±460），缩进内侧会被
        // 楼板整个盖住，从俯角看只剩个柱脚。
        {
            const float PX       = 460.0f;      // 柱位（相对房间中心）
            const float POST_HW  = 32.0f;       // 柱截面 half → 64cm 见方
            const float BEAM_HW  = 19.0f;       // 梁/撑截面 half → 38cm 见方
            const float POST_BOT = -80.0f;      // 柱底
            const float POST_TOP = LIFT;        // 柱顶 = 楼板底面
            const float BEAM_ZC  = LIFT - BEAM_HW;  // 圈梁中心，贴着楼板
            const float TIP_Z    = -70.0f;      // 倒三角的尖，落到地面

            const float px[4] = { -PX, +PX, +PX, -PX };
            const float py[4] = { -PX, -PX, +PX, +PX };
            const char* postTag[4] = { "Mesh_post_SW", "Mesh_post_SE", "Mesh_post_NE", "Mesh_post_NW" };

            // 1) 四根竖柱
            for (int i = 0; i < 4; i++)
            {
                addBoxSpec(
                    Vector3(CX + px[i], CY + py[i], (POST_TOP + POST_BOT) * 0.5f),
                    Vector3(POST_HW, POST_HW, (POST_TOP - POST_BOT) * 0.5f),
                    true, 1, FilterDataT{0,0,0,0}, postTag[i]);
            }

            // 2) 柱顶圈梁：四边各一根，把四根柱连成一个刚性平面
            addBeamSpec(Vector3(CX - PX, CY - PX, BEAM_ZC), Vector3(CX + PX, CY - PX, BEAM_ZC), BEAM_HW, "Mesh_rim_S");
            addBeamSpec(Vector3(CX - PX, CY + PX, BEAM_ZC), Vector3(CX + PX, CY + PX, BEAM_ZC), BEAM_HW, "Mesh_rim_N");
            addBeamSpec(Vector3(CX - PX, CY - PX, BEAM_ZC), Vector3(CX - PX, CY + PX, BEAM_ZC), BEAM_HW, "Mesh_rim_W");
            addBeamSpec(Vector3(CX + PX, CY - PX, BEAM_ZC), Vector3(CX + PX, CY + PX, BEAM_ZC), BEAM_HW, "Mesh_rim_E");

            // 3) 倒三角斜撑：每面两根，从圈梁两端斜向内下，交于地面中点。
            //    三角 = 圈梁（上边）+ 这两根斜撑。四面共 8 根。
            struct Side { const char* name; float ax, ay, bx, by; };
            const Side sides[4] = {
                { "S", CX - PX, CY - PX, CX + PX, CY - PX },
                { "N", CX - PX, CY + PX, CX + PX, CY + PX },
                { "W", CX - PX, CY - PX, CX - PX, CY + PX },
                { "E", CX + PX, CY - PX, CX + PX, CY + PX },
            };

            for (int s = 0; s < 4; s++)
            {
                const Side& sd = sides[s];

                // 尖角：这一面的中点，落到地面
                const float mx = (sd.ax + sd.bx) * 0.5f;
                const float my = (sd.ay + sd.by) * 0.5f;
                const Vector3 tip(mx, my, TIP_Z);

                std::string t0 = std::string("Mesh_brace_") + sd.name + "_a";
                std::string t1 = std::string("Mesh_brace_") + sd.name + "_b";

                addBeamSpec(Vector3(sd.ax, sd.ay, BEAM_ZC), tip, BEAM_HW, t0);
                addBeamSpec(Vector3(sd.bx, sd.by, BEAM_ZC), tip, BEAM_HW, t1);
            }
        }

        // 立柱：故意用 32 位索引，走 TriangleMeshT.Flags & 2 == 0 那一支
        addBoxSpec(Vector3(CX - 300, CY - 350, 200 + LIFT), Vector3(30, 30, 200), false, 1, FilterDataT{0,0,0,0}, "Mesh_column_32bit");

        // 可破坏墙：QueryFilterData.word0/word2 都 > 0，命中原版 alwaysCheckShape 分支
        addBoxSpec(Vector3(CX + 200, CY - 498, 150 + LIFT), Vector3(200, 8, 150), true, 1,
                   FilterDataT{ 1, 0, 1, 0 }, "Mesh_breakable_wall");

        // 斜坡：网格本身是平的，靠 shape 局部 transform 摆成斜面 —— 验证
        // CombinePose = GlobalPose * LocalPose 这条链
        {
            MeshSpec ramp;
            AppendBox(ramp.verts, ramp.indices, Vector3(0, 0, 0), Vector3(200, 120, 6));
            ramp.sixteenBit = true;
            ramp.shapeFlags = 1;
            ramp.queryFilter = FilterDataT{ 0,0,0,0 };
            ramp.shapeTransform = MakeTransform(
                Vector3(0, 0, 0),
                QuatAxisAngle(Vector3(1, 0, 0), -0.35f));   // 局部倾斜
            ramp.bodyPos = Vector3(CX, CY + 200, 90 + LIFT);  // 世界位置
            ramp.tag = "Mesh_ramp";
            specs.push_back(std::move(ramp));
        }

        for (auto& spec : specs)
        {
            uint64_t mesh = b.AddTriangleMesh(spec.verts, spec.indices, spec.sixteenBit, spec.tag.c_str());

            PxTriangleMeshGeometryT geom{};
            geom.mType = PxGeometryType::eTRIANGLEMESH;
            geom.Scale.scale = Vector3(1, 1, 1);          // 必须是 1，否则顶点被压成 0
            geom.Scale.rotation = QuatIdentity();
            geom.mMeshFlags = 0;
            geom.mTriangleMesh = reinterpret_cast<TriangleMeshT*>(mesh);

            uint64_t shape = b.AddShape(
                PxGeometryType::eTRIANGLEMESH, &geom, sizeof(geom),
                spec.shapeTransform, spec.shapeFlags, spec.queryFilter,
                FilterDataT{ 0, 0, 0, 0 }, spec.tag.c_str());

            uint64_t body = b.AddBody(
                MakeTransform(spec.bodyPos, QuatIdentity()),
                MakeTransform(Vector3(0, 0, 0), QuatIdentity()),
                PxRigidBodyFlag::eKINEMATIC, "Body_static");

            b.Attach(shape, body, false);
        }

        // ---------------- 动态刚体：三个可动 triangle mesh prop ----------------
        //
        // 为什么动态物体也用 triangle mesh 而不是 box：
        //   LoadDynamicRigidShape 的过滤谓词是
        //       return type != PxGeometryType::eTRIANGLEMESH;
        //   （Physx.h:1140）—— 返回 true 即删除，所以 box / convex 在这里
        //   一律被丢掉，压根到不了 GetMeshData 的 box 分支。
        //   想让动态场景真的有东西，几何体就必须是 triangle mesh。
        //   box / convex 两条分支的实际行为放在下面的「诊断对象」里单独验证。

        auto addDynamicProp = [&](const std::vector<Vector3>& verts,
                                  const std::vector<uint32_t>& indices,
                                  const Vector3& center,
                                  const PxTransformT& bodyToActor,
                                  int mode, float speed, float amp,
                                  float phase, const Vector3& axis,
                                  const char* tag)
        {
            uint64_t mesh = b.AddTriangleMesh(verts, indices, true, tag);

            PxTriangleMeshGeometryT geom{};
            geom.mType = PxGeometryType::eTRIANGLEMESH;
            geom.Scale.scale = Vector3(1, 1, 1);
            geom.Scale.rotation = QuatIdentity();
            geom.mMeshFlags = 0;
            geom.mTriangleMesh = reinterpret_cast<TriangleMeshT*>(mesh);

            uint64_t shape = b.AddShape(
                PxGeometryType::eTRIANGLEMESH, &geom, sizeof(geom),
                MakeTransform(Vector3(0, 0, 0), QuatIdentity()),
                1, FilterDataT{ 0, 0, 0, 0 }, FilterDataT{ 0, 0, 0, 0 }, tag);

            AnimBody anim;
            anim.addr = b.AddBody(
                MakeTransform(center, QuatIdentity()),
                bodyToActor,
                PxRigidBodyFlag::eENABLE_CCD, tag, &anim.body);
            anim.center = center;
            anim.axis = axis;
            anim.mode = mode;
            anim.speed = speed;
            anim.amp = amp;
            anim.phase = phase;

            b.Attach(shape, anim.addr, true);
            demo.anim.push_back(anim);
        };

        // 1) 箱子外形（局部坐标以原点为中心，位姿全靠 body 摆）
        {
            std::vector<Vector3> v;
            std::vector<uint32_t> idx;
            AppendBox(v, idx, Vector3(0, 0, 0), Vector3(60, 60, 60));
            addDynamicProp(v, idx, Vector3(CX - 150, CY - 150, 160 + LIFT),
                MakeTransform(Vector3(0, 0, 40), QuatIdentity()),   // bodyToActor 带平移 -> 走 getInverse
                0, 1.2f, 0.9f, 0.0f, Vector3(0, 0, 1), "Prop_crate");
        }

        // 2) 八棱柱外形：绕自身轴自转 + 上下浮动
        {
            const int N = 8;
            const float R = 65.0f, H = 140.0f;
            std::vector<Vector3> v;
            std::vector<uint32_t> idx;

            for (int i = 0; i < N; i++)
            {
                float a = (float)i / N * 6.2831853f;
                v.push_back(Vector3(std::cos(a) * R, std::sin(a) * R, -H * 0.5f));
            }
            for (int i = 0; i < N; i++)
            {
                float a = (float)i / N * 6.2831853f;
                v.push_back(Vector3(std::cos(a) * R, std::sin(a) * R, H * 0.5f));
            }
            for (int i = 0; i < N; i++)
            {
                int j = (i + 1) % N;
                idx.push_back(i);       idx.push_back(j);       idx.push_back(N + j);
                idx.push_back(i);       idx.push_back(N + j);   idx.push_back(N + i);
            }
            // 顶面 / 底面扇形
            for (int i = 1; i + 1 < N; i++)
            {
                idx.push_back(0);       idx.push_back(i + 1);   idx.push_back(i);
                idx.push_back(N);       idx.push_back(N + i);   idx.push_back(N + i + 1);
            }

            addDynamicProp(v, idx, Vector3(CX + 150, CY + 150, 180 + LIFT),
                MakeTransform(Vector3(0, 0, 0), QuatIdentity()),
                1, 1.6f, 90.0f, 0.9f, Vector3(0, 0, 1), "Prop_barrel");
        }

        // 3) 薄板：绕 Y 轴摆动，bodyToActor 带旋转 —— 验证四元数求逆
        {
            std::vector<Vector3> v;
            std::vector<uint32_t> idx;
            AppendBox(v, idx, Vector3(0, 0, 0), Vector3(150, 12, 90));
            addDynamicProp(v, idx, Vector3(CX + 150, CY - 150, 320 + LIFT),
                MakeTransform(Vector3(0, 0, 30), QuatAxisAngle(Vector3(1, 0, 0), 0.35f)),
                2, 0.9f, 0.55f, 2.1f, Vector3(0, 1, 0), "Prop_panel");
        }

        // ---------------- 诊断对象：box + convex（故意不挂 pruner）----------------
        //
        // 这两条分支在现在的加载链路里到不了（被上面两个过滤谓词全删了）。
        // 想验证它们的代码本身对不对，只能绕过 pruner 直接调 GetMeshData。
        // 挂在 demo.diagnosticObjects 上，由 main 里的 DiagnoseMeshes 使用。
        {
            // box
            PxBoxGeometry boxGeom{};
            boxGeom.mType = PxGeometryType::eBOX;
            boxGeom.mHalfExtents = Vector3(50, 50, 50);

            uint64_t boxShape = b.AddShape(
                PxGeometryType::eBOX, &boxGeom, sizeof(boxGeom),
                MakeTransform(Vector3(0, 0, 0), QuatIdentity()),
                0, FilterDataT{ 0, 0, 0, 0 }, FilterDataT{ 0, 0, 0, 0 }, "Diag_box");
            uint64_t boxBody = b.AddBody(
                MakeTransform(Vector3(CX, CY, 400 + LIFT), QuatIdentity()),
                MakeTransform(Vector3(0, 0, 0), QuatIdentity()),
                PxRigidBodyFlag::eENABLE_CCD, "Diag_box_body");

            // convex：八棱柱凸包
            const int N = 8;
            const float R = 70.0f, H = 120.0f;
            std::vector<Vector3> hv;
            for (int i = 0; i < N; i++)
            {
                float a = (float)i / N * 6.2831853f;
                hv.push_back(Vector3(std::cos(a) * R, std::sin(a) * R, 0));
            }
            for (int i = 0; i < N; i++)
            {
                float a = (float)i / N * 6.2831853f;
                hv.push_back(Vector3(std::cos(a) * R, std::sin(a) * R, H));
            }

            std::vector<std::vector<uint8_t>> faces;
            for (int i = 0; i < N; i++)
            {
                int j = (i + 1) % N;
                faces.push_back({ (uint8_t)i, (uint8_t)j, (uint8_t)(N + j), (uint8_t)(N + i) });
            }
            std::vector<uint8_t> topFace, bottomFace;
            for (int i = 0; i < N; i++) { topFace.push_back((uint8_t)(N + i)); bottomFace.push_back((uint8_t)(N - 1 - i)); }
            faces.push_back(topFace);
            faces.push_back(bottomFace);

            uint32_t idxCount = 0;
            uint64_t convexMesh = b.AddConvexMesh(hv, faces, "ConvexMesh", &idxCount);

            PxConvexMeshGeometryT convexGeom{};
            convexGeom.mType = PxGeometryType::eCONVEXMESH;
            convexGeom.Scale.scale = Vector3(1, 1, 1);
            convexGeom.Scale.rotation = QuatIdentity();
            convexGeom.ConvexMesh = reinterpret_cast<ConvexMeshT*>(convexMesh);
            convexGeom.mMaxMargin = 0.05f;
            convexGeom.mMeshFlags = 0;

            // GetMeshData 里凸包多一道闸：QueryFilterData.word3 > 100 才入列
            uint64_t convexShape = b.AddShape(
                PxGeometryType::eCONVEXMESH, &convexGeom, sizeof(convexGeom),
                MakeTransform(Vector3(0, 0, 0), QuatIdentity()),
                0, FilterDataT{ 0, 0, 0, 200 }, FilterDataT{ 0, 0, 0, 0 }, "Diag_convex");
            uint64_t convexBody = b.AddBody(
                MakeTransform(Vector3(CX + 320, CY + 320, 120 + LIFT), QuatIdentity()),
                MakeTransform(Vector3(0, 0, 0), QuatIdentity()),
                PxRigidBodyFlag::eENABLE_CCD, "Diag_convex_body");

            demo.diagnosticObjects.push_back(PrunerPayload{ boxShape, boxBody });
            demo.diagnosticObjects.push_back(PrunerPayload{ convexShape, convexBody });
        }

        demo.physxInstance = b.BuildScene(&demo.staticExtAddr, &demo.dynamicExtAddr);
        demo.staticObjects = b.StaticObjects();
        demo.dynamicObjects = b.DynamicObjects();
        demo.sceneAddr = demo.physxInstance ? mockmem::Space().FindByTag("NpScene") : 0;
        return demo;
    }
} // namespace mockscene
