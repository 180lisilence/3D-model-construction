#pragma once

// ============================================================================
// mock/EmbreeCompat.h
//
// 纯 CPU 的 Embree4 API 兼容层。
//
// 目的：让 VisibleScene.h 一行不改也能编译运行，不依赖 embree4.dll / tbb12.dll。
//
// 覆盖范围 = VisibleScene.h 实际用到的接口子集：
//   设备/场景   rtcNewDevice / rtcReleaseDevice / rtcSetDeviceErrorFunction
//               rtcSetDeviceProperty / rtcNewScene / rtcReleaseScene
//               rtcSetSceneBuildQuality / rtcSetSceneFlags / rtcCommitScene
//   几何        rtcNewGeometry / rtcReleaseGeometry / rtcAttachGeometry
//               rtcGetGeometry / rtcDisableGeometry / rtcEnableGeometry
//               rtcNewGeometryBuffer / rtcSetGeometryUserData
//               rtcGetGeometryUserData / rtcCommitGeometry
//   求交        rtcInitRayQueryContext / rtcInitIntersectArguments / rtcIntersect1
//
// 句柄设计（关键）：
//   本层把 RTCScene 和 RTCGeometry 都定义为「指向堆对象的裸指针的整数化」。
//   rtcGetGeometry(scene, id) 返回 id 本身，因此 rtcDisableGeometry /
//   rtcEnableGeometry 只拿到 id —— 所以几何状态挂在场景里，而场景指针
//   通过线程局部的 currentScene 传递（VisibleScene 是单线程串行调用的）。
//
// 与 Embree 的语义对齐点：
//   - geomID 单调递增、永不复用
//   - disabled 几何不参与求交，rtcGetGeometryUserData 仍可读
//   - hit.geomID == RTC_INVALID_GEOMETRY_ID 表示未命中
//   - ray.tfar 命中后收缩到最近交点距离
//
// 实现：扁平三角形表 + 中位数分割 BVH + 迭代栈求交（Moller-Trumbore）。
// ============================================================================

#include <cstdint>
#include <cmath>
#include <cstring>
#include <cfloat>
#include <vector>
#include <algorithm>
#include <unordered_map>

// ---------------------------------------------------------------------------
// 基础类型
// ---------------------------------------------------------------------------
typedef uint64_t RTCGeometry;
typedef uint64_t RTCScene;

#define RTC_INVALID_GEOMETRY_ID ((unsigned int)-1)
#define RTC_MAX_INSTANCE_LEVEL_COUNT 1

enum RTCError
{
    RTC_ERROR_NONE = 0,
    RTC_ERROR_UNKNOWN = 1,
    RTC_ERROR_INVALID_ARGUMENT = 2,
    RTC_ERROR_INVALID_OPERATION = 3,
    RTC_ERROR_OUT_OF_MEMORY = 4,
    RTC_ERROR_UNSUPPORTED_CPU = 5,
    RTC_ERROR_CANCELLED = 6,
};

enum RTCDeviceProperty
{
    RTC_DEVICE_PROPERTY_VERSION = 0,
    RTC_DEVICE_PROPERTY_VERSION_MAJOR = 1,
    RTC_DEVICE_PROPERTY_VERSION_MINOR = 2,
    RTC_DEVICE_PROPERTY_VERSION_PATCH = 3,
    RTC_DEVICE_PROPERTY_TASKING_SYSTEM = 9,
};

enum RTCSceneFlagBits
{
    RTC_SCENE_FLAG_NONE = 0,
    RTC_SCENE_FLAG_DYNAMIC = (1 << 0),
    RTC_SCENE_FLAG_COMPACT = (1 << 1),
    RTC_SCENE_FLAG_ROBUST = (1 << 2),
};
typedef unsigned int RTCSceneFlags;

