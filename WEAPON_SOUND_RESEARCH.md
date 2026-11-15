# BLACK Enhanced Edition — Weapon Sound Research & Acoustic Evaluation

This report documents the independent search, license verification, downloading, and objective Python + `ffmpeg` + `numpy` (`48,000 Hz` single-channel PCM float32) acoustic evaluation of candidate firearm, mechanical, environmental, flyby, and surface impact recordings for BLACK Enhanced Edition.

---

## 1. Verified Open-License Sound Sources (`CC0-1.0` / Public Domain)

No paywalls, access-controlled portals, or restricted libraries were bypassed. Every integrated asset comes strictly from verified `CC0-1.0` / Public Domain sources:

1. **FreeFirearmsSFXLibrary (`buddingmonkey/FreeFirearmsSFXLibrary`)** — License: **`CC0-1.0`**
   - `780` studio-grade `24-bit` WAV recordings (`44.1 / 48 / 96 kHz`) captured from real live firearms across near, mid, far, behind-shooter, and suppressed indoor/outdoor microphone positions, plus close-mic magazine, bolt, slide, hammer, pump, and selector mechanics.
   - Weapons utilized: **M1911 (`.45 ACP`)**, **Walther PPQ (`9mm` & Suppressed `9mm`)**, **Smith & Wesson 642 (`.38 Special` revolver mechanics & shot layer)**, **Carl Gustav M45 / Swedish K (`9mm` SMG)**, **PPSh (`7.62x25` high-ROF SMG)**, **AK-47 (`7.62x39`)**, **Norinco SKS (`7.62x39` & suppressed)**, **AR-15 (`5.56x45`)**, **Benelli Nova (`12-gauge` pump)**, **Winchester Model 12 (`12-gauge` pump)**, **Charles Daly (`12-gauge`)**, **Tikka Model T3 (`.30-06`)**, **Mosin Nagant (`7.62x54R`)**, **Savage Model 10 (`.300 AAC Blackout` & suppressed)**, **Ruger 10/22 (suppressed indoor bolt/muzzle)**, **Marvel Suppressed `.22`**, **Bersa `.380`**.
2. **Freesound Public CC0 Previews (`freesound.org`, Creative Commons 0)** — License: **`CC0-1.0` (verified per sound page `creativecommons.org/publicdomain/zero/1.0`)**
   - Downloaded `-hq.mp3` (`44.1 / 48 kHz`) verified recordings to cover firearms and impact classes absent from studio rifle ranges: **`.357 Magnum` revolver** (`683186`, `683175`), **`H&K MP5`** (`201672`, `593888`, `616757`), **`FN P90`** (`418431`), **`Uzi` full reload** (`677160`), **`M60 / M249 LMG`** (`482122`, `538302`), **`M2010 ESR` field sniper** (`855608`, `524738`, `524739`), **`RPG-7` launch & rocket load** (`249298`, `725401`), **`M203` 40mm grenade launcher** (`163458`), **real bullet flyby & whiz** (`855248`, `789222`), **brass & 12-gauge shell casings** (`210102`, `205126`, `318964`, `147261`), **bullet ricochets** (`478345`, `30932`, `523403`), **concrete/dirt/metal/wood/glass impacts** (`683774`, `426850`, `321478`, `240463`, `406562`, `873248`, `481936`, `453499`, `721363`, `595487`), and **explosions** (`459973`, `523783`).
3. **CC0 Public Domain Sounds (`lavenderdotpet/CC0-Public-Domain-Sounds`)** — License: **`CC0-1.0`**
   - `48,000 Hz` OGG combat/foley variations for glass (`glass_01..03.ogg`), metal (`metal_01..03.ogg`, `metal_hit_01..03.ogg`), wood (`wooden_01..02.ogg`), and concussive blast (`explosion.ogg`).

*(Note: `Deases/GunLib` `LICENSE` was also verified as `CC0 1.0 Universal`, though studio 24-bit WAV + Freesound HQ CC0 candidates provided higher fidelity and cleaner transient dynamics.)*

---

## 2. Objective Acoustic Evaluation Methodology

