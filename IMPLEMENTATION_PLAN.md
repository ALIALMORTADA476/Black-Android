# BLACK Enhanced Edition — Modular Modification Settings Implementation Plan (`IMPLEMENTATION_PLAN.md`)

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a modular in-game modification and settings management system for BLACK Android with 7 categorized domains, first-class presets (Original, Enhanced, Performance, Custom), non-destructive external asset package loading, and strict protection against cosmetic or unsupported switches.

**Architecture:** A 4-layer decoupled system combining:
1. A typed metadata and schema registry (`ModFeatureMeta` + `black.toml` via `toml++ v3.4.0`) enforcing live capability validation.
2. An automatic VFS overlay in `runtime/kernel/files.cpp::Resolve()` that prioritizes `/storage/emulated/0/Black/mods/<id>/vfs/` overrides while falling back automatically to the untouched Xbox `.iso`.
3. A premium military tactical ImGui console (7 categories + Tactical Briefing card + Restart queue badge) mirrored into the in-game Flash options menu.
4. Concrete host Vulkan post-processing, audio mixing, guest memory readers, and Arabic RTL shaping.

**Tech Stack:** C++20, Android NDK 28 (arm64-v8a), Vulkan 1.1 + HLSL SPIR-V, SDL3, Dear ImGui 1.91.0, toml++ v3.4.0, HarfBuzz + FriBidi (for Arabic), XboxXBE-Recomp portable kernel runtime.

**Spec Reference:**
- [`MOD_SETTINGS_ARCHITECTURE.md`](MOD_SETTINGS_ARCHITECTURE.md)
- [`MOD_SETTINGS_UI_DESIGN.md`](MOD_SETTINGS_UI_DESIGN.md)
- [`SETTINGS_SCHEMA.md`](SETTINGS_SCHEMA.md)
- [`FEATURE_IMPLEMENTATION_MATRIX.md`](FEATURE_IMPLEMENTATION_MATRIX.md)
- [`REPOSITORY_AUDIT.md`](REPOSITORY_AUDIT.md)

## Global Constraints
- **Preservation & Non-Destruction**: Never modify, unpack, or overwrite the original Xbox `.iso` or `default.xbe`.
- **Anti-Cosmetic Law**: Every interactive UI toggle or slider must bind to a working runtime consumer; unverified/future features must remain visually locked as `Planned` or `Unavailable`.
- **Clean Fork Isolation**: All modifications must remain confined to fork branch `black-enhanced-edition` under `ALIALMORTADA476/Black-Android`.
- **Backward Compatibility**: Existing top-level keys in `black.toml` must remain readable so previous saves and settings never break.

---

## Tasks

### Task 1: Enhanced Settings Schema & Metadata Registry

**Files:**
- Create: `src/common/mod_schema.h`
- Create: `src/common/mod_schema.cpp`
- Modify: `src/common/options.h:10-75`
- Modify: `src/common/game_info.cpp:30-135`
- Modify: `native/main.cpp:125-155`

**Interfaces:**
- Consumes: `toml++ v3.4.0`, `app::Settings`, `game::Options`.
- Produces: `struct EnhancedOptions`, `enum class FeatureStatus`, `enum class ApplyScope`, `struct ModFeatureMeta`, `modsettings::GetFeature(OptionId)`.

- [ ] **Step 1: Write host test or validation harness for schema serialization**
  - Verify that `mod_schema.h` parses and serializes namespaced tables (`[graphics]`, `[combat]`, `[presets]`, `[packages]`) alongside legacy root keys without throwing or discarding unknown keys.
- [ ] **Step 2: Define `ModFeatureMeta` and `EnhancedOptions` structs in `src/common/mod_schema.h`**
  - Enforce status flags (`Implemented`, `Planned`, `Unavailable`) and apply scopes (`Immediate`, `ShaderRebuild`, `SceneReload`, `AppRestart`).
