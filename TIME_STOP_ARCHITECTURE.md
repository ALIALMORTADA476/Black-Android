# TIME STOP / DEAD EYE — World Simulation Architecture

**Project:** BLACK Enhanced Edition (`E:\ai\black`, branch `black-enhanced-edition`)  
**Scope:** Selective world-freeze architecture replacing global pause/timescale hacks with deterministic subsystem-level execution gating.

---

## 1. Execution Model Overview

BLACK does **not** drive all gameplay systems from a single lockstep callback. Instead, `sub_001F1080` (`generated/game/x86_recomp.52.cpp`) orchestrates independent managers per frame when `[kWorld + 0x5AE0] == 0x1C` (active in-level gameplay state). Setting global `game_speed = 0` or calling an Android/SDL app pause would freeze camera look, player movement, input polling, rendering, or trigger OS ANR/black-screen issues.

Instead, the Time Stop controller maintains:
- **Player / Render / Audio-Input Path:** Always active (`1.0x` or user-configured rate).
- **Frozen World Path:** Selectively gated via `X86_FUNC` strong-symbol hooks when `TimeStopState::IsFrozen() == true` AND gameplay state is active (`worldState == 0x1C`).
- **Safe Resume Path:** Tracks monotonic delta time across frozen frames to shift active projectile spawn timestamps forward and drains overflowed player projectiles once simulation slots open.

---

## 2. Subsystem Execution Matrix

Verified against `generated/game/x86_recomp.*.cpp`, `src/game/hooks.cpp`, `src/game/esp.cpp`, `src/game/cheats.cpp`, and `src/game/physics_lab.cpp`:

| Subsystem | Function / Entry Point | Stack Cleanup (`ret`) | Behavior During Time Stop | Rationale |
|---|---|---|---|---|
| **Master Frame Coordinator** | `sub_001F1080` | Calls subsystems | **ACTIVE** | Keeps frame pipeline, Vulkan rendering, camera matrices, and UI responsive. |
| **Player Input, Movement & Camera** | `sub_001E5FA0` (`[kWorld+0x30]->vtable[0]`), `sub_001D2F20`, static collision `sub_000CC1F0` | Preserved | **ACTIVE** | Player retains full movement, camera aim, weapon firing, ammo consumption, and static wall/floor collision. |
| **Player Weapon Firing** | `sub_001EEFD0` (`hooks.cpp:361`) | `ret 0x20` | **ACTIVE** | Player can trigger shots, reload, and throwables while world is frozen. |
| **Weapon / HUD / Viewmodel Sync** | `sub_001BE710` (`[0x2D12D4]`) | Preserved | **ACTIVE** | Keeps viewmodel animations, ammo HUD counters, and weapon switching responsive for player. |
| **Enemy AI & Character Step** | `sub_001DFD90` (`esp.cpp:205`) | `ret 0x04` (`esp += 8`) | **FROZEN** (`kind != 2`) | Checks `R32(ecx + 0xC4) != 2` (`kKindPlayer = 2`). Still records ESP target metadata (`g_seen`), then returns early without stepping enemy AI, movement, skeletal animation, or firing. |
| **Squad / Combat Coordinator** | `sub_001CCBB0` (`[0x2D1980]`) | `ret 0x04` (`esp += 8`) | **FROZEN** | Halts squad tactic updates and coordinated enemy reactions. |
| **AI Director** | `sub_001E7FF0` (`[0x2D45D0]`) | `ret 0x08` (`esp += 12`) | **FROZEN** | Stops global threat evaluation, reinforcements, and AI perception ticks. |
| **Rigid-Body & Ragdoll Solver** | `sub_000E0AA0` (`[0x2D45F4]`) | `ret 0x04` (executed selectively) | **SELECTIVE SOLVE** (`player` active; non-player bodies locked) | Identifies `player_bodies` from solver slots (`owner == player` or `[owner+0xC4] == 2`). Lets `__imp__sub_000E0AA0` run so player movement, gravity, and collisions against static geometry and frozen objects work normally, while snapshotting, zeroing, locking poses (`+0x40..+0x7F` & `owner+0x70..+0xAF`), and restoring pre-freeze velocities/accumulators (`+0x20[3]`, `+0x30[3]`, `+0x80[3]`, `+0x90[3]`, `+0x9C`, `+0xA0[3]`, `+0xAC`) on resume for all non-player bodies across `[phys_world+0x13C]`, `[phys_world+0x1EC]`, and `[phys_world+0x29C]`. |
| **Glass & Destructible Debris** | `sub_001690D0` (`hooks.cpp:564`) | `ret 0x04` (`esp += 8`) | **FROZEN** | Prevents destructible glass/shards from advancing or clearing during freeze. |
| **Particle, Smoke & Sparks Sim** | `sub_001780B0` (`[0x2D1990]`) | `ret` (`esp += 4`) | **FROZEN** | Skips simulation step (`0x4E2C0`, `0x506D0`, `0x53EA0`, etc.) while keeping billboard/trail renderers (`sub_0015B380`, `sub_0016C9B0`, `sub_0016A790`, `sub_0016A3D0`) live so smoke, fire, and shards stay suspended visibly in mid-air. |
| **World Scripts & Mission Timers** | `sub_001F6280` (`[0x2D19B0]`), `sub_001D0500` (`[0x2D0D7C]`) | `ret` / `ret 0x04` | **FROZEN** (only in `0x1C`) | Suspends scripted world events and 10x world event slots only during active gameplay (`[kWorld+0x5AE0] == 0x1C`). |
| **Pool A Throwables (Grenades)** | `sub_001E4A30` (`vtable 0x28BDD8`) | `ret 0x04` (`esp += 8`) | **FROZEN** (`dt > 0.0f`) | Allows `dt == 0.0f` spawn initialization inside `sub_001DCE60`; freezes movement, fuse countdown (`+0x110`), and explosion trigger whenever `dt > 0.0f`. |
| **Pool B Rockets / M203** | `sub_001E43F0` (`vtable 0x28C740`) | `ret 0x04` (`esp += 8`) | **FROZEN** (`dt > 0.0f`) | Allows `dt == 0.0f` spawn initialization inside `sub_001E35F0`; freezes flight integration and TTL countdown whenever `dt > 0.0f`. |
| **Weapon Manager Bullet Stepper** | `sub_001E26E0` (`[0x2D12D0]+0x6E0`), `sub_001E1630` (`+0x20`) | `ret 0x04` (`esp += 8`) | **FROZEN** + **TTL Shift** | Halts integration while advancing `+0x84` (`spawn_time`) by frozen `world_dt` so active bullets do not expire immediately on resume. |
| **Weapon Manager Bullet Spawner** | `sub_001DB7B0` | `ret 0x14` (`esp += 24`) | **ACTIVE + OVERFLOW QUEUE** | Lets newly fired player bullets populate free `[0x2D12D0]+0x6E0` slots (`< 16`) immediately (where they hang motionless), and queues excess shots in host memory if all 16 slots fill during freeze. |
| **World Entity Loop 2 (Render/Sync)** | `sub_001F1080` (`vtable+0x10` = `0x001B7710`) | — | **ACTIVE** | Shared render/transform publish step keeps frozen Pool A/B grenades and rockets visible in the scene. |

