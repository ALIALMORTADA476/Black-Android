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
}

}  // namespace game::combat_feedback