enum RTCBuildQuality
{
    RTC_BUILD_QUALITY_LOW = 0,
    RTC_BUILD_QUALITY_MEDIUM = 1,
    RTC_BUILD_QUALITY_HIGH = 2,
    RTC_BUILD_QUALITY_REFIT = 3,
};

enum RTCGeometryType
{
    RTC_GEOMETRY_TYPE_TRIANGLE = 0,
};

enum RTCFormat
{
    RTC_FORMAT_UNDEFINED = 0,
    RTC_FORMAT_UINT = 0x1000,
    RTC_FORMAT_UINT3 = 0x1002,
    RTC_FORMAT_FLOAT = 0x2000,
    RTC_FORMAT_FLOAT3 = 0x2002,
    RTC_FORMAT_FLOAT4 = 0x2003,
};

enum RTCBufferType
{
    RTC_BUFFER_TYPE_INDEX = 0,
    RTC_BUFFER_TYPE_VERTEX = 1,
};

// ---------------------------------------------------------------------------
// 射线 / 命中
// ---------------------------------------------------------------------------
struct RTCRay
{
    float org_x, org_y, org_z;
    float tnear;
    float dir_x, dir_y, dir_z;
    float time;
    float tfar;
    unsigned int mask;
    unsigned int id;
    unsigned int flags;
};

struct RTCHit
{
    float Ng_x, Ng_y, Ng_z;
    float u, v;
    unsigned int primID;
    unsigned int geomID;
    unsigned int instID[RTC_MAX_INSTANCE_LEVEL_COUNT];
    unsigned int instPrimID[RTC_MAX_INSTANCE_LEVEL_COUNT];
};

struct RTCRayHit
{
    RTCRay ray;
    RTCHit hit;
};

struct RTCRayQueryContext
{
    unsigned int instStackSize;
    unsigned int instID[RTC_MAX_INSTANCE_LEVEL_COUNT];
};

struct RTCIntersectArguments
{
    RTCSceneFlags flags;
    void* feature_mask;
    void* context;
    void* filter;
};

typedef void (*RTCErrorFunction)(void* userPtr, enum RTCError code, const char* str);

// ---------------------------------------------------------------------------
// 内部实现
// ---------------------------------------------------------------------------
namespace mockembree
{
    struct Tri
    {
        float v0[3], v1[3], v2[3];
        unsigned int geomID;
        unsigned int primID;
    };

    struct BvhNode
    {
        float bmin[3], bmax[3];
        int left = -1;
        int right = -1;
        int triStart = 0;
        int triCount = 0;
    };

    struct GeomState
    {
        bool enabled = true;
        bool committed = false;
        void* userData = nullptr;
        std::vector<float> vertices;
        std::vector<unsigned int> indices;
    };

    // 尚未 attach 的几何（rtcNewGeometry 到 rtcAttachGeometry 之间）。
    // 键从 0x08000000 起，故意和场景内的 geomID（0,1,2...）错开 ——
    // 否则「复用被 disable 的 geomID」会和「新建几何的句柄」撞号。
    struct GeomHandle
    {
        bool alive = false;
        void* userData = nullptr;
        std::vector<float> vertices;
        std::vector<unsigned int> indices;
    };

    inline std::unordered_map<unsigned int, GeomHandle>& handleTable()
    {
        static std::unordered_map<unsigned int, GeomHandle> t;
        return t;
    }

    inline unsigned int& handleNext()
    {
        static unsigned int n = 0x08000000u;
        return n;
    }

    struct DeviceImpl
    {
        RTCErrorFunction errorFn = nullptr;
        void* errorUserPtr = nullptr;
    };

    struct SceneImpl
    {
        std::vector<GeomState> geoms;
        std::vector<Tri> tris;
        std::vector<BvhNode> nodes;
        bool dirty = true;
    };

    inline GeomHandle* resolveHandle(RTCGeometry g)
    {
        auto& tab = handleTable();
        auto it = tab.find((unsigned int)g);
        if (it == tab.end() || !it->second.alive) return nullptr;
        return &it->second;
    }

