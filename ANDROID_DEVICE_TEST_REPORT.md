# Android Device Test Report — Stage 6 Physics Lab Unleashed (Build 40)

## 1. Device & Build Identity
- **Connected Device**: Xiaomi `25080RABDG` (`damfmjij5pbe7dbu`), Android 16 (API 36), SoC `MT6878` (8 cores), GPU `Mali-G615 MC2` (Vulkan 1.3.247), Display `2772x1280 @ 120 Hz`
- **Application ID**: `com.racas.black.enhanced`
- **Installed Release**: `versionCode=40`, `versionName=1.5.2-physics-lab-unleashed`
- **Signing Certificate (Preserved Build 26 Key)**:
  - DN: `C=US, O=Android, CN=Android Debug`
  - SHA-256: `1a9833560e6294f39805fcabfe1efa64ad8df73effc992fd71fd7b045cde84e8`
- **Original Game Assets**:
  - `assets/black.iso` (`1686306816 bytes`, `574 files`) and `default.xbe` (`sha1 c5bfed25449c8212d7a976f15246e87ead57713f`) untouched.

## 2. Root-Cause Diagnosis & Fixes for Weak / Suppressed Physics Effects (`Build 39 -> Build 40`)
- **Symptom Reported**: After fixing the RPG/explosion crash in Build 39, the user reported that most Physics Lab sliders had only a very weak/slight effect during gameplay.
- **Root Causes Uncovered via Live Telemetry (`logs/black.log`) & Recompiled Assembly Inspection**:
  1. **Root Cause 1 — `IsValidGuestAddr` Upper Bound `0x04000000u` Blocked Xbox Stack (`0xD0000000`), Contiguous (`0x80000000`), and System (`0xB0000000`) Virtual Addresses**:
     - Live `logs/black.log` from 900 seconds of gameplay showed:
       `[PHYSICS_LAB] ... grav=0.10x(y=-25.0) ... hits: blast=379 impact=102 grav=0 deb=806936 rag=287230 rec=67`
     - In `xbe_sdk/runtime/kernel/memory.h`, Xbox virtual memory maps user heap up to `0x7FFE0000`, contiguous physical memory at `0x80000000`, system memory at `0xB0000000`, and thread stacks at `0xD0000000..0xDFFFFFFF`.
     - Checking `addr <= 0x04000000u` rejected 100% of addresses above `64 MB`, completely disabling:
       - `PhysicsGravityPost` (`grav=0`, leaving `y=-25.0` unscaled even when `gravity_scale = 0.10x`).
       - `ScalePushedVec3` (all 11 stack-allocated 3D velocity/impulse hooks: `PhysicsBlastLinearA`, `PhysicsBlastAngularA`, `PhysicsImpactLinearA..D`, `PhysicsImpactAngularA..D`, `PhysicsDebrisLaunch`, `PhysicsCharacterLaunchA..D`, because `ctx.esp` and `ptr` reside in `0xD0000000..0xDFFFFFFF`).
       - `PhysicsRagdollDamping` (`[ctx.esp + 0x0C]` linear velocity retention factor).
       - `ApplyRecoilKick` (`profile_reg` and `ctx.eax`).
     - **Fix**: Replaced the `0x04000000u` range check in `IsValidGuestAddr` (`src/game/physics_lab.cpp`) with the kernel's O(1) page-table check `mem::IsCommitted(addr) && mem::IsCommitted(addr + span - 1u)`.
  2. **Root Cause 2 — `PhysicsRagdollSpeedA` (`0x00077E03`) & `PhysicsRagdollSpeedB` (`0x00077E78`) Scaled `ctx.xmm[0].f[0]` (`|v|^2`), Which Clamped Speeds Down**:
     - In `generated/game/x86_recomp.11.cpp` (`sub_00077D60`), `ctx.xmm[0].f[0]` is the body's actual squared speed `|v|^2` and `ctx.xmm[1].f[0]` is `top_speed_cap^2`. When `|v|^2 > top_speed_cap^2`, the solver multiplies velocity by `sqrt(xmm1 / xmm0)`.
     - Multiplying `ctx.xmm[0].f[0] *= r` inflated `|v|^2` by `r`, reducing `xmm1 / xmm0` by `r` and clamping velocities down.
     - **Fix**: Removed `ctx.xmm[0].f[0]` scaling in `PhysicsRagdollSpeedA` and `PhysicsRagdollSpeedB` and raised `ctx.xmm[1].f[0] *= (r * r * 4.0f)`.
  3. **Root Cause 3 — `RagdollLookAhead` (`src/game/hooks.cpp`, `0x000E054A` & `0x000E056D`) Braked High-Speed Ragdolls in Mid-Air**:
     - Every frame, `sub_000E04E0` casts a ray `8 frames * speed` ahead of a ragdoll and brakes its velocity if any wall or floor is within that lookahead distance. At 5x-10x launch speeds, `8 frames * speed` spans an entire room.
     - **Fix**: Scaled down the lookahead multiplier in `RagdollLookAhead` (`src/game/hooks.cpp`) inversely with `std::max({1.0f, ragdoll_launch, character_reaction, explosion_force})`.
  4. **Root Cause 4 — Slow-Motion (`physics_time_scale < 1.0f`) Shrunk Instantaneous Impulses & Ground Friction Dragged Horizontal Launches**:
     - In `sub_000DC070`, `sub_000DC100`, `sub_000DC960`, `sub_000DCC40`, and `sub_000DCF80`, the solver multiplies impulses by `[world+0x380]` (`dt = FrameTime() * t_scale`), so slow-motion (`physics_time_scale < 1.0f`) proportionally weakened every impulse.
     - Furthermore, in `sub_001FFE70` (`001FFF7B minss xmm0, [esp+0x10]`), character death hit speed is ceiling-clamped right before `PhysicsCharacterLaunchA` (`0x1FFFB1`).
     - **Fix**: Added `TimeCompensatedScale` in `src/game/physics_lab.cpp` so slow-motion never weakens impulses, amplified post-clamp 3D launch vectors in `ScalePushedVec3`, and injected upward arc lift (`vy += lift`) proportional to horizontal speed so launched characters and props fly through the air instead of scraping along the ground (while preserving `1.0f` Classic identically).

