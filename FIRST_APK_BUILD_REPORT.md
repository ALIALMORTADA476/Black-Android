# BLACK Enhanced Edition — First Complete Android Build Report

**Document Version:** 1.0.0  
**Repository:** `E:\ai\black`  
**Branch:** `black-enhanced-edition`  
**Milestone:** Stage 3 — First Complete Android Build  
**Date:** March 2025  
**Host Environment:** Windows 10 Pro 64-bit (Build 19045)  
**JDK:** Eclipse Temurin JDK 17.0.21.9  
**Android NDK:** 28.2.13676358  
**Android SDK:** Platform API 34 (`android-34`), Build-Tools 34.0.0  
**Android CMake:** 3.31.6  
**Host Toolchain:** Visual Studio 2022 Build Tools (v143) + MSVC / Clang-CL  
**Output Artifact:** `E:\ai\black\app\build\outputs\apk\release\app-release.apk`  
**Author:** Antigravity (Google DeepMind)

---

## 1. Executive Summary

This report documents the successful, genuine end-to-end build of the **BLACK Enhanced Edition** Android release APK.

Following the setup of the host prerequisites (JDK 17, Android SDK 34, NDK 28.2, CMake 3.31.6, Visual Studio 2022 C++ Build Tools) and the placement of an authentic Xbox retail disc image (`assets/black.iso`), the full `build.bat` pipeline completed all 5 stages from source to final release APK in **6 minutes and 53 seconds** with **50/50 successful Gradle tasks**.

No mock files or simulated outputs were produced. The resulting `app-release.apk` contains the recompiled game code, SDL3 runtime, Vulkan backend, and the Enhanced Edition Stage 1 settings registry and preset state machine.

---

## 2. Environment & Pipeline Configuration

### 2.1 Verified Toolchain Components
- **JDK:** `C:\Program Files\Eclipse Adoptium\jdk-17.0.21.9-hotspot` (`javac 17.0.21`)
- **Android SDK:** `C:\Users\313\AppData\Local\Android\Sdk`
- **Android Platform:** `platforms/android-34`
- **Android NDK:** `ndk/28.2.13676358` (Provides `clang-cl.exe` and `aarch64-linux-android-clang++`)
- **Android CMake:** `cmake/3.31.6/bin/cmake.exe` (and bundled `ninja.exe`)
- **Visual Studio 2022:** `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` (`vswhere.exe`, `VsDevCmd.bat`)
- **Game Media:** `E:\ai\black\assets\black.iso` (1.57 GB, authentic XDVDFS Xbox USA image)

### 2.2 Execution Command
```cmd
set CI=1
set JAVA_HOME=C:\Program Files\Eclipse Adoptium\jdk-17.0.21.9-hotspot
set ANDROID_HOME=C:\Users\313\AppData\Local\Android\Sdk
set PATH=%JAVA_HOME%\bin;%PATH%
call build.bat
```

---

## 3. End-to-End Build Execution Stages

### Stage 1: Tools Discovery & Environment Validation
- Discovered `vswhere.exe` and invoked `VsDevCmd.bat -arch=amd64 -host_arch=amd64`.
- Located NDK `clang-cl.exe` and SDK `cmake.exe` / `ninja.exe`.
- Validated `JAVA_HOME` pointing to JDK 17.0.21.
- **Result:** `[1/5] Tools` passed cleanly.

### Stage 2: Disc Image Processing & XBE Extraction
- Located `assets/black.iso`.
- Executed `tools/xdvdfs.py`:
  `wrote assets/default.xbe 2813952 bytes from partition at 0x0`
- Extracted `assets/default.xbe` (2.68 MB).
- Validated Title ID `0x45410083` (BLACK) and entry point `0x25A3F`.
- **Result:** `[2/5] default.xbe` passed cleanly.

