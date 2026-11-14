# Black Android — Repository Audit (`REPOSITORY_AUDIT.md`)

> **Scope & Baseline State**: Read-only architectural audit of fork `ALIALMORTADA476/Black-Android` (upstream `RanggaCasper/Black-Android`), isolated on branch `black-enhanced-edition` in `e:\ai\black`. No upstream code, build scripts, or game binaries were modified during this audit.

---

## 1. Repository & Git Isolation Status

- **Origin remote**: `https://github.com/ALIALMORTADA476/Black-Android.git`
- **Upstream remote**: `https://github.com/RanggaCasper/Black-Android.git` (`pushurl = no_push` configured to block accidental pushes to upstream)
- **Active branch**: `black-enhanced-edition` (75 tracked repository files)
- **Pinned upstream SDK dependency**: [`CMakeLists.txt`](CMakeLists.txt#L24-L25) fetches `TGP482/XboxXBE-Recomp` at commit `99078b119b5d5e0d1d6afb1390e29437c0c984c0` (v1.5.0 baseline), along with:
  - `KhronosGroup/glslang` (`16.0.0`, `ENABLE_HLSL ON`, `ENABLE_OPT OFF`)
  - `marzer/tomlplusplus` (`v3.4.0`)
  - `ocornut/imgui` (`v1.91.0`)
  - `libsdl-org/SDL` (`release-3.4.16`, cloned by Gradle task `fetchSDL` into `third_party/SDL`)

---

## 2. High-Level Four-Layer Architecture

`Black-Android` is **not an Xbox emulator**. It statically recompiles the original Xbox USA `default.xbe` ahead-of-time into C++ (`generated/game/`) and links it with the `XboxXBE-Recomp` portable kernel/XDK runtime, custom game-specific HLE/hook modules, an Android SDL3 + Vulkan 1.1 backend, and an Android Activity wrapper.

| Layer | Responsibility | Key Files |
|---|---|---|
| **1. Android Wrapper (Java / Gradle)** | `SDLActivity` subclass (`BlackActivity`), scoped/all-files storage & `.iso` importer (`GameImporter`), OTA release checker (`UpdateChecker`), manifest/Gradle config | `app/build.gradle`, `app/src/main/AndroidManifest.xml`, `app/src/main/res/values/strings.xml`, `app/src/main/java/com/racas/black/{BlackActivity,GameImporter,UpdateChecker}.java` |
| **2. Native Android Host & Vulkan Renderer** | SDL3 lifecycle, `black.toml` settings (`main.cpp`), XBE discovery & in-memory Europe-to-US patching (`game_files.cpp`), touch controls (`touch.cpp`), gyroscope (`gyro.cpp`), gamepad/HID (`input.cpp`), audio (`audio.cpp`), Dear ImGui UI bridge (`ui.cpp`), D3D8→Vulkan 1.1 + post-process filters (`vulkan.cpp`, `vk_shader.cpp`), dynamic resolution (`dynamic_resolution.cpp`) | `native/{main,host.h,game_files,game_host,ui,input,gyro,touch,audio,update,vk_shader,dynamic_resolution,vulkan}.cpp`, `native/compat/{crt.h,x86intrin.h}` |
| **3. Game Hooks, Custom D3D8 & Sound (`src/`)** | Game options schema (`options.h`, `game_info.cpp`), ImGui/shared option rows (`menu.cpp`), UI translation table (`translations.cpp`), aspect math (`aspect.h`), HLE D3D8 + NEON threaded batching (`d3d8.cpp`, `render_thread.cpp`), DirectSound/mixer (`src/sound/*`), and guest x86 overrides/mid-function hooks (`src/game/*.cpp` + `config/game.toml`) | `config/game.toml`, `src/common/*`, `src/d3d/*`, `src/sound/*`, `src/game/*` |
| **4. Recompiler Tools & Generated Code + Original Assets** | Zydis-based x86→C++ static recompiler (`xbe_recomp`), XDK 5849 signature scanner (`xbe_symbols`), Xbox XDVDFS extractor (`tools/xdvdfs.py`), generated `x86_recomp.0..63.cpp` + `x86_func_mapping.cpp`, plus user-supplied `default.xbe` / disc `.iso` | `tools/{CMakeLists.txt,xdvdfs.py}`, `generated/game/` (generated at build), external `assets/` / `iso/` |

---

## 3. Detailed File & Component Inventory

### 3.1 Build System, Recompiler & CI
- [`build.bat`](build.bat): Windows-only 5-step build orchestrator checking VS 2022 (`vswhere.exe`), Android SDK Platform 34, NDK `28.2.13676358`, CMake `3.31.6`, `git`, and JDK 17+ (`JAVA_HOME`). Extracts `assets/default.xbe` via `tools/xdvdfs.py` if an `.iso` is present, builds `xbe_recomp.exe` and `xbe_symbols.exe` in `out/build/android-tools`, runs `xbe_recomp.exe config/game.toml --xdk <sdk>/xdk` to emit 64 source partitions into `generated/game/`, and calls `gradlew.bat assembleRelease`.
- [`tools/xdvdfs.py`](tools/xdvdfs.py): Scans Xbox XDVDFS volume descriptors (`MICROSOFT*XBOX*MEDIA` at offsets `0`, `0x18300000`, `0xFD90000`, `0x2080000`) to extract `default.xbe`.
- [`CMakeLists.txt`](CMakeLists.txt): Builds `libmain.so` (`arm64-v8a`, `CMAKE_CXX_STANDARD 20`, `-fno-strict-aliasing -fsigned-char`, `-Wl,-z,max-page-size=16384`). Excludes Windows-only SDK modules (`/runtime/app/`, `/core/input.cpp`, `/gpu/(d3d12|render_thread).cpp`, `/audio/xaudio2.cpp`, `/xdk/(dsound|d3d8).cpp`) in favor of `native/` and `src/d3d/` + `src/sound/`. Embeds `src/common/europe.patch` into `.rodata` via `EUROPE_PATCH` macro and generates `image_sha1.cpp` (`kImageSha1`) and `build_info.h` (`BLACK_COMMIT`).
- [`app/build.gradle`](app/build.gradle): `applicationId "com.racas.black"`, `minSdk 29`, `targetSdk 34`, `compileSdk 34`, `ndkVersion "28.2.13676358"`, CMake `3.31.6`, `abiFilters "arm64-v8a"`, Java 17.
- [`.github/workflows/build.yml`](.github/workflows/build.yml): Tag-triggered (`v*`) `windows-2025` workflow decrypting `ci/default.xbe.enc` (AES-256-CBC, PBKDF2 200k iterations) and running `build.bat`.

### 3.2 Android Java Wrapper (`app/src/main/`)
- [`BlackActivity.java`](app/src/main/java/com/racas/black/BlackActivity.java): Wraps `SDLActivity`, creates `getExternalFilesDir(null)/assets` on startup, handles runtime storage permissions (`MANAGE_EXTERNAL_STORAGE` on API 30+ or `READ`/`WRITE_EXTERNAL_STORAGE` on API 29), enumerates removable storage via `cardFolderPaths()`, passes controller fallback JNI events (`nativeLooseKey`, `nativeLooseAxes`, `nativeLooseRelease`), and hosts `UpdateChecker` prompts.
- [`GameImporter.java`](app/src/main/java/com/racas/black/GameImporter.java): Lets the user pick an Xbox `.iso` via `ACTION_OPEN_DOCUMENT`, verifies free space (`size + 64 MiB`), validates `MICROSOFT*XBOX*MEDIA` at sector 32 (`PARTITIONS = {0, 0x18300000L, 0xFD90000L, 0x2080000L}`) when seekable, streams chunks into a `.part` file, `fsync()`s, and atomically renames to `<name>.iso`.
- [`UpdateChecker.java`](app/src/main/java/com/racas/black/UpdateChecker.java): **Fork-isolation note**: Line 23 hardcodes `static final String REPO = "RanggaCasper/Black-Android";`. Enhanced Edition builds should either disable startup checks by default or repoint `REPO` to `ALIALMORTADA476/Black-Android` to prevent upstream APKs from prompting to overwrite custom builds.
- [`AndroidManifest.xml`](app/src/main/AndroidManifest.xml): Sets `sensorLandscape`, `android.hardware.vulkan.version="0x401000"` (`required="true"`), `MANAGE_EXTERNAL_STORAGE`, `android:preferMinimalPostProcessing="true"`. Does **not** declare `android:supportsRtl="true"` (though UI rendering occurs in SDL/Vulkan/ImGui/Flash rather than Android XML Views).

### 3.3 Native Host, Shaders & Vulkan Pipeline (`native/`)
- [`native/main.cpp`](native/main.cpp): Initializes SDL3 (`SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO | SDL_INIT_SENSOR`), loads/saves `black.toml`, creates Vulkan fullscreen window, initializes Dear ImGui (`InitUi()`, lines 299–306 — uses ImGui default embedded ASCII bitmap font; no external TTF/FreeType/Arabic range loaded), calls `FindGame()`, and invokes `kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / "hdd").string())`.
- [`native/game_files.cpp`](native/game_files.cpp): Searches shared storage, removable cards, and app-private `assets/` in priority order (checking loose `default.xbe` before `.iso` images). Patches Europe XBE (`kEuropeSha1 = "796ad8369354e48aa15644d4707dd70bd3bc3b4c"`) in RAM via `kEuropePatch` (`europe.patch`) to match `kImageSha1`. If an `.iso` is selected, mounts it read-only via `kernel::MountDisc()`.
- [`native/vulkan.cpp`](native/vulkan.cpp) (3,417 lines) & [`native/vk_shader.cpp`](native/vk_shader.cpp) (185 lines):
  - `VulkanBackend` implements `gpu::Backend` on Vulkan 1.1 with optional dynamic rendering & timeline semaphores (with render-pass + fence fallbacks for older mobile drivers).
  - Enables `samplerAnisotropy`, `fillModeNonSolid`, `textureCompressionBC` (with CPU `DecodeDxt` fallback if BC hardware support is absent).
  - Uses `glslang` (`vk_shader.cpp`) to compile translated XDK vertex/pixel shader HLSL into SPIR-V 1.3 and caches `.spv` (`kCacheVersion = 2`) plus `VkPipelineCache` (`shadercache/pipelines.bin`).
  - `StageSampler()` (lines 2337–2376) handles per-stage linear/nearest/anisotropic filtering (`app::Get().aniso`, hardware-clamped), LOD clamp/bias, and depth comparison samplers for shadows.
  - `GlowComposite()` (lines 2814–2829) detects additive half-width bloom composite draws to implement the `bloom` toggle.
  - `DrawFrame()` (lines 3155–3254) executes `kBlitHlsl` or `kFilterHlsl` fullscreen passes (HDR Reinhard tone mapping `Tone()`, FXAA `Fxaa()`, SSAA multi-tap `opts.z`, CRT `pscrt`, CAS sharpen `pssharpen`, and 5 color presets `pscolor`: Vibrant, Noir, Sepia, Night Vision, VHS) before calling `RenderUi()` (`ImGui_ImplVulkan_RenderDrawData` + `touch::Draw()`).
  - **Key observation**: `native/vulkan.cpp` does **not** currently subclass or register `XboxXBE-Recomp`'s `gpu::g_shader_hooks` (`runtime/gpu/shaders.h`), leaving `ShaderHooks::Shadow()`, `Fetched()`, `Cube()`, and `Output()` completely untouched and available for host shader enhancements.
- [`native/dynamic_resolution.cpp`](native/dynamic_resolution.cpp): Adjusts `render_percent` between `dynamic_min_percent` and `dynamic_max_percent` (in 5% steps) based on FPS target and `dynamic_hold`.
- [`native/ui.cpp`](native/ui.cpp), [`native/input.cpp`](native/input.cpp), [`native/gyro.cpp`](native/gyro.cpp), [`native/touch.cpp`](native/touch.cpp), [`native/audio.cpp`](native/audio.cpp), [`native/update.cpp`](native/update.cpp): Handle touch-to-ImGui routing, gamepads + MIUI loose-controller fallback, phone/DualSense gyro (`android.toml`), virtual touch controls (`layout.toml`, `skins/buttons.png`), and AAudio stereo/5.1 fold-down.

### 3.4 Game Hooks, UI & Localization Layer (`src/` & `config/game.toml`)
- [`config/game.toml`](config/game.toml): Configures Black (`title_id = 0x45410083`, `entry = 0x25A3F`, `xdk = 5849`, `partitions = 64`), symbol addresses, and 60+ `[[hook]]` entries (`AfterCreateDevice`, `StoreViewport`, `SkipVideo`, `MenuTick`, `ZoomStep`, `FieldOfView`, `FlashSprite`, `GunEffect*`, `PhysicsTime`, `Body*`, `Joint*`, `Shard*`, `ReloadBlur*`, `DistanceShorter*`, `LimitLonger`, `CharacterDetail`, `PropLimit`, `ParticleFade`, `GroundCoverFull`, `DrawListRoom`, `DrawOverlay`, `MouseLook`, `AimAssist`, `HoldAim`, `MenuCursor`, `MenuPrompts`, `OptionsMenu`, `ExitMenu*`, `NameEntry`, `EndImmediate`, `HudItem`, `GameLanguage`).
- [`src/common/options.h`](src/common/options.h), [`src/common/game_info.cpp`](src/common/game_info.cpp), [`src/common/menu.cpp`](src/common/menu.cpp), [`src/common/translations.cpp`](src/common/translations.cpp):
  - `kLanguages = {"int", "fra", "deu", "ita", "esp", "ind"}`.
  - `translations.cpp` translates UI option labels via `app::Ui()` using UTF-16 `kEntries[]` tables. **Audit finding**: `"AMBIENT OCCLUSION"` is already translated in `translations.cpp` line 73 across all 5 non-English languages, even though no Ambient Occlusion setting exists yet in `options.h` or `menu.cpp` (`kGraphicsRows[]`).
- [`src/game/language.cpp`](src/game/language.cpp) & [`src/game/prompts.cpp`](src/game/prompts.cpp):
  - `GameLanguage` patches guest language index `0x2D0D78` (`0=UK, 1=US, 2=Ja, 3=Ge, 4=Fr, 5=Sp, 6=It`) and checks `D:\Language\Strings\Main%s.bin`.
  - `sub_001F3A80` (`prompts.cpp`) intercepts game string lookups by CRC/ID, substitutes `strings_ind.inc` (851 strings from `MainUS.bin`) when `BootLanguage() == "ind"`, rewords controller legends for keyboard/touch, and pads allocated guest buffers to at least `kStringRoom = 128` bytes because HUD message box (`0x12B800`) copies up to 127 bytes unconditionally.
- [`src/game/options_menu.cpp`](src/game/options_menu.cpp) & [`src/game/exit_menu.cpp`](src/game/exit_menu.cpp): Inject custom Recomp settings pages, `CREDITS`, and `QUIT` directly into Flash scenes (`OptionsMenuXbox`, `PauseMenuOptionXbox`, `ControllerSetup`, `BlackTitleMenu*`) by duplicating ActionScript movie clips and writing `$RC_*` variables, measuring glyphs via `"Eurostile LT Std"` (`kFindFont = 0x13CD0`).
- [`src/game/hooks.cpp`](src/game/hooks.cpp), [`src/game/hud.cpp`](src/game/hud.cpp), [`src/game/fps_counter.cpp`](src/game/fps_counter.cpp), [`src/game/esp.cpp`](src/game/esp.cpp), [`src/game/cheats.cpp`](src/game/cheats.cpp), [`src/game/weapons.cpp`](src/game/weapons.cpp):
  - Decouple frame delta time, physics/ragdoll stepping, and glass shard physics (`sub_001690D0`, `0x110` bytes per shard) from 30 FPS.
  - Scale LOD/draw/prop/particle fade distances (`Reach()` up to 8×) and expand draw lists (`DrawListRoom`: `4096 / 0 / 2048 / 512` entries).
  - Reveal confirmed guest memory offsets for `kWorld` (`0x2D45D4`), `kWorldPlayer` (`0x30`), `kCharHealth` (`0x2F0`), `kCharDying` (`0x384`), `kCharKind` (`0xC4`), `kGame` (`0x2D45F0`), `kDifficulty` (`0x2014C`), and `sub_001E74B0` (soldier bullet-hit handler).

---

## 4. Runtime Asset & ISO Architecture vs Build-Time Recompilation

`Black-Android` strictly decouples the distributed APK (`libmain.so` + Java shell + [`app/src/main/assets/skins/buttons.png`](app/src/main/assets/skins/buttons.png)) from copyrighted Xbox game assets. No `.iso`, `default.xbe`, level archives, videos, or audio files are bundled inside `app-release.apk`.

### 4.1 Runtime ISO Selection, Permissions & Discovery Flow
1. **Android Permission & Picker Flow ([`BlackActivity.java`](app/src/main/java/com/racas/black/BlackActivity.java), [`GameImporter.java`](app/src/main/java/com/racas/black/GameImporter.java))**:
   - `BlackActivity.onCreate()` ensures `getExternalFilesDir(null)/assets` exists (`Android/data/com.racas.black/files/assets/` — accessible with zero storage permissions).
   - `hasGame()` checks `own/assets` first, then (only if `hasStorageAccess()` is granted: `Environment.isExternalStorageManager()` on API 30+, `READ/WRITE_EXTERNAL_STORAGE` on API 29) `/storage/emulated/0/Black`, `/storage/emulated/0/Black/assets`, and removable SD/USB volumes (`cardFolders()` / JNI `cardFolderPaths()`).
   - If no game is present, `askForGame()` offers `"Import game"` → `GameImporter.pick()` (`ACTION_OPEN_DOCUMENT`), checks free disk space (`size + 64 MiB`), validates XDVDFS magic (`"MICROSOFT*XBOX*MEDIA"` at sector 32 for `PARTITIONS = {0, 0x18300000L, 0xFD90000L, 0x2080000L}` when seekable), streams chunks into `<name>.part`, `fsync()`s, and atomically renames to `<name>.iso` inside `/storage/emulated/0/Black` (if `hasStorageAccess()`) or `getExternalFilesDir(null)/assets` (scoped app-private fallback).
2. **Native Discovery & Polling ([`native/game_files.cpp`](native/game_files.cpp))**:
   - `FindGame()` calls `WaitForGame()` on startup (polling every ~1 s while processing `SDL_PollEvent` so `BlackActivity`'s importer UI remains responsive).
   - `AssetFolders()` searches directories in strict priority order:
     1. `/storage/emulated/0/Black`
     2. `/storage/emulated/0/Black/assets`
     3. `<removable_volume>/Black` and `<removable_volume>/Black/assets`
     4. `ExeDir() / "assets"` (`SDL_GetAndroidExternalStoragePath()/assets`, falling back to internal storage)
   - Inside each folder, `Search()` checks for an **extracted `default.xbe` on the host filesystem first** (`ReadHostFile(assets / "default.xbe", xbe)` with `disc = nullptr`), and **only if absent** scans regular `.iso` files sorted alphabetically.
   - `consider()` checks whether SHA-1 matches `kImageSha1` (USA `default.xbe`) directly or after in-memory `ConvertEurope()` patching (`kEuropeSha1` `"796ad8369354e48aa15644d4707dd70bd3bc3b4c"` + embedded `kEuropePatch`). Unrecognized releases trigger `AskOtherRelease()` (persisted in `ExeDir() / "other_releases.txt"`).
   - If loaded from `.iso`, `FindGame()` calls `kernel::MountDisc(std::move(disc))` before [`native/main.cpp`](native/main.cpp) boots the kernel via `kernel::Boot(std::move(game.xbe), "default.xbe", game.dir.string(), (app::ExeDir() / "hdd").string())`.

### 4.2 Direct ISO Streaming vs Runtime Extraction (`XboxXBE-Recomp`)
- **No runtime extraction occurs.** All disc assets remain inside the `.iso` on device storage:
  - `Disc::Open()` (`runtime/kernel/disc.cpp`) opens the `.iso` once read-only (`O_RDONLY` on Android), scans `kPartitions = {0, 0x18300000}`, parses the XDVDFS B-tree (`ReadDir()`) into an in-memory directory tree (`Disc::Entry`), and closes nothing until exit.
  - Only `default.xbe` is copied fully into RAM at boot (`disc->Load("default.xbe", xbe)`).
  - Every subsequent asset read during gameplay goes through `Disc::ReadAt()` (`pread()` positional reads, lock-free/thread-safe).
  - `MountDefaults()` (`runtime/kernel/files.cpp`) maps `\??\D:` → `\Device\CdRom0` (`game_dir` / `MountedDisc()`) and `\Device\Harddisk0\Partition1..5` → `ExeDir()/hdd/{E,C,X,Y,Z}`.
- **Partition-offset compatibility quirk**: `tools/xdvdfs.py` (build-time) and `GameImporter.java` (Android import check) accept 4 partition offsets (`0`, `0x18300000`, `0xFD90000`, `0x2080000`), whereas `XboxXBE-Recomp`'s `Disc::Open()` only scans `{0, 0x18300000}`. Unusual dumps (e.g., XGD3 at `0xFD90000` or XGD1 video partition `0x2080000`) can pass `GameImporter` yet fail `Disc::Open()` unless normalized/extracted.

### 4.3 Asset Override Rules & External Mod Packaging Constraints
- **Why loose overrides beside an `.iso` are currently ignored**: In `XboxXBE-Recomp/runtime/kernel/files.cpp` (`Resolve()`, lines 275–276), when `best->first == kCdRom` (`\Device\CdRom0` / `D:\`) and `MountedDisc()` is non-null, `Resolve()` returns `disc->Find(parts)` immediately without falling back to host filesystem files.
- **Preserving original ISO compatibility while packaging external mods**:
  1. **Extracted-folder override (works today without touching kernel code)**: Because `Search()` prioritizes loose `default.xbe` before `.iso`, placing unmodified `default.xbe` + extracted folders (`GlobData.bin`, `chars/`, `data/`, `export/`, `language/`, `levels/`, `sound/`, `videos/`) lets `Resolve()` fall through to `MatchCase(best->second, parts)`.
  2. **Host-side overlay / mod folder (recommended Enhanced Edition extension)**: Adding an optional host override lookup before `MountedDisc()` (e.g., `/storage/emulated/0/Black/mods/` or `ExeDir()/mods/`) plus external host-only resources (Arabic `.ttf`, external translation packs, texture replacement packs, `black.toml`) lets users keep their **untouched original `.iso`** while installing Enhanced Edition asset packs separately from the APK.
- **Which assets can be overridden WITHOUT rebuilding `libmain.so` (`default.xbe` recompile)**:

| Asset / Layer | Location | Safe to Override Without Rebuilding `libmain.so`? | Technical Reason / Limits |
|---|---|---|---|
| **Executable (`default.xbe`)** | Inside `.iso` or `Black/default.xbe` | **No** | Code is statically recompiled ahead-of-time into `generated/game/x86_recomp.0..63.cpp`; `Boot()` and `game_files.cpp` enforce `entry == 0x25A3F` and `kImageSha1`. |
| **Language string banks (`language/Strings/Main*.bin`)** | `D:\language\Strings\` | **Partially / Format-bound** | Loaded dynamically from `D:`, so compatible text edits within existing ID/CRC keys and `0x12B800` buffer limits work; **cannot** add new glyphs (Arabic), RTL shaping, or wider UI layouts without native/Flash/hook code in `libmain.so`. |
| **FMV videos (`videos/*.bik` / disc videos)** | `D:\videos\` | **Yes (format-compatible)** | Streamed externally from `D:` at runtime; not hashed into `kImageSha1`. |
| **Audio streams & banks (`sound/`)** | `D:\sound\` | **Conditional (format/table match)** | Streamed externally from `D:`, provided sample formats/headers/bank indices match what compiled guest code expects. |
| **Level, character, export & global data (`GlobData.bin`, `chars/`, `data/`, `export/`, `levels/`)** | `D:\` | **Conditional (schema/offset match)** | Not baked into `libmain.so`, so compatible asset replacements (same formats, shaders/tables, valid memory sizes) can be swapped externally, whereas structural format changes or custom particles/decals require matching guest code/hooks. |
| **Host configs & future external packs (`black.toml`, `android.toml`, `layout.toml`, `fonts/`, `mods/`)** | `ExeDir()` / `Black/` | **Yes** | Read directly by `native/` C++ code on the Android host outside `D:\` (note: `skins/buttons.png` currently loads from APK assets unless `touch::LoadSkin()` is extended to check external paths first). |

---

## 5. Compatibility & Fork Hygiene Notes

1. **USA vs Europe XBE Handling**: Build-time recompilation **must** target USA `default.xbe` (`kImageSha1` mismatch triggers runtime prompt). Europe discs are supported cleanly at runtime via in-memory patching (`europe.patch` in `native/game_files.cpp`), so any new `[[hook]]` addresses added to `config/game.toml` must match the USA `default.xbe` VA layout.
2. **Fork Update Isolation**: Update `UpdateChecker.java` (`REPO`) before distributing personal builds so upstream releases never overwrite `black-enhanced-edition` APKs.
3. **Host vs Guest Coupling**: Modifications implemented in `native/vulkan.cpp`, `src/common/`, or `XboxXBE-Recomp`'s `ShaderHooks` carry zero recompiler risk and fast incremental NDK rebuilds, whereas changes to guest combat/blood/decal/corpse/Flash internals require `default.xbe` disassembly, `config/game.toml` hooks, and 64-partition recompilation (`generated/game/`).