- [ ] **Step 3: Update `src/common/options.h` & `src/common/game_info.cpp`**
  - Integrate new fields (`tone_mapping`, `cas_sharpen_enabled`, `cas_sharpen_strength`, `bloom_intensity`, `texture_lod_bias`, `screen_damage_vignette`, `soft_shadows`, etc.) with legacy fallback logic in `game::Load()` and `game::Save()`.
- [ ] **Step 4: Update `native/main.cpp` table serialization**
  - Ensure `app::Table()` writes both legacy flat keys and namespaced sections atomically.
- [ ] **Step 5: Verify build compiles cleanly with schema changes**
  - Run test or build check; confirm zero regression on existing settings.

---

### Task 2: Presets Engine & Staged Restart Tracking

**Files:**
- Create: `src/common/presets.h`
- Create: `src/common/presets.cpp`
- Modify: `src/common/menu.cpp:240-300`

**Interfaces:**
- Consumes: `mod_schema.h`, `game::Options`, `app::Settings`.
- Produces: `enum class PresetKind { Original, Enhanced, Performance, Custom }`, `presets::Apply(PresetKind)`, `presets::Active()`, `presets::HasPendingRestart()`, `presets::PendingRestartCount()`.

- [ ] **Step 1: Implement preset mapping tables in `src/common/presets.cpp`**
  - Map `Original`, `Enhanced`, and `Performance` values strictly matching `SETTINGS_SCHEMA.md`.
  - Exclude cheats, controls, and language choices from automatic preset overrides.
- [ ] **Step 2: Implement snapshot and dirty-tracking logic**
  - Before applying a preset, store current values in `[presets.last_custom]` so user adjustments can be undone.
  - Automatically flip active preset to `Custom` when any member option is manually adjusted.
- [ ] **Step 3: Implement staged restart counter**
  - Increment pending restart count when `ApplyScope::AppRestart` or `SceneReload` items are changed.
- [ ] **Step 4: Expose preset helpers to UI**
  - Wire `presets::Next()` and `presets::ResetCategory()` for gamepad shortcuts.

---

### Task 3: Modular Asset Overlay & Package Manager Subsystem

**Files:**
- Create: `native/mods/mod_manager.h`
- Create: `native/mods/mod_manager.cpp`
- Modify: `CMakeLists.txt:40-70` (add `native/mods/mod_manager.cpp` to build)
- Modify: `native/main.cpp:280-310` (initialize `mods::PackageManager::Scan()`)
- Modify: `TGP482/XboxXBE-Recomp/runtime/kernel/files.cpp:270-285`

**Interfaces:**
- Consumes: `std::filesystem`, `toml++`, `/storage/emulated/0/Black/mods/`, `MountedDisc()`.
- Produces: `mods::PackageManager`, `mods::ResolveVfsOverride(const std::vector<std::string>& parts) -> std::optional<fs::path>`, `mods::ListPackages()`.

- [ ] **Step 1: Create `native/mods/mod_manager.h` and manifest parser**
  - Parse `mod.toml` (`title_id == "0x45410083"`, `priority`, `vfs`, `host` flags).
  - Enforce security rule: Reject any package attempting to override `default.xbe`.
- [ ] **Step 2: Implement directory scanner in `native/mods/mod_manager.cpp`**
  - Probe primary path `/storage/emulated/0/Black/mods/` and fallback `Android/data/com.racas.black/files/mods/`.
  - Build active priority-sorted file index for enabled packages.
- [ ] **Step 3: Integrate VFS hook into `runtime/kernel/files.cpp::Resolve()`**
  - Check `mods::ResolveVfsOverride(parts)` before `MountedDisc()` lookup; return host path if present, otherwise fall back to `disc->Find(parts)`.
- [ ] **Step 4: Verify fallback to untouched ISO**
  - Verify that deleting or disabling a package instantly reverts file resolution to the untouched disc image.

---

