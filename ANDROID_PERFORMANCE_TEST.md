# Android Performance & Live Device Verification (`ANDROID_PERFORMANCE_TEST.md`)

**Project:** BLACK Enhanced Edition  
**Repository:** `E:\ai\black` (Branch: `black-enhanced-edition`)  
**Target Device:** Xiaomi `25080RABDG` (`lapis_global` / `damfmjij5pbe7dbu`), ARM64 (`arm64-v8a`), `Mali-G615 MC2` (Vulkan `1.3.247`), `1280x2772` @ `120 Hz`  
**APK Build Built (`v54`):** `versionCode = 54`, `versionName = "2.1.0-ballistics-tracers"` (`42,966,668` bytes at `app/build/outputs/apk/release/app-release.apk`) — prior in-place device boot verified on `v53` (`ADB` currently disconnected during `v54` build).

---

## 1. Build, In-Place Install & Boot Verification

- **Gradle Release Build (`v54`, `assembleRelease`):** `BUILD SUCCESSFUL in 32s` (`49 actionable tasks: 18 executed, 31 up-to-date`), producing `app/build/outputs/apk/release/app-release.apk` (`42,966,668` bytes).
- **Pending `v54` Device Install:** `adb devices -l` returned empty when `v54` finished compiling; install via `adb install -r app/build/outputs/apk/release/app-release.apk` once the phone is reconnected.
- **Prior Baseline In-Place Install & Active Process Verification (`v53` on `damfmjij5pbe7dbu`, `PID = 23385`):**
  - `Game: default.xbe in .../assets/black.iso` (`1,686,306,816 bytes`, `574 files`, `sha1 c5bfed25449c8212d7a976f15246e87ead57713f`)
  - `GPU: Mali-G615 MC2, Vulkan 1.3.247, driver 0B001000`
  - `Low latency audio enabled`
  - `ui: FontAtlas build=1 tex=1024x2048 glyphs=1286` (`DroidSans-Bold.ttf` + `NotoNaskhArabic`)
  - Zero crashes (`adb logcat -b crash`).

---

## 2. Host Automated Verification Results

- **C++ Core Test Suite (`tests/build/test_settings_core.exe`):** `22 passed, 0 failed`
  - Covers `21_BallisticsSchemaAndPresets` (`original` / `performance` / `enhanced` / `custom`, clamping, TOML round-trip, cheat/control isolation) and `22_BallisticsWeaponClassAndFrequency` (`ClassifyWeaponCode`, `ShouldEmitTracer` modes `0..5`, suppressed cadence).
- **Python Verification Suite (`tests/test_settings_suite.py`):** `13 passed, 0 failed out of 13 tests`
  - Includes `Test 13: Ballistics Tracers Schema & Arabic Menu Coverage` verifying all 10 `[ballistics]` schema keys, UI bindings, and Arabic RTL translations.

---

## 3. Root-Cause Fixes Applied During Device Testing

1. **Xbox Virtual Address Space Rejection (`MemMapped` / `HookAddrMapped` in `src/game/ballistics_fx.cpp`, `src/game/hooks.cpp`, `src/game/time_stop.cpp`):**
   - `sub_001D2860` (`muzzle_mat`), `sub_001DB7B0` (`muzzle_ea`, `dir_ea`), `sub_00168E20` (`start_ea`, `end_ea`), and `fire_pkt` frequently reside on Xbox thread stacks (`0xD0000000+`) or system/contiguous regions (`0x80000000+` / `0xB0000000+`), which exceeded `kUserHigh (0x7FFE0000)`.
   - Replaced `< kUserHigh` guards with full 32-bit committed-page checks (`va >= 0x10000u && (uint64_t(va) + size) <= 0x100000000ULL && mem::IsCommitted(...)`).
2. **Release Build Telemetry Visibility (`src/game/ballistics_fx.cpp`):**
   - Discovered during `versionCode = 52` gameplay logs that `[BALLISTICS_FX]` telemetry used `LOGI(...)` (`Level::Info`), which Release builds (`NDEBUG`) omit (`xbe_sdk/runtime/core/log.h: // a release build writes Report and Error lines only`).
   - Switched telemetry output to `LOGR(...)` (`Level::Report`) in `versionCode = 53` so live Android Release diagnostics write `[BALLISTICS_FX]` every `3.0s` into `logcat` and `logs/black.log`.