    // 线程局部：最近的场景，供只拿 geomId 的 enable/disable 使用
    inline SceneImpl*& tlsScene()
    {
        static thread_local SceneImpl* s = nullptr;
        return s;
    }

    inline void reportError(DeviceImpl* dev, RTCError code, const char* msg)
    {
        if (dev && dev->errorFn) dev->errorFn(dev->errorUserPtr, code, msg);
    }

    // ---- BVH ----
    inline void triBounds(const Tri& t, float* bmin, float* bmax)
    {
        for (int k = 0; k < 3; k++)
        {
            bmin[k] = std::min(t.v0[k], std::min(t.v1[k], t.v2[k]));
            bmax[k] = std::max(t.v0[k], std::max(t.v1[k], t.v2[k]));
        }
    }

    inline int buildNode(SceneImpl* s, int start, int count)
    {
        int idx = (int)s->nodes.size();
        s->nodes.push_back(BvhNode{});

        float bmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
        float bmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (int i = start; i < start + count; i++)
        {
            float tb0[3], tb1[3];
            triBounds(s->tris[i], tb0, tb1);
            for (int k = 0; k < 3; k++)
            {
                bmin[k] = std::min(bmin[k], tb0[k]);
                bmax[k] = std::max(bmax[k], tb1[k]);
            }
        }
        std::memcpy(s->nodes[idx].bmin, bmin, sizeof(bmin));
        std::memcpy(s->nodes[idx].bmax, bmax, sizeof(bmax));

        const int LEAF_SIZE = 8;
        if (count <= LEAF_SIZE)
        {
            s->nodes[idx].triStart = start;
            s->nodes[idx].triCount = count;
            return idx;
        }

        int axis = 0;
        float ext = bmax[0] - bmin[0];
        if (bmax[1] - bmin[1] > ext) { axis = 1; ext = bmax[1] - bmin[1]; }
        if (bmax[2] - bmin[2] > ext) { axis = 2; }

        int mid = start + count / 2;
        std::nth_element(
            s->tris.begin() + start,
            s->tris.begin() + mid,
            s->tris.begin() + start + count,
            [axis](const Tri& a, const Tri& b) {
                float ca = (a.v0[axis] + a.v1[axis] + a.v2[axis]) * (1.0f / 3.0f);
                float cb = (b.v0[axis] + b.v1[axis] + b.v2[axis]) * (1.0f / 3.0f);
                return ca < cb;
            });

        int l = buildNode(s, start, mid - start);
        int r = buildNode(s, mid, start + count - mid);
        s->nodes[idx].left = l;
        s->nodes[idx].right = r;
        s->nodes[idx].triCount = 0;
        return idx;
    }

    inline void rebuild(SceneImpl* s)
    {
        s->tris.clear();
        s->nodes.clear();

        for (size_t gi = 0; gi < s->geoms.size(); gi++)
        {
            auto& g = s->geoms[gi];
            if (!g.enabled || !g.committed) continue;
            size_t vcount = g.vertices.size() / 3;
            size_t tcount = g.indices.size() / 3;
            for (size_t t = 0; t < tcount; t++)
            {
                unsigned i0 = g.indices[t * 3 + 0];
                unsigned i1 = g.indices[t * 3 + 1];
                unsigned i2 = g.indices[t * 3 + 2];
                if (i0 >= vcount || i1 >= vcount || i2 >= vcount) continue;
                Tri tri;
                std::memcpy(tri.v0, &g.vertices[i0 * 3], sizeof(float) * 3);
                std::memcpy(tri.v1, &g.vertices[i1 * 3], sizeof(float) * 3);
                std::memcpy(tri.v2, &g.vertices[i2 * 3], sizeof(float) * 3);
                tri.geomID = (unsigned)gi;
                tri.primID = (unsigned)t;
                s->tris.push_back(tri);
            }
        }

        if (!s->tris.empty()) buildNode(s, 0, (int)s->tris.size());
        s->dirty = false;
    }

