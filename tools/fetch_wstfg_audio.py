#!/usr/bin/env python3
"""
Targeted fetcher & 48 kHz / 24-bit WAV converter for WSTFG (Dark Signal) tactical firearm,
suppressed, sub-bass punch, casing-by-surface, explosion, inspect, and bullet flyby audio.
Downloads only the curated paths needed for BLACK Enhanced Edition into .local/audio_src/wstfg/.
"""
import json
import os
import subprocess
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed

REPO = r"e:\ai\black"
OUT_DIR = os.path.join(REPO, ".local", "audio_src", "wstfg")
RAW_BASE = "https://raw.githubusercontent.com/oleh5230/WSTFG/main/"
TREE_URL = "https://api.github.com/repos/oleh5230/WSTFG/git/trees/main?recursive=1"
FFMPEG = r"C:\Users\313\AppData\Local\Microsoft\WinGet\Links\ffmpeg.exe"

# Exact prefix groups to download from WSTFG gamedata/sounds/
WANTED_PREFIXES = (
    # Stereo close primary weapon shots
    "gamedata/sounds/weapons/_gunfire/stereo/beretta/",
    "gamedata/sounds/weapons/_gunfire/stereo/1911/",
    "gamedata/sounds/weapons/_gunfire/stereo/glock/",
    "gamedata/sounds/weapons/_gunfire/stereo/deagle/",
    "gamedata/sounds/weapons/_gunfire/stereo/mp412/",
    "gamedata/sounds/weapons/_gunfire/stereo/pp19/",
    "gamedata/sounds/weapons/_gunfire/stereo/udp9/",
    "gamedata/sounds/weapons/_gunfire/stereo/mac10/",
    "gamedata/sounds/weapons/_gunfire/stereo/mp5/",
    "gamedata/sounds/weapons/_gunfire/stereo/p90/",
    "gamedata/sounds/weapons/_gunfire/stereo/akm/",
    "gamedata/sounds/weapons/_gunfire/stereo/g36/",
    "gamedata/sounds/weapons/_gunfire/stereo/m4/",
    "gamedata/sounds/weapons/_gunfire/stereo/hk416/",
    "gamedata/sounds/weapons/_gunfire/stereo/fal/",
    "gamedata/sounds/weapons/_gunfire/stereo/g3/",
    "gamedata/sounds/weapons/_gunfire/stereo/ugl/",
    "gamedata/sounds/weapons/_gunfire/stereo/spas12/",
    "gamedata/sounds/weapons/_gunfire/stereo/pump/",
    "gamedata/sounds/weapons/_gunfire/stereo/l96/",
    "gamedata/sounds/weapons/_gunfire/stereo/awm/",
    "gamedata/sounds/weapons/_gunfire/stereo/svd/",
    "gamedata/sounds/weapons/_gunfire/stereo/m249/",
    # Dedicated stereo & mono suppressed weapon shots (all 10 BLACK suppressed classes)
    "gamedata/sounds/weapons/_sd/stereo/pb/",
    "gamedata/sounds/weapons/_sd/stereo/pistol/",
    "gamedata/sounds/weapons/_sd/stereo/glock/",
    "gamedata/sounds/weapons/_sd/stereo/smg9/",
    "gamedata/sounds/weapons/_sd/stereo/udp9/",
    "gamedata/sounds/weapons/_sd/stereo/mac10/",
    "gamedata/sounds/weapons/_sd/stereo/mp5sd/",
    "gamedata/sounds/weapons/_sd/stereo/p90/",
    "gamedata/sounds/weapons/_sd/stereo/762_warsaw/",
    "gamedata/sounds/weapons/_sd/stereo/g36/",
    "gamedata/sounds/weapons/_sd/stereo/556_c/",
    "gamedata/sounds/weapons/_sd/stereo/scar/",
    "gamedata/sounds/weapons/_sd/stereo/dvl/",
    "gamedata/sounds/weapons/_sd/stereo/awm/",
    "gamedata/sounds/weapons/_sd/stereo/m249/",
    "gamedata/sounds/weapons/_sd/stereo/spas12/",
    # Tails, sub-bass punch, rockets, grenades & explosions
    "gamedata/sounds/weapons/_bass/",
    "gamedata/sounds/weapons/_gunfire/mono/_pistol/",
    "gamedata/sounds/weapons/_gunfire/mono/_smg/",
    "gamedata/sounds/weapons/_gunfire/mono/_ar/",
    "gamedata/sounds/weapons/_gunfire/mono/_rifle/",
    "gamedata/sounds/weapons/_gunfire/mono/_shotgun/",
    "gamedata/sounds/weapons/_gunfire/mono/_sniper/",
    "gamedata/sounds/weapons/_gunfire/mono/_sniper_big/",
    "gamedata/sounds/weapons/_boom/mono/rpg/",
    "gamedata/sounds/weapons/_boom/mono/frag/",
    "gamedata/sounds/weapons/_boom/mono/debris/",
    "gamedata/sounds/weapons/_boom/mono/reflector/",
    "gamedata/sounds/weapons/_boom/stereo/rg6/",
    "gamedata/sounds/weapons/m203/",
    "gamedata/sounds/weapons/grenade/",
    "gamedata/sounds/whine/",
    "gamedata/sounds/firemode/",
    "gamedata/sounds/casings/",
)