---

## 3. Safe Time & State Management

1. **Menu / Cutscene / Pause Safety:**  
   Time Stop only gates subsystems when `kWorld` is valid and `R32(kWorld + 0x5AE0) == 0x1C`. Outside active gameplay (loading screens, pause menus, title menu), the controller immediately releases freeze state and clears any pending overflow queue so menus never lock.
2. **Delta-Time Spike Prevention:**  
   `sub_001E26E0` checks bullet lifetime against `[kWorld + 0x20] > [slot + 0x84] + [0x295C70]`. While frozen, `time_stop` measures delta `[kWorld + 0x20]` each frame and adds that delta to `[slot + 0x84]` of every active bullet slot (`[slot + 0x88] != 0`). On resume, remaining lifetime matches the exact remaining flight duration prior to freeze.
3. **Physics Lab Coexistence (`PhysicsTimeScale`):**  
   Global slow-motion (`PhysicsTimeScale`) scales `dt` smoothly, whereas Time Stop is a selective world-clock freeze. Emergency deactivation via Mod Menu, keyboard hotkey (`T` / configurable), or touchscreen HUD toggle immediately restores all subsystems cleanly.
4. **Activation Modes:**  
   - **Toggle Mode:** Press to freeze, press again (or hit duration cap) to resume.  
   - **Hold Mode:** Active only while key/touch button is held; releasing immediately resumes world simulation.  
   - **Max Duration & Cooldown:** Configurable optional duration cap (`0s` = unlimited, up to `30s`) and optional cooldown (`0s` = none, up to `30s`), enforced deterministically in `common::TimeStopStateMachine`.

---

## 4. Hooks & Ownership Layout

- `src/common/time_stop_state.h` — Pure, platform-independent `TimeStopStateMachine` and `TimeStopConfig` (covered by C++ unit tests on desktop and shared with Android runtime).
- `src/game/time_stop.h` / `src/game/time_stop.cpp` — Guest `X86_FUNC` hooks, selective `sub_000E0AA0` non-player body freeze/resume isolation, `[kWorld + 0x20]` timestamp compensation, `[0x2D12D0] + 0x6E0` overflow bullet queue & replay, and projected suspended-projectile markers (`DrawOverlay`).
- `src/game/esp.cpp` — Cooperates on `sub_001DFD90` so ESP keeps tracking frozen enemies and enemy execution returns cleanly when Time Stop is active.
- `src/game/hooks.cpp` — Cooperates on `sub_001690D0` (glass/debris) and triggers Time Stop tick / overlay updates alongside frame/menu lifecycle.
- `src/common/mod_schema.*`, `src/common/menu.cpp`, `src/common/translations.cpp`, `native/touch.cpp` — Persistent TOML configuration, Arabic/English UI under `PHYSICS LAB`, keyboard hotkeys, and native circular HUD button (`"time_stop"`, `kTimeStop = -3`, `r = 0.04f`, `y = 0.07f`, `x = 0.635f` alongside Gyro & Panel/Mod Menu).
