# Black Android — Baseline Build & Verification Guide (`BUILD_GUIDE.md`)

> **Important Pre-Check (Current Machine Status)**: As of this audit, no Xbox ISO or `default.xbe` exists under `e:\ai\black`, and this Windows host lacks the required Android/C++ build dependencies (`vswhere.exe` / Visual Studio 2022 missing; no Android SDK Platform 34, NDK `28.2.13676358`, CMake `3.31.6`, or JDK 17–21 installed; only Python `3.12.10`, Git/`gh`, and JRE `1.8.0_121` are present). A baseline APK build therefore **could not be executed locally yet**. The procedure below documents the exact verified pipeline from [`build.bat`](build.bat), [`CMakeLists.txt`](CMakeLists.txt), [`app/build.gradle`](app/build.gradle), [`tools/xdvdfs.py`](tools/xdvdfs.py), and [`.github/workflows/build.yml`](.github/workflows/build.yml).

---

## 1. Mandatory Requirements

### 1.1 Legally Obtained Game Assets
- **Required release**: Original **Xbox** release of *Black* (**USA** recommended for building, or **Europe** for runtime play on Android once built from USA `default.xbe`).
- **Do NOT use PS2 images**: This project statically recompiles Xbox x86 code (`default.xbe`) and Xbox XDK 5849 calls (`D3D8`, `DSound`, `XInput`, `XNet`); PlayStation 2 ELFs/ISOs are completely incompatible.
- **Asset placement for building** (picked up automatically by `build.bat` lines 47–54):
  - Option A: Place the extracted USA `default.xbe` at `e:\ai\black\assets\default.xbe`.
  - Option B: Place your unmodified USA `.iso` inside `e:\ai\black\assets\*.iso` or `e:\ai\black\iso\*.iso`. `build.bat` invokes `tools/xdvdfs.py` in read-only mode to extract `default.xbe` into `assets\default.xbe` without altering the `.iso`.
  - **Strict safety rule**: Never overwrite or modify the original `.iso` or `default.xbe`. Notice `.gitignore` keeps `assets/`, `iso/`, `generated/`, `out/`, and `third_party/` out of version control so copyrighted binaries are never committed.

