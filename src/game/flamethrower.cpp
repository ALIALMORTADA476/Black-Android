#include "game/flamethrower.h"

namespace game::flamethrower {

void EnsureLoaded() {}

bool IsActive() { return false; }

void SetActive(bool) {}

void Toggle() {}

void ForceDeactivate() {}

void RefillFuel() {}

void ResetLevelState() {}

void SelectSpecialWeapon(uint8_t) {}

void CycleSpecialWeapon(int) {}

void ToggleFireMode() {}

void CycleScopeZoom() {}

void TriggerManualReload() {}

void SetSpecialAiming(bool) {}

float GetAdsFovScale() { return 1.0f; }

bool IsScopedOut() { return false; }

bool ShouldHideBlackArms() { return false; }

const char* GetSpecialWeaponName(uint8_t) { return ""; }

void FilterGameplayPad(int, input::Pad&) {}

void RenderAndStepFrame(X86Context&, uint8_t*) {}

void OnActorStep(X86Context&, uint8_t*, uint32_t, float) {}

void RenderAudioMix(float*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}

HudState GetHudState() { return HudState{}; }

}  // namespace game::flamethrower
