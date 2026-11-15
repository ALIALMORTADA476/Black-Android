// Delirium Physics Runtime Integration for BLACK Enhanced Edition
// Bridges platform-independent game::delirium::Simulation with live Xbox x86
// guest memory, procedural stagger/clutching overlays, blind-fire scatter,
// varied ragdoll collapse vectors, and post-death agony bone impulses.
#include "game/delirium_physics.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "game/ballistics_fx.h"
#include "game/blood_decal_rules.h"
#include "game/delirium_rules.h"
#include "game/layout.h"
#include "game/physics_lab.h"
#include "game/time_stop.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;

X86_EXTERN_FUNC(__imp__sub_000DC960);
X86_EXTERN_FUNC(__imp__sub_0017F270);
X86_EXTERN_FUNC(__imp__sub_00195920);
X86_EXTERN_FUNC(__imp__sub_001DB910);

namespace game::delirium_phys {
namespace {

constexpr uint32_t kCharPosOffset = 0xA0u;
constexpr uint32_t kCharKindOffset = 0xC4u;
constexpr uint32_t kCharRightVecOffset = 0xF0u;
constexpr uint32_t kCharOverlayMgrOffset = 0x328u;
constexpr uint32_t kCharBodyOffset = 0x324u;
constexpr uint32_t kCharDyingOffset = 0x384u;
constexpr uint32_t kPlayerKind = 2u;

std::mutex g_sim_mutex;
game::delirium::Simulation g_sim;
uint32_t g_last_lethal_victim = 0;

inline bool IsValidGuestAddr(uint32_t addr, uint32_t span = 16u) noexcept {
    if (addr < mem::kUserLow || span == 0u ||
        addr > (0xFFFFFFFFu - (span - 1u))) {
        return false;
    }
    return mem::IsCommitted(addr) && mem::IsCommitted(addr + span - 1u);
}

struct WoundDripSlot {
    uint32_t who = 0;
    float accum = 0.0f;
};
constexpr size_t kMaxDripSlots = 32;
WoundDripSlot g_drip_slots[kMaxDripSlots]{};

void StepWoundBloodDrip(uint32_t who, float dt, float intensity) noexcept {
    if (!who || dt <= 0.0f || intensity <= 0.02f) return;
    if (!IsValidGuestAddr(who + kCharPosOffset, 12u)) return;
    const float* pos = &G<float>(who + kCharPosOffset);
    if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) return;

