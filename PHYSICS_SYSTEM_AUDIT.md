# BLACK Enhanced Edition — Stage 6: Physics System Audit

**Repository:** `E:\ai\black` (Branch: `black-enhanced-edition`)  
**Target Binary Audited:** `assets/default.xbe` (read-only) & recompiled C++ (`generated/game/`, `src/game/hooks.cpp`)  
**Audit Method:** Direct disassembly inspection of recompiled XBE functions (`generated/game/x86_recomp.*.cpp`), XBE `.rdata`/vtable binary layout extraction, and cross-reference with existing `config/game.toml` runtime hooks.

---

## Executive Summary

BLACK (2006, Criterion/RenderWare-derived custom Xbox engine) separates world physics simulation, projectile hit response, radial blast broadcasting, glass/destructible shard stepping, weapon recoil, and ragdoll solver limits into well-defined guest C++ functions. Because the ARM64 Android port executes statically recompiled x86 code against guest memory via `X86Context`, all six target **PHYSICS LAB** controls map cleanly to verified, reversible **guest-side hooks** registered in `config/game.toml` and implemented in `src/game/physics_lab.cpp`—without fabricating sliders or breaking original `Classic` (`1.0x`) gameplay.

Conversely, expanding static guest pools (e.g., increasing the fixed 20-entry `0x14`-slot dying/corpse table `[ecx+eax+0x2960]` or expanding blood/decal ring buffers) requires deeper allocator/array reverse engineering and is explicitly classified as **Planned (Requires Deep RE)**.

---

## Section 1 — Deep Subsystem Investigation (A–E)

### A. Explosion Physics
- **Blast Broadcaster (`sub_001E90B0`, `generated/game/x86_recomp.51.cpp`)**:
  - Called from weapon/ordnance detonation paths (`generated/game/x86_recomp.39.cpp:1837, 2225, 2749, 2886` and `sub_001EB1D0` at `generated/game/x86_recomp.52.cpp:4363`).
  - Iterates spatial candidates around the detonation origin, wakes dormant rigid bodies via `sub_001DDF50` -> `sub_000E0820` (`[obj+0xB4]`), computes radial blast attenuation, spawns explosion FX (`sub_00169960` at `0x1EA004`), and dispatches entity virtual blast handlers at vtable slot `+0x54`.
- **Verified Radial Rigid-Body Impulse Paths (All feed `sub_000DCC40`, `x86_recomp.23.cpp`)**:
  1. `sub_001E3FA0` (called by `sub_001EA0D0` pushing `100.0f` = `0x42C80000`): pushes radial force scalar `xmm0` into `sub_000DCC40` at **`0x001E4034`**.
  2. `sub_001E5410` (Destructible / Multi-part Props vtable `0x28ED70`, slot `+0x54` at `0x28EDC4`): loads radial blast impulse `[ebp+0x10]` into `xmm0` and calls `sub_000DCC40` at **`0x001E568E`**.
  3. `sub_001E8D80` (Physics Props vtable `0x28EC80`, slot `+0x54` at `0x28ECD4`): clamps radial impulse `>= 70.0f` (`[0x2888C8]`), invokes `sub_001D3F80`, and calls `sub_000DCC40` at **`0x001E8EFC`**.
  4. `sub_001DF830` (Character vtable `+0x54` at `0x28C214`): handles blast damage and character stagger/death transition (`sub_00195920` anim state `0x1E` / `0x20`).
- **Low-Level Solver Scaling (`sub_000DCC40`)**:
  - Scales incoming `xmm0` scalar impulse by `[0x0028B260]` (`0.03846154f = 1/26`) and `[world+0x380]` (`dt = 1/60`), applies linear delta to `[body+0x90..0x98]`, and torque `(hit_pos - com) x impulse` to `[body+0xA0..0xA8]`.
- **Runtime Mutability**: Safe at runtime. Scaling `ctx.xmm[0].f[0]` strictly at **`0x001E4034`**, **`0x001E568E`**, and **`0x001E8EFC`** alters explosion physical impulse cleanly with zero effect on gunshot damage or gravity.

