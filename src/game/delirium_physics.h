#pragma once

#include <cstdint>

struct X86Context;
union X86Xmm;

namespace game::delirium_phys {

void OnBulletHit(const X86Context& ctx, uint8_t* base, uint32_t victim,
                 uint32_t attacker, uint32_t hit_dir_ea, float force_val,
                 uint8_t zone_byte) noexcept;

void OnEnemyFrameStep(X86Context& ctx, uint8_t* base, uint32_t who,
                      float dt) noexcept;

void TriggerExternalAgony(X86Context& ctx, uint8_t* base, uint32_t who,
                          uint32_t clip_id) noexcept;

bool ApplyDeathCollapseVariation(uint32_t approx_owner,
                                 X86Xmm& vel_inout) noexcept;

void ResetAll() noexcept;

}  // namespace game::delirium_phys
