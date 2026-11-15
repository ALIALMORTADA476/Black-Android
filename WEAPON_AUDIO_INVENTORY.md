# BLACK Enhanced Edition — Weapon & Combat Audio Inventory

This document catalogs the original Xbox audio architecture, wave banks, sampling specifications, runtime DirectSound dispatch path, and hook points observed in `e:\ai\black` and `assets/black.iso` (`1,686,306,816` bytes, XDVDFS root sector `264`, `574` total files, `323` audio/weapon bank files).

---

## 1. Host & Guest Audio Engine Architecture

| Component | File / Address | Observed Behavior |
|---|---|---|
| **Host Mixer & Output** | `src/sound/mixer.cpp`, `native/audio.cpp` | Renders at `48000 Hz` into 6-channel speaker layout (`FL, FR, C, Sub, BL, BR`), feeds mono reverb send bus `send` (`kSends = 1 << 11`, mix bin 11) into `g_reverb.Run()`, then applies optional `combat_dynamic_boost` (`tanh`) and `master_gain_percent` clamping (`[10, 150]%`). |
| **DirectSound HLE Buffers** | `src/sound/buffers.cpp`, `src/sound/sound.h` | `IDirectSoundBuffer_SetBufferData` (`sub_0021A7FD`), `Play` (`sub_00219773`), `Stop`, `StopEx`, `SetVolume`, `SetFrequency`, `SetPosition`, `SetAllParameters`. Tracks active `Voice` instances in `sound::g_voices` protected by `sound::g_lock`. |
| **DSP / Reverb** | `sound/dsp/BkXboxFx.bin` (`23,780` bytes) | Hardware Xbox DSP image; host runs I3DL2 room/outdoor reverb inside `g_reverb.Run(send.data(), out, frames)` at `24 kHz`. |
| **Player Weapon Fire Hook** | `src/game/hooks.cpp` (`sub_001EEFD0`), `generated/game/x86_recomp.52.cpp` | Detects exact per-round firing (`Rounds(weapon) < rounds`), weapon struct pointer (`+0x00` `kWeaponType`, `+0xD8` `kWeaponAnim`, `+0xEC` `kWeaponData`, `+0xF4` `kWeaponMagazine`, `+0xF8` `kWeaponAltAmmo`, `+0x108` `kWeaponSuppressed`). Calls guest sound triggers `sub_00122330` / `sub_0012F810`. |
| **Animation & Foley Clips** | `src/game/hooks.cpp` (`sub_0017B130`) | Tracks animation state (`ac + 0x5C` clip name, `kFireLoopHash = 0x6D9D2AC0`, fire clips ending `S_100`, `S_110`, `S_114`, `S_130`, `S_170`, `S_176`). |
| **Enemy Gun Hold / Spatial** | `src/game/hooks.cpp` (`sub_0013FC20`), `generated/game/x86_recomp.33.cpp` | Controls enemy gunfire loop hold durations and 3D spatialized sound emitters. |
| **Low-level Wave Submit** | `generated/game/x86_recomp.14.cpp` (`0x0008E2A6`) | Passes `[ebx + 0x90]` RenderWare Wave struct sample pointer (`+0x48`) and byte size (`+0x4C`) straight into `IDirectSoundBuffer_SetBufferData` (`sub_0021A7FD`). |

---

## 2. Original ISO Sound Bank Formats

BLACK uses Criterion RenderWare Audio (`0x809`) wave dictionaries (`GUID = 042d3a455fe4c84b81f0df758b01f273`), stored either standalone (`.awd`) or embedded inside first-person weapon archives (`Levels/Level_NN/fpguns/bg1_<weapon>.db` header + `.wdd` raw wave payload):

- **Format `0x120104` (Xbox IMA ADPCM 4-bit)**: Used by all 31 `fpguns/bg1_*.db` + `.wdd` files (`36` bytes per mono block -> `64` frames). First-person gunfire loops (`*LF` / `*sLF`) are all capped at **`16,000 Hz`** (`8 kHz` Nyquist limit). Reloads/cocking range from `20,000 Hz` to `44,100 Hz`.
- **Format `0x120110` (16-bit Signed Mono PCM)**: Used by `sound/banks/Anim.awd`, `AIWpns.awd`, `Level.awd`, and `Collide.awd` (`11,025 Hz` to `48,000 Hz`).

### Baseline Acoustic Measurements of Original First-Person Gunfire (`black.iso`)

Decoded directly via Xbox IMA ADPCM in Python + FFT analysis:

| Weapon Bank | Sample Rate | Duration | Peak Level | RMS Level | Spectral Centroid | `< 150 Hz` Energy | `> 4 kHz` Energy |
|---|---|---|---|---|---|---|---|
| `bg1_ak1` (`bg1_ak1LF`) | `16,000 Hz` | `1.59 s` | `0.0 dBFS` | `-14.2 dBFS` | `2156 Hz` | `45.6%` | `3.6%` |
| `bg1_pst` (`bg1_pstLF`) | `16,000 Hz` | `1.72 s` | `-0.1 dBFS` | `-9.8 dBFS` | `1472 Hz` | `70.3%` | `0.4%` |
| `bg1_shg` (`bg1_shgLF`) | `16,000 Hz` | `1.85 s` | `0.0 dBFS` | `-8.8 dBFS` | `1882 Hz` | `51.1%` | `1.6%` |
| `bg1_snr` (`bg1_snrLF`) | `16,000 Hz` | `2.04 s` | `0.0 dBFS` | `-9.3 dBFS` | `1996 Hz` | `45.0%` | `2.3%` |

