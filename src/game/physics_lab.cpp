// Stage 6 PHYSICS LAB — Runtime Guest Physics Hooks & Non-Compounding
// Multipliers
#include "game/physics_lab.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "common/mod_schema.h"
#include "common/options.h"
#include "core/log.h"
#include "game/combat_feedback_rules.h"
#include "game/delirium_physics.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;

namespace game::physics_lab {
namespace {

constexpr uint32_t kPhysics = 0x2D45F4;
constexpr uint32_t kSolverWorld = 0x18;
constexpr uint32_t kPhysicsGravity = 0x360;
constexpr uint32_t kPhysicsGravityStep = 0x370;
constexpr uint32_t kPhysicsStep = 0x380;

RuntimeParams g_params;

std::atomic<uint64_t> g_blast_hits{0};
std::atomic<uint64_t> g_impact_hits{0};
std::atomic<uint64_t> g_gravity_steps{0};
std::atomic<uint64_t> g_debris_steps{0};
std::atomic<uint64_t> g_ragdoll_steps{0};
std::atomic<uint64_t> g_recoil_kicks{0};
std::atomic<float> g_active_gravity_y{-25.0f};

// Gravity baseline tracking so scaled gravity never compounds frame-over-frame.
uint32_t g_last_world = 0;
float g_base_gravity_y = -25.0f;
float g_written_gravity_y = -25.0f;
bool g_gravity_overridden = false;
float g_prev_health_ratio = 1.0f;
float g_damage_flash = 0.0f;

float SanitizeMultiplier(float val, float lo, float hi, float fallback) {
  if (!std::isfinite(val))
    return fallback;
  return std::clamp(val, lo, hi);
}

void MaybeLogSummary(bool force = false) {
  static auto s_last_log =
      std::chrono::steady_clock::now() - std::chrono::seconds(10);
  const auto now = std::chrono::steady_clock::now();
  if (!force &&
      std::chrono::duration_cast<std::chrono::seconds>(now - s_last_log)
              .count() < 5) {
    return;
  }
  s_last_log = now;
  const auto &p = g_params;
  LOGR("[PHYSICS_LAB] exp=%.2fx rad=%.2fx imp=%.2fx lnc=%.2fx rag=%.2fx "
       "str=%.2fx deb=%.2fx bnc=%.2fx grav=%.2fx(y=%.1f) tms=%.2fx shk=%.2fx "
       "rec=%.2fx | "
       "hits: blast=%llu impact=%llu grav=%llu deb=%llu rag=%llu rec=%llu",
       p.explosion_force.load(std::memory_order_relaxed),
       p.explosion_radius.load(std::memory_order_relaxed),
       p.impact_force.load(std::memory_order_relaxed),
       p.ragdoll_launch.load(std::memory_order_relaxed),
       p.character_reaction.load(std::memory_order_relaxed),
       p.structural_destruction.load(std::memory_order_relaxed),
       p.debris_intensity.load(std::memory_order_relaxed),
       p.prop_bounciness.load(std::memory_order_relaxed),
       p.gravity_scale.load(std::memory_order_relaxed),
       g_active_gravity_y.load(std::memory_order_relaxed),
       p.physics_time_scale.load(std::memory_order_relaxed),
       p.camera_shockwave.load(std::memory_order_relaxed),
       p.weapon_recoil.load(std::memory_order_relaxed),
       static_cast<unsigned long long>(
           g_blast_hits.load(std::memory_order_relaxed)),
       static_cast<unsigned long long>(
           g_impact_hits.load(std::memory_order_relaxed)),
       static_cast<unsigned long long>(
           g_gravity_steps.load(std::memory_order_relaxed)),
       static_cast<unsigned long long>(
           g_debris_steps.load(std::memory_order_relaxed)),
       static_cast<unsigned long long>(
           g_ragdoll_steps.load(std::memory_order_relaxed)),
       static_cast<unsigned long long>(
           g_recoil_kicks.load(std::memory_order_relaxed)));
}

} // namespace

RuntimeParams &Params() { return g_params; }

void SyncFromEnhanced(const modsettings::EnhancedOptions &enh) {
  const float exp_f =
      SanitizeMultiplier(enh.physics_explosion_force, 0.25f, 10.00f, 1.0f);
  const float imp_f =
      SanitizeMultiplier(enh.physics_impact_force, 0.25f, 10.00f, 1.0f);
  const float grv_f =
      SanitizeMultiplier(enh.physics_gravity_scale, 0.05f, 5.00f, 1.0f);
  const float deb_f =
      SanitizeMultiplier(enh.physics_debris_intensity, 0.25f, 6.00f, 1.0f);
  const float rag_f =
      SanitizeMultiplier(enh.physics_character_reaction, 0.25f, 8.00f, 1.0f);
  const float rec_f =
      SanitizeMultiplier(enh.physics_weapon_recoil, 0.00f, 4.00f, 1.0f);
  const float rad_f =
      SanitizeMultiplier(enh.physics_explosion_radius, 0.50f, 5.00f, 1.0f);
  const float lnc_f =
      SanitizeMultiplier(enh.physics_ragdoll_launch, 0.25f, 10.00f, 1.0f);
  const float str_f = SanitizeMultiplier(enh.physics_structural_destruction,
                                         0.25f, 10.00f, 1.0f);
  const float bnc_f =
      SanitizeMultiplier(enh.physics_prop_bounciness, 0.25f, 6.00f, 1.0f);
  const float shk_f =
      SanitizeMultiplier(enh.physics_camera_shockwave, 0.00f, 4.00f, 1.0f);
  const float tms_f =
      SanitizeMultiplier(enh.physics_time_scale, 0.20f, 2.00f, 1.0f);

  const bool changed =
      std::fabs(g_params.explosion_force.load(std::memory_order_relaxed) -
                exp_f) > 1e-4f ||
      std::fabs(g_params.impact_force.load(std::memory_order_relaxed) - imp_f) >
          1e-4f ||
      std::fabs(g_params.gravity_scale.load(std::memory_order_relaxed) -
                grv_f) > 1e-4f ||
      std::fabs(g_params.debris_intensity.load(std::memory_order_relaxed) -
                deb_f) > 1e-4f ||
      std::fabs(g_params.character_reaction.load(std::memory_order_relaxed) -
                rag_f) > 1e-4f ||
      std::fabs(g_params.weapon_recoil.load(std::memory_order_relaxed) -
                rec_f) > 1e-4f ||
      std::fabs(g_params.explosion_radius.load(std::memory_order_relaxed) -
                rad_f) > 1e-4f ||
      std::fabs(g_params.ragdoll_launch.load(std::memory_order_relaxed) -
                lnc_f) > 1e-4f ||
      std::fabs(
          g_params.structural_destruction.load(std::memory_order_relaxed) -
          str_f) > 1e-4f ||
      std::fabs(g_params.prop_bounciness.load(std::memory_order_relaxed) -
                bnc_f) > 1e-4f ||
      std::fabs(g_params.camera_shockwave.load(std::memory_order_relaxed) -
                shk_f) > 1e-4f ||
      std::fabs(g_params.physics_time_scale.load(std::memory_order_relaxed) -
                tms_f) > 1e-4f;

  g_params.explosion_force.store(exp_f, std::memory_order_relaxed);
  g_params.impact_force.store(imp_f, std::memory_order_relaxed);
  g_params.gravity_scale.store(grv_f, std::memory_order_relaxed);
  g_params.debris_intensity.store(deb_f, std::memory_order_relaxed);
  g_params.character_reaction.store(rag_f, std::memory_order_relaxed);
  g_params.weapon_recoil.store(rec_f, std::memory_order_relaxed);
  g_params.explosion_radius.store(rad_f, std::memory_order_relaxed);
  g_params.ragdoll_launch.store(lnc_f, std::memory_order_relaxed);
  g_params.structural_destruction.store(str_f, std::memory_order_relaxed);
  g_params.prop_bounciness.store(bnc_f, std::memory_order_relaxed);
  g_params.camera_shockwave.store(shk_f, std::memory_order_relaxed);
  g_params.physics_time_scale.store(tms_f, std::memory_order_relaxed);
  g_params.delirium_enabled.store(enh.delirium_physics_enabled,
                                  std::memory_order_relaxed);
  g_params.delirium_intensity.store(
      SanitizeMultiplier(enh.delirium_intensity, 0.25f, 3.00f, 1.35f),
      std::memory_order_relaxed);
  g_params.delirium_blind_fire.store(enh.delirium_blind_fire,
                                     std::memory_order_relaxed);
  g_params.delirium_agony_duration.store(
      SanitizeMultiplier(enh.delirium_agony_duration, 0.00f, 2.50f, 1.10f),
      std::memory_order_relaxed);

  if (changed)
    MaybeLogSummary(true);
}

TelemetrySnapshot GetTelemetry() {
  TelemetrySnapshot t;
  t.blast_hits = g_blast_hits.load(std::memory_order_relaxed);
  t.impact_hits = g_impact_hits.load(std::memory_order_relaxed);
  t.gravity_steps = g_gravity_steps.load(std::memory_order_relaxed);
  t.debris_steps = g_debris_steps.load(std::memory_order_relaxed);
  t.ragdoll_steps = g_ragdoll_steps.load(std::memory_order_relaxed);
  t.recoil_kicks = g_recoil_kicks.load(std::memory_order_relaxed);
  t.active_gravity_y = g_active_gravity_y.load(std::memory_order_relaxed);
  return t;
}

} // namespace game::physics_lab

