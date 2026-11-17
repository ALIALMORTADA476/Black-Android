#include "game/special_arsenal_core.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace game::special_arsenal {

namespace {

inline uint32_t ReadU32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

inline uint64_t ReadU64(const uint8_t* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, 8);
    return v;
}

inline float ReadF32(const uint8_t* p) {
    float f = 0.0f;
    std::memcpy(&f, p, 4);
    return f;
}

inline uint16_t ReadU16(const uint8_t* p) {
    uint16_t v = 0;
    std::memcpy(&v, p, 2);
    return v;
}

constexpr uint32_t kBmshMagic = 0x48534D42u;  // "BMSH"
constexpr uint32_t kBtexMagic = 0x58455442u;  // "BTEX"
constexpr uint32_t kMaxVerts = 65500u;
constexpr uint32_t kMaxIndices = 360000u;

}  // namespace

bool ParseBmsh(const uint8_t* data, size_t size, BmshModel& out) {
    out = BmshModel{};
    if (!data || size < 68u) return false;
    const uint32_t magic = ReadU32(data);
    const uint32_t ver = ReadU32(data + 4);
    if (magic != kBmshMagic) return false;

    if (ver == 1u) {
        const uint32_t vc = ReadU32(data + 8);
        const uint32_t ic = ReadU32(data + 12);
        if (vc == 0 || ic == 0 || vc > kMaxVerts || ic > kMaxIndices) return false;
        const size_t need = 68u + size_t(vc) * 28u + size_t(ic) * 2u;
        if (size < need) return false;

        out.version = 1u;
        for (int k = 0; k < 3; ++k) {
            out.bounds_min[k] = ReadF32(data + 16 + k * 4);
            out.bounds_max[k] = ReadF32(data + 28 + k * 4);
            out.nozzle_pos[k] = ReadF32(data + 40 + k * 4);
            out.pilot_pos[k] = ReadF32(data + 52 + k * 4);
        }
        out.eject_pos[0] = 0.025f;
        out.eject_pos[1] = 0.02f;
        out.eject_pos[2] = 0.12f;

        out.vertices.resize(vc);
        const uint8_t* vp = data + 68u;
        for (uint32_t i = 0; i < vc; ++i, vp += 28u) {
            BmshVertexV2& v = out.vertices[i];
            v.x = ReadF32(vp + 0);
            v.y = ReadF32(vp + 4);
            v.z = ReadF32(vp + 8);
            v.nx = ReadF32(vp + 12);
            v.ny = ReadF32(vp + 16);
            v.nz = ReadF32(vp + 20);
            v.bgra = ReadU32(vp + 24);
            v.u = 0.0f;
            v.v = 0.0f;
        }
        out.indices.resize(ic);
        const uint8_t* ip = data + 68u + size_t(vc) * 28u;
        for (uint32_t i = 0; i < ic; ++i) {
            const uint16_t idx = ReadU16(ip + i * 2u);
            if (idx >= vc) return false;
            out.indices[i] = idx;
        }
        BmshPart body{};
        std::memcpy(body.name, "body", 5);
        body.first_index = 0u;
        body.index_count = ic;
        out.parts.push_back(body);
        return true;
    }

    if (ver == 2u) {
        if (size < 84u) return false;
        const uint32_t vc = ReadU32(data + 8);
        const uint32_t ic = ReadU32(data + 12);
        const uint32_t pc = ReadU32(data + 16);
        const uint32_t flags = ReadU32(data + 20);
        if (vc == 0 || ic == 0 || pc == 0 || vc > kMaxVerts || ic > kMaxIndices || pc > 64u) {
            return false;
        }
        const size_t need = 84u + size_t(vc) * 36u + size_t(ic) * 2u + size_t(pc) * 64u;
        if (size < need) return false;

        out.version = 2u;
        out.flags = flags;
        for (int k = 0; k < 3; ++k) {
            out.bounds_min[k] = ReadF32(data + 24 + k * 4);
            out.bounds_max[k] = ReadF32(data + 36 + k * 4);
            out.nozzle_pos[k] = ReadF32(data + 48 + k * 4);
            out.pilot_pos[k] = ReadF32(data + 60 + k * 4);
            out.eject_pos[k] = ReadF32(data + 72 + k * 4);
        }

        out.vertices.resize(vc);
        const uint8_t* vp = data + 84u;
        for (uint32_t i = 0; i < vc; ++i, vp += 36u) {
            BmshVertexV2& v = out.vertices[i];
            v.x = ReadF32(vp + 0);
            v.y = ReadF32(vp + 4);
            v.z = ReadF32(vp + 8);
            v.nx = ReadF32(vp + 12);
            v.ny = ReadF32(vp + 16);
            v.nz = ReadF32(vp + 20);
            v.bgra = ReadU32(vp + 24);
            v.u = ReadF32(vp + 28);
            v.v = ReadF32(vp + 32);
        }

        out.indices.resize(ic);
        const uint8_t* ip = data + 84u + size_t(vc) * 36u;
        for (uint32_t i = 0; i < ic; ++i) {
            const uint16_t idx = ReadU16(ip + i * 2u);
            if (idx >= vc) return false;
            out.indices[i] = idx;
        }

        out.parts.resize(pc);
        const uint8_t* pp = ip + size_t(ic) * 2u;
        for (uint32_t i = 0; i < pc; ++i, pp += 64u) {
            BmshPart& pt = out.parts[i];
            std::memcpy(pt.name, pp, 32);
            pt.name[31] = '\0';
            pt.first_index = ReadU32(pp + 32);
            pt.index_count = ReadU32(pp + 36);
            if (uint64_t(pt.first_index) + uint64_t(pt.index_count) > ic) return false;
            for (int k = 0; k < 3; ++k) {
                pt.pivot[k] = ReadF32(pp + 40 + k * 4);
                pt.rest_offset[k] = ReadF32(pp + 52 + k * 4);
            }
        }
        return true;
    }

    return false;
}

