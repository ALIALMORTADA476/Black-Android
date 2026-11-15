#include "game/delirium_rules.h"

#include <algorithm>
#include <cmath>

namespace game::delirium {

namespace {

inline float ClampSafe(float v, float lo, float hi, float fallback) noexcept {
    if (!std::isfinite(v)) return fallback;
    return std::clamp(v, lo, hi);
}

inline void NormalizeVec3(const float in[3], float out[3]) noexcept {
    const float x = std::isfinite(in[0]) ? in[0] : 0.0f;
    const float y = std::isfinite(in[1]) ? in[1] : 0.0f;
    const float z = std::isfinite(in[2]) ? in[2] : 1.0f;
    const float len2 = x * x + y * y + z * z;
    if (len2 <= 1e-6f) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 1.0f;
        return;
    }
    const float inv = 1.0f / std::sqrt(len2);
    out[0] = x * inv;
    out[1] = y * inv;
    out[2] = z * inv;
}

inline bool IsLimbOrHeadZone(HitZone zone) noexcept {
    return zone == HitZone::Head || zone == HitZone::LeftArm ||
           zone == HitZone::RightArm || zone == HitZone::LeftLeg ||
           zone == HitZone::RightLeg;
}

}  // namespace

HitZone ClassifyHitZone(uint8_t raw_zone, const float hit_dir[3]) noexcept {
    const float lat = (hit_dir && std::isfinite(hit_dir[0])) ? hit_dir[0] : 0.0f;
    switch (raw_zone) {
    case 0x08u:
        return HitZone::Head;
    case 0x07u:
        return (lat > 0.35f) ? HitZone::RightArm : HitZone::LeftArm;
    case 0x09u:
        return (lat < -0.35f) ? HitZone::LeftArm : HitZone::RightArm;
    case 0x0Au:
        return (lat > 0.25f) ? HitZone::RightLeg : HitZone::LeftLeg;
    case 0x01u:
    case 0x02u:
    case 0x03u:
    case 0x04u:
    case 0x05u:
    case 0x06u:
        return HitZone::TorsoChest;
    default:
        return HitZone::CenterMass;
    }
}

void Simulation::Reset() noexcept {
    actors_.fill(ActorState{});
    tick_counter_ = 0;
}

void Simulation::SetConfig(const Config& cfg) noexcept {
    cfg_.enabled = cfg.enabled;
    cfg_.intensity = ClampSafe(cfg.intensity, 0.25f, 3.00f, 1.35f);
    cfg_.blind_fire_enabled = cfg.blind_fire_enabled;
    cfg_.agony_duration = ClampSafe(cfg.agony_duration, 0.00f, 2.50f, 1.10f);
}

Simulation::ActorState* Simulation::FindActor(uint32_t ea) noexcept {
    if (ea == 0u) return nullptr;
    for (auto& slot : actors_) {
        if (slot.ea == ea) {
            slot.stamp = ++tick_counter_;
            return &slot;
        }
    }
    return nullptr;
}

Simulation::ActorState* Simulation::FindOrAllocActor(uint32_t ea) noexcept {
    if (ea == 0u) return nullptr;
    ActorState* oldest = &actors_[0];
    for (auto& slot : actors_) {
        if (slot.ea == ea) {
            slot.stamp = ++tick_counter_;
            return &slot;
        }
        if (slot.ea == 0u) {
            slot = ActorState{};
            slot.ea = ea;
            slot.stamp = ++tick_counter_;
            return &slot;
        }
        if (slot.stamp < oldest->stamp) {
            oldest = &slot;
        }
    }
    *oldest = ActorState{};
    oldest->ea = ea;
    oldest->stamp = ++tick_counter_;
    return oldest;
}

