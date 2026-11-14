# BLACK Enhanced Edition — Settings Schema Specification (`SETTINGS_SCHEMA.md`)

> **Persistence File**: `ExeDir() / "black.toml"` (e.g., `Android/data/com.racas.black/files/black.toml`), parsed and serialized via `toml++ v3.4.0` in [`native/main.cpp`](native/main.cpp) and [`src/common/game_info.cpp`](src/common/game_info.cpp).  
> **Backward Compatibility**: All existing flat top-level keys (`render_percent`, `fps_cap`, `vsync`, `anisotropic_filtering`, `fov`, `screen_filter`, `language`, etc.) from upstream `Black-Android` remain preserved at root level or cleanly migrated so existing user `black.toml` files never break or reset.

---

## 1. Schema Structure & Migration Strategy

1. **Legacy Root Keys (Preserved 1:1)**: Keep existing keys from `app::Settings` and `app::kGame.options` readable at root level (`render_percent`, `fps_cap`, `vsync`, `anisotropic_filtering`, `bloom`, `fog`, `hdr`, `anti_aliasing`, `draw_distance`, `dynamic_resolution`, `fov`, `viewmodel_fov`, `audio_output`, `language`, etc.).
2. **Modular Enhanced Sections**: New Enhanced Edition settings, presets, and package states are stored under namespaced TOML tables:
   - `[presets]`
   - `[graphics]`
   - `[combat]`
   - `[characters]`
   - `[audio]`
   - `[arabic]`
   - `[gameplay]`
   - `[interface]`
   - `[packages]` (plus per-package `mod.toml` manifests inside `Black/mods/<id>/`)

---

## 2. Complete Annotated TOML Schema