## 3. Automated & Live Device Verification Results
- **Host C++ Test Suite (`test_settings_core.exe`)**: `17 passed, 0 failed`.
- **Host Python Test Suite (`test_settings_suite.py`)**: `10 passed, 0 failed`.
- **Live Device Verification (`damfmjij5pbe7dbu`)**:
  - Installed via `adb -s damfmjij5pbe7dbu install -r app/build/outputs/apk/release/app-release.apk` (`Success`, `versionCode=40`, `versionName=1.5.2-physics-lab-unleashed`).
  - Active PID `22971`, zero crashes in `adb logcat -b crash -d`.
  - Live `logs/black.log` telemetry from gameplay on `damfmjij5pbe7dbu` confirms `grav=0.10x(y=-2.5)` (`grav=1195` steps, previously `grav=0` and `y=-25.0` in Build 39) along with `blast=59`, `impact=5`, `deb=81300`, `rag=12839`, `rec=1`:
    ```
    [   14.195] [PHYSICS_LAB] exp=9.00x rad=3.50x imp=7.50x lnc=9.00x rag=7.00x str=9.00x deb=5.50x bnc=4.00x grav=0.10x(y=-2.5) tms=0.35x shk=2.50x rec=0.50x | hits: blast=0 impact=0 grav=1 deb=0 rag=0 rec=0
    [   39.268] [PHYSICS_LAB] exp=9.00x rad=3.50x imp=7.50x lnc=9.00x rag=7.00x str=9.00x deb=5.50x bnc=4.00x grav=0.10x(y=-2.5) tms=0.35x shk=2.50x rec=0.50x | hits: blast=59 impact=5 grav=1195 deb=81300 rag=12839 rec=1
    ```
