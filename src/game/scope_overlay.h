#pragma once

namespace game {
namespace scope_overlay {

// Draws the ZF39 sniper optic mask and the German No. 1 three-post reticle over
// the frame while the Mauser 1891-12 is shouldered. Pure overlay: it reads the
// arsenal HUD snapshot, it never touches the scene or the weapon state.
void DrawOverlay(float screen_w, float screen_h);

}  // namespace scope_overlay
}  // namespace game
