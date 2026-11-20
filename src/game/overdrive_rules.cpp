#include "game/overdrive_rules.h"

#include <algorithm>
#include <cmath>

namespace game::overdrive_rules {

SpecialAmmoType SanitizeSpecialAmmo(uint32_t mode) noexcept {
    if (mode > uint32_t(SpecialAmmoType::RailAP)) {
        return SpecialAmmoType::RailAP;
    }
    return static_cast<SpecialAmmoType>(mode);
}

SpecialAmmoType CycleSpecialAmmo(SpecialAmmoType current) noexcept {
    const uint32_t next = (uint32_t(current) + 1u) % 4u;
    return static_cast<SpecialAmmoType>(next);
}

SpecialAmmoProfile GetSpecialAmmoProfile(SpecialAmmoType type) noexcept {
    switch (type) {
        case SpecialAmmoType::ThermiteIncendiary:
            return {type, "THRM", 1.22f, 1.15f, 1.35f, true, false, 1u};
        case SpecialAmmoType::ExplosiveHE:
            return {type, "HE", 1.38f, 1.20f, 2.65f, false, true, 2u};
        case SpecialAmmoType::RailAP:
            return {type, "RAIL", 1.55f, 1.50f, 2.25f, false, false, 5u};
        case SpecialAmmoType::Standard:
        default:
            return {SpecialAmmoType::Standard, "STD", 1.0f, 1.0f, 1.0f, false, false, 0u};
    }
}

void AdrenalineEngine::Reset() noexcept {
    charge_ = 0.0f;
    overdrive_active_ = false;
    overdrive_remaining_ = 0.0f;
    overdrive_duration_ = 8.0f;
    streak_ = 0;
    streak_remaining_ = 0.0f;
    idle_combat_sec_ = 0.0f;
    reflex_remaining_ = 0.0f;
    world_time_scale_ = 1.0f;
}

SiphonReward AdrenalineEngine::RecordCombatEvent(bool is_player_shot, bool headshot, bool lethal,
                                                 float cur_hp, float max_hp,
                                                 uint16_t cur_clip, uint16_t max_clip,
                                                 bool siphon_enabled, bool reflex_enabled) noexcept {
    SiphonReward out{};
    if (!is_player_shot) return out;

    idle_combat_sec_ = 0.0f;
    float gain = headshot ? 14.0f : 6.5f;
    if (lethal) {
        streak_ = std::min(streak_ + 1u, 99u);
        streak_remaining_ = 3.80f;
        gain += 22.0f + float(std::min(streak_, 5u)) * 6.0f;

        if (reflex_enabled && streak_ >= 2u) {
            reflex_remaining_ = std::max(reflex_remaining_, 1.15f);
        }

        if (siphon_enabled) {
            if (std::isfinite(cur_hp) && std::isfinite(max_hp) && max_hp > 1.0f && cur_hp < max_hp) {
                const float frac = overdrive_active_ ? 0.24f : 0.16f;
                out.hp_restore = std::min(max_hp - std::max(0.0f, cur_hp), max_hp * frac);
            }
            if (max_clip > 0u && cur_clip < max_clip) {
                const float clip_frac = overdrive_active_ ? 0.25f : 0.16f;
                const uint32_t want = std::max(1u, uint32_t(std::ceil(float(max_clip) * clip_frac)));
                out.clip_restore = uint16_t(std::min<uint32_t>(want, uint32_t(max_clip - cur_clip)));
            }
        }
    }

    if (!overdrive_active_) {
        charge_ = std::clamp(charge_ + gain, 0.0f, 100.0f);
    } else {
        overdrive_remaining_ = std::min(overdrive_duration_, overdrive_remaining_ + (lethal ? 0.90f : 0.18f));
        charge_ = std::clamp((overdrive_remaining_ / std::max(overdrive_duration_, 0.1f)) * 100.0f, 0.0f, 100.0f);
    }
    return out;
}

bool AdrenalineEngine::CanActivateManual() const noexcept {
    return !overdrive_active_ && charge_ >= 40.0f;
}

bool AdrenalineEngine::TryActivateOverdrive(bool force_free) noexcept {
    if (overdrive_active_) return false;
    if (!force_free && charge_ < 40.0f) return false;

    const float eff_charge = force_free ? std::max(charge_, 100.0f) : std::clamp(charge_, 40.0f, 100.0f);
    overdrive_duration_ = 4.2f + (eff_charge / 100.0f) * 4.8f;
    overdrive_remaining_ = overdrive_duration_;
    overdrive_active_ = true;
    charge_ = eff_charge;
    return true;
}

OverdriveStepOutput AdrenalineEngine::Step(float dt, bool system_enabled, bool reflex_enabled) noexcept {
    if (!system_enabled) {
        Reset();
        return Snapshot();
    }
    if (!std::isfinite(dt) || dt < 0.0f) dt = 0.0f;
    dt = std::min(dt, 0.1f);

    if (streak_ > 0) {
        streak_remaining_ -= dt;
        if (streak_remaining_ <= 0.0f) {
            streak_ = 0;
            streak_remaining_ = 0.0f;
        }
    }

    if (overdrive_active_) {
        overdrive_remaining_ -= dt;
        if (overdrive_remaining_ <= 0.0f) {
            overdrive_active_ = false;
            overdrive_remaining_ = 0.0f;
            charge_ = 0.0f;
        } else {
            charge_ = std::clamp((overdrive_remaining_ / std::max(overdrive_duration_, 0.1f)) * 100.0f, 0.0f, 100.0f);
        }
    } else {
        idle_combat_sec_ += dt;
        if (idle_combat_sec_ > 4.5f && charge_ > 0.0f) {
            charge_ = std::max(0.0f, charge_ - 6.5f * dt);
        }
    }

    float target_scale = 1.0f;
    if (reflex_enabled && reflex_remaining_ > 0.0f) {
        reflex_remaining_ = std::max(0.0f, reflex_remaining_ - dt);
        target_scale = 0.48f;
    }
    const float blend_rate = (target_scale < world_time_scale_) ? 16.0f : 6.5f;
    world_time_scale_ += (target_scale - world_time_scale_) * std::min(1.0f, dt * blend_rate);
    world_time_scale_ = std::clamp(world_time_scale_, 0.35f, 1.0f);

    return Snapshot();
}

OverdriveStepOutput AdrenalineEngine::Snapshot() const noexcept {
    OverdriveStepOutput out;
    out.overdrive_active = overdrive_active_;
    out.charge_pct = charge_;
    out.overdrive_sec = overdrive_remaining_;
    out.streak_count = streak_;
    out.streak_timer_sec = streak_remaining_;
    out.fire_rate_multiplier = overdrive_active_ ? 1.45f : 1.0f;
    out.dash_cooldown_multiplier = overdrive_active_ ? 0.50f : 1.0f;
    out.world_time_scale = world_time_scale_;
    if (overdrive_active_) {
        const float ratio = std::clamp(overdrive_remaining_ / std::max(overdrive_duration_, 0.1f), 0.0f, 1.0f);
        out.edge_energy = 0.45f + 0.55f * ratio;
    } else if (reflex_remaining_ > 0.0f) {
        out.edge_energy = 0.30f;
    } else {
        out.edge_energy = 0.0f;
    }
    return out;
}

bool ComputeDashMoveVector(const float cam_forward[3], const float cam_right[3],
                           float stick_x, float stick_y, float out_dir[3]) noexcept {
    if (!cam_forward || !cam_right || !out_dir) return false;
    float fx = cam_forward[0], fz = cam_forward[2];
    float flen = std::sqrt(fx * fx + fz * fz);
    if (flen < 1e-4f) return false;
    fx /= flen;
    fz /= flen;

    float rx = cam_right[0], rz = cam_right[2];
    float rlen = std::sqrt(rx * rx + rz * rz);
    if (rlen < 1e-4f) {
        rx = fz;
        rz = -fx;
    } else {
        rx /= rlen;
        rz /= rlen;
    }

    float dx = fx, dz = fz;
    if ((std::fabs(stick_x) + std::fabs(stick_y)) > 0.15f) {
        dx = fx * stick_y + rx * stick_x;
        dz = fz * stick_y + rz * stick_x;
        const float dlen = std::sqrt(dx * dx + dz * dz);
        if (dlen > 1e-4f) {
            dx /= dlen;
            dz /= dlen;
        } else {
            dx = fx;
            dz = fz;
        }
    }
    out_dir[0] = dx;
    out_dir[1] = 0.0f;
    out_dir[2] = dz;
    return true;
}

void KineticDashController::Reset() noexcept {
    dash_remaining_ = 0.0f;
    cooldown_remaining_ = 0.0f;
    shockwave_pending_ = false;
}

bool KineticDashController::TryTrigger(bool enabled, bool /*overdrive_active*/,
                                       bool /*shockwave_enabled*/) noexcept {
    if (!enabled || dash_remaining_ > 0.0f || cooldown_remaining_ > 0.0f) {
        return false;
    }
    dash_duration_ = 0.26f;
    dash_remaining_ = dash_duration_;
    cooldown_remaining_ = 1.20f;
    shockwave_pending_ = false;
    return true;
}

DashStepOutput KineticDashController::Step(float dt) noexcept {
    if (!std::isfinite(dt) || dt < 0.0f) dt = 0.0f;
    dt = std::min(dt, 0.08f);

    DashStepOutput out{};
    if (cooldown_remaining_ > 0.0f) {
        cooldown_remaining_ = std::max(0.0f, cooldown_remaining_ - dt);
    }

    shockwave_pending_ = false;
    out.emit_shockwave = false;

    if (dash_remaining_ > 0.0f) {
        const float elapsed_frac = std::clamp(1.0f - (dash_remaining_ / std::max(dash_duration_, 0.01f)), 0.0f, 1.0f);
        const float curve = 1.0f - elapsed_frac * elapsed_frac;
        out.dashing = true;
        out.speed_mps = 62.0f * curve;
        out.fov_boost_deg = 16.5f * curve;
        dash_remaining_ = std::max(0.0f, dash_remaining_ - dt);
    }
    out.cooldown_remaining = cooldown_remaining_;
    return out;
}

VisorShaderParams ComputeVisorShaderParams(bool thermal_enabled, bool in_gameplay,
                                           bool menu_open, float /*edge_energy*/) noexcept {
    VisorShaderParams p{};
    if (!in_gameplay || menu_open) return p;
    p.thermal_mix = thermal_enabled ? 1.0f : 0.0f;
    p.overdrive_edge = 0.0f;
    return p;
}

bool ShouldRenderThermalBracket(bool thermal_enabled, bool target_alive, float clip_w) noexcept {
    return thermal_enabled && target_alive && std::isfinite(clip_w) && clip_w > 0.08f;
}

void KillCamSlowmoController::Reset() noexcept {
    remaining_ = 0.0f;
    duration_ = 1.15f;
    min_scale_ = 0.40f;
}

void KillCamSlowmoController::Trigger(bool enabled, float intensity) noexcept {
    if (!enabled) return;
    const float k = std::clamp(std::isfinite(intensity) ? intensity : 1.0f, 0.4f, 1.5f);
    duration_ = 0.90f + 0.30f * k;
    remaining_ = duration_;
    min_scale_ = std::clamp(0.48f - 0.10f * k, 0.34f, 0.55f);
}

float KillCamSlowmoController::Step(float dt, bool enabled) noexcept {
    if (!enabled || remaining_ <= 0.0f) {
        remaining_ = 0.0f;
        return 1.0f;
    }
    if (std::isfinite(dt) && dt > 0.0f) {
        remaining_ = std::max(0.0f, remaining_ - dt);
    }
    if (remaining_ <= 0.0f) return 1.0f;
    const float u = 1.0f - (remaining_ / std::max(duration_, 0.1f));
    // Sharp entry into slow-mo, then smooth quadratic recovery back to 1.0x
    if (u < 0.55f) return min_scale_;
    const float rec = (u - 0.55f) / 0.45f;
    return min_scale_ + (1.0f - min_scale_) * (rec * rec);
}

}  // namespace game::overdrive_rules