3. **Arabic RTL Mod Menu Layout Clipping (`src/common/menu.cpp`):**
   - Visual inspection of `device_live.png` revealed the `RESTORE ORIGINAL BULLET EFFECTS` button overlapped vertically/clipped in 3-column Arabic RTL layout.
   - Separated preset buttons (`PERFORMANCE MODE` | `ENHANCED MODE`) onto a 2-column row above a full-width crimson `RESTORE ORIGINAL BULLET EFFECTS` button (`device_v52.png` confirmed clean Mod Menu rendering and live slider changes persisting to `black.toml`).
4. **Player First-Person Muzzle & World Depth Occlusion (`v54` — `src/game/ballistics_fx.cpp`, `src/d3d/d3d8.cpp`, `src/d3d/world_overlay.h`, `src/game/hooks.cpp`, `src/common/menu.cpp`):**
   - Fixed `LookupFirePktWeapon` treating `world + kWorldPlayer` (`0x30`) as a pointer instead of an inline struct (`out_is_player` was always `false`, leaving player physical origins collapsed at `[player+0x100]` eye / center crosshair `(cx, cy)`). Added off-axis `sub_001D2860` validation (`perp_sq > 0.0020f`) with view-space barrel offset fallback (`dir * 0.22f + right * 0.14f - up * 0.095f`, scaled `0.50x` under ADS zoom) and 2-node player muzzle smoke burst.
   - Replaced unoccluded `ImGui::GetBackgroundDrawList()` 2D drawing (`app::DrawUi`) with hardware depth-tested 3D fixed-function (`D3DFVF_XYZ | D3DFVF_DIFFUSE`) rendering via `d3d::DrawWorldOverlay` inside `sub_00176F60` (`kAlphaDrawList` post-pass, `ZENABLE=1`, `ZWRITE=0`, `ZFUNC=LESSEQUAL`, `CULL=NONE`) so walls, buildings, and cover occlude all tracers and smoke naturally.

---

## 4. Observed Device State, Configuration & Budgets

### Observed Device Screenshots & FPS HUD Readings
- **Title Screen (`device_fg.png`, `v52`):** `FPS: 62.4`, Arabic HUD/font crisp.
- **Loading Screen (`device_v53_boot.png`, `v53`):** `FPS: 60.5`, clean boot with Vulkan active.
- **Mod Menu — `[4] BALLISTICS & TRACERS` / `"المقذوفات والمسارات"` (`device_v52.png`, `v52`):** `FPS: 44.3` with live 3D scene behind menu (`011/030` ammo); verified Arabic sliders (`تكرار المسار: جميع الطلقات`, `طول مسار الطلقة: 160%`, `شدة إضاءة المسار: 110%`) and persisted `[ballistics]` state in `/sdcard/Android/data/com.racas.black.enhanced/files/black.toml`.

### Zero-Allocation Memory & Draw Budgets (CPU/GPU Protection)
| Preset | Max Simultaneous Tracers | Max Simultaneous Smoke Nodes | Default Frequency | Default Smoke Mode |
|---|---|---|---|---|
| **Original (`original`)** | `0` (disabled) | `0` (disabled) | `0` (`Off`) | `0` (`No Smoke`) |
| **Performance (`performance`)** | `32` (static `std::array` slice) | `64` (static `std::array` slice) | `3` (`Every 3rd Bullet`) | `1` (`Light`, `0.65s`, `70%`) |
| **Enhanced (`enhanced`)** | `96` (full static ring buffer) | `192` (full static ring buffer) | `1` (`Every Bullet`) | `2` (`Medium`, `1.10s`, `105%`) |

### Testing Boundary Note (`INJECT_EVENTS`)
- Automated touchscreen/key injection via `adb shell input` is blocked by Xiaomi HyperOS (`SecurityException: Injecting input events requires ... INJECT_EVENTS`), so interactive mission gunfire under `versionCode = 53` (`LOGR` enabled) requires triggering shots directly on the touchscreen while reading live `[BALLISTICS_FX]` telemetry via `adb logcat -s BLACK` or in the `LIVE BALLISTICS TELEMETRY` Mod Menu panel.