using game::physics_lab::g_active_gravity_y;
using game::physics_lab::g_base_gravity_y;
using game::physics_lab::g_blast_hits;
using game::physics_lab::g_debris_steps;
using game::physics_lab::g_gravity_overridden;
using game::physics_lab::g_gravity_steps;
using game::physics_lab::g_impact_hits;
using game::physics_lab::g_last_world;
using game::physics_lab::g_params;
using game::physics_lab::g_ragdoll_steps;
using game::physics_lab::g_recoil_kicks;
using game::physics_lab::g_written_gravity_y;
using game::physics_lab::kPhysics;
using game::physics_lab::kPhysicsGravity;
using game::physics_lab::kPhysicsGravityStep;
using game::physics_lab::kPhysicsStep;
using game::physics_lab::kSolverWorld;
using game::physics_lab::MaybeLogSummary;

namespace {

inline float TimeCompensatedScale(float s, float extra_gain = 1.6f) noexcept {
  const float tms = g_params.physics_time_scale.load(std::memory_order_relaxed);
  if (std::fabs(s - 1.0f) <= 1e-4f && std::fabs(tms - 1.0f) <= 1e-4f)
    return 1.0f;
  const float effective_s = (s > 1.0f) ? (1.0f + (s - 1.0f) * extra_gain) : s;
  const float dt_comp = (tms < 0.99f && tms > 0.05f) ? (1.0f / tms) : 1.0f;
  return effective_s * dt_comp;
}

inline void ApplyExplosionImpulse(X86Context &ctx) {
  g_blast_hits.fetch_add(1, std::memory_order_relaxed);
  const float s = TimeCompensatedScale(
      g_params.explosion_force.load(std::memory_order_relaxed), 2.4f);
  if (std::fabs(s - 1.0f) > 1e-4f && std::isfinite(ctx.xmm[0].f[0])) {
    ctx.xmm[0].f[0] *= s;
  }
  MaybeLogSummary();
}

inline void ApplyBulletImpactImpulse(X86Context &ctx) {
  g_impact_hits.fetch_add(1, std::memory_order_relaxed);
  const float s = TimeCompensatedScale(
      g_params.impact_force.load(std::memory_order_relaxed), 2.4f);
  if (std::fabs(s - 1.0f) > 1e-4f && std::isfinite(ctx.xmm[0].f[0])) {
    ctx.xmm[0].f[0] *= s;
  }
  MaybeLogSummary();
}

inline void ScaleFloatBits(uint32_t &bits, float scale) {
  if (std::fabs(scale - 1.0f) <= 1e-4f)
    return;
  float f = 0.0f;
  std::memcpy(&f, &bits, sizeof(f));
  if (!std::isfinite(f))
    return;
  f *= scale;
  std::memcpy(&bits, &f, sizeof(bits));
}

inline bool IsValidGuestAddr(uint32_t addr, uint32_t span = 16u) noexcept {
  if (addr < mem::kUserLow || span == 0u || addr > (0xFFFFFFFFu - (span - 1u)))
    return false;
  return mem::IsCommitted(addr) && mem::IsCommitted(addr + span - 1u);
}

// Scales 3D velocity/impulse vectors allocated on the Xbox stack right before
// sub_000DC070/DC100 calls.
inline void ScalePushedVec3(const X86Context &ctx, float scale,
                            float lift_factor = 0.0f) {
  const float eff_scale = TimeCompensatedScale(scale, 1.75f);
  if (std::fabs(eff_scale - 1.0f) <= 1e-4f && lift_factor <= 1e-4f)
    return;
  if (!IsValidGuestAddr(ctx.esp, 4u))
    return;
  const uint32_t ptr = R32(ctx.esp);
  if ((ptr & 0xF0000000u) != 0xD0000000u || !IsValidGuestAddr(ptr, 12u))
    return; // Stack-local vec3 only; never mutate globals.
  for (uint32_t i = 0; i < 3; i++) {
    float &v = G<float>(ptr + i * 4u);
    if (std::isfinite(v))
      v *= eff_scale;
  }
  if (lift_factor > 1e-4f) {
    const float vx = G<float>(ptr + 0u);
    float &vy = G<float>(ptr + 4u);
    const float vz = G<float>(ptr + 8u);
    if (std::isfinite(vx) && std::isfinite(vy) && std::isfinite(vz)) {
      const float horiz = std::sqrt(vx * vx + vz * vz);
      const float lift =
          std::min(horiz * 0.30f + 2.0f, 35.0f) * std::min(lift_factor, 8.0f);
      vy += lift;
    }
  }
}

inline void ApplyRecoilKick(X86Context &ctx, uint32_t profile_reg) {
  const uint64_t shot_seq =
      g_recoil_kicks.fetch_add(1, std::memory_order_relaxed);
  const bool no_rec = game::Opt().no_recoil;
  const bool enh_rec =
      modsettings::Registry().GetEnhanced().enhanced_recoil_feedback;
  float yaw_kick_deg = 0.0f;
  const float punch = game::combat_feedback::ComputeEnhancedRecoilKickScale(
      enh_rec, no_rec, shot_seq, yaw_kick_deg);
  const float base_r =
      no_rec ? 0.0f : g_params.weapon_recoil.load(std::memory_order_relaxed);
  const float r = no_rec ? 0.0f : (base_r * punch);
  if (std::fabs(r - 1.0f) < 1e-4f)
    return;

  if (r <= 1e-4f) {
    ctx.xmm[0].f[0] = 0.0f;
    ctx.xmm[1].f[0] = 0.0f;
    if (IsValidGuestAddr(ctx.eax, 0x4D0u))
      G<float>(ctx.eax + 0x4C4u) = 0.0f;
    MaybeLogSummary();
    return;
  }

  if (IsValidGuestAddr(profile_reg, 0x70u) &&
      IsValidGuestAddr(ctx.eax, 0x4D0u)) {
    const float target = G<float>(profile_reg + 0x68u) * r;
    const float cur = G<float>(ctx.eax + 0x4C4u);
    const float blend = G<float>(profile_reg + 0x6Cu);
    if (std::isfinite(target) && std::isfinite(cur) && std::isfinite(blend)) {
      ctx.xmm[1].f[0] = (target - cur) * blend;
    }
  } else if (std::isfinite(ctx.xmm[1].f[0])) {
    ctx.xmm[1].f[0] *= r;
  }
  // NOTE: Do NOT scale ctx.xmm[0].f[0] here — [profile+0x70] is the per-frame
  // recoil decay factor (< 1.0) stored into [eax+0x4C8] and multiplied every
  // frame in RecoilDecay (0x1E5FB7). Scaling it > 1.0 causes runaway pitch
  // spin.
  MaybeLogSummary();
}

} // namespace