    const float interval = game::blood_decals::ComputeWoundDripIntervalSec(intensity);
    size_t free_idx = kMaxDripSlots;
    size_t idx = kMaxDripSlots;
    for (size_t i = 0; i < kMaxDripSlots; ++i) {
        if (g_drip_slots[i].who == who) { idx = i; break; }
        if (free_idx == kMaxDripSlots && g_drip_slots[i].who == 0u) free_idx = i;
    }
    if (idx == kMaxDripSlots) {
        idx = (free_idx < kMaxDripSlots) ? free_idx : (who % kMaxDripSlots);
        g_drip_slots[idx] = {who, 0.0f};
    }
    g_drip_slots[idx].accum += dt;
    if (g_drip_slots[idx].accum >= interval) {
        g_drip_slots[idx].accum -= interval;
        const float drip_pos[3] = {pos[0], pos[1] + 0.85f, pos[2]};
        game::ballistics::EmitWoundDrip(drip_pos, pos[1] + 0.024f);
    }
}

inline uint32_t PlayerEa() noexcept {
    if (!IsValidGuestAddr(kWorld, 4u)) return 0;
    const uint32_t world = R32(kWorld);
    return world ? (world + kWorldPlayer) : 0;
}

inline bool IsNonPlayerSoldier(uint32_t ea) noexcept {
    if (!ea || !IsValidGuestAddr(ea, 0x390u)) return false;
    if (ea == PlayerEa()) return false;
    return R32(ea + kCharKindOffset) != kPlayerKind;
}

inline game::delirium::Config ReadConfigFromLab() noexcept {
    const auto& p = game::physics_lab::Params();
    game::delirium::Config cfg;
    cfg.enabled = p.delirium_enabled.load(std::memory_order_relaxed);
    cfg.intensity = p.delirium_intensity.load(std::memory_order_relaxed);
    cfg.blind_fire_enabled =
        p.delirium_blind_fire.load(std::memory_order_relaxed);
    cfg.agony_duration =
        p.delirium_agony_duration.load(std::memory_order_relaxed);
    return cfg;
}

void DriveClutchAndAnim(X86Context& ctx, uint8_t* base, uint32_t who,
                        const game::delirium::StepOutput& step) noexcept {
    if (!IsValidGuestAddr(ctx.esp - 0x40u, 0x50u)) return;

    // 1. Trigger upper-body wound-clutch / flinch reaction via sub_0017F270
    // Guarded identically to sub_001E74B0 (x86_recomp.51.cpp:19849-19900):
    // +0x0C in the op=5 packet is an INTEGER hit-zone index (0..10), NEVER a float!
    const uint32_t kind_obj = R32(who + 0x320u);
    const bool allow_overlay =
        IsValidGuestAddr(kind_obj, 8u) && R32(kind_obj + 4u) != 0x28u;
    const uint32_t overlay_mgr = R32(who + kCharOverlayMgrOffset);
    if (allow_overlay && IsValidGuestAddr(overlay_mgr, 0x58u) &&
        R32(overlay_mgr) == who) {
        const uint32_t vtbl_obj = R32(overlay_mgr + 0x50u);
        const uint32_t gate_obj = R32(overlay_mgr + 0x54u);
        if (IsValidGuestAddr(vtbl_obj, 4u) &&
            IsValidGuestAddr(R32(vtbl_obj), 0x30u) &&
            IsValidGuestAddr(gate_obj, 8u) && R32(gate_obj + 4u) != 0u) {
            static const uint32_t kOverlayScratch = mem::AllocSystem(0x20u, 16u);
            if (kOverlayScratch) {
                uint8_t* p = static_cast<uint8_t*>(HostPtr(kOverlayScratch));
                std::memset(p, 0, 0x18u);
                const uint32_t op = 5u;
                const uint32_t channel =
                    (step.overlay_variant == 0x2Au) ? 0x2Au : 0x29u;
                const float amp = std::clamp(step.overlay_weight, 0.20f, 1.35f);
                const uint32_t zone_idx =
                    (step.anim_clip_id == 0x1Fu) ? 7u : 1u;
                const uint8_t side_flag =
                    (step.lean_yaw_bias < 0.0f) ? 1u : 0u;
                std::memcpy(p + 0x00u, &op, 4u);
                std::memcpy(p + 0x04u, &channel, 4u);
                std::memcpy(p + 0x08u, &amp, 4u);
                std::memcpy(p + 0x0Cu, &zone_idx, 4u);
                p[0x10u] = side_flag;

                X86Context tmp = ctx;
                tmp.esp -= 4u;
                *X86_PTR(uint32_t, tmp.esp) = 0u;  // dummy return for ret
                tmp.ecx = overlay_mgr;
                tmp.eax = kOverlayScratch;
                __imp__sub_0017F270(tmp, base);
            }
        }
    }

    // 2. Trigger procedural hit clip (0x1E torso / 0x1F limb) on mover+0xC90
    const uint32_t mover = R32(who + kCharBodyOffset);
    if (IsValidGuestAddr(mover, 0xCD0u) && R32(mover + 0x90u) != 2u) {
        X86Context tmp = ctx;
        auto push32 = [&](uint32_t v) {
            tmp.esp -= 4u;
            *X86_PTR(uint32_t, tmp.esp) = v;
        };
        push32(0x3F800000u);  // 1.0f blend weight
        push32(0u);
        push32(0u);
        push32(0u);  // dummy return address popped by ret 0x0C
        tmp.ecx = step.anim_clip_id;
        tmp.eax = mover + 0xC90u;
        __imp__sub_00195920(tmp, base);
    }
}

void DriveAgonyPulse(X86Context& ctx, uint8_t* base, uint32_t who,
                     const game::delirium::AgonyPulse& pulse) noexcept {
    if (!pulse.active || pulse.bone_idx > 10u) return;
    if (!IsValidGuestAddr(ctx.esp - 0x40u, 0x50u)) return;
    const uint32_t body = R32(who + kCharBodyOffset);
    if (!IsValidGuestAddr(body, 0x94u) || R32(body + 0x90u) != 2u) return;

    const uint32_t table_addr = pulse.bone_idx * 4u + 0x28B2B4u;
    if (!IsValidGuestAddr(table_addr, 4u)) return;
    const uint32_t slot = R32(table_addr);
    if (slot >= 24u) return;

    const uint32_t inner1 = R32(body + 0x0Cu);
    if (!IsValidGuestAddr(inner1, 0x10u)) return;
    const uint32_t inner2 = R32(inner1 + 0x0Cu);
    if (!IsValidGuestAddr(inner2, slot * 0x70u + 0x60u)) return;
    const uint32_t bone_rb = R32(slot * 0x70u + inner2 + 0x58u);
    if (!IsValidGuestAddr(bone_rb, 0xB0u)) return;
    const uint32_t mat_ptr = R32(bone_rb + 0x4Cu);
    if (!IsValidGuestAddr(mat_ptr, 0x388u)) return;

    static const uint32_t kDirScratch = mem::AllocSystem(0x10u, 16u);
    static const uint32_t kPosScratch = mem::AllocSystem(0x10u, 16u);
    if (!kDirScratch || !kPosScratch) return;

    float* dir = static_cast<float*>(HostPtr(kDirScratch));
    dir[0] = pulse.impulse_dir[0];
    dir[1] = pulse.impulse_dir[1];
    dir[2] = pulse.impulse_dir[2];
    dir[3] = 0.0f;

    float* pos = static_cast<float*>(HostPtr(kPosScratch));
    const float* world_pos = &G<float>(who + kCharPosOffset);
    if (!std::isfinite(world_pos[0]) || !std::isfinite(world_pos[1]) ||
        !std::isfinite(world_pos[2])) {
        return;
    }
    pos[0] = world_pos[0];
    pos[1] = world_pos[1] + 0.35f;
    pos[2] = world_pos[2];
    pos[3] = 0.0f;

    uint32_t force_bits = 0;
    const float f = std::clamp(pulse.impulse_force, 0.05f, 2.20f);
    std::memcpy(&force_bits, &f, 4u);

    X86Context tmp = ctx;
    auto push32 = [&](uint32_t v) {
        tmp.esp -= 4u;
        *X86_PTR(uint32_t, tmp.esp) = v;
    };
    push32(force_bits);
    push32(kPosScratch);
    push32(pulse.bone_idx);
    push32(body);
    push32(0u);  // dummy return address popped by ret 0x10
    tmp.eax = kDirScratch;
    __imp__sub_000DC960(tmp, base);
}

}  // namespace

