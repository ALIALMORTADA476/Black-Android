#include "game/weapon_hands_ik.h"

#include <array>
#include <cmath>
#include <cstdint>

#include "d3d/world_overlay.h"

namespace game::hands_ik {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg = kPi / 180.0f;

constexpr uint32_t PackCol(float r, float g, float b) {
    return (0xFFu << 24) |
           (uint32_t(int(r * 255.0f + 0.5f)) << 16) |
           (uint32_t(int(g * 255.0f + 0.5f)) << 8) |
           uint32_t(int(b * 255.0f + 0.5f));
}

constexpr uint32_t kLeather = PackCol(0.20f, 0.215f, 0.165f);
constexpr uint32_t kCrease = PackCol(0.13f, 0.14f, 0.105f);
constexpr uint32_t kKnuckle = PackCol(0.26f, 0.27f, 0.21f);
constexpr uint32_t kSleeve = PackCol(0.11f, 0.12f, 0.10f);

struct Mesh {
    static constexpr uint32_t kMaxVerts = 1024;
    static constexpr uint32_t kMaxIdx = 4096;
    std::array<d3d::ViewmodelVertex, kMaxVerts> verts{};
    std::array<uint16_t, kMaxIdx> indices{};
    uint32_t nv = 0;
    uint32_t ni = 0;

    uint16_t Add(Vec3 p, Vec3 n, uint32_t col) {
        if (nv >= kMaxVerts) return 0;
        d3d::ViewmodelVertex v{};
        v.x = p.x; v.y = p.y; v.z = p.z;
        v.nx = n.x; v.ny = n.y; v.nz = n.z;
        v.bgra = col;
        verts[nv] = v;
        return uint16_t(nv++);
    }