// Explosion impulse & pre-threshold fracture force hooks
void PhysicsBlastImpulseA(X86Context &ctx, uint8_t *) {
  ApplyExplosionImpulse(ctx);
}
void PhysicsBlastImpulseB(X86Context &ctx, uint8_t *) {
  ApplyExplosionImpulse(ctx);
}
void PhysicsBlastImpulseC(X86Context &ctx, uint8_t *) {
  ApplyExplosionImpulse(ctx);
}
void PhysicsBlastDamageA(X86Context &ctx, uint8_t *) {
  g_blast_hits.fetch_add(1, std::memory_order_relaxed);
  const float s =
      g_params.explosion_force.load(std::memory_order_relaxed) *
      g_params.structural_destruction.load(std::memory_order_relaxed) *
      std::max(1.0f, g_params.explosion_radius.load(std::memory_order_relaxed));
  ScaleFloatBits(ctx.eax, s);
  MaybeLogSummary();
}
void PhysicsBlastDamageB(X86Context &ctx, uint8_t *) {
  g_blast_hits.fetch_add(1, std::memory_order_relaxed);
  const float s =
      g_params.explosion_force.load(std::memory_order_relaxed) *
      g_params.structural_destruction.load(std::memory_order_relaxed) *
      std::max(1.0f, g_params.explosion_radius.load(std::memory_order_relaxed));
  ScaleFloatBits(ctx.edx, s);
  MaybeLogSummary();
}
void PhysicsBlastLinearA(X86Context &ctx, uint8_t *) {
  g_blast_hits.fetch_add(1, std::memory_order_relaxed);
  const float exp_f = g_params.explosion_force.load(std::memory_order_relaxed);
  const float lnc_f = g_params.ragdoll_launch.load(std::memory_order_relaxed);
  const float s = exp_f * std::max(1.0f, lnc_f);
  const float lift = (std::max(exp_f, lnc_f) > 1.0f)
                         ? (std::max(exp_f, lnc_f) - 1.0f) * 1.25f
                         : 0.0f;
  ScalePushedVec3(ctx, s, lift);
  MaybeLogSummary();
}
void PhysicsBlastAngularA(X86Context &ctx, uint8_t *) {
  g_blast_hits.fetch_add(1, std::memory_order_relaxed);
  const float s = g_params.explosion_force.load(std::memory_order_relaxed) *
                  g_params.prop_bounciness.load(std::memory_order_relaxed);
  ScalePushedVec3(ctx, s);
}

// Bullet/projectile impact fracture force & rigid-body launch hooks
void PhysicsImpactDamage(X86Context &ctx, uint8_t *) {
  g_impact_hits.fetch_add(1, std::memory_order_relaxed);
  const float imp = g_params.impact_force.load(std::memory_order_relaxed);
  const float str =
      g_params.structural_destruction.load(std::memory_order_relaxed);
  const float s = imp * str * (imp > 1.0f ? 2.5f : 1.0f);
  if (std::fabs(s - 1.0f) > 1e-4f && std::isfinite(ctx.xmm[0].f[0])) {
    ctx.xmm[0].f[0] *= s;
  }
  MaybeLogSummary();
}
void PhysicsImpactImpulseA(X86Context &ctx, uint8_t *) {
  ApplyBulletImpactImpulse(ctx);
}
void PhysicsImpactImpulseB(X86Context &ctx, uint8_t *) {
  ApplyBulletImpactImpulse(ctx);
}
void PhysicsImpactAngularA(X86Context &ctx, uint8_t *) {
  g_impact_hits.fetch_add(1, std::memory_order_relaxed);
  const float s = g_params.impact_force.load(std::memory_order_relaxed) *
                  g_params.prop_bounciness.load(std::memory_order_relaxed);
  ScalePushedVec3(ctx, s);
  MaybeLogSummary();
}
void PhysicsImpactLinearA(X86Context &ctx, uint8_t *) {
  g_impact_hits.fetch_add(1, std::memory_order_relaxed);
  const float imp_f = g_params.impact_force.load(std::memory_order_relaxed);
  const float lift = (imp_f > 1.0f) ? (imp_f - 1.0f) * 0.30f : 0.0f;
  ScalePushedVec3(ctx, imp_f, lift);
}
void PhysicsImpactAngularB(X86Context &ctx, uint8_t *) {
  g_impact_hits.fetch_add(1, std::memory_order_relaxed);
  const float s = g_params.impact_force.load(std::memory_order_relaxed) *
                  g_params.prop_bounciness.load(std::memory_order_relaxed);
  ScalePushedVec3(ctx, s);
  MaybeLogSummary();
}
void PhysicsImpactLinearB(X86Context &ctx, uint8_t *) {
  const float imp_f = g_params.impact_force.load(std::memory_order_relaxed);
  const float lift = (imp_f > 1.0f) ? (imp_f - 1.0f) * 0.30f : 0.0f;
  ScalePushedVec3(ctx, imp_f, lift);
}
void PhysicsImpactAngularC(X86Context &ctx, uint8_t *) {
  g_impact_hits.fetch_add(1, std::memory_order_relaxed);
  const float s = g_params.impact_force.load(std::memory_order_relaxed) *
                  g_params.prop_bounciness.load(std::memory_order_relaxed);
  ScalePushedVec3(ctx, s);
  MaybeLogSummary();
}
void PhysicsImpactLinearC(X86Context &ctx, uint8_t *) {
  const float imp_f = g_params.impact_force.load(std::memory_order_relaxed);
  const float lift = (imp_f > 1.0f) ? (imp_f - 1.0f) * 0.30f : 0.0f;
  ScalePushedVec3(ctx, imp_f, lift);
}
void PhysicsImpactAngularD(X86Context &ctx, uint8_t *) {
  g_impact_hits.fetch_add(1, std::memory_order_relaxed);
  const float s = g_params.impact_force.load(std::memory_order_relaxed) *
                  g_params.prop_bounciness.load(std::memory_order_relaxed);
  ScalePushedVec3(ctx, s);
  MaybeLogSummary();
}
void PhysicsImpactLinearD(X86Context &ctx, uint8_t *) {
  const float imp_f = g_params.impact_force.load(std::memory_order_relaxed);
  const float lift = (imp_f > 1.0f) ? (imp_f - 1.0f) * 0.30f : 0.0f;
  ScalePushedVec3(ctx, imp_f, lift);
}

// World gravity post-PhysicsTime step (runs at 0x1F11D5 right before solver
// step call [edx])
void PhysicsGravityPost(X86Context &, uint8_t *) {
  uint32_t physics = R32(kPhysics);
  uint32_t solver = IsValidGuestAddr(physics, 8u) ? R32(physics) : 0;
  uint32_t world =
      IsValidGuestAddr(solver, 0x20u) ? R32(solver + kSolverWorld) : 0;
  if (!IsValidGuestAddr(world, 0x390u))
    return;

  g_gravity_steps.fetch_add(1, std::memory_order_relaxed);

  float &gy = G<float>(world + kPhysicsGravity + 4);
  float &gsy = G<float>(world + kPhysicsGravityStep + 4);
  const float dt = G<float>(world + kPhysicsStep);

  // Refresh unscaled baseline if level reloaded or guest reinitialized world
  // gravity
  if (world != g_last_world ||
      (g_gravity_overridden ? (std::fabs(gy - g_written_gravity_y) > 1e-2f)
                            : true)) {
    if (std::isfinite(gy) && gy != 0.0f) {
      if (!g_gravity_overridden || world != g_last_world ||
          std::fabs(gy - g_written_gravity_y) > 1e-2f) {
        g_base_gravity_y = gy;
      }
    } else {
      g_base_gravity_y = -25.0f;
    }
    g_last_world = world;
  }

  // Track live player health ratio and damage pulse for screen-space vignette &
  // low-HP desaturation
  {
    uint32_t rworld = R32(kWorld);
    uint32_t gobj = R32(kGame);
    uint32_t me = IsValidGuestAddr(rworld, kWorldPlayer + 0x300u)
                      ? (rworld + kWorldPlayer)
                      : 0;
    if (me && IsValidGuestAddr(gobj, 0x20150u) && R32(me + 0xC4u) == 2u) {
      const float hp = G<float>(me + 0x2F0u);
      const float full = (R32(gobj + 0x2014Cu) >= 2u) ? 750.0f : 1200.0f;
      const float ratio = std::clamp(hp / full, 0.0f, 1.0f);
      if (ratio < game::physics_lab::g_prev_health_ratio - 0.004f) {
        game::physics_lab::g_damage_flash = std::min(
            1.0f, game::physics_lab::g_damage_flash +
                      (game::physics_lab::g_prev_health_ratio - ratio) * 4.5f +
                      0.35f);
      } else {
        const float decay =
            (dt > 0.0f && std::isfinite(dt)) ? dt * 1.4f : 0.025f;
        game::physics_lab::g_damage_flash =
            std::max(0.0f, game::physics_lab::g_damage_flash - decay);
      }
      game::physics_lab::g_prev_health_ratio = ratio;
      game::g_player_health_ratio.store(ratio, std::memory_order_relaxed);
      game::g_recent_damage.store(game::physics_lab::g_damage_flash,
                                  std::memory_order_relaxed);
    } else {
      game::physics_lab::g_prev_health_ratio = 1.0f;
      game::physics_lab::g_damage_flash = 0.0f;
      game::g_player_health_ratio.store(1.0f, std::memory_order_relaxed);
      game::g_recent_damage.store(0.0f, std::memory_order_relaxed);
    }
  }

  const float g_scale = g_params.gravity_scale.load(std::memory_order_relaxed);
  static float s_prev_g_scale = 1.0f;
  const bool g_scale_changed = std::fabs(g_scale - s_prev_g_scale) > 1e-4f;
  s_prev_g_scale = g_scale;

  constexpr uint32_t kPhysicsBodyEnd = 0x270;
  constexpr uint32_t kPhysicsBodies = 0x29C;
  constexpr uint32_t kBodyNext = 0x2C;
  constexpr uint32_t kBodyImpulseY = 0x94;

  auto adjust_bodies = [&](float delta_gsy) {
    if (std::fabs(delta_gsy) <= 1e-7f)
      return;
    uint32_t body = R32(world + kPhysicsBodies);
    for (int left = 4096; body && body != world + kPhysicsBodyEnd &&
                          IsValidGuestAddr(body, 0xA0u) && left;
         left--, body = R32(body + kBodyNext)) {
      float &imp_y = G<float>(body + kBodyImpulseY);
      if (std::isfinite(imp_y))
        imp_y += delta_gsy;
    }
  };

  if (std::fabs(g_scale - 1.0f) < 1e-4f) {
    if (g_gravity_overridden) {
      const float old_gsy = gsy;
      gy = g_base_gravity_y;
      if (dt > 0.0f && std::isfinite(dt))
        gsy = gy * dt * dt;
      adjust_bodies(gsy - old_gsy);
      g_gravity_overridden = false;
    }
    g_active_gravity_y.store(gy, std::memory_order_relaxed);
    return;
  }

  const float old_gsy = gsy;
  g_written_gravity_y = g_base_gravity_y * g_scale;
  gy = g_written_gravity_y;
  if (dt > 0.0f && std::isfinite(dt))
    gsy = gy * dt * dt;
  if (g_scale_changed || std::fabs(gsy - old_gsy) > 1e-6f) {
    adjust_bodies(gsy - old_gsy);
  }
  g_gravity_overridden = true;
  g_active_gravity_y.store(gy, std::memory_order_relaxed);
  MaybeLogSummary();
}

