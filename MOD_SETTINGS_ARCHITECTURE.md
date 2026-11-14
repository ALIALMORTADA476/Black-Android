# BLACK Enhanced Edition — Modular Modification Settings Architecture (`MOD_SETTINGS_ARCHITECTURE.md`)

> **Status**: Architecture & Design Specification (Research Phase — No source code modified).  
> **Target Repository**: `Black-Android` (`black-enhanced-edition` branch).  
> **Core Principle**: Zero cosmetic switches. Every interactive setting maps deterministically to an active host subsystem, a verified guest recompilation hook, or a validated modular asset overlay. Unsupported or unverified modifications are strictly locked as **Planned** or **Unavailable**.

---

## 1. Architectural Vision & Layer Boundaries

Because `Black-Android` runs statically recompiled Xbox x86 code (`generated/game/*.cpp` → `libmain.so`) inside the `XboxXBE-Recomp` runtime while streaming read-only disc data (`D:\`) from user storage, the modification settings architecture spans **four isolated execution layers**:

```
┌────────────────────────────────────────────────────────────────────────────┐
│ 1. PRESENTATION & SETTINGS UI LAYER (Host ImGui + Flash Bridge)            │
│    • Primary UI: Modular Mil-Spec ImGui Console (src/common/mod_menu.cpp)  │
│    • Mirror UI:  In-Game Flash Options Link (src/game/options_menu.cpp)    │
│    • Presets & Dirty-State Tracker (Original / Enhanced / Perf / Custom)   │
└───────────────────────────────┬────────────────────────────────────────────┘
                                │ (Read/Write & Validation)
┌───────────────────────────────▼────────────────────────────────────────────┐
│ 2. PERSISTENCE & CAPABILITY REGISTRY (src/common/options.h, native/main)   │
│    • Persistent Store: ExeDir() / "black.toml" (+ mods/lock state)         │
│    • Feature Registry: Maps each option ID -> {Layer, Status, RestartScope}│
│    • Hardware & Asset Capability Probe (Vulkan limits, HDR, Disc/Mods)     │
└───────────┬───────────────────────────┬────────────────────────┬───────────┘
            │                           │                        │
┌───────────▼───────────┐   ┌───────────▼───────────┐   ┌────────▼───────────┐
│ 3A. HOST RUNTIME      │   │ 3B. MODULAR ASSET     │   │ 3C. GUEST LOGIC    │
│ • VulkanBackend       │   │     OVERLAY (VFS)     │   │     HOOKS (XBE)    │
│ • ShaderHooks (HLSL)  │   │ • Black/mods/<id>/    │   │ • config/game.toml │
│ • AAudio Mixer        │   │ • Pre-ISO Resolve()   │   │ • src/game/*.cpp   │
│ • ImGui / Touch / UI  │   │ • Untouched ISO Fall  │   │ • Guest RAM Reads  │
└───────────────────────┘   └───────────────────────┘   └────────────────────┘
```

---

## 2. Setting Capability & Integrity Model (Anti-Cosmetic-Switch Rule)

To enforce strict engineering honesty, every option defined in the settings system binds to a typed metadata descriptor (`ModFeatureMeta`) instead of a standalone boolean/integer field:

```cpp
enum class FeatureStatus : uint8_t {
    Implemented,  // Live and hooked into active host or guest execution path
    Planned,      // Designed / mapped to a future milestone; control is LOCKED (non-interactive)
    Unavailable   // Temporarily disabled due to missing hardware capability (e.g. SDR screen HDR)
                  // or missing external asset package (e.g. no Arabic voice pack detected)
};

enum class ApplyScope : uint8_t {
    Immediate,    // Takes effect on the next rendered frame or audio tick
    ShaderRebuild,// Triggers brief pipeline/cache flush without closing app
    SceneReload,  // Takes effect on next mission load / level reset
    AppRestart    // Requires relaunching the app (latched at boot, e.g., BootLanguage / VFS mount)
};
```

### Runtime Enforcement Rules
1. **No Phantom Bindings**: The UI cannot register a toggle or slider unless it binds to a `ModFeatureMeta` entry with a concrete runtime consumer (`VulkanBackend`, `audio::`, `touch::`, `game::`, or `mods::PackageManager`).
2. **Non-Interactive Locks**: Items marked `FeatureStatus::Planned` or `FeatureStatus::Unavailable` cannot be toggled, stepped, or modified by presets. They display a clear state badge explaining *why* (`[PLANNED — MILESTONE 5]` or `[REQUIRES HDR DISPLAY]` / `[REQUIRES ASSET PACK]`).
3. **Dirty Restart Queue**: When an `ApplyScope::AppRestart` or `ApplyScope::SceneReload` setting is changed, the live active state (`BootConfig`) remains untouched until restart/reload, while the staged value (`PendingConfig`) is written to `black.toml` and flagged in the UI status bar (`RESTART REQUIRED (1)`).

---

## 3. Configuration Lifecycle & Hot-Apply vs Restart Matrix

### 3.1 Dual-Buffer State Pattern (`Live` vs `Staged`)
Currently, [`native/main.cpp`](native/main.cpp#L132-L149) reads `black.toml` at startup into `app::Settings` + `game::Options` (`Opt()`), and [`src/game/game_info.cpp`](src/game/game_info.cpp#L75-L78) latches `BootLanguage()` using a static initializer.

The Enhanced Edition architecture standardizes this into:
- **Staged Config (`game::Opt()`, `app::Get()`, `mods::Registry()`)**: Reflects user edits in real time and immediately serializes to `ExeDir()/black.toml` (via `toml++ v3.4.0` atomic temp write + rename).
- **Boot-Latched Config (`game::BootState()`)**: Holds boot-time immutable settings (VFS mounted mod paths, language IDs, guest memory pool capacities allocated at `Boot()`/`DrawListRoom`).
- **Apply Dispatcher (`modsettings::ApplyChanged(OptionId)`)**:
  - `Immediate` changes (CAS sharpen, ACES tone mapping, bloom scale, screen damage vignette, FPS cap, audio channel downmix, FOV, touch opacity/skin, cheats) update atomic uniforms or subsystem state immediately on the next frame.
  - `ShaderRebuild` changes (enabling/changing `soft_shadows` PCF mode via `gpu::ShaderHooks::Shadow()`) notify `VulkanBackend` to select the alternate cached SPIR-V permutation (pre-warmed or compiled cleanly via `vk_shader.cpp`) without terminating the session.
  - `SceneReload` changes (e.g., future blood decal pool scaling or particle multiplier affecting level initialization) mark `pending_scene_reload = true`.
  - `AppRestart` changes (enabling/disabling an asset package that alters `D:\` tables/archives, switching localization mode, changing XBE-coupled memory allocations) mark `pending_app_restart = true` and surface an inline **RESTART NOW** action (`kernel::Relaunch` or clean Android Activity restart).

---

## 4. Modular Asset Modification Subsystem (`Black/mods/`)

As established in [`REPOSITORY_AUDIT.md`](REPOSITORY_AUDIT.md#L98-L120), the baseline runtime streams all `D:\` files directly from the `.iso` via `XboxXBE-Recomp`'s `Disc::ReadAt()` (`pread`), and `files.cpp::Resolve()` skips host lookups whenever `MountedDisc()` is active. Modifying original `.iso` files violates preservation rules and is slow/destructive on mobile storage.

### 4.1 Non-Destructive External Package Layout
All optional enhancement assets reside outside the APK and outside the original `.iso`:
- **Primary path**: `/storage/emulated/0/Black/mods/<package_id>/`
- **Scoped fallback path**: `Android/data/com.racas.black/files/mods/<package_id>/`

Each package folder contains a declarative manifest (`mod.toml`) and clean subdirectories mirroring either **Xbox Disc VFS overrides** (`vfs/...`, e.g., `vfs/sound/...`, `vfs/videos/...`) or **Host-Managed Packs** (`host/...`, e.g., `host/textures/<hash>.ktx2`, `host/fonts/arabic.ttf`, `host/strings/ara.toml`):

```text
/storage/emulated/0/Black/
├── Black (USA).iso                  <-- Untouched original disc image (read-only)
└── mods/
    ├── arabic_ui_pack/
    │   ├── mod.toml                 <-- Package metadata, hash, priority, compatibility
    │   └── host/
    │       ├── fonts/Cairo-Bold.ttf
    │       └── strings/ara_main.toml
    └── hd_hud_icons/
        ├── mod.toml
        └── host/textures/...
```

### 4.2 Virtual File System Overlay & Automatic ISO Fallback
To enable/disable `vfs/` overrides safely without mutating the `.iso`:
1. On boot, `mods::PackageManager::Scan()` discovers installed packages, validates each `mod.toml` (`api_version`, target `title_id = 0x45410083`, file hashes/existence), and checks enabled state + load priority stored in `black.toml`.
2. In `XboxXBE-Recomp/runtime/kernel/files.cpp` (`Resolve()`), before the `MountedDisc()` return at lines 275–276:
   ```cpp
   if (best->first == kCdRom) {
       if (auto override_path = mods::ResolveVfsOverride(parts)) {
           return {override_path->string()};  // Host file from enabled mod package
       }
       if (const Disc* disc = MountedDisc()) {
           return {{}, true, disc->Find(parts)};  // Automatic fallback to untouched ISO
       }
   }
   ```
3. **File Collision & Compatibility Rules**:
   - **No XBE overrides via `mods/`**: Files targeting `default.xbe` in `vfs/` are explicitly blocked by `PackageManager::Validate()` because guest machine code is compiled into `libmain.so` (`kImageSha1` mismatch protection).
   - **Priority Arbitration**: Packages declare integer `priority` (plus user override order). Higher priority wins per file path; toggling any package OFF removes its VFS entries from the active lookup index and restores original ISO data seamlessly on restart.
   - **Compatibility Checks**: Before enabling a package, `PackageManager` checks:
     1. Manifest `min_port_version` ≤ current build version.
     2. Required disc region/release compatibility (e.g., `any`, `usa`, `europe`).
     3. Missing-dependency or mutual-conflict declarations (e.g., two incompatible HUD texture packs).
     4. Optional CRC32/size sanity check for guest-consumed binary tables (e.g., `Main*.bin`).

---

## 5. Game Logic Modification Bridge (Host vs Recompiled Guest)

Options modify game behavior through three concrete mechanisms, strictly ordered from lowest risk to highest complexity:

1. **Host Pipeline & Shader Injection (`native/vulkan.cpp`, `native/audio.cpp`, `native/touch.cpp`)**:
   - Zero risk to `default.xbe` stability.
   - Connects directly to Vulkan fullscreen passes (`kFilterHlsl`), texture sampling (`StageSampler()`), `gpu::ShaderHooks::Shadow()`/`Output()`, ImGui overlay rendering, and AAudio channel mixing (`audio::SetOutputMode`).
2. **Confirmed Guest Memory Readers & Existing Recomp Hooks (`src/game/*.cpp`, `config/game.toml`)**:
   - **Read-only Combat Feedback**: Reads confirmed guest RAM (`kWorld = 0x2D45D4`, `kWorldPlayer = 0x30`, `kCharHealth = 0x2F0`, `kCharDying = 0x384`, `HudShown()`) on the render/hook side to drive Screen Blood Vignette & Low-Health Desaturation in `VulkanBackend::DrawFrame()`.
   - **Existing Parameterized Hooks**: Scales LOD/particle fade (`ParticleFade @ 0x16DFFB`, `DrawDistance`), draw-list pool limits (`DrawListRoom @ 0x1EE5EA`), FOV (`FieldOfView`, `GunEffect`), string ID hooks (`sub_001F3A80`), and ragdoll/physics tick timing (`PhysicsTime @ 0x1F11CD`, `BodyRestTick @ 0x77EE9`).
3. **New Guest Disassembly Hooks (Future Milestones — Locked as `Planned` Until Implemented)**:
   - Features requiring reverse-engineering of unverified XBE offsets—such as blood emitter burst counts (`sub_001E74B0` callee emitters), decal ring buffer sizes/fade timers (`DistanceShorter` sites), corpse despawn/AI-pool decoupling (`kCharDying`), and in-engine Flash font atlas injection (`"Eurostile LT Std"`)—remain **non-interactive `Planned` rows** until their `[[hook]]` entries in `config/game.toml` and `src/game/` implementations are built, compiled across the 64 partitions, and verified on hardware.

---

## 6. Presets Architecture

Four first-class presets manage cross-category coherence without altering gameplay cheats or user key/touch bindings:

| Preset | Philosophy | Covered Domains |
|---|---|---|
| **Original** | 2006 Xbox visual & audio baseline (pure preservation) | 67% (~720p) or native scaling, FXAA/AA off, ACES/HDR off, Sharpen 0%, Bloom 100%, Draw Distance 1×, Soft Shadows Off, Damage Vignette Off, Touch Skin Classic, English/Disc language, Cheats unchanged. |
| **Enhanced** | Curated "Enhanced Edition" showcase balanced for Snapdragon 8xx / modern Mali/Adreno GPUs | Dynamic Res (60 FPS target, 75%–100%), FXAA On, ACES Filmic Tone Mapping, CAS Sharpen 35%, Anisotropic 16×, Draw Distance 2×, Soft Shadows 4-Tap PCF, Screen Damage Vignette On, Modern Touch Skin. |
| **Performance** | Sustained frame pacing & low thermal load on mid-range / older Vulkan 1.1 phones | Dynamic Res (60 FPS target, 50%–85%), FXAA Off, Reinhard/Off Tone Mapping, CAS Sharpen 20%, Anisotropic 4×, Draw Distance 1×, Soft Shadows Off, Bloom 50%, Screen Damage Vignette Minimal. |
| **Custom** | Automatic state entered whenever any preset-managed setting is manually edited | Retains exact per-option user values; changing back to identical values of a built-in preset optionally re-highlights that preset or preserves `Custom`. |

### Preset Safety Rules
- **Scope Exclusions**: Presets **never** overwrite cheats (`infinite_ammo`, `god_mode`, etc.), button coordinates (`layout.toml`), keybindings, controller deadzones, or user-downloaded localization choice (`language` / `arabic_ui`).
- **One-Click Reversible**: Applying a preset snapshots the previous custom profile in memory/TOML (`[presets.last_custom]`) so users can revert an accidental preset switch immediately.

