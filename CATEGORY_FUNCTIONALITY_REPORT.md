# BLACK Enhanced Edition — Category Functionality & Controls Report

**Repository:** `E:\ai\black`  
**Branch:** `black-enhanced-edition`  
**Target Package:** `com.racas.black.enhanced`  
**Milestone:** Stage 4.1 — Tactical Console UX Redesign & Navigation Repair  
**Target Build:** Build 27 (`versionCode: 27`, `versionName: "1.2.1-enhanced-arabic-ui"`)  
**Status:** Implemented & Verified  

---

## 1. Executive Summary

This report documents the functional features, underlying technical hooks, application scopes, and current status of all 8 categories within the redesigned Tactical Operations Console.

In accordance with the project's **Anti-Mockup Rule**:
- Every exposed toggle or slider controls real guest or host variables.
- Features that depend on unintegrated guest hooks or external asset packages are strictly marked with the `[PLANNED]` badge and disabled, complete with explanatory technical tooltips.

---

## 2. Category Breakdown & Functional Architecture

### 2.1 Category 0 — Graphics (`الرسومات`)

| Setting / Option | Hook / Variable | Scope | Status | Notes |
|---|---|---|---|---|
| **Render Resolution** | `app::Get().render_percent` | ShaderRebuild | **ACTIVE** | 50% to 200% resolution scaling. |
| **VSync** | `game::Opt().vsync` | Immediate | **ACTIVE** | Toggles Vulkan presentation mode (`FIFO` vs `IMMEDIATE`/`MAILBOX`). |
| **Max Frame Rate** | `app::Get().fps_cap` | Immediate | **ACTIVE** | 30, 60, 75, 90, 120, 144, 165, 240, 360, Refresh, Uncapped. |
| **Anisotropic Filtering** | `game::Opt().anisotropic` | Immediate | **ACTIVE** | Default, 2x, 4x, 8x, 16x sampling. |
| **Anti-Aliasing** | `game::Opt().anti_aliasing` | ShaderRebuild | **ACTIVE** | OFF, FXAA, SSAA 2X. |
| **Dynamic Resolution** | `game::Opt().dynamic_resolution` | Immediate | **ACTIVE** | Dynamic DRS scaling. When active, exposes Target FPS, Min/Max Res, Hold frames. |
| **Volumetric Fog** | `game::Opt().fog` | Immediate | **ACTIVE** | Real-time distance fog density control. |
| **Bloom Glow** | `game::Opt().bloom` | Immediate | **ACTIVE** | Full-screen Gaussian bloom pipeline. |
| **Bloom Intensity** | `enh.bloom_intensity` | Immediate | **ACTIVE** | 50% to 200% bloom brightness multiplier. |
| **Reload Blur** | `game::Opt().reload_blur` | Immediate | **ACTIVE** | Radial weapon reload focus blur effect. |
| **Draw Distance** | `game::Opt().draw_distance` | Immediate | **ACTIVE** | 1x (default), 2x, 4x, 8x geometry culling multiplier. |
| **HDR Pipeline** | `game::Opt().hdr` | ShaderRebuild | **ACTIVE** | High dynamic range color buffer output. |
| **Texture LOD Bias** | `enh.texture_lod_bias` | Immediate | **ACTIVE** | 0.0 (Standard), -0.5 (Sharp), -1.0 (Ultra Sharp). |
| **Trilinear Filtering** | `enh.trilinear_filtering` | Immediate | **ACTIVE** | Forced trilinear mipmap interpolation. |
| **Soft Shadow Filtering** | PCF shadow map hook | AppRestart | `[PLANNED]` | 4-Tap / 9-Tap PCF filtering shader hook. |
| **Ambient Occlusion (SSAO)** | Half-Res SSAO buffer | AppRestart | `[PLANNED]` | Screen-space ambient occlusion pipeline. |
| **HD Textures Overlay** | Asset loader hook | AppRestart | `[PLANNED]` | Direct replacement from `/Black/mods/textures/`. |

---

### 2.2 Category 1 — Combat & Blood Effects (`القتال والتأثيرات`)

| Setting / Option | Hook / Variable | Scope | Status | Notes |
|---|---|---|---|---|
| **Damage Vignette** | `enh.screen_damage_vignette` | Immediate | **ACTIVE** | Tactical red screen vignette when health drops. |
| **Vignette Intensity** | `enh.damage_vignette_intensity` | Immediate | **ACTIVE** | 50% to 150% visual opacity curve. |
| **Low Health Desaturation** | `enh.low_health_desaturation` | Immediate | **ACTIVE** | Grayscale color grading when critical health reached. |
| **Explosion Glow Boost** | `enh.explosion_glow_boost` | Immediate | **ACTIVE** | 50% to 200% dynamic light intensity on explosions. |
| **Particle Persistence** | `enh.particle_fade_scale` | Immediate | **ACTIVE** | 50% to 300% particle lifetime scaling. |
| **Blood Particle Multiplier**| Guest emitter hook | SceneReload | `[PLANNED]` | 2x-3x guest particle emitter hook. |
| **Persistent Blood Decals** | Decal pool hook | SceneReload | `[PLANNED]` | Prevents blood splatter fade-out. |
| **Max Decals Buffer** | Decal buffer expansion | SceneReload | `[PLANNED]` | 128 / 256 / 512 dynamic decal pool allocation. |
| **Corpse Persistence** | Cleanup timer hook | SceneReload | `[PLANNED]` | Extends corpse despawn timer. |
| **Diverse Bullet Impacts** | Surface material hook | SceneReload | `[PLANNED]` | Material-specific impact decal variety. |