// Glass/shatter shard acceleration (runs at 0x1693EA before ShardStep #1; xmm0
// holds [ebx+0xC0] accel)
void PhysicsShardGravity(X86Context &ctx, uint8_t *) {
  const float g_scale = g_params.gravity_scale.load(std::memory_order_relaxed);
  if (std::fabs(g_scale - 1.0f) > 1e-4f) {
    for (int i = 0; i < 3; i++) {
      if (std::isfinite(ctx.xmm[0].f[i]))
        ctx.xmm[0].f[i] *= g_scale;
    }
  }
}

// Glass/shatter shard velocity displacement (runs at 0x1693F6 before ShardStep
// #2; xmm0 holds [eax] velocity)
void PhysicsShardDebris(X86Context &ctx, uint8_t *) {
  g_debris_steps.fetch_add(1, std::memory_order_relaxed);
  const float d = g_params.debris_intensity.load(std::memory_order_relaxed);
  const float sd =
      g_params.structural_destruction.load(std::memory_order_relaxed);
  const float s = d * std::max(1.0f, std::sqrt(std::max(0.25f, sd)));
  if (std::fabs(s - 1.0f) > 1e-4f) {
    for (int i = 0; i < 3; i++) {
      if (std::isfinite(ctx.xmm[0].f[i]))
        ctx.xmm[0].f[i] *= s;
    }
  }
}

// Debris chunk rigid-body velocity launch (runs at 0x1EA22A before sub_000DC070
// in sub_001EA1E0)
void PhysicsDebrisLaunch(X86Context &ctx, uint8_t *) {
  g_debris_steps.fetch_add(1, std::memory_order_relaxed);
  const float d = g_params.debris_intensity.load(std::memory_order_relaxed);
  const float sd =
      g_params.structural_destruction.load(std::memory_order_relaxed);
  const float s = d * std::max(1.0f, std::sqrt(std::max(0.25f, sd)));
  const float lift = (s > 1.0f) ? std::min((s - 1.0f) * 0.55f, 2.10f) : 0.0f;
  ScalePushedVec3(ctx, s, lift);
  MaybeLogSummary();
}

// Particle fade band scaling (runs at 0x16DFFF before subss xmm2, xmm3)
// Keeps xmm3 <= 0.90f * xmm2 so (far_cut - fade_band) stays strictly positive
// and divss never blows up.
void PhysicsParticleDebris(X86Context &ctx, uint8_t *) {
  g_debris_steps.fetch_add(1, std::memory_order_relaxed);
  const float d = g_params.debris_intensity.load(std::memory_order_relaxed);
  if (std::fabs(d - 1.0f) > 1e-4f && d > 0.05f &&
      std::isfinite(ctx.xmm[3].f[0]) && ctx.xmm[3].f[0] > 0.0f &&
      std::isfinite(ctx.xmm[2].f[0]) && ctx.xmm[2].f[0] > 0.0f) {
    const float scaled = ctx.xmm[3].f[0] / d;
    ctx.xmm[3].f[0] = std::min(scaled, ctx.xmm[2].f[0] * 0.90f);
  }
}

// Ragdoll linear & angular velocity retention (runs at 0x77DA1 after
// BodyDamping + [esp+0x0C] store)
void PhysicsRagdollDamping(X86Context &ctx, uint8_t *) {
  g_ragdoll_steps.fetch_add(1, std::memory_order_relaxed);
  const float r =
      std::max(g_params.character_reaction.load(std::memory_order_relaxed),
               g_params.prop_bounciness.load(std::memory_order_relaxed));
  if (std::fabs(r - 1.0f) < 1e-4f)
    return;

  // Preserve energetic launch/scatter cleanly when r > 1, or settle faster when
  // r < 1
  const float exp_factor = 1.0f / std::clamp(r, 0.25f, 8.00f);
  if (std::isfinite(ctx.xmm[0].f[0]) && ctx.xmm[0].f[0] > 0.0f &&
      ctx.xmm[0].f[0] < 1.0f) {
    ctx.xmm[0].f[0] =
        std::clamp(std::pow(ctx.xmm[0].f[0], exp_factor), 0.05f, 0.9998f);
  }
  if (IsValidGuestAddr(ctx.esp, 0x10u)) {
    float &ang = G<float>(ctx.esp + 0x0Cu);
    if (std::isfinite(ang) && ang > 0.0f && ang < 1.0f) {
      ang = std::clamp(std::pow(ang, exp_factor), 0.05f, 0.9998f);
    }
  }
}

// Ragdoll & Rigid Body speed cap (runs AFTER BodyTopSpeed + mulss xmm1, xmm1 at
// 0x77DFF and 0x77E74) IMPORTANT: In sub_00077D60 (x86_recomp.11.cpp:21835),
// xmm0 is |v|^2 (actual squared velocity) and xmm1 is top_speed_cap^2. Never
// inflate xmm0 (which would cause comiss xmm0, xmm1 and sqrt(xmm1/xmm0) to
// clamp speeds DOWN); only raise xmm1 (top_speed_cap^2)!
void PhysicsRagdollSpeedA(X86Context &ctx, uint8_t *) {
  const float r = std::max(
      {1.0f, g_params.character_reaction.load(std::memory_order_relaxed),
       g_params.ragdoll_launch.load(std::memory_order_relaxed),
       g_params.explosion_force.load(std::memory_order_relaxed),
       g_params.impact_force.load(std::memory_order_relaxed),
       g_params.debris_intensity.load(std::memory_order_relaxed)});
  if (std::fabs(r - 1.0f) > 1e-4f && std::isfinite(ctx.xmm[1].f[0])) {
    ctx.xmm[1].f[0] *= (r * r * 6.0f);
  }
}

void PhysicsRagdollSpeedB(X86Context &ctx, uint8_t *) {
  const float r = std::max(
      {1.0f, g_params.character_reaction.load(std::memory_order_relaxed),
       g_params.ragdoll_launch.load(std::memory_order_relaxed),
       g_params.explosion_force.load(std::memory_order_relaxed),
       g_params.impact_force.load(std::memory_order_relaxed),
       g_params.debris_intensity.load(std::memory_order_relaxed),
       g_params.prop_bounciness.load(std::memory_order_relaxed)});
  if (std::fabs(r - 1.0f) > 1e-4f && std::isfinite(ctx.xmm[1].f[0])) {
    ctx.xmm[1].f[0] *= (r * r * 6.0f);
  }
}