bool ParseBtexHeader(const uint8_t* data, size_t size, BtexHeader& out) {
    out = BtexHeader{};
    if (!data || size < 32u) return false;
    out.magic = ReadU32(data + 0);
    out.version = ReadU32(data + 4);
    out.width = ReadU32(data + 8);
    out.height = ReadU32(data + 12);
    out.d3d_format = ReadU32(data + 16);
    out.payload_bytes = ReadU32(data + 20);
    out.cache_key = ReadU64(data + 24);
    if (out.magic != kBtexMagic || out.version != 1u) return false;
    if (out.width == 0 || out.height == 0) return false;
    if (uint64_t(out.width) * uint64_t(out.height) * 4ull != out.payload_bytes) return false;
    if (size < 32u + size_t(out.payload_bytes)) return false;
    return true;
}

BtexValidation ValidateBtex(const uint8_t* data, size_t size) {
    BtexValidation res{};
    if (!ParseBtexHeader(data, size, res.header)) return res;

    const uint32_t n_pix = res.header.width * res.header.height;
    const uint8_t* px = data + 32u;
    const uint32_t stride = (n_pix > 65536u) ? 4u : 1u;
    uint64_t white_cnt = 0;
    uint64_t sample_cnt = 0;
    double sum_lum = 0.0;
    double sum_lum_sq = 0.0;

    for (uint32_t i = 0; i < n_pix; i += stride) {
        const uint32_t bgra = ReadU32(px + size_t(i) * 4u);
        const uint8_t r = uint8_t((bgra >> 16) & 0xFFu);
        const uint8_t g = uint8_t((bgra >> 8) & 0xFFu);
        const uint8_t b = uint8_t(bgra & 0xFFu);
        if (r >= 245u && g >= 245u && b >= 245u) ++white_cnt;
        const double lum = 0.299 * r + 0.587 * g + 0.114 * b;
        sum_lum += lum;
        sum_lum_sq += lum * lum;
        ++sample_cnt;
    }

    if (sample_cnt == 0u) return res;
    const double mean = sum_lum / double(sample_cnt);
    const double var = std::max(0.0, (sum_lum_sq / double(sample_cnt)) - mean * mean);
    res.white_fraction = float(double(white_cnt) / double(sample_cnt));
    res.mean_luma = float(mean);
    res.std_rgb = float(std::sqrt(var));
    res.valid = true;
    return res;
}

void ArsenalSim::ResetAll() {
    initialized_ = true;
    active_ = SpecialWeaponId::Flamethrower;
    was_firing_flame_ = false;
    phase_ = ActionPhase::Idle;
    phase_timer_ = 0.0f;
    reload_timer_ = 0.0f;
    ads_blend_ = 0.0f;
    fire_cooldown_ = 0.0f;
    flame_pressure_ramp_ = 0.0f;
    boil_emit_timer_ = 0.0f;
    hammer_strike_timer_ = 0.0f;
    prev_fire_held_ = false;
    shell_pending_in_cycle_ = false;
    shot_seq_ = 0;

    fuel_pct_ = 100.0f;
    flame_reserve_pct_ = 200;
    ignition_cartridges_ = 5;

    m91_mag_ = 5;
    m91_reserve_ = 65;
    m91_reload_close_cue_sent_ = false;

    c96_mag_ = 20;
    c96_reserve_ = 120;
    c96_full_auto_ = true;
    c96_bolt_locked_open_ = false;
    c96_reload_close_cue_sent_ = false;

    maxim_mag_ = 250;
    maxim_reserve_ = 500;
    maxim_water_temp_c_ = 28.0f;
    maxim_water_level_l_ = 4.5f;
    maxim_wheel_angle_rad_ = 0.0f;
    maxim_belt_shift_ = 0.0f;
    maxim_recoil_kick_ = 0.0f;
    m91_recoil_kick_ = 0.0f;
    c96_barrel_kick_ = 0.0f;
    c96_bolt_kick_ = 0.0f;
}

