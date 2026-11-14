# BLACK Enhanced Edition — Device Deployment & Verification Report

**Milestone**: Stage 4 — In-Game Tactical Mod Menu & Functional Feature Integration  
**Target Device Serial**: `damfmjij5pbe7dbu`  
**Target Model**: Xiaomi Redmi 25080RABDG  
**OS Version**: Android 16 (HyperOS V816, Linux Kernel 6.6)  
**Primary ABI**: `arm64-v8a`  
**Application ID**: `com.racas.black.enhanced`  
**Version**: `v1.2.0-enhanced` (Build `25`)  

---

## 1. Target Device Identification

```
Device Serial       : damfmjij5pbe7dbu
Connection Status   : device (ADB over USB)
Manufacturer        : Xiaomi
Brand / Model       : Redmi / 25080RABDG
Platform / Arch     : arm64-v8a
OS Version          : Android 16 (API Level 36 / HyperOS V816)
SELinux Enforcing   : Yes
Coexistence Target  : Side-by-side with original com.racas.black
```

---

## 2. Artifact Build Metrics

```
APK File Location   : app/build/outputs/apk/release/app-release.apk
Device Location     : /sdcard/Download/Black_Enhanced.apk
File Size           : 22,491,202 bytes (22.49 MB)
SHA-256 Checksum    : 3ED11EC175E4F64312C8587943FB4A1CDB47F1C3A88178AC06E509C9DB6FD7E4
Package Name        : com.racas.black.enhanced
Version Code        : 25
Version Name        : 1.2.0-enhanced
Application Label   : Black Enhanced
Compile SDK Version : 34 (Android 14)
Target SDK Version  : 34 (Android 14)
Min SDK Version     : 29 (Android 10)
Native ABI          : arm64-v8a
Native Libraries    : libmain.so, libSDL3.so
Page Alignment      : 16 KB (-Wl,-z,max-page-size=16384 verified for Android 15/16)
```

---

## 3. Side-by-Side Installation Verification

### 3.1 Direct Streamed ADB Installation
```powershell
adb install -r app/build/outputs/apk/release/app-release.apk
```
**Output**:
```
Performing Streamed Install
Success
```

### 3.2 Dual Package Presence Verification
Querying `pm list packages` confirms that both the original unmodified game and the Enhanced Edition are installed simultaneously:
```
package:com.racas.black
package:com.racas.black.enhanced
```
- Original application `com.racas.black` (versionCode 23, signed by APKVISION) is preserved intact with all original user data, saves, and cache unaffected.
- Enhanced Edition `com.racas.black.enhanced` (versionCode 25, versionName `1.2.0-enhanced`) is installed side-by-side as a distinct application.

### 3.3 Package Metadata Dumpsys Inspection
```powershell
adb shell "dumpsys package com.racas.black.enhanced | grep -E 'versionCode|versionName|pkgFlags'"
```
**Output**:
```
versionCode=25 minSdk=29 targetSdk=34
versionName=1.2.0-enhanced
pkgFlags=[ HAS_CODE ALLOW_CLEAR_USER_DATA ]
```

### 3.4 Download Mirror Copy
The updated Build 25 APK has also been mirrored to the device's internal storage for convenient standalone installation or sharing:
```
/sdcard/Download/Black_Enhanced.apk (22,491,202 bytes)
```

---

## 4. Test Suite Execution & Verification Results

### 4.1 Native C++ Test Harness (`tests/build/test_settings_core.exe`)
```
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

### 4.2 Python Settings Suite (`tests/test_settings_suite.py`)
```
[RUN ] Test 1: Default Settings
[PASS] Test 1: Default Settings
[RUN ] Test 2: Settings Save and Load
[PASS] Test 2: Settings Save and Load
[RUN ] Test 3: Invalid Values & Robustness
[PASS] Test 3: Invalid Values & Robustness
[RUN ] Test 4: Missing Keys Partial Load
[PASS] Test 4: Missing Keys Partial Load
[RUN ] Test 5: Unknown Keys Preservation
[PASS] Test 5: Unknown Keys Preservation
[RUN ] Test 6: Preset Switching & Cheat/Control Isolation
[PASS] Test 6: Preset Switching & Cheat/Control Isolation
[RUN ] Test 7: Custom Snapshot Recovery
[PASS] Test 7: Custom Snapshot Recovery
[RUN ] Test 8: Restart Required Settings & Boot Snapshot
[PASS] Test 8: Restart Required Settings & Boot Snapshot
[RUN ] Test 9: Schema Migration
[PASS] Test 9: Schema Migration

Results: 9 passed, 0 failed out of 9 tests.
```

---

## 5. Verification Verdict

- **Total Automated Tests**: 18
- **Passed**: 18 (100%)
- **Failed**: 0
- **Device Deployment Status**: **SUCCESS** (`com.racas.black.enhanced` v1.2.0-enhanced / Build 25 active on target device `damfmjij5pbe7dbu`)
- **Side-by-Side Coexistence**: **CONFIRMED**

