# Physics Lab Prototype & Expanded Implementation Report (Build 40)

## 1. Overview
Stage 6 `PHYSICS LAB` has been expanded to **12 real-time guest physics parameters** and **5 presets** (`Classic`, `Realistic`, `Cinematic`, `Mayhem / فوضى عارمة`, `Custom`), wired directly into the recompiled Xbox guest physics functions (`generated/game/x86_recomp.11.cpp`, `x86_recomp.23.cpp`, `x86_recomp.51.cpp`, `x86_recomp.54.cpp`) and unleashed in **Build 40 (`1.5.2-physics-lab-unleashed`)**.

## 2. Verified Guest Physics Hooks & SIMD Register Contracts
1. **Committed Guest Memory Validation (`IsValidGuestAddr` via `mem::IsCommitted`)**:
   - Validates guest addresses across all committed Xbox regions (`kUserLow..kUserHigh`, `kContiguousBase = 0x80000000`, `kSystemBase = 0xB0000000`, and `kStackBase = 0xD0000000`) in O(1) time via `mem::IsCommitted(addr) && mem::IsCommitted(addr + span - 1u)`.
2. **Stack-Pushed 3D Launch Vectors & Upward Arc Lift (`ScalePushedVec3`)**:
   - Scales 3D linear and angular velocity/impulse vectors on the Xbox stack (`0xD0000000..0xDFFFFFFF`) right before `sub_000DC070` / `sub_000DC100` calls (`PhysicsBlastLinearA`, `PhysicsBlastAngularA`, `PhysicsImpactLinearA..D`, `PhysicsImpactAngularA..D`, `PhysicsDebrisLaunch`, `PhysicsCharacterLaunchA..D`).
   - Compensates for slow-motion `dt` scaling (`TimeCompensatedScale`) and adds upward vertical lift (`vy += lift`) proportional to horizontal launch speed so bodies and props launch into an aerial arc instead of dragging against floor friction.
3. **Solver Top-Speed Cap & Anti-Wall-Brake (`PhysicsRagdollSpeedA/B` & `RagdollLookAhead`)**:
   - In `sub_00077D60` (`0x00077E03` and `0x00077E78`), scales `ctx.xmm[1].f[0]` (`top_speed_cap^2`) by `r * r * 4.0f` without touching `ctx.xmm[0].f[0]` (`|v|^2`).
   - In `sub_000E04E0` (`0x000E054A` and `0x000E056D`), scales down `RagdollLookAhead` by `std::max({1.0f, ragdoll_launch, character_reaction, explosion_force})` so the 8-frame wall-braking raycast does not prematurely brake launched ragdolls in mid-air.
4. **Universal Radial Blast Impulse & Upward Lift (`sub_000DCC40`)**:
   - `0x000DCC6A` (`PhysicsUniversalBlastImpulse`): Reads `body_type = ctx.eax` (loaded at `0x000DCC60 mov eax, [ecx+0xC4]`), scales all 4 SIMD lanes `ctx.xmm[1].f[0..3]` by `TimeCompensatedScale(explosion_force * ragdoll_launch, 2.0f)`, and boosts vertical lift `ctx.xmm[0].f[1]`.
   - `0x000DCCCA` (`PhysicsBlastRagdollLift`): Scales the vertical ragdoll lift component `ctx.xmm[0].f[0]` by `max(ragdoll_launch, explosion_force)`.
5. **Universal Bullet / Point Impact Impulse (`sub_000DCF80`)**:
   - `0x000DCFA7` (`PhysicsUniversalPointImpulse`): Scales all 4 SIMD lanes `ctx.xmm[0].f[0..3]` of the broadcast point-impulse scalar by `TimeCompensatedScale(impact_force * ragdoll_launch, 2.0f)`, and adds upward lift to `ctx.xmm[1].f[1]`.
6. **Universal Ragdoll Bone Lift & Impulse (`sub_000DC960`)**:
   - `0x000DC983` (`PhysicsRagdollBoneLift`) & `0x000DC9A6` (`PhysicsRagdollBoneImpulse`): Scales vertical bone lift and bone impulse magnitude by `TimeCompensatedScale(character_reaction * ragdoll_launch * impact_force, 1.85f)`.
7. **World Gravity & Physics Time Scale (`0x001F11D5` & `0x000CE840`)**:
   - `PhysicsGravityPost` (`src/game/physics_lab.cpp`) and `PhysicsTime` (`src/game/hooks.cpp`): Scales world gravity (`world + 0x364` / `world + 0x374`) and active rigid-body vertical impulses (`body + 0x94`) by `gravity_scale`, and simulation step `dt` by `physics_time_scale` without compounding across frames.