```toml
# ==============================================================================
# BLACK Enhanced Edition — Configuration File (black.toml)
# Schema Version: 2 (migrates cleanly from upstream defaults = 2)
# ==============================================================================
defaults = 2
schema_version = 2

# --- Existing Upstream Root Keys (Kept for 100% Upstream Compatibility) -------
display_mode = "fullscreen"            # "fullscreen" | "borderless" | "windowed"
width = 1920
height = 1080
render_percent = 67                    # 20..200 (step 5)              [Immediate]
fps_cap = 0                            # 0=unlimited, 1=refresh, 30..360 [Immediate]
vsync = false                          # bool                           [Immediate]
anisotropic_filtering = 0              # 0 (default), 2, 4, 8, 16       [Immediate]
anti_aliasing = 0                      # 0=Off, 1=FXAA, 2=SSAA 2x       [Immediate]
screen_filter = 0                      # 0..8 (None, CRT, Sharpen, etc.) [Immediate]
bloom = true                           # bool                           [Immediate]
fog = true                             # bool                           [Immediate]
hdr = false                            # bool (legacy; mapped to tone_mapping) [Immediate]
reload_blur = true                     # bool                           [Immediate]
draw_distance = 1                      # 1, 2, 4, 8                     [Immediate]
dynamic_resolution = false             # bool                           [Immediate]
dynamic_target_fps = 60                # 30, 45, 60, 90, 120            [Immediate]
dynamic_min_percent = 40               # 20..dynamic_max_percent        [Immediate]
dynamic_max_percent = 100              # dynamic_min_percent..200       [Immediate]
dynamic_hold = 5                       # 1..30 seconds                  [Immediate]
fmv_scale = 0                          # 0=Fill, 1=4:3                  [Immediate]
skip_intros = false                    # bool                           [Immediate]
fps_counter = false                    # bool                           [Immediate]
audio_output = 0                       # 0=Auto, 1=Mono, 2=Stereo, 3=5.1 [Immediate]
language = "int"                       # "int"|"fra"|"deu"|"ita"|"esp"|"ind"|"ara" [AppRestart]

# Controls & Cheats (excluded from visual/perf presets)
fov = 70                               # 45..140                        [Immediate]
viewmodel_fov = 70                     # 45..140                        [Immediate]
walk_speed = 50                        # 10..100 (%)                    [Immediate]
hold_aim = true                        # bool                           [Immediate]
invert_mouse = false                   # bool                           [Immediate]
mouse_sensitivity = 1.0                # 0.1..5.0                       [Immediate]
touch_sensitivity = 1.0                # 0.1..5.0                       [Immediate]
left_stick_deadzone = 0                # 0..50 (%)                      [Immediate]
right_stick_deadzone = 0               # 0..50 (%)                      [Immediate]
left_stick_sensitivity = 1.0           # 0.1..3.0                       [Immediate]
right_stick_sensitivity = 1.0          # 0.1..3.0                       [Immediate]
infinite_ammo = false
rapid_fire = 1
no_recoil = false
game_speed = 100
god_mode = false
one_hit_kill = false
infinite_grenades = false
unlock_levels = false
unlock_black_ops = false
esp = false
aimbot = false

# ==============================================================================
# [presets] — Preset Selection & Snapshot Recovery
# ==============================================================================
[presets]
active = "custom"                      # "original" | "enhanced" | "performance" | "custom"

[presets.last_custom]                  # Snapshot saved before applying a built-in preset
saved = false

# ==============================================================================
# [graphics] — Enhanced Host Vulkan & ShaderHook Settings
# ==============================================================================
[graphics]
tone_mapping = 0                       # 0=Original, 1=Reinhard HDR, 2=ACES Filmic [Immediate]
cas_sharpen_enabled = false            # bool (works independently of FXAA/filters) [Immediate]
cas_sharpen_strength = 35              # 0..100 (%)                     [Immediate]
bloom_intensity = 100                  # 0..200 (%) (0 acts as bloom off) [Immediate]
texture_lod_bias = 0.0                 # 0.0, -0.5, -1.0 (mip sharpness) [Immediate]
trilinear_filtering = false            # bool (force linear mipmaps on world tex) [Immediate]
soft_shadows = 0                       # 0=Off, 1=PCF 4-Tap, 2=PCF 9-Tap [ShaderRebuild]
ambient_occlusion = 0                  # 0=Off, 1=Half-Res [PLANNED - locked until Milestone 4]
hd_texture_packs = false               # bool              [PLANNED - locked until Phase 3]

# ==============================================================================
# [combat] — Combat Feedback & Blood / Gore Extensions
# ==============================================================================
[combat]
screen_damage_vignette = false         # bool (Host blood vignette on HP drop) [Immediate]
damage_vignette_intensity = 75         # 10..100 (%)                    [Immediate]
low_health_desaturation = false        # bool (Host saturation drop < 30% HP) [Immediate]
explosion_glow_boost = 100             # 50..200 (%) (Host glow highlight curve) [Immediate]
particle_fade_scale = 100              # 100..400 (%) (via ParticleFade hook) [Immediate]
blood_particle_multiplier = 1          # 1..3x [PLANNED - locked pending XBE blood hook]
persistent_blood_decals = false        # bool  [PLANNED - locked pending decal ring RE]
max_blood_decals = 128                 # 64..1024 [PLANNED]
corpse_persistence_mode = 0            # 0=Default, 1=2x, 2=4x [PLANNED - high pool risk]
diverse_bullet_impacts = false         # bool  [PLANNED - locked pending impact RE]

# ==============================================================================
# [characters] — Character Rendering & Ragdoll Extensions
# ==============================================================================
[characters]
character_lod_scale = 1                # 1, 2, 4 (linked to CharacterDetail/Reach) [Immediate]
ragdoll_60fps_physics = true           # bool (PhysicsTime @ 0x1F11CD / BodyRestTick) [Immediate]
custom_character_models = false        # bool  [PLANNED / REQUIRES ASSET PACK]

# ==============================================================================
# [audio] — AAudio Mixer & Remastered Sound Extensions
# ==============================================================================
[audio]
combat_dynamic_boost = false           # bool (Host punchy limiter/EQ curve) [Immediate]
master_gain_percent = 100              # 10..150 (%)                    [Immediate]
mod_audio_pack_enabled = false         # bool (uses D:\sound\ VFS package) [AppRestart]

# ==============================================================================
# [arabic] — Arabic Localization & RTL Engine
# ==============================================================================
[arabic]
arabic_ui_enabled = false              # bool (ImGui HarfBuzz/FriBidi UI + Noto Font) [AppRestart]
arabic_rtl_mirror = true               # bool (Mirror ImGui rows RTL when arabic_ui on) [Immediate]
arabic_subtitles_overlay = false       # bool (Native RTL subtitle/objective HUD) [AppRestart]
arabic_flash_menus = false             # bool [PLANNED - Flash glyph atlas splicing]
arabic_voice_pack = false              # bool [PLANNED - Requires external VO pack]

# ==============================================================================
# [gameplay] — Camera, Aiming & Cheats Organization
# ==============================================================================
[gameplay]
enhanced_recoil_feedback = false       # bool [PLANNED - pending weapon table hook]
disable_aim_assist = false             # bool (mapped to existing AimAssist hook if exposed) [Immediate]

# ==============================================================================
# [interface] — Touch, HUD, Telemetry & Video Options
# ==============================================================================
[interface]
settings_theme_accent = "brass"        # "brass" | "crimson" | "olive"  [Immediate]
advanced_perf_hud = false              # bool (FPS + Frame Time ms + DynRes %) [Immediate]
show_restart_banners = true            # bool                           [Immediate]
hide_hud_on_screenshots = false        # bool                           [Immediate]

# ==============================================================================
# [packages] — Modular Asset Packages (Black/mods/<id>/)
# ==============================================================================
[packages]
overlay_enabled = true                 # Master toggle for VFS mod overlay [AppRestart]
enabled = []                           # Ordered list of active package IDs (priority order)
                                       # e.g. ["hd_textures_v1", "arabic_subtitles_pack"]
```

