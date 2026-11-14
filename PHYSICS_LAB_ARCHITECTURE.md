# BLACK Enhanced Edition — Stage 6: PHYSICS LAB Architecture

## 1. Design Principles

1. **Zero Fake Sliders**: Every control in `Category::PhysicsLab` (`TacticalCategory::PhysicsLab`) binds directly to verified XBE execution sites in `config/game.toml` and `src/game/physics_lab.cpp`.
2. **Non-Compounding Determinism**:
   - Original guest constants (`-600.0f` world gravity) are NEVER read back and re-multiplied across frames.
   - Instead, `PhysicsGravityPost` computes `target_g = -600.0f * scale` from the immutable baseline and recalculates `[world+0x374] = target_g * dt * dt` once per step.
   - Selecting **Classic** (`1.00f` on every axis) causes all Physics Lab hooks to execute no-ops (or write `-600.0f` verbatim), preserving authentic 2006 Xbox BLACK physics.
- **Host-Guest Isolation**:
   - User-configured multipliers live in `modsettings::EnhancedOptions` (persistent TOML `[physics_lab]`), mirrored lock-free into `game::physics_lab::RuntimeParams` via `game::physics_lab::SyncFromEnhanced(const modsettings::EnhancedOptions&)`.
   - `x86_recomp.exe` emits direct hook calls at exact guest PCs without altering any existing hook functions in `src/game/hooks.cpp`.

---

## 2. Hook Topology (`config/game.toml` -> `src/game/physics_lab.cpp`)

| Hook Name | Guest VA | X86 Context / Register Contract | Runtime Action |
|---|---|---|---|
| `PhysicsBlastImpulseA` | `0x001E4034` | `sub_001E3FA0` pre-`sub_000DCC40`, `xmm0` = scalar impulse | `ctx.xmm[0].f[0] *= explosion_force` |
| `PhysicsBlastImpulseB` | `0x001E568E` | `sub_001E5410` pre-`sub_000DCC40`, `xmm0` = scalar impulse | `ctx.xmm[0].f[0] *= explosion_force` |
| `PhysicsBlastImpulseC` | `0x001E8EFC` | `sub_001E8D80` pre-`sub_000DCC40`, `xmm0` = scalar impulse | `ctx.xmm[0].f[0] *= explosion_force` |
| `PhysicsImpactImpulseA` | `0x001E5952` | `sub_001E5710` pre-`sub_000DCF80`, `xmm0` = bullet impulse | `ctx.xmm[0].f[0] *= impact_force` |
| `PhysicsImpactImpulseB` | `0x001E908D` | `sub_001E8F20` pre-`sub_000DCF80`, `xmm0` = bullet impulse | `ctx.xmm[0].f[0] *= impact_force` |
| `PhysicsGravityPost` | `0x001F11D5` | `sub_001F1080` post-`PhysicsTime`, reads `kPhysics -> solver -> world` | Sets `[world+0x364] = g_base_gravity_y * gravity_scale`; refreshes `[world+0x374]` |
| `PhysicsShardGravity` | `0x001693EA` | `sub_001690D0` pre-`ShardStep` (vel), `xmm0` = `[ebx+0xC0]` | Scales `ctx.xmm[0].f[0..2]` by `gravity_scale` |
| `PhysicsShardDebris` | `0x001693F6` | `sub_001690D0` pre-`ShardStep` (pos), `xmm0` = vel | Scales `ctx.xmm[0].f[0..2]` by `debris_intensity` |
| `PhysicsParticleDebris` | `0x0016DFFF` | Post-`ParticleFade`, `xmm3` = fade step | Scales `ctx.xmm[3].f[0]` by `1.0f / debris_intensity` (clamped `<= 0.90f * xmm2`) |
| `PhysicsRagdollDamping` | `0x00077DA1` | Post-`BodyDamping`, `xmm0`=lin retain, `[esp+0x0C]`=ang retain | `pow(retain, 1.0f / character_reaction)` clamped `[0.05f, 0.9995f]` |
| `PhysicsRagdollSpeedA` | `0x00077DFF` (`after=true`) | Post-`BodyTopSpeed` #1 & `mulss xmm1, xmm1` (`v_max^2`) | `ctx.xmm[1].f[0] *= (character_reaction * character_reaction)` |
| `PhysicsRagdollSpeedB` | `0x00077E74` (`after=true`) | Post-`BodyTopSpeed` #2 & `mulss xmm1, xmm1` (`v_max^2`) | `ctx.xmm[1].f[0] *= (character_reaction * character_reaction)` |
| `PhysicsRecoilKick` | `0x001EFE50` | `sub_001EEFD0` pre-`addss`, `eax` = char (`[eax+0xC4]==2`), `ebx` = weapon | Computes `(target * r - cur) * blend` into `xmm1.f[0]` & scales `xmm0.f[0] *= r` (`r = no_recoil ? 0 : weapon_recoil`) |