### B. Character Physics & Ragdoll
- **Death / Dying State Machine**:
  - `kCharDying = 0x384`, `kCharHealth = 0x2F0` (`src/game/hooks.cpp`).
  - `sub_001DA240` sets `[edi+0x384] = 1` on fatal damage, records timestamp `[edi+0x2F8]`, and triggers ragdoll transition.
  - `sub_001DFD90` updates active ragdoll/dying character transforms (`0x384 == 2` dispatches `sub_0017F0A0`).
  - `sub_001DF440` indexes a fixed `0x14` (20-slot) byte table `[ecx+eax+0x2960]` for corpse/ragdoll slots.
- **Active Ragdoll Solver (`x86_recomp.11.cpp` + `config/game.toml`)**:
  - **`BodyDamping` (`0x00077D9B`)**: computes per-tick linear (`xmm0`) and angular (`xmm1`, stored at `[esp+0x0C]`) velocity retention factors (`1.0f - damping`). Followed immediately by **`0x00077DA1`** (`lea edx, [esp+0x0C]`).
  - **`BodyTopSpeed` (`0x00077DFA`, `0x00077E6F`)**: clamps ragdoll bone speed (`ctx.xmm[1].f[0]`) against frame-adjusted ceiling right before `mulss xmm1, xmm1` at **`0x00077DFF`** and **`0x00077E74`**.
  - **`BodyRestTick` (`0x00077EE9`)**: controls rest sleep counter `[esi+0xAC]`.
  - Joint constraints: `JointTurnCap*`, `ContactReach`, `RagdollLookAhead`, `JointAnchorGap`, `JointTurnBack` in `config/game.toml`.
- **Runtime Mutability**: Safe and responsive at runtime via post-hooks at **`0x00077DA1`** (ragdoll damping retention), **`0x00077DFF`**, and **`0x00077E74`** (ragdoll peak speed cap).

### C. Projectile & Impact Physics
- **Hitscan / Projectile Raycast Dispatch (`sub_001E2B90`, `x86_recomp.51.cpp:6351-6534`)**:
  - Computes hit vector and reads bullet/ammo impact scalar `[eax+0x18]` (`0x1E2CBC`), stores `[esp+0x14]`, scales direction, and calls vtable slot `+0x4C` (`0x1E2D1C`, e.g., `sub_001E3F30`, `sub_001DFC30`).
- **Separated Rigid-Body Bullet Impact Applicators (Vtable slot `+0x48`)**:
  - Confirmed via `.rdata` XBE vtable scan (`0x28EC80` Props, `0x28ED70` Destructibles):
    1. `sub_001E5710` (Destructibles slot `+0x48` at `0x28EDB8`): spawns particle burst (`sub_001E5070`) and applies directional bullet impulse `xmm0` to `[esi+0xB4]` via `sub_000DCF80` at **`0x001E5952`**.
    2. `sub_001E8F20` (Props slot `+0x48` at `0x28ECC8`): calls `sub_001E8C50` (`[esp+0x18] * 0.04f`) and applies rigid-body bullet impulse `xmm0` via `sub_000DCF80` at **`0x001E908D`**.
    3. `sub_001E74B0` (Characters slot `+0x48` at `0x28C208`): handles character hit damage & stagger (`sub_001CC1A0`, `sub_0017F270`, `sub_001BE350`).
- **Critical Design Distinction**: Hooking **`0x001E5952`** and **`0x001E908D`** (both feeding `sub_000DCF80`) scales bullet/projectile *physical knockback on props and destructibles* independently of bullet damage numbers and without colliding with explosion radial force (`sub_000DCC40`).

### D. Weapon Behavior
- **Player Recoil Kick (`sub_001EEFD0`, `x86_recomp.52.cpp:17072-17091`)**:
  - Checks player marker `[eax+0xC4] == 2`.
  - Computes pitch recoil delta in `xmm1` (`[ebx+0x68] - [eax+0x4C4]) * [ebx+0x6C]`) and adds it to `[eax+0x4C4]` at **`0x001EFE50`** (`addss xmm1, [eax+0x4C4]`).
- **Recoil Recovery (`RecoilDecay`, `0x001E5FB7`) & Weapon Sway (`SwayBegin`/`SwayEnd`)**:
  - `RecoilDecay` already scales exponential recoil decay by frame time in `src/game/hooks.cpp`.
- **Runtime Mutability**: Hooking **`0x001EFE50`** and scaling `ctx.xmm[1].f[0]` (only when `X86_MEM32(ctx.eax + 0xC4u) == 2u`, and respecting `no_recoil` cheat) provides zero-overhead, instant weapon recoil tuning from `0%` (`0.00x`) to `250%` (`2.50x`).

