#pragma once

#include <cstdint>

struct X86Context;
namespace input {
struct Pad;
}

namespace game::flamethrower {

struct HudState {
    bool active = false;
    bool firing = false;
    bool reloading = false;
    float fuel_pct = 100.0f;
    float reload_progress = 0.0f;
    float pressure = 0.0f;
    uint32_t active_burning_enemies = 0;
};

// Initializes/loads weapons/flamethrower.bmsh lazily on first use.
void EnsureLoaded();

// State query & standalone equip/unequip controls (never replaces existing guns).
bool IsActive();
void SetActive(bool enable);
void Toggle();
void ForceDeactivate();
void RefillFuel();
void ResetLevelState();

// Intercepts Fire (RT / LMB) and Reload (A / R) while Flamethrower is active so the
// underlying hidden weapon neither shoots bullets nor wastes real ammunition.
void FilterGameplayPad(int port, input::Pad& pad);

// Advances simulation, renders 3D lit Flamethrower viewmodel, and emits pilot + jet flames.
// Called on guest thread inside sub_00176F60 when kGunDrawList is intercepted.
void RenderAndStepFrame(X86Context& ctx, uint8_t* base);

// Evaluates enemy cone exposure, fire DoT, burning agony reactions, and attached flames/smoke.
// Called on guest thread inside sub_001DFD90 after each character's native update.
void OnActorStep(X86Context& ctx, uint8_t* base, uint32_t who, float dt);

// Mixes procedural flamethrower audio directly on the mixer thread into out_channels interleaved frames.
void RenderAudioMix(float* out, uint32_t frames, uint32_t sample_rate,
                    uint32_t out_channels, uint32_t ch_left, uint32_t ch_right);

// Snapshot for HUD / touch gauge & settings panel telemetry.
HudState GetHudState();

}  // namespace game::flamethrower