### Task 4: Tactical Mil-Spec ImGui UI & Flash Mirror Bridge

**Files:**
- Create: `src/common/mod_menu.h`
- Create: `src/common/mod_menu.cpp`
- Modify: `src/common/menu.h:20-40`
- Modify: `src/common/menu.cpp:70-290`
- Modify: `src/game/options_menu.cpp:40-100`

**Interfaces:**
- Consumes: `ImGui 1.91.0`, `mod_schema.h`, `presets.h`, `mod_manager.h`.
- Produces: `modui::DrawEnhancedConsole()`, `modui::IsOpen()`, Flash menu entry `ENHANCED OPS`.

- [ ] **Step 1: Implement Tactical Mil-Spec theme styling and color tokens**
  - Configure `--surface-panel` (`#141311`), `--accent-brass` (`#C59B3C`), `--accent-gold` (`#E5B84B`), and `--status-active` (`#6E8B4B`).
- [ ] **Step 2: Build top bar (Preset switcher + 7 category tabs)**
  - Add category navigation: `Graphics`, `Combat & Blood`, `Characters`, `Audio`, `Arabic`, `Gameplay`, `Interface`.
- [ ] **Step 3: Build two-column layout (60% controls / 40% Tactical Briefing card)**
  - Render categorized rows with min 48dp touch targets and gamepad D-pad focus.
  - Render right-side Tactical Briefing card with live architectural metadata, performance gauge, and non-cosmetic status badges.
- [ ] **Step 4: Render non-interactive status badges for `Planned` and `Unavailable` items**
  - Display lock glyph and milestone info; prevent user interaction while keeping item inspectable.
- [ ] **Step 5: Build bottom action bar & pending restart banner**
  - Render persistent pending restart count banner with a direct `RESTART NOW` action.
- [ ] **Step 6: Inject `ENHANCED OPS` link into in-game Flash options menu**
  - Wire `src/game/options_menu.cpp` to launch the console directly from the title screen.

---

### Task 5: Host Vulkan Visual Enhancements (ACES, CAS, Soft Shadows)

**Files:**
- Modify: `native/vulkan.cpp:85-205` (`kFilterHlsl` and uniforms)
- Modify: `native/vulkan.cpp:2337-2380` (`StageSampler()`)
- Modify: `native/vulkan.cpp:2814-2835` (`GlowComposite()`)
- Modify: `native/vulkan.cpp:3155-3255` (`DrawFrame()`)
- Modify: `native/vk_shader.cpp:1-40` (bump cache version if shader code modified)

**Interfaces:**
- Consumes: `[graphics]` settings from `mod_schema.h`.
- Produces: Standalone CAS sharpening pass, ACES Filmic curve, scalable bloom, and PCF soft shadow hook.

- [ ] **Step 1: Add ACES Filmic tone mapping curve to `kFilterHlsl`**
  - Implement ACES curve alongside existing Reinhard `Tone()` in `native/vulkan.cpp`.
- [ ] **Step 2: Decouple CAS sharpening from screen filters**
  - Allow standalone CAS sharpening with adjustable strength slider (`0%–100%`) stacking with FXAA and HDR.
- [ ] **Step 3: Implement bloom intensity scaling**
  - Scale composite constants in `GlowComposite()` according to `bloom_intensity` setting.
- [ ] **Step 4: Implement configurable Mip LOD Bias & Trilinear override**
  - Subtract `texture_lod_bias` in `StageSampler()` to improve surface sharpness on mobile screens.
- [ ] **Step 5: Register `gpu::g_shader_hooks` for PCF Soft Shadows**
  - Implement 4-tap Poisson-disk PCF in `ShaderHooks::Shadow()`; tie to `soft_shadows` setting.

---

### Task 6: Screen Blood Damage Vignette & Combat Feedback

**Files:**
- Modify: `native/vulkan.cpp:3160-3220` (`kFilterHlsl` / `DrawFrame()`)
- Modify: `src/game/hud.cpp:240-265`
- Modify: `src/game/cheats.cpp:15-35`

