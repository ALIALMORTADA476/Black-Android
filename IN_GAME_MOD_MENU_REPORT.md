# BLACK Enhanced Edition — In-Game Tactical Mod Menu Report

**Milestone**: Stage 4 — In-Game Tactical Mod Menu & Functional Feature Integration  
**Target Package**: `com.racas.black.enhanced`  
**Version**: `v1.2.0-enhanced` (Build `25`)  
**Target Branch**: `black-enhanced-edition`  
**Repository**: `E:\ai\black`  

---

## 1. Executive Summary

In Stage 4, the **In-Game Tactical Mod Menu** (the *Tactical Ops Console*) has been designed and implemented for BLACK Enhanced Edition. Rather than providing a mockup, this implementation integrates Dear ImGui (v1.91.0) with the engine's Vulkan rendering and SDL3 input infrastructure.

The Tactical Ops Console gives players real-time in-game control over all supported enhancements without leaving the combat environment, while strictly adhering to the **Strict Anti-Mockup Rule** by displaying unhooked features as read-only with explicit tactical badges and requirements.

---

## 2. Architectural Overview

```
+----------------------------------------------------------------------------------------------------+
|                         BLACK ENHANCED EDITION -- TACTICAL OPS CONSOLE                              |
|                                     v1.2.0-enhanced (BUILD 25)                                     |
+----------------------------------------------------------------------------------------------------+
| TACTICAL PROFILE: [ ORIGINAL ]    [ ENHANCED ]    [ PERFORMANCE ]    [ CUSTOM ]                     |
|                   Preserves 2006  Modern Visual   Optimized 60 FPS   User-defined custom tuning    |
+----------------------------------------------------------------------------------------------------+
| [GRAPHICS] [COMBAT] [CHARACTERS] [AUDIO] [LOCALIZATION] [GAMEPLAY] [INTERFACE] [CHEATS & WEAPONS]  |
+----------------------------------------------------------------------------------------------------+
|                                                                                                    |
|  DISPLAY ENGINE                                                                                    |
|    RENDER RESOLUTION ..................................................... [<] 100% (1920x1080) [>] |
|    FIELD OF VIEW ......................................................... [<] 70 (DEFAULT) [>]   |
|    VIEWMODEL FIELD OF VIEW ............................................... [<] 70 (DEFAULT) [>]   |
|    FMV SCALE ............................................................. [<] FILL [>]           |
|    SCREEN FILTER ......................................................... [<] OFF [>]            |
|    TONE MAPPING .......................................................... [<] ACES FILMIC [>]     |
|    CAS CONTRAST SHARPENING ............................................... [PLANNED] (Compute)     |
|    CAS SHARPEN STRENGTH .................................................. [PLANNED] (35%)         |
|                                                                                                    |
|  PERFORMANCE & PACING                                                                              |
|    VERTICAL SYNC ......................................................... [<] OFF [>]           |
|    MAX FRAME RATE ........................................................ [<] UNLIMITED [>]      |
|    FPS COUNTER ........................................................... [<] ON [>]            |
|    DYNAMIC RESOLUTION .................................................... [<] OFF [>]           |
|                                                                                                    |
|  TEXTURE & RENDERING QUALITY                                                                       |
|    ANISOTROPIC FILTERING ................................................. [<] 8x [>]             |
|    DRAW DISTANCE ......................................................... [<] 2x [>]             |
|    RELOAD BLUR ........................................................... [<] ON [>]            |
|    BLOOM GLOW ............................................................ [<] ON [>]            |
|    BLOOM INTENSITY ....................................................... [<] 100% (DEFAULT) [>]  |
|    VOLUMETRIC FOG ........................................................ [<] ON [>]            |
|    ANTI-ALIASING ......................................................... [<] FXAA [>]          |
|    HDR PIPELINE .......................................................... [<] ON [>]            |
|    TEXTURE LOD BIAS ...................................................... [<] -0.5 (SHARP) [>]   |
|    TRILINEAR FILTERING ................................................... [<] ON [>]            |
|    SOFT SHADOW FILTERING ................................................. [PLANNED] (PCF 4-Tap)  |
|    AMBIENT OCCLUSION (SSAO) .............................................. [PLANNED] (Half-Res)   |
|    HD TEXTURES OVERLAY ................................................... [PLANNED] (Asset Pack) |
|                                                                                                    |
+----------------------------------------------------------------------------------------------------+
| LB/RB: Switch Tabs | D-Pad/Arrows: Adjust | B/Back: Close          [RESTORE DEFAULTS] [CLOSE CONSOLE]|
+----------------------------------------------------------------------------------------------------+
```

