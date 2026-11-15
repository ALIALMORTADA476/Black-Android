# Smoke Trail Implementation Report (`SMOKE_TRAIL_IMPLEMENTATION.md`)

**Project:** BLACK Enhanced Edition  
**Repository:** `E:\ai\black` (Branch: `black-enhanced-edition`)

---

## 1. Independent Smoke Architecture

Smoke trails run on their own independent pipeline and ring buffer (`std::array<SmokeNode, 192>`) in `src/game/ballistics_fx.cpp`, controlled separately from bullet tracer visibility:

- **Toggling Bullet Tracers OFF (`tracers_enabled = false`) while Smoke Trails remain ON continues to emit weapon smoke exhaust**, and vice versa.
- **Pool Capacity Caps:**
  - `Enhanced` / `Custom`: up to `192` simultaneous active `SmokeNode` particles (`96` max simultaneous tracers).
  - `Performance` preset: capped at `64` `SmokeNode` particles (`32` max simultaneous tracers) for locked high-FPS thermal headroom on mobile SoCs.

---

## 2. Emission Sources

1. **Muzzle Discharge Vapor (`OnPhysicalBulletSpawn`):**
   - Emits barrel discharge smoke ahead of `visual_origin` (`visual_origin + dir * 0.18m`, plus an extra forward wisp at `dir * 0.48m` for player first-person visibility), grouped for multi-pellet shotgun blasts (`same_shot_burst` `< 8ms`) so shotguns produce one cohesive barrel blast cloud.
2. **In-Flight Bullet Vapor Wisp (`OnPhysicalBulletStep`):**
   - Samples bullet flight distance once `traveled >= 1.2m` every `2.6m` (`Sniper` / `Lmg`) or `4.2m` (other calibers), storing `prev_pos` and `pos` so `RenderWorldScene` emits both a 3D tapered directional wisp quad (`Emit3DRibbon`) and a 6-slice radial soft gradient disc (`Emit3DSoftDisc` fading from center alpha to `0` at perimeter, alpha-blended `SRCALPHA + INVSRCALPHA` with `ZENABLE=1, ZWRITE=0` hardware Z-test).
3. **Hitscan Vapor Trail (`OnHitscanShot`):**
   - Places distributed smoke nodes along `[start -> end]` when smoke is enabled.
4. **Explosive Launcher & Grenade Exhaust (`RenderWorldScene` Pool B Monitor):**
   - Reads `[0x002D1998] + 0x3720` active bitmap and Pool B slots at exact engine stride `0x2400 + i * 0x120` (world position `+0xA0..+0xA8`, verified from `sub_001E35F0`), emitting dense rising exhaust plumes (`ttl * 1.45`, base radius `0.16 * density`) along real rocket/M203 trajectories.

---

## 3. Physics & Dissipation Model

- **Thermal Buoyancy + Air Drag:**
  - Each `SmokeNode` rises gently (`vel.y` ~ `0.34..0.55 m/s`) while lateral drift decelerates smoothly via atmospheric drag (`drag = max(0, 1 - world_dt * 1.6)`).
- **Turbulent Expansion & Smooth Alpha Fade:**
  - Radius expands naturally across normalized lifetime `life = age / ttl`: `base_radius * (1 + life * (from_launcher ? 3.8 : 2.6))`.
  - Alpha attenuates via non-linear curve `opacity * (1 - life)^1.65`, avoiding sudden pop-out disappearance.
- **Time Stop Aware:**
  - During `time_stop::IsFrozen()`, `world_dt = 0.0f`, suspending both smoke drift and dissipation until time resumes.

---

## 4. User Controls & Smoke Modes

| Control | Range / Options | Effect |
|---|---|---|
| `smoke_trails_enabled` | `true` / `false` | Master independent smoke trail switch |
| `smoke_mode` | `0=Off`, `1=Light`, `2=Medium`, `3=Enhanced` | Scales density multiplier (`0 / 0.58x / 1.00x / 1.48x`) and TTL factor (`0.72x / 1.00x / 1.18x`) |
| `smoke_intensity` | `10% .. 200%` (default `105%`) | Continuous opacity & puff thickness multiplier (`0.10x .. 2.00x`) |
| `smoke_duration` | `0.20s .. 3.00s` (default `1.10s`) | Base dissipation lifetime in world seconds |