---

## 3. Field Reference & Validation Constraints

| Section | Key | Type | Valid Range / Values | Default (Original) | Default (Enhanced) | Apply Scope | Source Handler |
|---|---|---|---|---|---|---|---|
| `[graphics]` | `tone_mapping` | `uint32` | `0..2` (`Off`, `Reinhard`, `ACES`) | `0` | `2` | Immediate | `native/vulkan.cpp` |
| `[graphics]` | `cas_sharpen_enabled` | `bool` | `false` / `true` | `false` | `true` | Immediate | `native/vulkan.cpp` |
| `[graphics]` | `cas_sharpen_strength` | `uint32` | `0..100` (step `5`) | `0` | `35` | Immediate | `native/vulkan.cpp` |
| `[graphics]` | `bloom_intensity` | `uint32` | `0..200` (step `10`) | `100` | `110` | Immediate | `native/vulkan.cpp` |
| `[graphics]` | `texture_lod_bias` | `float` | `0.0`, `-0.5`, `-1.0` | `0.0` | `-0.5` | Immediate | `StageSampler()` |
| `[graphics]` | `trilinear_filtering` | `bool` | `false` / `true` | `false` | `true` | Immediate | `StageSampler()` |
| `[graphics]` | `soft_shadows` | `uint32` | `0..2` (`Off`, `4-Tap`, `9-Tap`) | `0` | `1` | ShaderRebuild | `ShaderHooks::Shadow()` |
| `[combat]` | `screen_damage_vignette` | `bool` | `false` / `true` | `false` | `true` | Immediate | `VulkanBackend::DrawFrame` |
| `[combat]` | `damage_vignette_intensity` | `uint32` | `10..100` (step `5`) | `75` | `75` | Immediate | `VulkanBackend::DrawFrame` |
| `[combat]` | `low_health_desaturation` | `bool` | `false` / `true` | `false` | `true` | Immediate | `kFilterHlsl` |
| `[combat]` | `particle_fade_scale` | `uint32` | `100..400` (step `25`) | `100` | `200` | Immediate | `src/game/hooks.cpp` |
| `[characters]` | `ragdoll_60fps_physics` | `bool` | `false` / `true` | `true` | `true` | Immediate | `PhysicsTime` hook |
| `[audio]` | `combat_dynamic_boost` | `bool` | `false` / `true` | `false` | `true` | Immediate | `src/sound/` |
| `[audio]` | `master_gain_percent` | `uint32` | `10..150` (step `5`) | `100` | `100` | Immediate | `native/audio.cpp` |
| `[arabic]` | `arabic_ui_enabled` | `bool` | `false` / `true` | `false` | Preserved | AppRestart | `translations.cpp`, `ui.cpp` |
| `[arabic]` | `arabic_subtitles_overlay` | `bool` | `false` / `true` | `false` | Preserved | AppRestart | `prompts.cpp`, `vulkan.cpp` |
| `[interface]` | `advanced_perf_hud` | `bool` | `false` / `true` | `false` | `false` | Immediate | `native/vulkan.cpp` / `menu.cpp` |
| `[packages]` | `overlay_enabled` | `bool` | `false` / `true` | `false` | `true` | AppRestart | `files.cpp::Resolve()` |

---

## 4. Per-Package Manifest Schema (`Black/mods/<id>/mod.toml`)

Every modular enhancement package placed inside `Black/mods/<id>/` is self-describing and validated before exposure in the UI:

```toml
[package]
id = "arabic_localization_hud"
name = "Arabic Subtitles & Mission Pack"
version = "1.0.0"
author = "Enhanced Edition Team"
category = "arabic"                    # graphics | combat | characters | audio | arabic | gameplay | interface
priority = 100                         # Higher wins when two packages override the same path
description = "Adds Arabic mission briefings and HUD dialogue strings without altering the original ISO."

[compatibility]
title_id = "0x45410083"                # Must match Black Xbox Title ID
min_port_version = 1
regions = ["usa", "europe"]            # Supported disc bases

[layers]
has_vfs_overrides = true               # Files under ./vfs/ override D:\ paths
has_host_resources = true              # Fonts / strings / textures under ./host/
```

### Package Validation Rules
- If `title_id != "0x45410083"` or any file inside `vfs/` attempts to replace `default.xbe`, the package is rejected with status **`INCOMPATIBLE`** (`FeatureStatus::Unavailable`) and never mounted.
- If `min_port_version` exceeds the running APK build version, the toggle locks with **`UPDATE REQUIRED`**.
- Disabling a package removes it from `[packages].enabled` and flags `AppRestart` if `has_vfs_overrides == true`.