// Character hit/ragdoll angular & linear launches (x86_recomp.54.cpp: 0x1FFFB1,
// 0x2082F9, 0x209119, 0x20914B)
void PhysicsCharacterLaunchA(X86Context &ctx, uint8_t *) {
  g_ragdoll_steps.fetch_add(1, std::memory_order_relaxed);
  const float rag = g_params.character_reaction.load(std::memory_order_relaxed);
  const float lnc = g_params.ragdoll_launch.load(std::memory_order_relaxed);
  const float s = rag * lnc;
  const float lift =
      (std::max(rag, lnc) > 1.0f) ? (std::max(rag, lnc) - 1.0f) * 1.4f : 0.0f;
  ScalePushedVec3(ctx, s, lift);
  MaybeLogSummary();
}
void PhysicsCharacterLaunchB(X86Context &ctx, uint8_t *) {
  g_ragdoll_steps.fetch_add(1, std::memory_order_relaxed);
  const float rag = g_params.character_reaction.load(std::memory_order_relaxed);
  const float lnc = g_params.ragdoll_launch.load(std::memory_order_relaxed);
  const float s = rag * lnc;
  const float lift =
      (std::max(rag, lnc) > 1.0f) ? (std::max(rag, lnc) - 1.0f) * 1.4f : 0.0f;
  ScalePushedVec3(ctx, s, lift);
  MaybeLogSummary();
}
void PhysicsCharacterLaunchC(X86Context &ctx, uint8_t *) {
  g_ragdoll_steps.fetch_add(1, std::memory_order_relaxed);
  const float rag = g_params.character_reaction.load(std::memory_order_relaxed);
  const float lnc = g_params.ragdoll_launch.load(std::memory_order_relaxed);
  const float s = rag * lnc;
  const float lift =
      (std::max(rag, lnc) > 1.0f) ? (std::max(rag, lnc) - 1.0f) * 1.4f : 0.0f;
  ScalePushedVec3(ctx, s, lift);
  MaybeLogSummary();
}
void PhysicsCharacterLaunchD(X86Context &ctx, uint8_t *) {
  const float rag = g_params.character_reaction.load(std::memory_order_relaxed);
  const float lnc = g_params.ragdoll_launch.load(std::memory_order_relaxed);
  const float s = rag * lnc;
  const float lift =
      (std::max(rag, lnc) > 1.0f) ? (std::max(rag, lnc) - 1.0f) * 2.2f : 0.0f;
  ScalePushedVec3(ctx, s, lift);
}

namespace {

inline float EffectiveRagdollBoost() noexcept {
  return std::max({g_params.ragdoll_launch.load(std::memory_order_relaxed),
                   g_params.character_reaction.load(std::memory_order_relaxed),
                   g_params.impact_force.load(std::memory_order_relaxed),
                   g_params.explosion_force.load(std::memory_order_relaxed)});
}

inline void ApplyRagdollLaunchVector(X86Xmm &v, float boost) noexcept {
  if (std::fabs(boost - 1.0f) <= 1e-3f)
    return;
  const float horiz = (boost > 1.0f) ? std::clamp(boost * 3.5f, 1.0f, 45.0f)
                                     : std::clamp(boost, 0.25f, 1.0f);
  if (std::isfinite(v.f[0]))
    v.f[0] *= horiz;
  if (std::isfinite(v.f[2]))
    v.f[2] *= horiz;
  if (std::isfinite(v.f[1])) {
    const float up = (boost > 1.0f) ? (boost - 1.0f) * 8.5f : 0.0f;
    v.f[1] = std::clamp(v.f[1] * boost + up, -40.0f, 95.0f);
  }
}

} // namespace

// Ragdoll death initial velocity override (sub_00200045 inside sub_001FFE70,
// x86_recomp.54.cpp:3288). Vanilla clamps the dying soldier's launch velocity
// to ~1.5 units/sec (`0x2AF550`) and stores it to `[eax+0x60]` & `[eax+0x70]`.
// When Ragdoll Launch / Explosion Force / Impact Force are elevated, amplify
// horizontal throw and inject strong upward lift!
X86_EXTERN_FUNC(__imp__sub_00200045);
X86_FUNC(sub_00200045) {
  const float boost = EffectiveRagdollBoost();
  if (IsValidGuestAddr(ctx.eax, 0x90u)) {
    bool modified = false;
    if (std::fabs(boost - 1.0f) > 1e-3f) {
      ApplyRagdollLaunchVector(ctx.xmm[1], boost);
      modified = true;
    }
    if (game::delirium_phys::ApplyDeathCollapseVariation(0u, ctx.xmm[1])) {
      modified = true;
    }
    if (modified) {
      *X86_PTR(X86Xmm, ctx.eax + 0x60u) = ctx.xmm[1];
      if (IsValidGuestAddr(ctx.esp, 0x38u)) {
        *X86_PTR(X86Xmm, ctx.esp + 0x24u) = ctx.xmm[1];
      }
    }
  }
  __imp__sub_00200045(ctx, base);
}

// Secondary ragdoll death initializer (sub_002080C0 in
// x86_recomp.54.cpp:24582). Many soldier deaths go through sub_002080C0 (`edi =
// ecx`), which clamps velocity against `0x2AF550` and writes `[state+0x60]` &
// `[state+0x70]` directly without calling sub_001FFE70/sub_00200045.
X86_EXTERN_FUNC(__imp__sub_002080C0);
X86_FUNC(sub_002080C0) {
  const float boost = EffectiveRagdollBoost();
  const bool check_boost = std::fabs(boost - 1.0f) > 1e-3f;
  const bool delirium_on =
      g_params.delirium_enabled.load(std::memory_order_relaxed);
  const uint32_t self = ctx.ecx;
  X86Xmm prev60{};
  bool had_prev = false;
  if ((check_boost || delirium_on) && IsValidGuestAddr(self, 0x18u)) {
    const uint32_t state = R32(self + 0x10u);
    if (IsValidGuestAddr(state, 0x80u)) {
      prev60 = *X86_PTR(X86Xmm, state + 0x60u);
      had_prev = true;
    }
  }
  __imp__sub_002080C0(ctx, base);
  if ((check_boost || delirium_on) && IsValidGuestAddr(self, 0x18u)) {
    const uint32_t state = R32(self + 0x10u);
    if (IsValidGuestAddr(state, 0x80u)) {
      X86Xmm v60 = *X86_PTR(X86Xmm, state + 0x60u);
      X86Xmm v70 = *X86_PTR(X86Xmm, state + 0x70u);
      const bool changed =
          !had_prev || (v60.u[0] != prev60.u[0] || v60.u[1] != prev60.u[1] ||
                        v60.u[2] != prev60.u[2]);
      const bool synced = (v60.u[0] == v70.u[0] && v60.u[1] == v70.u[1] &&
                           v60.u[2] == v70.u[2]);
      if (changed || synced) {
        bool modified = false;
        if (check_boost) {
          ApplyRagdollLaunchVector(v60, boost);
          modified = true;
        }
        if (game::delirium_phys::ApplyDeathCollapseVariation(self, v60)) {
          modified = true;
        }
        if (modified) {
          *X86_PTR(X86Xmm, state + 0x60u) = v60;
          *X86_PTR(X86Xmm, state + 0x70u) = v60;
          g_ragdoll_steps.fetch_add(1, std::memory_order_relaxed);
          MaybeLogSummary();
        }
      }
    }
  }
}

// Universal Ragdoll Bone Lift & Impulse (sub_000DC960 at 0x000DC983 and
// 0x000DC9A6)
void PhysicsRagdollBoneLift(X86Context &ctx, uint8_t *) {
  const float lnc =
      std::max(g_params.ragdoll_launch.load(std::memory_order_relaxed),
               g_params.character_reaction.load(std::memory_order_relaxed));
  if (std::fabs(lnc - 1.0f) > 1e-4f && std::isfinite(ctx.xmm[0].f[0])) {
    ctx.xmm[0].f[0] =
        ctx.xmm[0].f[0] * lnc + (lnc > 1.0f ? (lnc - 1.0f) * 1.15f : 0.0f);
  }
}

void PhysicsRagdollBoneImpulse(X86Context &ctx, uint8_t *) {
  g_ragdoll_steps.fetch_add(1, std::memory_order_relaxed);
  const float rag = g_params.character_reaction.load(std::memory_order_relaxed);
  const float lnc = g_params.ragdoll_launch.load(std::memory_order_relaxed);
  const float imp = g_params.impact_force.load(std::memory_order_relaxed);
  const float raw_s = rag * lnc * std::max(1.0f, imp);
  const float s = TimeCompensatedScale(raw_s, 2.25f);
  if (std::fabs(s - 1.0f) > 1e-4f && std::isfinite(ctx.xmm[0].f[0])) {
    ctx.xmm[0].f[0] *= s;
  }
  MaybeLogSummary();
}

