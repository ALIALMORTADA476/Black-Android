# BLACK Enhanced Edition — Complete Audio, Combat, AI, Visuals & UX Overhaul Plan

> **For agentic workers:** Implement task-by-task using TDD (`tests/` C++ & Python verification), zero duplicate logic, and final APK + ADB verification.

**Goal:** First upgrade every weapon and tactical sound in `black_weapon_audio.pak` using ultra-realistic per-weapon recordings (`WSTFG` Dark Signal + `Sonniss GDC 2015-2026` + `FFX 192kHz`), then implement and unlock the full suite of combat feel, VFX, AI, localization, shader/graphics, and mobile UX enhancements.

**Architecture:**
- Phase 1: Python audio download + OGG-to-48kHz-WAV conversion (`ffmpeg`) + isolated per-weapon mix redesign + multi-surface shell casings in `tools/build_weapon_audio_pack.py`.
- Phase 2: Pure C++ host-testable rule/simulation headers (`src/game/combat_feedback_rules.h`, `src/game/blood_decal_rules.h`, `src/game/overdrive_rules.h`, `src/common/gyro_filter.h`) tested via `tests/test_settings_core.cpp` + `.tools/w64devkit/bin/g++.exe` before wiring runtime hooks (`sound/`, `d3d/`, `game/`, `native/`, `common/`).

**Tech Stack:** C++20, Vulkan HLSL->SPIR-V (`native/vulkan.cpp`), Xbox XDK static recompilation hooks, Python 3.12, NumPy, `ffmpeg`, SDL3, ImGui, Gradle/NDK 28.

---

## Global Constraints
- Zero digitally clipped samples (`total_clipped == 0`) in `black_weapon_audio.pak`.
- Maintain strict backwards compatibility with `config/game.toml` and `android.toml` serialization.
- Host unit tests (`tests/test_settings_suite.py` + C++ compiled unit tests) must pass after every code milestone.
- Final build via `gradlew.bat assembleRelease` and verified on connected device `damfmjij5pbe7dbu`.

---

## Tasks

### Task 1: Upgrade Per-Weapon Audio Pack (Download WSTFG + Missing GDC 2024/2026 & Remaster)
- **Files:**
  - Create: `tools/fetch_wstfg_audio.py`
  - Modify: `tools/fetch_sonniss_gdc.py` (add missing GDC 2016–2024 firearm & explosion IDs)
  - Modify: `tools/build_weapon_audio_pack.py` (isolate per-weapon primary sources, remove `Snake 20ga/9mm` bleed, integrate `on_gun` mics & WSTFG stereo shots/suppressed/RPG/casings)
  - Output: `app/src/main/assets/weapon_audio/black_weapon_audio.pak`
- **Verification:** Run `tools/build_weapon_audio_pack.py`, verify `total_clipped == 0`, distinct primary sources across all 17 base + 10 suppressed slots, and healthy dynamic crest factors.

### Task 2: Audio DSP Upgrades — Tinnitus ("Duck & Ring"), Acoustic Occlusion & Multi-Surface Casings
- **Files:**
  - Modify: `src/common/mod_schema.h`, `src/common/mod_schema.cpp` (add `AudioTinnitusEnabled` / `AudioOcclusionEnabled`)
  - Modify: `src/sound/weapon_audio.h`, `src/sound/weapon_audio.cpp`, `src/sound/mixer.cpp`
  - Test: `tests/test_settings_core.cpp`
- **Deliverable:** `TriggerExplosionConcussion(dist_m)` drives low-pass occlusion + decaying stereo tinnitus sine wave + `TriggerShellClink` surface differentiation (`concrete`, `metal`, `dirt/grass`) + wall/occlusion dampening for behind-wall emitters.

