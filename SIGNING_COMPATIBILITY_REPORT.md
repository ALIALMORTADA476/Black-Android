# BLACK Enhanced Edition — Signing Compatibility & Side-by-Side Installation Report

**Document Version:** 1.0.0  
**Repository:** `E:\ai\black`  
**Branch:** `black-enhanced-edition`  
**Date:** March 2025  
**Target Device:** Xiaomi Redmi (`25080RABDG`, Xiaomi HyperOS V816, Android 16)  
**Device Serial:** `damfmjij5pbe7dbu`  
**Author:** Antigravity (Google DeepMind)

---

## 1. Executive Summary

This report documents the investigation and resolution of the `INSTALL_FAILED_UPDATE_INCOMPATIBLE` error encountered when attempting to install the locally compiled BLACK Enhanced Edition APK over an existing device installation.

In strict accordance with the user's instructions:
1. **Zero Data Loss:** The original application (`com.racas.black`) was **not** uninstalled, and its user data, saves, and cache were not erased.
2. **Certificate Forensic Analysis:** The installed APK was pulled and compared against the local build, revealing that the installed package was signed with a third-party certificate (`APKVISION.ORG`), whereas local builds use the default Android signing key.
3. **Side-by-Side Decoupling:** The Enhanced Edition was reconfigured with a distinct application ID:
   `applicationId: com.racas.black.enhanced`
4. **Shared Storage & ISO Preservation:** Both applications can simultaneously and non-destructively read `/storage/emulated/0/Black/black.iso`, while maintaining strictly isolated application data folders in `Android/data/`.
5. **Direct On-Device Distribution:** The verified release APK was transferred directly to the device at `/sdcard/Download/Black_Enhanced.apk`.

---

## 2. Phase 1 — Certificate Forensic Investigation

### 2.1 Retrieval of Installed APK
Using ADB without modifying or uninstalling the package:
```cmd
adb shell pm path com.racas.black
-> package:/data/app/~~sBlNb7XDV387Itk8Ms8aKg==/com.racas.black-4VfRf0JFEyGU2Sm66oJ9Vg==/base.apk

adb pull /data/app/~~sBlNb7XDV387Itk8Ms8aKg==/com.racas.black-4VfRf0JFEyGU2Sm66oJ9Vg==/base.apk .tools/original_installed.apk
```
Pulled binary size: `22,373,729 bytes`.

### 2.2 Certificate Comparison (`apksigner verify --print-certs`)

| Attribute | Installed Original APK | Locally Built Release APK |
|---|---|---|
| **Distinguished Name (DN)** | `CN=APKVISION.ORG, OU=APKVISION.ORG, O=APKVISION.ORG, L=APKVISION.ORG, ST=APKVISION.ORG, C=APKVISION.ORG` | `C=US, O=Android, CN=Android Debug` |
| **SHA-256 Digest** | `a54b28d5675941c1d39d6937b4f8e4c69966dede417aca2785bd8dc840c8d1d8` | `1a9833560e6294f39805fcabfe1efa64ad8df73effc992fd71fd7b045cde84e8` |
| **SHA-1 Digest** | `496900e8e6f947ad542a5f5f0434cb7aa01029ac` | `3464c2e74c69269f78b78483e7ee8da8563db4e6` |
| **MD5 Digest** | `ed0a4db37c992a1012548d9d060d46cd` | `95ebbb4ec9b7ea26adec512acc16b95a` |

### 2.3 Root Cause of `INSTALL_FAILED_UPDATE_INCOMPATIBLE`
Android's package manager enforces cryptographic signature matching for package updates. Because the installed APK originates from an external distribution signed by `APKVISION.ORG`, and our local build uses our local signing key, Android security prohibits updating `com.racas.black` in-place. Because the private key and keystore password for `APKVISION.ORG` are proprietary and unavailable, an in-place update cannot be signed identically.

---

## 3. Phase 2 & 3 — Side-by-Side Architecture & Compatibility

To guarantee zero impact on the existing game installation, BLACK Enhanced Edition was configured as an independent, side-by-side package:

### 3.1 Package Configuration (`app/build.gradle`)
```groovy
android {
    namespace "com.racas.black"
    compileSdk 34
    ndkVersion "28.2.13676358"

    defaultConfig {
        applicationId "com.racas.black.enhanced"
        minSdk 29
        targetSdk 34
        versionCode project.hasProperty("versionCode") ? project.versionCode.toInteger() : 24
        versionName project.hasProperty("versionName") ? project.versionName : "1.1.0-enhanced"
        ndk {
            abiFilters "arm64-v8a"
        }
    }
}
```