// Universal Radial Explosion Impulse & Ragdoll Lift (sub_000DCC40 at 0x000DCC6A
// and 0x000DCCCA) At 0x000DCC6A (right before `mulps xmm0, xmm1`):
// - `ctx.eax` was loaded by `000DCC60 mov eax, [ecx+0xC4]` and holds the
// integer `body_type` directly (3 = ragdoll/character, 4 = rigid prop).
// - `ctx.xmm[0]` holds the 3D blast direction vector `[ecx]` (X, Y, Z, W).
// - `ctx.xmm[1]` holds the broadcast scalar blast impulse (`shufps xmm1, xmm1,
// 0x00`).
void PhysicsUniversalBlastImpulse(X86Context &ctx, uint8_t *) {
  g_blast_hits.fetch_add(1, std::memory_order_relaxed);
  const float exp_f = g_params.explosion_force.load(std::memory_order_relaxed);
  const float lnc_f = g_params.ragdoll_launch.load(std::memory_order_relaxed);
  const float deb_f = g_params.debris_intensity.load(std::memory_order_relaxed);
  const float rad_f = g_params.explosion_radius.load(std::memory_order_relaxed);
  const uint32_t body_type = ctx.eax;
  const float raw_s = (body_type == 3u)
                          ? (exp_f * std::max({1.0f, lnc_f, rad_f}))
                          : (exp_f * std::max({1.0f, deb_f, rad_f}));
  const float s = TimeCompensatedScale(raw_s, 2.5f);
  if (std::fabs(s - 1.0f) > 1e-4f) {
    for (int i = 0; i < 4; i++) {
      if (std::isfinite(ctx.xmm[1].f[i])) {
        ctx.xmm[1].f[i] *= s;
      }
    }
  }
  const float lift_ctrl = std::max({lnc_f, exp_f, rad_f});
  if (std::fabs(lift_ctrl - 1.0f) > 1e-4f && std::isfinite(ctx.xmm[0].f[1])) {
    if (ctx.xmm[0].f[1] > 0.0f) {
      ctx.xmm[0].f[1] *= lift_ctrl;
    }
    if (lift_ctrl > 1.0f) {
      ctx.xmm[0].f[1] += (lift_ctrl - 1.0f) * 0.85f;
    }
  }
  MaybeLogSummary();
}

void PhysicsBlastRagdollLift(X86Context &ctx, uint8_t *) {
  const float lnc = g_params.ragdoll_launch.load(std::memory_order_relaxed);
  const float exp_f = g_params.explosion_force.load(std::memory_order_relaxed);
  const float lift_ctrl = std::max(lnc, exp_f);
  if (std::fabs(lift_ctrl - 1.0f) > 1e-4f && std::isfinite(ctx.xmm[0].f[0])) {
    ctx.xmm[0].f[0] = ctx.xmm[0].f[0] * lift_ctrl +
                      (lift_ctrl > 1.0f ? (lift_ctrl - 1.0f) * 1.25f : 0.0f);
  }
}

// Universal Point / Bullet Impact Impulse (sub_000DCF80 at 0x000DCFA7)
// At 0x000DCFA7 (right before `mulps xmm1, xmm0`):
// - `ctx.xmm[0]` holds the broadcast scalar point impulse (`000DCFA3 shufps
// xmm0, xmm0, 0x00`).
// - `ctx.xmm[1]` holds the 3D point-impulse direction vector.
void PhysicsUniversalPointImpulse(X86Context &ctx, uint8_t *) {
  g_impact_hits.fetch_add(1, std::memory_order_relaxed);
  const float imp_f = g_params.impact_force.load(std::memory_order_relaxed);
  const float lnc_f = g_params.ragdoll_launch.load(std::memory_order_relaxed);
  const float raw_s = imp_f * std::max(1.0f, lnc_f);
  const float s = TimeCompensatedScale(raw_s, 2.5f);
  if (std::fabs(s - 1.0f) > 1e-4f) {
    for (int i = 0; i < 4; i++) {
      if (std::isfinite(ctx.xmm[0].f[i])) {
        ctx.xmm[0].f[i] *= s;
      }
    }
  }
  const float lift_ctrl = std::max(lnc_f, imp_f);
  if (lift_ctrl > 1.0f && std::isfinite(ctx.xmm[1].f[1])) {
    ctx.xmm[1].f[1] += (lift_ctrl - 1.0f) * 0.55f;
  }
  MaybeLogSummary();
}

namespace {

inline float EffectiveBlastRadius() noexcept {
  const float rad = g_params.explosion_radius.load(std::memory_order_relaxed);
  const float exp_f = g_params.explosion_force.load(std::memory_order_relaxed);
  const float derived =
      (exp_f >= 1.0f) ? (1.0f + (exp_f - 1.0f) * 0.55f) : exp_f;
  return std::clamp(std::max(rad, derived), 0.25f, 8.0f);
}

} // namespace

// Outer explosion query wrapper (sub_001E5410 in x86_recomp.51.cpp:12952).
// Expands `[ecx+0xF4]` outer blast sphere before spatial queries run so distant
// props/ragdolls receive shockwaves.
namespace game {
namespace ballistics {
void OnExplosionDetonate(const float pos[3], float radius, float force);
}
} // namespace game

X86_EXTERN_FUNC(__imp__sub_001E5410);
X86_FUNC(sub_001E5410) {
  const uint32_t exp_desc = ctx.ecx;
  float deton_pos[3] = {};
  bool have_deton_pos = false;
  float base_radius = 6.5f;
  if (IsValidGuestAddr(exp_desc, 0x100u)) {
    const float r = G<float>(exp_desc + 0xF4u);
    if (std::isfinite(r) && r > 0.25f && r < 200.0f)
      base_radius = r;
  }
  const uint32_t pos_ptr =
      IsValidGuestAddr(ctx.esp + 0x10u, 4u) ? R32(ctx.esp + 0x10u) : 0u;
  if (pos_ptr && IsValidGuestAddr(pos_ptr, 12u)) {
    deton_pos[0] = G<float>(pos_ptr + 0u);
    deton_pos[1] = G<float>(pos_ptr + 4u);
    deton_pos[2] = G<float>(pos_ptr + 8u);
    have_deton_pos =
        std::isfinite(deton_pos[0]) && std::isfinite(deton_pos[1]) &&
        std::isfinite(deton_pos[2]) &&
        (deton_pos[0] * deton_pos[0] + deton_pos[1] * deton_pos[1] +
         deton_pos[2] * deton_pos[2]) > 0.01f;
  }
  const float rad = EffectiveBlastRadius();
  const float exp_f = g_params.explosion_force.load(std::memory_order_relaxed);
  if (have_deton_pos) {
    game::ballistics::OnExplosionDetonate(deton_pos, base_radius * rad, exp_f);
    game::delirium_phys::OnExplosionShockwave(deton_pos, base_radius * rad, exp_f);
  }
  if (std::fabs(rad - 1.0f) > 1e-4f && IsValidGuestAddr(exp_desc, 0x100u)) {
    const float orig_r = G<float>(exp_desc + 0xF4u);
    if (std::isfinite(orig_r) && orig_r > 0.01f) {
      G<float>(exp_desc + 0xF4u) = orig_r * rad;
      __imp__sub_001E5410(ctx, base);
      G<float>(exp_desc + 0xF4u) = orig_r;
      g_blast_hits.fetch_add(1, std::memory_order_relaxed);
      MaybeLogSummary();
      return;
    }
  }
  __imp__sub_001E5410(ctx, base);
}

// Explosion Blast Radius Falloff hooks (0x001E8E26, 0x001E9060, 0x001E5664)
// NOTE: xmm0/xmm1 hold squared effective blast cutoff compared via `comiss xmm,
// dist_sq; jb skip`, so we MUST MULTIPLY (not divide!) by `rad * rad` to expand
// the active blast zone.
void PhysicsBlastRadiusFalloffA(X86Context &ctx, uint8_t *) {
  const float rad = EffectiveBlastRadius();
  if (std::fabs(rad - 1.0f) > 1e-4f && std::isfinite(ctx.xmm[0].f[0])) {
    ctx.xmm[0].f[0] *= (rad * rad);
  }
}

