#pragma once

#include <cstdint>
#include <vector>

#include "d3d/world_overlay.h"

namespace game::hands_skinner {

inline constexpr uint32_t kHsknMagic = 0x4E4B5348u;  // 'HSKN'
inline constexpr uint32_t kMaxHsknJoints = 40u;

struct HsknVertex {
    float px = 0.0f, py = 0.0f, pz = 0.0f;  // Pack space (x=fwd, y=up, z=right)
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    float u = 0.0f, v = 0.0f;
    uint32_t bgra = 0xFFFFFFFFu;
    uint8_t bones[4]{};
    float weights[4]{};
};

struct HsknJoint {
    int32_t parent = -1;
    float rest_t[3]{};
    float rest_q[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float rest_s[3]{1.0f, 1.0f, 1.0f};
    float inv_bind[16]{};  // Row-major 4x4
};

struct HsknChannel {
    uint32_t joint_idx = 0;
    uint32_t path_kind = 0;  // 0=translation(3f), 1=rotation(4f), 2=scale(3f)
    std::vector<float> times;
    std::vector<float> values;
};

struct HsknClip {
    char name[32]{};
    float duration_s = 0.0f;
    std::vector<HsknChannel> channels;
};

class SkinnedHandsAsset {
public:
    bool Parse(const uint8_t* data, size_t size);
    bool IsLoaded() const noexcept { return loaded_; }

    uint32_t VertexCount() const noexcept { return uint32_t(verts_.size()); }
    uint32_t IndexCount() const noexcept { return uint32_t(indices_.size()); }
    uint32_t JointCount() const noexcept { return uint32_t(joints_.size()); }
    uint32_t ClipCount() const noexcept { return uint32_t(clips_.size()); }

    const std::vector<HsknVertex>& Vertices() const noexcept { return verts_; }
    const std::vector<uint16_t>& Indices() const noexcept { return indices_; }
    const std::vector<HsknClip>& Clips() const noexcept { return clips_; }

    // Evaluates joint skinning matrices (skin[j] = anim_world[j] * inv_bind[j]) in pack space,
    // and optionally returns animated joint origins in pack space for verification/IK checks.
    bool EvaluatePose(uint32_t clip_idx, float time_s, float trigger_pull,
                      float out_skin_mats[kMaxHsknJoints][16],
                      float out_joint_pos[kMaxHsknJoints][3] = nullptr) const;

    // Skins all vertices into D3D viewmodel coordinates (x=right, y=up, z=forward).
    bool SkinVertices(uint32_t clip_idx, float time_s, float trigger_pull,
                      std::vector<d3d::ViewmodelTexturedVertex>& out_verts) const;

    // Cross-fades smoothly between two animation poses (`blend` 0->A, 1->B) in pack space.
    bool SkinVerticesBlended(uint32_t clip_a, float time_a, uint32_t clip_b, float time_b,
                             float blend, float trigger_pull,
                             std::vector<d3d::ViewmodelTexturedVertex>& out_verts) const;

private:
    bool SamplePoseLocals(uint32_t clip_idx, float time_s, float trigger_pull,
                          float out_t[kMaxHsknJoints][3],
                          float out_q[kMaxHsknJoints][4],
                          float out_s[kMaxHsknJoints][3]) const;

    void BuildSkinFromLocals(const float t[kMaxHsknJoints][3],
                             const float q[kMaxHsknJoints][4],
                             const float s[kMaxHsknJoints][3],
                             float out_skin_mats[kMaxHsknJoints][16],
                             float out_joint_pos[kMaxHsknJoints][3]) const;

    void ApplySkinMatrices(const float skin_mats[kMaxHsknJoints][16],
                           std::vector<d3d::ViewmodelTexturedVertex>& out_verts) const;

    bool loaded_ = false;
    std::vector<HsknVertex> verts_;
    std::vector<uint16_t> indices_;
    std::vector<HsknJoint> joints_;
    std::vector<HsknClip> clips_;
};

}  // namespace game::hands_skinner