### Task 3: Combat Feedback — Directional Damage Arc, Camera Kinetic Punch & Tactical Hitstop
- **Files:**
  - Modify: `src/game/combat_feedback_rules.h`, `src/game/combat_feedback_rules.cpp`
  - Modify: `src/game/combat_feedback.h`, `src/game/combat_feedback.cpp`
  - Modify: `src/game/cheats.cpp`, `src/game/hooks.cpp`, `src/game/esp.cpp`
  - Modify: `src/common/mod_schema.h`, `src/common/mod_schema.cpp`
  - Test: `tests/test_settings_core.cpp`
- **Deliverable:** Directional damage indicator arc + yaw/roll micro-punch when player takes damage; 1–3 frame target/FX micro-hitstop on lethal headshots, close shotgun blasts, and RPG direct hits without freezing camera look.

### Task 4: Ballistics, Gore & VFX — Scorch Decals, Muzzle Heat Shimmer, Smoke Dispersion & Exit-Wound Wall Splatter
- **Files:**
  - Modify: `src/game/ballistics_fx.cpp`, `src/game/ballistics_fx.h`, `src/game/blood_decal_rules.h`, `src/game/blood_decal_rules.cpp`
  - Modify: `src/common/mod_schema.cpp` (unlock `CombatMaxBloodDecals`, `CombatCorpsePersistenceMode`)
  - Test: `tests/test_settings_core.cpp`
- **Deliverable:** Persistent 3D explosion scorch marks, `DrawScreenWarp` barrel heat shimmer after sustained fire, blast shockwaves radially pushing active smoke plumes outward, exit-wound arterial wall splatter behind hit targets, unlocked decal budget (256..1024) & corpse mode control.

### Task 5: AI Behavior, Slow-Mo Bullet-Cam, Active Reload & Weapon Inspect
- **Files:**
  - Modify: `src/game/delirium_rules.h`, `src/game/delirium_rules.cpp`, `src/game/delirium_physics.cpp`
  - Modify: `src/game/overdrive_rules.h`, `src/game/overdrive_rules.cpp`, `src/game/overdrive.cpp`
  - Modify: `src/game/hooks.cpp`, `src/game/hud.cpp`
  - Modify: `src/common/mod_schema.h`, `src/common/mod_schema.cpp`
- **Deliverable:** AI suppression flinch/duck/spread increase under player near-miss tracers + grenade evade scatter; optional Last-Kill / Sniper Headshot 1.2s slow-mo dip; Active Reload golden sweet-spot bar + Weapon Inspect trigger with mechanical clicks.

### Task 6: Graphics Shader Completion, Arabic Flash Unlock & Mobile UX (`ADS-Only Gyro` & HUD Presets)
- **Files:**
  - Modify: `native/vulkan.cpp` (implement & bind `GraphicsAmbientOcclusion` Half-Res screen-space crevice occlusion & `GraphicsSoftShadows` / directional shadow smoothing + concussion blur in `kFilterHlsl`)
  - Modify: `src/common/mod_schema.cpp` (unlock `GraphicsAmbientOcclusion`, `GraphicsSoftShadows`, `GraphicsHdTexturesEnabled`, `ArabicFlashMenus`, plus new controls)
  - Modify: `native/gyro.cpp`, `src/common/gyro_filter.h` (add `ADS` mode triggered when aiming/zooming)
  - Modify: `src/game/hud.cpp`, `src/common/menu.cpp` (2-Finger / 4-Finger Claw HUD presets + eye-care Mod Menu controls & Arabic strings)
  - Test: `tests/test_settings_core.cpp`, `tests/test_settings_suite.py`

### Task 7: Final Regression Tests, ARM64 APK Build & Android Device Verification
- **Verification:**
  - Run `python tests/test_settings_suite.py` (all tests pass).
  - Run `.\gradlew.bat assembleRelease` and confirm zero errors.
  - Install on `damfmjij5pbe7dbu`, launch app, capture `logcat` & screenshot/sanity checks for HUD, audio pack load (`black_weapon_audio.pak`), Mod Menu, and gameplay stability.
