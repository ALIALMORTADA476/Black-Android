#pragma once

#include <cstdint>

struct X86Context;
union X86Xmm;

namespace game::delirium_phys {

void OnBulletHit(const X86Context& ctx, uint8_t* base, uint32_t victim,
                 uint32_t attacker, uint32_t hit_dir_ea, float force_val,
                 uint8_t zone_byte) noexcept;

void OnExplosionShockwave(const float blast_pos[3], float blast_radius,
                          float blast_force) noexcept;

void OnEnemyFrameStep(X86Context& ctx, uint8_t* base, uint32_t who,
                      float dt) noexcept;

void TriggerExternalAgony(X86Context& ctx, uint8_t* base, uint32_t who,
                          uint32_t clip_id) noexcept;

bool ApplyDeathCollapseVariation(uint32_t approx_owner,
                                 X86Xmm& vel_inout) noexcept;

uint8_t GetDismemberMask(uint32_t soldier_ea) noexcept;
void TrackSoldierPtr(uint32_t soldier_ea) noexcept;
void OnPlayerTracerNearSoldiers(const float start[3], const float end[3]) noexcept;
void OnGrenadeWorldThreat(const float grenade_pos[3]) noexcept;

void ResetAll() noexcept;

}  // namespace game::delirium_phys