### 1.2 Windows Build Toolchain Prerequisites
Checked in exact order by [`build.bat`](build.bat#L12-L44):
1. **Visual Studio 2022** with *Desktop development with C++* (`Microsoft.VisualStudio.Component.VC.Tools.x86.x64`, located via `%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe`). Used to compile Windows host tools (`xbe_recomp.exe`, `xbe_symbols.exe`) using `clang-cl.exe` (VS Clang or NDK's `clang-cl.exe`) and `Ninja`.
2. **Android Studio / Android SDK** (`%ANDROID_HOME%` or `%LOCALAPPDATA%\Android\Sdk`) containing:
   - `platforms;android-34` (Android SDK Platform 34)
   - `ndk;28.2.13676358` (NDK `28.2.13676358`)
   - `cmake;3.31.6` (CMake `3.31.6` — note CI also bundles Ninja inside SDK CMake)
   - `build-tools;34.0.0` (recommended for `apksigner` / `adb`)
3. **JDK 17 or newer** (`JAVA_HOME` pointing to Android Studio JBR or JDK 17–21; [`app/build.gradle`](app/build.gradle#L86-L89) targets `JavaVersion.VERSION_17`).
4. **Git** (`where git`) — needed for Gradle `fetchSDL` (`release-3.4.16`) and CMake `FetchContent` (`XboxXBE-Recomp` `99078b119b5d5e0d1d6afb1390e29437c0c984c0`, `glslang` `16.0.0`, `tomlplusplus` `v3.4.0`, `imgui` `v1.91.0`).
5. **Python 3.x** (`where python`) — required only when extracting `default.xbe` from an `.iso` via [`tools/xdvdfs.py`](tools/xdvdfs.py).

---

## 2. Build-Time XBE Extraction vs Runtime ISO Loading

A critical architectural boundary separates **building the APK on PC** from **running the game on Android**:

| Aspect | Build-Time (`build.bat` + `CMakeLists.txt`) | Runtime (APK on Android Device) |
|---|---|---|
| **Purpose** | Extract Xbox executable (`default.xbe`) so `xbe_recomp.exe` can translate guest x86 machine code into C++ compiled into `libmain.so`. | Locate user-supplied `.iso` or extracted game directory, verify `default.xbe` SHA-1 against `kImageSha1`, and stream assets (`D:\`) directly from user storage. |
| **Game Assets in APK** | **None.** Only [`app/src/main/assets/skins/buttons.png`](app/src/main/assets/skins/buttons.png) and compiled native binaries (`libmain.so`, etc.) are packaged in `app-release.apk`. | Loaded externally from `/storage/emulated/0/Black`, removable storage `Black/`, or `Android/data/com.racas.black/files/assets/`. |
| **ISO Tooling** | [`tools/xdvdfs.py`](tools/xdvdfs.py) reads 4 Xbox partition offsets (`0`, `0x18300000`, `0xFD90000`, `0x2080000`) to extract `assets/default.xbe` on Windows. | [`GameImporter.java`](app/src/main/java/com/racas/black/GameImporter.java) copies `.iso` into storage; `XboxXBE-Recomp` `Disc::Open()` opens `.iso` read-only (`O_RDONLY`, partition offsets `{0, 0x18300000}`) and reads on demand with `pread()` (**no runtime extraction**). |

### Build-Time Recompilation Steps

1. **ISO → XBE Extraction (`tools/xdvdfs.py`)**:
   - Opens the `.iso` in binary read-only mode (`open(iso_path, "rb")`), searches standard Xbox partition offsets (`0`, `0x18300000`, `0xFD90000`, `0x2080000`) for the `"MICROSOFT*XBOX*MEDIA"` header at sector 32 (`+ 32 * 2048`), traverses the root B-tree directory table, and writes **only** `default.xbe` into `assets\default.xbe` (other disc assets are not copied into the APK).
2. **Host Recompiler Build (`tools/CMakeLists.txt` → `out/build/android-tools`)**:
   - Fetches `XboxXBE-Recomp` (or reuses a sibling `../XboxXBE-Recomp` or local `sdk/` checkout) and builds two Windows CLI executables linked against `Zydis v4.1.1` and `toml++ v3.4.0`:
     - `xbe_symbols.exe`: XDK 5849 signature scanner (only run by `build.bat` if `[symbols]` is absent in `config\game.toml` — it is already populated in this repo).
     - `xbe_recomp.exe`: Decodes `.xbe` sections configured in [`config/game.toml`](config/game.toml) (`code_sections = [".text", "DSOUND", "WMADEC", "XMV", "XGRPH", "D3D", "XPP"]`, `entry = 0x25A3F`), injects every `[[hook]]` stub and `X86_FUNC(sub_XXXXXXXX)` / `X86_EXTERN_FUNC` call boundary, and emits 64 C++ translation units (`x86_recomp.0.cpp` .. `x86_recomp.63.cpp`), `x86_func_mapping.cpp`, `x86_recomp_shared.h`, and `image_symbols.h` into `generated/game/`.
3. **Android Cross-Compilation & Packaging (`app/build.gradle` + `CMakeLists.txt`)**:
   - Gradle `preBuild` clones `SDL` (`release-3.4.16`) into `third_party/SDL` (unless already present).
   - CMake verifies `generated/game/x86_func_mapping.cpp` and `assets/default.xbe` exist, computes SHA-1 of `assets/default.xbe` into `image_sha1.cpp` (`kImageSha1`), embeds `src/common/europe.patch` into `game_files.cpp`, compiles the recompiled partitions with `-O2 -w -fno-strict-aliasing -fsigned-char`, links `SDL3`, `glslang`, `toml++`, `imgui`, `vulkan`, `android`, `log` into `libmain.so` (`-Wl,-z,max-page-size=16384`), and packages `app/build/outputs/apk/release/app-release.apk` (signed with `BLACK_KEYSTORE*` env vars if defined, otherwise debug-signed so it installs directly).

---

## 3. Baseline Build Procedure (Step-by-Step)

```bat
REM From Developer / standard Windows Command Prompt inside e:\ai\black:
REM 1. Ensure you are on the isolated fork branch:
git checkout black-enhanced-edition

REM 2. Place your legally obtained USA default.xbe in assets\ (or USA .iso in assets\ or iso\)
REM    NEVER commit or modify the ISO/XBE.

REM 3. Set JAVA_HOME and ANDROID_HOME if not in default locations:
set ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk
set JAVA_HOME=C:\Program Files\Android\Android Studio\jbr

REM 4. Run the baseline build (first build takes ~30-90 minutes compiling 64 partitions):
build.bat
```

Optional faster iterative rebuilds (after `generated/game` is already generated and when only editing `native/` or `src/` C++ files, **without** changing `config/game.toml` or `xbe` code):
```bat
gradlew.bat assembleRelease
```
Note: Adding or changing `[[hook]]` entries or `X86_FUNC(sub_...)` signatures in `config/game.toml` / `src/game/` requires re-running `xbe_recomp.exe` (`build.bat` Step 4). Adding pure host files in `native/` or modifying `src/` C++ bodies of existing hooks only needs `gradlew.bat assembleRelease`.

---

## 4. Baseline APK Verification & Testing Procedure

1. **Install APK on an Android 10+ arm64 Vulkan 1.1+ device**:
   ```bat
   adb install -r app\build\outputs\apk\release\app-release.apk
   ```
2. **Deploy Runtime Game Assets**:
   - Grant **All files access** (`MANAGE_EXTERNAL_STORAGE`) when prompted on launch (or Android 10 storage permission), or let the in-app **Import game** picker copy your `.iso` into scoped storage (`Android/data/com.racas.black/files/assets/`) with no special permissions required.
   - Runtime search priority in [`native/game_files.cpp`](native/game_files.cpp#L93-L100):
     1. `/storage/emulated/0/Black` and `/storage/emulated/0/Black/assets`
     2. `<removable_volume>/Black` and `<removable_volume>/Black/assets`
     3. `Android/data/com.racas.black/files/assets` (`ExeDir() / "assets"`)
   - Within each folder, loose `default.xbe` + extracted folders (`GlobData.bin`, `chars`, `data`, `export`, `language`, `levels`, `sound`, `videos`) take precedence over `.iso` images. When using an `.iso`, assets are streamed directly in place via `pread()` without unpacking anything onto phone storage.
   - If supplying the Europe release, [`native/game_files.cpp`](native/game_files.cpp#L321-L327) matches `kEuropeSha1` (`796ad8369354e48aa15644d4707dd70bd3bc3b4c`) and applies `europe.patch` in memory so it matches `kImageSha1`.
3. **Functional Smoke Checklist**:
   - Boot to title menu without `"another release"` mismatch prompt.
   - Verify Vulkan rendering, SPIR-V shader cache generation, and audio playback.
   - Open in-game **Options** → confirm **GRAPHICS**, **TOUCH CONTROLS**, **GENERAL**, **MOUSE AND KEYBOARD** (`options_menu.cpp`), and touch-gear ImGui panel (**CHEATS**, etc. in `menu.cpp`) open cleanly.
   - Load Level 1 (*Veblensk Street*), verify HUD anchors (`hud.cpp`), shooting/reloading/FOV (`hooks.cpp`, `weapons.cpp`), and save creation under `Android/data/com.racas.black/files/hdd/E/UDATA/45410083/`.
   - Capture logs on failure via `adb logcat -s BLACK SDL` and `Android/data/com.racas.black/files/logs/black.log`.

---

## 5. Packaging & Distributing External Asset Mods Separately from the APK

To respect copyright boundaries and preserve 100% compatibility with untouched Xbox `.iso` images:

1. **Never bundle game assets into `app-release.apk`**: Keep `assets/`, `iso/`, and redistributable game media outside the APK (only engine code, `europe.patch`, native libs, and [`app/src/main/assets/skins/buttons.png`](app/src/main/assets/skins/buttons.png) ship inside the APK).
2. **Loose external override behavior (baseline vs Enhanced Edition overlay)**:
   - **Baseline behavior today**: `XboxXBE-Recomp`'s `Resolve()` (`runtime/kernel/files.cpp`) resolves `D:\` exclusively through `MountedDisc()` whenever an `.iso` is mounted, ignoring loose files placed beside the `.iso`. Users applying custom `D:\` replacement files on the unmodified runtime must therefore extract their legally owned `.iso` (`default.xbe` + folders) and place overrides inside the extracted directory.
   - **Recommended Enhanced Edition packaging**: Add a non-destructive pre-disc overlay lookup in host/kernel resolution (checking e.g. `/storage/emulated/0/Black/mods/` or `Android/data/com.racas.black/files/mods/` before falling back to `MountedDisc()`), and load host-managed resources (Arabic `.ttf` fonts, external localization packs, custom touch skins, or HD texture packs) directly from `/storage/emulated/0/Black/` or app-private `files/`. Users then keep two clean, separate pieces:
     1. **Untouched original Xbox `.iso`** (imported via the standard picker or copied to `/storage/emulated/0/Black/`).
     2. **Optional Enhanced Edition Asset Pack (`.zip` extracted to `Black/mods/` or `Black/`)** containing only user-created mods (fonts, translations, texture/UI replacements), requiring **no `.iso` modification** and **no rebuild of `libmain.so`** for compatible pure-data assets.
3. **Rule of thumb for rebuild necessity**:
   - **No `libmain.so` / APK rebuild required**: Replacing `black.toml` / `android.toml` / `layout.toml`, format-compatible `videos/`, `sound/`, or `D:\` data files (once overlay or extracted mode is active), or host-loaded font/translation/texture packs supported by the compiled engine.
   - **APK (`gradlew.bat assembleRelease`) rebuild required**: Any C++/shader changes in `native/` or `src/` (Vulkan effects, Arabic text shaper, ImGui/Flash font bridge, existing hook logic).
   - **Full XBE recompilation (`build.bat` / `xbe_recomp.exe`) required**: Adding or moving `[[hook]]` VA addresses in [`config/game.toml`](config/game.toml) or modifying recompiled `generated/game/*.cpp` bindings.

---

## 6. GitHub Actions CI Note (Fork Builds)

[`build.yml`](.github/workflows/build.yml) builds release APKs on `windows-2025` runners when a `v*` tag is pushed, using repository secrets `XBE_KEY` (to decrypt `ci/default.xbe.enc`), `KEYSTORE_BASE64`, `KEY_ALIAS`, `KEY_PASSWORD`, and `STORE_PASSWORD`. Because `XBE_KEY` is private to the upstream maintainer, fork CI workflows will require supplying your own encrypted `default.xbe` secret or triggering builds locally via `build.bat`.

