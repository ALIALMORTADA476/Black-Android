# Delirium Physics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a Delirium Physics (Ready or Not v1.5–1.6) inspired active enemy hit-reaction, stumbling, wound-clutching, erratic blind-fire, varied collapse, and timed agony-writhing system for BLACK Enhanced Edition.

**Architecture:** Split into a platform-independent C++20 simulation core (`src/game/delirium_rules.h/.cpp`) tested natively via `tests/test_settings_core.cpp`, wired into guest x86 engine hooks (`src/game/delirium_physics.h/.cpp`, `cheats.cpp`, `esp.cpp`, `physics_lab.cpp`) and configurable via `[physics_lab]` (`mod_schema.h/.cpp`, `menu.cpp`).

**Tech Stack:** C++20, CMake + Ninja host test harness (`test_settings_core`), XboxXBE-Recomp guest hooks (`X86_FUNC`/`__imp__sub_*`), ImGui Mod Menu, Gradle/NDK Android Release APK.

**Spec:** [2026-11-15-delirium-physics-design.md](file:///e:/ai/black/docs/superpowers/specs/2026-11-15-delirium-physics-design.md)

## Global Constraints

- Do not break Classic 2006 preset invariants asserted in `TestPhysicsLabSchemaAndPresets` (`1.00f` across the 12 core multipliers).
- Strictly respect `game::time_stop::IsFrozen()` so Time Stop never gets un-frozen by Delirium impulses.
- Keep hot-path per-frame/per-hit state allocation-free (`std::array` fixed slots).
- Strictly never inject AI blind-fire aim scatter onto the player entity.

---

### Task 1: Pure Delirium Physics Rules & State Machine (TDD)

**Files:**
- Create: `src/game/delirium_rules.h`
- Create: `src/game/delirium_rules.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/test_settings_core.cpp`

- [ ] **Step 1: Write failing unit tests in `tests/test_settings_core.cpp`** covering hit-zone mapping, shotgun pellet aggregation, light-scratch poise threshold, stumble velocity & balance recovery, wound clutching pulses, blind-fire scatter, varied collapse torque, and agony envelope decay to zero.
- [ ] **Step 2: Run `cmake --build tests/build` / `test_settings_core.exe` to confirm RED failure** (missing `delirium_rules.h`).
- [ ] **Step 3: Implement `src/game/delirium_rules.h` & `src/game/delirium_rules.cpp`** and add `delirium_rules.cpp` to `tests/CMakeLists.txt`.
- [ ] **Step 4: Run `cmake --build tests/build` & `test_settings_core.exe`** to verify GREEN pass.

---

### Task 2: Schema, Presets, & Physics Lab Runtime Sync (TDD)

**Files:**
- Modify: `src/common/mod_schema.h`
- Modify: `src/common/mod_schema.cpp`
- Modify: `src/game/physics_lab.h`
- Modify: `src/game/physics_lab.cpp`
- Modify: `tests/test_settings_core.cpp`

- [ ] **Step 1: Add Delirium Physics schema assertions in `tests/test_settings_core.cpp`** (new settings `PhysicsDeliriumEnabled`, `PhysicsDeliriumIntensity`, `PhysicsDeliriumBlindFire`, `PhysicsDeliriumAgonyDuration`, TOML roundtrip, bounds clamping, and `"classic"` / `"cinematic"` preset values).
- [ ] **Step 2: Run host tests to verify RED failure.**
- [ ] **Step 3: Add settings to `mod_schema.h/.cpp` and sync fields in `physics_lab.h/.cpp`.**
- [ ] **Step 4: Run `test_settings_core.exe` and `tests/test_settings_suite.py` to confirm GREEN pass.**

---

### Task 3: Guest Engine Hooks Integration (`delirium_physics.cpp` + `cheats.cpp` + `esp.cpp` + `physics_lab.cpp`)

**Files:**
- Create: `src/game/delirium_physics.h`
- Create: `src/game/delirium_physics.cpp`
- Modify: `src/game/cheats.cpp`
- Modify: `src/game/esp.cpp`
- Modify: `src/game/physics_lab.cpp`

- [ ] **Step 1: Implement `delirium_physics.h/.cpp`** integrating:
  - `OnBulletHit(victim, pos, dir, force, zone_id, attacker)` calling into `delirium::Simulation`.
  - `OnEnemyFrameStep(enemy_ea, dt)` driving stumble capsule displacement (`[enemy+0xA0]`/`[enemy+0x190]`), procedural clutching overlay (`sub_0017F270` + `sub_00195920`), and post-death ragdoll agony bone impulses (`sub_000DC960` on `[enemy+0x800]` with reentrancy guard).
  - `X86_FUNC(sub_001DB910)` hooking AI shooting target scatter while off-balance (`[shooter+0xC4] != 2`).
- [ ] **Step 2: Wire `sub_001E74B0` (`cheats.cpp`), `sub_001DFD90` (`esp.cpp`), and `sub_00200045`/`sub_002080C0` (`physics_lab.cpp`)** for collapse `keep_momentum` + zone spin variation.

---

### Task 4: Physics Lab Mod Menu UI (`src/common/menu.cpp`)

**Files:**
- Modify: `src/common/menu.cpp`

- [ ] **Step 1: Add `"DELIRIUM PHYSICS"` section in `DrawCategoryPhysicsLab`** (`src/common/menu.cpp`) exposing toggle, intensity slider (`0.25x..3.00x`), blind-fire toggle, and agony duration slider (`0.00s..2.50s`).

---

### Task 5: Full Verification & Release APK Build

- [ ] **Step 1: Run `tests/build/test_settings_core.exe` and `python tests/test_settings_suite.py`** after final edit.
- [ ] **Step 2: Build Android Release APK (`.\gradlew.bat assembleRelease`)** and confirm exit code 0 and APK output.