void OnBulletHit(const X86Context&, uint8_t* base, uint32_t victim, uint32_t,
                 uint32_t hit_dir_ea, float force_val,
                 uint8_t zone_byte) noexcept {
    if (!IsNonPlayerSoldier(victim)) return;

    const auto cfg = ReadConfigFromLab();
    if (!cfg.enabled) return;

    game::delirium::HitEvent ev;
    ev.victim_ea = victim;
    ev.raw_force = std::isfinite(force_val) ? std::max(0.0f, force_val) : 1.0f;
    ev.raw_zone = zone_byte;
    ev.is_lethal = (R32(victim + kCharDyingOffset) != 0u);

    if (IsValidGuestAddr(hit_dir_ea, 12u)) {
        const float* d = X86_PTR(const float, hit_dir_ea);
        if (std::isfinite(d[0]) && std::isfinite(d[1]) && std::isfinite(d[2])) {
            ev.hit_dir[0] = d[0];
            ev.hit_dir[1] = d[1];
            ev.hit_dir[2] = d[2];
        }
    }

    if (IsValidGuestAddr(victim + kCharRightVecOffset, 12u)) {
        const float* right = X86_PTR(const float, victim + kCharRightVecOffset);
        if (std::isfinite(right[0]) && std::isfinite(right[1]) &&
            std::isfinite(right[2])) {
            ev.lateral_dot = right[0] * ev.hit_dir[0] +
                             right[1] * ev.hit_dir[1] +
                             right[2] * ev.hit_dir[2];
        }
    }

    std::lock_guard<std::mutex> lock(g_sim_mutex);
    g_sim.SetConfig(cfg);
    g_sim.OnBulletHit(ev);
    if (ev.is_lethal) {
        g_last_lethal_victim = victim;
    }
}

