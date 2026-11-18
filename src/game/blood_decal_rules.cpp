#include "game/blood_decal_rules.h"

#include <algorithm>
#include <cmath>

namespace game::blood_decals {

float ComputePoolLifetimeSec(bool persistent_enabled) noexcept {
    // 0.0f = infinite lifetime while in-level; pools only recycle on LRU overflow.
    return persistent_enabled ? 0.0f : 10.0f;
}

uint32_t ResolveMaxBloodPools(uint32_t requested) noexcept {
    if (requested == 0u) return 512u;
    return std::clamp(requested, 256u, 1024u);
}

uint32_t SelectEvictionSlot(const bool* active, const float* ages,
                            const float* dist_sq_to_player, uint32_t count) noexcept {
    if (!active || !ages || !dist_sq_to_player || count == 0u) return 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        if (!active[i]) return i;
    }

    uint32_t best_idx = 0u;
    float best_score = -1.0f;
    for (uint32_t i = 0u; i < count; ++i) {
        const float age = std::max(0.0f, ages[i]);
        const float dist_sq = std::max(0.0f, dist_sq_to_player[i]);
        // Protect fresh close-range blood splatters for at least 8s unless everything is close.
        const float settled_bonus = (age >= 8.0f) ? 2.5f : 0.25f;
        const float age_factor = std::min(age, 180.0f) * 0.22f;
        const float dist_factor = std::sqrt(dist_sq) * 3.0f;
        const float score = (age_factor + dist_factor) * settled_bonus;
        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }
    return best_idx;
}

CoagulationVisual EvaluatePoolCoagulation(float age_sec, float base_opacity) noexcept {
    const float age = std::max(0.0f, age_sec);
    const float op = std::clamp(base_opacity, 0.0f, 1.0f);

    // Rapid hydraulic arterial expansion over first ~1.2 seconds.
    const float spread = 0.50f + 0.50f * (1.0f - std::exp(-age / 0.32f));

    // Hemoglobin deoxygenation + edge drying curve over ~18 seconds, then permanently stable.
    const float coag = std::clamp(1.0f - std::exp(-age / 5.5f), 0.0f, 1.0f);

    // Vivid oxyhemoglobin arterial crimson when fresh -> rich dark claret when coagulated (never pitch black!)
    const float fresh_r = 0.860f, fresh_g = 0.095f, fresh_b = 0.115f;
    const float dried_r = 0.540f, dried_g = 0.052f, dried_b = 0.068f;

    CoagulationVisual v{};
    v.spread_scale = std::clamp(spread, 0.45f, 1.0f);
    v.r = fresh_r + (dried_r - fresh_r) * coag;
    v.g = fresh_g + (dried_g - fresh_g) * coag;
    v.b = fresh_b + (dried_b - fresh_b) * coag;
    // Dried blood keeps full grounded stain contrast with no timed fade-out.
    v.a = op * (0.96f - 0.04f * coag);
    v.specular_sheen = std::clamp(0.88f * (1.0f - coag) * (1.0f - 0.65f * coag), 0.03f, 0.90f);
    return v;
}

float ComputeWoundDripIntervalSec(float bleed_intensity) noexcept {
    const float t = std::clamp(bleed_intensity, 0.0f, 1.0f);
    return 0.85f + (0.22f - 0.85f) * t;
}

}  // namespace game::blood_decals