void ArsenalSim::SelectWeapon(SpecialWeaponId id) {
    if (!initialized_) ResetAll();
    if (uint8_t(id) >= uint8_t(SpecialWeaponId::Count)) id = SpecialWeaponId::Flamethrower;
    if (active_ == id) return;
    active_ = id;
    phase_ = ActionPhase::Idle;
    phase_timer_ = 0.0f;
    reload_timer_ = 0.0f;
    fire_cooldown_ = 0.0f;
    was_firing_flame_ = false;
    flame_pressure_ramp_ = 0.0f;
    shell_pending_in_cycle_ = false;
}

void ArsenalSim::RefillActive() {
    if (!initialized_) ResetAll();
    switch (active_) {
        case SpecialWeaponId::Flamethrower:
            fuel_pct_ = 100.0f;
            flame_reserve_pct_ = 200;
            ignition_cartridges_ = 5;
            break;
        case SpecialWeaponId::Mauser1891:
            m91_mag_ = 5;
            m91_reserve_ = 65;
            break;
        case SpecialWeaponId::MauserC96:
            c96_mag_ = 20;
            c96_reserve_ = 120;
            c96_bolt_locked_open_ = false;
            break;
        case SpecialWeaponId::Maxim1910:
            maxim_mag_ = 250;
            maxim_reserve_ = 500;
            maxim_water_temp_c_ = 28.0f;
            maxim_water_level_l_ = 4.5f;
            break;
        default:
            break;
    }
    phase_ = ActionPhase::Idle;
    reload_timer_ = 0.0f;
}

