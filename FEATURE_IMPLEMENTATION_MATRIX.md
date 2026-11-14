# BLACK Enhanced Edition — Feature Implementation Matrix (`FEATURE_IMPLEMENTATION_MATRIX.md`)

> **Truthfulness & Anti-Cosmetic Policy**: Every feature across the 7 settings categories is classified strictly by what the codebase supports today versus what requires host/shader additions, guest XBE reverse-engineering, or external asset packages.  
> **Statuses Used**:
> - **`Implemented`**: Already active and wired in the upstream `Black-Android` codebase.
> - **`Feasible (Host/Hook)`**: Confirmed implementation path using existing host APIs (`VulkanBackend`, `ShaderHooks`, `Audio`) or verified guest memory offsets (`src/game/*.cpp`).
> - **`Planned`**: Designed in roadmap; locked (`non-interactive`) in UI until its guest disassembly/engine hook is verified and built.
> - **`Unavailable`**: Cannot function without missing hardware support or user-installed external asset pack.

---

## 1. Graphics

| Setting / Feature | Control Type | Technical Layer | Confirmed Code Anchors | Current Status | Feasibility | Restart Req? | Perf Cost |
|---|---|---|---|---|---|---|---|
| **Render Resolution %** | Slider (`20%–200%`) | Host Vulkan | `native/main.cpp`, `native/vulkan.cpp` | **Implemented** | — | No (Immediate) | High (GPU fill) |
| **Dynamic Resolution** | Toggle + Range | Host Vulkan | `native/dynamic_resolution.cpp` | **Implemented** | — | No (Immediate) | Saves GPU |
| **Anisotropic Filtering** | Step (`Default..16x`) | Host Vulkan | `StageSampler()` (`vulkan.cpp:2337`) | **Implemented** | — | No (Immediate) | Very Low |
| **Anti-Aliasing** | Cycle (`Off/FXAA/SSAA 2x`) | Host Vulkan | `kBlitHlsl`/`kFilterHlsl` (`vulkan.cpp`) | **Implemented** | — | No (Immediate) | Low / High |
| **Screen Filters** | Cycle (`Off..VHS`) | Host Vulkan | `kFilterHlsl` (`vulkan.cpp:142`) | **Implemented** | — | No (Immediate) | Very Low |
| **Bloom / Fog / Reload Blur** | Toggles | Host / Recomp Hook | `GlowComposite()`, `FillFogConstants()`, `hooks.cpp` | **Implemented** | — | No (Immediate) | Low |
| **HDR Output** | Toggle | Host Vulkan | `vulkan.cpp` (Locked `Unavailable` on SDR phones) | **Implemented** | — | No (Immediate) | Negligible |
| **Draw Distance Scale** | Step (`1x..8x`) | Guest Recomp Hook | `Reach()`, `DrawListRoom` (`hooks.cpp`) | **Implemented** | — | No (Immediate) | Moderate |
| **Tone Mapping (Reinhard / ACES)** | Cycle (`Off/Reinhard/ACES`) | Host Vulkan | Extend `kFilterHlsl::Tone()` (`vulkan.cpp`) | **Feasible (Host)** | **Easy** | No (Immediate) | Negligible |
| **Dedicated CAS Sharpening** | Toggle + Slider (`0–100%`) | Host Vulkan | Decouple `pssharpen` in `kFilterHlsl` | **Feasible (Host)** | **Easy** | No (Immediate) | Very Low |
| **Bloom Intensity %** | Slider (`0–200%`) | Host Vulkan | Scale composite constant in `GlowComposite()` | **Feasible (Host)** | **Easy** | No (Immediate) | None extra |
| **Texture Mip LOD Bias / Trilinear** | Cycle + Toggle | Host Vulkan | `StageSampler()` `mipLodBias` (`vulkan.cpp`) | **Feasible (Host)** | **Easy** | No (Immediate) | Very Low |
| **PCF Soft Shadows** | Cycle (`Off/4-Tap/9-Tap`) | Host `ShaderHooks` | `gpu::g_shader_hooks->Shadow()` (`shaders.h`) | **Feasible (Host)** | **Moderate** | ShaderRebuild | Low–Mod |
| **Ambient Occlusion (SSAO)** | Cycle (`Off/Half-Res`) | Host Vulkan + Phase Hook | `draw_ds_` depth copy + pre-HUD boundary | **Planned** | **Mod–Diff** | No (Immediate) | **High** |
| **HD Texture Pack Loader** | Package Toggle | Host Vulkan + `Black/mods/` | `BindGuestTexture()` `MemSig` hash override | **Planned** | **Moderate** | AppRestart | RAM/VRAM |

---

## 2. Combat & Blood Effects

