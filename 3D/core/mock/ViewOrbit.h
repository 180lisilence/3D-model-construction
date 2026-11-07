// ============================================================================
// mock/ViewOrbit.h
//
// 轨道视角：把相机绕场景中心（pivot）转起来。
//
// 为什么不用 WM_MOUSE*：叠层窗口带 WS_EX_TRANSPARENT（鼠标穿透），
// 窗口拿不到任何鼠标消息 —— 这是叠层的核心特性，不能为了旋转把它去掉。
// 所以这里全部走**全局轮询**：GetCursorPos + GetAsyncKeyState。
//   好处：不动窗口样式、不装钩子、不影响穿透、不影响下层程序。
//   代价：键盘是全局的，所以键盘默认**不接管**（要按 F9 显式打开），
//         否则你在这层上面打字，WASD 会顺手把相机转走。
//
// 手势：
//   中键 / 右键 按住拖动  = 转视角（抓起场景拖，向右拖场景向右走）
//   拖拽中 + Q/E          = 拉远 / 拉近（对数缩放，远近手感一致）
//   拖拽中 + R            = 复位（yaw / pitch / 距离全部归零）
//   F9                    = 键盘接管开关（WASD / 方向键 / QE / R 常驻生效）
//
// 为什么主推中键：叠层是穿透的，鼠标事件照旧送给下层窗口。
// 在桌面空白处右键拖拽，松手会弹出桌面右键菜单挡住画面；
// 中键在桌面 / 资源管理器里都没有默认行为，拖起来干净。
// 右键仍然保留（用惯了右键拖的人不用改）。
//
// 数学：
//   eye 相对 pivot 的向量换成球坐标 (r, az, el)，
//   az += yaw，el += pitch，r *= distMul，再转回笛卡尔。
//   pivot 取当前机位自己的 target —— 不硬换成房间中心，
//   否则第一次拖动时半径会突变，画面会"跳"一下。
// ============================================================================
#pragma once

#include <windows.h>

#include <cmath>

namespace vieworbit
{
    constexpr float kPi      = 3.14159265358979f;
    constexpr float kDeg2Rad = kPi / 180.0f;
    constexpr float kRad2Deg = 180.0f / kPi;