void Simulation::PopulateCollapse(ActorState& st) noexcept {
    CollapseProfile& c = st.pending_collapse;
    c.valid = true;

    const float scale = cfg_.intensity;
    const float f = std::clamp(st.last_force, 0.50f, 4.00f) * scale;
    const float side = (std::fabs(st.lateral_dot) > 0.08f)
                           ? st.lateral_dot
                           : ((st.last_zone == HitZone::LeftArm || st.last_zone == HitZone::LeftLeg) ? -0.65f : 0.65f);

    // Keep accumulated stagger momentum + add zone-specific dramatic launch
    c.launch_vel[0] = st.stumble_vel[0] * 0.72f + st.last_dir[0] * (2.95f * f);
    c.launch_vel[1] = 0.0f;
    c.launch_vel[2] = st.stumble_vel[2] * 0.72f + st.last_dir[2] * (2.95f * f);

    c.spin_torque[0] = 0.0f;
    c.spin_torque[1] = 0.0f;
    c.spin_torque[2] = 0.0f;

    switch (st.last_zone) {
    case HitZone::Head:
        c.upward_lift = 1.90f * scale;
        c.spin_torque[0] = -1.75f * scale; // whip head/spine backward
        c.spin_torque[1] = side * 0.85f * scale;
        break;
    case HitZone::LeftArm:
    case HitZone::RightArm: {
        const float dir_sign = (st.last_zone == HitZone::RightArm) ? 1.0f : -1.0f;
        c.upward_lift = 1.15f * scale;
        c.spin_torque[1] = dir_sign * 2.55f * scale; // strong shoulder twist
        c.spin_torque[2] = dir_sign * 1.35f * scale;
        break;
    }
    case HitZone::LeftLeg:
    case HitZone::RightLeg:
        c.upward_lift = -0.85f * scale; // knees buckle downward immediately
        c.spin_torque[0] = 1.10f * scale;
        c.spin_torque[2] = side * 2.25f * scale;
        break;
    case HitZone::TorsoChest:
    case HitZone::CenterMass:
    default:
        c.upward_lift = 1.35f * scale;
        c.spin_torque[0] = -1.15f * scale;
        c.spin_torque[1] = side * 1.30f * scale;
        c.spin_torque[2] = side * 0.95f * scale;
        break;
    }

    c.launch_vel[1] += c.upward_lift;
    st.collapse_ready = true;
}

void Simulation::OnBulletHit(const HitEvent& ev) noexcept {
    if (!cfg_.enabled || ev.victim_ea == 0u) return;

    ActorState* st = FindOrAllocActor(ev.victim_ea);
    if (!st) return;

    float norm_dir[3]{};
    NormalizeVec3(ev.hit_dir, norm_dir);

    st->last_zone = ClassifyHitZone(ev.raw_zone, norm_dir);
    st->last_dir[0] = norm_dir[0];
    st->last_dir[1] = norm_dir[1];
    st->last_dir[2] = norm_dir[2];
    st->lateral_dot = ClampSafe(ev.lateral_dot, -1.0f, 1.0f, 0.0f);

    const float raw_f = ClampSafe(ev.raw_force, 0.0f, 12.0f, 0.5f);
    st->last_force = std::max(st->last_force * 0.5f, raw_f);

    // Multi-pellet shotgun / rapid-fire aggregation within ~45ms
    if (st->pellet_window > 0.0f) {
        st->pellet_accum = std::min(6.0f, st->pellet_accum + raw_f);
    } else {
        st->pellet_accum = raw_f;
    }
    st->pellet_window = 0.045f;

    const float effective_shock = st->pellet_accum * cfg_.intensity;

    if (ev.is_lethal) {
        PopulateCollapse(*st);
        if (cfg_.agony_duration > 0.02f && !st->agony_started) {
            st->agony_started = true;
            st->agony_total = cfg_.agony_duration;
            st->agony_remaining = cfg_.agony_duration;
            st->agony_phase = 0.0f;
            st->agony_step_count = 0;
        }
        return;
    }

    // Light grazing scratch floor: keep soldiers poised until hit meaningfully
    constexpr float kPoiseFloor = 0.42f;
    if (effective_shock < kPoiseFloor) {
        return;
    }

    // Recoil kick + multi-step balance degradation
    const float impulse = std::clamp((effective_shock - 0.25f) * 2.05f, 0.70f, 6.8f);
    st->stumble_vel[0] = std::clamp(st->stumble_vel[0] * 0.45f + norm_dir[0] * impulse, -5.8f, 5.8f);
    st->stumble_vel[1] = 0.0f;
    st->stumble_vel[2] = std::clamp(st->stumble_vel[2] * 0.45f + norm_dir[2] * impulse, -5.8f, 5.8f);

    st->balance = std::max(0.0f, st->balance - effective_shock * 0.28f);

    // Dramatic BLACK pacing: stumble duration scales cleanly without endless lock
    const float dur = std::clamp(0.45f + effective_shock * 0.24f, 0.45f, 1.35f);
    st->stumble_timer = std::max(st->stumble_timer, dur);
    st->clutch_timer = std::max(st->clutch_timer, dur * 1.25f);
    st->clutch_cooldown = 0.0f; // Trigger wound clutch immediately on next tick

    if (cfg_.blind_fire_enabled && (st->balance <= 0.65f || effective_shock >= 1.50f)) {
        st->blind_fire_timer = std::max(st->blind_fire_timer, dur * 0.85f);
    }
}

