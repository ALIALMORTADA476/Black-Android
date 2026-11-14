# BLACK Enhanced Edition — Native C++ Test Results

**Document Version:** 1.0.0  
**Repository:** `e:\ai\black`  
**Branch:** `black-enhanced-edition`  
**Milestone:** Stage 2.1 — Environment Recovery & Genuine Build Validation  
**Date:** March 2025  
**Compiler:** GNU C++ Compiler (GCC) 16.2.0 (x86_64-w64-mingw32)  
**Build System:** CMake 4.4.4 + Ninja 1.13.0  
**Target:** `tests/build/test_settings_core.exe`  
**Author:** Antigravity (Google DeepMind)

---

## 1. Executive Summary

This report documents the genuine native C++ compilation, execution, and verification of the BLACK Enhanced Edition settings architecture:
- `src/common/mod_schema.cpp` & `src/common/mod_schema.h`
- `src/common/presets.cpp` & `src/common/presets.h`
- `tests/test_settings_core.cpp`
- `tests/CMakeLists.txt`

A local, non-invasive C++20 toolchain (`GCC 16.2.0`, `CMake 4.4.4`, `Ninja 1.13.0`) was deployed into `.tools/w64devkit` without altering system-wide configuration or registry settings. The native test harness was compiled and linked as an executable (`test_settings_core.exe`).

During the initial native test execution, a real schema bug was detected: `render_percent` was configured with `NumericBounds{20, 200, 5, ...}`, which clamped the authentic console default of `67%` down to `65%`. This bug was resolved by adjusting `uint_step = 1`, achieving **100% test pass rate (9/9 passed, 0 failed)** across both native C++ and Python verification suites.

---

## 2. Compilation Log & Toolchain Configuration

### 2.1 Toolchain Identification
```text
Compiler: g++.exe (GCC) 16.2.0 (x86_64-w64-mingw32)
Build Tool: cmake.exe version 4.4.4
Generator: Ninja 1.13.0
C++ Standard: C++20 (-std=gnu++20)
```

### 2.2 CMake Configuration & FetchContent Output
```text
-- The CXX compiler identification is GNU 16.2.0
-- Detecting CXX compiler ABI info
-- Detecting CXX compiler ABI info - done
-- Check for working CXX compiler: e:/ai/black/.tools/w64devkit/bin/g++.exe - skipped
-- Detecting CXX compile features
-- Detecting CXX compile features - done
-- Populating tomlplusplus
-- Configuring done (6.6s)
-- Generating done (0.0s)
-- Build files have been written to: E:/ai/black/tests/build
```

### 2.3 Ninja Build Targets
```text
[1/4] Building CXX object CMakeFiles/test_settings_core.dir/E_/ai/black/src/common/presets.cpp.obj
[2/4] Building CXX object CMakeFiles/test_settings_core.dir/test_settings_core.cpp.obj
[3/4] Building CXX object CMakeFiles/test_settings_core.dir/E_/ai/black/src/common/mod_schema.cpp.obj
[4/4] Linking CXX executable test_settings_core.exe
```
**Compilation Result:** Clean build, 0 warnings, 0 linker errors. Output binary: `tests/build/test_settings_core.exe`.

---

## 3. Bug Discovery & Root Cause Analysis

### 3.1 Initial Failure Trace
On the first test run, tests 4 and 6 failed with the following assertion:
```text
[FAIL] TestMissingKeys:233 Expected equality: s.render_percent vs 67u
[FAIL] TestPresetSwitching:340 Expected equality: s.render_percent vs 67u
Summary: 7 passed, 2 failed.
```

### 3.2 Root Cause
1. In `src/common/mod_schema.cpp`:
   ```cpp
   add(SettingId::RenderPercent, "render_percent", nullptr, Category::Graphics, SettingType::Uint,
       FeatureStatus::Implemented, ApplyScope::Immediate, "RENDER RESOLUTION",
       "Internal rendering resolution percentage relative to device screen height.",
       NumericBounds{20, 200, 5, 0.f, 0.f, 0.1f, {}, {}});
   ```