---

### 2.3 Category 2 — Characters (`الشخصيات`)

| Setting / Option | Hook / Variable | Scope | Status | Notes |
|---|---|---|---|---|
| **Ragdoll 60 FPS Physics** | `enh.ragdoll_60fps_physics` | Immediate | **ACTIVE** | Decouples ragdoll solver rate from 30 FPS guest tick. |
| **Character LOD Scale** | `enh.character_lod_scale` | Immediate | **ACTIVE** | 1x (Default), 2x (High), 4x (Ultra) mesh distance scale. |
| **Custom Character Models** | Asset package hook | AppRestart | `[PLANNED]` | High-polygon mesh package support in `/Black/mods/`. |

---

### 2.4 Category 3 — Audio (`الصوتيات`)

| Setting / Option | Hook / Variable | Scope | Status | Notes |
|---|---|---|---|---|
| **Master Audio Gain** | `enh.master_gain_percent` | Immediate | **ACTIVE** | 50% to 150% audio gain stepper with real-time scaling. |
| **Audio Output Mode** | `game::Opt().audio_output` | Immediate | **ACTIVE** | Cycles AUTOMATIC, MONO, 2.0 STEREO, 5.1 SURROUND via `audio::SetOutputMode()`. |
| **Combat Dynamic Boost** | `enh.combat_dynamic_boost` | Immediate | **ACTIVE** | Dynamically boosts gunfire and explosions during firefights. |
| **Audio Engine Status** | `audio::Active()` | Dynamic Display | **ACTIVE** | Verified active AudioTrack / OpenSLES streaming on Android. |
| **HD Audio Replacement Pack**| ADPCM bank hook | AppRestart | `[PLANNED]` | High-sample-rate ADPCM sound package. |
| **Spatial Audio Enhancement**| DSP surround filter | AppRestart | `[PLANNED]` | Multi-channel 3D positional audio DSP. |

---

### 2.5 Category 4 — Localization (`التعريب واللغات`)

| Setting / Option | Hook / Variable | Scope | Status | Notes |
|---|---|---|---|---|
| **Arabic UI Translation** | `enh.arabic_ui_enabled` | Immediate | **ACTIVE** | Live toggle between Arabic and English UI. |
| **RTL Text Mirroring** | `enh.arabic_rtl_mirror` | Immediate | **ACTIVE** | Live toggle between mirrored RTL and LTR layouts. |
| **Subtitles Overlay** | `enh.arabic_subtitles_overlay` | Immediate | **ACTIVE** | Enables Arabic subtitle rendering pipeline. |
| **System Language** | `game::Opt().language` | AppRestart | **ACTIVE** | ENGLISH, FRENCH, GERMAN, ITALIAN, SPANISH, INDONESIAN, ARABIC. |
| **Flash Native Menus** | SWF bytecode patch | AppRestart | `[PLANNED]` | SWF glyph table patch for stock Xbox menus. |
| **Arabic Voice Dubbing** | Dialogue audio pack | AppRestart | `[PLANNED]` | Arabic dialogue audio soundbank package. |

---

### 2.6 Category 5 — Gameplay (`أسلوب اللعب`)

| Setting / Option | Hook / Variable | Scope | Status | Notes |
|---|---|---|---|---|
| **Field of View (FOV)** | `game::Opt().fov` | Immediate | **ACTIVE** | 60° to 110° camera FOV. |
| **Viewmodel FOV** | `game::Opt().viewmodel_fov` | Immediate | **ACTIVE** | First-person gun model FOV adjustment. |
| **Walk Speed** | `game::Opt().walk_speed` | Immediate | **ACTIVE** | 50% to 150% operator movement speed. |
| **Hold to Aim** | `game::Opt().hold_aim` | Immediate | **ACTIVE** | Toggle vs hold for weapon aiming. |
| **Invert Look** | `game::Opt().invert_mouse` | Immediate | **ACTIVE** | Invert vertical camera axis. |
| **Game Speed** | `game::Opt().game_speed` | Immediate | **ACTIVE** | 0.25x (Slow-Mo) to 2.0x (Hyper) simulation speed. |
| **Skip Intros** | `game::Opt().skip_intros` | Immediate | **ACTIVE** | Skips EA/Criterion opening videos directly to title screen. |
| **Disable Aim Assist** | `enh.disable_aim_assist` | Immediate | **ACTIVE** | Disables sticky aim assist for authentic hardcore gunplay. |
| **Enhanced Recoil Feedback** | Controller haptics | Immediate | `[PLANNED]` | Haptic feedback curve hook. |