EXACT_FILES = (
    "gamedata/sounds/weapons/rocket_fly.ogg",
    "gamedata/sounds/weapons/mp5_new/mp5_inspect.ogg",
    "gamedata/sounds/weapons/mp5_new/mp5_magcheck.ogg",
    "gamedata/sounds/weapons/spas/inspect.ogg",
    "gamedata/sounds/weapons/spas/magcheck.ogg",
    "gamedata/sounds/weapons/uzi/uzi_inspect.ogg",
    "gamedata/sounds/weapons/uzi/uzi_magcheck.ogg",
    "gamedata/sounds/weapons/wpn_l96_new/l96_new_inspect.ogg",
    "gamedata/sounds/weapons/remington870/rem870_inspect.ogg",
    "gamedata/sounds/weapons/remington870/rem870_pump.ogg",
    "gamedata/sounds/weapons/m4a1_ras/m4a1_switch.ogg",
    "gamedata/sounds/weapons/glock/glock_reload.ogg",
    "gamedata/sounds/weapons/glock/glock_reload_empty.ogg",
    "gamedata/sounds/weapons/p90/p90_reload.ogg",
    "gamedata/sounds/weapons/p90/p90_reload_full.ogg",
    "gamedata/sounds/weapons/mac10/mac10_reload.ogg",
    "gamedata/sounds/weapons/mac10/mac10_reload_empty.ogg",
    "gamedata/sounds/weapons/librarian_m249/reload.ogg",
    "gamedata/sounds/weapons/librarian_m249/reload_empty.ogg",
    "gamedata/sounds/weapons/rpg7/rpg7_reload.ogg",
)


def fetch_tree():
    req = urllib.request.Request(TREE_URL, headers={"User-Agent": "black-audio-builder"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    out = []
    for item in data.get("tree", []):
        p = item.get("path", "")
        if not p.endswith(".ogg"):
            continue
        if p in EXACT_FILES or any(p.startswith(pref) for pref in WANTED_PREFIXES):
            # Keep first 2 variants of each casing combo to avoid bloated downloads
            if p.startswith("gamedata/sounds/casings/"):
                base = os.path.basename(p)
                if not (base.endswith("_1.ogg") or base.endswith("_2.ogg")):
                    continue
                if any(cal in base for cal in ("127mm_", "338_", "_mud_", "_water_", "m209")):
                    continue
            out.append(p)
    return sorted(set(out))


def download_and_convert(rel_git_path):
    rel_clean = rel_git_path.replace("gamedata/sounds/", "", 1)
    wav_rel = os.path.splitext(rel_clean)[0] + ".wav"
    wav_path = os.path.join(OUT_DIR, wav_rel.replace("/", os.sep))
    if os.path.isfile(wav_path) and os.path.getsize(wav_path) > 1024:
        return rel_clean, "CACHED", os.path.getsize(wav_path)

    os.makedirs(os.path.dirname(wav_path), exist_ok=True)
    ogg_tmp = wav_path + ".tmp.ogg"
    url = RAW_BASE + urllib.request.quote(rel_git_path)
    p = subprocess.run(
        ["curl.exe", "-s", "-f", "-L", "--max-time", "30", "-o", ogg_tmp, url],
        capture_output=True,
        timeout=35,
    )
    if p.returncode != 0 or not os.path.isfile(ogg_tmp) or os.path.getsize(ogg_tmp) < 128:
        raise RuntimeError(f"curl failed rc={p.returncode} for {url}")

    ff = subprocess.run(
        [FFMPEG, "-y", "-v", "error", "-i", ogg_tmp, "-ar", "48000", "-acodec", "pcm_s24le", wav_path],
        capture_output=True,
        timeout=30,
    )
    try:
        os.remove(ogg_tmp)
    except OSError:
        pass
    if ff.returncode != 0 or not os.path.isfile(wav_path) or os.path.getsize(wav_path) < 1024:
        raise RuntimeError(f"ffmpeg failed for {rel_git_path}: {ff.stderr.decode(errors='ignore')}")
    return rel_clean, "DOWNLOADED", os.path.getsize(wav_path)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    paths = fetch_tree()
    print(f"Fetching & converting {len(paths)} WSTFG tactical audio files -> {OUT_DIR}")
    ok = 0
    fail = 0
    total_kb = 0.0
    with ThreadPoolExecutor(max_workers=6) as ex:
        futs = {ex.submit(download_and_convert, p): p for p in paths}
        for fut in as_completed(futs):
            p = futs[fut]
            try:
                rel, st, sz = fut.result()
                ok += 1
                total_kb += sz / 1024.0
                if st == "DOWNLOADED":
                    print(f"  [{st}] {rel} ({sz/1024:.0f} KB)")
            except Exception as e:
                fail += 1
                print(f"  [FAIL] {p}: {e}")
    print(f"Finished: {ok} ok ({total_kb/1024.0:.1f} MB WAVs), {fail} failed.")


if __name__ == "__main__":
    main()