| Setting / Feature | Control Type | Technical Layer | Confirmed Code Anchors | Current Status | Feasibility | Restart Req? | Perf Cost |
|---|---|---|---|---|---|---|---|
| **Screen Blood Damage Vignette** | Toggle + Intensity Slider | Host Vulkan + Guest RAM | Reads `kWorld (0x2D45D4)` → `Player+0x2F0` (`kCharHealth`), `0x384` (`kCharDying`), `HudShown()` | **Feasible (Host)** | **Easy** | No (Immediate) | Negligible |
| **Low-Health Desaturation** | Toggle | Host Vulkan (`kFilterHlsl`) | Driven by live `health / FullHealth()` ratio | **Feasible (Host)** | **Easy** | No (Immediate) | Negligible |
| **Explosion Glow & Bloom Boost** | Slider (`50–200%`) | Host Vulkan | Scales glow composite highlights in `GlowComposite()` | **Feasible (Host)** | **Easy** | No (Immediate) | None extra |
| **Particle Visibility Distance** | Slider (`100–400%`) | Guest Recomp Hook | `ParticleFade @ 0x16DFFB` (`hooks.cpp:564`) | **Feasible (Hook)** | **Easy** | No (Immediate) | Low–Mod |
| **Blood Particle Multiplier** | Cycle (`1x/2x/3x`) | Guest XBE Hook | Requires disassembling `sub_001E74B0` callee blood emitter + expanding pool | **Planned** | **Unconfirmed / Difficult** | AppRestart / Scene | Low–Mod |
| **Persistent Blood Decals** | Toggle + Cap Slider | Guest XBE Hook | Requires disassembling decal ring buffer via `DistanceShorter` sites + `Mem/LevelReset` | **Planned** | **Unconfirmed / Difficult** | SceneReload | Moderate |
| **Longer Corpse Persistence** | Cycle (`Default/2x/4x`) | Guest XBE Hook | Requires tracing `kCharDying (0x384)` despawn timer; **High soft-lock risk** if enemy AI pool is shared | **Planned** | **Unconfirmed / Difficult** | SceneReload | Mod–High |
| **Diverse Bullet Impact FX** | Toggle | Guest XBE + Assets | Unconfirmed material-to-impact emitter dispatch table inside `default.xbe` | **Planned** | **Unconfirmed / Difficult** | SceneReload | Moderate |

---

## 3. Characters