---

## 3. Input Architecture & Accessibility

### 3.1 Triple-Trigger Activation
The console can be opened and closed through three distinct input vectors:
1. **On-Screen Touch Overlay**: Dedicated Gear icon (`touch::kPanel`) located at the top-left of the HUD.
2. **Game Controller Shortcuts**:
   - `L3 + R3` (Clicking both analog thumbsticks simultaneously)
   - `Start + Back` (Simultaneous press)
3. **Hardware Keyboard**: `F10` or `Escape` (when not actively capturing keybinds).

### 3.2 Total Input Isolation
When `app::MenuOpen()` is true:
- **Game Controller (`native/input.cpp`)**: In `input::Poll(port)`, all gamepad button bits, analog triggers, and stick axes (`lx, ly, rx, ry`) are zeroed before being passed to `touch::Apply()` and `app::kGame.pad()`.
- **Touch Screen (`native/touch.cpp`)**: Line 1488 suppresses all gameplay touch pads (`b.press != kPanel && !editing`), ensuring no touch event triggers weapon firing, weapon switching, or camera pivoting.
- **Mouse & Keyboard (`native/main.cpp`)**: Cursor is unlatched (`input::ReleaseCursor()`) and routed exclusively to Dear ImGui IO events.

### 3.3 Controller Navigation Mapping
- **D-Pad Up / Down**: Moves active focus between settings rows.
- **D-Pad Left / Right**: Steps numeric values and cycles enumeration options via `OptionRow`.
- **Button A (South / Cross)**: Confirms selections, opens links, triggers buttons.
- **Button B (East / Circle) & Back**: Instantly closes the Tactical Ops Console.
- **Left Bumper (LB / L1)**: Cycles category tabs left.
- **Right Bumper (RB / R1)**: Cycles category tabs right.

### 3.4 Touch UX & Hitbox Optimization
- Minimum touch hitbox height of `em * 1.8f` to `em * 2.0f` (~45–52 physical dp on 1080p+ mobile displays), exceeding Google Material guidelines (48dp).
- **DragScroll Inertia**: Integrated touch dragging in the content window with smooth exponential glide decay (`kFriction = 6.0f`).
- Deadband threshold (`std::fabs(drag.y) > em * 0.5f`) prevents accidental setting changes while scrolling.

---

## 4. Preset State Machine Integration

A persistent tactical profile bar is positioned at the top of the interface:
`[ ORIGINAL ]   [ ENHANCED ]   [ PERFORMANCE ]   [ CUSTOM ]`

1. **Active Preset Highlight**: Displays active profile retrieved from `modsettings::Presets().ActiveKind()`.
2. **One-Tap Switching**: Tapping any preset executes `modsettings::Presets().ApplyPreset()`, modifying only preset-managed settings while preserving:
   - Cheats (God mode, infinite ammo, rapid fire, etc.)
   - Controller bindings and deadzones
   - Localization settings (Language, Arabic translation toggles)
3. **Auto-Custom Transition**: Modifying any individual managed setting (e.g. toggling Bloom or stepping Anisotropic Filtering) notifies `modsettings::Presets().NotifySettingChanged()`, smoothly transitioning the preset kind to `Custom` and latching user modifications.
4. **Custom Snapshot Recovery**: Switching from `Custom` to `Enhanced` and back to `Custom` restores the user's bespoke settings without data loss.