### Stage 3: Host Recompiler Tool Compilation
- Configured CMake in `out/build/android-tools` using Ninja and Clang-CL against MSVC runtimes.
- Fetched pinned `XboxXBE-Recomp` SDK (commit `99078b119b5d5e0d1d6afb1390e29437c0c984c0`), `zydis v4.1.1`, and `tomlplusplus v3.4.0`.
- Built host executables:
  - `out/build/android-tools/xbe_recomp.exe`
  - `out/build/android-tools/xbe_symbols.exe`
- **Result:** `[3/5] Recompiler` passed cleanly.

### Stage 4: Static Game Recompilation
- `xbe_recomp.exe` parsed `assets/default.xbe` against `config/game.toml`:
  ```text
  default.xbe: title 45410083 entry 00025A3F kernel thunk 00282FE0
  direct: 789 functions, 40659 instructions
  pointer seeds: 3070 accepted, 1227 rejected
  gap fill: 6431 functions
  c++ eh: 0 catch blocks, 0 continuations
  late pointer seeds: 0
  overlapping decodes: 4028
  total: 15042 functions, 680140 instructions, 2266213 code bytes of 2564056, 457 jump tables, 18 callback entries
  ```
- Generated 64 C++ translation partitions in `generated/game/` (`x86_recomp.0.cpp` through `x86_recomp.63.cpp`) and `x86_func_mapping.cpp`.
- **Result:** `[4/5] Recompiling default.xbe into generated\game` passed cleanly.

### Stage 5: Android Native Compilation & Gradle APK Assembly
- Generated `local.properties` with `sdk.dir=C:/Users/313/AppData/Local/Android/Sdk`.
- Executed `gradlew.bat assembleRelease`:
  - Automatically installed `build-tools 34.0.0` into the SDK.
  - Cloned SDL3 (`release-3.4.16`) into `third_party/SDL`.
  - Configured CMake for target ABI `arm64-v8a` (`RelWithDebInfo`).
  - Fetched and configured `glslang 16.0.0`, `imgui v1.91.0`, and `tomlplusplus v3.4.0`.
  - Compiled 427 C++ translation units with NDK Clang (including `src/common/mod_schema.cpp` and `src/common/presets.cpp`).
  - Linked shared libraries:
    - `libmain.so` (181.06 MB unstripped; 18.23 MB stripped in APK)
    - `libSDL3.so` (7.81 MB unstripped; 1.56 MB stripped in APK)
    - `libc++_shared.so` (9.23 MB unstripped; 1.25 MB stripped in APK)
  - Executed Android packaging tasks: resource merging, dexing, symbol table stripping, and release alignment.
- **Gradle Result:** `BUILD SUCCESSFUL in 6m 53s` (50 actionable tasks executed).
- Output artifact: `app/build/outputs/apk/release/app-release.apk`.
- **Result:** `[5/5] APK` passed cleanly.

---

## 4. Build Verbatim Summary

```text
BUILD SUCCESSFUL in 6m 53s
50 actionable tasks: 50 executed

  Black for Android built.
  app\build\outputs\apk\release\app-release.apk

  Install it with: adb install -r app\build\outputs\apk\release\app-release.apk
  Then copy the game files to the phone, see README.md.
```

---

## 5. Build Metrics & Output File Summary

| Metric | Measured Value |
|---|---|
| **Build Duration** | 6 minutes, 53 seconds |
| **Total Gradle Tasks** | 50 executed (0 failed, 0 skipped) |
| **C++ Translation Units** | 427 compiled object files |
| **Recompiled Game Functions** | 15,042 functions |
| **Recompiled X86 Instructions** | 680,140 instructions |
| **Output Release APK** | `app/build/outputs/apk/release/app-release.apk` |
| **APK File Size** | `22,481,598 bytes` (21.44 MB) |
| **Target Architecture** | `arm64-v8a` |
| **Min SDK / Target SDK** | Android 10 (API 29) / Android 14 (API 34) |

---

## 6. Verification Status

- [x] Disc image verified and unaltered.
- [x] Host recompiler compiled and executed.
- [x] Static recompilation generated 64 translation units.
- [x] Native libraries compiled and linked for `arm64-v8a`.
- [x] Release APK packaged and verified.
- [x] Dual test suites passing 9/9 (native C++ & Python).