    inline bool rayTri(const RTCRay& ray, const Tri& t, float& tout, float& uout, float& vout)
    {
        float e1[3] = { t.v1[0] - t.v0[0], t.v1[1] - t.v0[1], t.v1[2] - t.v0[2] };
        float e2[3] = { t.v2[0] - t.v0[0], t.v2[1] - t.v0[1], t.v2[2] - t.v0[2] };
        float d[3] = { ray.dir_x, ray.dir_y, ray.dir_z };

        float p[3] = {
            d[1] * e2[2] - d[2] * e2[1],
            d[2] * e2[0] - d[0] * e2[2],
            d[0] * e2[1] - d[1] * e2[0]
        };
        float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
        if (std::fabs(det) < 1e-9f) return false;
        float invDet = 1.0f / det;

        float tv[3] = { ray.org_x - t.v0[0], ray.org_y - t.v0[1], ray.org_z - t.v0[2] };
        float uu = (tv[0] * p[0] + tv[1] * p[1] + tv[2] * p[2]) * invDet;
        if (uu < 0.0f || uu > 1.0f) return false;

        float q[3] = {
            tv[1] * e1[2] - tv[2] * e1[1],
            tv[2] * e1[0] - tv[0] * e1[2],
            tv[0] * e1[1] - tv[1] * e1[0]
        };
        float vv = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * invDet;
        if (vv < 0.0f || uu + vv > 1.0f) return false;

        float tt = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * invDet;
        if (tt < ray.tnear || tt > ray.tfar) return false;

        tout = tt; uout = uu; vout = vv;
        return true;
    }

    inline bool rayBox(const RTCRay& ray, const float* bmin, const float* bmax, float tmax)
    {
        float t0 = ray.tnear, t1 = tmax;
        const float o[3] = { ray.org_x, ray.org_y, ray.org_z };
        const float d[3] = { ray.dir_x, ray.dir_y, ray.dir_z };

        for (int a = 0; a < 3; a++)
        {
            if (std::fabs(d[a]) < 1e-20f)
            {
                if (o[a] < bmin[a] || o[a] > bmax[a]) return false;
                continue;
            }
            float inv = 1.0f / d[a];
            float tn = (bmin[a] - o[a]) * inv;
            float tf = (bmax[a] - o[a]) * inv;
            if (tn > tf) std::swap(tn, tf);
            if (tn > t0) t0 = tn;
            if (tf < t1) t1 = tf;
            if (t0 > t1) return false;
        }
        return true;
    }
} // namespace mockembree

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
using RTCDevice = mockembree::DeviceImpl*;

#define MOCK_DEV(x) (reinterpret_cast<mockembree::DeviceImpl*>(x))
#define MOCK_SCN(x) (reinterpret_cast<mockembree::SceneImpl*>(x))

inline RTCDevice rtcNewDevice(const char* /*config*/)
{
    return new mockembree::DeviceImpl();
}

inline void rtcReleaseDevice(RTCDevice device)
{
    delete MOCK_DEV(device);
}

inline void rtcSetDeviceErrorFunction(RTCDevice device, RTCErrorFunction fn, void* userPtr)
{
    if (!device) return;
    MOCK_DEV(device)->errorFn = fn;
    MOCK_DEV(device)->errorUserPtr = userPtr;
}

inline int rtcSetDeviceProperty(RTCDevice /*device*/, RTCDeviceProperty /*prop*/, int /*value*/)
{
    return RTC_ERROR_NONE;
}

inline RTCScene rtcNewScene(RTCDevice /*device*/)
{
    return reinterpret_cast<RTCScene>(new mockembree::SceneImpl());
}

inline void rtcReleaseScene(RTCScene scene)
{
    if (scene) delete MOCK_SCN(scene);
}

inline void rtcSetSceneBuildQuality(RTCScene /*scene*/, RTCBuildQuality /*q*/) {}
inline void rtcSetSceneFlags(RTCScene /*scene*/, RTCSceneFlags /*flags*/) {}

