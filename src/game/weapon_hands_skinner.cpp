#include "game/weapon_hands_skinner.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace game::hands_skinner {
namespace {

inline uint32_t ReadU32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline int32_t ReadI32(const uint8_t* p) {
    int32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline uint16_t ReadU16(const uint8_t* p) {
    uint16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline float ReadF32(const uint8_t* p) {
    float v = 0.0f;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline void NormalizeQuat(float q[4]) {
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 1e-8f) {
        const float inv = 1.0f / n;
        q[0] *= inv;
        q[1] *= inv;
        q[2] *= inv;
        q[3] *= inv;
    } else {
        q[0] = 0.0f;
        q[1] = 0.0f;
        q[2] = 0.0f;
        q[3] = 1.0f;
    }
}

inline void NlerpQuat(const float a[4], const float b[4], float alpha, float out[4]) {
    const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    const float sign = (dot < 0.0f) ? -1.0f : 1.0f;
    const float ia = 1.0f - alpha;
    const float ib = alpha * sign;
    out[0] = a[0] * ia + b[0] * ib;
    out[1] = a[1] * ia + b[1] * ib;
    out[2] = a[2] * ia + b[2] * ib;
    out[3] = a[3] * ia + b[3] * ib;
    NormalizeQuat(out);
}

inline void QuatMul(const float a[4], const float b[4], float out[4]) {
    const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
    NormalizeQuat(out);
}

void BuildTrsRowMajor(const float t[3], const float q[4], const float s[3], float m[16]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float xw = x * w, yw = y * w, zw = z * w;

    m[0] = (1.0f - 2.0f * (yy + zz)) * s[0];
    m[1] = (2.0f * (xy - zw)) * s[1];
    m[2] = (2.0f * (xz + yw)) * s[2];
    m[3] = t[0];

    m[4] = (2.0f * (xy + zw)) * s[0];
    m[5] = (1.0f - 2.0f * (xx + zz)) * s[1];
    m[6] = (2.0f * (yz - xw)) * s[2];
    m[7] = t[1];

    m[8] = (2.0f * (xz - yw)) * s[0];
    m[9] = (2.0f * (yz + xw)) * s[1];
    m[10] = (1.0f - 2.0f * (xx + yy)) * s[2];
    m[11] = t[2];

    m[12] = 0.0f;
    m[13] = 0.0f;
    m[14] = 0.0f;
    m[15] = 1.0f;
}

void MulMat4RowMajor(const float a[16], const float b[16], float out[16]) {
    for (int r = 0; r < 4; ++r) {
        const float a0 = a[r * 4 + 0], a1 = a[r * 4 + 1], a2 = a[r * 4 + 2], a3 = a[r * 4 + 3];
        out[r * 4 + 0] = a0 * b[0] + a1 * b[4] + a2 * b[8] + a3 * b[12];
        out[r * 4 + 1] = a0 * b[1] + a1 * b[5] + a2 * b[9] + a3 * b[13];
        out[r * 4 + 2] = a0 * b[2] + a1 * b[6] + a2 * b[10] + a3 * b[14];
        out[r * 4 + 3] = a0 * b[3] + a1 * b[7] + a2 * b[11] + a3 * b[15];
    }
}

}  // namespace

bool SkinnedHandsAsset::Parse(const uint8_t* data, size_t size) {
    loaded_ = false;
    verts_.clear();
    indices_.clear();
    joints_.clear();
    clips_.clear();

    if (!data || size < 32u) return false;
    if (ReadU32(data) != kHsknMagic) return false;
    const uint32_t ver = ReadU32(data + 4);
    const uint32_t vc = ReadU32(data + 8);
    const uint32_t ic = ReadU32(data + 12);
    const uint32_t jc = ReadU32(data + 16);
    const uint32_t cc = ReadU32(data + 20);
    if (ver != 1u || vc == 0u || vc >= 65535u || ic < 3u || (ic % 3u) != 0u ||
        jc == 0u || jc > kMaxHsknJoints || cc == 0u || cc > 16u) {
        return false;
    }

    constexpr size_t kVertBytes = 56u;  // 8*4 + 4 + 4 + 4*4
    constexpr size_t kJointBytes = 108u;  // 4 + 10*4 + 16*4
    size_t cur = 32u;
    const size_t verts_bytes = size_t(vc) * kVertBytes;
    const size_t idx_bytes = size_t(ic) * 2u;
    const size_t joints_bytes = size_t(jc) * kJointBytes;
    if (cur + verts_bytes + idx_bytes + joints_bytes > size) return false;

    verts_.resize(vc);
    for (uint32_t i = 0; i < vc; ++i) {
        const uint8_t* vp = data + cur + size_t(i) * kVertBytes;
        HsknVertex& v = verts_[i];
        v.px = ReadF32(vp + 0);
        v.py = ReadF32(vp + 4);
        v.pz = ReadF32(vp + 8);
        v.nx = ReadF32(vp + 12);
        v.ny = ReadF32(vp + 16);
        v.nz = ReadF32(vp + 20);
        v.u = ReadF32(vp + 24);
        v.v = ReadF32(vp + 28);
        v.bgra = ReadU32(vp + 32);
        for (int s = 0; s < 4; ++s) {
            v.bones[s] = std::min<uint8_t>(vp[36 + s], uint8_t(jc - 1u));
            v.weights[s] = ReadF32(vp + 40 + s * 4);
        }
    }
    cur += verts_bytes;

    indices_.resize(ic);
    for (uint32_t i = 0; i < ic; ++i) {
        const uint16_t idx = ReadU16(data + cur + size_t(i) * 2u);
        if (idx >= vc) return false;
        indices_[i] = idx;
    }
    cur += idx_bytes;

    joints_.resize(jc);
    for (uint32_t j = 0; j < jc; ++j) {
        const uint8_t* jp = data + cur + size_t(j) * kJointBytes;
        HsknJoint& jnt = joints_[j];
        jnt.parent = ReadI32(jp + 0);
        if (jnt.parent >= int32_t(j)) return false;
        for (int k = 0; k < 3; ++k) jnt.rest_t[k] = ReadF32(jp + 4 + k * 4);
        for (int k = 0; k < 4; ++k) jnt.rest_q[k] = ReadF32(jp + 16 + k * 4);
        for (int k = 0; k < 3; ++k) jnt.rest_s[k] = ReadF32(jp + 32 + k * 4);
        for (int k = 0; k < 16; ++k) jnt.inv_bind[k] = ReadF32(jp + 44 + k * 4);
        NormalizeQuat(jnt.rest_q);
    }
    cur += joints_bytes;

    clips_.resize(cc);
    for (uint32_t c = 0; c < cc; ++c) {
        if (cur + 40u > size) return false;
        HsknClip& cl = clips_[c];
        std::memcpy(cl.name, data + cur, 32u);
        cl.name[31] = '\0';
        cl.duration_s = ReadF32(data + cur + 32u);
        const uint32_t ch_cnt = ReadU32(data + cur + 36u);
        cur += 40u;
        if (ch_cnt > 512u) return false;
        cl.channels.resize(ch_cnt);
        for (uint32_t k = 0; k < ch_cnt; ++k) {
            if (cur + 12u > size) return false;
            HsknChannel& ch = cl.channels[k];
            ch.joint_idx = ReadU32(data + cur + 0u);
            ch.path_kind = ReadU32(data + cur + 4u);
            const uint32_t key_cnt = ReadU32(data + cur + 8u);
            cur += 12u;
            if (ch.joint_idx >= jc || ch.path_kind > 2u || key_cnt == 0u || key_cnt > 4096u) {
                return false;
            }
            const size_t stride_f = (ch.path_kind == 1u) ? 4u : 3u;
            const size_t t_bytes = size_t(key_cnt) * 4u;
            const size_t v_bytes = size_t(key_cnt) * stride_f * 4u;
            if (cur + t_bytes + v_bytes > size) return false;
            ch.times.resize(key_cnt);
            ch.values.resize(size_t(key_cnt) * stride_f);
            for (uint32_t ti = 0; ti < key_cnt; ++ti) {
                ch.times[ti] = ReadF32(data + cur + size_t(ti) * 4u);
            }
            cur += t_bytes;
            for (size_t vi = 0; vi < ch.values.size(); ++vi) {
                ch.values[vi] = ReadF32(data + cur + vi * 4u);
            }
            cur += v_bytes;
        }
    }

    loaded_ = true;
    return true;
}

bool SkinnedHandsAsset::SamplePoseLocals(uint32_t clip_idx, float time_s, float trigger_pull,
                                         float out_t[kMaxHsknJoints][3],
                                         float out_q[kMaxHsknJoints][4],
                                         float out_s[kMaxHsknJoints][3]) const {
    if (!loaded_ || clip_idx >= clips_.size()) return false;
    const uint32_t jc = uint32_t(joints_.size());
    for (uint32_t j = 0; j < jc; ++j) {
        for (int k = 0; k < 3; ++k) out_t[j][k] = joints_[j].rest_t[k];
        for (int k = 0; k < 4; ++k) out_q[j][k] = joints_[j].rest_q[k];
        for (int k = 0; k < 3; ++k) out_s[j][k] = joints_[j].rest_s[k];
    }

    const HsknClip& cl = clips_[clip_idx];
    const float tq = std::max(0.0f, time_s);
    for (const HsknChannel& ch : cl.channels) {
        const size_t kn = ch.times.size();
        size_t i0 = 0;
        size_t i1 = 0;
        float alpha = 0.0f;
        if (kn > 1u) {
            if (tq <= ch.times.front()) {
                i0 = i1 = 0;
            } else if (tq >= ch.times.back()) {
                i0 = i1 = kn - 1u;
            } else {
                auto it = std::upper_bound(ch.times.begin(), ch.times.end(), tq);
                i1 = size_t(std::distance(ch.times.begin(), it));
                i0 = (i1 > 0u) ? (i1 - 1u) : 0u;
                const float span = ch.times[i1] - ch.times[i0];
                alpha = (span > 1e-6f) ? ((tq - ch.times[i0]) / span) : 0.0f;
            }
        }
        const uint32_t j = ch.joint_idx;
        if (ch.path_kind == 0u) {
            const float* v0 = &ch.values[i0 * 3u];
            const float* v1 = &ch.values[i1 * 3u];
            for (int k = 0; k < 3; ++k) out_t[j][k] = v0[k] + (v1[k] - v0[k]) * alpha;
        } else if (ch.path_kind == 1u) {
            const float* v0 = &ch.values[i0 * 4u];
            const float* v1 = &ch.values[i1 * 4u];
            NlerpQuat(v0, v1, alpha, out_q[j]);
        } else {
            const float* v0 = &ch.values[i0 * 3u];
            const float* v1 = &ch.values[i1 * 3u];
            for (int k = 0; k < 3; ++k) out_s[j][k] = v0[k] + (v1[k] - v0[k]) * alpha;
        }
    }

    // Add tactile right index-finger trigger curl during firing/idle on clip 0
    if (clip_idx == 0u && trigger_pull > 0.01f && jc > 26u) {
        const float half_ang = -0.14f * std::clamp(trigger_pull, 0.0f, 1.0f);
        const float dq[4] = {0.0f, 0.0f, std::sin(half_ang), std::cos(half_ang)};
        float tmp_q[4]{};
        QuatMul(out_q[25], dq, tmp_q);
        std::memcpy(out_q[25], tmp_q, sizeof(tmp_q));
        QuatMul(out_q[26], dq, tmp_q);
        std::memcpy(out_q[26], tmp_q, sizeof(tmp_q));
    }
    return true;
}

void SkinnedHandsAsset::BuildSkinFromLocals(const float t[kMaxHsknJoints][3],
                                            const float q[kMaxHsknJoints][4],
                                            const float s[kMaxHsknJoints][3],
                                            float out_skin_mats[kMaxHsknJoints][16],
                                            float out_joint_pos[kMaxHsknJoints][3]) const {
    const uint32_t jc = uint32_t(joints_.size());
    std::array<std::array<float, 16>, kMaxHsknJoints> world_mats{};
    for (uint32_t j = 0; j < jc; ++j) {
        float loc[16]{};
        BuildTrsRowMajor(t[j], q[j], s[j], loc);
        if (joints_[j].parent >= 0) {
            MulMat4RowMajor(world_mats[size_t(joints_[j].parent)].data(), loc, world_mats[j].data());
        } else {
            std::memcpy(world_mats[j].data(), loc, sizeof(loc));
        }
        if (out_joint_pos) {
            out_joint_pos[j][0] = world_mats[j][3];
            out_joint_pos[j][1] = world_mats[j][7];
            out_joint_pos[j][2] = world_mats[j][11];
        }
        MulMat4RowMajor(world_mats[j].data(), joints_[j].inv_bind, out_skin_mats[j]);
    }
}

bool SkinnedHandsAsset::EvaluatePose(uint32_t clip_idx, float time_s, float trigger_pull,
                                     float out_skin_mats[kMaxHsknJoints][16],
                                     float out_joint_pos[kMaxHsknJoints][3]) const {
    float t[kMaxHsknJoints][3]{}, q[kMaxHsknJoints][4]{}, s[kMaxHsknJoints][3]{};
    if (!SamplePoseLocals(clip_idx, time_s, trigger_pull, t, q, s)) return false;
    BuildSkinFromLocals(t, q, s, out_skin_mats, out_joint_pos);
    return true;
}

void SkinnedHandsAsset::ApplySkinMatrices(const float skin_mats[kMaxHsknJoints][16],
                                          std::vector<d3d::ViewmodelTexturedVertex>& out_verts) const {
    const size_t n = verts_.size();
    out_verts.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const HsknVertex& v = verts_[i];
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        float nx = 0.0f, ny = 0.0f, nz = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const float w = v.weights[k];
            if (w <= 1e-5f) continue;
            const float* m = skin_mats[v.bones[k]];
            px += w * (m[0] * v.px + m[1] * v.py + m[2] * v.pz + m[3]);
            py += w * (m[4] * v.px + m[5] * v.py + m[6] * v.pz + m[7]);
            pz += w * (m[8] * v.px + m[9] * v.py + m[10] * v.pz + m[11]);

            nx += w * (m[0] * v.nx + m[1] * v.ny + m[2] * v.nz);
            ny += w * (m[4] * v.nx + m[5] * v.ny + m[6] * v.nz);
            nz += w * (m[8] * v.nx + m[9] * v.ny + m[10] * v.nz);
        }
        const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nl > 1e-6f) {
            const float inv = 1.0f / nl;
            nx *= inv;
            ny *= inv;
            nz *= inv;
        }
        // Pack (x=fwd, y=up, z=right) -> D3D viewmodel space (x=right, y=up, z=forward)
        d3d::ViewmodelTexturedVertex& dst = out_verts[i];
        dst.x = pz;
        dst.y = py;
        dst.z = px;
        dst.nx = nz;
        dst.ny = ny;
        dst.nz = nx;
        dst.bgra = v.bgra;
        dst.u = v.u;
        dst.v = v.v;
    }
}