StepOutput Simulation::StepAlive(uint32_t soldier_ea, float dt) noexcept {
    StepOutput out{};
    if (!cfg_.enabled || soldier_ea == 0u || !std::isfinite(dt) || dt <= 0.0f) {
        return out;
    }

    ActorState* st = FindActor(soldier_ea);
    if (!st) return out;

    const float step_dt = std::clamp(dt, 0.0f, 0.05f);

    if (st->pellet_window > 0.0f) {
        st->pellet_window = std::max(0.0f, st->pellet_window - step_dt);
        if (st->pellet_window == 0.0f) {
            st->pellet_accum = 0.0f;
        }
    }

    // Balance recovers naturally once impact passes
    st->balance = std::min(1.0f, st->balance + step_dt * 0.35f);

    if (st->stumble_timer > 0.0f) {
        st->stumble_timer = std::max(0.0f, st->stumble_timer - step_dt);
        st->stumble_wave += step_dt * 11.5f;

        // Inject lateral wobble perpendicular to shot direction so soldier visibly loses footing
        const float wobble = std::sin(st->stumble_wave) * (0.85f * cfg_.intensity);
        const float perp_x = -st->last_dir[2];
        const float perp_z = st->last_dir[0];

        out.apply_stumble_vel = true;
        out.stumble_vel[0] = st->stumble_vel[0] + perp_x * wobble;
        out.stumble_vel[1] = 0.0f;
        out.stumble_vel[2] = st->stumble_vel[2] + perp_z * wobble;

        const float decay = std::exp(-3.4f * step_dt);
        st->stumble_vel[0] *= decay;
        st->stumble_vel[2] *= decay;
        out.lean_yaw_bias = st->lateral_dot * 0.6f + wobble * 0.35f;
    }

    if (st->clutch_timer > 0.0f) {
        st->clutch_timer = std::max(0.0f, st->clutch_timer - step_dt);
        if (st->clutch_cooldown > 0.0f) {
            st->clutch_cooldown = std::max(0.0f, st->clutch_cooldown - step_dt);
        }
        if (st->clutch_cooldown <= 0.0f) {
            out.trigger_clutch_overlay = true;
            out.anim_clip_id = IsLimbOrHeadZone(st->last_zone) ? 0x1Fu : 0x1Eu;
            const bool right_side = (st->last_zone == HitZone::RightArm ||
                                     st->last_zone == HitZone::RightLeg ||
                                     st->lateral_dot >= 0.0f);
            out.overlay_variant = right_side ? 0x2Au : 0x29u;
            out.overlay_weight = std::clamp(0.75f * cfg_.intensity, 0.35f, 2.25f);
            st->clutch_cooldown = 0.22f; // Cadence sustains clutched posture cleanly
        }
    }

    if (cfg_.blind_fire_enabled && st->blind_fire_timer > 0.0f) {
        st->blind_fire_timer = std::max(0.0f, st->blind_fire_timer - step_dt);
        st->scatter_phase += step_dt * 19.0f;
        out.blind_fire_ready = true;
        out.aim_scatter[0] = std::sin(st->scatter_phase * 1.3f) * (2.4f * cfg_.intensity);
        out.aim_scatter[1] = (0.75f + 0.55f * std::cos(st->scatter_phase * 2.1f)) * cfg_.intensity;
        out.aim_scatter[2] = std::cos(st->scatter_phase * 1.7f) * (1.8f * cfg_.intensity);
    }

    return out;
}