| Setting / Feature | Control Type | Technical Layer | Confirmed Code Anchors | Current Status | Feasibility | Restart Req? | Perf Cost |
|---|---|---|---|---|---|---|---|
| **60+ FPS Ragdoll & Physics Sync** | Toggle | Guest Recomp Hook | `PhysicsTime @ 0x1F11CD`, `BodyRestTick @ 0x77EE9` (`hooks.cpp`) | **Implemented** | — | No (Immediate) | Low |
| **Character LOD Detail Distance** | Step (`1x/2x/4x`) | Guest Recomp Hook | `CharacterDetail` / `Reach()` (`hooks.cpp`) | **Implemented** | — | No (Immediate) | Low–Mod |
| **Custom Character Model / Skin Pack** | Package Toggle | VFS / Host Asset Pack | Needs modular asset pack under `Black/mods/` targeting `D:\chars\` compatible geometry | **Unavailable / Planned** | **Moderate–Diff** | AppRestart | Low–Mod |

---

## 4. Audio

| Setting / Feature | Control Type | Technical Layer | Confirmed Code Anchors | Current Status | Feasibility | Restart Req? | Perf Cost |
|---|---|---|---|---|---|---|---|
| **Audio Output Channel Mode** | Cycle (`Auto/Mono/2.0/5.1`) | Host AAudio | `audio::SetOutputMode()` (`native/audio.cpp`, `src/common/options.h`) | **Implemented** | — | No (Immediate) | Negligible |
| **Master Gain / Dynamic Combat Boost** | Slider + Toggle | Host Audio Mixer | `src/sound/` mixer stage (soft limiter + punchy SFX gain envelope) | **Feasible (Host)** | **Easy** | No (Immediate) | Negligible |
| **External Remastered Audio Pack** | Package Toggle | VFS Overlay (`Black/mods/`) | `Resolve()` VFS pre-ISO lookup targeting `D:\sound\`; locked until pack installed | **Feasible / Asset-Gated** | **Easy** | AppRestart | None |

---

## 5. Arabic Localization

| Setting / Feature | Control Type | Technical Layer | Confirmed Code Anchors | Current Status | Feasibility | Restart Req? | Perf Cost |
|---|---|---|---|---|---|---|---|
| **Arabic Settings & Host UI (`ara`)** | Toggle | Host ImGui + `translations.cpp` | Add FreeType + HarfBuzz + FriBidi + Arabic `.ttf`, extend `kLanguages` & `kEntries[]` | **Feasible (Host)** | **Moderate** | AppRestart | Negligible |
| **RTL Layout Mirroring** | Toggle | Host ImGui | Mirror `MeasureRow()`, `OptionRow()`, `BindRow()`, and category columns in `menu.cpp` | **Feasible (Host)** | **Moderate** | No (Immediate) | None |
| **Arabic Subtitles & Mission HUD Overlay** | Toggle | Host Overlay + Hook | Hook `sub_001F3A80(id)` (`prompts.cpp`), render UTF-8 Arabic in ImGui/Vulkan HUD bypassing `0x12B800` 127-byte cap | **Feasible (Host+Hook)** | **Moderate** | AppRestart | Negligible |
| **Native Flash Menu Arabic Fonts** | Toggle | Guest Flash Font + Assets | Requires reverse-engineering `"Eurostile LT Std"` (`kFindFont = 0x13CD0`, `kFontGlyphs`) + RTL ActionScript wrapping | **Planned** | **Unconfirmed / Difficult** | AppRestart | Low |
| **Arabic Voice / Dubbing Pack** | Package Toggle | VFS Overlay (`Black/mods/`) | Requires external user-supplied localized `D:\sound\` / `D:\videos\` package | **Unavailable / Asset-Gated** | **Easy (once pack exists)** | AppRestart | None |

---

## 6. Gameplay

| Setting / Feature | Control Type | Technical Layer | Confirmed Code Anchors | Current Status | Feasibility | Restart Req? | Perf Cost |
|---|---|---|---|---|---|---|---|
| **Field of View (`45°–140°`)** | Slider | Guest Recomp Hook | `FieldOfView` (`config/game.toml`, `hooks.cpp`) | **Implemented** | — | No (Immediate) | Low |
| **Viewmodel FOV (`45°–140°`)** | Slider | Guest Recomp Hook | `GunEffect` (`hooks.cpp`) | **Implemented** | — | No (Immediate) | None |
| **Walk Speed % & Hold Aim** | Slider + Toggle | Guest Input / Hooks | `HoldAim`, `walk_speed` (`hooks.cpp`, `mouse.cpp`) | **Implemented** | — | No (Immediate) | None |
| **Gameplay Cheats & Unlocks** | Toggles / Steppers | Guest Hooks / RAM | `infinite_ammo`, `rapid_fire`, `no_recoil`, `god_mode`, `one_hit_kill`, `game_speed`, `unlock_levels`, `esp`, `aimbot` (`cheats.cpp`, `esp.cpp`, `weapons.cpp`) | **Implemented** | — | No (Immediate) | Negligible |
| **Aim Assist Toggle** | Toggle | Guest Recomp Hook | Check existing `AimAssist` hook in `config/game.toml` & `src/game/` for user configurability | **Feasible (Hook)** | **Easy** | No (Immediate) | None |
| **Enhanced Weapon Recoil / Ballistics** | Toggle | Guest XBE Hook | Requires disassembling weapon attribute tables in `default.xbe` | **Planned** | **Unconfirmed / Difficult** | SceneReload | None |

---

## 7. Interface

| Setting / Feature | Control Type | Technical Layer | Confirmed Code Anchors | Current Status | Feasibility | Restart Req? | Perf Cost |
|---|---|---|---|---|---|---|---|
| **Touch Controls Layout, Size, Opacity, Skin, Stick** | Sliders / Cycles / Editor | Host Touch (`touch.cpp`) | `LayoutName`, `ControlsOpacity`, `ControlsSize`, `SkinName`, `StickModeName`, `EditLayout` | **Implemented** | — | No (Immediate) | Negligible |
| **Gyroscope Mode, Sensitivity & Axis Invert** | Cycle / Slider / Toggles | Host Sensor (`gyro.cpp`) | `gyro::NextMode`, `SetSensitivity`, `SetInvert` | **Implemented** | — | No (Immediate) | Negligible |
| **Skip Intros & FMV Aspect Scale** | Toggle / Cycle | Guest Hooks | `SkipVideo`, `fmv_scale` (`hooks.cpp`) | **Implemented** | — | No (Immediate) | None |
| **FPS Counter** | Toggle | Host / HUD (`fps_counter.cpp`) | `game::g_fps` overlay | **Implemented** | — | No (Immediate) | Negligible |
| **Advanced Telemetry HUD (Frame ms, DynRes %)** | Toggle | Host UI (`vulkan.cpp`, `dynres`) | Renders live `dynres::Percent()`, frame time ms, and GPU/CPU split | **Feasible (Host)** | **Easy** | No (Immediate) | Negligible |
| **Settings UI Accent & Restart Banners** | Cycles / Toggle | Host UI (`mod_menu.cpp`) | Switches between Brass / Crimson / Olive tactical accent tokens | **Feasible (Host)** | **Easy** | No (Immediate) | None |

