# TIME STOP / DEAD EYE — Implementation Report

**Project:** BLACK Enhanced Edition (`E:\ai\black`, branch `black-enhanced-edition`)  
**Scope:** Summary of implemented C++ hooks, pure state machine logic, selective physics-solver isolation, physical bullet queue/replay, Mod Menu & touch HUD integration, and automated host tests.

---

## 1. Files Implemented & Updated

| Component | File(s) | Purpose |
|---|---|---|
| **Pure State Machine** | [`src/common/time_stop_state.h`](file:///e:/ai/black/src/common/time_stop_state.h) | Platform-independent `TimeStopStateMachine` supporting `Toggle` and `Hold` activation modes, optional `max_duration_sec` and `cooldown_sec` caps, gameplay-only gating (`world_active`), and `SanitizeFreezeWorldDelta` delta clamp. |
| **Schema & Persistence** | [`src/common/mod_schema.h`](file:///e:/ai/black/src/common/mod_schema.h), [`src/common/mod_schema.cpp`](file:///e:/ai/black/src/common/mod_schema.cpp) | Stores Time Stop configuration (`time_stop_enabled`, `time_stop_mode`, `time_stop_max_duration`, `time_stop_cooldown`) inside Enhanced settings TOML persistence. |
| **Translations & Mod Menu** | [`src/common/translations.cpp`](file:///e:/ai/black/src/common/translations.cpp), [`src/common/menu.cpp`](file:///e:/ai/black/src/common/menu.cpp) | Adds Time Stop section under `PHYSICS LAB` with Arabic/English labels (`TIME STOP` / `إيقاف الزمن`, `FREEZE WORLD` / `تجميد العالم`, `RESUME TIME` / `استئناف الزمن`, `MAX DURATION` / `مدة التجميد`, plus mode/cooldown/status telemetry). |
| **Subsystem Freeze Hooks** | [`src/game/time_stop.h`](file:///e:/ai/black/src/game/time_stop.h), [`src/game/time_stop.cpp`](file:///e:/ai/black/src/game/time_stop.cpp) | Implements `PreFrameSync`, `PostFrameSnapshot`, selective `sub_000E0AA0` solver isolation, `sub_001CCBB0`/`sub_001E7FF0`/`sub_001780B0`/`sub_001F6280`/`sub_001D0500` gating, Pool A/B projectile suspension (`sub_001E4A30`, `sub_001E43F0`), weapon bullet stepper gating & `[kWorld+0x20]` TTL compensation (`sub_001E26E0`, `sub_001E1630`), `[0x2D12D0]+0x6E0` 16-slot overflow queue & replay (`sub_001DB7B0`), and world-space suspended projectile markers (`DrawOverlay`). |
| **Cooperating Hooks** | [`src/game/esp.cpp`](file:///e:/ai/black/src/game/esp.cpp), [`src/game/hooks.cpp`](file:///e:/ai/black/src/game/hooks.cpp) | Cooperates on enemy update `sub_001DFD90` (`kind != 2`), destructible glass/debris `sub_001690D0`, frame overlay rendering, and clean cleanup/resume transitions. |
| **Touch HUD & Hotkeys** | [`native/touch.cpp`](file:///e:/ai/black/native/touch.cpp) | Adds first-class circular HUD control `time_stop` (`kTimeStop = -3`, `Icon::TimeStop`, default `{0.635f, 0.07f, 0.04f}` in `kTwoFinger`, `kThreeFinger`, `kFourFinger` presets—same `y = 0.07f` and `r = 0.04f` as `gyro` and `panel` Mod Menu buttons), full Touch Editor drag/resize/hide support, active cyan countdown badge, and keyboard hotkey binding (`kTimeStopAction`). |
| **Automated Host Tests** | [`tests/test_settings_core.cpp`](file:///e:/ai/black/tests/test_settings_core.cpp), [`tests/test_settings_suite.py`](file:///e:/ai/black/tests/test_settings_suite.py) | Verifies `TimeStopStateMachine` transitions, hold vs toggle modes, cooldown/max-duration caps, TOML persistence, and Arabic UI translations. |

---

## 2. Player Independence Fix (Selective `sub_000E0AA0` Isolation)

During initial on-device validation (`versionCode 48`), skipping `sub_000E0AA0` (`[0x2D45F4]`) unconditionally froze non-player rigid bodies but also prevented player locomotion because the player's capsule/movement pipeline routes through `sub_000E0AA0` -> `sub_0007DAB0` -> `sub_000E09A0`.

To preserve full player movement, aiming, weapon firing, and static/frozen-object collision during Time Stop:
1. **Participant Identification (`CollectSolverParticipants`):** Scans active slots in `[solver + 0x50]` (`0x6E` slots) and `[solver + 0x2320]` (`0x14` slots). Any slot owned by the player (`owner == kWorld + 0x30` or `[owner + 0xC4] == 2`) registers its physics body into `out_player_bodies`.
2. **Pre-Solve Snapshot & Freeze (`PrepareSelectivePhysicsSolve`):** Walks `[phys_world + 0x13C]`, `[phys_world + 0x1EC]`, and `[phys_world + 0x29C]` active body linked lists (guarded by `mem::IsCommitted` and step bounds). Every non-player body has its pre-freeze `vel_lin` (`+0x20..+0x2B`), `vel_ang` (`+0x30..+0x3B`), 64-byte `transform` (`+0x40..+0x7F`), `vel_prev` (`+0x80..+0x8B`), `force_acc` (`+0x90..+0x9B`), `rest_energy` (`+0x9C`), `torque_acc` (`+0xA0..+0xAB`), and `rest_ticks` (`+0xAC`) captured once, while keeping linked-list pointers (`+0x2C` next, `+0x3C` prev, `+0x8C` flags) untouched. Non-player velocity/force/torque vectors are zeroed and `transform` matrices are locked before running `__imp__sub_000E0AA0`.
3. **Post-Solve Lock (`LockAfterSelectivePhysicsSolve`):** Re-applies locked non-player `transform` and zeroed kinematics plus non-player `owner + 0x70..+0xAF` transforms immediately after `__imp__sub_000E0AA0` completes, eliminating single-frame gravity drift or player-bump displacement on frozen props/ragdolls.
4. **Momentum Restoration (`RestoreFrozenPhysicsOnResume`):** When Time Stop exits, verifies each snapshotted body is still present in live `phys_world` active lists and restores its exact saved linear/angular velocities, accumulators, and rest counters so suspended objects continue seamlessly from their exact pre-freeze trajectory.

---

## 3. Touch HUD Control Placement Fix (`native/touch.cpp`)

- Removed the oversized top-center pill previously drawn in `game::time_stop::DrawOverlay` (`src/game/time_stop.cpp`), leaving only projected world-space suspended bullet/grenade/rocket markers while frozen.
- Registered `{"time_stop", "Time Stop", Icon::TimeStop, kTimeStop, When::Play}` inside `kButtons[]` right before `"zoom"` (`kZoomButton` and `kStick` indices remain preserved at the tail of `Layout`).
- Positioned `time_stop` at `{0.635f, 0.07f, 0.04f}` across `kTwoFinger`, `kThreeFinger`, and `kFourFinger`, matching the exact height (`y = 0.07f`), button radius (`r = 0.04f`), and `0.054f` horizontal pitch of the existing top row (`gyro` at `0.419f`, `map` at `0.473f`, `pause` at `0.527f`, `panel`/Mod Menu at `0.581f`, `time_stop` at `0.635f`).
- Renders a crisp stopwatch vector icon (`Icon::TimeStop`), highlights cyan with remaining/elapsed seconds (`cy + r * 1.08f`) while frozen (or cooldown countdown when cooling down), and supports `Hold` and `Toggle` input directly in `FingerDown`/`FingerUp`.

---

## 4. Host Verification Results

Re-verified after the final C++ edits (`time_stop.h`, `time_stop.cpp`, `touch.cpp`):
- **`ctest --test-dir tests/build --output-on-failure`:** `1/1 Test #1: SettingsCoreTests ... Passed`
- **`python tests/test_settings_suite.py`:** `11 passed, 0 failed out of 11 tests.`
- **`.\gradlew.bat assembleDebug` (`versionCode 49`):** `BUILD SUCCESSFUL in 26s`
