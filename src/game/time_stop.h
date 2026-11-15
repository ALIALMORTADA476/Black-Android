#pragma once

#include <cstdint>

namespace game {

// Helper implemented in esp.cpp using the active scene view/projection matrices.
bool ProjectWorldPoint(const float world_pos[3], float out_norm_xy[2]);

namespace time_stop {

struct TelemetrySnapshot {
    bool frozen = false;
    bool on_cooldown = false;
    bool in_gameplay = false;
    float elapsed_sec = 0.0f;
    float remaining_sec = 0.0f;
    float cooldown_sec = 0.0f;
    uint32_t frozen_enemies = 0;
    uint32_t frozen_bodies = 0;
    uint32_t frozen_bullets = 0;
    uint32_t frozen_grenades = 0;
    uint32_t frozen_rockets = 0;
    uint32_t queued_overflow_bullets = 0;
    uint64_t skipped_ai_ticks = 0;
    uint64_t skipped_physics_ticks = 0;
    uint64_t skipped_particle_ticks = 0;
};

// Lock-free hot-path queries for X86_FUNC hooks.
bool IsFrozen();
bool IsActiveGameplay();

// State & control triggers (callable from menu, keyboard hotkeys, or touch HUD).
void SetMenuEnabled(bool enable);
void ForceResume(bool apply_cooldown = false);
void OnTriggerPressed();
void SetHoldInput(bool held);

// Telemetry & overflow helpers invoked by cooperated hooks (esp.cpp / hooks.cpp / time_stop).
void RecordFrozenEnemy(uint32_t enemy_ptr);
void RecordSkippedAiTick();
void RecordSkippedPhysicsTick();
void RecordSkippedParticleTick();
bool TryQueueOverflowBullet(uint32_t mgr, uint32_t muzzle_ea, uint32_t fire_pkt, uint32_t dir_ea,
                            float range, float speed, uint8_t flags, uint32_t caller_esp);

// Visual HUD badge + suspended physical bullet spatial indicators.
void DrawOverlay(float screen_w, float screen_h);

TelemetrySnapshot GetTelemetry();

}  // namespace time_stop
}  // namespace game