void OnEnemyFrameStep(X86Context& ctx, uint8_t* base, uint32_t who,
                      float dt) noexcept {
    if (game::time_stop::IsFrozen()) return;
    if (!IsNonPlayerSoldier(who)) return;

    const auto cfg = ReadConfigFromLab();
    if (!cfg.enabled) return;

    const float clamped_dt =
        std::isfinite(dt) ? std::clamp(dt, 0.0f, 0.05f) : 0.016f;
    if (clamped_dt <= 1e-5f) return;

    const bool is_dying = (R32(who + kCharDyingOffset) != 0u);
    if (!is_dying) {
        game::delirium::StepOutput step{};
        {
            std::lock_guard<std::mutex> lock(g_sim_mutex);
            g_sim.SetConfig(cfg);
            step = g_sim.StepAlive(who, clamped_dt);
        }
        if (step.apply_stumble_vel) {
            float* pos = &G<float>(who + kCharPosOffset);
            if (std::isfinite(pos[0]) && std::isfinite(pos[2])) {
                pos[0] += step.stumble_vel[0] * clamped_dt;
                pos[2] += step.stumble_vel[2] * clamped_dt;
            }
        }
        if (step.trigger_clutch_overlay) {
            DriveClutchAndAnim(ctx, base, who, step);
        }
        if (step.apply_stumble_vel || step.blind_fire_ready) {
            StepWoundBloodDrip(who, clamped_dt,
                               step.apply_stumble_vel ? 0.85f : 0.45f);
        }
    } else if (cfg.agony_duration > 1e-3f) {
        const uint32_t body = R32(who + kCharBodyOffset);
        if (!IsValidGuestAddr(body, 0x94u) || R32(body + 0x90u) != 2u) {
            return;
        }
        game::delirium::AgonyPulse pulse{};
        {
            std::lock_guard<std::mutex> lock(g_sim_mutex);
            g_sim.SetConfig(cfg);
            pulse = g_sim.StepAgony(who, clamped_dt);
        }
        if (pulse.active) {
            DriveAgonyPulse(ctx, base, who, pulse);
            StepWoundBloodDrip(who, clamped_dt, 0.55f);
        }
    }
}

void TriggerExternalAgony(X86Context& ctx, uint8_t* base, uint32_t who,
                          uint32_t clip_id) noexcept {
    if (!IsNonPlayerSoldier(who)) return;
    if (R32(who + kCharDyingOffset) != 0u) return;
    game::delirium::StepOutput step{};
    step.trigger_clutch_overlay = true;
    step.overlay_weight = 0.92f;
    step.anim_clip_id = clip_id;
    DriveClutchAndAnim(ctx, base, who, step);
}