void PhysicsBlastRadiusFalloffB(X86Context &ctx, uint8_t *) {
  const float rad = EffectiveBlastRadius();
  if (std::fabs(rad - 1.0f) > 1e-4f && std::isfinite(ctx.xmm[1].f[0])) {
    ctx.xmm[1].f[0] *= (rad * rad);
  }
}

void PhysicsBlastRadiusFalloffC(X86Context &ctx, uint8_t *) {
  const float rad = EffectiveBlastRadius();
  if (std::fabs(rad - 1.0f) > 1e-4f && std::isfinite(ctx.xmm[0].f[0])) {
    ctx.xmm[0].f[0] *= (rad * rad);
  }
}

// Explosion Shockwave Camera Shake hooks (0x001E9216, 0x001E92F1)
namespace {
inline void InjectBlastLensPulse(float raw_shake) noexcept {
  if (!std::isfinite(raw_shake))
    return;
  const float mag = std::clamp(std::fabs(raw_shake) * 0.85f, 0.0f, 1.0f);
  if (mag <= 0.04f)
    return;
  float prev = game::g_blast_lens_pulse.load(std::memory_order_relaxed);
  while (mag > prev && !game::g_blast_lens_pulse.compare_exchange_weak(
                           prev, mag, std::memory_order_relaxed)) {
  }
}
} // namespace

void PhysicsCameraShakeA(X86Context &ctx, uint8_t *) {
  const float shk = g_params.camera_shockwave.load(std::memory_order_relaxed);
  if (std::isfinite(ctx.xmm[0].f[0])) {
    if (std::fabs(shk - 1.0f) > 1e-4f) {
      ctx.xmm[0].f[0] *= shk;
    }
    InjectBlastLensPulse(ctx.xmm[0].f[0]);
  }
}

void PhysicsCameraShakeB(X86Context &ctx, uint8_t *) {
  const float shk = g_params.camera_shockwave.load(std::memory_order_relaxed);
  if (std::isfinite(ctx.xmm[0].f[0])) {
    if (std::fabs(shk - 1.0f) > 1e-4f) {
      ctx.xmm[0].f[0] *= shk;
    }
    InjectBlastLensPulse(ctx.xmm[0].f[0]);
  }
}

// Primary weapon recoil kick (runs at 0x1EF702 before addss xmm1, [eax+0x4C4];
// profile pointer in edi)
void PhysicsRecoilKickPrimary(X86Context &ctx, uint8_t *) {
  ApplyRecoilKick(ctx, ctx.edi);
}

// Secondary weapon recoil kick (runs at 0x1EFE50 before addss xmm1,
// [eax+0x4C4]; profile pointer in ebx)
void PhysicsRecoilKick(X86Context &ctx, uint8_t *) {
  ApplyRecoilKick(ctx, ctx.ebx);
}

namespace {

inline void GetFireAndSmokeMultipliers(float &out_size_mult,
                                       float &out_density_mult) {
  const auto &enh = modsettings::Registry().GetEnhanced();
  const float rad_f = g_params.explosion_radius.load(std::memory_order_relaxed);
  const float exp_f = g_params.explosion_force.load(std::memory_order_relaxed);
  const float imp_f = g_params.impact_force.load(std::memory_order_relaxed);
  const float smoke_f = std::clamp(
      static_cast<float>(enh.particle_fade_scale) / 100.0f, 0.25f, 10.0f);
  const float glow_f = std::clamp(
      static_cast<float>(enh.explosion_glow_boost) / 100.0f, 0.25f, 5.0f);

  // Unified master power across explosion, blast radius, impact, and smoke/fire
  // scale
  const float unified_power =
      std::max({exp_f, imp_f, smoke_f, glow_f,
                (rad_f >= 1.0f) ? (1.0f + (rad_f - 1.0f) * 2.2f) : rad_f});

  // Map unified_power (0.25x .. 10.0x) into a bold 3D world-size multiplier
  // (0.25x .. 5.50x) while keeping soft circular radial gradients and viewport
  // safety clamps intact.
  if (unified_power > 1.0f) {
    out_size_mult =
        std::clamp(1.0f + (unified_power - 1.0f) * 0.50f, 1.0f, 5.50f);
  } else {
    out_size_mult = std::clamp(unified_power, 0.25f, 1.0f);
  }

  // Map unified_power into particle lifetime & plume persistence multiplier
  // (0.25x .. 3.70x)
  if (unified_power > 1.0f) {
    out_density_mult =
        std::clamp(1.0f + (unified_power - 1.0f) * 0.30f, 1.0f, 3.70f);
  } else {
    out_density_mult = std::clamp(unified_power, 0.25f, 1.0f);
  }
}

} // namespace

// 1. GPU Particle Emitter Projection State Setup (sub_0016DEC0 in
// x86_recomp.38.cpp:11833) Populates emitter projection state `esi` (0x90 bytes
// on stack) fresh on every draw from definition `edi`. Consumed immediately by
// sub_00158B70 and sub_0015B380 (where `ebx` = `esi`). IMPORTANT:
// - NEVER modify +0x30..+0x38 (base float RGB) or +0x50..+0x54 (age knee
// reciprocals) — modifying those
//   breaks color/alpha curve interpolation and turns soft radial smoke/fire
//   textures into hard squares!
// - Scale +0x80, +0x84, +0x88 (true 3D-to-screen particle size projection
// vector), while keeping
//   +0x60..+0x6C viewport screen-size clamp capped at <= 2.35x so near-camera
//   muzzle/smoke puffs never swallow the player weapon or cover the whole
//   screen.
X86_EXTERN_FUNC(__imp__sub_0016DEC0);
X86_FUNC(sub_0016DEC0) {
  __imp__sub_0016DEC0(ctx, base);
  float size_mult = 1.0f, density_mult = 1.0f;
  GetFireAndSmokeMultipliers(size_mult, density_mult);
  if (std::fabs(size_mult - 1.0f) <= 1e-3f &&
      std::fabs(density_mult - 1.0f) <= 1e-3f) {
    return;
  }
  const uint32_t esi = ctx.esi;
  if (!IsValidGuestAddr(esi, 0x90u))
    return;

  if (std::fabs(size_mult - 1.0f) > 1e-3f) {
    // Scale true particle world-to-screen size projection vector (+0x80..+0x88)
    for (uint32_t off : {0x80u, 0x84u, 0x88u}) {
      float v = G<float>(esi + off);
      if (std::isfinite(v))
        G<float>(esi + off) = v * size_mult;
    }
    // Keep max screen-space clamp bounded so near-camera particles stay safe
    // while world plumes expand.
    const float clamp_mult = std::clamp(size_mult, 0.25f, 2.35f);
    for (uint32_t off : {0x60u, 0x64u, 0x68u, 0x6Cu}) {
      float v = G<float>(esi + off);
      if (std::isfinite(v))
        G<float>(esi + off) = v * clamp_mult;
    }
  }
  const float vis_mult = std::max(size_mult, density_mult);
  if (vis_mult > 1.001f) {
    // Lower minimum screen-size culling threshold (+0x70) so distant
    // smoke/embers stay visible.
    float min_cull = G<float>(esi + 0x70u);
    if (std::isfinite(min_cull) && min_cull > 0.0f) {
      G<float>(esi + 0x70u) = min_cull / vis_mult;
    }
    // Extend far clip distance (+0x44) and soften far distance fade slope
    // (+0x4C).
    float far_clip = G<float>(esi + 0x44u);
    if (std::isfinite(far_clip) && far_clip > 0.0f) {
      G<float>(esi + 0x44u) = far_clip * vis_mult;
    }
    float far_slope = G<float>(esi + 0x4Cu);
    if (std::isfinite(far_slope) && far_slope > 0.0f) {
      G<float>(esi + 0x4Cu) = far_slope / vis_mult;
    }
  }
}

