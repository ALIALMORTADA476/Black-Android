# BLACK Enhanced Edition — Sonniss GDC Weapon Audio Research & Evaluation

## 1. Mission & Quality Target

This research establishes a military-grade tactical firearm soundscape for **BLACK Enhanced Edition** comparable in depth, punch, acoustic decay, and weapon separation to modern tactical shooters such as **SQUAD**. Generic pitched-down stock clicks and short synthetic loops were rejected in favor of authentic multi-distance, multi-microphone live firearm recordings audited quantitatively prior to mastering.

---

## 2. Primary Source Index — Sonniss #GameAudioGDC Archive (2015–2024)

- **Search Index**: GDC Bundle Finder (`https://yansixing.github.io/gdc-bundle-finder/`) covering **`2,124` libraries** (`7,492` audio files across 2015–2024 bundles).
- **Host Archive**: Official Sonniss GDC distribution (`https://downloads.sonniss.com/`).
- **Selective HTTP Range Extraction (`tools/fetch_sonniss_gdc.py`)**: Rather than downloading multi-hundred-gigabyte bundles blindly, the targeted ZIP central-directory byte-range extractor fetched **150 curated `.wav` recordings** (`1,517.2 MB` raw PCM, spanning **`66` libraries**, **`3,189.1 s`** total audio: `146` files at `96 kHz / 24-bit`, `2` files at `192 kHz / 24-bit`, `2` files at `48 kHz`).

### Search Domains Covered

| Category | Keywords Audited | Selected Distance / Mic Contexts |
|---|---|---|
| **Assault & Battle Rifles** | AKM, AK-47, AK4, AK5, G36C, FAMAS, M4/HK416, L85, G3, FN FAL | Close `1m` DPA4062/MKH416, `50m–55m` behind/front off-axis, Open Area, Valley Field Forest, Catacombs, Long Corridor |
| **Pistols & Magnums** | Walther P38, HK VP70, Glock 17/18, SIG P229, Colt M1911A1, S&W M29 .44, Colt Python .357 | `1m–2m` MKH416/PZM, `10m` London Warehouse, Small Living Room, `50m–200m` Front/Behind Outdoor |
| **Submachine Guns** | Uzi, Mini Uzi, Ingram M10, HK MP5, FN P90, FN Five-seveN, HK MP7A1, M3 Grease Gun, MP40 | `1m–2m` MKH416/CS3, Forest Road `10m`, Large Hall, Urban Exterior, `50m–300m` Far Field |
| **Shotguns** | Remington 870, SPAS-12, Mossberg 12g, Saiga-12, TS 12g | `10m` Warehouse U87, `20m` In Front, Office Corridor Adjacent Room, `3m` Semi-Auto, Solid Slug |
| **Sniper & Heavy LMG** | Barrett M98B .338, Lynx .50, SVD Dragunov, Remington 700, Sako TRG-22, L110A2 (M249 SAW), L7A2 GPMG, HK MG4, Browning M2, MG42 | `1m–4m` Close, Mountain Top `Long Burst`, Warfare Large Field Projectile Tail, `50m–65m` Behind |
| **Suppressed & Launchers** | De Lisle .45ACP, PPK Cafeteria Suppressor, Sako TRG Silencer, Pistol/Sniper Silenced, RPG-7, Smoke GRL, Artillery, Explosions | Cafeteria Nextdoor, `1m–55m` Suppressed, Through Car Roof RPG, Outdoor/Urban/Mortar Detonations |
| **Flyby, Cracks, Casings & Impacts** | Supersonic Whizzes, Bullet Cracks, Ricochets, `7.62x39`, `9x21`, `.50 BMG`, 12g Shells, Concrete/Metal/Glass/Dirt/Body | `99.7s` Warfare M10 Whizzes & Cracks (`255/256` clean), Airy Passby, Shell/Casing Drop Rolls |

---

## 3. Quantitative Audition & Selection Criteria (`tools/analyze_gdc_takes.py`)

Before any file enters `black_weapon_audio.pak`, `analyze_gdc_takes.py` inspects all `150` WAVs (`2,562` individual detected takes):

1. **Raw Digital Clipping Rejection**: Evaluates each mic channel in its native bit-depth (`|x| >= 0.9995`). Close mics that overloaded the preamp on hotter takes (e.g., individual hot takes inside `998`, `1003`, `1014`, `1039`) are rejected in favor of verified zero-clip takes (`clean_takes_count > 0`) or complementary `10m–55m` unclipped field takes (`6902`, `6903`, `6909`, `727`, `729`, `1089`, `2783`, `6202`, `1853`).
2. **Pre-Resampling Intersample Headroom**: Hot `96 kHz / 24-bit` close-mic masters recorded near `-1.0 dBFS` can exhibit fractional intersample peaks after FIR decimation to `48 kHz`; `build_weapon_audio_pack.py` scales peaks above `0.81` (`-1.8 dB`) *prior* to anti-aliased `48 kHz` downsampling.
3. **Natural Decay & Tail Separation**: Verifies minimum usable tail (`>= 1.2s–2.6s`) for every gunshot tail slot (`TAIL_RIFLE_MATTAK`, `TAIL_HEAVY_MATTAK`, `TAIL_INDOOR_ECHO`, `TAIL_SHOTGUN_OUTDOOR`, `TAIL_PISTOL_OUTDOOR`, `TAIL_SMG_OUTDOOR`) and ensures zero double-transient flamming when layered behind primary gunfire.
4. **Distinct Per-Weapon Identity**: Enforces strict separation so none of BLACK's 17 base firearms share the same primary GDC source recording.
