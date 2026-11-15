# BLACK Enhanced Edition — Delirium Physics Design Spec

**Date:** 2026-11-15  
**Branch:** `black-enhanced-edition`  
**Status:** Approved (`الخيار 1 — Delirium State Machine + Procedural Skeletal & Agony Active Ragdoll`)  
**Inspiration:** *Delirium Physics* (Ready or Not, v1.5–v1.6 Nov 2026) & Euphoria-style active hit reactions, calibrated for BLACK's fast-paced cinematic gunplay.

---

## 1. Goals & Gameplay Pillars

Deliver 6 cohesive physical hit-reaction behaviors on enemy soldiers while preserving engine stability, 60+ FPS smoothness, Time Stop compatibility, and player feel:

1. **Directional Recoil / Flinch (`يتراجع عند الإصابة`):** Significant hits produce a snappy directional recoil impulse opposite to projectile travel plus procedural upper-body kickback, while light grazing scratches preserve soldier poise so gunfights feel punchy rather than twitchy.
2. **Wound-Zone Clutching (`يمسك مكان الإصابة`):** Enemy soldiers register where they were hit (`Head`, `TorsoChest`, `LeftArm`, `RightArm`, `LeftLeg`, `RightLeg`, `CenterMass`) via `sub_001E74B0` (`dl` bone/zone ID) and continuously bias procedural hit-reaction channels (`sub_0017F270`) and localized reaction clips (`sub_00195920` `0x1E`/`0x1F`) toward protecting/clutching the struck region while stumbling.
3. **Balance Loss & Multi-Step Stagger (`يفقد توازنه ويتعثر`):** Stacked hits or high-impulse impacts (including Delirium v1.5 multi-pellet shotgun aggregation within a ~45 ms window) erode stability and inject decaying lateral-oscillating stumble velocity into the enemy's live capsule (`sub_001DFD90`: `[char+0xA0]` & `[char+0x190]`), keeping living soldiers upright and stable without unstable ragdoll-to-T-pose ground recovery.
4. **Erratic Blind-Fire While Off-Balance (`يحاول إطلاق النار وهو فاقد توازنه`):** When balance drops below the stagger threshold during active combat, AI weapon firing (`sub_001DB910` -> `loc_001DBAC3` -> `sub_00168E20`) stays live with an erratic, dynamic aim scatter cone biased sideways/upward so staggering soldiers spray dramatic, believable panic shots without laser-aiming through flinches (strictly excludes the player).
5. **Varied Physics-Driven Collapse (`تسقط بحركات متنوعة`):** On lethal/critical transition (`sub_001E74B0` lethal branch, `sub_001FFE70` / `sub_00200045`, `sub_002080C0`), `keep_momentum` transfers the soldier's accumulated stagger momentum plus zone-specific rotational spin/pitch bias into the `[char+0x800]` ragdoll body (`[+0x90] == 2`) so headshots, shoulder hits, leg sweeps, and chest blasts each fall distinctly.
6. **Brief Agony Collapse & Writhing Phase (`حركة سقوط واحتضار قصيرة`):** Newly ragdolled soldiers enter a timed agony window (`0.70s – 1.45s` cinematic default, scaled by user config) applying damped, alternating spine/arm/head bone impulses via `sub_000DC960` before settling cleanly onto the ground.

---

## 2. Architecture & Engine Integration Points

### 2.1 Platform-Independent Simulation Core (`src/game/delirium_rules.h`, `src/game/delirium_rules.cpp`)
Zero Android/x86 dependencies so host C++ tests (`test_settings_core`) and Android (`libmain.so`) run the exact same state machine and math:
- Fixed-size active actor tracker (zero hot-path heap allocation, LRU/stale eviction by monotonic timestamp).
- **Per-enemy `SoldierState`:**
  - Impact accumulator (`pending_pellet_force`, `pellet_window_remaining`) for shotgun & rapid-burst aggregation + light-scratch floor.
  - Balance scalar (`1.0f` poised -> `0.0f` staggered), lateral stagger wave phase/frequency, directional stumble velocity `(vx, vz)`, recoil timer.
  - Wound clutching state (`HitZone`, `clutch_timer`, `clutch_retrigger_cooldown`).
  - Erratic blind-fire window (`blind_fire_timer`, `scatter_phase`, `nervous_burst_shots`).
  - Collapse momentum transfer (`stagger_vel`, `last_hit_dir`, `last_hit_zone`, `last_hit_force`, `collapse_profile`).
  - Agony writhing timer (`agony_remaining`, `agony_total`, `agony_phase`, `ragdoll_body_ea`).