---

### 2.7 Category 6 — Interface & Touch Controls (`الواجهة والتحكم`)

| Setting / Option | Hook / Variable | Scope | Status | Notes |
|---|---|---|---|---|
| **Console Accent Theme** | `enh.theme_accent` | Immediate | **ACTIVE** | Tactical Brass, Special Ops Crimson, Military Olive. |
| **Advanced Perf HUD** | `enh.advanced_perf_hud` | Immediate | **ACTIVE** | Real-time FPS, frame time, GPU/CPU timing display. |
| **Show Restart Banners** | `enh.show_restart_banners` | Immediate | **ACTIVE** | Prominent warning banner when pending settings require restart. |
| **Hide HUD on Screenshots** | `enh.hide_hud_on_screenshots`| Immediate | **ACTIVE** | Automatically hides Dear ImGui / HUD during screenshot capture. |
| **Touch Sensitivity** | `game::Opt().touch_sensitivity`| Immediate | **ACTIVE** | Stepper 0.1 to 5.0 touch look sensitivity. |
| **Controls Layout** | `touch::g_preset` | Immediate | **ACTIVE** | Default (2 Finger), 3 Finger (Claw), 4 Finger. |
| **Controls Opacity** | `touch::g_opacity` | Immediate | **ACTIVE** | 10% to 100% on-screen touch control transparency. |
| **Controls Size** | `touch::g_scale` | Immediate | **ACTIVE** | 0.5x to 1.5x on-screen button scale. |
| **Button Style / Skin** | `touch::g_skin` | Immediate | **ACTIVE** | Xbox, PlayStation, Minimal, Tactical. |
| **Analog Stick Mode** | `touch::g_fixed_stick` | Immediate | **ACTIVE** | Floating thumb stick vs Fixed position. |
| **Edit Controls Position** | `touch::g_editing` | Immediate | **ACTIVE** | Opens interactive drag-and-drop on-screen button editor. |
| **Gyroscope Controls** | `game::Opt().gyro` | Immediate | **ACTIVE** | Off, Yaw, Roll, Invert X/Y, Sensitivity. |
| **Controller Deadzones** | `game::Opt().left_deadzone` | Immediate | **ACTIVE** | Left and right stick deadzones and sensitivities. |

---

### 2.8 Category 7 — Cheats & Weapons (`الأسلحة والشفرات`)

| Setting / Option | Hook / Variable | Scope | Status | Notes |
|---|---|---|---|---|
| **Infinite Ammo** | `game::Opt().infinite_ammo` | Immediate | **ACTIVE** | Eliminates magazine depletion. |
| **Infinite Grenades** | `game::Opt().infinite_grenades` | Immediate | **ACTIVE** | Grenade inventory never decrements. |
| **Rapid Fire** | `game::Opt().rapid_fire` | Immediate | **ACTIVE** | Removes weapon cycle delay. |
| **No Recoil** | `game::Opt().no_recoil` | Immediate | **ACTIVE** | Perfect pinpoint weapon stability. |
| **God Mode** | `game::Opt().god_mode` | Immediate | **ACTIVE** | Complete invulnerability to enemy damage. |
| **One-Hit Kill** | `game::Opt().one_hit_kill` | Immediate | **ACTIVE** | Any bullet neutralizes targets instantly. |
| **ESP Tactical Lines** | `game::Opt().esp` | Immediate | **ACTIVE** | Direct line-of-sight visual indicators to all enemies. |
| **Aimbot (While Aiming In)** | `game::Opt().aimbot` | Immediate | **ACTIVE** | Magnetic target snapping while scoped or aiming. |
| **Unlock All Missions** | `game::Opt().unlock_levels` | Immediate | **ACTIVE** | Unlocks all 8 campaign missions. |
| **Unlock Black Ops Difficulty**| `game::Opt().unlock_blackops` | Immediate | **ACTIVE** | Unlocks hardest operational difficulty and Silver Weapons. |
| **Weapon Arsenal Picker** | `weapons::Give(type)` | Immediate | **ACTIVE** | 17 player weapons directly grantable in missions. |
| **Fill Ammunition** | `weapons::FillAmmo()` | Immediate | **ACTIVE** | Instantly replenishes all ammunition. |
| **Fill Medkits** | `weapons::FillMedkits()` | Immediate | **ACTIVE** | Instantly replenishes operator medkits. |

---

## 3. Verification & Compliance Verdict

All 8 categories:
1. Provide responsive, immediate feedback on modern touch screens.
2. Meet or exceed 52dp touch target guidelines.
3. Decouple category navigation from menu close logic.
4. Support full Arabic translation and RTL mirroring.
5. Accurately report real system and guest functionality while keeping roadmap items appropriately labeled as `[PLANNED]`.