    void Tri(uint16_t a, uint16_t b, uint16_t c) {
        if (ni + 3 > kMaxIdx) return;
        indices[ni++] = a;
        indices[ni++] = b;
        indices[ni++] = c;
    }
};

Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 RotateAxis(Vec3 v, Vec3 axis, float angle) {
    // Rodrigues rotation.
    const float c = std::cos(angle), s = std::sin(angle);
    return v * c + Cross(axis, v) * s + axis * Dot(axis, v) * (1.0f - c);
}

void AddQuad(Mesh& m, Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, Vec3 n, uint32_t col) {
    const uint16_t a = m.Add(p0, n, col);
    const uint16_t b = m.Add(p1, n, col);
    const uint16_t c = m.Add(p2, n, col);
    const uint16_t d = m.Add(p3, n, col);
    m.Tri(a, b, c);
    m.Tri(a, c, d);
}

void AddBox(Mesh& m, Vec3 center, Vec3 u, Vec3 v, Vec3 w,
            float hu, float hv, float hw, uint32_t col) {
    const Vec3 pu = u * hu, pv = v * hv, pw = w * hw;
    AddQuad(m, center + pu - pv + pw, center + pu + pv + pw,
            center + pu + pv - pw, center + pu - pv - pw, u, col);
    AddQuad(m, center - pu + pv + pw, center - pu - pv + pw,
            center - pu - pv - pw, center - pu + pv - pw, u * -1.0f, col);
    AddQuad(m, center + pv - pu + pw, center + pv + pu + pw,
            center + pv + pu - pw, center + pv - pu - pw, v, col);
    AddQuad(m, center - pv + pu + pw, center - pv - pu + pw,
            center - pv - pu - pw, center - pv + pu - pw, v * -1.0f, col);
    AddQuad(m, center + pw - pu + pv, center + pw + pu + pv,
            center + pw + pu - pv, center + pw - pu - pv, w, col);
    AddQuad(m, center - pw + pu + pv, center - pw - pu + pv,
            center - pw - pu - pv, center - pw + pu - pv, w * -1.0f, col);
}

void AddSegment(Mesh& m, Vec3 a, Vec3 b, float r0, float r1, uint32_t col) {
    constexpr int kSides = 6;
    const Vec3 dir = Normalize(b - a);
    Vec3 t1 = Cross(dir, Vec3{0.0f, 1.0f, 0.0f});
    if (Length(t1) < 1e-4f) t1 = Cross(dir, Vec3{1.0f, 0.0f, 0.0f});
    t1 = Normalize(t1);
    const Vec3 t2 = Normalize(Cross(dir, t1));

    uint16_t ringA[kSides], ringB[kSides];
    for (int i = 0; i < kSides; ++i) {
        const float ang = kPi * 2.0f * float(i) / float(kSides);
        const Vec3 off = t1 * std::cos(ang) + t2 * std::sin(ang);
        ringA[i] = m.Add(a + off * r0, off, col);
        ringB[i] = m.Add(b + off * r1, off, col);
    }
    for (int i = 0; i < kSides; ++i) {
        const int j = (i + 1) % kSides;
        m.Tri(ringA[i], ringA[j], ringB[j]);
        m.Tri(ringA[i], ringB[j], ringB[i]);
    }
    const uint16_t ca = m.Add(a, dir * -1.0f, col);
    const uint16_t cb = m.Add(b, dir, col);
    for (int i = 0; i < kSides; ++i) {
        const int j = (i + 1) % kSides;
        m.Tri(ca, ringA[j], ringA[i]);
        m.Tri(cb, ringB[i], ringB[j]);
    }
}

Vec3 TransformPoint(const float world[16], Vec3 p) {
    return {
        world[0] * p.x + world[4] * p.y + world[8] * p.z + world[12],
        world[1] * p.x + world[5] * p.y + world[9] * p.z + world[13],
        world[2] * p.x + world[6] * p.y + world[10] * p.z + world[14]
    };
}

Vec3 TransformDir(const float world[16], Vec3 p) {
    return {
        world[0] * p.x + world[4] * p.y + world[8] * p.z,
        world[1] * p.x + world[5] * p.y + world[9] * p.z,
        world[2] * p.x + world[6] * p.y + world[10] * p.z
    };
}

// Socket local position after the same rigid part transform that
// ApplyWeaponPartKinematics applies to the visible part.
Vec3 AnimatedSocket(HandSocket s, const sa::PartAnimPose& p) {
    Vec3 v = SocketPos(s);
    if (s == HandSocket::BoltKnob) {
        // Pivot lives in the baked (scaled) geometry space, so it scales with the
        // mesh; the bolt travel itself is the same constant the part kinematics
        // applies to the visible bolt, so it must stay unscaled.
        const Vec3 pivot{0.0f, 0.0395f * kModelScale, 0.295f * kModelScale};
        Vec3 l = v - pivot;
        const float a = p.bolt_lift_deg * kDeg;
        const float c = std::cos(a), sn = std::sin(a);
        const Vec3 r{l.x * c - l.y * sn, l.x * sn + l.y * c, l.z};
        return r + pivot + Vec3{0.0f, 0.0f, -p.bolt_retract_m};
    }
    if (s == HandSocket::ClipThumb) {
        if (p.clip_eject > 0.01f) {
            return v + Vec3{p.clip_eject * 0.075f, p.clip_eject * 0.085f, 0.0f};
        }
        return v + Vec3{0.0f, (1.0f - p.clip_insert) * 0.055f, 0.0f};
    }
    return v;
}

void BuildHand(Mesh& m, Vec3 wrist, Vec3 palm, Vec3 elbow, Vec3 gun_fwd, Vec3 up,
               const float curl[5][3], bool right) {
    const Vec3 fwd = Normalize(palm - wrist);
    Vec3 side = Normalize(Cross(fwd, up));
    if (Length(side) < 1e-4f) side = Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 palm_up = Normalize(Cross(side, fwd));

    const Vec3 palm_center = Lerp(wrist, palm, 0.55f);
    AddBox(m, palm_center, fwd, side, palm_up, 0.030f, 0.036f, 0.013f, kLeather);
    AddBox(m, palm_center + palm_up * 0.011f, fwd, side, palm_up,
           0.026f, 0.031f, 0.003f, kKnuckle);

    const float spread[4] = {-0.026f, -0.009f, 0.009f, 0.026f};
    const float lens[3] = {0.030f, 0.024f, 0.018f};
    const float rads[3] = {0.0095f, 0.0080f, 0.0065f};
    for (int f = 0; f < 4; ++f) {
        const int fi = (f == 0) ? 0 : f;  // index finger uses trigger curl
        Vec3 p = palm_center + fwd * 0.028f + side * spread[f] - palm_up * 0.004f;
        Vec3 d = fwd;
        for (int j = 0; j < 3; ++j) {
            d = Normalize(RotateAxis(d, side, -curl[fi][j] * kDeg));
            const Vec3 q = p + d * lens[j];
            AddSegment(m, p, q, rads[j], rads[j] * 0.88f,
                       (j == 0) ? kKnuckle : kLeather);
            p = q;
        }
    }

    const float tside = right ? -1.0f : 1.0f;
    Vec3 tp = palm_center + side * (0.034f * tside) + fwd * 0.006f;
    Vec3 td = Normalize(fwd * 0.55f + palm_up * 0.85f);
    for (int j = 0; j < 2; ++j) {
        td = Normalize(RotateAxis(td, fwd, curl[4][j] * kDeg * 0.5f * tside));
        const Vec3 q = tp + td * (j == 0 ? 0.028f : 0.022f);
        AddSegment(m, tp, q, 0.010f, 0.008f, kLeather);
        tp = q;
    }

    AddSegment(m, elbow, wrist, 0.043f, 0.031f, kSleeve);
    AddSegment(m, Lerp(elbow, wrist, 0.12f), Lerp(elbow, wrist, 0.32f),
               0.044f, 0.042f, kCrease);
}

}  // namespace

