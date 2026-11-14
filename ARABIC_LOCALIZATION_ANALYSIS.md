# Black Android — Arabic Localization Feasibility & Technical Analysis (`ARABIC_LOCALIZATION_ANALYSIS.md`)

---

## 1. Executive Summary & Text Rendering Topology

`Black-Android` currently renders text through **three separate subsystems**, none of which presently support Arabic shaping, bidirectional text ordering (BiDi), right-to-left (RTL) mirroring, or Arabic Unicode glyphs:

1. **Dear ImGui Host Overlay (`native/main.cpp`, `native/vulkan.cpp`, `src/common/menu.cpp`, `src/common/translations.cpp`)**: Used for the touch-gear Settings/Cheats panel, touch layout overlay (`native/touch.cpp`), and error/system prompts. Currently relies exclusively on ImGui's embedded default Latin font (`AddFont*` is never called in `InitUi()`), has no FreeType/HarfBuzz/FriBidi integration, and draws LTR rows (`RowLabel` left, controls right).
2. **Recompiled Game Flash UI (`src/game/options_menu.cpp`, `src/game/exit_menu.cpp`, `src/game/prompts.cpp`, `src/game/language.cpp`, `src/game/flash.h`, `src/game/layout.h`)**: All main menus, pause/options pages, mission briefings, and prompt overlays run through Black's embedded Flash player using the game's `"Eurostile LT Std"` font (`kFindFont = 0x13CD0`, `"Eurostile LT Std"` at `0x28E63C`) and a 128-bucket glyph hash table (`kFontGlyphs`, `code & 0x7F`). Text is pulled via `sub_001F3A80(id)` (UTF-8 output in `ctx.eax`).
3. **Game 2D Overlay / HUD Text Layer (`src/game/fps_counter.cpp`, `src/game/hud.cpp`)**: Renders FPS counter, watermark, and in-game 2D HUD text via `kText = 0xD9600(font, wide_chars, x, y, size, colour)` using font `kFont = 0x2F433C`.

---

## 2. How Current Localization Works (`ind` / Indonesian Pattern)

The port already proves that **non-disc languages can be added without modifying original `.iso` assets**, thanks to the Indonesian (`"ind"`) implementation:

