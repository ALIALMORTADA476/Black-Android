#pragma once

// Native first-person hand IK for the Mauser 1891-12 viewmodel.
//
// The asset pack ships hand sockets / IK curves / finger-curl poses but no hand
// mesh, and the engine uses rigid per-part transforms rather than skeletal
// animation. This subsystem therefore generates a small procedural gloved hand
// mesh and positions it with analytic two-bone IK. Pure math lives here (inline,
// host-testable); D3D code lives only in weapon_hands_ik.cpp.

#include <cmath>
#include <cstdint>

#include "game/special_arsenal_core.h"

namespace game::hands_ik {

namespace sa = game::special_arsenal;

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    constexpr Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};

inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Normalize(Vec3 a) {
    const float l = Length(a);
    return l > 1e-6f ? a * (1.0f / l) : Vec3{};
}
inline Vec3 Lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Pack coordinates are (x=forward, y=up, z=right); the d3d viewmodel uses
// (x=right, y=up, z=forward). Same mapping as tools/build_m91_glb.py.
inline Vec3 PackToD3d(const float p[3]) { return {p[2], p[1], p[0]}; }

enum class HandSocket : uint8_t {
    RightGrip,
    LeftForegrip,
    BoltKnob,
    ClipThumb,
    TriggerFinger,
    ShoulderStock
};

// Pinned to 1.0. The pack's Kar98k measures 0.848 m butt-to-muzzle (a real one
// is 1.11 m) but hands.zip authors the real first-person hands at *human* scale
// against these same unscaled socket coordinates, so scaling the weapon alone
// would slide the grip away from the hands. The "too small / too far" look is
// solved at the camera by pulling the viewmodel closer, not by rescaling.
inline constexpr float kModelScale = 1.0f;

// Socket world positions from hand_ik_mounts.json, in pack coordinates (metres).
inline Vec3 SocketPosPack(HandSocket s) {
    switch (s) {
        case HandSocket::RightGrip:    return {0.245f, -0.018f, 0.008f};
        case HandSocket::LeftForegrip: return {0.555f, 0.012f, -0.006f};
        case HandSocket::BoltKnob:     return {0.295f, 0.022f, 0.048f};
        case HandSocket::ClipThumb:    return {0.335f, 0.068f, 0.0f};
        case HandSocket::TriggerFinger:return {0.319f, 0.0f, 0.002f};
        case HandSocket::ShoulderStock:return {0.015f, -0.025f, 0.0f};
    }
    return {};
}

inline Vec3 SocketPosScaled(HandSocket s) { return SocketPosPack(s) * kModelScale; }

inline Vec3 SocketPos(HandSocket s) {
    const Vec3 p = SocketPosScaled(s);
    const float v[3] = {p.x, p.y, p.z};
    return PackToD3d(v);
}

// Analytic two-bone IK. Returns the elbow position; `bend_dir` is a hint for
// which side the elbow bends toward. When the target is out of reach it is
// clamped to full extension and the function returns false.
inline bool SolveTwoBoneIK(Vec3 shoulder, Vec3 target, float upper, float lower,
                           Vec3 bend_dir, Vec3& elbow) {
    const Vec3 delta = target - shoulder;
    const float dist = Length(delta);
    if (dist < 1e-5f) {
        elbow = shoulder + Normalize(bend_dir) * upper;
        return false;
    }
    const Vec3 u = delta * (1.0f / dist);
    const float lo = std::fabs(upper - lower) + 1e-4f;
    const float hi = upper + lower - 1e-4f;
    const float d = Clamp(dist, lo, hi);
    const float a = (upper * upper - lower * lower + d * d) / (2.0f * d);
    const float h2 = upper * upper - a * a;
    const float h = std::sqrt(h2 > 0.0f ? h2 : 0.0f);
    Vec3 n = bend_dir - u * Dot(bend_dir, u);
    if (Length(n) < 1e-5f) {
        // Fallback hint orthogonal to u.
        n = Vec3{-u.y, u.x, 0.0f};
        if (Length(n) < 1e-5f) n = Vec3{0.0f, -u.z, u.y};
    }
    n = Normalize(n);
    elbow = shoulder + u * a + n * h;
    return dist <= hi && dist >= lo;
}

enum class GripTarget : uint8_t { Grip, BoltKnob, ClipThumb };

// Mirrors animation_ik_weight_curves from the pack.
inline GripTarget SelectGripTarget(sa::ActionPhase phase, bool reloading,
                                   float bolt_lift_deg, float clip_insert) {
    if (reloading && clip_insert > 0.0f) return GripTarget::ClipThumb;
    if (bolt_lift_deg > 0.5f) return GripTarget::BoltKnob;
    switch (phase) {
        case sa::ActionPhase::BoltLift:
        case sa::ActionPhase::BoltBack:
        case sa::ActionPhase::BoltPushCocking:
        case sa::ActionPhase::BoltLock:
            return GripTarget::BoltKnob;
        default:
            return GripTarget::Grip;
    }
}

// Exponential smoothing factor for target switching (no popping).
inline float SmoothTargetBlend(float dt, float speed = 12.0f) {
    return 1.0f - std::exp(-Clamp(dt, 0.0f, 0.1f) * speed);
}

// Renders both gloved hands into the M91 viewmodel. Implemented in .cpp.
void RenderWeaponHands(const float world[16], const float view[16], const float proj[16],
                       const float eye[3], const float right[3], const float up[3],
                       const float fwd[3], const sa::WeaponStatus& st, float dt);

}  // namespace game::hands_ik