inline void rtcCommitScene(RTCScene scene)
{
    auto* s = MOCK_SCN(scene);
    if (!s) return;
    mockembree::tlsScene() = s;
    mockembree::rebuild(s);
}

inline RTCGeometry rtcNewGeometry(RTCDevice /*device*/, RTCGeometryType /*type*/)
{
    unsigned int id = mockembree::handleNext()++;
    auto& h = mockembree::handleTable()[id];
    h = mockembree::GeomHandle{};
    h.alive = true;
    return id;
}

inline void rtcReleaseGeometry(RTCGeometry geom)
{
    auto* h = mockembree::resolveHandle(geom);
    if (!h) return;
    mockembree::handleTable().erase((unsigned int)geom);
}

inline void* rtcSetNewGeometryBuffer(
    RTCGeometry geom,
    RTCBufferType type,
    unsigned int /*slot*/,
    RTCFormat format,
    size_t /*byteStride*/,
    size_t itemCount)
{
    // 两种情况：
    //   1) 刚 rtcNewGeometry 出来的句柄            -> 写 GeomHandle
    //   2) 复用被 disable 的几何（rtcGetGeometry） -> 写 SceneImpl::geoms[id]
    // VisibleScene 的 UpdateMesh 两种都会用到，所以这里必须都覆盖。
    if (auto* h = mockembree::resolveHandle(geom))
    {
        if (type == RTC_BUFFER_TYPE_VERTEX && format == RTC_FORMAT_FLOAT3)
        {
            h->vertices.assign(itemCount * 3, 0.0f);
            return h->vertices.data();
        }
        if (type == RTC_BUFFER_TYPE_INDEX && format == RTC_FORMAT_UINT3)
        {
            h->indices.assign(itemCount * 3, 0u);
            return h->indices.data();
        }
        return nullptr;
    }

    auto* s = mockembree::tlsScene();
    if (s && geom < s->geoms.size())
    {
        auto& st = s->geoms[geom];
        if (type == RTC_BUFFER_TYPE_VERTEX && format == RTC_FORMAT_FLOAT3)
        {
            st.vertices.assign(itemCount * 3, 0.0f);
            st.committed = true;
            s->dirty = true;
            return st.vertices.data();
        }
        if (type == RTC_BUFFER_TYPE_INDEX && format == RTC_FORMAT_UINT3)
        {
            st.indices.assign(itemCount * 3, 0u);
            st.committed = true;
            s->dirty = true;
            return st.indices.data();
        }
    }
    return nullptr;
}

inline void rtcSetGeometryUserData(RTCGeometry geom, void* ptr)
{
    if (auto* h = mockembree::resolveHandle(geom))
    {
        h->userData = ptr;
        return;
    }
    auto* s = mockembree::tlsScene();
    if (s && geom < s->geoms.size())
        s->geoms[geom].userData = ptr;
}

inline void rtcCommitGeometry(RTCGeometry /*geom*/) {}

inline unsigned int rtcAttachGeometry(RTCScene scene, RTCGeometry geom)
{
    auto* s = MOCK_SCN(scene);
    auto* h = mockembree::resolveHandle(geom);
    if (!s || !h) return RTC_INVALID_GEOMETRY_ID;

    mockembree::GeomState st;
    st.enabled = true;
    st.committed = true;
    st.userData = h->userData;
    st.vertices = h->vertices;
    st.indices = h->indices;

    unsigned int id = (unsigned int)s->geoms.size();
    s->geoms.push_back(std::move(st));
    s->dirty = true;

    mockembree::handleTable().erase((unsigned int)geom);
    return id;
}

