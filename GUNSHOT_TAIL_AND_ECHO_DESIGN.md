# BLACK Enhanced Edition — Multi-Layer Gunshot Tail ("Mattak") & Environmental Echo Design

## 1. Acoustic Philosophy: Real Battlefield Firearm Rendering

Traditional video game audio plays a single compressed 1D buffer (`0.2s–0.5s`) per trigger pull, resulting in dry, flat, fatiguing firefights. Modern military shooters such as **SQUAD** derive their realism from **multi-stage physical propagation**:

1. **Direct Muzzle Report (`0–45 ms`)**: High-transient close microphone blast (`1m–4m`) carrying pressure crack, receiver punch, and low-frequency chest impact.
2. **Primary Weapon Identity (`25–450 ms`)**: Action/caliber signature (`7.62x39mm` piston thump vs `5.56mm` NATO snap vs `.338 Lapua` / `.50` shockwave vs `12g` bore boom).
3. **Rolling Outdoor Gunshot Tail ("Mattak", `25–2500 ms`)**: Authentic open-area, valley, forest, and mountain decay (`2783` AK5 Valley Field, `3086` AK47 Burst Tail, `1008` AKM 55m, `5034` MG42 Mountain Top, `1852` Warfare Large Field Tail, `4935` M1911 200m, `6203` Glock 100m).
4. **Environmental Reflections & Room Echo (`35–1800 ms`)**: Real corridor, concrete hall, and catacomb acoustic reflections (`2780` AK4 Long Corridor, `2779` AK47 Catacombs, `6964` Mossberg Office Corridor, `6965` MP5 Large Hall) coupled dynamically into BLACK's hardware-emulated **24 kHz I3DL2 DSP Reverb (`sound::g_reverb`)**.
5. **Far-Field Distant Gunfire (`50–300 m`)**: Distance-attenuated, air-absorbed enemy/battlefield fire (`1073` M3 300m, `5041` MP40 250m, `1245` M16 Distant, `3087` XM8 Distant Exterior, `1851` Cannon Forest Distant).
6. **Event-Gated Supersonic Cracks & Flybys**: Whip-sharp overhead bullet cracks and Doppler whizzes (`1853` Warfare M10 Whizzes & Cracks, `4847`, `1606`, `2514`) triggered **strictly** on genuine incoming bullet flyby (`ENV_FLYBY`) or ricochet (`IMP_RICOCHET`) events — never falsely attached to the shooter's own fired rounds.

---

## 2. Layered Trigger Pipeline in `src/sound/weapon_audio.cpp`

### A. Player Weapon Fire (`OnPlayerFire` -> `TriggerPlayerWeaponLayersLocked`)

Every round fired spawns three sample-aligned, independently gain-controlled voices across the `72`-voice lock-protected mixer pool:

| Layer | Stem Category | BWAR Slot Routing | Pre-Delay (`@48kHz`) | `owner_voice_obj` | Non-Truncation & Room Adaptation |
|---|---|---|---|---|---|
| **1. Primary Report** | `StemCategory::Weapon` | Weapon's unique slot (`0..16`, `17..26`, `95`) | `0` samples (`0 ms`) | `voice_obj` (`0` on `sub_001EEFD0`) | Fast `12 ms` fade if loop stops (`OnVoiceStop`) |
| **2. Rolling Tail ("Mattak")** | `StemCategory::GunshotTail` | `TailSlotForPlayerSlot()`: `97` Pistol, `98` SMG, `99` Rifle Mattak, `100` Heavy/MG/Sniper Mattak, `104` Shotgun, `105` Suppressed Crack | `640` (`13.3 ms` supp.) / `960` (`20 ms` pistol/SMG) / `1200` (`25 ms` rifle/heavy/shotgun) | **`0` (detached)** | **Never cut off by `OnVoiceStop()`**: rings out `1.3s–2.5s` naturally after full-auto trigger release |
| **3. Environmental Echo** | `StemCategory::EnvironmentalEcho` | `101` (`TAIL_INDOOR_ECHO`) | `1680` (`35 ms`) | **`0` (detached)** | Reads `g_reverb.on` & `g_reverb.now.late` via `SetEnvironmentState()`: `0.24x` outdoors vs `0.52x–1.30x` indoor rooms + `0.48` reverb send into `g_reverb.Run()` |

### B. Enemy AI & Distant Gunfire (`OnVoicePlay`)

- AI weapon loops/single shots (`slots 74..78, 80..83`) map to `StemCategory::DistantGunfire`.
- Using cached listener coordinates (`g_state.listener_pos`), `Estimate3DDistance()` checks 3D emitter distance:
  - When `dist > 14.0f` and `audio_distant_gunfire > 0`, `OnVoicePlay()` spawns an aligned far-field tail (`1152` samples / `24 ms` offset) choosing **`kSlotTailRifleMattak (99)`** (`14m–45m`) or **`kSlotDistantBattlefield (102)`** (`> 45m`).
  - `Compute3DGainAndPan()` extends max rolloff ceiling (`>= 240.0f` meters) for `DistantGunfire` & `GunshotTail` voices, while `RenderMix()` applies a per-voice 1-pole low-pass air-absorption filter (`air_alpha = clamp(18.0f / (dist + 6.0f), 0.16f, 1.0f)`) above `12m` so far-away firefights sound naturally diffused and atmospheric.

### C. Supersonic Bullet Cracks & Flybys

- Triggered **only** when the game emits `ENV_FLYBY` (`slot 85`) or alternating `IMP_RICOCHET` (`slot 91`) buffers (`StemCategory::BulletFlyby`).
- Layers `kSlotEnvSupersonicCrack (103)` at the projectile's 3D vector (`min_dist >= 4.0f`, `max_dist >= 95.0f`), controlled directly by the **Bullet Flyby** (`audio_bullet_flyby`) slider.

### D. Clip-Free Polyphony Bus Protection

To prevent digital harshness when `72` simultaneous voices (`Weapon` + `GunshotTail` + `EnvironmentalEcho` + `DistantGunfire` + `Explosion`) stack with sliders pushed to `150%`, `RenderMix()` passes any channel excursion above `±0.88` linear through a smooth `tanh` soft-knee safety curve (`SoftClipCeiling`: `0.88f + 0.12f * tanhf((x - 0.88f) / 0.12f)`).
