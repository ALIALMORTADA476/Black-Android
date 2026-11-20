#include "game/combat_feedback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

#include <imgui.h>

#include "common/mod_schema.h"
#include "common/options.h"
#include "game/time_stop.h"
#include "sound/weapon_audio.h"

namespace app {
bool MenuOpen();
}

namespace game {
bool HudShown();
}

namespace game::combat_feedback {

namespace {

constexpr float kSqrt2Inv = 0.70710678f;

std::mutex g_hit_mutex;
bool g_has_hit = false;
HitKind g_hit_kind = HitKind::Body;
std::chrono::steady_clock::time_point g_hit_tp{};

struct DamageArcSlot {
    bool active = false;
    float rel_angle_rad = 0.0f;
    float intensity = 0.0f;
    std::chrono::steady_clock::time_point tp{};
};
DamageArcSlot g_damage_arcs[6]{};
size_t g_arc_cursor = 0;

float g_pending_yaw_kick = 0.0f;
float g_pending_pitch_kick = 0.0f;
std::chrono::steady_clock::time_point g_hitstop_until{};
ActiveReloadState g_active_reload{};
float g_active_reload_flash = 0.0f;
bool g_active_reload_flash_ok = false;

}  // namespace

void RecordPlayerHit(bool is_headshot, bool is_lethal) {
    const HitKind kind = ClassifyHit(is_headshot, is_lethal);
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_hit_mutex);
        // Never allow a non-lethal follow-up frame on the same burst to downgrade an active kill-confirm flash
        if (g_has_hit) {
            const float cur_age = std::chrono::duration<float>(now - g_hit_tp).count();
            if (cur_age < HitmarkerDurationSec(g_hit_kind) &&
                uint8_t(kind) < uint8_t(g_hit_kind) && cur_age < 0.075f) {
                return;
            }
        }
        g_has_hit = true;
        g_hit_kind = kind;
        g_hit_tp = now;
    }

    if (modsettings::Registry().GetEnhanced().hitmarker_feedback) {
        sound::weapon_audio::TriggerHitConfirm(is_headshot, is_lethal);
    }
}

void RecordDirectionalDamage(const float cam_pos[3], const float cam_fwd[3],
                             const float cam_right[3], const float source_pos[3],
                             float damage_norm) {
    const auto& enh = modsettings::Registry().GetEnhanced();
    if (!enh.directional_damage_indicator) return;
    float angle = 0.0f, yaw_k = 0.0f, pitch_k = 0.0f, roll_k = 0.0f;
    ComputeDirectionalDamageImpulse(cam_pos, cam_fwd, cam_right, source_pos, damage_norm,
                                    angle, yaw_k, pitch_k, roll_k);
    std::lock_guard<std::mutex> lock(g_hit_mutex);
    DamageArcSlot& slot = g_damage_arcs[g_arc_cursor % 6u];
    g_arc_cursor = (g_arc_cursor + 1u) % 6u;
    slot.active = true;
    slot.rel_angle_rad = angle;
    slot.intensity = std::clamp(damage_norm, 0.25f, 1.0f);
    slot.tp = std::chrono::steady_clock::now();
    g_pending_yaw_kick = std::clamp(g_pending_yaw_kick + yaw_k, -4.5f, 4.5f);
    g_pending_pitch_kick = std::clamp(g_pending_pitch_kick + pitch_k, -3.5f, 3.5f);
}

void ConsumeCameraKineticKick(float& out_yaw_deg, float& out_pitch_deg) {
    out_yaw_deg = 0.0f;
    out_pitch_deg = 0.0f;
    std::lock_guard<std::mutex> lock(g_hit_mutex);
    out_yaw_deg = g_pending_yaw_kick;
    out_pitch_deg = g_pending_pitch_kick;
    g_pending_yaw_kick = 0.0f;
    g_pending_pitch_kick = 0.0f;
}

void TriggerHitstop(bool is_headshot, bool is_lethal, uint8_t weapon_tier, float distance_m) {
    if (!modsettings::Registry().GetEnhanced().tactical_hitstop_enabled) return;
    const float sec = EvaluateHitstopDurationSec(is_headshot, is_lethal, weapon_tier, distance_m);
    if (sec <= 0.004f) return;
    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::microseconds(int64_t(sec * 1000000.0f));
    std::lock_guard<std::mutex> lock(g_hit_mutex);
    if (until > g_hitstop_until) g_hitstop_until = until;
}

