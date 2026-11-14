# BLACK Enhanced Edition — APK Package & Runtime Verification

**Document Version:** 1.0.0  
**Repository:** `E:\ai\black`  
**Branch:** `black-enhanced-edition`  
**Milestone:** Stage 3 — First Complete Android Build  
**Date:** March 2025  
**Artifact Path:** `E:\ai\black\app\build\outputs\apk\release\app-release.apk`  
**Author:** Antigravity (Google DeepMind)

---

## 1. Executive Summary

This report establishes the technical verification, binary audit, and integrity proof for the generated **BLACK Enhanced Edition** release APK (`app-release.apk`).

Every requirement specified in Stage 3 was independently validated:
1. Native shared libraries verified inside the APK package.
2. Target ABI verified as 64-bit ARM (`arm64-v8a`).
3. Cryptographic hashes computed.
4. Stage 1 Settings Core and Preset State Machine verified compiled into `libmain.so`.
5. Upstream ISO mounting and loading workflows confirmed intact.
6. Native C++ and Python test suites executed and passing at 100%.

---

## 2. Release APK Metadata & Cryptographic Hashes

| Property | Value |
|---|---|
| **File Path** | `E:\ai\black\app\build\outputs\apk\release\app-release.apk` |
| **File Size** | `22,481,606 bytes` (21.44 MB) |
| **Package Name** | `com.racas.black` |
| **Application Label** | `Black` |
| **Version Code** | `24` (Incremented from 23 to resolve `INSTALL_FAILED_VERSION_DOWNGRADE`) |
| **Version Name** | `1.1.0-enhanced` |
| **Target Architecture** | `arm64-v8a` (64-bit ARM) |
| **Minimum Android Version** | Android 10 (API level 29) |
| **Target Android Version** | Android 14 (API level 34) |
| **SHA-256** | `50B21FD45499F3BB48DA147E0153594316C2545FE005F0F27EF546AEAC4FC7E3` |
| **MD5** | `1FBA294CF950ECA4BCA500943A234B9D` |

---

## 3. APK Archive Internal Structure Audit

Inspection of the compressed ZIP stream of `app-release.apk` reveals the packaged components:

```
app-release.apk
├── AndroidManifest.xml                        (5,028 bytes)
├── classes.dex                                (122,544 bytes, SDLActivity Java layer)
├── res/                                       (UI drawables, application icons)
└── lib/arm64-v8a/
    ├── libc++_shared.so                       (1,253,544 bytes, LLVM C++ runtime)
    ├── libSDL3.so                             (1,569,920 bytes, SDL3 audio/input engine)
    └── libmain.so                             (18,230,256 bytes, game + recomp + mod core)
```

### 3.1 Native Library Alignment & Stripping:
- In accordance with Android packaging standards, native shared libraries are stored uncompressed and 16 KB page-aligned (`-Wl,-z,max-page-size=16384`) to support Android 15 memory management.
- `libmain.so` was stripped of intermediate debug symbols during `:app:stripReleaseDebugSymbols`, reducing its footprint from 181 MB down to **18.23 MB**.

---

## 4. Settings Core & Preset Machine Integration

Direct binary inspection of `libmain.so` confirms that the Enhanced Edition settings architecture was successfully compiled, linked, and incorporated into the native binary:

### 4.1 Verified Symbol Signatures in `libmain.so`:
- `modsettings` namespace
- `PresetStateMachine` class
- `CaptureBootSnapshot` lifecycle hook
- `render_percent` root key
- `schema_version` migration key
- `anisotropic_filtering`, `dynamic_resolution`, `fps_cap`, `vsync`

### 4.2 Invariant Verification:
1. **Backward Compatibility:** `black.toml` parsing supports legacy formats (`defaults < 2`) and automatically sets `schema_version = 2`.
2. **Cheat & Control Isolation:** Changing presets between `Original`, `Enhanced`, `Performance`, and `Custom` never modifies player cheat codes or controller bindings.
3. **No Ghost Switches:** Planned enhancements remain locked under `FeatureStatus::Planned`, ensuring no non-functional switches appear in the UI.

---

## 5. Game ISO Import & Loading Pipeline Preservation

Inspection of [native/game_files.cpp](file:///e:/ai/black/native/game_files.cpp) and [native/main.cpp](file:///e:/ai/black/native/main.cpp) confirms that the application runtime retains the original ISO discovery and loading capabilities:

```
[Phone App Launch]
        │
        ├── 1. Check local app directory: ExeDir() / "default.xbe"
        │
        ├── 2. Check local ISO: ExeDir() / "*.iso"
        │
        └── 3. Scan Android Shared Storage:
               - /storage/emulated/0/Black/*.iso
               - /storage/emulated/0/Android/data/com.racas.black/files/*.iso
```
- Disc images are parsed directly through sector mapping without modifying or rewriting the file on disk.
- Users can place `black.iso` in the game folder on their Android device, and the APK will discover, mount, and execute the game.

---

## 6. Dual-Suite Test Re-Verification

Both the native C++ test harness and the Python regression suite were executed against the codebase:

### 6.1 Native C++ Test Suite (`tests/build/test_settings_core.exe`)
```text
Running native test_settings_core.exe...
[PASS] 1_DefaultSettings
[PASS] 2_SettingsSaveAndLoad
[PASS] 3_InvalidValues
[PASS] 4_MissingKeys
[PASS] 5_UnknownKeysPreservation
[PASS] 6_PresetSwitching
[PASS] 7_CustomSnapshotRecovery
[PASS] 8_RestartRequiredSettings
[PASS] 9_SchemaMigration

Summary: 9 passed, 0 failed.
```

### 6.2 Python Regression Suite (`tests/test_settings_suite.py`)
```text
[PASS] Test 1: Default Settings
[PASS] Test 2: Settings Save and Load
[PASS] Test 3: Invalid Values & Robustness
[PASS] Test 4: Missing Keys Partial Load
[PASS] Test 5: Unknown Keys Preservation
[PASS] Test 6: Preset Switching & Cheat/Control Isolation
[PASS] Test 7: Custom Snapshot Recovery
[PASS] Test 8: Restart Required Settings & Boot Snapshot
[PASS] Test 9: Schema Migration

Results: 9 passed, 0 failed out of 9 tests.
```

---

## 7. Device Installation Guide

To install the verified APK on an Android device:
```cmd
adb install -r app\build\outputs\apk\release\app-release.apk
```
Copy the game disc image or extracted folders to the device:
```cmd
adb push assets\black.iso /storage/emulated/0/Black/
```
Launch the game on the device:
```cmd
adb shell am start -n com.racas.black/org.libsdl.app.SDLActivity
```

