# BLACK Enhanced Edition — Projectile, Tracer & Muzzle System Audit

**Repository:** `E:\ai\black` (branch: `black-enhanced-edition`)  
**Audit Scope:** Recompiled x86 firing pipeline (`generated/game/x86_recomp.*.cpp`), weapon manager/data tables (`src/game/weapons.cpp`), camera/projection pipeline (`src/game/esp.cpp`, `src/game/hooks.cpp`), D3D8/Vulkan backend (`src/d3d/d3d8.cpp`, `native/vulkan.cpp`), and Time Stop synchronization (`src/game/time_stop.cpp`).

---

## 1. Master Weapon Firing Dispatch (`sub_001DBD50`)

All firearm discharges (player and AI) enter `sub_001DBD50` (`x86_recomp.50.cpp:17650-19060`), receiving a `fire_packet*` structure:
- `fire_pkt + 0x04`: `weapon*` (`+0x00` weapon type index, `+0xEC` weapon def ptr, `+0xF0` owner character ptr, `+0x107` player aim-origin flag, `+0x108` suppressor attached flag).
- `fire_pkt + 0x08`: `ammo_def*`.
- `fire_pkt + 0x0C`: `weapon_fire_data*` (`+0x14` range/damage scalar, `pellet_count` looped at `001DC8DF`).

At `loc_001DBF60`, each shot/pellet resolves:
1. **3D Barrel Muzzle World Matrix (`[owner+0x4C]` -> `sub_001D2860`)**: Computes a 4x4 world matrix; translation row/column `[mat + 0x30 .. +0x38]` holds the 3D weapon muzzle world position (`[esp+0x100]` in `sub_001DBD50`).
2. **Physics Origin (`[esp+0x30]`)**: If `[weapon + 0x107] != 0`, copies `[owner + 0x100]` (camera/aim ray origin) so player shots travel straight along crosshair precision, while retaining `[esp+0x100]` (`sub_001D2860`) as the visual weapon muzzle.
3. **Spread & Direction (`sub_001CEF40`)**: Computes unit direction vector into `[esp+0x40]` and muzzle velocity into `[esp+0x1C]`.
4. **Execution Fork (`kCharKind == 2` Player vs AI)**:
   - **Player (`loc_001DC06B`)**: Spawns a true physical stepped projectile via `sub_001DB7B0` into pool `[0x002D12D0] + 0x6E0`.
   - **Enemy / AI (`loc_001DC0DB..001DC8CA`)**: Executes an immediate raycast (`sub_001DAFF0` at `001DC32C`), checks camera bullet-flyby proximity, triggers impact feedback (`sub_001DB860`), and submits a native tracer streak (`sub_00168E20` at `001DC8BD`).

---

## 2. Physical Projectile Pools & Simulation Loop

### 2.1 Primary Physical Bullet Pool (`[0x002D12D0] + 0x6E0`)
- **Capacity:** `16` active slots x `0x90` bytes stride (`2,304` bytes).
- **Spawner:** `sub_001DB7B0` (`x86_recomp.50.cpp`) initializes:
  - `+0x40` `vec4`: segment start / initial world origin
  - `+0x50` `vec4`: live world position (updated per tick)
  - `+0x60` `vec4`: normalized direction vector
  - `+0x70` `uint32`: `fire_packet*` (`weapon*`, `ammo*`, `weapon_fire_data*`)
  - `+0x74` `float`: step distance delta
  - `+0x78` `float`: remaining range budget
  - `+0x7C` `float`: world speed (units/sec)
  - `+0x80` `uint8`: projectile behavior flags (`bit 1 = suppressed/no-tracer`)
  - `+0x84` `float`: spawn world clock (`[0x002D45D4] + 0x20`)
  - `+0x88` `uint8`: active slot flag (`1 = active`)
  - `+0x89` `uint8`: tracer draw-on-finish flag
- **Spawn Validation (`sub_001DB600`):** Pre-checks raycast (`sub_001DAFF0`) and victim character kind (`[slot+0x8C]`, `kind == 3, 4, or 7`).
- **Simulation Tick (`sub_001E26E0`, `x86_recomp.51.cpp:5492-5770`):**
  - Steps each active slot by `dt * speed` along `+0x60`.
  - Runs raycast `sub_001DAFF0` over `[prev_pos -> next_pos]`.
  - On hit, invokes `sub_001E2630`, which triggers `sub_001DB910` (surface impact effects, decals, ricochets, impulses, optional `sub_00168E20`).
  - Decrements remaining range `[slot+0x78]`; clears `[slot+0x88] = 0` when expired or stopped.
  - Note: Original game calls `sub_00168E20` at `0x001E286D` only at slot termination (`[slot+0x88] == 0 && [slot+0x89] != 0`).

### 2.2 Secondary Projectile Pool (`[0x002D12D0] + 0x20`)
- **Capacity:** `36` slots x `0x30` bytes, stepped by `sub_001E1630` (`x86_recomp.51.cpp:891-1105`).