bool IsHitstopActive() {
    if (!modsettings::Registry().GetEnhanced().tactical_hitstop_enabled) return false;
    std::lock_guard<std::mutex> lock(g_hit_mutex);
    return std::chrono::steady_clock::now() < g_hitstop_until;
}

void StartActiveReload(float duration_sec) {
    if (!modsettings::Registry().GetEnhanced().active_reload_enabled) return;
    std::lock_guard<std::mutex> lock(g_hit_mutex);
    g_active_reload.Start(duration_sec);
    g_active_reload_flash = 0.0f;
}

float TryActiveReloadTap() {
    if (!modsettings::Registry().GetEnhanced().active_reload_enabled) return 1.0f;
    ActiveReloadTapResult res;
    {
        std::lock_guard<std::mutex> lock(g_hit_mutex);
        res = g_active_reload.TryTap(g_active_reload.Elapsed());
        if (res.handled) {
            g_active_reload_flash = 0.45f;
            g_active_reload_flash_ok = res.success;
        }
    }
    if (res.handled && res.success) {
        sound::weapon_audio::TriggerHitConfirm(true, false);
        return res.remaining_scale;
    }
    return res.handled ? res.remaining_scale : 1.0f;
}

void StepCombatFeedbackFrame(float dt) {
    if (!std::isfinite(dt) || dt <= 0.0f) return;
    std::lock_guard<std::mutex> lock(g_hit_mutex);
    g_active_reload.Step(dt);
    if (g_active_reload_flash > 0.0f) {
        g_active_reload_flash = std::max(0.0f, g_active_reload_flash - dt);
    }
}

