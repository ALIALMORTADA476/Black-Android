#pragma once

#include <cstddef>
#include <cstdint>

struct X86Context;

namespace game {

// Thread-safe copy of recently active living enemy pointers from esp.cpp.
size_t CopyLivingEnemies(uint32_t* out_who, size_t max_count);

namespace overdrive {

bool NeedsEnemyTracking() noexcept;
bool InSplashOrBurn() noexcept;

// Triggers callable from Touch HUD, Keyboard hotkeys, or Mod Menu
void TriggerKineticDash();
void TriggerOverdriveSurge();
void CycleSpecialAmmoMode();
void ToggleThermalVisor();
void SetTouchStickInput(float lx, float ly);
void ResetLevelState();

// Player frame update hook (called inside sub_001E5FA0 before __imp__sub_001E5FA0)
void OnPlayerFramePreUpdate(X86Context& ctx, uint8_t* base, uint32_t player_ea);

// Enemy frame update hook (called inside sub_001DFD90 after __imp__sub_001DFD90)
void OnEnemyFrameUpdate(X86Context& ctx, uint8_t* base, uint32_t who, float dt);

// Bullet-hit modifiers & reactive combat payloads (called inside sub_001E74B0)
void ModifyPlayerBulletHit(uint32_t victim_ea, bool is_headshot, float& raw_force, float& hp_scale);
void OnPlayerBulletPostHit(X86Context& ctx, uint8_t* base, uint32_t victim_ea, uint32_t me_ea,
                           const float hit_pos[3], const float hit_dir[3],
                           bool is_headshot, bool is_lethal);

// Real-time multipliers & Vulkan shader parameters
float GetWeaponWaitMultiplier() noexcept;
float GetDashFovBoostDeg() noexcept;
float GetWorldTimeScaleMultiplier() noexcept;
void GetShaderPostParams(float& out_thermal_mix, float& out_overdrive_edge) noexcept;

// HUD overlay (Dash speed lines, Thermal threat brackets, Overdrive/Ammo/Streak HUD telemetry)
void DrawHudOverlay(float screen_w, float screen_h);

struct HudSnapshot {
    bool enabled = true;
    bool overdrive_active = false;
    float charge_pct = 0.0f;
    float overdrive_sec = 0.0f;
    uint32_t streak = 0;
    float dash_cooldown = 0.0f;
    bool dashing = false;
    uint32_t ammo_mode = 0;
    const char* ammo_short = "STD";
    bool thermal_on = false;
};

HudSnapshot GetHudSnapshot() noexcept;

}  // namespace overdrive
}  // namespace game