bool SkinnedHandsAsset::SkinVertices(uint32_t clip_idx, float time_s, float trigger_pull,
                                     std::vector<d3d::ViewmodelTexturedVertex>& out_verts) const {
    float skin_mats[kMaxHsknJoints][16]{};
    if (!EvaluatePose(clip_idx, time_s, trigger_pull, skin_mats, nullptr)) return false;
    ApplySkinMatrices(skin_mats, out_verts);
    return true;
}

bool SkinnedHandsAsset::SkinVerticesBlended(uint32_t clip_a, float time_a, uint32_t clip_b, float time_b,
                                            float blend, float trigger_pull,
                                            std::vector<d3d::ViewmodelTexturedVertex>& out_verts) const {
    const float b = std::clamp(blend, 0.0f, 1.0f);
    if (b <= 1e-4f || clip_a == clip_b) {
        return SkinVertices(clip_a, time_a, trigger_pull, out_verts);
    }
    if (b >= 0.9999f) {
        return SkinVertices(clip_b, time_b, trigger_pull, out_verts);
    }
    float ta[kMaxHsknJoints][3]{}, qa[kMaxHsknJoints][4]{}, sa[kMaxHsknJoints][3]{};
    float tb[kMaxHsknJoints][3]{}, qb[kMaxHsknJoints][4]{}, sb[kMaxHsknJoints][3]{};
    if (!SamplePoseLocals(clip_a, time_a, trigger_pull, ta, qa, sa)) return false;
    if (!SamplePoseLocals(clip_b, time_b, trigger_pull, tb, qb, sb)) return false;

    const uint32_t jc = uint32_t(joints_.size());
    float tm[kMaxHsknJoints][3]{}, qm[kMaxHsknJoints][4]{}, sm[kMaxHsknJoints][3]{};
    for (uint32_t j = 0; j < jc; ++j) {
        for (int k = 0; k < 3; ++k) {
            tm[j][k] = ta[j][k] + (tb[j][k] - ta[j][k]) * b;
            sm[j][k] = sa[j][k] + (sb[j][k] - sa[j][k]) * b;
        }
        NlerpQuat(qa[j], qb[j], b, qm[j]);
    }
    float skin_mats[kMaxHsknJoints][16]{};
    BuildSkinFromLocals(tm, qm, sm, skin_mats, nullptr);
    ApplySkinMatrices(skin_mats, out_verts);
    return true;
}

}  // namespace game::hands_skinner
