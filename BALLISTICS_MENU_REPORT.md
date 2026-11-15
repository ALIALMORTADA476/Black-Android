# Ballistics & Tracers Mod Menu Report (`BALLISTICS_MENU_REPORT.md`)

**Project:** BLACK Enhanced Edition  
**Repository:** `E:\ai\black` (Branch: `black-enhanced-edition`)

---

## 1. Schema Registration & TOML Persistence (`mod_schema.h` / `mod_schema.cpp`)

Added `Category::Ballistics` and 10 validated, immediately applied (`ApplyScope::Immediate`) settings persisted under the `[ballistics]` TOML table:

| SettingId | Key | Default (`Enhanced`) | Validation / Bounds |
|---|---|---|---|
| `BallisticsPreset` | `ballistics_preset` | `"enhanced"` | `"original"`, `"performance"`, `"enhanced"`, `"custom"` |
| `BallisticsTracersEnabled` | `tracers_enabled` | `true` | Boolean |
| `BallisticsTracerFrequency` | `tracer_frequency` | `1` (Every Bullet) | `0..5` (`Off`, `Every`, `2nd`, `3rd`, `5th`, `Selective`) |
| `BallisticsTracerTrailLength` | `tracer_trail_length` | `115` | `25 .. 250%` |
| `BallisticsTracerBrightness` | `tracer_brightness` | `110` | `20 .. 200%` |
| `BallisticsTracerColor` | `tracer_color` | `0` (Weapon Realistic) | `0..4` (`Weapon Realistic`, `NATO Gold`, `Crimson Red`, `Phosphor Green`, `Arctic White`) |
| `BallisticsSmokeTrailsEnabled` | `smoke_trails_enabled` | `true` | Boolean |
| `BallisticsSmokeMode` | `smoke_mode` | `2` (Medium) | `0..3` (`No Smoke`, `Light`, `Medium`, `Enhanced`) |
| `BallisticsSmokeIntensity` | `smoke_intensity` | `105` | `10 .. 200%` |
| `BallisticsSmokeDuration` | `smoke_duration` | `1.10f` | `0.20 .. 3.00s` |

---

## 2. Presets & Restore Original (`SettingsRegistry::ApplyBallisticsPreset`)

- **Performance Mode (`"performance"`):** Enables tracers (`Every 3rd Bullet`, length `95%`, brightness `100%`) and `Light` smoke (`70%` intensity, `0.65s` duration), and reduces active pool caps to `32` tracers / `64` smoke nodes.
- **Enhanced Mode (`"enhanced"`):** Enables `Every Bullet` tracers (`115%` length, `110%` brightness, weapon-realistic colors) and `Medium` smoke (`105%` intensity, `1.10s` duration) at full `96` / `192` capacities.
- **Restore Original Bullet Effects (`"original"`):** Disables enhanced tracers (`tracers_enabled = false`, `tracer_frequency = 0`) and smoke trails (`smoke_trails_enabled = false`, `smoke_mode = 0`), restoring native vanilla projectile rendering without affecting any other Mod Menu category.

---

## 3. Mod Menu Tab & Full Arabic Localization (`menu.h`, `menu.cpp`, `translations.cpp`)

- Added `TacticalCategory::BallisticsTracers` (`[4] BALLISTICS & TRACERS` / `"المقذوفات والمسارات"`) inside `menu.cpp` (`kCategories[]`, `DrawCategoryContent()`, `DrawCategoryBallisticsTracers()`).
- Includes:
  - 2-column preset bar (`PERFORMANCE MODE` | `ENHANCED MODE`) plus a full-width crimson `RESTORE ORIGINAL BULLET EFFECTS` button below it (preventing RTL text clipping on mobile viewports) with right-aligned active preset status readout.
  - Full tracer & smoke interactive step controls with RTL layout support.
  - Live Ballistics Telemetry readout (`Tracers`, `Smoke`, `Shots`, `Emitted`, `Hitscan`, `Culled`).
- Added 31 Arabic translations in `src/common/translations.cpp` (`عربي RTL`) and verified every key via automated Python & C++ unit tests (`TestBallisticsSchemaAndPresets`, `TestBallisticsWeaponClassAndFrequency`, `test_13_ballistics_tracers_and_arabic`).
