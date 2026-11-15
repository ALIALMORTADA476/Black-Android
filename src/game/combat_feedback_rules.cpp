#include "game/combat_feedback_rules.h"

#include <algorithm>
#include <cmath>

namespace game::combat_feedback {

HitKind ClassifyHit(bool is_headshot, bool is_lethal) {
    if (is_lethal && is_headshot) return HitKind::HeadshotKill;
    if (is_lethal) return HitKind::Kill;
    if (is_headshot) return HitKind::Headshot;
    return HitKind::Body;
}

float HitmarkerDurationSec(HitKind kind) {
    switch (kind) {
        case HitKind::Body: return 0.135f;
        case HitKind::Headshot: return 0.165f;
        case HitKind::Kill: return 0.215f;
        case HitKind::HeadshotKill: return 0.240f;
    }
    return 0.135f;
}

HitmarkerVisualSpec EvaluateHitmarkerVisual(float age_sec, HitKind kind, float ui_scale) {
    HitmarkerVisualSpec spec;
    spec.kind = kind;
    const float ttl = HitmarkerDurationSec(kind);
    if (!std::isfinite(age_sec) || age_sec < 0.0f || age_sec >= ttl) {
        return spec;
    }

    const float scale = std::clamp(std::isfinite(ui_scale) ? ui_scale : 1.0f, 0.70f, 2.50f);
    const float u = std::clamp(age_sec / ttl, 0.0f, 1.0f);

    // Quick Awwwards-style spring punch-out over first 28ms, then tight settle toward reticle ring.
    const float snap = 1.0f - std::exp(-age_sec * 85.0f);
    const float impulse = std::exp(-age_sec * 24.0f) * snap;
    const float settle = 1.0f - u;

    // Fade holds high contrast for first 45% then tapers smoothly via cubic ease-out.
    const float fade_t = std::clamp((u - 0.35f) / 0.65f, 0.0f, 1.0f);
    spec.alpha = std::clamp(snap * (1.0f - fade_t * fade_t), 0.0f, 1.0f);
    if (spec.alpha <= 0.01f) {
        spec.active = false;
        return spec;
    }

    spec.active = true;
    const float base_gap = (kind == HitKind::Kill || kind == HitKind::HeadshotKill) ? 8.2f : 7.0f;
    const float expand_px = (kind == HitKind::HeadshotKill ? 4.8f : 3.2f) * impulse + 1.2f * settle;
    spec.inner_gap_px = (base_gap + expand_px) * scale;

    switch (kind) {
        case HitKind::Body:
            spec.tick_len_px = 5.5f * scale;
            spec.thickness_px = 1.85f * scale;
            spec.r = 240; spec.g = 244; spec.b = 248;
            break;
        case HitKind::Headshot:
            spec.tick_len_px = 7.2f * scale;
            spec.thickness_px = 2.25f * scale;
            spec.r = 255; spec.g = 204; spec.b = 72;  // crisp tactical amber brass
            break;
        case HitKind::Kill:
            spec.tick_len_px = 7.5f * scale;
            spec.thickness_px = 2.35f * scale;
            spec.has_outer_kill_wing = true;
            spec.outer_wing_gap_px = spec.inner_gap_px + spec.tick_len_px + 2.8f * scale;
            spec.r = 255; spec.g = 236; spec.b = 224;
            spec.wing_r = 255; spec.wing_g = 78; spec.wing_b = 52;
            break;
        case HitKind::HeadshotKill:
            spec.tick_len_px = 8.4f * scale;
            spec.thickness_px = 2.55f * scale;
            spec.has_outer_kill_wing = true;
            spec.outer_wing_gap_px = spec.inner_gap_px + spec.tick_len_px + 3.4f * scale;
            spec.r = 255; spec.g = 212; spec.b = 84;
            spec.wing_r = 255; spec.wing_g = 62; spec.wing_b = 42;
            break;
    }

    return spec;
}

float ComputeEnhancedRecoilDecayFactor(float base_factor, float frame_share,
                                       bool enhanced_recoil, bool no_recoil) {
    if (no_recoil) return 0.0f;
    if (!std::isfinite(base_factor) || base_factor <= 0.0f) return 0.0f;
    if (!std::isfinite(frame_share) || frame_share <= 0.0f) return std::clamp(base_factor, 0.0f, 0.9995f);

    const float clamped_base = std::clamp(base_factor, 0.001f, 0.9995f);
    // Enhanced Recoil Curves tighten settle exponent by 1.38x so rapid bursts feel punchy & recover crisply.
    const float exp_mul = enhanced_recoil ? 1.38f : 1.0f;
    return std::clamp(std::pow(clamped_base, frame_share * exp_mul), 0.0f, 0.9995f);
}

float ComputeEnhancedRecoilKickScale(bool enhanced_recoil, bool no_recoil,
                                     uint64_t shot_seq, float& out_yaw_kick_deg) {
    out_yaw_kick_deg = 0.0f;
    if (no_recoil) return 0.0f;
    if (!enhanced_recoil) return 1.0f;

    // Alternating low-amplitude horizontal signature breaks sterile vertical robot ladder without harming aim.
    uint64_t h = shot_seq * 6364136223846793005ULL + 1442695040888963407ULL;
    h ^= (h >> 29);
    const float signed_unit = (float(int32_t(h & 0xFFFFu)) / 32767.5f) - 1.0f;
    const float parity = (shot_seq & 1ULL) ? 1.0f : -1.0f;
    out_yaw_kick_deg = std::clamp(parity * (0.07f + 0.09f * std::fabs(signed_unit)), -0.18f, 0.18f);
    return 1.12f;
}

}  // namespace game::combat_feedback
