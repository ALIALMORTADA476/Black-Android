#!/usr/bin/env python3
"""
Targeted HTTP Range extractor for Sonniss #GameAudioGDC (2015-2024) individual WAV files.
Uses the GDC Bundle Finder index to fetch ONLY selected firearm, tail, echo, flyby,
casing, mechanical, and explosion WAVs directly from downloads.sonniss.com without downloading full multi-GB ZIPs.
"""
import json
import os
import re
import struct
import subprocess
import sys
import zlib
from concurrent.futures import ThreadPoolExecutor, as_completed

REPO = r"e:\ai\black"
GDC_DIR = os.path.join(REPO, ".local", "audio_src", "gdc")
SCRATCH = r"C:\Users\313\.gemini\antigravity\brain\db2924e5-7d7e-4013-bc79-497f520eff0a\scratch"
HTML_PATH = os.path.join(SCRATCH, "gdc_finder.html")

# Curated file IDs from gdc_finder.html (every one verified firearm, tail, echo, flyby, casing, mechanical, explosion)
CURATED_FIDS = [
    # --- PISTOLS ---
    5148,  # Walther P38 Firing 2m Behind (60s)
    5149,  # Walther P38 Removing Magazine
    6202,  # Glock 17 Firing 20m In Front
    6203,  # Glock 17 Firing 100m In Front Off Axis (Distant tail!)
    6963,  # Glock 17 Small Living Room Single Shots (Indoor acoustics!)
    1045,  # Glock 18 1m Left
    1046,  # Glock 18 75m Behind
    1005,  # HK VP70 9mm 1m In Front
    1006,  # HK VP70 9mm On Gun
    1089,  # SIG P229 9mm 1m Left
    1090,  # SIG P229 9mm 50m Behind
    1014,  # Beretta 93R 9mm 1m Left
    727,   # Colt M1911 .45 ACP London Warehouse 10m x6
    4935,  # M1911A1 Firing 200m Front Target (Distant!)
    4936,  # M1911A1 Handling Dry Fire
    1094,  # S&W M29 .44 Magnum 1.5m Behind
    1095,  # S&W M29 .44 Magnum 2m Behind PZM
    1096,  # S&W M66 .357 Magnum 1m Left
    1097,  # S&W M66 .357 Magnum 50m Behind
    6876,  # Colt Python .357 Magnum 160m Behind (Distant + tail!)
    6877,  # Colt Python Loading Gun
    1115,  # S&W M29 Cylinder Open & Close
    6943,  # S&W 60 Revolver Open Close with Shells
    6883,  # Desert Eagle Magazine In & Out

    # --- SMGs (MAC-10, MP5, UZI, P90) ---
    1055,  # Ingram M10 9mm 1m Right
    1056,  # Ingram M10 9mm 50m Behind
    1001,  # HK MP5 9mm 2m Behind
    1002,  # HK MP5 9mm 75m Behind
    2784,  # MP5 Forest Road 10m Behind (Outdoor forest tail!)
    6965,  # MP5 Large Hall Bursts (Indoor hall echo!)
    1246,  # MP5 Burst Shot Short Urban Exterior
    1111,  # Uzi 9mm 1m Right
    1112,  # Uzi 9mm 75m Behind
    1074,  # Mini Uzi 9mm 1m Left
    1075,  # Mini Uzi 9mm 30m In Front
    1043,  # FN P90 5.7mm 1m Left
    1044,  # FN P90 5.7mm 75m Behind
    1041,  # FN Five-seveN 5.7mm 1m Right
    1003,  # HK MP7 4.6mm 1m Left
    3089,  # SMG MP7A1 Auto Tail Extra
    1072,  # M3 Grease Gun .45 0.3m Side
    1073,  # M3 Grease Gun .45 300m In Front (Ultra distant!)
    5041,  # MP 40 250m Holophone (Distant battlefield!)
    5042,  # MP 40 Cocking
    5118,  # PPSh41 Pulling Back Bolt

    # --- ASSAULT / BATTLE RIFLES (AK-47, G36C, M16A2, M4A1/HK416) ---
    1007,  # AKM 7.62x39mm 1m Left
    1008,  # AKM 7.62x39mm 55m In Front (Mattak!)
    2782,  # AK47 Big Open Area 2m Above Behind (Outdoor open area Mattak!)
    2779,  # AK47 Catacombs Burst Wide Stereo (Indoor reflection!)
    3086,  # BR AK47 Burst With Tail (23.7s!)
    6989,  # AK5 7.62mm Firing Hit Metal Armor Handheld Near Shooter
    6962,  # AK5 7.62mm Large Hall Long Bursts
    2783,  # AK5 Valley Field Forest 50m Behind (Rolling valley Mattak tail!)
    2780,  # AK4 Long Corridor Single Shots Behind In Corner (Indoor corridor!)
    1051,  # HK G36C 5.56mm 1m Left
    1052,  # HK G36C 5.56mm 50m Behind
    1113,  # HK G36C Mag In & Out
    995,   # HK G3 7.62x51mm 1m Left
    996,   # HK G3 7.62x51mm 55m In Front
    1026,  # Enfield L85 5.56mm 1m Left
    1027,  # Enfield L85 5.56mm On Gun
    1034,  # FAMAS G2 5.56mm 1m Left
    1035,  # FAMAS G2 5.56mm 55m In Front
    1245,  # M16_INT_B_distant
    997,   # HK 416 5.56mm 1m Left
    998,   # HK 416 5.56mm On Gun
    730,   # Audiobeast Small Room HK416 5.56mm
    67,    # Coll Anderson M4 Shots 02 B
    69,    # Coll Anderson AR-15 M4 Charging Handle With Mag
    70,    # Coll Anderson Unloading Magazine Singles 01
    3087,  # BR XM8 Auto Distant Exterior
    1039,  # FN FAL 7.62x51mm 1.5m Behind
    1040,  # FN FAL 7.62x51mm 25m In Front

    # --- SHOTGUNS (Remington 870, SPAS-12) ---
    729,   # Remington 870 12g Warehouse 10m Single Shot x8
    6964,  # Mossberg 12g Office Corridor 20m Adjacent Room
    1302,  # TS 12g Shell Load Round Eject
    1303,  # TS 12g Shotgun Fire Round Eject
    1304,  # TS 12g Shotgun Fire Slug
    1305,  # TS 12g Shotgun Quick Pump
    6902,  # SPAS-12 Pump Action 20m In Front
    6903,  # SPAS-12 Semi Automatic 3m Behind Left
    6904,  # SPAS-12 Pump Action Fast Close
    1098,  # SPAS-12 12g Solid Slug 1m Right
    1099,  # SPAS-12 12g Solid Slug 50m Behind
    6659,  # Saiga-12 Chamber Eject Bullet
    6661,  # Saiga-12 Single Shots 1m Side MONO
    1114,  # Saiga-12 Cocking

    # --- SNIPER RIFLES (.308 / .338 / .50 / 7.62x54R) ---
    1012,  # Barrett M98B .338 Lapua 2m Right x17
    1013,  # Barrett M98B .338 Lapua On Gun x17
    6662,  # SVD Dragunov Mag Insert
    6663,  # SVD Dragunov Single Shot Core Long
    6664,  # SVD Dragunov 30m Front
    1083,  # Remington 700 7.62x51mm 1m Right x14
    1084,  # Remington 700 7.62x51mm 50m Behind x14
    6908,  # Lynx .50 Cal 65m Behind
    6909,  # Lynx .50 Cal 4m Left
    6910,  # Lynx .50 Cal Shell Ejections
    1087,  # Sako TRG 22 1m Left x5
    5024,  # K98k Handling Various 1m Right
    5095,  # M1903A3 Cycling Bolt

    # --- HEAVY MACHINE GUNS (M249 SAW / MG4 / Browning) ---
    999,   # HK MG4 5.56mm 1m Left Bursts x16
    1000,  # HK MG4 5.56mm 35m In Front Bursts x16
    1060,  # L110A2 (FN Minimi / M249!) 5.56mm 1m Right
    1061,  # L110A2 (FN Minimi / M249!) 50m Behind
    1062,  # L7A2 GPMG 7.62mm 1m In Front
    1063,  # L7A2 GPMG 7.62mm 50m Behind
    728,   # Browning M2 .50 Machine Gun Warehouse 3m Burst x2
    5034,  # MG 42 Mountain Top Burst (Amazing mountain echo!)
    5035,  # MG 42 Top Cover Up
    1852,  # Warfare t2 MG Firing Close Projectile Tail Large Field
    1851,  # Warfare t1b Cannon Firing Forest Distant

    # --- SUPPRESSED WEAPONS ---
    1088,  # Sako TRG 22 Silencer 1m Right x6
    1020,  # De Lisle Carbine .45ACP Suppressed 1m Right x14
    1021,  # De Lisle Carbine .45ACP Suppressed 55m In Front x14
    2781,  # PPK Cafeteria Suppressor Double Shots Nextdoor
    2535,  # Gamemaster Pistol Shot Silenced 04
    2536,  # Gamemaster Silenced Sniper Shot 01
    2537,  # Gamemaster Silencer Barrel Screw Fast

    # --- RPG / GRENADE LAUNCHER / EXPLOSIONS ---
    6991,  # RPG Firing And Impact Through Car Roof
    5829,  # Bluezone Weapon Smoke Grenade Launcher 003
    4440,  # Bluezone Shell Trajectory 004
    4441,  # Bluezone Tank Artillery Cannon Shot 012
    68,    # Coll Anderson Mortar Explosion 08
    1610,  # explosion_far_distant_02
    1611,  # explosion_large_08
    1613,  # explosion_med_long_tail_01
    4439,  # Bluezone explosion_outdoors_large_005
    5827,  # Bluezone explosion_mortar_002_01
    5828,  # Bluezone explosion_urban_004_02
    5515,  # DeepExplosion02
    5516,  # FarExplosion03
    6992,  # Tank Mine Explosion Distant

    # --- FLYBY, SUPERSONIC CRACK, RICOCHET, CASINGS, IMPACTS ---
    1853,  # warfare_t3_mg_whizzes_ricochets_bullet_cracks_M10 (99.7s goldmine!)
    4847,  # PM_BBI_Bullet_Passby_Whizzby_Airy_5
    1606,  # bullet_flyby_fast_05
    2514,  # bullet_flyby_fast_06
    1607,  # bullet_impact_body_thump_02
    1608,  # bullet_impact_concrete_brick_01
    1609,  # bullet_impact_metal_heavy_08
    4844,  # PM_BBI_Bullet_Impact_Dirt_3
    4845,  # PM_BBI_Bullet_Impact_Glass_1
    4846,  # PM_BBI_Bullet_Impact_Hit_Body_Flesh_25
    5194,  # 7.62x39 Casings Impact and Roll Tile Floor
    5195,  # 9mmx21 Casings Roll Concrete Floor
    3088,  # Casing 50BMG Dirt
    5826,  # cartridge_case_handling_018
    2534,  # bullet_shell_bounce_wood2_05
    1273,  # ShotgunShell_Land_Concrete_02
    1116,  # Steyr TMP Cocking & Dry Fire
    1624,  # gun_semi_auto_rifle_cock_02
]

