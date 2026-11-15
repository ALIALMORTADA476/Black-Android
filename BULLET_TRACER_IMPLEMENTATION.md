# Bullet Tracer Implementation Report (`BULLET_TRACER_IMPLEMENTATION.md`)

**Project:** BLACK Enhanced Edition  
**Repository:** `E:\ai\black` (Branch: `black-enhanced-edition`)  
**Release Target:** `versionCode = 54` (`2.1.0-ballistics-tracers`)

---

## 1. Architecture Overview

The bullet tracer system is implemented as a dedicated runtime subsystem in `src/game/ballistics_fx.h`, `src/game/ballistics_fx.cpp`, `src/game/ballistics_rules.cpp`, and `src/d3d/world_overlay.h` + `src/d3d/d3d8.cpp` (`d3d::DrawWorldOverlay`). It operates purely as a **read-only observer** of BLACK's native ballistics engine and renders physical bullet trajectories and synchronized hitscan visuals directly inside the **3D hardware depth-tested scene pass** (`sub_00176F60` at the end of `kAlphaDrawList`, prior to `kGunDrawList` first-person viewmodel & 2D HUD).

### Why Z-Buffered 3D World Quads Inside `kAlphaDrawList`
1. **True Depth Occlusion Behind Buildings & Walls:** By submitting `D3DFVF_XYZ | D3DFVF_DIFFUSE` camera-aligned 3D triangle quads through `d3d::DrawWorldOverlay(..., true)` with `ZENABLE=1`, `ZWRITEENABLE=0`, `ZFUNC=D3DCMP_LESSEQUAL`, and `CULLMODE=NONE` directly against the active Vulkan depth-stencil buffer (`draw_ds_`), every tracer and smoke plume is cleanly occluded by walls, buildings, terrain, and foreground geometry.
2. **Physics & Damage Fidelity Preserved:** Physical projectile speeds (`[slot+0x7C]`), trajectories, gravity, impact resolution (`sub_001E2630`), and hitscan raycast checks are never slowed down or modified.
3. **Zero Heap Allocations During Combat:** Pre-allocated fixed ring buffers (`std::array<TracerInstance, 96>`, `std::array<MuzzleEntry, 24>`) and reusable static vertex scratch buffers avoid per-frame `malloc`/`free` spikes on ARM64 mobile GPUs.
4. **Full D3D8 Render-State Isolation:** `d3d::DrawWorldOverlay` snapshots and restores all touched transforms (`VIEW`/`PROJECTION`/`WORLD`), shaders, blend states, depth-stencil states, and texture stages before returning so guest draws remain untouched.

---

## 2. Hook & Observation Pipeline

| Engine Point | Source Hook | Data Captured (Read-Only) | Purpose |
|---|---|---|---|
| `sub_00176F60` (`kAlphaDrawList`) | `src/game/hooks.cpp` | `View` & `Projection` (`XDK_CDevice_Transforms`) | Compute world-to-clip `vp[16]`, camera eye, and forward vector (`CaptureSceneCamera`) |
| `sub_001D2860` | `src/game/hooks.cpp` | `owner = ctx.ecx`, `mat[16]` from `out` | Record real barrel muzzle coordinate prior to viewmodel FOV distortion (`NotifyMuzzleMatrix`) |
| `sub_001DB7B0` | `src/game/time_stop.cpp` | Newly activated `[0x002D12D0]+0x6E0` slot + `fire_pkt` | Spawn tracked `TracerInstance` with true `origin`, `dir`, `speed`, `WeaponClass`, suppressed state |
| `sub_001E26E0` | `src/game/time_stop.cpp` | Active slot `prev_pos -> new_pos` & active flag | Advance visual `head_pos` frame-by-frame and trigger impact decay on termination |
| `sub_001E2630` | `src/game/ballistics_fx.cpp` | `slot_ea = ctx.esi`, world impact `[slot+0x50..0x58]` | Snap visual head cleanly to impact point (`OnPhysicalBulletHit`) |
| `sub_00168E20` | `src/game/ballistics_fx.cpp` | `[start*], [end*], caller ret_addr` | Emit synchronized visual hitscan tracer (`is_physical = false`), skipping `0x001E286D` physical duplicate |