// 关键点：这里必须返回「裸 geomID」，不能返回自造句柄。
// 因为 VisibleScene 复用被 disable 的几何时会走：
//     geom = rtcGetGeometry(scene, id);
//     rtcSetNewGeometryBuffer(geom, ...);   // 这一步要求 geom 还是新建时的语义
// 自造句柄会让 rtcSetNewGeometryBuffer 找不到对应的 GeomHandle。
// 场景归属改用 tlsScene 传递：rtcGetGeometry 一被调用就把 scene 记下来，
// 紧接着的 disable / enable / userData 都能拿到正确的场景。
inline RTCGeometry rtcGetGeometry(RTCScene scene, unsigned int geomID)
{
    mockembree::tlsScene() = MOCK_SCN(scene);
    return geomID;
}

inline void rtcDisableGeometry(RTCGeometry geom)
{
    auto* s = mockembree::tlsScene();
    if (!s) return;
    if (geom < s->geoms.size())
    {
        s->geoms[geom].enabled = false;
        s->dirty = true;
    }
}

inline void rtcEnableGeometry(RTCGeometry geom)
{
    auto* s = mockembree::tlsScene();
    if (!s) return;
    if (geom < s->geoms.size())
    {
        s->geoms[geom].enabled = true;
        s->dirty = true;
    }
}

inline void* rtcGetGeometryUserData(RTCGeometry geom)
{
    auto* s = mockembree::tlsScene();
    if (!s) return nullptr;
    if (geom < s->geoms.size()) return s->geoms[geom].userData;
    return nullptr;
}

inline void rtcInitRayQueryContext(RTCRayQueryContext* ctx)
{
    if (!ctx) return;
    ctx->instStackSize = 0;
    for (auto& x : ctx->instID) x = 0;
}

inline void rtcInitIntersectArguments(RTCIntersectArguments* args)
{
    if (!args) return;
    args->flags = RTC_SCENE_FLAG_NONE;
    args->feature_mask = nullptr;
    args->context = nullptr;
    args->filter = nullptr;
}

inline void rtcIntersect1(RTCScene scene, RTCRayHit* rayhit, RTCIntersectArguments* /*args*/)
{
    auto* s = MOCK_SCN(scene);
    if (!s || !rayhit) return;
    mockembree::tlsScene() = s;
    if (s->dirty) mockembree::rebuild(s);
    if (s->nodes.empty()) return;

    RTCRay& ray = rayhit->ray;
    float best = ray.tfar;
    unsigned int bestGeom = RTC_INVALID_GEOMETRY_ID;
    unsigned int bestPrim = 0;
    float bestU = 0.0f, bestV = 0.0f;

    int stack[256];
    int sp = 0;
    stack[sp++] = 0;

    while (sp > 0)
    {
        const auto& n = s->nodes[stack[--sp]];
        if (!mockembree::rayBox(ray, n.bmin, n.bmax, best)) continue;

        if (n.triCount > 0)
        {
            for (int i = n.triStart; i < n.triStart + n.triCount; i++)
            {
                const auto& t = s->tris[i];
                if (t.geomID < s->geoms.size() && !s->geoms[t.geomID].enabled) continue;
                float tt, uu, vv;
                if (mockembree::rayTri(ray, t, tt, uu, vv) && tt < best)
                {
                    best = tt; bestGeom = t.geomID; bestPrim = t.primID;
                    bestU = uu; bestV = vv;
                }
            }
        }
        else if (sp + 2 <= 256)
        {
            if (n.left >= 0) stack[sp++] = n.left;
            if (n.right >= 0) stack[sp++] = n.right;
        }
    }

    if (bestGeom != RTC_INVALID_GEOMETRY_ID)
    {
        ray.tfar = best;
        rayhit->hit.geomID = bestGeom;
        rayhit->hit.primID = bestPrim;
        rayhit->hit.u = bestU;
        rayhit->hit.v = bestV;
        rayhit->hit.Ng_x = rayhit->hit.Ng_y = rayhit->hit.Ng_z = 0.0f;
        rayhit->hit.instID[0] = 0;
        rayhit->hit.instPrimID[0] = 0;
    }
}