void RenderWeaponHands(const float world[16], const float view[16], const float proj[16],
                       const float eye[3], const float right[3], const float up[3],
                       const float fwd[3], const sa::WeaponStatus& st, float dt) {
    if (st.id != sa::SpecialWeaponId::Mauser1891) return;
    if (!world || !view || !proj || !eye || !right || !up || !fwd) return;

    const Vec3 E{eye[0], eye[1], eye[2]};
    const Vec3 R{right[0], right[1], right[2]};
    const Vec3 U{up[0], up[1], up[2]};
    const Vec3 F{fwd[0], fwd[1], fwd[2]};

    const Vec3 shoulder_r = E + R * 0.17f + U * -0.19f + F * -0.04f;
    const Vec3 shoulder_l = E + R * -0.17f + U * -0.19f + F * -0.04f;

    const GripTarget gt = SelectGripTarget(st.phase, st.reloading,
                                           st.pose.bolt_lift_deg, st.pose.clip_insert);
    const HandSocket rs = (gt == GripTarget::Grip) ? HandSocket::RightGrip
                        : (gt == GripTarget::BoltKnob) ? HandSocket::BoltKnob
                                                       : HandSocket::ClipThumb;

    const Vec3 gun_fwd = Normalize(TransformDir(world, Vec3{0.0f, 0.0f, 1.0f}));
    const Vec3 r_palm = TransformPoint(world, AnimatedSocket(rs, st.pose));
    const Vec3 l_palm = TransformPoint(world, AnimatedSocket(HandSocket::LeftForegrip, st.pose));
    const Vec3 r_target = r_palm - gun_fwd * 0.034f + U * -0.012f;
    const Vec3 l_target = l_palm - gun_fwd * 0.030f + U * -0.018f;

    static Vec3 s_r{}, s_l{};
    static bool s_init = false;
    const float blend = s_init ? SmoothTargetBlend(dt, 9.0f) : 1.0f;
    s_r = s_init ? Lerp(s_r, r_target, blend) : r_target;
    s_l = s_init ? Lerp(s_l, l_target, blend) : l_target;
    s_init = true;

    Vec3 elbow_r{}, elbow_l{};
    SolveTwoBoneIK(shoulder_r, s_r, 0.28f, 0.26f,
                   R * 0.55f + F * 0.25f + U * -0.20f, elbow_r);
    SolveTwoBoneIK(shoulder_l, s_l, 0.28f, 0.26f,
                   R * -0.55f + F * 0.25f + U * -0.20f, elbow_l);

    const float trig = Clamp(st.pose.trigger_pull, 0.0f, 1.0f);
    float curl_r[5][3] = {
        {18.0f + trig * 26.0f, 42.0f + trig * 18.0f, 35.0f + trig * 10.0f},
        {58, 68, 52}, {62, 72, 55}, {65, 74, 58}, {22, 34, 28}
    };
    float curl_l[5][3] = {
        {48, 58, 42}, {52, 62, 46}, {55, 64, 48}, {58, 66, 50}, {35, 42, 25}
    };
    if (gt == GripTarget::BoltKnob || gt == GripTarget::ClipThumb) {
        curl_r[0][0] = 42; curl_r[0][1] = 52; curl_r[0][2] = 40;
        curl_r[4][0] = 38; curl_r[4][1] = 45; curl_r[4][2] = 35;
    }

    static Mesh s_mesh;
    s_mesh.nv = 0;
    s_mesh.ni = 0;
    BuildHand(s_mesh, s_r, r_palm, elbow_r, gun_fwd, U, curl_r, true);
    BuildHand(s_mesh, s_l, l_palm, elbow_l, gun_fwd, U, curl_l, false);

    const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    d3d::DrawViewmodelMesh(s_mesh.verts.data(), s_mesh.nv,
                           s_mesh.indices.data(), s_mesh.ni,
                           ident, view, proj, eye, 0.0f);
}

}  // namespace game::hands_ik