### E. World Physics & Debris
- **World Gravity Vector (`kPhysics = 0x2D45F4`)**:
  - `sub_000754D0` initializes `[world+0x360]` = `{0.0f, -600.0f, 0.0f, 1.0f}`, `[world+0x380]` = `1/60`, `[world+0x384]` = `60.0f`.
  - `sub_001F1080` runs `PhysicsTime @ 0x001F11CD` (`src/game/hooks.cpp`) right before stepping the world solver (`0x001F11D8 call [edx]`), writing `[world+0x370] = [world+0x360] * dt * dt`.
  - Hooking **`0x001F11D5`** (`lea esi, [edi+0x30]`, immediately post-`PhysicsTime` and pre-solver) lets the host compute `kBaseGravityY (-600.0f) * gravity_scale` non-compoundingly into `[edi+0x364]` and refresh `[edi+0x374] = (-600.0f * gravity_scale) * dt * dt`.
- **Debris / Glass Shards & Particles (`sub_001690D0`, `x86_recomp.37.cpp`, `ParticleFade`, `x86_recomp.38.cpp`)**:
  - `ShardStep` (`0x001693ED`) steps shard velocity (`xmm0` = `[ebx+0xC0]` gravity/accel) into `[eax]`; second `ShardStep` (`0x001693FB`) steps position by shard velocity.
  - Pre-hooking **`0x001693EA`** (`movaps xmm1, [eax]`) scales shard gravity `ctx.xmm[0].f[0..2]` by `gravity_scale`.
  - Pre-hooking **`0x001693F6`** (`movaps xmm1, [esp+0x44]`) scales airborne shard velocity step `ctx.xmm[0].f[0..2]` by `debris_intensity`.
  - Post-hooking **`0x0016DFFF`** (after `ParticleFade @ 0x0016DFFB`) scales particle distance fade decrement `ctx.xmm[3].f[0]` by `1.0f / debris_intensity` so glass/dust/debris smoke remains crisp and intense under elevated debris presets without spiking mobile overdraw.

---

## Section 2 — Stage 6 Section 5: Advanced Combat Systems Feasibility Audit

| Candidate System | Findings from `default.xbe` & Codebase | Feasibility Verdict |
|---|---|---|
| **Higher blood particle count** | `sub_001E74B0` (`x86_recomp.51.cpp`) & `BLOOD_AND_CORPSE_SYSTEM_ANALYSIS.md` confirm blood FX emit through `sub_00169960` / particle manager `0x2D1990+0x65690` with pool caps. Hook-only boost is possible once emitter burst-count args at `sub_001E74B0` call sites are mapped. | **Planned (Requires Targeted Burst-Count RE)** |
| **Persistent wall decals & bullet marks** | Wall decal slots are ring-buffered inside the RenderWare/world decal array with a fixed static capacity. Enlarging the ring requires reallocating/relocating guest heap globals outside safe `[[hook]]` patching. | **Planned (Requires Deep Memory RE)** |
| **Longer corpse persistence** | `sub_001DF440` reads/writes a fixed `0x14` (20-slot) byte table `[ecx+eax+0x2960]`, and `sub_001DA240` records death timestamp `[edi+0x2F8]`. Extending timeout before slot recycling is feasible once the recycle comparator on `+0x2F8` is isolated, but total simultaneous bodies stay capped at 20 without buffer relocation. | **Planned (Timeout RE Needed; 20-Slot Cap Fixed)** |
| **More dynamic death animations / ragdoll** | Directly confirmed and wired via `BodyDamping`, `BodyTopSpeed` (`0x00077DA1`, `0x00077DFF`, `0x00077E74`) and `sub_001E4034`/`1E568E`/`1E8EFC` impulses. | **Implemented Now (`Character Reaction`)** |
| **Expanded glass & structural debris** | Directly confirmed in `sub_001690D0` (`0x001693EA`, `0x001693F6`) and `ParticleFade` (`0x0016DFFF`). | **Implemented Now (`Debris Intensity`)** |
| **Stronger explosion reaction on enemies & props** | Directly confirmed in `sub_001E90B0` -> `sub_001E3FA0`, `sub_001E5410`, `sub_001E8D80` -> `sub_000DCC40`. | **Implemented Now (`Explosion Force` + `Character Reaction`)** |