**Interfaces:**
- Consumes: `kWorld (0x2D45D4)`, `Player+0x2F0` (`kCharHealth`), `0x384` (`kCharDying`), `HudShown()`.
- Produces: Dynamic damage vignette pulse, low-health desaturation factor passed to `kFilterHlsl`.

- [ ] **Step 1: Implement damage impulse tracker in `src/game/hud.cpp`**
  - Read player health safely; track negative health deltas and low-health ratio (`< 30%`).
- [ ] **Step 2: Add radial vignette pass in `native/vulkan.cpp::DrawFrame()`**
  - Feed damage flash intensity into `kFilterHlsl` constant buffer; compute subtle procedural blood vignette on hit.
- [ ] **Step 3: Connect low-health desaturation**
  - Smoothly reduce color saturation in `kFilterHlsl` when health is critically low.
- [ ] **Step 4: Verify zero overhead and zero impact on guest state**
  - Confirm guest logic remains 100% read-only with no side effects on game saves or AI scripts.

---

### Task 7: Arabic Localization Engine Phase 1: Host ImGui & RTL

**Files:**
- Modify: `CMakeLists.txt:20-50` (fetch `harfbuzz` and `fribidi`)
- Modify: `src/common/options.h:12` (add `"ara"` to `kLanguages`)
- Modify: `src/common/translations.cpp:20-290`
- Modify: `native/ui.cpp:30-80` (load Arabic TTF font into ImGui)
- Modify: `src/common/mod_menu.cpp` (RTL column/row mirroring)

**Interfaces:**
- Consumes: HarfBuzz, FriBidi, `app/src/main/assets/fonts/NotoSansArabic-Bold.ttf`.
- Produces: Arabic glyph rendering in ImGui, visual RTL reordering in `app::Ui()`, mirrored settings console.

- [ ] **Step 1: Link HarfBuzz and FriBidi via `CMakeLists.txt`**
  - Add static dependencies for OpenType text shaping and Unicode BiDi reordering.
- [ ] **Step 2: Load Arabic TTF font in `native/ui.cpp::InitUi()`**
  - Configure `ImFontConfig` with Arabic glyph ranges (`0x0600..0x06FF` + Presentation Forms-B).
- [ ] **Step 3: Update `src/common/translations.cpp` with BiDi shaping**
  - Shape UTF-8 Arabic text before rendering; bypass Latin-only `Upper()` conversion for Arabic code units.
- [ ] **Step 4: Implement RTL console mirroring in `src/common/mod_menu.cpp`**
  - Mirror category tabs, row labels, and controls from right-to-left when Arabic is selected.
- [ ] **Step 5: Verify mixed Arabic/English telemetry rendering**
  - Test strings like `"60 FPS معدل الإطارات"`; confirm letters join correctly and numbers read LTR.

---

### Task 8: Verification, Fallback Validation & Non-Regression Testing

**Files:**
- Test / Verify: Entire integrated suite on Android device / emulator.

- [ ] **Step 1: Verify untouched ISO fallback**
  - Launch with original USA and Europe `.iso` images; verify unmodified stock experience boots flawlessly.
- [ ] **Step 2: Test Presets state machine**
  - Toggle between `Original`, `Enhanced`, and `Performance`; confirm only valid options change and cheats remain untouched.
- [ ] **Step 3: Test Pending Restart queue**
  - Change language or package; confirm restart badge appears and `RESTART NOW` reboots cleanly.
- [ ] **Step 4: Test Modular Package enable/disable**
  - Place a test asset pack in `Black/mods/test_pack/`; confirm VFS override works when enabled and reverts when disabled.
- [ ] **Step 5: Run smoke test checklist**
  - Verify Level 1 (*Veblensk Street*) runs with 60 FPS, soft shadows, CAS sharpen, and damage vignette without crash or memory leak.