Across **`162` downloaded candidate recordings** (`e:\ai\black\.local\audio_src\`, gitignored), every asset was decoded at `48,000 Hz` float32 without stereo `L+R` summing overload (`decode_best_channel` + `28 Hz` zero-phase subsonic trim), sliced into clean single takes (`find_slices`), and measured for:

- **Peak Level (`dBFS`)** & **Clipped Sample Count** (`|x| >= 0.999`)
- **RMS Level (`dBFS`)** & **Crest Factor (`Peak - RMS dB`)** — authentic un-squashed live gunfire retains `14–30 dB` crest
- **10–90% Attack Rise Time (`ms`)** — real close gunfire transients rise in `< 2.5 ms`
- **Spectral Centroid (`Hz`)** & **4-Band Energy Split** (`Sub 25–150 Hz`, `Body 150–1000 Hz`, `Crack 1–5 kHz`, `Air >5 kHz`)

---

## 3. Accepted High-Realism Recordings vs. Rejected Candidates

### Rejected Candidates (Observed Acoustic Defects)

| Candidate Source | Reason for Rejection (Measured Evidence) |
|---|---|
| `freesound/165394_heavy_machine_gun.mp3` | **Severe brickwall clipping**: `42,160` clipped samples, RMS `-2.15 dBFS`, crest collapsed to `7.1 dB`. |
| `freesound/191289_m249_machine_gun.mp3` | **Severe brickwall clipping**: `14,957` clipped samples, RMS `-2.60 dBFS`, crest `6.9 dB`. |
| `freesound/397657_m249_machine_gun.mp3` | **Distorted & sub-heavy**: `1,704` clipped samples, `52.6%` sub-bass mud. |
| `freesound/390663_submachine_gun.mp3` | **Heavy MP3 overload**: `2,171` clipped samples on peaks. |
| `freesound/381569` & `369781` (Silenced) | **Over-compressed/clipped**: `1,017–1,152` clipped samples; replaced by clean `24-bit` FFX `Walther PPQ X_29/X_43` & `Freesound 693855`. |
| `freesound/453499` Slice `[0]` | **Pre-clipped take**: `rms -8.06 dBFS`, crest `9.0 dB`; Slice `[1]` (`crest 17.6 dB`, `air 49.1%`) and `721363` accepted instead. |
| `ffx/Master Tracks/Model 12/K_6.wav` | **Near-empty file**: total active duration only `18.1 ms`, peak `-51.97 dBFS`; replaced by `K_5.wav` & `Nova/O_6.wav`. |

### Selected Core Recordings (Sample of Verified Clean Slices at `48 kHz`)

| Weapon / Role | Source Track (License `CC0-1.0`) | Orig Format | Slice Dur (`ms`) | Crest (`dB`) | Attack (`ms`) | Centroid (`Hz`) | Acoustic Character |
|---|---|---|---|---|---|---|---|
| `PST` / `PS5` (`.45` Pistol) | `ffx/1911/A_41.wav`, `A_42P.wav`, `A_35.wav` | `24-bit WAV` | `376–1186` | `25.3–30.6` | `0.02–0.69` | `946–1425` | Heavy `.45 ACP` punch + rear report |
| `GK1` / `GK5` (`9mm` Pistol) | `ffx/Walther PPQ/X_39P.wav`, `X_41.wav`, `freesound/855652` | `24-bit WAV` / `44.1k` | `373–678` | `25.7–27.1` | `0.02` | `1096–1168` | Crisp polymer `9mm` snap + forest tail |
| Suppressed Pistols & SMGs | `ffx/Walther PPQ/X_29.wav`, `X_43.wav`, `freesound/693855` | `24-bit WAV` / `44.1k` | `189–539` | `18.8–35.5` | `0.02–2.00` | `4219–6710` | Dry mechanical baffle *thwip* + slide clack |
| `MGN` / `MG5` (`.357` Mag) | `freesound/683186`, `683175` + `ffx/S&W 642 V_27P` + `Mosin M_21P` | `44.1k` / `24-bit WAV` | `456–1400` | `21.6–24.1` | `0.02–2.58` | `591–1119` | Massive magnum concussive chest slam + tail |
| `SMG` / `SM5` (`Uzi 9mm`) | `ffx/Carl Gustav M45/G_25.wav`, `G_31P.wav`, `G_33P.wav` | `24-bit WAV` | `83–1150` | `16.7–30.5` | `0.02–0.44` | `1503–1954` | Open-bolt `9mm` metallic chatter & tail |
| `SM3` / `MC5` (`MAC-10`) | `ffx/PPSh/P_30P.wav`, `P_32P.wav` + `G_33P` | `24-bit WAV` | `71–809` | `16.1–28.4` | `0.02–0.04` | `1151–1546` | High-cyclic compact SMG mechanical buzz |
| `MP1` / `MP5` (`H&K MP5`) | `freesound/201672_mp5_down_block`, `593888`, `616757` + `X_41` | `48k` / `24-bit WAV` | `45–85` (+ tail) | `11.7–27.0` | `0.60` | `2087–2960` | Tight roller-delayed `9mm` midrange bark |
| `P90` / `P95` (`FN P90`) | `freesound/418431_fnp90_burst` + `ffx/PPSh P_32P` + `D_31` | `44.1k` / `24-bit WAV` | `39–276` | `10.6–28.0` | `0.02–0.25` | `1948–2987` | Rapid `5.7x28mm` staccato velocity snap |
| `AK1` / `AK5` (`AK-47`) | `ffx/AK-47/C_25.wav`, `C_28P.wav`, `C_29P.wav`, `C_39.wav` | `24-bit WAV` | `75–883` | `14.4–29.6` | `0.02–0.12` | `1071–1503` | Deep `7.62x39` bark + steel carrier clank |
| `ASR` / `AS5` (`G36C`) | `ffx/SKS/U_14P.wav` + `ffx/AR-15/D_31.wav` + `T_29` | `24-bit WAV` | `276–1149` | `26.4–28.3` | `0.15–0.21` | `716–1159` | Punchy short-barrel polymer tactical crack |
| `M16` / `M45` / `BNS` (`M16A2`) | `ffx/AR-15/D_31.wav`, `D_32P.wav`, `D_24P.wav`, `D_27.wav` | `24-bit WAV` | `275–903` | `24.6–28.2` | `0.02–0.25` | `716–988` | Crisp `5.56x45` whip + buffer-spring ring |
| Suppressed Rifles (`AK1s/ASRs/M16s`) | `ffx/Savage 10 .300 BLK T_15/T_29/T_31` + `SKS U_19P` | `24-bit WAV` | `298–1400` | `20.7–25.8` | `0.02–1.60` | `826–6718` | Subsonic baffle puff + supersonic crack |
| `SHG` / `SH5` (`SPAS-12`) | `ffx/Nova/O_19.wav`, `O_21P.wav`, `O_18.wav`, `CD/H_21P` | `24-bit WAV` | `317–1186` | `26.5–27.2` | `0.02–0.58` | `970–1097` | Explosive `12-gauge` chest boom + steel pump |
| `RM1` / `RM5` (`Remington 870`) | `ffx/Model 12/K_20.wav`, `K_22P.wav`, `K_19.wav` | `24-bit WAV` | `240–1400` | `25.6–27.9` | `0.02–0.54` | `814–1081` | Heavy wooden-stocked 12-ga blast + action |
| `SNR` / `SN5` (`WA 2000`) | `freesound/855608_m2010`, `524738` + `ffx/Tikka W_29P` | `48k` / `24-bit WAV` | `407–1400` | `17.1–25.6` | `0.79–6.10` | `614–2598` | Magnum sniper thunder + mountain echo tail |
| `HVY` / `HV5` (`M249 SAW`) | `freesound/482122` (`0` clips!) + `538302` (`[0,2,3]`) + `D_31` | `48k` / `44.1k` | `58–77` (+ tail) | `10.5–15.7` | `0.19–1.94` | `2320–5628` | Crisp belt-fed `5.56` roar + link chatter |
| `RPG` (`RPG-7`) | `freesound/249298` (`[0,2,4]`), `725401` (rocket load) | `44.1k` | `545–1177` | `15.0–16.1` | `0.79–7.15` | `1434–1780` | Booster ignition *WHOOSH* + propellant tail |
| `GRL` & `BNS` M203 (`40mm`) | `freesound/163458`, `520053`, `ffx/Nova` breech foley | `44.1k` / `24-bit WAV` | `426–790` | `11.1–13.8` | `0.15` | `241–484` | Authentic hollow `40mm` tube *THOOMP* |
| Flyby / Casings / Impacts | `855248`, `789222`, `210102`, `318964`, `147261`, `478345`, `30932`, `683774`, `426850`, `240463`, `406562`, `873248`, `453499`, `721363`, `lav/*.ogg` | `44.1–48k` | `80–1400` | `12.1–34.4` | `0.02–3.08` | `58–9330` | Clean crisp whizzes, brass pings, shatters & ricochets |