bool ApplyDeathCollapseVariation(uint32_t approx_owner,
                                 X86Xmm& vel_inout) noexcept {
    const auto cfg = ReadConfigFromLab();
    if (!cfg.enabled) return false;

    game::delirium::CollapseProfile prof{};
    {
        std::lock_guard<std::mutex> lock(g_sim_mutex);
        g_sim.SetConfig(cfg);
        if (approx_owner) {
            prof = g_sim.ConsumeCollapseProfile(approx_owner);
        }
        if (!prof.valid && g_last_lethal_victim) {
            prof = g_sim.ConsumeCollapseProfile(g_last_lethal_victim);
            g_last_lethal_victim = 0;
        }
    }
    if (!prof.valid) return false;

    if (std::isfinite(vel_inout.f[0])) {
        vel_inout.f[0] =
            std::clamp(vel_inout.f[0] + prof.launch_vel[0], -35.0f, 35.0f);
    }
    if (std::isfinite(vel_inout.f[1])) {
        vel_inout.f[1] = std::clamp(
            vel_inout.f[1] + prof.launch_vel[1] + prof.upward_lift, -25.0f,
            35.0f);
    }
    if (std::isfinite(vel_inout.f[2])) {
        vel_inout.f[2] =
            std::clamp(vel_inout.f[2] + prof.launch_vel[2], -35.0f, 35.0f);
    }
    return true;
}

void ResetAll() noexcept {
    std::lock_guard<std::mutex> lock(g_sim_mutex);
    g_sim.Reset();
    g_last_lethal_victim = 0;
    std::memset(g_drip_slots, 0, sizeof(g_drip_slots));
}

}  // namespace game::delirium_phys

// AI firing decision/trace wrapper (sub_001DB910, x86_recomp.50.cpp:16871).
// When a non-player enemy tries to return fire while stumbling off-balance,
// redirect `edi` (target position descriptor) to a private guest scratch copy
// with scattered aim coordinates so caller/player state is never mutated.
X86_FUNC(sub_001DB910) {
    uint32_t orig_edi = ctx.edi;
    bool redirected = false;

    if (!game::time_stop::IsFrozen() &&
        game::delirium_phys::IsValidGuestAddr(ctx.esp + 0x04u, 4u) &&
        game::delirium_phys::IsValidGuestAddr(orig_edi, 0x30u)) {
        const auto cfg = game::delirium_phys::ReadConfigFromLab();
        if (cfg.enabled && cfg.blind_fire_enabled) {
            const uint32_t ctrl = R32(ctx.esp + 0x04u);
            if (game::delirium_phys::IsValidGuestAddr(ctrl, 0x08u)) {
                const uint32_t ai = R32(ctrl + 0x04u);
                if (game::delirium_phys::IsValidGuestAddr(ai, 0xF4u)) {
                    const uint32_t shooter = R32(ai + 0xF0u);
                    if (game::delirium_phys::IsNonPlayerSoldier(shooter)) {
                        float target[3] = {0.0f, 0.0f, 0.0f};
                        const float* src_f = X86_PTR(const float, orig_edi);
                        target[0] = src_f[0];
                        target[1] = src_f[1];
                        target[2] = src_f[2];
                        if (std::isfinite(target[0]) &&
                            std::isfinite(target[1]) &&
                            std::isfinite(target[2])) {
                            bool applied = false;
                            {
                                std::lock_guard<std::mutex> lock(
                                    game::delirium_phys::g_sim_mutex);
                                game::delirium_phys::g_sim.SetConfig(cfg);
                                applied = game::delirium_phys::g_sim
                                              .ApplyBlindFireScatter(shooter,
                                                                     target);
                            }
                            if (applied) {
                                static const uint32_t kBlindTargetScratch =
                                    mem::AllocSystem(0x40u, 16u);
                                if (kBlindTargetScratch) {
                                    std::memcpy(HostPtr(kBlindTargetScratch),
                                                HostPtr(orig_edi), 0x30u);
                                    float* dst_f = static_cast<float*>(
                                        HostPtr(kBlindTargetScratch));
                                    dst_f[0] = target[0];
                                    dst_f[1] = target[1];
                                    dst_f[2] = target[2];
                                    ctx.edi = kBlindTargetScratch;
                                    redirected = true;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    __imp__sub_001DB910(ctx, base);
    if (redirected) {
        ctx.edi = orig_edi;
    }
}