**Key Deficiency Identified**: Original weapon fire waves hit `0.0 dBFS` brickwall peaks with heavily compressed `-8.8` to `-9.8 dBFS` RMS, lack ultrasonic/high-frequency transient detail above `4 kHz` (`0.4%–3.6%` energy) with a hard `8 kHz` Nyquist ceiling, and stitch rapid-fire shots into fixed `~1.0–2.0 s` looping buffers (`*LF`) rather than dynamic per-round multi-layer transients.

---

## 3. Complete 17-Weapon & Variant First-Person Audio Inventory (`fpguns/*.db` / `.wdd`)

| Weapon Code | Silver Variant | Weapon Identity | Fire Wave (`16 kHz`) | Suppressed Wave (`16 kHz`) | Mechanical / Reload Waves (`20–44.1 kHz`) |
|---|---|---|---|---|---|
| `PST` | `PS5` | DC9 / Browning-style Pistol | `bg1_pstLF` / `bg1_ps5LF` (`1696 ms`) | `bg1_pstsLF` (`372 ms`) | `PstUnld0` (`502ms`), `PstLd0` (`485ms`), `PstLd0b` (`314ms`), `PstCockP1` (`400ms`), `PstBrch0` (`302ms`), `PstBbrch1` (`284ms`), `PstFire` (`184ms`) |
| `GK1` | `GK5` | Glock 19 Pistol | `bg1_gk1LF` / `bg1_gk5LF` (`1148 ms`) | `bg1_gk1sLF` (`292 ms`) | `GkUnld0` (`238ms`), `Gkld0` (`446ms`), `GkCock0` (`240ms`) |
| `MGN` | `MG5` | .357 Magnum Revolver | `bg1_mgnLF` / `bg1_mg5LF` (`1696 ms`) | *(fallback `bg1_sm3LF`)* | `MgnUnLd0` (`251ms`), `MgnLd0..3` (`168–312ms`) |
| `SMG` | `SM5` | Uzi Submachine Gun | `bg1_smgLF` (`1000ms`), `bg1_sm5LF` (`156ms`) | `bg1_smgsLF` (`180 ms`) | `Sbmgn-UnLd0` (`424ms`), `Sbmgn-Ld0` (`498ms`), `Sbmgn-Cock` (`596ms`) |
| `SM3` | `MC5` | MAC-10 Submachine Gun | `bg1_sm3LF` (`408ms`), `bg1_mc5LF` (`180ms`) | `bg1_sm3sLF` (`200 ms`) | *(shared `Sbmgn-UnLd0`, `Sbmgn-Ld0`, `Sbmgn-Cock`)* |
| `MP1` | `MP5` | H&K MP5 | `bg1_mp1LF` / `bg1_mp5LF` (`1028 ms`) | `bg1_mp1sLF` (`236 ms`), `bg1_sm3sLF` (`200 ms`) | `Mp1Unld0` (`389ms`), `Mp1Ld0` (`256ms`), `Mp1Cock0` (`181ms`), `FireSelect` (`128ms`) |
| `P90` | `P95` | FN P90 PDW | `bg1_p90LF` / `bg1_p95LF` (`804 ms`) | `bg1_p90sLF` (`180 ms`) | `P90Unld0` (`456ms`), `P90Ld0` (`301ms`), `P90Ld1` (`365ms`), `P90Cock0` (`576ms`) |
| `AK1` | `AK5` | AK-47 Assault Rifle | `bg1_ak1LF` / `bg1_ak5LF` (`1564 ms`) | `bg1_ak1sLF` (`232 ms`) | `AkUnld0` (`259ms`), `AkLd0` (`378ms`), `AkLd1` (`256ms`), `AkCock0` (`304ms`), `AkCock1` (`312ms`), `SbmgnPin0/1` (`194/116ms`) |
| `ASR` | `AS5` | G36C Assault Rifle | `bg1_asrLF` (`1468ms`), `bg1_as5LF` (`1096ms`) | `bg1_asrsLF` (`276 ms`) | `AssRfl-UnLd0` (`446ms`), `AssRfl-Ld0` (`526ms`), `AssRfl-Cock` (`560ms`), `FireSelect` (`128ms`), `AssRflFire` (`154ms`) |
| `M16` | `M45` | M16A2 Assault Rifle | `bg1_m16LF` / `bg1_m45LF` (`1568 ms`) | `bg1_m16sLF` (`412 ms`) | `M16UnLd0` (`301ms`), `M16Ld0` (`384ms`), `M16Ld1` (`400ms`), `M16Cock0` (`784ms`), `FireSelect` (`128ms`) |
| `BNS` | — | M16A2 + M203 Grenade Launcher | `bg1_bnsLF` (`1568 ms`, shared M16 + M203 fire) | `bg1_m16sLF` (`412 ms`) | *(shared `M16UnLd0`, `M16Ld0/1`, `M16Cock0`, `FireSelect`)* |
| `SHG` | `SH5` | Franchi SPAS-12 Shotgun | `bg1_shgLF` / `bg1_sh5LF` (`1820 ms`) | *(fallback `bg1_sm3LF`)* | `Shtg01-Cock0` (`552ms`) |
| `RM1` | `RM5` | Remington 870 Pump Shotgun | `bg1_rm1LF` / `bg1_rm5LF` (`1564 ms`) | *(fallback `bg1_sm3LF`)* | `ShtgRm-Cock0` (`225ms`), `ShtgRm-Cock1` (`240ms`), `ShtgRm-Ld0` (`237ms`) |
| `SNR` | `SN5` | Walther WA 2000 Sniper Rifle | `bg1_snrLF` / `bg1_sn5LF` (`2004 ms`) | *(fallback `bg1_sm3LF`)* | `SnpRfl-Unld0` (`454ms`), `SnpRfl-Ld0` (`355ms`), `SnpRfl-Cock0` (`342ms`), `SnpRflrCap` (`344ms`), `Silencer_On0/1` (`285/344ms`) |
| `HVY` | `HV5` | M249 SAW LMG | `bg1_hvyLF` / `bg1_hv5LF` (`1188 ms`) | *(fallback `bg1_sm3LF`)* | `HvMgnLd0` (`1466ms`), `HvMgnULd0` (`291ms`), `HvMgnULd1` (`237ms`), `HvMgnCk1` (`573ms`) |
| `RPG` | — | RPG-7 Rocket Launcher | `bg1_rpgLF` (`2016 ms`) | — | `Rpg-Ld0` (`810ms`), `Rpg-Attach0` (`522ms`) |
| `GRL` | — | M203 Standalone Grenade Launcher | `bg1_grlLF` (`1044 ms`) | — | `Grdlch-Cock` (`733ms`), `Grdlch-Case0` (`824ms`), `Grdlch-C0` (`432ms`), `Grdlch-St0` (`216ms`) |

