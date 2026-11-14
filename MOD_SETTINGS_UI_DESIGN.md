# BLACK Enhanced Edition — Modular Settings UI Design (`MOD_SETTINGS_UI_DESIGN.md`)

> **Aesthetic Direction**: *"Tactical Ops Console"* — Dark ballistic gunmetal surfaces paired with spent-cartridge brass hairlines, crisp stenciled headings, and high-contrast numeric telemetry, matching BLACK's cover art and in-game Flash dossier aesthetic.  
> **Rendering Backend**: Dear ImGui `v1.91.0` (`VulkanBackend::RenderUi()`), with an optional entry link injected inside BLACK's native Flash pause/options menu (`src/game/options_menu.cpp`).

---

## 1. Visual Design System & Color Tokens

The Enhanced Edition expands the brass-on-black palette already present in [`src/common/menu.cpp`](src/common/menu.cpp#L75-L95) (`kPanelBg`, `kBorder`, `kHeading`) into a disciplined semantic token system guaranteeing **WCAG AA/AAA contrast (> 11:1)** across mobile OLED screens:

| Token Name | Hex / RGBA | Usage | Contrast vs `#141311` |
|---|---|---|---|
| `--surface-void` | `#090807` (`rgba(9,8,7,0.96)`) | Modal backdrop & recessed frames | — |
| `--surface-panel` | `#141311` (`rgba(20,19,17,0.95)`) | Main container & briefing card | — |
| `--surface-row` | `#1D1B17` | Setting rows & unselected category tabs | — |
| `--surface-row-focus` | `#332814` | Focused / hovered row highlight | — |
| `--accent-brass` | `#C59B3C` | Primary structural borders, active sliders, key badges | **7.2 : 1** |
| `--accent-gold` | `#E5B84B` | Section headings, active tab text, restart badge | **9.8 : 1** |
| `--text-primary` | `#ECE8DF` | Primary option labels & briefing copy | **14.9 : 1** |
| `--text-secondary` | `#9E978B` | Technical sublabels, hints, metadata values | **6.1 : 1** |
| `--text-locked` | `#5E5A52` | Non-interactive `[PLANNED]` / `[UNAVAILABLE]` text | Intentional dim |
| `--status-active` | `#6E8B4B` (Olive Drab) | `[ACTIVE]` pill & enabled toggle indicator | **4.9 : 1** |
| `--status-restart` | `#D9822B` (Amber Flare) | `[RESTART REQUIRED]` pill & counter | **6.6 : 1** |
| `--status-alert` | `#C83232` (Ballistic Crimson) | High perf cost gauge & `[UNAVAILABLE]` warning | **4.6 : 1** |

---

## 2. Typography Hierarchy

| Role | Font Stack | Size (1080p ref) | Treatment |
|---|---|---|---|
| **Console Title / Presets Header** | `Eurostile LT Std` (or geometric condensed sans fallback) + Cairo Bold (Arabic) | `24sp` | All-caps, letter-spacing `+0.08em`, `--accent-gold` |
| **Category Navigation Tabs** | Condensed Geometric Sans (`18sp`) | `18sp` | Bold uppercase, 2px bottom brass underline when active |
| **Option Label** | Clean Tactical Sans (`16sp`) | `16sp` | Sentence or Upper Title case, `--text-primary` |
| **Tabular Values / Telemetry** | Monospace Lining Numerals (`16sp`) | `16sp` | Right-aligned fixed-width cells (`60 FPS`, `16x`, `-0.5`) |
| **Briefing Description & Badges** | Tactical Sans (`14sp` / `12sp`) | `14sp` / `12sp` | Line-height `1.45`, concise single-job captions |

---

## 3. Navigation & Screen Architecture

### 3.1 Entry Points
1. **In-Game / Touch Gear Button & `F10` / `Start+Select`**: Opens the Enhanced Modular Settings console directly over gameplay or menus without disrupting guest state.
2. **In-Game Flash Options Link (`src/game/options_menu.cpp`)**: Adds an `ENHANCED OPS` entry under the native options menu so controller players can jump straight into the Enhanced Edition console from the title screen.

### 3.2 Responsive Landscape Layout (`16:9` / `20:9` Mobile Screens)
Designed for landscape mobile play (`sensorLandscape` in `AndroidManifest.xml`), respecting horizontal notch/cutout safe areas (`SafeAreaInsets`):

```text
+---------------------------------------------------------------------------------------------------+
| BLACK // ENHANCED EDITION OPS          PRESET: [ ORIGINAL | *ENHANCED* | PERFORMANCE | CUSTOM ]   |
+---------------------------------------------------------------------------------------------------+
| [1. GRAPHICS] [2. COMBAT & BLOOD] [3. CHARACTERS] [4. AUDIO] [5. ARABIC] [6. GAMEPLAY] [7. UI]   |
+------------------------------------------------------------+--------------------------------------+
|  SCROLLABLE SETTINGS LIST (60% Width)                      |  TACTICAL BRIEFING & STATUS (40%)    |
|                                                            |                                      |
|  ── POST-PROCESSING & TONE MAPPING ──────────────────────  |  SELECTED:                           |
|  Tone Mapping              <   ACES FILMIC   >  [ACTIVE]   |  ACES FILMIC TONE MAPPING            |
|  CAS Sharpening            [  OFF  | *ON*   ]   [ACTIVE]   |                                      |
|  Sharpen Strength          [====|-------] 35%   [ACTIVE]   |  LAYER:  Host Vulkan (kFilterHlsl)   |
|  Bloom Intensity           [======|-----] 110%  [ACTIVE]   |  APPLY:  Instant (No Restart)        |
|                                                            |  COST:   [■□□□] Negligible (<0.2ms)  |
|  ── LIGHTING & DEPTH ────────────────────────────────────  |                                      |
|  Soft Shadows (ShaderHook) <   PCF 4-TAP     >  [SHADER]   |  DESCRIPTION:                        |
|  Ambient Occlusion (SSAO)  [ LOCKED ]  [PLANNED: PHASE 4]  |  Replaces Xbox overbright framebuffer|
|                                                            |  clipping with cinematic ACES curve  |
|  ── TEXTURES & ASSET PACKS ──────────────────────────────  |  preserving highlight detail in fire, |
|  Anisotropic Filtering     <      16X        >  [ACTIVE]   |  muzzle flashes, and explosions.     |
|  Texture Mip LOD Bias      <     -0.5        >  [ACTIVE]   |                                      |
|  HD Texture Pack Loader    [ LOCKED ]  [PLANNED: PHASE 3]  |  SOURCE BINDING:                     |
|                                                            |  [graphics].tone_mapping = 2         |
+------------------------------------------------------------+--------------------------------------+
|  [!] 1 SETTING PENDING RESTART (Arabic Subtitles)   [RESTORE PRESET]  [RESTART NOW]   [ CLOSE ]   |
+---------------------------------------------------------------------------------------------------+
```

---

## 4. Control Ergonomics & Component Specifications

### 4.1 Touch & Gamepad Unification
Every interactive row obeys strict Android touch accessibility and controller D-pad ergonomics:
- **Minimum Touch Height**: Every interactive row is `>= 48dp` tall (`ImGui::GetFrameHeight()` scaled for DPI) with `8dp` vertical spacing to prevent fat-finger taps.
- **Gamepad Navigation**:
  - `L1 / R1` (LB / RB): Cycle between the **7 Categories**.
  - `Up / Down` (D-Pad / Left Stick): Move focus row-by-row (smoothly skipping headings and auto-updating the Tactical Briefing card).
  - `Left / Right` (D-Pad): Decrement/increment sliders or step segmented toggles.
  - `Y / Triangle`: Cycle Quick Preset (`Original` → `Enhanced` → `Performance`).
  - `X / Square`: Restore defaults for active category.

### 4.2 Interactive Widget Variants
1. **Segmented Toggle (`[ OFF | ON ]`)**: Clear dual-segment pill rather than an ambiguous tiny checkbox; active state lights up `--status-active` Olive/Brass.
2. **Tactile Stepped Slider (`[====|-----] 35%`)**: Supports drag scrubbing via touch, plus flanked `-` / `+` repeat buttons (`>= 44x44px` hitboxes) for exact numeric adjustments (e.g., FOV `45–140`, CAS `0–100%`, Scaling `20–200%`).
3. **Locked Status Card (`[PLANNED]` / `[UNAVAILABLE]`)**:
   - Visually recessed (`--surface-void` fill, `--text-locked` label, padlock or alert glyph).
   - **Focus-inspectable**: Users can still tap or highlight a `Planned` / `Unavailable` row to read **why** it is unavailable in the Tactical Briefing card (e.g., *"Requires guest blood-emitter hook disassembly in Milestone 5"* or *"Disabled: Display does not report HDR10 output"*), without ever pretending to toggle a fake switch.

---

## 5. Right-to-Left (Arabic Mode) Layout Rules

When `[arabic].arabic_ui_enabled` and `[arabic].arabic_rtl_mirror` are active:
- **Horizontal Column Flip**: Settings list moves to the right column (60%) and Tactical Briefing moves to the left column (40%).
- **Row Mirroring**: Setting label aligns flush-right; sliders, arrows (`> value <`), and `[OFF | ON]` toggles align flush-left.
- **Category Tab Ordering**: Tabs order from Right (`1. الرسوميات`) to Left (`7. الواجهة`).
- **BiDi Mixed Numbers**: FPS numbers, percentages (`35%`), and ratios (`16:9`) stay embedded cleanly left-to-right inside Arabic labels via HarfBuzz + FriBidi visual shaping (`app::Ui()`).

---

## 6. Pending Restart & Safety Confirmation Modals

1. **Pending Restart Banner**: Persistent footer strip when `AppRestart` options change (`"1 change requires app restart to load modified assets/language"` + inline **`RESTART NOW`** button calling `kernel::Relaunch`).
2. **Preset Confirmation Guard**: Switching away from a heavily customized setup triggers a brief inline confirmation or saves a one-click **`[presets.last_custom]`** rollback snapshot so no user tuning is lost accidentally.
3. **Incompatible Asset Package Modal**: If a package inside `Black/mods/` fails `mod.toml` compatibility validation (`title_id` mismatch or forbidden `default.xbe` override), the UI marks its row `[INCOMPATIBLE]` and opens a diagnostic card showing the exact offending rule.

