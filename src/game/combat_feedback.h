#pragma once

#include "game/combat_feedback_rules.h"

namespace game::combat_feedback {

void RecordPlayerHit(bool is_headshot, bool is_lethal);
void DrawOverlay(float screen_w, float screen_h);

}  // namespace game::combat_feedback
