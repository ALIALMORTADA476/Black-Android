#pragma once

#include <cstdint>

namespace game::combat_feedback {

enum class HitKind : uint8_t {
    Body = 0,
    Headshot = 1,
    Kill = 2,
    HeadshotKill = 3
};

struct HitmarkerVisualSpec {
    bool active = false;
    HitKind kind = HitKind::Body;
    float alpha = 0.0f;
    float inner_gap_px = 7.0f;
    float tick_len_px = 6.0f;
    float thickness_px = 2.0f;
    bool has_outer_kill_wing = false;
    float outer_wing_gap_px = 14.0f;
    uint8_t r = 240, g = 244, b = 248;
    uint8_t wing_r = 255, wing_g = 90, wing_b = 56;
};

HitKind ClassifyHit(bool is_headshot, bool is_lethal);
float HitmarkerDurationSec(HitKind kind);
HitmarkerVisualSpec EvaluateHitmarkerVisual(float age_sec, HitKind kind, float ui_scale = 1.0f);

// Enhanced Recoil Curve helpers (SettingId::GameplayEnhancedRecoil):
// - Decay returns factor < 1.0f so pitch settles snappier (1.38x exponent) with zero runaway spin.
// - Kick scales initial muzzle snap slightly (1.12x) plus crisp deterministic micro-sway yaw.
float ComputeEnhancedRecoilDecayFactor(float base_factor, float frame_share,
                                       bool enhanced_recoil, bool no_recoil);
float ComputeEnhancedRecoilKickScale(bool enhanced_recoil, bool no_recoil,
                                     uint64_t shot_seq, float& out_yaw_kick_deg);

}  // namespace game::combat_feedback