### 2.3 Explosive Projectile Pools (`[0x002D1998]`)
- **Pool A (Thrown Grenades):** `32` slots x `0x120` bytes (occupancy bitmap `+0x3700`), spawned via `sub_001DCE60` (`x86_recomp.50.cpp:19760`), updated via `sub_001E4A30`.
- **Pool B (RPG / M203 Rockets):** `16` slots x `0x120` bytes (occupancy bitmap `+0x3720`), spawned via `sub_001E35F0` (`x86_recomp.51.cpp:7679`), updated via `sub_001E43F0`.

### 2.4 Native Tracer Streak Pool (`[0x002D1990] + 0x4E2C0`)
- **Spawner:** `sub_00168E20` (`x86_recomp.37.cpp:24166-24442`), writing into `18` (`0x12`) streak slots (`0x30` byte stride) and calling `sub_00165450` / `sub_00151470`.
- **Limitation in Original Engine:** Fixed `18`-slot capacity, RNG skip check when `bl == 0`, minimal single-frame streak thickness and lifetime, no configurable smoke ribbon or continuous flight attachment for high-velocity rounds.

---

## 3. Weapon Mechanism Classification

Weapon codes identified from `[0x002D12D0]` manager table (`src/game/weapons.cpp`):

| Weapon Category | Weapon Codes (`weapons.cpp`) | Player Mechanism | Enemy / AI Mechanism | Tracer & Smoke Integration Strategy |
|---|---|---|---|---|
| **Pistols** | `PST`, `PS5`, `GK1`, `GK5`, `MGN`, `MG5` | Physical Stepped (`sub_001DB7B0`) | Raycast Hitscan (`sub_001DBD50` -> `sub_00168E20`) | Compact bright streak (`MGN` heavier hot core), light transient smoke |
| **Submachine Guns** | `SMG`, `SM5`, `SM3`, `MC5`, `MP1`, `MP5`, `P90`, `P95` | Physical Stepped (`sub_001DB7B0`) | Raycast Hitscan (`sub_001DBD50` -> `sub_00168E20`) | Nimble rapid streaks honoring frequency selector, light crisp wisp smoke |
| **Assault Rifles** | `AK1`, `AK5`, `ASR`, `AS5`, `M16`, `M45`, `BNS` | Physical Stepped (`sub_001DB7B0`) | Raycast Hitscan (`sub_001DBD50` -> `sub_00168E20`) | Crisp NATO/AK tracer core + amber outer halo + subtle tapered smoke ribbon |
| **Shotguns** | `SHG`, `SH5`, `RM1`, `RM5` | Multi-Pellet Physical (`sub_001DB7B0` loop) | Multi-Pellet Hitscan (`sub_001DBD50`) | Grouped buckshot micro-streaks sharing shot burst cadence + wider cloud puff |
| **Sniper Rifles** | `SNR`, `SN5` | High-Velocity Physical (`sub_001DB7B0`) | Raycast Hitscan (`sub_001DBD50` -> `sub_00168E20`) | Long precision ballistic streak + persistent vapor/smoke contrail along flight |
| **Light Machine Guns** | `HVY`, `HV5` | Physical Stepped (`sub_001DB7B0`) | Raycast Hitscan (`sub_001DBD50` -> `sub_00168E20`) | High-intensity NATO tracer rounds (1-in-N military mix or every round) + hot smoke |
| **Rocket / Grenade Launchers** | `RPG`, `GRL` | Physical Explosive Pool B/A (`sub_001E35F0`, `sub_001DCE60`) | Physical Explosive Pool B/A | Continuous dense rocket/grenade exhaust smoke trail tracking active slot |
| **Suppressed Variants** | `*5` variants & `weapon+0x108 != 0` | Same underlying pool, flag bit observed | Hitscan flag observed | Attenuated subtle warm ember streak & low-signature smoke when Selective/Realistic color |

---

## 4. Camera, Projection & Overlay Hooks

- **3D Scene Camera Capture:** `sub_00176F60` at `kAlphaDrawList` (`src/game/hooks.cpp:215`) executes with the active scene `D3DTS_VIEW` (`kDevice + XDK_CDevice_Transforms + 0`) and `D3DTS_PROJECTION` (`+ 64`) matrices active before the HUD pass (`native/vulkan.cpp:1132`).
- **Pre-HUD Swapchain Rendering:** `app::DrawUi()` (`src/common/menu.cpp:3257`) runs inside `VulkanBackend::RenderUi()` at full native display resolution, before touch controls, subtitles, and Time Stop badges when `BackgroundDrawList` is used.
- **Physics Non-Interference Guarantee:** `ballistics_fx` hooks treat guest velocity `[slot+0x7C]`, remaining range `[slot+0x78]`, spread (`sub_001CEF40`), raycast (`sub_001DAFF0`), and hit damage (`sub_001E2630` / `sub_001DB910`) strictly as read-only inputs. Hitscan enemy trajectories remain hitscan in simulation while rendering synchronized high-speed visual streaks cleanly along `[start -> hit_end]`.
