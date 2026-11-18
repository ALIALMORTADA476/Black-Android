#include "game/scope_overlay.h"

#include <algorithm>
#include <cmath>

#include <imgui.h>

#include "common/options.h"
#include "game/flamethrower.h"
#include "game/time_stop.h"

namespace app {
bool MenuOpen();
}

namespace game {
namespace scope_overlay {
namespace {

constexpr float kTwoPi = 6.283185307179586f;

// game::flamethrower::HudState::weapon_id slot for the Mauser 1891-12.
constexpr uint8_t kMauser1891 = 1u;
// The optic only resolves once the rifle is nearly shouldered, so the mask
// fades in with the ADS blend instead of popping on at the first frame.
constexpr float kOnsetAds = 0.55f;
constexpr float kFullAds = 0.92f;

ImU32 Black(float alpha) {
  return IM_COL32(0, 0, 0, int(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f));
}

// Radial band between two radii, built from quads: an annulus is not convex, so
// ImDrawList has no single primitive for it.
void BandRing(ImDrawList* dl, float cx, float cy, float inner, float outer,
              float alpha, int segments = 112) {
  const ImU32 col = Black(alpha);
  float prev_ix = cx + inner, prev_iy = cy;
  float prev_ox = cx + outer, prev_oy = cy;
  for (int i = 1; i <= segments; ++i) {
    const float t = kTwoPi * float(i) / float(segments);
    const float c = std::cos(t), s = std::sin(t);
    const float ix = cx + c * inner, iy = cy + s * inner;
    const float ox = cx + c * outer, oy = cy + s * outer;
    dl->AddQuadFilled(ImVec2(prev_ix, prev_iy), ImVec2(ix, iy), ImVec2(ox, oy),
                      ImVec2(prev_ox, prev_oy), col);
    prev_ix = ix;
    prev_iy = iy;
    prev_ox = ox;
    prev_oy = oy;
  }
}

// German No. 1 (ZF39) three-post reticle: bottom, left and right posts with a
// fine crosshair and a sharp aiming tip in the centre.
void DrawReticle(ImDrawList* dl, float cx, float cy, float r, float alpha, float zoom_x, float zero_m) {
  const ImU32 col = Black(alpha);
  const float post = std::max(2.2f, r * 0.032f);
  const float hair = std::max(1.0f, post * 0.28f);

  // Tapered German No. 1 (ZF39) heavy horizontal & bottom posts
  dl->AddLine(ImVec2(cx, cy + r * 0.96f), ImVec2(cx, cy + r * 0.16f), col, post * 1.15f);
  dl->AddLine(ImVec2(cx - r * 0.96f, cy), ImVec2(cx - r * 0.22f, cy), col, post);
  dl->AddLine(ImVec2(cx + r * 0.96f, cy), ImVec2(cx + r * 0.22f, cy), col, post);

  // Fine reticle hairlines + vertical elevation stadia ticks
  dl->AddLine(ImVec2(cx, cy + r * 0.16f), ImVec2(cx, cy - r * 0.88f), col, hair);
  dl->AddLine(ImVec2(cx - r * 0.22f, cy), ImVec2(cx + r * 0.22f, cy), col, hair);
  for (int t = 1; t <= 4; ++t) {
    const float off = r * (0.09f * float(t));
    const float w = (t % 2 == 0) ? r * 0.036f : r * 0.020f;
    dl->AddLine(ImVec2(cx - w, cy - off), ImVec2(cx + w, cy - off), col, hair);
    dl->AddLine(ImVec2(cx - off, cy - w * 0.55f), ImVec2(cx - off, cy + w * 0.55f), col, hair);
    dl->AddLine(ImVec2(cx + off, cy - w * 0.55f), ImVec2(cx + off, cy + w * 0.55f), col, hair);
  }

  // Sharp German pointed top triangle (aiming tip)
  const float tip = post * 1.15f;
  dl->AddTriangleFilled(ImVec2(cx, cy - tip * 0.55f), ImVec2(cx - tip, cy + tip * 1.1f),
                        ImVec2(cx + tip, cy + tip * 1.1f), col);

  // Crisp magnification & zeroing optical readout inside lower scope bevel
  char badge[48];
  std::snprintf(badge, sizeof(badge), "ZF39  %.0fX   %dm", zoom_x, int(zero_m + 0.5f));
  const float fs = std::max(13.0f, r * 0.055f);
  ImFont* font = ImGui::GetFont();
  const ImVec2 ts = font ? font->CalcTextSizeA(fs, 1000.0f, 0.0f, badge) : ImVec2(90.0f, fs);
  const ImU32 badge_col = IM_COL32(228, 214, 178, int(alpha * 215.0f + 0.5f));
  dl->AddText(font, fs, ImVec2(cx - ts.x * 0.5f, cy + r * 0.62f), badge_col, badge);
}

}  // namespace

void DrawOverlay(float screen_w, float screen_h) {
  if (screen_w <= 1.0f || screen_h <= 1.0f) return;
  if (game::g_hide_hud) return;
  if (app::MenuOpen() || !game::HudShown() || !time_stop::IsActiveGameplay()) return;

  const game::flamethrower::HudState hud = game::flamethrower::GetHudState();
  if (!hud.active || hud.weapon_id != kMauser1891) return;

  const float strength =
      std::clamp((hud.ads_blend - kOnsetAds) / (kFullAds - kOnsetAds), 0.0f, 1.0f);
  if (strength <= 0.0f) return;

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  if (!dl) return;

  const float cx = screen_w * 0.5f;
  const float cy = screen_h * 0.5f;
  const float r = std::min(screen_w, screen_h) * 0.42f;

  // Opaque tube mask: everything outside the sight picture goes black.
  BandRing(dl, cx, cy, r, std::sqrt(cx * cx + cy * cy) + 4.0f, strength);

  // Eye-box shadow -- the sight picture darkens smoothly toward the rim.
  BandRing(dl, cx, cy, r * 0.91f, r, strength * 0.50f);
  BandRing(dl, cx, cy, r * 0.84f, r * 0.91f, strength * 0.22f);

  // Optic brass/steel rim highlight.
  dl->AddCircle(ImVec2(cx, cy), std::max(1.0f, r - 1.0f),
                IM_COL32(18, 20, 24, int(strength * 230.0f + 0.5f)), 128, 3.0f);

  DrawReticle(dl, cx, cy, r, strength, hud.scope_zoom_x, hud.scope_zero_m);
}

}  // namespace scope_overlay
}  // namespace game