    inline float Clampf(float v, float lo, float hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // 键按下 -> 上升沿只触发一次
    inline bool Edge(bool now, bool& prev)
    {
        const bool e = now && !prev;
        prev = now;
        return e;
    }

    struct Config
    {
        float sensYaw   = 0.26f;    // 度 / 像素
        float sensPitch = 0.22f;    // 度 / 像素
        float keyYawRate   = 78.0f; // 度 / 秒
        float keyPitchRate = 62.0f; // 度 / 秒
        float keyZoomRate  = 0.85f; // 距离倍率 / 秒（指数）
        float minDistMul = 0.24f;
        float maxDistMul = 2.60f;
        float maxElev    = 86.0f;   // 仰角上限（度），别到 90 否则 up 向量退化
        float floorZ     = 18.0f;   // 眼高下限（cm），低于这个就是钻到地底下了
    };

    struct State
    {
        float yaw     = 0.0f;       // 累计方位偏移（度）
        float pitch   = 0.0f;       // 累计仰角偏移（度）
        float distMul = 1.0f;       // 距离倍率

        bool  dragging = false;     // 右键正在拖
        bool  keyMode  = false;     // F9 键盘接管
        bool  engaged  = false;     // 用户动过 -> 停掉自动环绕

        POINT lastDrag{};
        bool  rEdge  = false;       // 边沿检测缓存
        bool  f9Edge = false;
        float idle   = 0.0f;        // 距上次输入多少秒（HUD 提示淡出用）
    };

    // 每帧调用一次。dt 单位秒。返回本帧是否产生了输入。
    inline bool Update(State& st, const Config& cfg, float dt)
    {
        if (!(dt > 0.0f)) dt = 0.0f;
        if (dt > 0.100f)  dt = 0.100f;   // 卡顿一帧不要甩半圈

        bool got = false;

        const bool rbtn = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        const bool mbtn = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
        const bool drag = rbtn || mbtn;         // 中键优先：桌面上中键没有副作用
        const bool f9   = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;

        if (Edge(f9, st.f9Edge)) st.keyMode = !st.keyMode;

        // ---- 右键 / 中键拖拽 ----
        POINT p{};
        GetCursorPos(&p);

        if (drag)
        {
            if (!st.dragging)
            {
                st.dragging = true;
                st.lastDrag = p;
            }
            else
            {
                const int dx = p.x - st.lastDrag.x;
                const int dy = p.y - st.lastDrag.y;
                if (dx != 0 || dy != 0)
                {
                    // 「抓起场景拖动」：向右拖 -> 场景向右 -> 相机反向绕（az 增大）
                    //                     向下拖 -> 场景向下 -> 相机抬高（el 增大）
                    st.yaw   += (float)dx * cfg.sensYaw;
                    st.pitch += (float)dy * cfg.sensPitch;
                    st.lastDrag = p;
                    got = true;
                }
            }
        }
        else
        {
            st.dragging = false;
        }

        // ---- 键盘：只在拖拽按键按住、或 F9 接管时生效 ----
        const bool keysOn = drag || st.keyMode;
        if (keysOn)
        {
            auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };

            float ky = 0.0f, kp = 0.0f, kz = 0.0f;
            if (held(VK_LEFT)  || held('A')) ky += 1.0f;
            if (held(VK_RIGHT) || held('D')) ky -= 1.0f;
            if (held(VK_UP)    || held('W')) kp += 1.0f;
            if (held(VK_DOWN)  || held('S')) kp -= 1.0f;
            if (held('Q') || held(VK_SUBTRACT)) kz -= 1.0f;
            if (held('E') || held(VK_ADD))      kz += 1.0f;

            if (ky != 0.0f) { st.yaw   += ky * cfg.keyYawRate   * dt; got = true; }
            if (kp != 0.0f) { st.pitch += kp * cfg.keyPitchRate * dt; got = true; }
            if (kz != 0.0f)
            {
                st.distMul *= std::exp(-kz * cfg.keyZoomRate * dt);
                st.distMul = Clampf(st.distMul, cfg.minDistMul, cfg.maxDistMul);
                got = true;
            }

            // R 复位
            const bool rk = held('R');
            if (Edge(rk, st.rEdge))
            {
                st.yaw = 0.0f;
                st.pitch = 0.0f;
                st.distMul = 1.0f;
                got = true;
            }
        }
        else
        {
            // 松开后要清掉边沿缓存，否则下次按下 R 会被当成"一直按着"
            st.rEdge = (GetAsyncKeyState('R') & 0x8000) != 0;
        }

        if (got)
        {
            st.engaged = true;
            st.idle = 0.0f;
        }
        else
        {
            st.idle += dt;
        }

        return got;
    }

    // 在现有 eye / target 上叠加轨道旋转。就地改写 eye。
    // pivot 就是 target 本身；返回实际用到的仰角（度），HUD 显示用。
    // st 传引用：仰角被夹住时把累计量一起回写，避免"拖到极限后反向要拖很久才动"的死区。
    inline float Apply(State& st, const Config& cfg,
                       float eye[3], const float pivot[3])
    {
        float vx = eye[0] - pivot[0];
        float vy = eye[1] - pivot[1];
        float vz = eye[2] - pivot[2];

        const float len = std::sqrt(vx * vx + vy * vy + vz * vz);
        if (len < 1.0e-3f) return 0.0f;

        const float elBase = std::asin(Clampf(vz / len, -1.0f, 1.0f));

        if (st.yaw == 0.0f && st.pitch == 0.0f && st.distMul == 1.0f)
            return elBase * kRad2Deg;

        float az = std::atan2(vy, vx);

        const float r = len * st.distMul;

        // 仰角下限按「眼高不许穿到 floorZ 以下」反推：
        // 半径越小，允许往下探的角度越大；半径 14 米时下限自然收到 -5° 左右。
        // 这是数值保护，不是审美裁剪 —— 钻到地面以下只会看到地底背面。
        const float sinMin = (cfg.floorZ - pivot[2]) / r;
        float elLo = std::asin(Clampf(sinMin, -0.995f, 0.995f));
        const float elHi = cfg.maxElev * kDeg2Rad;
        if (elLo > elHi) elLo = elHi;

        az += st.yaw * kDeg2Rad;

        const float elRaw = elBase + st.pitch * kDeg2Rad;
        const float el = Clampf(elRaw, elLo, elHi);
        if (el != elRaw)
            st.pitch = (el - elBase) * kRad2Deg;   // 回写，消掉死区

        const float ce = std::cos(el);
        eye[0] = pivot[0] + r * ce * std::cos(az);
        eye[1] = pivot[1] + r * ce * std::sin(az);
        eye[2] = pivot[2] + r * std::sin(el);

        return el * kRad2Deg;
    }
} // namespace vieworbit
