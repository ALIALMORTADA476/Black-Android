#pragma once

#include <cstdint>

namespace game::overdrive_rules {

enum class SpecialAmmoType : uint8_t {
    Standard = 0,
    ThermiteIncendiary = 1,
    ExplosiveHE = 2,
    RailAP = 3,
};

struct SpecialAmmoProfile {
    SpecialAmmoType type = SpecialAmmoType::Standard;
    const char* short_code = "STD";
    float damage_scale = 1.0f;
    float headshot_scale = 1.0f;
    float force_scale = 1.0f;
    bool ignite_target = false;
    bool detonate_on_hit = false;
    uint32_t tracer_color_override = 0;  // 0=default, 1=gold, 2=crimson, 5=electric cyan
};

SpecialAmmoType SanitizeSpecialAmmo(uint32_t mode) noexcept;
SpecialAmmoType CycleSpecialAmmo(SpecialAmmoType current) noexcept;
SpecialAmmoProfile GetSpecialAmmoProfile(SpecialAmmoType type) noexcept;

struct SiphonReward {
    float hp_restore = 0.0f;
    uint16_t clip_restore = 0;
};

struct OverdriveStepOutput {
    bool overdrive_active = false;
    float charge_pct = 0.0f;        // 0..100
    float overdrive_sec = 0.0f;
    uint32_t streak_count = 0;
    float streak_timer_sec = 0.0f;
    float fire_rate_multiplier = 1.0f;
    float dash_cooldown_multiplier = 1.0f;
    float world_time_scale = 1.0f;    // Smooth reflex multi-kill world slow-mo (0.48..1.0)
    float edge_energy = 0.0f;         // 0..1 peripheral shader energy pulse
};

class AdrenalineEngine {
public:
    void Reset() noexcept;

    SiphonReward RecordCombatEvent(bool is_player_shot, bool headshot, bool lethal,
                                   float cur_hp, float max_hp,
                                   uint16_t cur_clip, uint16_t max_clip,
                                   bool siphon_enabled, bool reflex_enabled) noexcept;

    bool CanActivateManual() const noexcept;
    bool TryActivateOverdrive(bool force_free = false) noexcept;

    OverdriveStepOutput Step(float dt, bool system_enabled, bool reflex_enabled) noexcept;

    OverdriveStepOutput Snapshot() const noexcept;

private:
    float charge_ = 0.0f;
    bool overdrive_active_ = false;
    float overdrive_remaining_ = 0.0f;
    float overdrive_duration_ = 8.0f;
    uint32_t streak_ = 0;
    float streak_remaining_ = 0.0f;
    float idle_combat_sec_ = 0.0f;
    float reflex_remaining_ = 0.0f;
    float world_time_scale_ = 1.0f;
};

// Pure directional horizontal dash basis: stick (x=right, y=forward) if pushed, else forward XZ
bool ComputeDashMoveVector(const float cam_forward[3], const float cam_right[3],
                           float stick_x, float stick_y, float out_dir[3]) noexcept;

struct DashStepOutput {
    bool dashing = false;
    bool emit_shockwave = false;
    float speed_mps = 0.0f;
    float fov_boost_deg = 0.0f;
    float cooldown_remaining = 0.0f;
};

class KineticDashController {
public:
    void Reset() noexcept;
    bool TryTrigger(bool enabled, bool overdrive_active, bool shockwave_enabled) noexcept;
    DashStepOutput Step(float dt) noexcept;
    float CooldownRemaining() const noexcept { return cooldown_remaining_; }
    bool IsDashing() const noexcept { return dash_remaining_ > 0.0f; }

private:
    float dash_remaining_ = 0.0f;
    float dash_duration_ = 0.16f;
    float cooldown_remaining_ = 0.0f;
    bool shockwave_pending_ = false;
};

struct VisorShaderParams {
    float thermal_mix = 0.0f;     // 0.0 or 1.0
    float overdrive_edge = 0.0f;  // 0.0..1.0
};

VisorShaderParams ComputeVisorShaderParams(bool thermal_enabled, bool in_gameplay,
                                           bool menu_open, float edge_energy) noexcept;

bool ShouldRenderThermalBracket(bool thermal_enabled, bool target_alive, float clip_w) noexcept;

}  // namespace game::overdrive_rules
