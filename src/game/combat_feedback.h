#pragma once

#include "game/combat_feedback_rules.h"

namespace game::combat_feedback {

void RecordPlayerHit(bool is_headshot, bool is_lethal);
void RecordDirectionalDamage(const float cam_pos[3], const float cam_fwd[3],
                             const float cam_right[3], const float source_pos[3],
                             float damage_norm);
void ConsumeCameraKineticKick(float& out_yaw_deg, float& out_pitch_deg);
void TriggerHitstop(bool is_headshot, bool is_lethal, uint8_t weapon_tier, float distance_m);
bool IsHitstopActive();
void StartActiveReload(float duration_sec);
float TryActiveReloadTap();
void StepCombatFeedbackFrame(float dt);
void DrawOverlay(float screen_w, float screen_h);

}  // namespace game::combat_feedback