### 2.2 Guest Engine Hook Layer (`src/game/delirium_physics.h`, `src/game/delirium_physics.cpp` + existing hooks)
- **Hit Intake (`sub_001E74B0` in `src/game/cheats.cpp`):** Captures `victim` (`ecx`), `hit_pos`, `hit_dir`, `force_arg`, `zone_id` (`dl`), `attacker`. Distinguishes alive vs lethal hits via `[victim + 0x384]` (`kCharDying`) and `[victim + 0x2F0]` (`kCharHealth`). Ignores hits when `attacker == victim` invalid or Time Stop freeze rules require deferral.
- **Enemy Frame Tick & Ragdoll Agony Step (`sub_001DFD90` in `src/game/esp.cpp`):**
  - When `!game::time_stop::IsFrozen()` and `who` is an enemy (`[who+0xC4] != 2`):
    - **Alive (`[who+0x384] == 0`):** Advances stumble simulation, applies bounded displacement/velocity nudge onto `[who+0xA0]` / `[who+0x190]`, and triggers procedural skeletal/clutch pulses via `sub_0017F270` (`[who+0x328]`) + `sub_00195920` when due.
    - **Dead/Dying (`[who+0x384] != 0` and `[who+0x324]` ragdoll `[body+0x90] == 2`):** Drives the timed agony spasm impulses across mapped ragdoll bones via `sub_000DC960` (reentrancy-guarded) until `agony_remaining <= 0`.
- **Erratic Blind-Fire Scatter (`sub_001DB910` in `src/game/delirium_physics.cpp`):** Inspects `[esi+0x04] -> [+0xF0]` shooter pointer; when shooter is an enemy in staggered/blind-fire state, clones and perturbs the passed target `float[3]` pointer with dynamic aim-scatter before invoking `__imp__sub_001DB910`.
- **Varied Collapse Momentum (`sub_00200045` & `sub_002080C0` in `src/game/physics_lab.cpp`):** Queries `delirium::ConsumeCollapseImpulse(self/state)` at ragdoll launch initialization to preserve stumble momentum (`keep_momentum`) and apply directional spin/lift variation (`PitchYawBias` by `HitZone`) before existing `PhysicsRagdoll` multipliers.

---

## 3. Configuration & Physics Lab UI

Extend `mod_schema.h` / `mod_schema.cpp`, `physics_lab.h` / `physics_lab.cpp`, and `menu.cpp` under `[physics_lab]`:
- `PhysicsDeliriumEnabled` (`bool`, default `true` in `EnhancedOptions`; `false` when `"classic"` preset is explicitly applied, preserving all 12 classic 2006 floats and `TestPhysicsLabSchemaAndPresets`).
- `PhysicsDeliriumIntensity` (`float`, `0.25 .. 3.00`, default `1.35` — tuned for BLACK's cinematic fast-paced feel).
- `PhysicsDeliriumBlindFire` (`bool`, default `true`).
- `PhysicsDeliriumAgonyDuration` (`float`, `0.00 .. 2.50`, default `1.10` seconds).
- Add a clean `"DELIRIUM PHYSICS"` section inside `DrawCategoryPhysicsLab` in `src/common/menu.cpp` with toggle, intensity slider, blind-fire toggle, and agony duration slider.

---

## 4. Verification Criteria
1. Host unit tests (`tests/test_settings_core.cpp` + `delirium_rules.cpp`) pass 100%, covering:
   - Hit-zone classification & shotgun multi-pellet impulse aggregation.
   - Light-scratch stability floor vs heavy recoil & stumble velocity decay.
   - Wound clutching pulse cadence & zone tracking.
   - Erratic blind-fire scatter activation strictly for staggered enemies (never non-staggered/player).
   - Varied collapse vectors (`Head`, `Chest`, `Left/Right Arm`, `Legs`) + `keep_momentum` preservation.
   - Timed agony spasm envelope rising smoothly and decaying strictly to zero after `agony_duration`.
   - Settings schema TOML round-trip, clamping, and `"classic"` preset preservation.
2. Python settings suite (`tests/test_settings_suite.py`) passes 100%.
3. Android Release APK (`gradlew assembleRelease`) compiles and packages cleanly with zero build errors.
