# BLACK Enhanced Edition — Stage 5
# Translation Resource Specification & Contributor Workflow

## 1. Overview

The BLACK Enhanced Edition localization architecture uses an external, structured JSON repository for all translated gameplay strings. This ensures:
- Translations remain completely decoupled from the original game ISO (`assets/black.iso`).
- Translations are easily editable and reviewable by translators and community contributors without requiring a C++ compiler.
- Translations are verified against actual in-game contexts before inclusion in release builds.

---

## 2. Directory Layout

All translation assets are stored in the root repository under `translations/`:

```
translations/
├── en/
│   └── gameplay_strings.json    # Canonical English strings with contexts and categories
└── ar/
    └── gameplay_strings.json    # Verified Arabic translations with RTL shaping notes
```

During build compilation, `scripts/build_arabic_translations.py` reads these JSON files and compiles them into `src/game/strings_ara.inc`, a sorted binary search array consumed by `src/game/arabic_strings.cpp`.

---

## 3. JSON Schema Specification

Each translation entry is a JSON object formatted according to the following schema:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "required": ["id", "id_int", "english", "arabic", "context", "category", "status"],
  "properties": {
    "id": {
      "type": "string",
      "pattern": "^0x[0-9A-Fa-f]{8}$",
      "description": "Hexadecimal representation of the 32-bit string identifier"
    },
    "id_int": {
      "type": "integer",
      "minimum": 0,
      "maximum": 4294967295,
      "description": "Unsigned 32-bit integer identifier used by the guest engine"
    },
    "english": {
      "type": "string",
      "description": "Canonical original English string extracted from MainUS.bin"
    },
    "arabic": {
      "type": "string",
      "description": "Modern Standard Arabic translation in standard logical UTF-8"
    },
    "context": {
      "type": "string",
      "description": "In-game location, mission, speaker, or UI context"
    },
    "category": {
      "type": "string",
      "enum": ["objective", "prompt", "intel", "subtitle", "hud"],
      "description": "Visual classification determining host UI layout and colors"
    },
    "status": {
      "type": "string",
      "enum": ["verified", "draft", "deprecated"],
      "description": "Verification status in device gameplay testing"
    }
  }
}
```

### Example Entry:
```json
{
  "id": "0x090F77E3",
  "id_int": 152008675,
  "english": "find the armoury",
  "arabic": "ابحث عن مخزن الأسلحة السري",
  "context": "Mission 1: Veblensk — Objective",
  "category": "objective",
  "status": "verified"
}
```

---

## 4. Categories & Visual Styling Matrix

| Category | UI Destination | Frame Color | Background | Default Display Time |
|---|---|---|---|---|
| `objective` | Top Centered Banner | Tactical Gold (`#D4AF37`) | Charcoal Translucent | 6.0 seconds |
| `intel` | Bottom Box | High-Contrast Cyan (`#40B4E5`) | Semi-transparent Black | 6.5 seconds |
| `prompt` | Bottom Box | Tactical Gold (`#D4AF37`) | Semi-transparent Black | 4.5 seconds |
| `subtitle` | Bottom Box | Pure White / Subtle Blue | Translucent Dark | 5.5 seconds |
| `hud` | Bottom Box | Neutral Gray / Brass | Translucent Dark | 4.0 seconds |

---

## 5. Contributor Workflow: Adding New Translations

1. **Find Unlocalized Strings**:
   Inspect `scripts/all_strings.json` for unused string IDs extracted from `MainUS.bin`.
2. **Add Entry to `scripts/build_arabic_translations.py`**:
   Add a tuple `(hex_id, arabic_text, context, category)` into `translations_db`.
3. **Regenerate Translation Resources**:
   Run the translation compiler:
   ```powershell
   python scripts/build_arabic_translations.py
   ```
   This updates:
   - `translations/en/gameplay_strings.json`
   - `translations/ar/gameplay_strings.json`
   - `src/game/strings_ara.inc`
4. **Run Verification Unit Tests**:
   ```powershell
   cmake --build tests/build
   ./tests/build/test_settings_core.exe
   ```
5. **Compile and Verify on Device**:
   ```powershell
   ./gradlew.bat assembleRelease
   adb install -r app/build/outputs/apk/release/app-release.apk
   ```