ArsenalStepEvents ArsenalSim::Step(const ArsenalInput& in) {
    if (!initialized_) ResetAll();
    ArsenalStepEvents ev{};
    const float dt = std::clamp(in.dt, 0.0f, 0.05f);

    const float target_ads = (in.equipped && in.ads_held && phase_ != ActionPhase::Reloading) ? 1.0f : 0.0f;
    const float ads_rate = (active_ == SpecialWeaponId::Maxim1910) ? 5.2f : 8.5f;
    ads_blend_ = std::clamp(ads_blend_ + (target_ads - ads_blend_) * std::min(1.0f, dt * ads_rate), 0.0f, 1.0f);

    if (fire_cooldown_ > 0.0f) fire_cooldown_ = std::max(0.0f, fire_cooldown_ - dt);
    if (hammer_strike_timer_ > 0.0f) hammer_strike_timer_ = std::max(0.0f, hammer_strike_timer_ - dt);
    m91_recoil_kick_ = std::max(0.0f, m91_recoil_kick_ - dt * 6.0f);
    c96_barrel_kick_ = std::max(0.0f, c96_barrel_kick_ - dt * 11.0f);
    if (!c96_bolt_locked_open_) {
        c96_bolt_kick_ = std::max(0.0f, c96_bolt_kick_ - dt * 15.0f);
    }
    maxim_recoil_kick_ = std::max(0.0f, maxim_recoil_kick_ - dt * 8.5f);
    maxim_belt_shift_ = std::max(0.0f, maxim_belt_shift_ - dt * 9.5f);

    if (active_ == SpecialWeaponId::Maxim1910 && in.equipped) {
        // Real 0.310m Sokolov carriage wheels rotate strictly with translational ground motion
        maxim_wheel_angle_rad_ += (in.player_speed_mps * dt) / 0.155f;
    }

    // Passive thermal cooling of Maxim 4.5L water jacket
    if (maxim_water_temp_c_ > 28.0f && phase_ != ActionPhase::Firing) {
        maxim_water_temp_c_ = std::max(28.0f, maxim_water_temp_c_ - 10.5f * dt);
    }

    if (!in.equipped) {
        if (was_firing_flame_) {
            was_firing_flame_ = false;
            ev.PushAudio(AudioCueKind::FlameValveShutoff);
        }
        phase_ = ActionPhase::Idle;
        flame_pressure_ramp_ = 0.0f;
        prev_fire_held_ = false;
        return ev;
    }

    // Handle reload completion first
    if (phase_ == ActionPhase::Reloading) {
        reload_timer_ -= dt;
        if (active_ == SpecialWeaponId::Mauser1891 && !m91_reload_close_cue_sent_) {
            const float rt = 1.0f - std::clamp(reload_timer_ / 1.75f, 0.0f, 1.0f);
            if (rt >= 0.80f) {
                m91_reload_close_cue_sent_ = true;
                ev.PushAudio(AudioCueKind::Mauser91ChamberClose);
            }
        }
        if (active_ == SpecialWeaponId::MauserC96 && !c96_reload_close_cue_sent_) {
            const float rt = 1.0f - std::clamp(reload_timer_ / 1.55f, 0.0f, 1.0f);
            if (rt >= 0.80f) {
                c96_reload_close_cue_sent_ = true;
                ev.PushAudio(AudioCueKind::C96BreechClack);
            }
        }
        if (active_ == SpecialWeaponId::Maxim1910) {
            maxim_water_temp_c_ = std::max(38.0f, maxim_water_temp_c_ - 45.0f * dt);
            maxim_water_level_l_ = std::min(4.5f, maxim_water_level_l_ + 2.5f * dt);
        }
        if (reload_timer_ <= 0.0f) {
            phase_ = ActionPhase::Idle;
            reload_timer_ = 0.0f;
            if (active_ == SpecialWeaponId::Flamethrower) {
                const int need = std::min(100 - int(std::floor(fuel_pct_)), flame_reserve_pct_);
                fuel_pct_ = std::min(100.0f, fuel_pct_ + float(need));
                flame_reserve_pct_ = std::max(0, flame_reserve_pct_ - need);
                ignition_cartridges_ = 5;
            } else if (active_ == SpecialWeaponId::Mauser1891) {
                const int need = std::min(5 - m91_mag_, m91_reserve_);
                m91_mag_ += need;
                m91_reserve_ -= need;
            } else if (active_ == SpecialWeaponId::MauserC96) {
                const int need = std::min(20 - c96_mag_, c96_reserve_);
                c96_mag_ += need;
                c96_reserve_ -= need;
                c96_bolt_locked_open_ = false;
                c96_bolt_kick_ = 0.0f;
            } else if (active_ == SpecialWeaponId::Maxim1910) {
                const int need = std::min(250 - maxim_mag_, maxim_reserve_);
                maxim_mag_ += need;
                maxim_reserve_ -= need;
                maxim_water_level_l_ = 4.5f;
                maxim_water_temp_c_ = std::min(maxim_water_temp_c_, 48.0f);
            }
        }
        prev_fire_held_ = in.fire_held;
        return ev;
    }

    // Trigger reload if requested and useful (interrupting idle or sustained firing cleanly)
    if (in.reload_pressed && (phase_ == ActionPhase::Idle || phase_ == ActionPhase::Firing)) {
        if (active_ == SpecialWeaponId::Flamethrower &&
            ((fuel_pct_ < 99.0f && flame_reserve_pct_ > 0) || ignition_cartridges_ < 5)) {
            phase_ = ActionPhase::Reloading;
            reload_timer_ = 1.85f;
            ev.PushAudio(AudioCueKind::FlameTankReload);
            return ev;
        }
        if (active_ == SpecialWeaponId::Mauser1891 && m91_mag_ < 5 && m91_reserve_ > 0) {
            phase_ = ActionPhase::Reloading;
            reload_timer_ = 1.75f;
            m91_reload_close_cue_sent_ = false;
            ev.PushAudio(AudioCueKind::Mauser91BoltCycle);
            ev.PushAudio(AudioCueKind::Mauser91StripperClip);
            return ev;
        }
        if (active_ == SpecialWeaponId::MauserC96 && c96_mag_ < 20 && c96_reserve_ > 0) {
            phase_ = ActionPhase::Reloading;
            reload_timer_ = 1.55f;
            c96_reload_close_cue_sent_ = false;
            ev.PushAudio(AudioCueKind::C96ClipReload);
            return ev;
        }
        if (active_ == SpecialWeaponId::Maxim1910 &&
            ((maxim_mag_ < 250 && maxim_reserve_ > 0) || maxim_water_temp_c_ >= 80.0f)) {
            phase_ = ActionPhase::Reloading;
            reload_timer_ = 2.15f;
            ev.PushAudio(AudioCueKind::MaximTopCoverReload);
            return ev;
        }
    }

    // Per-weapon authentic mechanics
    switch (active_) {
        case SpecialWeaponId::Flamethrower: {
            const bool want_fire = in.fire_held && (fuel_pct_ > 0.25f);
            if (want_fire) {
                if (phase_ == ActionPhase::Idle) {
                    if (ignition_cartridges_ <= 0) {
                        phase_ = ActionPhase::Reloading;
                        reload_timer_ = 1.85f;
                        ev.PushAudio(AudioCueKind::FlameTankReload);
                        break;
                    }
                    ignition_cartridges_--;
                    phase_ = ActionPhase::Igniting;
                    phase_timer_ = 0.045f;
                    ev.PushAudio(AudioCueKind::FlameIgniteSquib);
                } else if (phase_ == ActionPhase::Igniting) {
                    phase_timer_ -= dt;
                    if (phase_timer_ <= 0.0f) {
                        phase_ = ActionPhase::Firing;
                    }
                }
                if (phase_ == ActionPhase::Firing) {
                    was_firing_flame_ = true;
                    flame_pressure_ramp_ = std::min(1.0f, flame_pressure_ramp_ + dt * 11.0f);
                    // ~7.0 seconds continuous thickened napalm capacity across twin tanks
                    fuel_pct_ = std::max(0.0f, fuel_pct_ - 14.2857f * dt);
                    ev.fire_flame_tick = true;
                    if (fuel_pct_ <= 0.25f) {
                        phase_ = ActionPhase::Idle;
                        was_firing_flame_ = false;
                        ev.PushAudio(AudioCueKind::FlameValveShutoff);
                    }
                }
            } else {
                if (was_firing_flame_ || phase_ == ActionPhase::Igniting) {
                    ev.PushAudio(AudioCueKind::FlameValveShutoff);
                }
                was_firing_flame_ = false;
                phase_ = ActionPhase::Idle;
                flame_pressure_ramp_ = std::max(0.0f, flame_pressure_ramp_ - dt * 14.0f);
                if (in.fire_pressed && fuel_pct_ <= 0.25f && flame_reserve_pct_ > 0) {
                    phase_ = ActionPhase::Reloading;
                    reload_timer_ = 1.85f;
                    ev.PushAudio(AudioCueKind::FlameTankReload);
                }
            }
            break;
        }

        case SpecialWeaponId::Mauser1891: {
            // Advance cock-on-close bolt state machine
            if (phase_ == ActionPhase::BoltWaitRelease) {
                if (!in.fire_held) {
                    phase_ = ActionPhase::BoltLift;
                    phase_timer_ = 0.11f;
                    ev.PushAudio(AudioCueKind::Mauser91BoltCycle);
                }
            } else if (phase_ == ActionPhase::BoltLift) {
                phase_timer_ -= dt;
                if (phase_timer_ <= 0.0f) {
                    phase_ = ActionPhase::BoltBack;
                    phase_timer_ = 0.14f;
                    shell_pending_in_cycle_ = true;
                }
            } else if (phase_ == ActionPhase::BoltBack) {
                phase_timer_ -= dt;
                if (shell_pending_in_cycle_ && phase_timer_ <= 0.07f) {
                    shell_pending_in_cycle_ = false;
                    ev.eject_shell = true;
                }
                if (phase_timer_ <= 0.0f) {
                    phase_ = ActionPhase::BoltPushCocking;
                    phase_timer_ = 0.18f;
                }
            } else if (phase_ == ActionPhase::BoltPushCocking) {
                phase_timer_ -= dt;
                if (phase_timer_ <= 0.0f) {
                    phase_ = ActionPhase::BoltLock;
                    phase_timer_ = 0.09f;
                    ev.PushAudio(AudioCueKind::Mauser91ChamberClose);
                }
            } else if (phase_ == ActionPhase::BoltLock) {
                phase_timer_ -= dt;
                if (phase_timer_ <= 0.0f) {
                    phase_ = ActionPhase::Idle;
                }
            } else if (phase_ == ActionPhase::Idle && in.fire_pressed) {
                if (m91_mag_ > 0) {
                    m91_mag_--;
                    shot_seq_++;
                    m91_recoil_kick_ = 1.0f;
                    ev.fire_bullet = true;
                    ev.bullet_damage = 135.0f;  // 7.65x53mm Argentine full-power rifle round
                    ev.bullet_velocity_mps = 820.0f;
                    ev.bullet_spread_deg = 0.06f + (1.0f - ads_blend_) * 0.45f;
                    ev.recoil_pitch_deg = 2.6f * (1.0f - 0.30f * ads_blend_);
                    ev.recoil_yaw_deg = ((shot_seq_ & 1u) ? 0.35f : -0.25f);
                    ev.recoil_roll_deg = -0.8f;
                    ev.PushAudio(AudioCueKind::Mauser91Shot);
                    phase_ = ActionPhase::BoltWaitRelease;
                } else {
                    ev.PushAudio(AudioCueKind::Mauser91DryFire);
                    if (m91_reserve_ > 0) {
                        phase_ = ActionPhase::Reloading;
                        reload_timer_ = 1.75f;
                        m91_reload_close_cue_sent_ = false;
                        ev.PushAudio(AudioCueKind::Mauser91BoltCycle);
                        ev.PushAudio(AudioCueKind::Mauser91StripperClip);
                    }
                }
            }
            break;
        }

        case SpecialWeaponId::MauserC96: {
            if (in.select_fire_pressed) {
                c96_full_auto_ = !c96_full_auto_;
                ev.PushAudio(AudioCueKind::C96BreechClack);
            }
            const bool trigger_req = c96_full_auto_ ? in.fire_held : (in.fire_held && !prev_fire_held_);
            if (trigger_req && fire_cooldown_ <= 0.0f) {
                if (c96_mag_ > 0) {
                    c96_mag_--;
                    shot_seq_++;
                    // ~950 RPM M712 Schnellfeuer cadence (0.06316s interval)
                    fire_cooldown_ = c96_full_auto_ ? 0.06316f : 0.125f;
                    c96_barrel_kick_ = 1.0f;
                    c96_bolt_kick_ = 1.0f;
                    hammer_strike_timer_ = 0.055f;

                    ev.fire_bullet = true;
                    ev.eject_shell = true;
                    ev.bullet_damage = 46.0f;  // 7.63x25mm high-velocity bottle-neck cartridge
                    ev.bullet_velocity_mps = 425.0f;
                    const float ads_scale = 1.0f - 0.50f * ads_blend_;
                    ev.bullet_spread_deg = (0.35f + (c96_full_auto_ ? 0.85f : 0.25f)) * ads_scale;
                    ev.recoil_pitch_deg = (c96_full_auto_ ? 1.45f : 1.75f) * (1.0f - 0.35f * ads_blend_);
                    ev.recoil_yaw_deg = ((shot_seq_ % 3 == 0) ? 0.55f : 0.22f) * (1.0f - 0.4f * ads_blend_);
                    ev.recoil_roll_deg = 0.9f;
                    ev.PushAudio(AudioCueKind::C96Shot);
                    ev.PushAudio(AudioCueKind::C96BreechClack);

                    if (c96_mag_ == 0) {
                        c96_bolt_locked_open_ = true;
                        ev.PushAudio(AudioCueKind::C96BoltLockOpen);
                    }
                } else if (in.fire_pressed) {
                    ev.PushAudio(AudioCueKind::C96DryFire);
                    if (c96_reserve_ > 0) {
                        phase_ = ActionPhase::Reloading;
                        reload_timer_ = 1.55f;
                        c96_reload_close_cue_sent_ = false;
                        ev.PushAudio(AudioCueKind::C96ClipReload);
                    }
                }
            }
            break;
        }

        case SpecialWeaponId::Maxim1910: {
            if (in.fire_held && maxim_mag_ > 0) {
                phase_ = ActionPhase::Firing;
                if (fire_cooldown_ <= 0.0f) {
                    maxim_mag_--;
                    shot_seq_++;
                    // ~550 RPM heavy Maxim cyclic rate (0.1091s), slows slightly if boiling over 100C
                    const bool superheated = maxim_water_temp_c_ >= 100.0f;
                    fire_cooldown_ = superheated ? 0.132f : 0.1091f;
                    maxim_recoil_kick_ = 1.0f;
                    maxim_belt_shift_ = 1.0f;
                    maxim_water_temp_c_ = std::min(104.0f, maxim_water_temp_c_ + 1.45f);

                    ev.fire_bullet = true;
                    ev.eject_shell = true;
                    ev.bullet_damage = 68.0f;  // 7.62x54mmR heavy sustained round
                    ev.bullet_velocity_mps = 840.0f;
                    const float brace = 1.0f - 0.45f * ads_blend_;
                    ev.bullet_spread_deg = (0.28f + 0.50f * brace);
                    ev.recoil_pitch_deg = 0.85f * brace;
                    ev.recoil_yaw_deg = ((shot_seq_ & 1u) ? 0.32f : -0.32f) * brace;
                    ev.recoil_roll_deg = ((shot_seq_ & 1u) ? 0.35f : -0.35f) * brace;
                    ev.PushAudio(AudioCueKind::MaximShot);
                }
            } else {
                phase_ = ActionPhase::Idle;
                if (in.fire_pressed && maxim_mag_ == 0 && maxim_reserve_ > 0) {
                    phase_ = ActionPhase::Reloading;
                    reload_timer_ = 2.15f;
                    ev.PushAudio(AudioCueKind::MaximTopCoverReload);
                }
            }

            // 4.5-liter water jacket boiling & steam plume emissions
            if (maxim_water_temp_c_ >= 96.0f) {
                maxim_water_level_l_ = std::max(0.25f, maxim_water_level_l_ - 0.085f * dt);
                boil_emit_timer_ -= dt;
                if (boil_emit_timer_ <= 0.0f) {
                    boil_emit_timer_ = 0.16f;
                    ev.steam_plume = true;
                    if ((shot_seq_ & 3u) == 0u) {
                        ev.PushAudio(AudioCueKind::MaximSteamHiss);
                    }
                    if ((shot_seq_ & 7u) == 0u) {
                        ev.PushAudio(AudioCueKind::MaximWaterBoil);
                    }
                }
            }
            break;
        }

        default:
            break;
    }

    prev_fire_held_ = in.fire_held;
    return ev;
}