bool Simulation::ApplyBlindFireScatter(uint32_t soldier_ea, float in_out_target[3]) noexcept {
    if (!cfg_.enabled || !cfg_.blind_fire_enabled || soldier_ea == 0u || !in_out_target) {
        return false;
    }
    ActorState* st = FindActor(soldier_ea);
    if (!st || st->blind_fire_timer <= 0.0f || st->stumble_timer <= 0.0f) {
        return false;
    }

    st->scatter_phase += 1.37f;
    const float sweep_x = std::sin(st->scatter_phase * 1.3f) * (2.6f * cfg_.intensity);
    const float lift_y = (0.95f + 0.65f * std::fabs(std::cos(st->scatter_phase * 2.1f))) * cfg_.intensity;
    const float sweep_z = std::cos(st->scatter_phase * 1.7f) * (2.1f * cfg_.intensity);

    if (std::isfinite(in_out_target[0])) in_out_target[0] += sweep_x;
    if (std::isfinite(in_out_target[1])) in_out_target[1] += lift_y;
    if (std::isfinite(in_out_target[2])) in_out_target[2] += sweep_z;
    return true;
}

CollapseProfile Simulation::ConsumeCollapseProfile(uint32_t soldier_ea) noexcept {
    CollapseProfile out{};
    if (!cfg_.enabled || soldier_ea == 0u) return out;
    ActorState* st = FindActor(soldier_ea);
    if (!st || !st->collapse_ready) return out;
    out = st->pending_collapse;
    st->collapse_ready = false;
    return out;
}

AgonyPulse Simulation::StepAgony(uint32_t soldier_ea, float dt) noexcept {
    AgonyPulse pulse{};
    if (!cfg_.enabled || soldier_ea == 0u || !std::isfinite(dt) || dt <= 0.0f) {
        return pulse;
    }
    ActorState* st = FindActor(soldier_ea);
    if (!st) return pulse;

    if (!st->agony_started) {
        if (cfg_.agony_duration <= 0.02f) return pulse;
        st->agony_started = true;
        st->agony_total = cfg_.agony_duration;
        st->agony_remaining = cfg_.agony_duration;
        st->agony_phase = 0.0f;
        st->agony_step_count = 0;
    }

    if (st->agony_remaining <= 0.0f || st->agony_total <= 0.01f) {
        return pulse;
    }

    const float step_dt = std::clamp(dt, 0.0f, 0.05f);
    st->agony_remaining = std::max(0.0f, st->agony_remaining - step_dt);
    if (st->agony_remaining <= 0.0f) {
        return pulse;
    }

    const float env = st->agony_remaining / st->agony_total;
    const float damped_env = env * env;
    st->agony_phase += step_dt * 14.0f;
    st->agony_step_count++;

    // Cycle across chest/spine, shoulders/arms, and head for natural dying spasms
    static constexpr uint32_t kAgonyBones[] = {1u, 2u, 3u, 0u, 4u, 5u};
    pulse.active = true;
    pulse.bone_idx = kAgonyBones[st->agony_step_count % 6u];

    const float curl = std::sin(st->agony_phase);
    const float twist = std::cos(st->agony_phase * 0.7f);
    float raw_dir[3] = {
        curl * 0.85f + st->last_dir[0] * 0.25f,
        0.35f + 0.40f * std::fabs(twist),
        twist * 0.85f + st->last_dir[2] * 0.25f,
    };
    NormalizeVec3(raw_dir, pulse.impulse_dir);

    pulse.impulse_force = 0.42f * cfg_.intensity * damped_env * (0.65f + 0.35f * std::fabs(curl));
    return pulse;
}

}  // namespace game::delirium