| Component | Source File & Symbols | Current Behavior |
|---|---|---|
| Language Registry | [`src/common/options.h`](src/common/options.h#L12) | `kLanguages[] = {"int", "fra", "deu", "ita", "esp", "ind"}` |
| Host Phrase Dictionary | [`src/common/translations.cpp`](src/common/translations.cpp#L20-L292) | `kEntries[]` maps English phrases to `char16_t` arrays `[fra, deu, ita, esp, ind]`. `app::Ui(text, caps)` performs longest-phrase matching, calls `Upper()` (ASCII + Latin-1 `0xE0..0xFE`), and converts BMP UTF-16 to UTF-8 via `AppendUtf8()`. |
| Disc Language Hook | [`src/game/language.cpp`](src/game/language.cpp#L19-L115) | `GameLanguage` (`@ 0x1F8120`, `0x1F813B`) sets guest language index `0x2D0D78` (`0=UK, 1=US, 2=Ja, 3=Ge, 4=Fr, 5=Sp, 6=It`), probes `D:\Language\Strings\Main%s.bin`, and provides binary search `IndonesianString(uint32_t id)` over 851 ID-sorted entries in [`src/game/strings_ind.inc`](src/game/strings_ind.inc). |
| String Lookup Interception | [`src/game/prompts.cpp`](src/game/prompts.cpp#L50-L176) | `X86_FUNC(sub_001F3A80)` intercepts every string lookup by 32-bit ID, swaps `ctx.eax` to guest-allocated UTF-8 copies (`IndonesianString(id)`), and pads buffers to at least `kStringRoom = 128` bytes because HUD message box `0x12B800` blindly copies 127 bytes. |
| Save Profile Isolation | [`src/game/save_name.cpp`](src/game/save_name.cpp#L21-L99) | `SaveName(0x7D0DDAB7)` (`FE_MCSAVE_PROFILE`) prevents language switches from breaking save lookup (`SaveMeta.xbx` in `hdd/E/UDATA/45410083`). |

**Why Indonesian works easily while Arabic breaks**: Indonesian uses standard ASCII/Latin characters already present in `"Eurostile LT Std"`, flows LTR, has no contextual letter joining, and requires no glyph replacement.

---

## 3. Feature-by-Feature Technical Investigation

### 3.1 Arabic UI Translation (`black.toml`, Host Menus & Flash Options)
- **Layer**: Native Host (`native/`) + Port Common (`src/common/`) + Port Game Hooks (`src/game/`).
- **Files & Functions**:
  - `src/common/options.h` (`kLanguages`)
  - `src/common/menu.cpp` (`kLanguageNames`, `StepInputRow` `kLanguage` case)
  - `src/common/translations.cpp` (`kEntries[]`, `Upper()`, `app::Ui()`)
  - `src/game/language.cpp` (`LanguageAvailable()`, `Translated()`, new `ArabicString(uint32_t id)`)
  - `src/game/prompts.cpp` (`sub_001F3A80`, `kButtons[]`)
- **Critical Bug to Fix in `translations.cpp`**: `app::Ui(text, caps=true)` passes every code unit through `Upper()`, and Flash menus/settings default to `caps = true`. While Arabic has no uppercase/lowercase, `kEntries` lookup, word-boundary splitting (`WordChar()`), and dynamic tokens (`"%u%% (%ux%u)"`, `"PRESS %s :"`, `"%s (RESTART)"`) require Arabic-aware template formatting rather than naïve ASCII concatenation.
- **Feasibility**:
  - **Host ImGui Settings Panel**: **Moderate** (straightforward dictionary extension once Arabic font/shaping/RTL are added to ImGui).
  - **In-Game Flash Menus**: **Difficult** (blocked by Flash font glyphs & RTL layout — see §3.2–3.4).

### 3.2 Arabic Font & Glyph Support
- **Layer**: Native Host (ImGui/Vulkan) + Recompiled Game Font System (`default.xbe` + disc font assets) OR Host Overlay Bypass.
- **Observed Mechanism**:
  - **ImGui (`native/main.cpp::InitUi`, `native/vulkan.cpp::InitUi`)**: Currently has zero TTF font loading (`ImGui::GetIO().Fonts->AddFontDefault()` implicitly). Adding an Arabic TTF (e.g., *Noto Sans Arabic* or *Cairo* bundled under `app/src/main/assets/fonts/`) with `ImFontConfig` + `ImGui::GetIO().Fonts->GetGlyphRangesArabic()` (covering `0x0600..0x06FF` and Presentation Forms A/B `0xFB50..0xFDFF`, `0xFE70..0xFEFF`, or using `imgui_freetype`) gives crisp Arabic glyph rendering in Vulkan/ImGui.
  - **Game `"Eurostile LT Std"` Font (`src/game/layout.h`, `src/game/fps_counter.cpp`, `src/game/options_menu.cpp`)**:
    - Lookups resolve via `Glyph(font, code)` hashing `(code & 0x7F)` across `kFontGlyphs` buckets (`kGlyphSize = 0x28`, `kGlyphCode` is `uint16_t`, with `kGlyphWidth`, `kGlyphLeft`, `kGlyphAdvance`).
    - **Unconfirmed**: How `"Eurostile LT Std"` bitmap/texture pages and glyph descriptor tables are packed inside the game archives (`GlobData.bin` / Flash `.swf` / font assets), and whether the disc font texture atlas has spare UV space or supports dynamic glyph table expansion in guest memory (`mem::AllocSystem`).
    - **Confirmed Bug in `fps_counter.cpp`**: Lines 72 and 82 iterate raw `uint8_t(*c)` bytes rather than decoding UTF-8 codepoints before casting to `uint16_t` for `kText = 0xD9600`.
- **Implementation Paths**:
  - *Path A (Host Overlay / Subtitles & Native Menu)*: Render Arabic subtitles/prompts/settings in a native Vulkan/ImGui HUD layer (using FreeType + Noto Sans Arabic) when `BootLanguage() == "ara"`, leaving untouched Flash assets fallback-safe.
  - *Path B (In-Engine Font Injection — Unconfirmed internals)*: Disassemble `kFindFont = 0x13CD0` and `kText = 0xD9600` to construct a runtime Arabic glyph atlas texture (`U+FE70..U+FEFF` Presentation Forms-B) and splice expanded `kFontGlyphs` buckets in guest memory.
- **Feasibility**: **Moderate** (Host ImGui / Native HUD Overlay) / **Difficult & Unconfirmed** (Modifying `Eurostile LT Std` inside game memory/assets).

### 3.3 Arabic Text Shaping & Bidirectional (BiDi) Rendering
- **Layer**: Native Host (`CMakeLists.txt` + `native/` + `src/common/`).
- **Technical Problem**:
  - Arabic letters change shape depending on position (*Isolated, Initial, Medial, Final*) plus mandatory ligatures (*Lam-Alef* `لا/لأ/لإ/لآ`). Neither Dear ImGui `v1.91.0` nor Black's Flash/2D text engine performs OpenType GSUB/GPOS shaping or Unicode UAX #9 BiDi reordering.
  - Mixed strings like `"16x ترشيح متباين الخواص"` or `"اضغط ENTER للمتابعة"` or `"720p (1280x720)"` will render backwards and disconnected if fed directly to an unshaped LTR renderer.
- **Implementation Approach**:
  - **In ImGui (`menu.cpp`, native HUD)**: Fetch/link **HarfBuzz** (shaping) + **Shepherd/libunibreak or FriBidi** (UAX #9 BiDi visual reordering) via `CMakeLists.txt` `FetchContent` + `imgui_freetype` (or pre-shape static UI labels cleanly and run lightweight runtime shaping + UAX #9 BiDi for dynamic strings like resolution/FPS/key names). Keep `app::Ui()` cache keyed by text + flags so per-frame overhead is near zero after first lookup.
  - **In Game Engine (`sub_001F3A80`)**: If feeding pre-shaped visual strings into an engine font that has Presentation Forms-B glyphs (`U+FE70..U+FEFF`), FriBidi + HarfBuzz (or Arabic reshaper converting logical UTF-8 → Presentation Forms-B in visual order) must run **at string assembly time** (after `PRESS %s :` or `%1 من %2` substitution), watching out for Flash multi-line word wrapping (which wraps left-to-right and reverses line order if a single logical line is reversed blindly without width-aware line breaking!).
- **Feasibility**: **Moderate** (ImGui/Host) / **Difficult** (Game Flash multi-line text fields).

### 3.4 Right-to-Left (RTL) Layout Mirroring
- **Layer**: Native Host (`src/common/menu.cpp`), Android Manifest (`AndroidManifest.xml`), and Flash Scene Hooks (`src/game/options_menu.cpp`, `src/game/prompts.cpp`, `src/game/hud.cpp`).
- **Files & Functions**:
  - [`src/common/menu.cpp`](src/common/menu.cpp#L174-L231): `MeasureRow()`, `RowLabel()`, `OptionRow()`, `BindRow()` hardcode LTR coordinates (`box.row_x` for label on the left, `block_x` / `ArrowButton` / `value` / key boxes pinned to the right edge). Mirroring when `rtl == true` puts `RowLabel` right-aligned at `box.row_x + box.row_w - label_w` and flips arrow buttons + `< >` direction semantics if desired.
  - [`src/game/options_menu.cpp`](src/game/options_menu.cpp#L98-L228): `Host::name_x` (`-37.0f`), `Host::value_x` (`185.0f`), `kTickGap`, `kBarLeft` hardcode Flash clip X coordinates (`flash::kClipX`) and text alignment inside ActionScript movie clips.
  - [`src/game/prompts.cpp`](src/game/prompts.cpp#L109-L128): `Prompts()` offsets `Text` clip relative to `Button` icon by `kIconWidth = 28.0f` rightward.
  - Stock Flash menus (`BlackTitleMenu`, `OptionsMenuXbox`, Pause Menu): Positions and slide animations (`Main.TextAnim`) are authored in `.swf` timelines (`Main*.bin`/Flash resources).
- **Feasibility**: **Moderate** (ImGui `menu.cpp` + `options_menu.cpp` Recomp rows) / **Difficult** (mirroring stock Flash `.swf` button layouts without visual overlap).

### 3.5 Mission, Briefing & Dialogue / Subtitle Translation
- **Layer**: Game Assets (`MainUS.bin` / `.xbe` / FMVs) + Hook (`src/game/prompts.cpp` / `src/game/language.cpp`).
- **Confirmed Evidence**:
  - [`src/game/strings_ind.inc`](src/game/strings_ind.inc) shows 851 ID-mapped strings extracted from `MainUS.bin`, including objective updates (`"pembaruan misi utama"`), Black Intel pickup descriptions, HUD hints, and menu strings (`0x00422B61` `"penghancuran %1 dari %2"` etc.).
  - [`src/game/prompts.cpp`](src/game/prompts.cpp#L50-L62) notes that guest function `0x12B800` (HUD message box) unconditionally copies **127 bytes**. Because Arabic UTF-8 encodes 2 bytes per codepoint (3 bytes for Presentation Forms-B `U+FE70..U+FEFF`!), long translated mission briefings/subtitles could truncate mid-codepoint around ~42–63 Arabic characters unless `0x12B800` is hooked or replaced with a larger buffer/native subtitle drawer!
- **Unconfirmed Aspects**:
  - Whether **all** spoken dialogue subtitles and live-action FMV cutscenes route through `MainUS.bin` and `sub_001F3A80`, or whether some Bink videos (`videos/`, [`src/game/hooks.cpp`](src/game/hooks.cpp#L640-L669)) rely on separate subtitle streams or burn-in assets. Inspecting the user's legally extracted `MainUS.bin` and `language/` folder is required to confirm 100% coverage.
- **Feasibility**: **Moderate** (for IDs already routed through `sub_001F3A80`, once Arabic rendering + buffer limit are solved) / **Unconfirmed** (full FMV cutscene subtitle coverage).

---

## 4. Compatibility Risks & Performance Costs

| Risk / Cost | Impact | Mitigation |
|---|---|---|
| **HUD 127-byte buffer truncation (`0x12B800`)** | High (corrupt UTF-8 tail or clipped Arabic objectives) | Render game Arabic subtitles/notifications via host overlay or hook `0x12B800` buffer copy limit. |
| **ActionScript / Flash text wrapping breaks RTL** | Medium (inverted line order on multi-line briefings) | Shape & wrap lines manually against actual pixel width before outputting visual lines, or use native ImGui/FreeType HUD overlay. |
| **Game font hash collisions (`code & 0x7F`)** | Medium (if injecting glyphs into guest `kFontGlyphs`) | `Glyph()` walks linked entries in each bucket until `kGlyphCode == code` or `kFontLastGlyph`, so adding glyphs works structurally but requires correct atlas UV/glyph array layout (**Unconfirmed**). |
| **Mobile GPU/CPU Cost (HarfBuzz + FriBidi)** | Negligible (< 0.1 ms with string cache) | Cache shaped outputs in `app::Ui()` / `g_arabic_cache` keyed by `(string_id, text_hash, max_width)`. |
| **APK Size Increase** | Low (~300 KB – 1.5 MB) | Bundle one compressed Arabic `.ttf` (e.g., Noto Sans Arabic Regular/Bold) + statically link HarfBuzz/FriBidi subset. |

---

## 5. Recommended Arabic Architecture Strategy

1. **Stage 1 (Low-Risk Host Foundation)**: Integrate `freetype` + `harfbuzz` + `fribidi` into `CMakeLists.txt`, load an Arabic font into ImGui, mirror `src/common/menu.cpp` (`RowLabel`, `OptionRow`, `BindRow`), add `"ara"` to `kLanguages` / `translations.cpp`, and build a clean host-rendered RTL **HUD Subtitle & Notification Overlay** in `VulkanBackend::RenderUi()` (gated by `hud::HudShown()`).
2. **Stage 2 (Asset Audit & String Extraction)**: Once the user supplies `MainUS.bin` (from their Xbox disc), inspect its binary format, confirm whether FMV subtitles live inside `MainUS.bin` or separate files, and translate the 851+ verified string IDs.
3. **Stage 3 (Deep Engine Integration — Optional)**: Decide based on Stage 2 whether to intercept `0xD9600` (`kText`) + Flash dynamic text rendering or patch `"Eurostile LT Std"` font tables in memory.