WeaponStatus ArsenalSim::GetStatus() const noexcept {
    return GetStatusFor(active_);
}

WeaponStatus ArsenalSim::GetStatusFor(SpecialWeaponId id) const noexcept {
    WeaponStatus s{};
    s.id = id;
    s.phase = (id == active_) ? phase_ : ActionPhase::Idle;
    s.reloading = (id == active_ && phase_ == ActionPhase::Reloading);
    s.ads_blend = (id == active_) ? ads_blend_ : 0.0f;

    switch (id) {
        case SpecialWeaponId::Flamethrower: {
            s.code = "FLM";
            s.display_name = "M2 FLAMETHROWER";
            s.fuel_pct = fuel_pct_;
            s.mag_ammo = int(std::lround(fuel_pct_));
            s.max_mag_ammo = 100;
            s.reserve_ammo = flame_reserve_pct_;
            s.ignition_cartridges = ignition_cartridges_;
            // Nitrogen constant-pressure regulator stays locked near 350 psi until tank < 2%
            s.regulator_psi = (fuel_pct_ > 2.0f) ? 350.0f : (fuel_pct_ * 175.0f);
            s.nozzle_range_m = (fuel_pct_ > 2.0f) ? 28.5f : (10.0f + fuel_pct_ * 9.0f);
            s.ads_fov_scale = 1.0f - 0.12f * s.ads_blend;
            s.pose.trigger_pull = (s.phase == ActionPhase::Firing || s.phase == ActionPhase::Igniting) ? 1.0f : 0.0f;
            s.pose.safety_lever = 1.0f;
            s.pose.gauge_needle = std::clamp(s.regulator_psi / 400.0f, 0.0f, 1.0f);
            s.pose.pilot_spark = (s.phase == ActionPhase::Igniting) ? 1.0f : 0.35f;
            s.pose.tank_tilt_rad = s.reloading ? 0.28f * std::sin(reload_timer_ * 2.2f) : 0.0f;
            break;
        }
        case SpecialWeaponId::Mauser1891: {
            s.code = "M91";
            s.display_name = "MAUSER 1891-12";
            s.mag_ammo = m91_mag_;
            s.max_mag_ammo = 5;
            s.reserve_ammo = m91_reserve_;
            s.ads_fov_scale = 1.0f - 0.46f * s.ads_blend;  // 1.85x precision iron sights
            s.pose.trigger_pull = (s.phase == ActionPhase::BoltWaitRelease) ? 0.95f : 0.0f;
            s.pose.safety_lever = s.reloading ? 0.0f : 1.0f;
            s.pose.rear_sight_deg = 4.5f * s.ads_blend;
            if (s.phase == ActionPhase::BoltLift) {
                const float t = 1.0f - std::clamp(phase_timer_ / 0.11f, 0.0f, 1.0f);
                s.pose.bolt_lift_deg = 68.0f * t;
            } else if (s.phase == ActionPhase::BoltBack) {
                const float t = 1.0f - std::clamp(phase_timer_ / 0.14f, 0.0f, 1.0f);
                s.pose.bolt_lift_deg = 68.0f;
                s.pose.bolt_retract_m = 0.096f * t;
                s.pose.bolt_stop_deg = -14.0f * t;
            } else if (s.phase == ActionPhase::BoltPushCocking) {
                const float t = std::clamp(phase_timer_ / 0.18f, 0.0f, 1.0f);
                s.pose.bolt_lift_deg = 68.0f;
                s.pose.bolt_retract_m = 0.096f * t;
                s.pose.bolt_stop_deg = -14.0f * t;
            } else if (s.phase == ActionPhase::BoltLock) {
                const float t = std::clamp(phase_timer_ / 0.09f, 0.0f, 1.0f);
                s.pose.bolt_lift_deg = 68.0f * t;
            } else if (s.reloading) {
                // Normalized 78-frame Mauser 1891-12 stripper-clip reload choreography
                const float rt = 1.0f - std::clamp(reload_timer_ / 1.75f, 0.0f, 1.0f);
                auto ramp = [](float a, float b, float x) -> float {
                    if (x <= a) return 0.0f;
                    if (x >= b) return 1.0f;
                    const float u = (x - a) / (b - a);
                    return 0.5f - 0.5f * std::cos(u * 3.14159265f);
                };
                if (rt < 0.13f) {
                    s.pose.bolt_lift_deg = 68.0f * ramp(0.0f, 0.13f, rt);
                } else if (rt < 0.85f) {
                    s.pose.bolt_lift_deg = 68.0f;
                } else {
                    s.pose.bolt_lift_deg = 68.0f * (1.0f - ramp(0.85f, 1.0f, rt));
                }
                if (rt < 0.13f) {
                    s.pose.bolt_retract_m = 0.0f;
                } else if (rt < 0.28f) {
                    s.pose.bolt_retract_m = 0.096f * ramp(0.13f, 0.28f, rt);
                } else if (rt < 0.69f) {
                    s.pose.bolt_retract_m = 0.096f;
                } else if (rt < 0.85f) {
                    s.pose.bolt_retract_m = 0.096f * (1.0f - ramp(0.69f, 0.85f, rt));
                }
                s.pose.bolt_stop_deg = -14.0f * (s.pose.bolt_retract_m / 0.096f);
                if (rt >= 0.24f && rt < 0.69f) {
                    s.pose.clip_insert = std::sin(ramp(0.24f, 0.69f, rt) * 3.14159265f);
                }
                if (rt >= 0.69f && rt < 0.94f) {
                    s.pose.clip_eject = ramp(0.69f, 0.94f, rt);
                }
                s.pose.clip_rounds_left = std::max(0.0f, 5.0f * (1.0f - ramp(0.32f, 0.66f, rt)));
            }
            break;
        }
        case SpecialWeaponId::MauserC96: {
            s.code = "C96";
            s.display_name = "MAUSER C96 M712";
            s.mag_ammo = c96_mag_;
            s.max_mag_ammo = 20;
            s.reserve_ammo = c96_reserve_;
            s.full_auto_mode = c96_full_auto_;
            s.bolt_locked_open = c96_bolt_locked_open_;
            s.ads_fov_scale = 1.0f - 0.20f * s.ads_blend;  // 1.25x tangent rear-sight aim
            s.pose.barrel_recoil_m = c96_barrel_kick_ * 0.0085f;
            s.pose.bolt_retract_m = c96_bolt_locked_open_ ? 0.043f : (c96_bolt_kick_ * 0.043f);
            s.pose.bolt_locked_open = c96_bolt_locked_open_;
            s.pose.hammer_cocked = (hammer_strike_timer_ > 0.0f) ? 0.0f : 1.0f;
            s.pose.trigger_pull = (hammer_strike_timer_ > 0.0f) ? 0.85f : 0.0f;
            s.pose.safety_lever = c96_full_auto_ ? 1.0f : 0.0f;
            s.pose.rear_sight_deg = 6.0f * s.ads_blend;
            if (s.reloading) {
                // Normalized 78-frame Mauser C96 M712 stripper-clip & bolt slam choreography
                const float rt = 1.0f - std::clamp(reload_timer_ / 1.55f, 0.0f, 1.0f);
                auto ramp = [](float a, float b, float x) -> float {
                    if (x <= a) return 0.0f;
                    if (x >= b) return 1.0f;
                    const float u = (x - a) / (b - a);
                    return 0.5f - 0.5f * std::cos(u * 3.14159265f);
                };
                if (rt < 0.13f) {
                    s.pose.bolt_retract_m = 0.043f * ramp(0.0f, 0.13f, rt);
                } else if (rt < 0.78f) {
                    s.pose.bolt_retract_m = 0.043f;
                } else if (rt < 0.90f) {
                    s.pose.bolt_retract_m = 0.043f * (1.0f - ramp(0.78f, 0.90f, rt));
                } else {
                    s.pose.bolt_retract_m = 0.0f;
                }
                s.pose.bolt_locked_open = (rt >= 0.08f && rt < 0.82f);
                if (rt >= 0.18f && rt < 0.76f) {
                    s.pose.clip_insert = std::sin(ramp(0.18f, 0.76f, rt) * 3.14159265f);
                }
                if (rt < 0.25f) {
                    s.pose.hammer_cocked = 1.0f - ramp(0.0f, 0.15f, rt);
                } else if (rt >= 0.80f) {
                    s.pose.hammer_cocked = ramp(0.80f, 0.92f, rt);
                } else {
                    s.pose.hammer_cocked = 0.0f;
                }
            }
            break;
        }
        case SpecialWeaponId::Maxim1910: {
            s.code = "MXM";
            s.display_name = "MAXIM PM M1910";
            s.mag_ammo = maxim_mag_;
            s.max_mag_ammo = 250;
            s.reserve_ammo = maxim_reserve_;
            s.full_auto_mode = true;
            s.water_temp_c = maxim_water_temp_c_;
            s.water_level_l = maxim_water_level_l_;
            s.water_boiling = (maxim_water_temp_c_ >= 96.0f);
            s.ads_fov_scale = 1.0f - 0.285f * s.ads_blend;  // 1.40x braced Sokolov shield aim
            s.pose.wheel_angle_rad = maxim_wheel_angle_rad_;
            s.pose.spade_thumb_press = (s.phase == ActionPhase::Firing) ? 1.0f : 0.0f;
            s.pose.belt_shift = maxim_belt_shift_;
            s.pose.barrel_recoil_m = maxim_recoil_kick_ * 0.012f;
            s.pose.steam_vent = s.water_boiling ? std::clamp((maxim_water_temp_c_ - 94.0f) / 10.0f, 0.25f, 1.0f) : 0.0f;
            if (s.reloading) {
                const float rt = 1.0f - std::clamp(reload_timer_ / 2.15f, 0.0f, 1.0f);
                s.pose.top_cover_deg = 78.0f * std::sin(rt * 3.14159f);
            }
            break;
        }
        default:
            break;
    }
    return s;
}

}  // namespace game::special_arsenal
