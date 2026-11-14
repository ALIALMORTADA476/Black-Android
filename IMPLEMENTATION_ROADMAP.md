# Black Android — Enhanced Edition Implementation Roadmap (`IMPLEMENTATION_ROADMAP.md`)

> **Goal**: Evolve `ALIALMORTADA476/Black-Android` (`black-enhanced-edition` branch) systematically, starting with high-impact, zero-regression host enhancements and moving into deeper recompiler/asset reverse-engineering only after a verified baseline APK exists.

---

## Stage 0 — Environment Setup & Untouched Baseline Build *(Gatekeeping Prerequisite)*
- **Actions**:
  1. Keep `UpdateChecker.java` (`REPO`) isolated (set to `"ALIALMORTADA476/Black-Android"` or disable startup auto-prompt for dev builds).
  2. Install build dependencies on Windows: Visual Studio 2022 (*Desktop development with C++*), Android Studio (Platform 34, NDK `28.2.13676358`, CMake `3.31.6`, Build-Tools), JDK 17+.
  3. Supply legally obtained **Xbox USA `default.xbe`** (or USA `.iso` in `assets/` or `iso/`).
  4. Run `build.bat` untouched and verify `app-release.apk` runs cleanly on a target Android 10+ arm64 Vulkan device (save, load, audio, touch/controller, options menu).
- **Exit Criteria**: Reproducible baseline APK + working `gradlew.bat assembleRelease` fast incremental rebuilds.

---

## Stage 1 — Quick Wins: Post-Process Graphics & Visual Damage Feedback *(Low Risk, Fast Iteration)*
- **No `config/game.toml` or `xbe_recomp` changes required** (fast Gradle/NDK C++ rebuilds only):
  1. **Damage & Low-Health Visual Feedback**: Read confirmed player health (`kCharHealth = 0x2F0`, `kCharDying = 0x384`, `HudShown()`) in host code; drive a radial crimson damage vignette + hit-flash pulse + low-health desaturation inside `native/vulkan.cpp` (`kFilterHlsl::Frame`).
  2. **Upgraded Graphics Controls**:
     - Upgrade `hdr` to support **ACES Filmic** vs **Reinhard** tone mapping (`kFilterHlsl`).
     - Decouple **CAS Sharpening** (`0%–100%`) from `screen_filter` so it composes cleanly alongside FXAA and color filters.
     - Add **Texture Mip LOD Bias** (`0.0 / -0.5 / -1.0`) in `StageSampler()`.
     - Add **Bloom Intensity** (`Off / Low / Normal / High`) via `GlowComposite` draw scaling.
- **Test Gate**: Visual verification across bright outdoor lighting (*Veblensk*), dark interiors, explosion bloom, low-health vignette behavior (hides cleanly in menus/cutscenes/loading), and zero FPS regression vs baseline.

---

## Stage 2 — ShaderHooks Lighting & Texture Enhancements *(Moderate Complexity, Host-Only)*
- **Target**: Implement a `VulkanShaderHooks : public gpu::ShaderHooks` subclass registered to `gpu::g_shader_hooks` (`XboxXBE-Recomp/runtime/gpu/shaders.h`) inside `native/vulkan.cpp`, and bump `vk_shader.cpp` `kCacheVersion`.
- **Features**:
  1. **PCF Soft Shadows** via `ShaderHooks::Shadow()` override (`Off / 4-tap / 9-tap` toggleable in Graphics menu).
  2. **Optional Hash-Based HD Texture Pack Loader** in `VulkanBackend::BindGuestTexture()` (using existing `tx.sig` hash + loose `Black/textures/` folder check).
  3. *(Optional Stretch)* **Half-Res SSAO**: Use `draw_ds_` depth target + a lightweight pre-HUD phase marker hook so AO only affects 3D world geometry and never darkens HUD/Flash.
- **Test Gate**: Verify Adreno & Mali Vulkan shader compilation (`.spv` cache regeneration, no validation errors or black shadows), frame time budget under 720p/1080p dynamic resolution.

---

## Stage 3 — Arabic Localization Foundation (Host UI, Shaping, BiDi, RTL & HUD Subtitles) *(Moderate Complexity)*
- **Actions**:
  1. Bundle FreeType + HarfBuzz + FriBidi (via `CMakeLists.txt` `FetchContent`) and an Arabic font (`NotoSansArabic-Regular.ttf` in `app/src/main/assets/fonts/`).
  2. Load Arabic font & shaping/BiDi pipeline into Dear ImGui (`native/main.cpp::InitUi()` + `native/vulkan.cpp::InitUi()`).
  3. Add `"ara"` to `src/common/options.h::kLanguages`, extend `src/common/translations.cpp::kEntries[]`, fix `Upper()` so Arabic passes untouched, and add RTL row mirroring to `src/common/menu.cpp` (`MeasureRow`, `RowLabel`, `OptionRow`, `BindRow`).
  4. Build a **Host Arabic Subtitle / Prompt Overlay** inside `VulkanBackend::RenderUi()` fed by `sub_001F3A80` (`src/game/prompts.cpp`) when `BootLanguage() == "ara"`, bypassing the 127-byte HUD buffer limit (`0x12B800`) and stock `"Eurostile LT Std"` font glyph absence.
- **Test Gate**: Check shaped contextual forms (`بـ/ـبـ/ـب/ب`), `لا` ligatures, mixed BiDi strings (`"16x ترشيح..."`, numbers, buttons), RTL menu navigation with touch & controller.

---

## Stage 4 — Deep Reverse-Engineering (`default.xbe` + Game Assets) for Combat Realism & Native Game Arabic *(High Complexity)*
- **Prerequisites**: Extract USA `default.xbe` and disc archives (`MainUS.bin`, `levels/`, `chars/`, `language/`), inspect in Ghidra/IDA Pro.
- **Tracks** (implement one hook at a time, recompile + smoke-test per stage):
  1. **`MainUS.bin` & Subtitle Coverage Audit**: Inspect `MainUS.bin` format (`strings_ind.inc` covers 851 IDs) and confirm how dialogue/FMV subtitles load → produce complete Arabic string table (`strings_ara.inc`).
  2. **Blood & Bullet Impact Pool Hooks**: Reverse-engineer `sub_001E74B0` callers and `0x16DFFB` (`ParticleFade`) / `0x16086D` (`DistanceShorter`) sites to locate particle pool limits, blood spawn counts, and decal ring buffer capacity. Expand pools using `mem::AllocSystem` (following `DrawListRoom`) and add configurable presets (`Blood & Decals: Original / Enhanced`) in `black.toml`.
  3. **Corpse Persistence (Safely Capped)**: Identify corpse timer & character pool limits around `kCharDying` (`0x384`). Only raise persistence with a strict FIFO cap and mission-pool safety checks so scripted reinforcement waves cannot soft-lock.
  4. **In-Engine Flash Arabic Font Injection** *(only if host overlay in Stage 3 is insufficient)*: Reverse-engineer `kFindFont = 0x13CD0` and `kText = 0xD9600`, inject Arabic Presentation Forms-B glyphs into `kFontGlyphs`, fix UTF-8 byte decoding in `fps_counter.cpp`, and mirror Flash menu clips in `options_menu.cpp` / `prompts.cpp`.
- **Test Gate**: Full mission playthrough (*Veblensk Street* through heavy firefights + mission transitions) checking memory leaks, pool exhaustion, wave progression, and shader cache stability.