---

## 4. Shared World, Enemy, Mechanical, Environmental & Impact Banks (`.awd`)

Across all `8` playable levels (`level_00`, `level_01`, `level_03`..`level_08`):

| Bank File | Scope | Key Wave Slots Requiring Remaster |
|---|---|---|
| `sound/banks/Anim.awd` (`29` entries) | First-person handling & suppressor | `Silencer_On0`, `Silencer_On2`, `Silencer_Of0`, `Silencer_Of1` (`24 kHz`), `ChngWpn0/1/2` (`20 kHz`), `meleeGun` (`24 kHz`) |
| `levels/level_NN/stg_0001/AIWpns.awd` (`107` unique names) | Enemy weapon fire & distant fire | `Distant0` (`16 kHz, 27 ms`), `Pstl0` (`20 kHz`), `PstlDst0` (`20 kHz`), `Shtg0` (`20 kHz`), `MgnDst0/1/2` (`16 kHz`), `Rpg0` (`20 kHz`), `Snpr0` (`24 kHz`), `CarrieSnpr` (`24 kHz`), `Silenced0` (`24 kHz`), `MCHNGN[S0/M0..3/E0]`, `SBMCHGN[S0/M0..3/E0]`, `E_Mac10_*`, `E_Uzi_*`, `E_Hvy_*`, `E_BlackHd_*`, `E_Commando_*`, `E_Navy_*`, `E_Rock_*`, `E_Alias_*`, `E_DieHard2_*`, `E_KarlDH_*`, `E_LKiss_*`, `E_Lkiss2_*`, `E_WeWere_*` |
| `levels/level_NN/Level.awd` (`47` unique names) | Empty click, flybys, tails, explosions | `DeadClick` (`32 kHz`), `BulletByS0..5` (`20 kHz, 296–393 ms`), `RpgPass0..2` (`24 kHz`), `Tail1` / `Tail2` (`11,025 Hz` lo-fi!), `EXPLOWQ0`, `EXPBASS` (`16 kHz`), `sil_pkp`, `PckUpAm0`, `Pckup0` |
| `levels/level_NN/Collide.awd` (`238` unique names) | Surface bullet impacts, debris, casings | `CONCRETE__*` / `PTCLSTN0__*` / `PTCLMSN*`, `METAL__*` / `METAL1/3/4*`, `WOOD__*` / `PTCLWOD0__*`, `GLASS__*` / `SHATTER__*`, `RICOGEN0__*` (`20 kHz`), `GENSHE01__*` (shell casings, `22,050 Hz`), `DIRT__*`, `FLESH__*`, `EXPLODE__*`, `GRENADELAN*`, `GRENADE___11` |

Total unique `(size, fnv64)` wave payload signatures cataloged across the entire ISO: **`448` distinct payloads** out of `2,193` dictionary entries, enabling O(1) deterministic runtime wave classification inside `IDirectSoundBuffer_SetBufferData`.
