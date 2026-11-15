#pragma once

#include <array>
#include <cstdint>

namespace game::delirium {

enum class HitZone : uint8_t {
    CenterMass = 0,
    Head,
    TorsoChest,
    LeftArm,
    RightArm,
    LeftLeg,
    RightLeg,
};

struct Config {
    bool enabled = true;
    float intensity = 1.35f;        // 0.25 .. 3.00 (BLACK cinematic dramatic default)
    bool blind_fire_enabled = true;
    float agony_duration = 1.10f;   // 0.00 .. 2.50 seconds
};

struct HitEvent {
    uint32_t victim_ea = 0;
    float hit_dir[3] = {0.0f, 0.0f, 1.0f};
    float raw_force = 0.0f;
    uint8_t raw_zone = 0;
    float lateral_dot = 0.0f; // >0 right side, <0 left side
    bool is_lethal = false;
};

struct StepOutput {
    bool apply_stumble_vel = false;
    float stumble_vel[3] = {0.0f, 0.0f, 0.0f};
    bool trigger_clutch_overlay = false;
    uint32_t anim_clip_id = 0x1Eu;    // 0x1E (torso) or 0x1F (limb/head)
    uint32_t overlay_variant = 0x29u; // 0x29 or 0x2A for sub_0017F270
    float overlay_weight = 0.0f;
    float lean_yaw_bias = 0.0f;
    bool blind_fire_ready = false;
    float aim_scatter[3] = {0.0f, 0.0f, 0.0f};
};

struct CollapseProfile {
    bool valid = false;
    float launch_vel[3] = {0.0f, 0.0f, 0.0f};
    float spin_torque[3] = {0.0f, 0.0f, 0.0f};
    float upward_lift = 0.0f;
};

struct AgonyPulse {
    bool active = false;
    uint32_t bone_idx = 0;
    float impulse_dir[3] = {0.0f, 0.0f, 0.0f};
    float impulse_force = 0.0f;
};

HitZone ClassifyHitZone(uint8_t raw_zone, const float hit_dir[3]) noexcept;

class Simulation {
public:
    static constexpr size_t kMaxTrackedSoldiers = 64;

    void Reset() noexcept;
    void SetConfig(const Config& cfg) noexcept;
    Config GetConfig() const noexcept { return cfg_; }

    void OnBulletHit(const HitEvent& ev) noexcept;
    StepOutput StepAlive(uint32_t soldier_ea, float dt) noexcept;
    bool ApplyBlindFireScatter(uint32_t soldier_ea, float in_out_target[3]) noexcept;
    CollapseProfile ConsumeCollapseProfile(uint32_t soldier_ea) noexcept;
    AgonyPulse StepAgony(uint32_t soldier_ea, float dt) noexcept;

private:
    struct ActorState {
        uint32_t ea = 0;
        uint64_t stamp = 0;
        HitZone last_zone = HitZone::CenterMass;
        float last_dir[3] = {0.0f, 0.0f, 1.0f};
        float lateral_dot = 0.0f;
        float last_force = 0.0f;

        // Shotgun / rapid-burst multi-pellet window
        float pellet_accum = 0.0f;
        float pellet_window = 0.0f;

        // Balance, stagger & clutching
        float balance = 1.0f;
        float stumble_vel[3] = {0.0f, 0.0f, 0.0f};
        float stumble_timer = 0.0f;
        float stumble_wave = 0.0f;
        float clutch_timer = 0.0f;
        float clutch_cooldown = 0.0f;

        // Erratic blind-fire while staggering
        float blind_fire_timer = 0.0f;
        float scatter_phase = 0.0f;

        // Collapse momentum & agony writhing
        bool collapse_ready = false;
        CollapseProfile pending_collapse{};
        bool agony_started = false;
        float agony_remaining = 0.0f;
        float agony_total = 0.0f;
        float agony_phase = 0.0f;
        uint32_t agony_step_count = 0;
    };

    ActorState* FindActor(uint32_t ea) noexcept;
    ActorState* FindOrAllocActor(uint32_t ea) noexcept;
    void PopulateCollapse(ActorState& st) noexcept;

    Config cfg_{};
    std::array<ActorState, kMaxTrackedSoldiers> actors_{};
    uint64_t tick_counter_ = 0;
};

}  // namespace game::delirium