// 1B. GPU Particle Simulation & Billowing Cloud Evaluator (sub_0015B380 in
// x86_recomp.36.cpp:13265) Args on stack: [esp+0x04] = particle instance list
// (`edx`), [esp+0x08] = emitter definition (`esi`),
//                [esp+0x0C] = projection state (`ebx`).
// Inside emitter definition `esi`:
//   +0x70 = particle max lifetime (seconds). `sub_0015B380` computes `1.0f /
//   [esi+0x70]` at entry so
//           normalized age `t` stays strictly in [0.0 .. 1.0] (100% square-free
//           soft circular alpha!).
//   +0x80 = initial velocity scale, +0x88 = quadratic expansion / buoyancy
//   scale. +0xA4 = birth radius, +0xA8 = mid-life knee radius, +0xAC =
//   end-of-life billowing cloud radius.
X86_EXTERN_FUNC(__imp__sub_0015B380);
X86_FUNC(sub_0015B380) {
  float size_mult = 1.0f, density_mult = 1.0f;
  GetFireAndSmokeMultipliers(size_mult, density_mult);
  if ((std::fabs(size_mult - 1.0f) > 1e-3f ||
       std::fabs(density_mult - 1.0f) > 1e-3f) &&
      IsValidGuestAddr(ctx.esp, 0x10u)) {
    const uint32_t def = R32(ctx.esp + 0x08u);
    if (IsValidGuestAddr(def, 0xD8u)) {
      const float orig_life = G<float>(def + 0x70u);
      const float orig_vel = G<float>(def + 0x80u);
      const float orig_acc = G<float>(def + 0x88u);
      const float orig_r0 = G<float>(def + 0xA4u);
      const float orig_r1 = G<float>(def + 0xA8u);
      const float orig_r2 = G<float>(def + 0xACu);

      const float life_scale = std::clamp(density_mult, 0.25f, 3.50f);
      const float r0_scale =
          (size_mult > 1.0f) ? (1.0f + (size_mult - 1.0f) * 0.55f) : size_mult;
      const float r1_scale =
          (size_mult > 1.0f) ? (1.0f + (size_mult - 1.0f) * 0.80f) : size_mult;
      const float r2_scale = (size_mult > 1.0f) ? size_mult : size_mult;
      const float vel_scale =
          (size_mult > 1.0f) ? (1.0f + (size_mult - 1.0f) * 0.60f) : size_mult;

      if (std::isfinite(orig_life) && orig_life > 0.01f && orig_life < 60.0f) {
        G<float>(def + 0x70u) = orig_life * life_scale;
      }
      if (std::isfinite(orig_vel))
        G<float>(def + 0x80u) = orig_vel * vel_scale;
      if (std::isfinite(orig_acc))
        G<float>(def + 0x88u) = orig_acc * vel_scale;
      if (std::isfinite(orig_r0) && orig_r0 > 0.0f)
        G<float>(def + 0xA4u) = orig_r0 * r0_scale;
      if (std::isfinite(orig_r1) && orig_r1 > 0.0f)
        G<float>(def + 0xA8u) = orig_r1 * r1_scale;
      if (std::isfinite(orig_r2) && orig_r2 > 0.0f)
        G<float>(def + 0xACu) = orig_r2 * r2_scale;

      __imp__sub_0015B380(ctx, base);

      G<float>(def + 0x70u) = orig_life;
      G<float>(def + 0x80u) = orig_vel;
      G<float>(def + 0x88u) = orig_acc;
      G<float>(def + 0xA4u) = orig_r0;
      G<float>(def + 0xA8u) = orig_r1;
      G<float>(def + 0xACu) = orig_r2;
      return;
    }
  }
  __imp__sub_0015B380(ctx, base);
}

// 2. Ribbon / Rocket Exhaust & Smoke Trail Renderer (sub_0016C9B0 in
// x86_recomp.38.cpp:4897) `edi` = trail instance, `ebx = R32(edi + 0x04)` =
// trail definition (+0x90 width, +0x9C lifetime, +0xA0/+0xA8 width scales).
X86_EXTERN_FUNC(__imp__sub_0016C9B0);
X86_FUNC(sub_0016C9B0) {
  float size_mult = 1.0f, density_mult = 1.0f;
  GetFireAndSmokeMultipliers(size_mult, density_mult);
  if ((std::fabs(size_mult - 1.0f) > 1e-3f ||
       std::fabs(density_mult - 1.0f) > 1e-3f) &&
      IsValidGuestAddr(ctx.edi, 0x20u)) {
    const uint32_t def = R32(ctx.edi + 0x04u);
    if (IsValidGuestAddr(def, 0xB0u)) {
      const float trail_w_mult = std::clamp(size_mult, 0.25f, 4.50f);
      const float trail_life_mult = std::clamp(density_mult, 0.25f, 3.50f);
      const float orig_w = G<float>(def + 0x90u);
      const float orig_life = G<float>(def + 0x9Cu);
      const float orig_s0 = G<float>(def + 0xA0u);
      const float orig_s1 = G<float>(def + 0xA8u);
      if (std::isfinite(orig_w))
        G<float>(def + 0x90u) = orig_w * trail_w_mult;
      if (std::isfinite(orig_life))
        G<float>(def + 0x9Cu) = orig_life * trail_life_mult;
      if (std::isfinite(orig_s0))
        G<float>(def + 0xA0u) = orig_s0 * trail_w_mult;
      if (std::isfinite(orig_s1))
        G<float>(def + 0xA8u) = orig_s1 * trail_w_mult;
      __imp__sub_0016C9B0(ctx, base);
      G<float>(def + 0x90u) = orig_w;
      G<float>(def + 0x9Cu) = orig_life;
      G<float>(def + 0xA0u) = orig_s0;
      G<float>(def + 0xA8u) = orig_s1;
      return;
    }
  }
  __imp__sub_0016C9B0(ctx, base);
}

// 3. CPU Billboard Particle Renderer (sub_0016A790 in x86_recomp.38.cpp:3)
// `eax` = particle count, `ecx` = particle array (stride 0x20: +0x0C world
// radius). IMPORTANT: Scale ONLY +0x0C (world radius). NEVER modify +0x1C (byte
// alpha) so radial soft edges stay circular!
X86_EXTERN_FUNC(__imp__sub_0016A790);
X86_FUNC(sub_0016A790) {
  float size_mult = 1.0f, density_mult = 1.0f;
  GetFireAndSmokeMultipliers(size_mult, density_mult);
  const int32_t count = static_cast<int32_t>(ctx.eax);
  const uint32_t arr = ctx.ecx;
  if (std::fabs(size_mult - 1.0f) > 1e-3f && count > 0 && count <= 512 &&
      IsValidGuestAddr(arr, static_cast<uint32_t>(count) * 0x20u)) {
    const float rad_mult = std::clamp(size_mult, 0.25f, 4.80f);
    float orig_rad[512];
    for (int32_t i = 0; i < count; i++) {
      const uint32_t p = arr + static_cast<uint32_t>(i) * 0x20u;
      orig_rad[i] = G<float>(p + 0x0Cu);
      if (std::isfinite(orig_rad[i])) {
        G<float>(p + 0x0Cu) = orig_rad[i] * rad_mult;
      }
    }
    __imp__sub_0016A790(ctx, base);
    for (int32_t i = 0; i < count; i++) {
      const uint32_t p = arr + static_cast<uint32_t>(i) * 0x20u;
      G<float>(p + 0x0Cu) = orig_rad[i];
    }
    return;
  }
  __imp__sub_0016A790(ctx, base);
}

// 4. Velocity-Stretched Spark & Fire Streak Particle Renderer (sub_0016A3D0 in
// x86_recomp.37.cpp:28112) `eax` = particle count, `R32(esp + 0x04)` = particle
// array (stride 0x20: +0x0C radius).
X86_EXTERN_FUNC(__imp__sub_0016A3D0);
X86_FUNC(sub_0016A3D0) {
  float size_mult = 1.0f, density_mult = 1.0f;
  GetFireAndSmokeMultipliers(size_mult, density_mult);
  const int32_t count = static_cast<int32_t>(ctx.eax);
  if (std::fabs(size_mult - 1.0f) > 1e-3f && count > 0 && count <= 512 &&
      IsValidGuestAddr(ctx.esp, 0x08u)) {
    const uint32_t arr = R32(ctx.esp + 0x04u);
    if (IsValidGuestAddr(arr, static_cast<uint32_t>(count) * 0x20u)) {
      const float rad_mult = std::clamp(size_mult, 0.25f, 4.20f);
      float orig_rad[512];
      for (int32_t i = 0; i < count; i++) {
        const uint32_t p = arr + static_cast<uint32_t>(i) * 0x20u;
        orig_rad[i] = G<float>(p + 0x0Cu);
        if (std::isfinite(orig_rad[i])) {
          G<float>(p + 0x0Cu) = orig_rad[i] * rad_mult;
        }
      }
      __imp__sub_0016A3D0(ctx, base);
      for (int32_t i = 0; i < count; i++) {
        const uint32_t p = arr + static_cast<uint32_t>(i) * 0x20u;
        G<float>(p + 0x0Cu) = orig_rad[i];
      }
      return;
    }
  }
  __imp__sub_0016A3D0(ctx, base);
}