---

## 5. Visual Identity & Theme Accents

The console faithfully embraces the iconic visual identity of Criterion's original BLACK:
- **Base Canvas**: Ultra-deep matte obsidian background (`rgba(9, 8, 6, 0.94)`).
- **Default Brass Accent**: Weathered military brass (`rgba(220, 176, 87, 1.0)`).
- **Live Theme Accent Engine**: Configurable under the `INTERFACE` tab:
  - **Tactical Brass**: Classic 2006 Criterion cover art brass.
  - **Special Ops Crimson**: Covert night-ops crimson (`rgba(235, 66, 61, 1.0)`).
  - **Military Olive**: Army tactical olive drab (`rgba(158, 191, 89, 1.0)`).
- **Dynamic Palette**: Real-time shader and UI color palette recalculation without requiring game restart.

---

## 6. Restart Notification System

For settings classified with `ApplyScope::AppRestart` (such as `arabic_ui_enabled` and `arabic_subtitles_overlay`):
- **Inline Notification**: The row value dynamically displays with an amber `(! RESTART)` tag when the modified value differs from the boot snapshot.
- **Tactical Footer Warning**: When `modsettings::Registry().RequiresAppRestart()` detects pending restart settings, an amber warning banner appears:
  `(!) RESTART REQUIRED (X settings changed - applies on next launch)`
- **Persistent State**: Settings are atomically saved to `black.toml` immediately so that they take effect cleanly upon the subsequent boot.

---

## 7. Delivery Status

| Component | Status | Target File |
|---|---|---|
| In-Game UI Architecture | **Implemented** | `src/common/menu.cpp` |
| Controller Combo Triggers (L3+R3) | **Implemented** | `native/input.cpp` |
| Input Isolation | **Implemented** | `native/input.cpp`, `native/touch.cpp` |
| Preset State Selector | **Implemented** | `src/common/menu.cpp`, `src/common/presets.cpp` |
| 8 Category Navigation Tabs | **Implemented** | `src/common/menu.cpp` |
| Strict Anti-Mockup Planned Rows | **Implemented** | `src/common/menu.cpp` |
| Restart Notification Banner | **Implemented** | `src/common/menu.cpp`, `src/common/mod_schema.cpp` |
| Theme Accent Switching | **Implemented** | `src/common/menu.cpp` |
| Arabic-First Language Default | **Implemented** | `src/common/options.h` |
| Arabic Contextual Shaping & BiDi | **Implemented** | `src/common/arabic.h` |
| Arabic Presentation Forms-B Font | **Implemented** | `app/src/main/assets/fonts/NotoNaskhArabic.ttf` |
| RTL Layout Mirroring | **Implemented** | `src/common/menu.cpp` |
| Android OS Arabic Localization | **Implemented** | `app/src/main/res/values-ar/strings.xml` |

---

## 8. Arabic-First Localization Integration

Per the Critical Addendum, Arabic localization has been realized as a first-class citizen:
- **Default Boot State**: `o.language = "ara"` and `arabic_ui_enabled = true`.
- **Standalone Contextual Shaper & BiDi**: Zero external library footprint via [src/common/arabic.h](file:///E:/ai/black/src/common/arabic.h).
- **RTL Mirrored Layout**: Value stepper placed on the left, right-aligned labels on the right, mirrored category tabs starting from GRAPHICS on the far right.
- **Font Rendering**: 120 KB bundled Google OFL `NotoNaskhArabic.ttf` merged into `ImFontAtlas` with full Presentation Forms-B support.
- **Detailed Report**: See [ARABIC_LOCALIZATION_REPORT.md](file:///E:/ai/black/ARABIC_LOCALIZATION_REPORT.md).