void DrawOverlay(float screen_w, float screen_h) {
    if (screen_w <= 1.0f || screen_h <= 1.0f) return;
    if (!time_stop::IsActiveGameplay() || game::g_hide_hud || app::MenuOpen() || !game::HudShown()) {
        return;
    }
    const auto& enh = modsettings::Registry().GetEnhanced();
    if (!enh.hitmarker_feedback || enh.hide_hud_on_screenshots) {
        return;
    }

    HitKind kind = HitKind::Body;
    float age = -1.0f;
    {
        std::lock_guard<std::mutex> lock(g_hit_mutex);
        if (!g_has_hit) return;
        age = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_hit_tp).count();
        kind = g_hit_kind;
        if (age >= HitmarkerDurationSec(kind)) {
            g_has_hit = false;
            return;
        }
    }

    const float ui_scale = std::clamp(screen_h / 720.0f, 0.85f, 2.20f);
    const HitmarkerVisualSpec spec = EvaluateHitmarkerVisual(age, kind, ui_scale);
    if (!spec.active) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    const float cx = screen_w * 0.5f;
    const float cy = screen_h * 0.5f;
    const uint8_t a8 = uint8_t(std::clamp(spec.alpha * 245.0f, 0.0f, 255.0f));
    const uint8_t shadow_a8 = uint8_t(std::clamp(spec.alpha * 160.0f, 0.0f, 255.0f));
    const ImU32 col_main = IM_COL32(spec.r, spec.g, spec.b, a8);
    const ImU32 col_shadow = IM_COL32(10, 12, 16, shadow_a8);

    static constexpr float kDirs[4][2] = {
        {-kSqrt2Inv, -kSqrt2Inv},
        { kSqrt2Inv, -kSqrt2Inv},
        {-kSqrt2Inv,  kSqrt2Inv},
        { kSqrt2Inv,  kSqrt2Inv},
    };

    for (const auto& d : kDirs) {
        const ImVec2 p0(cx + d[0] * spec.inner_gap_px, cy + d[1] * spec.inner_gap_px);
        const ImVec2 p1(cx + d[0] * (spec.inner_gap_px + spec.tick_len_px),
                        cy + d[1] * (spec.inner_gap_px + spec.tick_len_px));
        dl->AddLine(p0, p1, col_shadow, spec.thickness_px + 1.6f);
        dl->AddLine(p0, p1, col_main, spec.thickness_px);

        if (spec.has_outer_kill_wing) {
            const float w_len = 4.4f * ui_scale;
            const ImVec2 w0(cx + d[0] * spec.outer_wing_gap_px,
                            cy + d[1] * spec.outer_wing_gap_px);
            const ImVec2 w1(cx + d[0] * (spec.outer_wing_gap_px + w_len),
                            cy + d[1] * (spec.outer_wing_gap_px + w_len));
            const ImU32 col_wing = IM_COL32(spec.wing_r, spec.wing_g, spec.wing_b, a8);
            dl->AddLine(w0, w1, col_shadow, spec.thickness_px + 1.4f);
            dl->AddLine(w0, w1, col_wing, spec.thickness_px);
        }
    }

    // Directional Damage Arc Chevrons around screen center (shows attacker exact angle)
    if (enh.directional_damage_indicator) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(g_hit_mutex);
        const float arc_rad = std::min(screen_w, screen_h) * 0.24f;
        for (auto& arc : g_damage_arcs) {
            if (!arc.active) continue;
            const float a = std::chrono::duration<float>(now - arc.tp).count();
            if (a >= 1.10f) {
                arc.active = false;
                continue;
            }
            const float fade = std::clamp(1.0f - (a / 1.10f), 0.0f, 1.0f);
            const float alpha = fade * fade * arc.intensity;
            const float ang = arc.rel_angle_rad - 1.5707963f; // 0 rad = top (-Y on screen)
            const float ca = std::cos(ang), sa = std::sin(ang);
            const float px = -sa, py = ca;
            const ImVec2 tip(cx + ca * (arc_rad + 14.0f * ui_scale),
                             cy + sa * (arc_rad + 14.0f * ui_scale));
            const ImVec2 wL(cx + ca * arc_rad + px * 13.0f * ui_scale,
                            cy + sa * arc_rad + py * 13.0f * ui_scale);
            const ImVec2 wR(cx + ca * arc_rad - px * 13.0f * ui_scale,
                            cy + sa * arc_rad - py * 13.0f * ui_scale);
            const ImU32 col_arc = IM_COL32(238, 46, 38, uint8_t(alpha * 225.0f));
            dl->AddTriangleFilled(tip, wL, wR, col_arc);
        }
    }

    // Active Reload tactical bar beneath reticle during reload
    if (enh.active_reload_enabled) {
        bool ar_on = false, ar_ok = false;
        float ar_prog = 0.0f, ar_flash = 0.0f;
        {
            std::lock_guard<std::mutex> lock(g_hit_mutex);
            ar_on = g_active_reload.IsActive();
            ar_prog = g_active_reload.Progress();
            ar_flash = g_active_reload_flash;
            ar_ok = g_active_reload_flash_ok;
        }
        if (ar_on || ar_flash > 0.01f) {
            const float bw = 126.0f * ui_scale;
            const float bh = 6.0f * ui_scale;
            const float bx0 = cx - bw * 0.5f;
            const float by0 = cy + 38.0f * ui_scale;
            dl->AddRectFilled(ImVec2(bx0 - 2.0f, by0 - 2.0f),
                              ImVec2(bx0 + bw + 2.0f, by0 + bh + 2.0f),
                              IM_COL32(12, 14, 18, 185), 3.0f);
            // Golden tactical sweet-spot zone [0.42..0.64]
            const float zx0 = bx0 + bw * ActiveReloadState::kBandMin;
            const float zx1 = bx0 + bw * ActiveReloadState::kBandMax;
            dl->AddRectFilled(ImVec2(zx0, by0), ImVec2(zx1, by0 + bh),
                              IM_COL32(236, 188, 68, 200), 1.5f);
            // Moving cursor
            const float cur_x = bx0 + bw * std::clamp(ar_prog, 0.0f, 1.0f);
            const ImU32 cur_col = (ar_flash > 0.01f)
                ? (ar_ok ? IM_COL32(95, 240, 140, 255) : IM_COL32(245, 72, 62, 255))
                : IM_COL32(242, 245, 250, 255);
            dl->AddRectFilled(ImVec2(cur_x - 2.0f * ui_scale, by0 - 3.0f),
                              ImVec2(cur_x + 2.0f * ui_scale, by0 + bh + 3.0f),
                              cur_col, 1.0f);
        }
    }
}

}  // namespace game::combat_feedback