---

## 3. Parameter Ranges, Defaults & Preset Profiles

### Verified Controls & Numeric Bounds

| Setting Key (`[physics_lab]`) | Classic Default | Min | Max | Step | Display Unit | Apply Scope |
|---|---|---|---|---|---|---|
| `preset` | `"classic"` | — | — | — | Enum string | `Immediate` |
| `explosion_force` | `1.00f` | `0.25f` | `3.00f` | `0.05f` | `%` + `x` (`100% (1.00x)`) | `Immediate` |
| `impact_force` | `1.00f` | `0.25f` | `3.00f` | `0.05f` | `%` + `x` (`100% (1.00x)`) | `Immediate` |
| `gravity_scale` | `1.00f` | `0.25f` | `2.50f` | `0.05f` | `%` + `x` (`100% (1.00x)`) | `Immediate` |
| `debris_intensity` | `1.00f` | `0.25f` | `3.00f` | `0.05f` | `%` + `x` (`100% (1.00x)`) | `Immediate` |
| `character_reaction` | `1.00f` | `0.25f` | `2.50f` | `0.05f` | `%` + `x` (`100% (1.00x)`) | `Immediate` |
| `weapon_recoil` | `1.00f` | `0.00f` | `2.50f` | `0.05f` | `%` + `x` (`100% (1.00x)`) | `Immediate` |

### Four Profile Definitions

| Profile | `explosion_force` | `impact_force` | `gravity_scale` | `debris_intensity` | `character_reaction` | `weapon_recoil` | Behavior Summary |
|---|---|---|---|---|---|---|---|
| **Classic** (`classic`) | `1.00x` | `1.00x` | `1.00x` | `1.00x` | `1.00x` | `1.00x` | Preserves exact original 2006 Xbox BLACK physics |
| **Realistic** (`realistic`) | `1.25x` | `1.30x` | `1.15x` | `1.35x` | `1.20x` | `1.15x` | Snappier weighted gravity, firmer physical impacts, crisp recoil |
| **Cinematic** (`cinematic`) | `2.10x` | `1.85x` | `0.80x` | `2.40x` | `1.75x` | `1.40x` | Dramatic explosive launches, floatier hang-time, intense flying debris |
| **Custom** (`custom`) | User-defined | User-defined | User-defined | User-defined | User-defined | User-defined | Automatically activated when any slider changes manually |

---

## 4. Schema Persistence & Bilingual Tactical Console UI

- **TOML Section (`[physics_lab]`)**: Added as a dedicated root table in `black.toml`, validated on load via `SettingsRegistry`, clamped within `NumericBounds`, serialized atomically by `SaveConfig()`, and preserved cleanly on migration.
- **Tactical Operations Console (`src/common/menu.cpp`)**:
  - Adds `TacticalCategory::PhysicsLab` (`PHYSICS LAB` / `مختبر الفيزياء`) with stable unique ImGui IDs.
  - Top bar inside the Physics Lab tab renders 4 full-height `>= 54dp` Preset Profile cards (`Classic` / `كلاسيكي`, `Realistic` / `واقعي`, `Cinematic` / `سينمائي`, `Custom` / `مخصص`) plus a `Reset All (Classic)` button.
  - Each of the 6 verified parameters displays: localized title & technical term, current numerical + percentage value, original default badge (`Original: 100% (1.00x)` / `الأصل: 100% (1.00x)`), wide touchscreen slider (`>= 52-56dp` row/grab height), and individual per-control **Reset** (`↺`) button.
  - All Arabic labels use verified glyphs present in `NotoNaskhArabic-Bold.ttf` (`0` missing glyphs) with RTL mirroring and BiDi numeric isolation.

