# BLACK Enhanced Edition — Audio Integration & Mixing Architecture Report

## 1. End-to-End Runtime Audio Pipeline

`BLACK Enhanced Edition` replaces the game's compressed `16 kHz` Xbox IMA ADPCM first-person weapon loops and low-bitrate environmental/impact `.awd` effects dynamically at runtime without altering `assets/black.iso` or `assets/default.xbe`:

```
  [hooks.cpp: sub_001EEFD0 Player Fire]
         │ (code, suppressed, alt_fire — zero latency on ammo decrement)
         ▼
  [weapon_audio::OnPlayerFire]
         ├──► Layer A (0 ms)   : Primary Close Report (StemCategory::Weapon)
         ├──► Layer B (13–25ms): Rolling Tail / "Mattak" (StemCategory::GunshotTail, owner=0)
         └──► Layer C (35 ms)  : Indoor/Outdoor Reflections (StemCategory::EnvironmentalEcho, owner=0)

  [buffers.cpp: IDirectSoundBuffer_Play]
         │ FNV-1a 64-bit payload lookup (323 ISO signatures -> 106 slots)
         ├──► FP Fire Loops (flags bit 0) -> Muted in HLE buffer; synthesized only if 60ms fallback needed
         ├──► AI Gunfire (slots 74..78, 80..83) -> StemCategory::DistantGunfire + Far-Field Layer (>14m)
         ├──► Bullet Flyby / Ricochet (slots 85, 91) -> StemCategory::BulletFlyby + Supersonic Crack (103)
         ├──► Reload / Mechanicals (slots 27..73, 96) -> StemCategory::Reload / Mechanical
         └──► Impacts / Explosions (slots 86..90, 92..94) -> StemCategory::Environment

  [mixer.cpp: Render()]
         │ weapon_audio::SetEnvironmentState(g_reverb.on, g_reverb.now.late)
         ▼
  [weapon_audio::RenderMix(..., out, send, ...)]
         │ 72-voice polyphony · 3D pan & extended distance rolloff · 1-pole air absorption >12m
         │ Soft-knee bus ceiling above ±0.88 linear · Wet reverb send injection
         ▼
  [g_reverb.Run(send, out, frames)] ──► [combat_dynamic_boost] ──► [master_gain_percent] ──► [AAudio Output]
```

---

## 2. Modified C++ Source Modules

| File | Key Integrations & Symbols |
|---|---|
| [`src/sound/weapon_audio.h`](file:///e:/ai/black/src/sound/weapon_audio.h) | Expanded `StemCategory` to all `8` independent tactical stems (`Weapon`, `Mechanical`, `Reload`, `Environment`, `GunshotTail`, `EnvironmentalEcho`, `DistantGunfire`, `BulletFlyby`); added `SetEnvironmentState(bool, float)` and `Telemetry::{tails_enhanced, distant_enhanced, flybys_enhanced}`. |
| [`src/sound/weapon_audio.cpp`](file:///e:/ai/black/src/sound/weapon_audio.cpp) | Upgraded to **BWAR v2** (`kExpectedVersion = 2`, `48000 Hz`, `106` slots, `kMaxActiveVoices = 72`). Added `delay_samples` alignment, `TriggerPlayerWeaponLayersLocked()`, `TailSlotForPlayerSlot()`, `Estimate3DDistance()`, adaptive indoor/outdoor reflection scaling via `env_reverb_active` / `env_reverb_late`, event-gated `kSlotEnvSupersonicCrack` on `ENV_FLYBY`/`IMP_RICOCHET`, and `SoftClipCeiling()` polyphony limiter. |
| [`src/sound/mixer.cpp`](file:///e:/ai/black/src/sound/mixer.cpp) | Calls `weapon_audio::SetEnvironmentState(g_reverb.on, g_reverb.now.late)` every audio block directly before `weapon_audio::RenderMix(...)` and `g_reverb.Run(...)`. |
| [`src/common/mod_schema.h`](file:///e:/ai/black/src/common/mod_schema.h) & [`src/common/mod_schema.cpp`](file:///e:/ai/black/src/common/mod_schema.cpp) | Added `SettingId::{AudioTailIntensity, AudioEchoIntensity, AudioDistantGunfire, AudioBulletFlyby}` (`audio_tail_intensity`, `audio_echo_intensity`, `audio_distant_gunfire`, `audio_bullet_flyby`), `[0, 150]%` step `5`, default `100`, `Implemented`, `Immediate`, persisted under `[audio]` in `settings.toml`. |
| [`src/common/menu.cpp`](file:///e:/ai/black/src/common/menu.cpp) | Added live sliders `GUNSHOT TAIL INTENSITY`, `ENVIRONMENTAL ECHO`, `DISTANT GUNFIRE`, and `BULLET FLYBY` inside `DrawCategoryAudioLanguage()`, expanded `RestoreOriginalAudio()` & `RestoreAllToOriginalGame()`, and updated telemetry readout (`Shots / Tail / Dist / Fly`). |
| [`src/common/translations.cpp`](file:///e:/ai/black/src/common/translations.cpp) | Added full Arabic translations for the 4 new tactical sliders (`حدة ذيل الطلقة والامتداد`, `صدى البيئة والانعكاسات`, `صوت الاشتباكات البعيدة`, `أزيز واختراق الرصاص`). |
| [`app/build.gradle`](file:///e:/ai/black/app/build.gradle) | Bumped `versionCode` `50 -> 51`, `versionName "2.0.0-mattak-remaster"`, bundling `app/src/main/assets/weapon_audio/black_weapon_audio.pak`. |

---

## 3. Original Audio A/B Switch & Zero-Destruction Guarantees

- Switching **`WEAPON AUDIO MODE`** (`mod_audio_pack_enabled`) to **`ORIGINAL`** or pressing **`RESTORE ORIGINAL AUDIO`** immediately calls `sound::weapon_audio::StopAll()`, resets all stem levels (`Weapon`, `Mechanical`, `Reload`, `Tail`, `Echo`, `Distant`, `Flyby`, `Environment`, `Master`) to `100%` (`AudioVariation = 50%`), disables enhancement in `OnPlayerFire()` / `OnVoicePlay()`, and restores BLACK's untouched `assets/black.iso` audio streams seamlessly without restart.
- `OnVoiceStop(voice_obj)` fades only `StemCategory::Weapon` voices bound to `voice_obj` (`12 ms`), preserving detached (`owner_voice_obj = 0`) `GunshotTail` and `EnvironmentalEcho` voices so outdoor gunfire rolls out completely even after rapid trigger release.