### 3.2 Compatibility Audit:
1. **Namespace vs. Application ID Decoupling:**
   - `namespace` remains `"com.racas.black"`, ensuring that Java packages, R classes, and JNI entry points (`Java_com_racas_black_BlackActivity_...` in `native/input.cpp`) match without modifying C++ source code.
   - `applicationId` is `"com.racas.black.enhanced"`, providing a unique package namespace in Android OS.
2. **Application Label:**
   - Updated `res/values/strings.xml` to `"Black Enhanced"`, giving the Enhanced Edition an identifiable title on the phone's home screen.
3. **Storage Isolation (`app::ExeDir()` in `native/main.cpp`):**
   - Original App Directory: `/storage/emulated/0/Android/data/com.racas.black/files/`
   - Enhanced App Directory: `/storage/emulated/0/Android/data/com.racas.black.enhanced/files/`
   - Game settings (`black.toml`), custom presets, logs, and Vulkan pipeline caches are strictly separated, preventing settings corruption or collisions.
4. **Shared ISO Access (`native/game_files.cpp`):**
   - Both applications read from `/storage/emulated/0/Black/black.iso`.
   - The ISO file is opened read-only (`std::ios::in | std::ios::binary`).
   - Neither application modifies or locks the disc image.

---

## 4. Phase 4 — Rebuild, Verification & On-Device Deployment

### 4.1 Rebuild Execution
```cmd
.\gradlew.bat assembleRelease
```
**Build Status:** `BUILD SUCCESSFUL in 18s` (Incremental rebuild, all 35 C++ translation units up-to-date).

### 4.2 Release APK Badging (`aapt2 dump badging`)
```text
package: name='com.racas.black.enhanced' versionCode='24' versionName='1.1.0-enhanced' compileSdkVersion='34'
application-label:'Black Enhanced'
launchable-activity: name='com.racas.black.BlackActivity'
native-code: 'arm64-v8a'
```

### 4.3 Artifact Attributes
- **Artifact Path:** `E:\ai\black\app\build\outputs\apk\release\app-release.apk`
- **File Size:** `22,481,606 bytes` (21.44 MB)
- **SHA-256:** `50B21FD45499F3BB48DA147E0153594316C2545FE005F0F27EF546AEAC4FC7E3`
- **MD5:** `1FBA294CF950ECA4BCA500943A234B9D`

---

## 5. Device Installation Status & Next Steps

### 5.1 Target Device Environment
- **Device Model:** Xiaomi Redmi (`25080RABDG`)
- **OS Version:** Android 16 (Xiaomi HyperOS V816)
- **Original App Status:** `package:com.racas.black` is currently installed, active, and preserved.

### 5.2 Xiaomi HyperOS ADB Restriction Note
When running `adb install`, Xiaomi HyperOS blocks automated USB installs with:
`Failure [INSTALL_FAILED_USER_RESTRICTED: Install canceled by user]`
This is a standard Xiaomi security policy that requires user authorization on the physical screen.

### 5.3 Ready-to-Install On-Device APK
To facilitate instant installation without ADB prompts, the release APK was copied directly to your phone's storage:
```text
/storage/emulated/0/Download/Black_Enhanced.apk
```

### 5.4 How to Complete the Coexistence Installation:
**Option A (Direct on Phone — Recommended):**
1. Open the **Files** or **Downloads** app on your phone.
2. Tap **`Black_Enhanced.apk`** and select **Install**.
3. Both **Black Android** (original) and **Black Enhanced** will coexist side by side.

**Option B (Via ADB):**
1. On your phone, go to: `Settings > Additional settings > Developer options`.
2. Toggle ON **"Install via USB"**.
3. Re-run:
   ```cmd
   adb install -r app\build\outputs\apk\release\app-release.apk
   ```

---

## 6. Coexistence Verification Checklist

- [x] Original package `com.racas.black` preserved without uninstallation or data loss.
- [x] Distinct application ID `com.racas.black.enhanced` configured and built.
- [x] Signing certificates and fingerprints compared and documented.
- [x] Internal namespace and JNI bindings preserved.
- [x] Shared ISO access verified safe and non-interfering.
- [x] Enhanced Edition APK (`Black_Enhanced.apk`) copied to device storage for installation.