2. The function `ClampAndStepUint` enforces step alignment:
   ```cpp
   if (b.uint_step > 1) {
       uint32_t rem = (v - b.uint_min) % b.uint_step;
       if (rem != 0) v -= rem;
   }
   ```
3. With `uint_min = 20` and `uint_step = 5`, any value must satisfy `(v - 20) % 5 == 0`.
4. The upstream default on mobile devices is `kFirstRunRenderPercent = 67` (~720p scaling).
5. Calculating `(67 - 20) % 5 = 47 % 5 = 2`. `67 - 2 = 65`.
6. Therefore, `s.render_percent` was inadvertently mutated to `65%` on every load and preset switch!

### 3.3 Resolution
In `src/common/mod_schema.cpp`:
Updated `render_percent` bounds to `NumericBounds{20, 200, 1, 0.f, 0.f, 0.1f, {}, {}}`. This allows fine-grained resolution scaling (including 67%, 72%, 100%, 144%) without unexpected truncations.

---

## 4. Final Verbatim Native Test Output

```text
Running native test_settings_core.exe...
[RUN ] 1_DefaultSettings
[PASS] 1_DefaultSettings
[RUN ] 2_SettingsSaveAndLoad
[PASS] 2_SettingsSaveAndLoad
[RUN ] 3_InvalidValues
[PASS] 3_InvalidValues
[RUN ] 4_MissingKeys
[PASS] 4_MissingKeys
[RUN ] 5_UnknownKeysPreservation
[PASS] 5_UnknownKeysPreservation
[RUN ] 6_PresetSwitching
[PASS] 6_PresetSwitching
[RUN ] 7_CustomSnapshotRecovery
[PASS] 7_CustomSnapshotRecovery
[RUN ] 8_RestartRequiredSettings
[PASS] 8_RestartRequiredSettings
[RUN ] 9_SchemaMigration
[PASS] 9_SchemaMigration

Summary: 9 passed, 0 failed.
```

---

## 5. Individual Test Case Breakdown

| Test ID | Test Name | Invariants Verified | Result |
|---|---|---|---|
| **Test 1** | `1_DefaultSettings` | 70+ settings initialized; locked features (`cas_sharpen_enabled`, etc.) reject mutations. | **PASS** |
| **Test 2** | `2_SettingsSaveAndLoad` | Full TOML serialization to file and exact value deserialization roundtrip. | **PASS** |
| **Test 3** | `3_InvalidValues` | Clamps out-of-range numbers, snaps invalid enum integers to discrete allowed lists. | **PASS** |
| **Test 4** | `4_MissingKeys` | Partial TOML inputs load cleanly; omitted keys retain exact defaults (`render_percent = 67`, `walk_speed = 50`). | **PASS** |
| **Test 5** | `5_UnknownKeysPreservation` | Arbitrary third-party keys (`custom_user_flag`, `future_port_option`) are preserved across save/reload cycles. | **PASS** |
| **Test 6** | `6_PresetSwitching` | Switching between Original, Enhanced, and Performance modifies target graphics options while strictly preserving cheats and controls. | **PASS** |
| **Test 7** | `7_CustomSnapshotRecovery` | Preserves manual user adjustments in `[presets.last_custom]` and restores them when returning to Custom. | **PASS** |
| **Test 8** | `8_RestartRequiredSettings` | Compares boot snapshot against current state; accurately flags `ApplyScope::AppRestart` settings (`audio_output`, `arabic_localization_enabled`). | **PASS** |
| **Test 9** | `9_SchemaMigration` | Version 1 files (`defaults = 1`) seamlessly upgrade to version 2 (`schema_version = 2`) with legacy fallback defaults. | **PASS** |

---

## 6. Verification Parity Confirmation

To ensure complete dual-suite integrity:
- **Native C++ Test Harness:** `test_settings_core.exe` -> **9/9 Passed (0 Failed)**
- **Python Verification Suite:** `python tests/test_settings_suite.py` -> **9/9 Passed (0 Failed)**

All core data structures and logic are proven sound under native compilation.