def load_index():
    with open(HTML_PATH, "r", encoding="utf-8", errors="ignore") as f:
        html = f.read()
    s0 = re.findall(r"<script[^>]*>(.*?)</script>", html, re.S)[0]
    def extract_var(name):
        m = re.search(rf"const {name}\s*=\s*(.*?);\s*(?=\nconst |\Z)", s0, re.S)
        return json.loads(m.group(1))
    return extract_var("ZIPS"), extract_var("LIBS"), extract_var("FILES")

def rfetch(url, start, end, retries=4):
    last_err = None
    for _ in range(retries):
        p = subprocess.run(
            ["curl.exe", "-s", "-f", "-L", "--max-time", "60", "-r", f"{start}-{end}", url],
            capture_output=True,
            timeout=70,
        )
        if p.returncode == 0 and len(p.stdout) > 0:
            return p.stdout
        last_err = f"curl rc={p.returncode} stderr={p.stderr[:120]!r}"
    raise RuntimeError(f"Range {start}-{end} failed: {last_err}")

def safe_name(s):
    return re.sub(r'[<>:"/\\|?*]', "_", s)

def download_one(fid, zips, libs, files):
    li, fn, dur, method, csz, off = files[fid]
    yr, part, lib = libs[li]
    url = zips[yr][str(part)][1]
    sub = safe_name(f"{yr}_{lib[:40].strip()}")
    out_dir = os.path.join(GDC_DIR, sub)
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, f"{fid}_{safe_name(os.path.basename(fn))}")
    if os.path.exists(out_path) and os.path.getsize(out_path) > 1000:
        return fid, out_path, os.path.getsize(out_path), "CACHED"

    lh = rfetch(url, off, off + 29)
    if len(lh) < 30 or lh[:4] != b"PK\x03\x04":
        raise RuntimeError(f"fid {fid}: bad ZIP local header {lh[:4]!r}")
    fname_len, extra_len = struct.unpack("<HH", lh[26:30])
    data_start = off + 30 + fname_len + extra_len
    comp_bytes = rfetch(url, data_start, data_start + csz - 1)
    if method == 8:
        raw = zlib.decompress(comp_bytes, -15)
    elif method == 0:
        raw = comp_bytes
    else:
        raise RuntimeError(f"fid {fid}: unsupported zip method {method}")
    with open(out_path, "wb") as f:
        f.write(raw)
    return fid, out_path, len(raw), "DOWNLOADED"

def main():
    os.makedirs(GDC_DIR, exist_ok=True)
    zips, libs, files = load_index()
    fids = list(dict.fromkeys(CURATED_FIDS))
    if len(sys.argv) > 1 and sys.argv[1] == "--test":
        fids = [1007, 1008]
    print(f"Fetching {len(fids)} Sonniss GDC recordings into {GDC_DIR} ...")
    ok = 0
    fail = 0
    total_bytes = 0
    with ThreadPoolExecutor(max_workers=2) as ex:
        futs = {ex.submit(download_one, fid, zips, libs, files): fid for fid in fids}
        for fut in as_completed(futs):
            fid = futs[fut]
            try:
                _, path, sz, status = fut.result()
                ok += 1
                total_bytes += sz
                print(f"  [{status}] {fid} ({sz/1024:.0f} KB) {os.path.basename(path)}")
            except Exception as e:
                fail += 1
                print(f"  [FAIL] {fid}: {e}")
    print(f"Done: {ok} ok, {fail} failed, total WAV size {total_bytes/1048576:.1f} MB")

if __name__ == "__main__":
    main()
