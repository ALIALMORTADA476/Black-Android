#pragma once

#include <cstdint>

namespace game::blood_decals {

struct CoagulationVisual {
    float spread_scale = 1.0f;
    float r = 0.14f;
    float g = 0.008f;
    float b = 0.012f;
    float a = 0.92f;
    float specular_sheen = 0.0f;
};

// Returns 0.0f when persistent_enabled == true (pools never expire on age);
// otherwise returns finite fallback lifetime in seconds.
float ComputePoolLifetimeSec(bool persistent_enabled) noexcept;

// Resolves the runtime pool ring-buffer budget (256..1024, default 512).
uint32_t ResolveMaxBloodPools(uint32_t requested) noexcept;

// Selects an inactive slot if available; otherwise evicts the best LRU pool
// balancing pool age (at least 8s settled preference) and distance from player.
uint32_t SelectEvictionSlot(const bool* active, const float* ages,
                            const float* dist_sq_to_player, uint32_t count) noexcept;

// Models physical arterial spread (0..1.2s) and hemoglobin oxidation/coagulation:
// wet glossy crimson when fresh -> thick oxidized claret/maroon without fading out.
CoagulationVisual EvaluatePoolCoagulation(float age_sec, float base_opacity) noexcept;

// Returns drip emission cadence (seconds) for staggering/limping wounded soldiers.
float ComputeWoundDripIntervalSec(float bleed_intensity) noexcept;

}  // namespace game::blood_decals