### Visual Barrel Alignment & First-Person Fix (`v54`)
- **`LookupFirePktWeapon` inline Player fix:** Fixed `kWorldPlayer` (`world + 0x30`) lookup so player ownership is reliably recognized (`owner == world + kWorldPlayer || kind == 2`, replacing an erroneous extra `R32(world + kWorldPlayer)` dereference).
- **Camera-Space Barrel Offset & Convergence:** When `sub_001D2860` does not provide an off-axis muzzle (`perp_sq > 0.0020f`), player first-person shots compute an off-axis view-space barrel origin (`dir * 0.22m + right * 0.14m - up * 0.095m`, scaled `0.50x` when zoomed `> 1.2x`) so shots never collapse onto the center crosshair pixel (`(cx, cy)`). Trails smoothly converge onto `[physics_origin + dir * d]` across `0.0m -> 3.5m` (`blend_o = clamp(tail_dist / 3.5f, 0, 1)`).

---

## 3. 3D Billboard Ribbon & Depth-Tested Additive Geometry

- **Behind-Camera Rejection:** Skips segments strictly behind `cam.eye + cam.forward * 0.08m`; perspective clipping and hardware per-pixel Z-testing run natively in Vulkan (`DrawWorldOverlay`).
- **Tapered Multi-Layer 3D Streak (`Emit3DRibbon` + `Emit3DSoftDisc`):** Each active tracer emits three camera-facing 3D triangle quads (blended additively with `SRCALPHA + ONE`) with perspective-calibrated world widths (`px_world = dist_cam * tan_half_fov_y * 0.0024f`):
  1. **Outer Atmospheric Halo:** `hw_halo * 0.15 -> hw_halo`, subtle alpha (`0.04 -> 0.28`).
  2. **Ballistic Body:** `hw_core * 0.20 -> hw_core * 1.45`, alpha (`0.10 -> 0.74`).
  3. **Incandescent Pyrotechnic Core:** `hw_core * 0.12 -> hw_core * 0.72`, crisp tip alpha (`0.18 -> 0.96`) + radial glowing head discs (`Emit3DSoftDisc` fading cleanly from center to `0` at edges).
- **Weapon Class Profiles (`ballistics_rules.cpp` + `ballistics_fx.cpp`):**
  - Distinct thickness, trail scale, brightness, and warm/amber/crimson/gold profiles for `Pistol` (plus `MGN` magnum boost), `Smg`, `Rifle`, `Shotgun` (grouping simultaneous pellets within `8ms` under one shot sequence), `Sniper`, `Lmg`, and `Launcher`.
  - Suppressed weapons (`*5` or `[weapon+0x108] != 0`) automatically scale thickness (`x0.80`), trail length (`x0.76`), and brightness (`x0.68`) with a warmer subsonic ember tone when using `Weapon Realistic` color mode.

---

## 4. Bullet Visibility Modes & Time Stop Integration

- **`ShouldEmitTracer(freq_mode, shot_seq, cls, suppressed, is_player)`** (`src/game/ballistics_rules.cpp`):
  - `0` (**Off**): Zero enhanced tracer emissions; original game behavior untouched.
  - `1` (**Every Bullet**): Renders 100% of shots.
  - `2` (**Every 2nd Bullet**): 1 in 2 (`shot_seq % 2 == 0`).
  - `3` (**Every 3rd Bullet**): 1 in 3 (`shot_seq % 3 == 0`).
  - `4` (**Every 5th Bullet**): 1 in 5 (`shot_seq % 5 == 0`).
  - `5` (**Selective**): Always shows `Sniper` & `Launcher`; shows `Lmg` & `Shotgun` every 2nd shot; suppressed rounds every 3rd; player `Rifle`/`Smg` every 3rd and AI `Rifle`/`Smg` every 4th.
- **Time Stop Suspension:** During `time_stop::IsFrozen()`, world-owned AI tracers freeze `age` advancement while active player shots remain tracked cleanly until paused or resumed, and level transitions call `ballistics::ResetAll()` via `PreFrameSync`.
