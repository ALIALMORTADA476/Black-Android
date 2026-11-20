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

// Directional damage angle (+right, -left, +/-pi behind) + camera kinetic punch (degrees)
void ComputeDirectionalDamageImpulse(const float cam_pos[3], const float cam_fwd[3],
                                     const float cam_right[3], const float source_pos[3],
                                     float damage_norm, float& out_rel_angle_rad,
                                     float& out_yaw_kick_deg, float& out_pitch_kick_deg,
                                     float& out_roll_kick_deg) noexcept;

// Tactical Hitstop: 1..3 frame impact micro-freeze (0 for non-critical body shots, <= 0.05s)
// weapon_tier: 0 = regular rifle/pistol, 1 = shotgun/heavy, 2 = sniper/explosive launcher
float EvaluateHitstopDurationSec(bool is_headshot, bool is_lethal,
                                 uint8_t weapon_tier, float distance_m) noexcept;

// Active Reload tactical sweet-spot window (press Reload mid-bar [0.42..0.64] for speed boost)
struct ActiveReloadTapResult {
    bool handled = false;
    bool success = false;
    float remaining_scale = 1.0f;  // <1 on boost, >1 on jam
};

class ActiveReloadState {
public:
    void Reset() noexcept;
    void Start(float total_duration_sec) noexcept;
    void Step(float dt) noexcept;
    ActiveReloadTapResult TryTap(float elapsed_sec) noexcept;
    bool IsActive() const noexcept { return active_; }
    bool Consumed() const noexcept { return tapped_; }
    float Progress() const noexcept;
    float Elapsed() const noexcept { return elapsed_; }
    static constexpr float kBandMin = 0.42f;
    static constexpr float kBandMax = 0.64f;

private:
    bool active_ = false;
    bool tapped_ = false;
    float duration_ = 1.6f;
    float elapsed_ = 0.0f;
};

}  // namespace game::combat_feedback
