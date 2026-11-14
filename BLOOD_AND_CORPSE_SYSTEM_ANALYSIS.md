# Black Android — Combat Realism, Blood & Corpse System Analysis (`BLOOD_AND_CORPSE_SYSTEM_ANALYSIS.md`)

> **Scope & Honesty Standard**: This analysis strictly separates **confirmed guest/host symbols & memory offsets** observed in the repository from **unconfirmed internals** inside Xbox `default.xbe` and original disc archives (`.iso` / `levels/` / `chars/` / `data/`), which were not yet extracted or disassembled during this audit.

---

## 1. Current State in `Black-Android`

At present, `src/game/` contains **no dedicated hooks** for blood emitters, blood decals, corpse despawn timers, bullet impact tables, or explosion/smoke emitters. However, existing cheat, ESP, physics, LOD, and draw-list hooks reveal key parts of Black's entity, combat hit, particle, and draw-list architecture:

| System | Confirmed Source Files & Symbols | Observed Offsets / Behavior |
|---|---|---|
| **Player & NPC Character Structs** | [`src/game/cheats.cpp`](src/game/cheats.cpp#L14-L30), [`src/game/esp.cpp`](src/game/esp.cpp#L18-L24), [`src/game/layout.h`](src/game/layout.h#L33-L35) | `kWorld = 0x2D45D4`, `kWorldPlayer = 0x30`, `kCharPosition = 0xA0`, `kCharKind = 0xC4` (`1` = enemy, `2` = player), `kCharHealth = 0x2F0` (`float`), `kCharDying = 0x384` (non-zero once killed), `kCharInvulnerable = 0x8F2`, `kFullHealth = {1200.0f, 750.0f}`. |
| **Bullet Hit Handler (Soldiers)** | [`src/game/cheats.cpp`](src/game/cheats.cpp#L80-L87) | `sub_001E74B0`: `thiscall` (`ecx` = victim, 6th stack arg `[esp+0x18]` = attacker, `ret 0x1C`). Player damage calls are noted in comments (`0x1ED990`, `0x1ED700`, `0x1ED2B0`, `0x1ED5D0`, `0x1ED1E0`). |
| **Particle Fade & Culling** | [`src/game/hooks.cpp`](src/game/hooks.cpp#L564-L580), [`config/game.toml`](config/game.toml) | `ParticleFade` (`@ 0x16DFFB`): `edi` = particle type (`kParticleFarCut = 0xC4`), `edx` = camera (`kCameraFarCut = 0x88`). Scales particle fade start alongside `Reach()` (`draw_distance` 1×–8×). Additional `LimitLonger` (`0x16DF94`, etc.) and `DistanceShorter` (`0x16086D`, `0x17DAD3`, etc.) hooks expand particle & bullet-hole draw distances! |
| **Draw List Capacity** | [`src/game/hooks.cpp`](src/game/hooks.cpp#L603-L615), [`src/game/layout.h`](src/game/layout.h#L44-L50) | `DrawListRoom` (`@ 0x1EE5EA`): Reallocates guest draw lists (`kDisplayDrawLists = 0xCA6C`) to `{4096, 0, 2048, 512}` entries via `mem::AllocSystem(caps[i] * 28)` (`kAlphaDrawList = 2`), proving guest render capacity caps can be safely expanded in host memory! |
| **Physics & Ragdoll Timing** | [`src/game/hooks.cpp`](src/game/hooks.cpp#L429-L534) | `PhysicsTime` (`@ 0x1F11CD`), `BodyRestTick` (`@ 0x77EE9`), `RagdollLookAhead` (`@ 0xE054A`, `0xE056D`) already hook ragdoll/physics solver (`kPhysics = 0x2D45F4`). |
| **Weapon FX & Debris (Shards)** | [`src/game/hooks.cpp`](src/game/hooks.cpp#L243-L250,L538-L562) | `FlashSprite` (`0x16B312`), `GunEffect` (`0x176C6E`), `GunEffectStart` (`0x162A20`), `TracerStart` (`0x1E285F`), glass shards `sub_001690D0` (`0x110` bytes per shard). |
| **HUD / 2D Damage Overlay Target** | [`src/game/hud.cpp`](src/game/hud.cpp#L244-L262), [`src/game/esp.cpp`](src/game/esp.cpp#L57-L59), [`native/vulkan.cpp`](native/vulkan.cpp#L3155-L3266) | `game::HudShown()` reports active gameplay vs cutscene/loading; `VulkanBackend::DrawFrame()` & `RenderUi()` execute Vulkan fullscreen post-process passes (`kFilterHlsl`) and ImGui drawlists every frame. |

---

## 2. Feature-by-Feature Feasibility & Technical Breakdown

### 2.1 Additional Visual Damage Effects (Screen Blood Vignette, Damage Pulse, Desaturation)
- **Layer**: **Native Host (`native/vulkan.cpp`)** + **Port Common (`src/common/options.h`, `menu.cpp`)** reading confirmed guest RAM (`src/game/cheats.cpp`, `src/game/hud.cpp`).
- **Implementation Approach**:
  - Track the player's current health (`G<float>(Player() + 0x2F0)` where `Player() = R32(0x2D45D4) + 0x30`, checking `kCharKind == 2`, `kCharDying == 0`, and `game::HudShown()`) against previous-frame health to detect damage impulses and compute low-health ratio (`health / FullHealth()`).
  - Pass `[damage_flash, low_health_ratio, hit_time]` to [`native/vulkan.cpp`](native/vulkan.cpp) (`kFilterHlsl` / `DrawFrame` constant buffer) or render a soft procedural/textured radial blood splatter overlay in `RenderUi()`.
  - Can also hook `sub_001E74B0` or player damage calls to compute directional damage indicators if desired.
- **Compatibility Risk**: **Very Low** (read-only on guest memory; zero effect on recompiled `default.xbe` logic or saves).
- **Performance Cost**: **Negligible** (trivial ALU vignette/tint math in existing fullscreen shader pass).
- **Feasibility**: **Easy** (host-only screen damage effects) / **Difficult & Unconfirmed** (for world-space wound decals on character skin meshes — requires skeletal/material RE).

### 2.2 Increasing Blood Effects (Particle Count, Droplet Scale, Spray Duration)
- **Layer**: **Recompiled Game Code (`default.xbe` + `config/game.toml` + `src/game/`)** + **Original Game Assets (particle FX / blood textures)**.
- **Known Anchors & Gaps**:
  - Confirmed bullet-hit entry (`sub_001E74B0`), particle fade scaling (`0x16DFFB` / `0x16DF94`), and alpha draw-list expansion (`DrawListRoom` cap `2048` in `hooks.cpp` — can be raised further if particles overflow).
  - **Unconfirmed**: Exact blood emitter spawn calls inside `sub_001E74B0` (or called sub-functions), particle pool allocator capacity, particle archetype descriptors, and blood emitter burst counts/lifetimes inside `default.xbe` and disc particle files.
- **Implementation Approach**:
  1. Disassemble USA `default.xbe` around `0x1E74B0` and cross-reference `0x16DFFB` (`ParticleFade`) to locate the particle pool allocator and blood spawn function(s).
  2. Hook spawn parameters via `config/game.toml` (`[[hook]]`) to multiply blood particle burst counts/velocities/scale, and expand the guest particle pool via `mem::AllocSystem` if fixed-size rings drop particles under heavy combat.
- **Compatibility Risk**: **Moderate** — overflowing fixed-size guest particle or alpha draw-list pools can drop muzzle flashes, smoke, or HUD quads unless all coupled buffers (`DrawListRoom` + particle pool) are resized together.
- **Performance Cost**: **Low–Moderate** on mobile Adreno/Mali GPUs (primarily alpha fill-rate/overdraw when multiple blood clouds fill the screen at close range).
- **Feasibility**: **Unconfirmed (requires `default.xbe` RE)** → **Moderate–Difficult** to implement cleanly.

### 2.3 Persistent Blood Decals (Floor/Wall Splatters)
- **Layer**: **Recompiled Game Code (`default.xbe` + `config/game.toml` + `src/game/`)**.
- **Known Anchors & Gaps**:
  - `config/game.toml` / `hooks.cpp` comments explicitly confirm Black already creates bullet holes and decals whose draw distance is scaled by `DistanceShorter` / `LimitLonger` (`"Characters, props, pickups, ground cover, particles and bullet holes"` at `hooks.cpp:565`).
  - **Unconfirmed**: Exact address of the decal/bullet-hole ring buffer, maximum active decal count (often small on 64 MB Xbox hardware, e.g., 32–128 slots where oldest decals or blood splatters overwrite immediately), and decal fade-out timer fields.
- **Implementation Approach**:
  1. Disassemble `0x16086D`, `0x1CA677`, `0x1D0FB1`, `0x1CA942`, `0x17DAD3` (`DistanceShorter` sites in `config/game.toml`) and trace decal allocation/submission.
  2. Expand the decal array capacity using `mem::AllocSystem` (mirroring `DrawListRoom` in `hooks.cpp`) and clamp/zero the blood-decal fade-out rate (keeping an upper bound such as 512–1,024 decals or configurable cap in `black.toml` so performance doesn't degrade over a long firefight).
  3. If alpha draw lists (`kAlphaDrawList = 2`, currently 2,048 entries) saturate from persistent decals, raise `DrawListRoom` `caps[2]` accordingly.
- **Compatibility Risk**: **Moderate–High** — replacing a fixed guest static buffer pointer requires catching *every* code reference in `default.xbe` that iterates or clears the decal table on level load/unload (`Mem/LevelReset`), otherwise stale pointers or leaks across missions will crash.
- **Performance Cost**: **Moderate** — decal overdraw and draw-call submission count scale linearly with retained decals in camera frustum.
- **Feasibility**: **Unconfirmed (requires `default.xbe` RE)** → **Difficult**.

### 2.4 Longer Corpse Persistence
- **Layer**: **Recompiled Game Code (`default.xbe` + `config/game.toml` + `src/game/`)**.
- **Known Anchors & Gaps**:
  - Confirmed character struct markers: `kCharKind = 0xC4`, `kCharDying = 0x384` (`cheats.cpp`, `esp.cpp`), plus physics world body iteration (`kPhysicsBodies = 0x29C`, `BodyRestTick` in `hooks.cpp`).
  - **Unconfirmed**: Exact corpse despawn timer offset inside the character/corpse struct, dead-body queue/pool limit (Black aggressively culls corpses once ragdolls settle or a max active corpse cap is exceeded), and **critical gameplay coupling**: whether mission encounter scripts reuse fixed enemy pool slots or wait for dead bodies to despawn before spawning reinforcements!
- **Implementation Approach**:
  1. Trace writes to `character + 0x384` (`kCharDying`) inside `default.xbe` to find the state machine transition into corpse state, the despawn countdown timer, and the corpse slot limit check.
  2. Extend the despawn timer or separate dead visual ragdolls from active AI slots *only if* enemy pools are independent; otherwise cap persistence conservatively (e.g., 2×–4× longer or up to `N` corpses before FIFO eviction) and reset cleanly on mission unload.
- **Compatibility Risk**: **High** — on original Xbox titles with fixed AI/character object pools, keeping corpses alive indefinitely can **soft-lock mission waves** (new enemies fail to spawn because corpse slots are occupied) or stall physics solver lists (`PhysicsBodies` max traversal 4,096 in `hooks.cpp:448`).
- **Performance Cost**: **Moderate–High** — active skinned meshes + ragdoll bodies increase CPU matrix skinning (`D3D` / `generated/game`) and Vulkan vertex upload bandwidth unless sleeping ragdolls freeze matrix updates.
- **Feasibility**: **Unconfirmed (requires `default.xbe` RE)** → **Difficult**.

### 2.5 More Diverse Bullet Impact Effects (Sparks, Dust, Ricochets, Surface Debris)
- **Layer**: **Recompiled Game Code (`default.xbe`) + Original Game Assets (`data/` / `levels/` particle & material tables)**.
- **Known Anchors & Gaps**:
  - Entirely **Unconfirmed**: Material-to-impact lookup tables (concrete, metal, wood, glass, flesh, water) and impact emitter archetypes live inside `default.xbe` and compiled level/particle archives. Glass shard behavior (`sub_001690D0` in `hooks.cpp`) is confirmed as a distinct subsystem.
- **Implementation Approach**: Reverse-engineer the raycast impact dispatcher in `default.xbe`, hook surface-material effect selection to trigger multi-layer particles (e.g., spark + smoke puff + chip debris simultaneously) or re-use existing particle archetypes (e.g., muzzle spark / grenade smoke / glass shard emitters) cleanly.
- **Compatibility Risk**: **Moderate** (particle pool saturation during rapid-fire or shotgun bursts).
- **Performance Cost**: **Moderate** (extra particle draw calls and fill-rate during intense shootouts).
- **Feasibility**: **Unconfirmed** → **Difficult**.

### 2.6 Improved Explosion & Smoke Effects
- **Layer**: **Dual-Layer** — **Host Renderer (`native/vulkan.cpp` + `ShaderHooks`)** + **Recompiled Game Code (`default.xbe` particle/light hooks)**.
- **Implementation Approach**:
  - *Host Vulkan / Shader enhancements (Confirmed feasible)*: Tune glow/bloom strength and HDR highlight curve in [`native/vulkan.cpp`](native/vulkan.cpp) (`GlowComposite()` & `kFilterHlsl::Tone()`) so explosions/muzzle flashes bloom warmly without blowing out the image; optionally expose explosion/particle dynamic light multipliers or additive overbright scaling via `ShaderHooks::Output()`.
  - *Guest Engine enhancements (Unconfirmed addresses)*: Increase particle fade distances (`ParticleFade` at `0x16DFFB` is already hooked!) and locate explosion emitter scale/smoke duration/debris count constants in `default.xbe`.
- **Compatibility Risk**: **Low** (Host shader tuning) / **Moderate** (Guest explosion pool expansion).
- **Performance Cost**: **Low** (Shader math) / **Moderate–High** (Large multi-layered transparent smoke billboards on mobile GPUs cause heavy overdraw, especially at >100% render resolution).
- **Feasibility**: **Moderate** (Host bloom/glow + `ParticleFade` distance tuning) / **Difficult & Unconfirmed** (Guest explosion archetype & smoke density overhaul).

---

## 3. Summary Matrix

| Modification | Layer | Confirmed Anchors | Feasibility | Risk | Perf Cost |
|---|---|---|---|---|---|
| **Visual Damage Vignette / Blood Pulse** | Host Vulkan/UI + Guest RAM read | `kCharHealth` (`0x2F0`), `kCharDying` (`0x384`), `HudShown()`, `kFilterHlsl` | **Easy** | Very Low | Negligible |
| **Explosion Glow & Particle Fade Tuning** | Host Vulkan + Existing Guest Hook | `GlowComposite()`, `Tone()`, `ParticleFade` (`0x16DFFB`) | **Moderate** | Low | Low |
| **Increased Blood Particle Effects** | Guest `default.xbe` + Assets | `sub_001E74B0`, `DrawListRoom` (`0x1EE5EA`) | **Unconfirmed / Difficult** | Moderate | Low–Mod |
| **Persistent Blood Decals** | Guest `default.xbe` | `DistanceShorter` (`0x16086D`, etc.), `DrawListRoom` | **Unconfirmed / Difficult** | Mod–High | Moderate |
| **Longer Corpse Persistence** | Guest `default.xbe` | `kCharDying` (`0x384`), `PhysicsTime` (`0x1F11CD`) | **Unconfirmed / Difficult** | **High** | Mod–High |
| **Diverse Bullet Impact Effects** | Guest `default.xbe` + Assets | `sub_001690D0` (glass shards only) | **Unconfirmed / Difficult** | Moderate | Moderate |

