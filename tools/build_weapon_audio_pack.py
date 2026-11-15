# -*- coding: utf-8 -*-
#!/usr/bin/env python3
"""
BLACK Enhanced Edition — Military-Grade 48 kHz True-Stereo Audio Mastering Pipeline & Pack Builder (BWAR v3)

Prioritizes user-curated assets in `صوتيات/` first (Modern Firearms Military Pack + Sonniss GDC 2020),
supplemented by 192 kHz FFX Master Tracks, `gdc_extra` (96 kHz/24-bit stereo), and curated Sonniss GDC (2015-2024).

Enforces:
  - 100% distinct primary firing sources across all 18+1 BLACK player weapons (zero shared primary WAVs)
  - Genuine 2ch True Stereo preservation + automatic L/R polarity/ITD phase repair + <120 Hz Mid bass anchor
  - Zero digitally clipped takes allowed into mastering; transparent -1.2 dBFS peak ceiling (total_clipped = 0)
  - Complete 129-slot coverage (0..128) including footsteps, landings, bodyfalls, melee, water, fire loop,
    pickups, sniper breath, heartbeat, debris, car windshield, weapon drops, HUD chimes, and flamethrower ignition.

Generates:
  - app/src/main/assets/weapon_audio/black_weapon_audio.pak (BWAR v3: 129 slots, 48 kHz stereo/mono PCM16)
  - scratch/weapon_audio_manifest.json (complete per-clip & per-slot acoustic + stereo audit metrics)
"""
import json
import math
import os
import struct
import sys
import numpy as np

REPO = r"e:\ai\black"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from analyze_gdc_takes import read_wav_float32, resample_48k

GDC_DIR = os.path.join(REPO, ".local", "audio_src", "gdc")
GDC_EXTRA_DIR = os.path.join(REPO, ".local", "audio_src", "gdc_extra")
SOTIYAT_DIR = os.path.join(REPO, "صوتيات")
FFX_MASTER_DIR = os.path.join(REPO, ".local", "audio_src", "ffx", "Master Tracks")
ANALYSIS_JSON = os.path.join(REPO, ".local", "audio_src", "gdc_analysis.json")
EXTRA_ANALYSIS_JSON = os.path.join(REPO, ".local", "audio_src", "gdc_extra_analysis.json")
SCRATCH = r"C:\Users\313\.gemini\antigravity\brain\db2924e5-7d7e-4013-bc79-497f520eff0a\scratch"
OUT_PAK = os.path.join(REPO, "app", "src", "main", "assets", "weapon_audio", "black_weapon_audio.pak")
OUT_MANIFEST = os.path.join(SCRATCH, "weapon_audio_manifest.json")
SR = 48000
PACK_VERSION = 3

# Canonical Slot IDs (BWAR v3: 129 slots 0..128 shared verbatim with src/sound/weapon_audio.cpp)
SLOTS = {
    # First-person Gunfire Unsuppressed (0..16)
    "PST_FIRE": 0,   "GK1_FIRE": 1,   "MGN_FIRE": 2,   "SMG_FIRE": 3,
    "SM3_FIRE": 4,   "MP1_FIRE": 5,   "P90_FIRE": 6,   "AK1_FIRE": 7,
    "ASR_FIRE": 8,   "M16_FIRE": 9,   "BNS_FIRE": 10,  "SHG_FIRE": 11,
    "RM1_FIRE": 12,  "SNR_FIRE": 13,  "HVY_FIRE": 14,  "RPG_FIRE": 15,
    "GRL_FIRE": 16,
    # Suppressed Gunfire (17..26)
    "PST_SUPP": 17,  "GK1_SUPP": 18,  "SMG_SUPP": 19,  "SM3_SUPP": 20,
    "MP1_SUPP": 21,  "P90_SUPP": 22,  "AK1_SUPP": 23,  "ASR_SUPP": 24,
    "M16_SUPP": 25,  "SNR_SUPP": 26,
    # Mechanicals & Reloads (27..73)
    "PST_UNLD": 27,  "PST_LD": 28,    "PST_COCK": 29,
    "GK1_UNLD": 30,  "GK1_LD": 31,    "GK1_COCK": 32,
    "MGN_UNLD": 33,  "MGN_LD": 34,
    "SMG_UNLD": 35,  "SMG_LD": 36,    "SMG_COCK": 37,
    "MP1_UNLD": 38,  "MP1_LD": 39,    "MP1_COCK": 40,
    "P90_UNLD": 41,  "P90_LD": 42,    "P90_COCK": 43,
    "AK1_UNLD": 44,  "AK1_LD": 45,    "AK1_COCK": 46,
    "ASR_UNLD": 47,  "ASR_LD": 48,    "ASR_COCK": 49,
    "M16_UNLD": 50,  "M16_LD": 51,    "M16_COCK": 52,
    "SHG_COCK": 53,  "RM1_LD": 54,    "RM1_COCK": 55,
    "SNR_UNLD": 56,  "SNR_LD": 57,    "SNR_COCK": 58,
    "HVY_UNLD": 59,  "HVY_LD": 60,    "HVY_COCK": 61,
    "RPG_LD": 62,    "RPG_ATTACH": 63,
    "GRL_OPEN": 64,  "GRL_CASE": 65,  "GRL_COCK": 66,
    "FIRE_SELECT": 67, "SILENCER_ON": 68, "SILENCER_OFF": 69,
    "DEAD_CLICK": 70,  "CHNG_WPN": 71,    "CASING_BRASS": 72, "CASING_SHELL": 73,
    # Enemy AI & Distant Gunfire (74..83)
    "AI_PISTOL": 74, "AI_SMG_PULSE": 75, "AI_SMG_START": 76,
    "AI_RIFLE_PULSE": 77, "AI_RIFLE_START": 78, "AI_TAIL": 79,
    "AI_SHOTGUN": 80, "AI_SNIPER": 81, "AI_RPG": 82, "AI_SUPP": 83,
    # Environment, Flyby, Impacts & Explosions (84..96)
    "ENV_TAIL": 84,  "ENV_FLYBY": 85, "ENV_RPG_PASS": 86,
    "IMP_CONCRETE": 87, "IMP_METAL": 88, "IMP_WOOD": 89, "IMP_GLASS": 90,
    "IMP_RICOCHET": 91, "IMP_DIRT": 92,  "IMP_FLESH": 93, "EXPLOSION": 94,
    "BNS_M203": 95,  "SILENCER_CASE": 96,
    # Dedicated Layered Outdoor Tails ("Mattak"), Room Reflections, Far-Field & Supersonic (97..105)
    "TAIL_PISTOL_OUTDOOR": 97,
    "TAIL_SMG_OUTDOOR": 98,
    "TAIL_RIFLE_MATTAK": 99,
    "TAIL_HEAVY_MATTAK": 100,
    "TAIL_INDOOR_ECHO": 101,
    "DISTANT_BATTLEFIELD": 102,
    "ENV_SUPERSONIC_CRACK": 103,
    "TAIL_SHOTGUN_OUTDOOR": 104,
    "TAIL_SUPPRESSED_CRACK": 105,
    # Expanded Non-Weapon & Stereo Foley Slots (BWAR v3, 106..128)
    "FS_CONCRETE_WALK": 106,
    "FS_CONCRETE_RUN": 107,
    "FS_GRASS_WALK": 108,
    "FS_DIRT_GRAVEL": 109,
    "FS_METAL": 110,
    "FS_WATER_PUDDLE": 111,
    "LAND_IMPACT": 112,
    "BODYFALL": 113,
    "MELEE_SWING": 114,
    "MELEE_HIT": 115,
    "WATER_SPLASH": 116,
    "FIRE_LOOP": 117,
    "PICKUP_AMMO": 118,
    "PICKUP_HEALTH": 119,
    "SNIPER_BREATH": 120,
    "HEARTBEAT_LOW_HLTH": 121,
    "DEBRIS_RUBBLE": 122,
    "CAR_WINDSHIELD": 123,
    "WEAPON_DROP": 124,
    "UI_HUD_CONFIRM": 125,
    "UI_HUD_HINT": 126,
    "UI_HUD_ALERT": 127,
    "FLAME_IGNITE": 128,
}

# Stem categories: matches StemCategory enum & StemForSlot() in weapon_audio.cpp
STEM_WEAPON, STEM_MECH, STEM_RELOAD, STEM_ENV = 0, 1, 2, 3
STEM_TAIL, STEM_ECHO, STEM_DISTANT, STEM_FLYBY = 4, 5, 6, 7


def slot_stem(slot_name):
    if slot_name == "AI_TAIL" or (slot_name.startswith("TAIL_") and slot_name != "TAIL_INDOOR_ECHO"):
        return STEM_TAIL
    if slot_name in ("ENV_TAIL", "TAIL_INDOOR_ECHO"):
        return STEM_ECHO
    if "_FIRE" in slot_name or slot_name.endswith("_SUPP") or slot_name in ("BNS_M203", "FLAME_IGNITE"):
        return STEM_WEAPON
    if slot_name.startswith("AI_") or slot_name == "DISTANT_BATTLEFIELD":
        return STEM_DISTANT
    if slot_name in ("ENV_FLYBY", "IMP_RICOCHET", "ENV_SUPERSONIC_CRACK"):
        return STEM_FLYBY
    if any(k in slot_name for k in ("_UNLD", "_LD", "RPG_ATTACH", "GRL_CASE", "GRL_OPEN")):
        return STEM_RELOAD
    if slot_name in ("MELEE_SWING", "MELEE_HIT", "PICKUP_AMMO", "PICKUP_HEALTH", "WEAPON_DROP") or any(
        k in slot_name for k in ("_COCK", "FIRE_SELECT", "SILENCER_", "DEAD_CLICK", "CHNG_WPN", "CASING_", "_CASE")
    ):
        return STEM_MECH
    return STEM_ENV


_ANALYSIS = None
_EXTRA_ANALYSIS = None
_WAV_CACHE = {}


def load_analysis():
    global _ANALYSIS
    if _ANALYSIS is None:
        with open(ANALYSIS_JSON, "r", encoding="utf-8") as f:
            _ANALYSIS = json.load(f)
    return _ANALYSIS


def load_extra_analysis():
    global _EXTRA_ANALYSIS
    if _EXTRA_ANALYSIS is None:
        if os.path.isfile(EXTRA_ANALYSIS_JSON):
            with open(EXTRA_ANALYSIS_JSON, "r", encoding="utf-8") as f:
                _EXTRA_ANALYSIS = json.load(f)
        else:
            _EXTRA_ANALYSIS = {}
    return _EXTRA_ANALYSIS


def S(rel):
    """Reference a curated user WAV inside e:\\ai\\black\\صوتيات."""
    return ("sotiyat", rel)


def F(rel):
    """Reference an unclipped 192 kHz/24-bit WAV inside .local/audio_src/ffx/Master Tracks."""
    return ("ffx", rel)


def resolve_ref(ref):
    if isinstance(ref, tuple):
        kind, rel = ref
        base = SOTIYAT_DIR if kind == "sotiyat" else FFX_MASTER_DIR
        full = os.path.join(base, rel.replace("/", os.sep))
        label = f"{kind}:{os.path.basename(rel)}"
        return full, label, None
    fid = int(ref)
    ext = load_extra_analysis()
    if str(fid) in ext:
        entry = ext[str(fid)]
        return entry["path"], f"GDC{fid}", None
    ana = load_analysis()
    if str(fid) in ana:
        entry = ana[str(fid)]
        full = os.path.join(GDC_DIR, entry["rel_path"].replace("/", os.sep))
        return full, f"GDC{fid}", entry
    raise FileNotFoundError(f"Unknown audio reference: {ref}")


def resample_48k_hq(x1d, src_sr):
    """High-precision anti-aliased resampling to 48,000 Hz (handles 44.1k->48k via FFT band-limit)."""
    if src_sr == SR or len(x1d) < 8:
        return x1d.astype(np.float32)
    if src_sr > SR:
        return resample_48k(x1d, src_sr)
    # 44.1 kHz -> 48 kHz (upsample via clean Fourier zero-padding with gentle 20.4 kHz taper)
    n_in = len(x1d)
    n_out = max(1, int(round(n_in * SR / float(src_sr))))
    nfft_in = 1 << int(np.ceil(np.log2(max(n_in, 64))))
    nfft_out = int(round(nfft_in * SR / float(src_sr)))
    # Align nfft_out even
    if nfft_out & 1:
        nfft_out += 1
    X = np.fft.rfft(x1d.astype(np.float64), n=nfft_in)
    freqs_in = np.fft.rfftfreq(nfft_in, 1.0 / src_sr)
    taper = np.ones_like(freqs_in)
    lo_t, hi_t = 19200.0, src_sr * 0.49
    tr = (freqs_in > lo_t) & (freqs_in <= hi_t)
    taper[tr] = 0.5 * (1.0 + np.cos(np.pi * (freqs_in[tr] - lo_t) / max(1.0, hi_t - lo_t)))
    taper[freqs_in > hi_t] = 0.0
    X *= taper
    X_out = np.zeros(nfft_out // 2 + 1, dtype=np.complex128)
    k_copy = min(len(X), len(X_out))
    X_out[:k_copy] = X[:k_copy]
    y = np.fft.irfft(X_out, n=nfft_out) * (float(nfft_out) / float(nfft_in))
    return y[:n_out].astype(np.float32)


def downmix_spatial_to_lr(arr, clip_lim):
    """Convert 1ch/2ch/4ch/7ch studio arrays cleanly into mono [N] or stereo [N, 2]."""
    _, ch = arr.shape
    if ch == 1:
        return arr[:, 0].astype(np.float32)
    if ch == 2:
        l = arr[:, 0].astype(np.float32)
        r = arr[:, 1].astype(np.float32)
        # If one channel has severe clipping while the other is spotless, prefer clean mono over clipping
        cl_l = int(np.sum(np.abs(l) >= clip_lim))
        cl_r = int(np.sum(np.abs(r) >= clip_lim))
        if (cl_l > 8 and cl_r == 0) or (cl_r > 8 and cl_l == 0):
            return r if cl_r == 0 else l
        return np.column_stack([l, r]).astype(np.float32)
    if ch == 4:
        # Quad / Holophone (FL, FR, RL, RR) -> balanced wide L/R
        l = 0.62 * arr[:, 0] + 0.48 * arr[:, 2]
        r = 0.62 * arr[:, 1] + 0.48 * arr[:, 3]
        mx = float(max(np.max(np.abs(l)), np.max(np.abs(r)), 1.0))
        if mx > 0.95:
            l /= (mx / 0.95)
            r /= (mx / 0.95)
        return np.column_stack([l, r]).astype(np.float32)
    # 7ch Holophone / Ambisonic array (prefer unclipped channels around center/surround pairs)
    ch_clips = [int(np.sum(np.abs(arr[:, i]) >= clip_lim)) for i in range(ch)]
    clean_idx = [i for i, c in enumerate(ch_clips) if c == 0]
    if len(clean_idx) >= 2:
        a, b = clean_idx[0], clean_idx[-1] if len(clean_idx) == 2 else clean_idx[1]
        return np.column_stack([arr[:, a], arr[:, b]]).astype(np.float32)
    best = int(np.argmin(ch_clips))
    return arr[:, best].astype(np.float32)


def repair_stereo_phase(seg):
    """
    Ensure genuine stereo width without polarity inversion, comb-filtering ITD lag, or negative correlation.
    Dual-mono sources fold cleanly to 1D mono; real stereo keeps true L/R [N, 2] with healthy phase coherence.
    """
    if seg.ndim != 2 or seg.shape[1] != 2 or len(seg) < 64:
        return seg if seg.ndim == 1 else seg[:, 0].astype(np.float32)

    l = seg[:, 0].astype(np.float32)
    r = seg[:, 1].astype(np.float32)
    diff_pk = float(np.max(np.abs(l - r)))
    mid = 0.5 * (l + r)
    side = 0.5 * (l - r)
    em = float(np.mean(mid * mid) + 1e-18)
    es = float(np.mean(side * side) + 1e-18)
    side_db = 10.0 * math.log10(es / em)
    if diff_pk < 1.5e-4 or side_db < -42.0:
        return mid.astype(np.float32)

    # Full-clip + active-window polarity/ITD scan
    lf = l.astype(np.float64)
    rf = r.astype(np.float64)
    denom_full = float(math.sqrt(max(1e-18, float(np.sum(lf * lf) * np.sum(rf * rf)))))
    c_all = float(np.sum(lf * rf) / denom_full)

    n_win = min(len(l), int(0.45 * SR))
    spk = int(np.argmax(np.maximum(np.abs(l), np.abs(r))))
    w0 = max(0, min(len(l) - n_win, spk - n_win // 6))
    w1 = w0 + n_win
    lw = l[w0:w1].astype(np.float64)
    rw = r[w0:w1].astype(np.float64)
    denom = float(math.sqrt(max(1e-18, float(np.sum(lw * lw) * np.sum(rw * rw)))))
    c0 = float(np.sum(lw * rw) / denom)

    max_lag = min(120, n_win // 4)
    best_pos_c = c0
    best_pos_lag = 0
    best_abs_c = c0
    for lag in range(-max_lag, max_lag + 1, 2):
        if lag >= 0:
            x, y = lw[lag:], rw[: n_win - lag]
        else:
            x, y = lw[: n_win + lag], rw[-lag:]
        c = float(np.sum(x * y) / denom)
        if c > best_pos_c:
            best_pos_c = c
            best_pos_lag = lag
        if abs(c) > abs(best_abs_c):
            best_abs_c = c

    # Polarity repair: flip R if dominant active-window or full-clip correlation is out-of-phase
    if (c0 < -0.12 and best_abs_c < -0.18) or (best_abs_c < -0.25 and c0 < 0.05) or (c_all < -0.18 and c0 < 0.02):
        r = -r
        rf = -rf
        rw = -rw
        c0 = -c0
        c_all = -c_all
        # Recompute positive lag peak after polarity flip
        best_pos_c = c0
        best_pos_lag = 0
        for lag in range(-max_lag, max_lag + 1, 2):
            if lag >= 0:
                x, y = lw[lag:], rw[: n_win - lag]
            else:
                x, y = lw[: n_win + lag], rw[-lag:]
            c = float(np.sum(x * y) / denom)
            if c > best_pos_c:
                best_pos_c = c
                best_pos_lag = lag

    # Time-of-arrival realignment if spaced microphones differ by >0.3ms (keep <=0.21ms natural ITD)
    if abs(best_pos_lag) > 14 and best_pos_c >= max(0.32, c0 + 0.14):
        keep = int(np.sign(best_pos_lag)) * 10
        shift = best_pos_lag - keep
        if shift > 0:
            r_new = np.zeros_like(r)
            r_new[shift:] = r[:-shift]
            r = r_new
        elif shift < 0:
            sh = -shift
            r_new = np.zeros_like(r)
            r_new[:-sh] = r[sh:]
            r = r_new

    # Phase coherence guard: gently tame anti-phase wide Side spill (corr < 0.25) while keeping true stereo
    lf2 = l.astype(np.float64)
    rf2 = r.astype(np.float64)
    denom2 = float(math.sqrt(max(1e-18, float(np.sum(lf2 * lf2) * np.sum(rf2 * rf2)))))
    c_post = float(np.sum(lf2 * rf2) / denom2)
    if c_post < 0.25:
        m2 = 0.5 * (l + r)
        s2 = 0.5 * (l - r)
        em2 = float(np.sum(m2.astype(np.float64) ** 2) + 1e-18)
        es2 = float(np.sum(s2.astype(np.float64) ** 2) + 1e-18)
        target_c = 0.28
        ratio = ((1.0 - target_c) / (1.0 + target_c)) * (em2 / es2)
        k_scale = max(0.38, min(1.0, math.sqrt(max(0.0, ratio))))
        s2 *= k_scale
        l = m2 + s2
        r = m2 - s2

    return np.column_stack([l, r]).astype(np.float32)


def decode_source(ref):
    """Decode WAV (mono or true stereo) to 48 kHz float32 with pre-resample safety & raw clip map."""
    cache_key = ref
    if cache_key in _WAV_CACHE:
        return _WAV_CACHE[cache_key]
    full, _, _ = resolve_ref(ref)
    if not os.path.isfile(full):
        raise FileNotFoundError(f"Missing source audio file: {full}")
    sr, bits, arr = read_wav_float32(full)
    clip_lim = 0.999 if bits >= 24 else (32766.0 / 32768.0)

    sig = downmix_spatial_to_lr(arr, clip_lim)
    # Build per-frame clipping mask BEFORE downsampling/scaling so we never promote clipped peaks
    if arr.shape[1] == 1:
        raw_clipped = np.abs(arr[:, 0]) >= clip_lim
    elif sig.ndim == 1:
        # Selected clean single channel when downmix chose 1D
        raw_clipped = np.abs(sig) >= clip_lim
    else:
        raw_clipped = (np.abs(sig[:, 0]) >= clip_lim) | (np.abs(sig[:, 1]) >= clip_lim)

    if sig.ndim == 1:
        pk = float(np.max(np.abs(sig)) + 1e-12)
        scale = min(1.0, 0.81 / pk) if pk > 0.81 else 1.0
        out = resample_48k_hq((sig * scale).astype(np.float32), sr)
    else:
        pk = float(np.max(np.abs(sig)) + 1e-12)
        scale = min(1.0, 0.81 / pk) if pk > 0.81 else 1.0
        l48 = resample_48k_hq((sig[:, 0] * scale).astype(np.float32), sr)
        r48 = resample_48k_hq((sig[:, 1] * scale).astype(np.float32), sr)
        n = min(len(l48), len(r48))
        out = np.column_stack([l48[:n], r48[:n]]).astype(np.float32)

    # Resample raw clipping flags to 48k grid via nearest index lookup
    if np.any(raw_clipped):
        idx48 = np.minimum(len(raw_clipped) - 1,
                           np.round(np.arange(len(out), dtype=np.float64) * (sr / float(SR))).astype(np.int64))
        clip48 = raw_clipped[idx48]
    else:
        clip48 = np.zeros(len(out), dtype=bool)

    _WAV_CACHE[cache_key] = (out, clip48)
    return out, clip48


def _apply_fft_curve(x, gain_fn):
    if x.ndim == 1:
        n = len(x)
        nfft = 1 << int(np.ceil(np.log2(max(n, 64))))
        f = np.fft.rfftfreq(nfft, 1.0 / SR)
        g = gain_fn(f)
        return np.fft.irfft(np.fft.rfft(x, n=nfft) * g, n=nfft)[:n].astype(np.float32)
    n, ch = x.shape
    nfft = 1 << int(np.ceil(np.log2(max(n, 64))))
    f = np.fft.rfftfreq(nfft, 1.0 / SR)
    g = gain_fn(f)
    out = np.empty_like(x, dtype=np.float32)
    for c in range(ch):
        out[:, c] = np.fft.irfft(np.fft.rfft(x[:, c], n=nfft) * g, n=nfft)[:n]
    return out


def fft_hpf(x, cutoff=28.0):
    def curve(f):
        g = np.ones_like(f)
        low = f < max(5.0, cutoff * 0.65)
        tr = (f >= max(5.0, cutoff * 0.65)) & (f < cutoff)
        g[low] = 0.0
        if cutoff > 10.0:
            g[tr] = 0.5 * (1.0 - np.cos(np.pi * (f[tr] - cutoff * 0.65) / (cutoff * 0.35)))
        return g
    return _apply_fft_curve(x, curve)


def fft_peq(x, bands):
    if not bands:
        return x
    def curve(f_raw):
        f = f_raw + 1e-6
        gain_db = np.zeros_like(f)
        for fc, q, gdb in bands:
            bw = max(0.15, 1.0 / q)
            dist = np.log2(f / fc) / bw
            gain_db += gdb * np.exp(-0.5 * (dist ** 2))
        return 10.0 ** (gain_db / 20.0)
    return _apply_fft_curve(x, curve)


def _trim_seg(seg, min_s):
    amp = np.abs(seg) if seg.ndim == 1 else np.max(np.abs(seg), axis=1)
    spk = float(np.max(amp) + 1e-12)
    on = np.where(amp >= spk * 0.025)[0]
    if len(on):
        seg = seg[max(0, int(on[0]) - int(0.001 * SR)):]
    amp2 = np.abs(seg) if seg.ndim == 1 else np.max(np.abs(seg), axis=1)
    nz = np.where(amp2 >= spk * (10.0 ** (-56.0 / 20.0)))[0]
    if len(nz):
        seg = seg[: min(len(seg), int(nz[-1]) + int(0.065 * SR))]
    return seg if len(seg) >= min_s else None


def get_takes(ref, max_dur_ms=950.0, hpf_hz=30.0, prefer_clean=True, min_dur_ms=55.0,
              min_gap_ms=400.0, whole_file=False, repair_stereo=True):
    """Extract clean individual takes (1ch or 2ch stereo) from GDC, GDC Extra, صوتيات, or FFX."""
    raw_sig, clip48 = decode_source(ref)
    x = fft_hpf(raw_sig, hpf_hz)
    max_s = int(max_dur_ms * 0.001 * SR)
    min_s = int(min_dur_ms * 0.001 * SR)

    if whole_file:
        seg = x[: min(len(x), max_s)].copy()
        seg = _trim_seg(seg, min_s)
        if seg is None:
            return []
        return [repair_stereo_phase(seg) if repair_stereo else seg]

    # Check if gdc_analysis already gives verified take timestamps (only when min_gap_ms >= 100)
    _, _, ana_entry = resolve_ref(ref)
    spans = []
    if ana_entry is not None and min_gap_ms >= 150.0:
        all_t = ana_entry["takes"]
        takes = list(all_t)
        if prefer_clean:
            clean = [t for t in takes if t["clipped"] == 0]
            if clean:
                takes = clean
            else:
                takes.sort(key=lambda t: (t["clipped"], -t["tail_ms"]))
        for t in takes:
            s0 = max(0, int(round(t["start_s"] * SR)))
            orig_i = t["idx"]
            s_next = int(round(all_t[orig_i + 1]["start_s"] * SR)) - int(0.018 * SR) if orig_i + 1 < len(all_t) else len(x)
            s1 = min(len(x), s_next, s0 + max_s)
            if s1 - s0 >= min_s:
                spans.append((s0, s1))
    else:
        # Adaptive block-peak onset detector for صوتيات, gdc_extra, FFX, and burst slicing
        amp = np.abs(x) if x.ndim == 1 else np.max(np.abs(x), axis=1)
        win = int(0.005 * SR)
        nb = len(amp) // win
        if nb < 2:
            seg = _trim_seg(x[: min(len(x), max_s)].copy(), min_s)
            return ([repair_stereo_phase(seg) if repair_stereo else seg] if seg is not None else [])
        bx = np.max(amp[: nb * win].reshape(nb, win), axis=1)
        gpk = float(np.max(bx) + 1e-12)
        nf = float(np.percentile(bx, 15))
        # Adaptive ceiling on nf*5.0 handles sustained bursts/tails spanning >85% of file
        trig = max(gpk * 0.14, min(gpk * 0.58, nf * 5.0))
        gap_b = max(1, int(min_gap_ms * 0.001 * SR / win))
        onsets = []
        i = 0
        while i < nb:
            if bx[i] >= trig:
                j = i
                while j > 0 and bx[j - 1] >= max(bx[i] * 0.05, nf * 2.5) and (i - j) < 6:
                    j -= 1
                onsets.append(j * win)
                i += gap_b
            else:
                i += 1
        cand = []
        for idx, s0 in enumerate(onsets):
            s_next = onsets[idx + 1] - int(0.015 * SR) if idx + 1 < len(onsets) else len(x)
            s1 = min(len(x), s_next, s0 + max_s)
            if s1 - s0 >= min_s:
                cl = int(np.sum(clip48[s0:s1]))
                cand.append((cl, s0, s1))
        if prefer_clean:
            clean = [c for c in cand if c[0] == 0]
            if clean:
                cand = clean
            else:
                cand.sort(key=lambda c: c[0])
        spans = [(s0, s1) for _, s0, s1 in cand]

    out = []
    for s0, s1 in spans:
        seg = _trim_seg(x[s0:s1].copy(), min_s)
        if seg is None:
            continue
        if repair_stereo:
            seg = repair_stereo_phase(seg)
        out.append(seg)
    return out


def get_tails(ref, skip_ms=42.0, max_dur_ms=2300.0, hpf_hz=42.0, min_gap_ms=400.0):
    """Extract pure rolling outdoor/indoor stereo acoustic tails after close muzzle attack."""
    takes = get_takes(ref, max_dur_ms=skip_ms + max_dur_ms, hpf_hz=hpf_hz, prefer_clean=True,
                      min_dur_ms=280.0, min_gap_ms=min_gap_ms)
    tails = []
    skip_s = int(skip_ms * 0.001 * SR)
    fade_in = int(0.016 * SR)
    for seg in takes:
        if len(seg) <= skip_s + int(0.25 * SR):
            continue
        tail = seg[skip_s:].copy()
        fi = min(fade_in, len(tail) // 5)
        if fi > 4:
            ramp = np.linspace(0.0, 1.0, fi, dtype=np.float32)
            tail[:fi] *= ramp if tail.ndim == 1 else ramp[:, None]
        tails.append(tail)
    return tails


def get_long_events(ref, min_gap_ms=1100.0, max_dur_ms=1800.0, min_dur_ms=420.0, hpf_hz=26.0):
    """Extract sustained multi-pulse bursts / long explosions / rocket passbys cleanly in stereo."""
    raw_sig, clip48 = decode_source(ref)
    x = fft_hpf(raw_sig, hpf_hz)
    amp = np.abs(x) if x.ndim == 1 else np.max(np.abs(x), axis=1)
    win = int(0.005 * SR)
    nb = len(amp) // win
    if nb < 4:
        return [repair_stereo_phase(x)] if len(x) >= int(min_dur_ms * 0.001 * SR) else []
    bx = np.max(amp[: nb * win].reshape(nb, win), axis=1)
    gpk = float(np.max(bx) + 1e-12)
    nf = float(np.percentile(bx, 15))
    trig = max(gpk * 0.16, min(gpk * 0.62, nf * 6.0))
    gap_b = max(1, int(min_gap_ms * 0.001 * SR / win))
    events = []
    i = 0
    while i < nb:
        if bx[i] >= trig:
            j = i
            while j > 0 and bx[j - 1] >= max(bx[i] * 0.04, nf * 3.0) and (i - j) < 12:
                j -= 1
            events.append(j * win)
            i += gap_b
        else:
            i += 1
    out = []
    max_s = int(max_dur_ms * 0.001 * SR)
    min_s = int(min_dur_ms * 0.001 * SR)
    for idx, s0 in enumerate(events):
        s_next = events[idx + 1] - int(0.025 * SR) if idx + 1 < len(events) else len(x)
        s1 = min(len(x), s_next, s0 + max_s)
        if s1 - s0 >= min_s and int(np.sum(clip48[s0:s1])) == 0:
            seg = x[s0:s1].copy()
            s_amp = np.abs(seg) if seg.ndim == 1 else np.max(np.abs(seg), axis=1)
            spk = float(np.max(s_amp) + 1e-12)
            nz = np.where(s_amp >= spk * (10.0 ** (-52.0 / 20.0)))[0]
            if len(nz):
                seg = seg[: min(len(seg), int(nz[-1]) + int(0.08 * SR))]
            if len(seg) >= min_s:
                out.append(repair_stereo_phase(seg))
    return out


def get_loop_slices(ref, dur_ms=2100.0, count=3, xfade_ms=90.0, hpf_hz=38.0):
    """Extract evenly spaced, click-free equal-power crossfaded stereo loops from steady beds."""
    raw_sig, _ = decode_source(ref)
    x = repair_stereo_phase(fft_hpf(raw_sig, hpf_hz))
    xf = int(xfade_ms * 0.001 * SR)
    seg_s = int(dur_ms * 0.001 * SR)
    total = len(x)
    # Trim very quiet intro/outro
    amp = np.abs(x) if x.ndim == 1 else np.max(np.abs(x), axis=1)
    spk = float(np.max(amp) + 1e-12)
    act = np.where(amp >= spk * 0.08)[0]
    a0 = int(act[0]) if len(act) else 0
    a1 = int(act[-1]) if len(act) else total
    if a1 - a0 < seg_s + xf + 64:
        a0, a1 = 0, total
    avail = max(seg_s + xf, a1 - a0)
    seg_s = min(seg_s, max(int(0.65 * SR), avail - xf - 16))
    step = max(1, (avail - seg_s - xf) // max(1, count))
    loops = []
    for k in range(count):
        s0 = min(max(0, total - seg_s - xf - 1), a0 + k * step)
        body = x[s0 : s0 + seg_s].copy()
        head_blend = x[s0 + seg_s : s0 + seg_s + xf]
        if len(head_blend) == xf and xf > 16:
            t = np.linspace(0.0, np.pi * 0.5, xf, dtype=np.float32)
            fi = np.sin(t)
            fo = np.cos(t)
            if body.ndim == 2:
                body[:xf] = body[:xf] * fi[:, None] + head_blend * fo[:, None]
            else:
                body[:xf] = body[:xf] * fi + head_blend * fo
        loops.append(body)
    return loops


def norm_unit(seg):
    mx = float(np.max(np.abs(seg)))
    return (seg / mx).astype(np.float32) if mx > 1e-6 else seg.astype(np.float32)


def _attack_sample(seg):
    amp = np.abs(seg) if seg.ndim == 1 else np.max(np.abs(seg), axis=1)
    pk = float(np.max(amp) + 1e-12)
    hits = np.where(amp >= pk * 0.35)[0]
    return int(hits[0]) if len(hits) else 0


def align_mix(layers, out_max_ms=None, force_mono=False):
    """Layer mono/stereo stems with attack transient lock (zero accidental L/R collapse)."""
    valid = [(norm_unit(s), w, off) for s, w, off in layers if s is not None and len(s) > 16]
    if not valid:
        return np.zeros(int(0.1 * SR), dtype=np.float32)

    use_stereo = (not force_mono) and any(s.ndim == 2 and s.shape[1] == 2 for s, _, _ in valid)
    # Lock simultaneous layers' attack onset within 1 ms if layer is intended for t=0
    atk0 = _attack_sample(valid[0][0])
    shifted = []
    for u, weight, offset_ms in valid:
        s_use = u
        if offset_ms == 0:
            atk = _attack_sample(s_use)
            diff = atk - atk0
            if diff > int(0.0018 * SR):
                s_use = s_use[diff:]
        if use_stereo:
            if s_use.ndim == 1:
                s_st = np.column_stack([s_use * 0.7071, s_use * 0.7071]).astype(np.float32)
            else:
                s_st = s_use.astype(np.float32)
            shifted.append((s_st, weight, offset_ms))
        else:
            s_mo = np.mean(s_use, axis=1).astype(np.float32) if s_use.ndim == 2 else s_use.astype(np.float32)
            shifted.append((s_mo, weight, offset_ms))

    buf_len = max(len(s) + int(off * 0.001 * SR) for s, _, off in shifted)
    if out_max_ms:
        buf_len = min(buf_len, int(out_max_ms * 0.001 * SR))

    mix = np.zeros((buf_len, 2), dtype=np.float32) if use_stereo else np.zeros(buf_len, dtype=np.float32)
    for u, weight, offset_ms in shifted:
        off = max(0, int(offset_ms * 0.001 * SR))
        if off >= buf_len:
            continue
        cnt = min(len(u), buf_len - off)
        mix[off : off + cnt] += u[:cnt] * float(weight)
    return mix


def master_clip(seg, eq_bands=None, transient_boost_db=1.5, comp_thresh_db=-14.0,
                comp_ratio=2.3, ceil_db=-1.2, fade_out_ms=45.0, force_mono=False,
                width_pct=100, is_loop=False, in_room_punch=False, supp_punch=False):
    """True-stereo HDR mastering: M/S <120Hz mono bass anchor, parallel body punch, 0 clipping."""
    x = seg.astype(np.float32).copy()
    if force_mono and x.ndim == 2:
        x = np.mean(x, axis=1).astype(np.float32)
    if x.ndim == 2 and x.shape[1] == 2:
        x = repair_stereo_phase(x)
    if len(x) < 64:
        return x, (2 if x.ndim == 2 else 1), int(width_pct)

    eff_eq = list(eq_bands) if eq_bands else []
    if in_room_punch:
        eff_eq.extend([(68.0, 1.15, 2.8), (135.0, 1.25, 3.6), (2150.0, 1.35, 1.6)])
    elif supp_punch:
        eff_eq.extend([(125.0, 1.25, 2.2), (2400.0, 1.35, 1.2)])
    if eff_eq:
        x = fft_peq(x, eff_eq)

    # Parallel low-frequency harmonic saturation (sub-bass translates physically on mobile speakers)
    if (in_room_punch or supp_punch) and not is_loop:
        lp_band = x - fft_hpf(x, cutoff=235.0)
        lp_pk = float(np.max(np.abs(lp_band)) + 1e-9)
        if lp_pk > 1e-6:
            drv = 2.3 if in_room_punch else 1.7
            wt = 0.24 if in_room_punch else 0.14
            harm = np.tanh((lp_band / lp_pk) * drv) * lp_pk
            x = x + wt * harm

    # Phase-locked <120 Hz bass anchor on Side channel so sub punch stays mono-compatible on phones
    if x.ndim == 2 and x.shape[1] == 2:
        mid = 0.5 * (x[:, 0] + x[:, 1])
        side = fft_hpf(0.5 * (x[:, 0] - x[:, 1]), cutoff=120.0)
        x = np.column_stack([mid + side, mid - side]).astype(np.float32)

    amp = np.abs(x) if x.ndim == 1 else np.max(np.abs(x), axis=1)

    # Shape ultra-short needle spikes (>99.6% transient shelf) before body compression so they don't starve body RMS
    if (in_room_punch or supp_punch) and not is_loop and len(amp) > 128:
        pk_all = float(np.max(amp) + 1e-12)
        p996 = max(float(np.percentile(amp, 99.6)), pk_all * 0.24)
        knee = p996 * (1.18 if in_room_punch else 1.35)
        if pk_all > knee * 1.12:
            over = np.maximum(0.0, amp - knee)
            scale = (knee + (p996 * 0.42) * np.tanh(over / max(p996 * 0.42, 1e-6))) / (amp + 1e-12)
            mask = amp > knee
            g_sp = np.where(mask, scale, 1.0).astype(np.float32)
            x *= g_sp if x.ndim == 1 else g_sp[:, None]
            amp = np.abs(x) if x.ndim == 1 else np.max(np.abs(x), axis=1)

    # Crisp 0..12ms muzzle transient preservation (coupled L+R)
    if transient_boost_db > 0.0 and not is_loop:
        spk = float(np.max(amp))
        hits = np.where(amp >= spk * 0.35)[0]
        if len(hits):
            t0 = int(hits[0])
            tr_len = int(0.012 * SR)
            t1 = min(len(x), t0 + tr_len)
            g_tr = 10.0 ** (transient_boost_db / 20.0)
            env = np.ones(len(x), dtype=np.float32)
            env[t0:t1] = np.linspace(g_tr, 1.0, t1 - t0, dtype=np.float32)
            x *= env if x.ndim == 1 else env[:, None]

    # Coupled RMS envelope follower + parallel NYC-style upward punch on close gunfire
    amp = np.abs(x) if x.ndim == 1 else np.max(np.abs(x), axis=1)
    pk0 = float(np.max(amp) + 1e-12)
    x /= pk0
    ax = amp / pk0
    thresh = 10.0 ** (comp_thresh_db / 20.0)
    atk_c = float(np.exp(-1.0 / (0.002 * SR)))
    rel_c = float(np.exp(-1.0 / (0.080 * SR)))
    env = 0.0
    gain_arr = np.ones(len(x), dtype=np.float32)
    slope = 1.0 - (1.0 / comp_ratio)
    for i in range(len(x)):
        s = float(ax[i])
        c = atk_c if s > env else rel_c
        env = c * env + (1.0 - c) * s
        if env > thresh:
            over_db = 20.0 * math.log10(env / thresh)
            gain_arr[i] = 10.0 ** ((-over_db * slope) / 20.0)
    x_comp = x * (gain_arr if x.ndim == 1 else gain_arr[:, None])

    if (in_room_punch or supp_punch) and not is_loop:
        # Upward parallel body lift: compress decay floor smoothly while preserving sharp attack punch
        p_thresh = 10.0 ** ((-27.0 if in_room_punch else -24.0) / 20.0)
        p_atk = float(np.exp(-1.0 / (0.004 * SR)))
        p_rel = float(np.exp(-1.0 / (0.048 * SR)))
        p_slope = 1.0 - (1.0 / (5.2 if in_room_punch else 3.4))
        p_env = 0.0
        p_gain = np.ones(len(x), dtype=np.float32)
        for i in range(len(x)):
            s = float(ax[i])
            c = p_atk if s > p_env else p_rel
            p_env = c * p_env + (1.0 - c) * s
            if p_env > p_thresh:
                od = 20.0 * math.log10(p_env / p_thresh)
                p_gain[i] = 10.0 ** ((-od * p_slope) / 20.0)
        smash = x * (p_gain if x.ndim == 1 else p_gain[:, None])
        # Makeup only when normalized RMS is modest; never overdrive continuous loops/explosions
        smash_rms = float(np.sqrt(np.mean(smash * smash)) + 1e-9)
        dry_rms = float(np.sqrt(np.mean(x_comp * x_comp)) + 1e-9)
        makeup = min(3.8 if in_room_punch else 2.2, (dry_rms / smash_rms) * (2.65 if in_room_punch else 1.55))
        dry_mix = 0.56 if in_room_punch else 0.70
        smash_mix = 0.62 if in_room_punch else 0.40
        x = x_comp * dry_mix + np.tanh(smash * makeup) * (0.48 if in_room_punch else 0.32) * smash_mix

        # Tight asymmetric in-room early reflections (9..23 ms, HPF >160 Hz) put shot inside the room
        if in_room_punch and x.ndim == 2 and x.shape[1] == 2:
            refl_src = fft_hpf(x, cutoff=160.0)
            refl = np.zeros_like(x)
            for ms_d, ch_i, g_r in ((9.5, 0, 0.24), (16.4, 0, 0.15), (12.8, 1, 0.24), (22.6, 1, 0.15)):
                d = int(ms_d * 0.001 * SR)
                if d < len(x):
                    refl[d:, ch_i] += refl_src[:-d, ch_i] * g_r
            x = x + refl
    else:
        x = x_comp

    # Target strong body RMS (-10.0..-13.5 dBFS across primary gunfire) without spike starvation
    if (in_room_punch or supp_punch) and not is_loop:
        pk_pre = float(np.max(np.abs(x)) + 1e-12)
        x /= pk_pre
        rms_now = float(np.sqrt(np.mean(x * x)) + 1e-9)
        target_rms = 10.0 ** ((-10.6 if in_room_punch else -14.2) / 20.0)
        if rms_now < target_rms:
            push = min(5.5 if in_room_punch else 2.4, target_rms / rms_now)
            # Soft waveshaping lifts gunpowder body smoothly without square-clipping
            x = np.tanh(x * push) / math.tanh(push)
            if in_room_punch:
                # Second gentle body-density polish if long decay pulled full-clip RMS low
                rms2 = float(np.sqrt(np.mean(x * x)) + 1e-9)
                t2 = 10.0 ** (-11.6 / 20.0)
                if rms2 < t2:
                    p2 = min(2.1, t2 / rms2)
                    x = np.tanh(x * p2) / math.tanh(p2)

    # Ensure strictly positive stereo correlation after wide FX/reflections
    if x.ndim == 2 and x.shape[1] == 2:
        el = float(np.sum(x[:, 0].astype(np.float64) ** 2))
        er = float(np.sum(x[:, 1].astype(np.float64) ** 2))
        corr_chk = float(np.sum(x[:, 0].astype(np.float64) * x[:, 1].astype(np.float64)) / (math.sqrt(el * er) + 1e-18))
        if corr_chk < 0.20:
            mid = 0.5 * (x[:, 0] + x[:, 1])
            side = 0.5 * (x[:, 0] - x[:, 1]) * 0.65
            x = np.column_stack([mid + side, mid - side]).astype(np.float32)

    # Transparent peak limiter to exact ceil_db ceiling (0 clipped samples guaranteed)
    ceil_lin = 10.0 ** (ceil_db / 20.0)
    pk = float(np.max(np.abs(x)) + 1e-12)
    x = (x / pk) * ceil_lin
    drive = 1.04
    x = (np.tanh(x * drive) / math.tanh(ceil_lin * drive)) * ceil_lin
    pk2 = float(np.max(np.abs(x)) + 1e-12)
    if pk2 > ceil_lin:
        x *= (ceil_lin / pk2)

    if not is_loop:
        fi = min(len(x) // 10, int(0.0003 * SR))
        fo = min(len(x) // 3, int(fade_out_ms * 0.001 * SR))
        if fi > 2:
            r_in = np.linspace(0.0, 1.0, fi, dtype=np.float32)
            x[:fi] *= r_in if x.ndim == 1 else r_in[:, None]
        if fo > 4:
            t = np.linspace(0.0, np.pi * 0.5, fo, dtype=np.float32)
            r_out = (np.cos(t) ** 2).astype(np.float32)
            x[-fo:] *= r_out if x.ndim == 1 else r_out[:, None]

    ch = 2 if (x.ndim == 2 and x.shape[1] == 2) else 1
    out = np.clip(x, -0.998, 0.998).astype(np.float32)
    return out, ch, (int(width_pct) if ch == 2 else 100)


def stats_of(x):
    if x.ndim == 2:
        mono = 0.5 * (x[:, 0] + x[:, 1])
        side = 0.5 * (x[:, 0] - x[:, 1])
        l, r = x[:, 0].astype(np.float64), x[:, 1].astype(np.float64)
        el, er = float(np.sum(l * l)), float(np.sum(r * r))
        corr = float(np.sum(l * r) / (math.sqrt(el * er) + 1e-18)) if el > 1e-12 and er > 1e-12 else 1.0
        side_db = 10.0 * math.log10((float(np.mean(side * side)) + 1e-18) / (float(np.mean(mono * mono)) + 1e-18))
        ch = 2
    else:
        mono = x
        corr = 1.0
        side_db = -140.0
        ch = 1
    pk = float(np.max(np.abs(x)) + 1e-12)
    rms = float(np.sqrt(np.mean(mono ** 2) + 1e-12))
    nfft = min(len(mono), 16384)
    sp = np.abs(np.fft.rfft(mono[:nfft] * np.hanning(nfft))) ** 2
    fr = np.fft.rfftfreq(nfft, 1.0 / SR)
    tot = float(np.sum(sp) + 1e-18)
    cen = float(np.sum(fr * sp) / tot)
    sub = float(np.sum(sp[(fr >= 25) & (fr < 150)]) / tot * 100.0)
    crk = float(np.sum(sp[(fr >= 1000) & (fr < 5000)]) / tot * 100.0)
    air = float(np.sum(sp[fr >= 5000]) / tot * 100.0)
    return {
        "ch": ch,
        "dur_ms": round(len(x) * 1000.0 / SR, 1),
        "pk_db": round(20.0 * math.log10(pk), 2),
        "rms_db": round(20.0 * math.log10(rms), 2),
        "crest_db": round(20.0 * math.log10(pk / rms), 2),
        "corr": round(corr, 3),
        "side_db": round(side_db, 1),
        "cen_hz": int(round(cen)),
        "sub_pct": round(sub, 1),
        "crk_pct": round(crk, 1),
        "air_pct": round(air, 1),
        "clipped": int(np.sum(np.abs(x) >= 0.9985)),
    }


def pick(slices, idx):
    return slices[idx % len(slices)]


def build_all_clips():
    print("Mastering BWAR v3 (48 kHz True Stereo + M/S Bass Anchor) prioritizing صوتيات + FFX 192k + GDC Extra + GDC...")
    T = lambda ref, mx=850.0, hpf=30.0, gap=400.0, whole=False: get_takes(
        ref, max_dur_ms=mx, hpf_hz=hpf, prefer_clean=True, min_gap_ms=gap, whole_file=whole
    )
    L = lambda ref, sk=42.0, mx=2200.0, hpf=40.0, gap=400.0: get_tails(
        ref, skip_ms=sk, max_dur_ms=mx, hpf_hz=hpf, min_gap_ms=gap
    )

    # =========================================================================
    # CURATED SLICED POOLS — ZERO SHARED PRIMARY FIREARM SOURCES!
    # =========================================================================
    # 1. صوتيات (User Folder First!)
    mf_pst = (
        T(S("Modern_Firearms_Military_Pack/Gun_Pistol_9mm_Shot_var01.wav"), 800, 32, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Gun_Pistol_9mm_Shot_var02.wav"), 800, 32, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Gun_Pistol_9mm_Shot_var03.wav"), 800, 32, whole=True)
    )
    luger_c = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/Super Thump - Weapons of World War II - Designed/STDSGN_WoWW2_Wep_Pistol_Luger_Shot_X5.wav"), 780, 32, gap=450.0)
    tt33_c  = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/Pole Position - Tokarev TT-33 semi-automatic pistol/TT33, Firing, t1, Inside Car, Omni, CMC6.wav"), 640, 35, gap=450.0)

    mf_asr = (
        T(S("Modern_Firearms_Military_Pack/Gun_Assault_Rifle_Burst_var01.wav"), 800, 32, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Gun_Assault_Rifle_Burst_var02.wav"), 800, 32, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Gun_Assault_Rifle_Burst_var03.wav"), 800, 32, whole=True)
    )
    mf_shg = (
        T(S("Modern_Firearms_Military_Pack/Gun_Shotgun_Blast_Pump_var01.wav"), 920, 28, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Gun_Shotgun_Blast_Pump_var02.wav"), 920, 28, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Gun_Shotgun_Blast_Pump_var03.wav"), 920, 28, whole=True)
    )
    wj_shg = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/Wav Junction Sound Effects - Shotguns/0003_Shotgun_Fire_3.wav"), 900, 28, whole=True)
    mf_snr = (
        T(S("Modern_Firearms_Military_Pack/Gun_Sniper_Rifle_Heavy_var01.wav"), 1020, 26, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Gun_Sniper_Rifle_Heavy_var02.wav"), 1020, 26, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Gun_Sniper_Rifle_Heavy_var03.wav"), 1020, 26, whole=True)
    )
    mf_sup = (
        T(S("Modern_Firearms_Military_Pack/Gun_Silenced_Suppressed_var01.wav"), 520, 38, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Gun_Silenced_Suppressed_var02.wav"), 520, 38, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Gun_Silenced_Suppressed_var03.wav"), 520, 38, whole=True)
    )
    ak0405_d = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/FLYSOUND - AK-74/AK0405 AK-74 rear long distance burst 05 rounds.wav"), 850, 35, gap=70.0)
    m3gg_c   = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/Super Thump - M3 Grease Gun Recorded Source/STSRC_WoWW2_Wep_SMG_M3_GreaseGun_Submachine_Gun_Auto_Shot_X7_Attached.wav"), 560, 35, gap=70.0)
    m1919_d  = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/Super Thump - Weapons of World War II - Designed/STDSGN_WoWW2_Wep_MG_M1919_Machinegun_Auto-Burst_Shot_X6.wav"), 680, 30, gap=450.0)
    k98_fire = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/Pole Position - Mauser Karabiner 98 kurz K98k bolt-action rifle/K98k, Firing, t2, MKH416.wav"), 880, 32, gap=450.0)
    mg42_c   = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/Pole Position - MG 42 machine gun/MG 42, Firing, t1, Burst, Long, Mountain Top, D100.wav"), 620, 38, gap=65.0)
    ppsh_fire = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/Pole Position - Tokarev PPSh-41 submachine gun/PPSh41, Firing, t1, Burst, Long, MKH416.wav"), 550, 40, gap=65.0)

    mf_exp = (
        get_long_events(S("Modern_Firearms_Military_Pack/Tactical_Frag_Grenade_Explode_var01.wav"), 1200, 1800, 650, 24) +
        get_long_events(S("Modern_Firearms_Military_Pack/Tactical_Frag_Grenade_Explode_var02.wav"), 1200, 1800, 650, 24) +
        get_long_events(S("Modern_Firearms_Military_Pack/Tactical_Frag_Grenade_Explode_var03.wav"), 1200, 1800, 650, 24)
    )
    mf_knife = (
        T(S("Modern_Firearms_Military_Pack/Tactical_Knife_Stab_Slash_var01.wav"), 650, 35, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Tactical_Knife_Stab_Slash_var02.wav"), 650, 35, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Tactical_Knife_Stab_Slash_var03.wav"), 650, 35, whole=True)
    )
    mf_bolt = (
        T(S("Modern_Firearms_Military_Pack/Foley_Bolt_Cock_Pull_var01.wav"), 650, 50, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Foley_Bolt_Cock_Pull_var02.wav"), 650, 50, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Foley_Bolt_Cock_Pull_var03.wav"), 650, 50, whole=True)
    )
    mf_mage = (
        T(S("Modern_Firearms_Military_Pack/Foley_Mag_Eject_Empty_var01.wav"), 650, 50, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Foley_Mag_Eject_Empty_var02.wav"), 650, 50, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Foley_Mag_Eject_Empty_var03.wav"), 650, 50, whole=True)
    )
    mf_magi = (
        T(S("Modern_Firearms_Military_Pack/Foley_Mag_Insert_Lock_var01.wav"), 650, 50, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Foley_Mag_Insert_Lock_var02.wav"), 650, 50, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Foley_Mag_Insert_Lock_var03.wav"), 650, 50, whole=True)
    )
    mf_shdrp = (
        T(S("Modern_Firearms_Military_Pack/Foley_Bullet_Shell_Drop_var01.wav"), 550, 110, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Foley_Bullet_Shell_Drop_var02.wav"), 550, 110, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Foley_Bullet_Shell_Drop_var03.wav"), 550, 110, whole=True)
    )

    # 2. FFX 192 kHz / 24-bit Unclipped Master Tracks
    ffx_1911  = T(F("1911/A_35.wav"), 760, 32, gap=450.0)
    ffx_ak47  = T(F("AK-47/C_25.wav"), 780, 30, gap=450.0)
    ffx_ar15  = T(F("AR-15/D_31.wav"), 740, 30, gap=450.0)
    ffx_ppsh  = T(F("PPSh/P_24.wav"), 620, 34, gap=450.0)
    ffx_tikka = T(F("Tikka/W_27.wav"), 980, 26, gap=450.0)
    ffx_sup22 = T(F("Marvel Suppressed .22 Handgun/J_2.wav"), 480, 42, gap=450.0)
    ffx_sup300 = T(F("Savage 10 .300 Blackout/T_15.wav"), 520, 36, gap=450.0)
    ffx_m12_h = T(F("Model 12/K_3.wav"), 650, 55, gap=300.0) + T(F("Model 12/K_5.wav"), 650, 55, gap=300.0)
    ffx_nov_h = T(F("Nova/O_0.wav"), 650, 55, gap=300.0) + T(F("Nova/O_1.wav"), 650, 55, gap=300.0)
    ffx_ak_h  = T(F("AK-47/C_10.wav"), 650, 55, whole=True) + T(F("AK-47/C_11.wav"), 650, 55, whole=True)
    ffx_ar_h  = T(F("AR-15/D_13.wav"), 650, 55, whole=True) + T(F("AR-15/D_14.wav"), 650, 55, whole=True)
    ffx_19_h  = T(F("1911/A_11.wav"), 650, 55, whole=True) + T(F("1911/A_24.wav"), 650, 55, whole=True)
    ffx_ppq_h = T(F("Walther PPQ/X_11.wav"), 650, 55, gap=250.0) + T(F("Walther PPQ/X_23.wav"), 650, 55, gap=250.0)
    ffx_cg_h  = T(F("Carl Gustav M45_Swedish K/G_2.wav"), 650, 55, gap=250.0)
    ffx_sw_h  = T(F("Smith & Wesson 642/V_1.wav"), 650, 55, gap=250.0) + T(F("Smith & Wesson 642/V_2.wav"), 650, 55, gap=250.0)
    ffx_tik_h = T(F("Tikka/W_12.wav"), 650, 55, gap=250.0) + T(F("Tikka/W_2.wav"), 650, 55, whole=True)

    # 3. GDC Extra + Curated GDC Firearms (Clean True-Stereo First!)
    # Pistols
    vp70_c    = T(1005, 650)              # HK VP70 9mm 1m
    glock_c   = T(6202, 700)              # Glock 17 20m (2ch stereo)
    glock_rm  = T(6963, 600)              # Glock 17 Small Room
    p229_c    = T(1089, 650)              # SIG P229 9mm 1m
    deagle_c  = T(6881, 880, gap=450.0)   # Desert Eagle .44 4m Left CSS5 (2ch stereo!)
    python_c  = T(6875, 880, gap=450.0)   # Colt Python .357 4m Left CSS5 (2ch stereo!)
    dw445_c   = T(6879, 850, gap=450.0)   # Dan Wesson .445 SuperMag 1m
    mag44_c   = T(1095, 950)              # S&W M29 .44 Magnum 2m PZM
    m1911_c   = T(727, 850)               # Colt M1911 .45 Warehouse 10m (2ch stereo)

    # SMGs
    uzi_c     = T(1111, 600)              # Uzi 9mm 1m
    muzi_d    = T(1075, 700)              # Mini Uzi 30m
    tommy_c   = T(852, 580, gap=65.0)     # Tommy Gun Burst (2ch stereo)
    mac10_c   = T(1055, 520)              # Ingram M10 1m
    mac10_d   = T(1056, 650)              # Ingram M10 50m
    mp5_c     = T(1001, 580)              # HK MP5 2m (2ch stereo)
    mp5_d     = T(1002, 680)              # HK MP5 75m
    mp40_c    = T(851, 560, gap=65.0)     # MP40 Burst (2ch stereo)
    p90_c     = T(1043, 550)              # FN P90 5.7mm 1m
    fn57_c    = T(1041, 600)              # FN Five-seveN 5.7mm
    mp7_c     = T(1003, 550)              # HK MP7 4.6mm PDW

    # Rifles / MGs
    akm_c     = T(1007, 720)              # AKM 7.62x39mm 1m
    akm_d     = T(1008, 750)              # AKM 7.62x39mm 55m
    ak47_bt   = T(3086, 950)              # BR AK47 Burst With Tail
    g36_c     = T(1051, 700)              # HK G36C 5.56mm 1m
    famas_c   = T(1034, 720)              # FAMAS G2 5.56mm 1m
    m4_c      = T(67, 800)                # Coll Anderson M4 Shots 02 B (2ch stereo)
    hk416_c   = T(997, 720)               # HK 416 5.56mm 1m
    hk416_rm  = T(730, 800)               # Small Room HK 416
    l85_c     = T(1026, 750)              # Enfield L85 5.56mm 1m
    g3_c      = T(995, 780)               # HK G3 7.62x51mm 1m
    ak5_arm   = T(6989, 850)              # AK5 7.62mm near shooter

    # Shotguns & Snipers & Heavy
    r870_c    = T(729, 920)               # Remington 870 12g Warehouse
    ts_fire   = T(1303, 920)              # TS 12g Fire Round Eject (2ch stereo)
    ts_slug   = T(1304, 960)              # TS 12g Fire Slug (2ch stereo)
    saiga_ds  = T(6660, 880, gap=450.0)   # Saiga-12 DESIGNED Stereo
    ber1201_c = T(6859, 860, gap=450.0)   # Beretta 1201F
    spas_c    = T(6902, 850)              # SPAS-12 Pump Action 20m (2ch stereo)
    spas_semi = T(6903, 750)              # SPAS-12 Semi Auto 3m
    moss_cor  = T(6964, 900)              # Mossberg 12g Office Corridor (2ch stereo)
    m98b_c    = T(1012, 980)              # Barrett M98B .338 Lapua 2m
    lynx50_c  = T(6909, 1020)             # Lynx .50 Cal 4m
    svd_c     = T(6663, 1020)             # SVD Dragunov 7.62x54R (2ch stereo)
    r700_d    = T(1084, 850)              # Remington 700 50m

    # Heavy MGs & Launchers & Flame
    m1919a4_c = T(1067, 680, gap=65.0)    # Browning M1919A4 ORTF (2ch stereo)
    l110_c    = T(1060, 680)              # L110A2 M249 SAW
    l7a2_c    = T(1062, 720)              # L7A2 GPMG 7.62mm
    mg4_c     = T(999, 680)               # HK MG4 5.56mm
    rpg_ext   = T(2894, 1150, gap=380.0)  # Rocket Launcher Stereo (clean takes)
    rpg_fire  = T(6991, 1150)             # RPG Firing + Impact (2ch stereo)
    shell_trj = T(4440, 900)              # Shell Trajectory (2ch stereo)
    mk44_c    = T(850, 920, gap=450.0)    # MK44 Cannon Stereo (2ch stereo!)
    grl2_c    = T(2882, 820, gap=350.0)   # Grenade Launcher 2 Stereo (clean take)
    grl_smoke = T(5829, 780)              # Smoke Grenade Launcher (2ch stereo)
    tank_can  = T(4441, 750)              # Tank Artillery Thump
    flamer_c  = T(2893, 560, gap=350.0)   # Firestorm Flamer Stereo
    match_ig  = T(4679, 620, gap=350.0)   # Match Ignite Stereo
    gas_loops = get_loop_slices(1493, dur_ms=2100.0, count=3, xfade_ms=90.0, hpf_hz=42.0)

    # Suppressed (100% unclipped clean pool — fid=1020 dropped cleanly!)
    supp_pst  = T(2535, 500)              # Pistol Shot Silenced 04 (2ch stereo)
    supp_ppk  = T(2781, 620)              # PPK Cafeteria Suppressor (2ch stereo)
    supp_dl   = T(1021, 580)              # De Lisle .45ACP Suppressed 55m
    supp_trg  = T(1088, 580)              # Sako TRG Silencer 1m
    supp_snp  = T(2536, 720)              # Silenced Sniper Shot 01 (2ch stereo)
    supp_hiss = (
        T(S("Modern_Firearms_Military_Pack/Tactical_Smoke_Grenade_Hiss_var01.wav"), 150, 220.0, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Tactical_Smoke_Grenade_Hiss_var02.wav"), 150, 220.0, whole=True) +
        T(S("Modern_Firearms_Military_Pack/Tactical_Smoke_Grenade_Hiss_var03.wav"), 150, 220.0, whole=True)
    )
    aksd_813  = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/FLYSOUND - AK-74/AKSD0813 AK-74 spot A close burst 03 rounds.wav"), 580, 65.0, gap=65.0)
    aksd_854  = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/FLYSOUND - AK-74/AKSD0854 AK-74 spot D far burst 03 rounds.wav"), 820, 45.0, gap=250.0)
    mnsd_315  = L(S("Sonniss.com - GDC 2020 - Game Audio Bundle/FLYSOUND - Mosin Nagant/MNSD0315 MOSIN-NAGANT mixed long distance shot.wav"), 45, 1800, 40.0, 450.0)

    # Rolling Acoustic Tails ("Mattak") & Indoor Reflections (True Stereo!)
    t_m1911   = L(4935, 40, 1500) + L(727, 55, 1600)
    t_glk100  = L(6203, 35, 1400) + L(6881, 45, 1700)
    t_smg     = L(3089, 30, 1800) + L(1056, 35, 1300) + L(1002, 35, 1300)
    t_mattak  = L(2783, 45, 2400) + L(3086, 45, 2200) + aksd_854 + mnsd_315[:2]
    t_heavy   = L(5034, 45, 2300, gap=450.0) + L(1852, 40, 2200) + L(1063, 45, 1600) + L(6908, 45, 1800)
    t_shotgun = L(6660, 55, 2000, gap=450.0) + L(6902, 45, 1600) + L(1304, 55, 1900)
    t_ind_2779 = L(2779, 30, 1850)
    t_ind_6962 = L(6962, 35, 1900)
    t_ind_6964 = L(6964, 35, 1750)
    t_indoor = []
    for k in range(4):
        if k < len(t_ind_2779): t_indoor.append(t_ind_2779[k])
        if k < len(t_ind_6962): t_indoor.append(t_ind_6962[k])
        if k < len(t_ind_6964): t_indoor.append(t_ind_6964[k])
    t_indoor += L(2780, 30, 1750)
    t_supp    = L(2781, 35, 1200) + L(2536, 35, 1100) + L(1021, 35, 1100)

    # Far-Field Battlefield (100m-300m sustained bursts)
    dist_300  = (get_long_events(1245, 950.0, 1450.0, 550.0, 60.0) +
                 get_long_events(3087, 1000.0, 1550.0, 550.0, 60.0) +
                 get_long_events(1073, 950.0, 1300.0, 550.0, 75.0) +
                 get_long_events(5041, 1000.0, 1400.0, 550.0, 75.0) +
                 get_long_events(1851, 1200.0, 1650.0, 600.0, 42.0))

    # Supersonic Cracks & Bullet Flybys (prioritizing صوتيات PM_BBI + 1853/4847)
    pm_flyby  = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/PMSFX - Bullet Bys &Impacts/PM_BBI_Bullet_Passby_Whizzby_Airy_5.wav"), 580, 80.0, gap=300.0)
    whip_fly  = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/Olivier Girardot - Hand Guns Sound Effects Pack/Whip sound 21.wav"), 450, 80.0, gap=250.0)
    flyby_whz = pm_flyby[:6] + T(4847, 650, 80.0) + T(1606, 550, 80.0) + T(2514, 550, 80.0) + whip_fly
    crk_1853  = get_takes(1853, max_dur_ms=420.0, hpf_hz=110.0, prefer_clean=True, min_dur_ms=55.0)

    # Mechanicals, Reloads, Casings (صوتيات + FFX 192k + GDC)
    p38_mag   = T(5149, 700, 45.0)
    deag_mag  = T(6883, 700, 45.0)
    m4_chrg   = T(69, 750, 50.0)
    m4_unld   = T(70, 700, 50.0)
    g36_mag   = T(1113, 750, 45.0)
    svd_mag   = T(6662, 750, 45.0)
    spas_pmp  = T(6904, 620, 55.0)
    ts_load   = T(1302, 700, 55.0)
    ts_pump   = T(1305, 520, 55.0)
    sg_load   = T(6659, 620, 55.0)
    wj_pump   = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/Wav Junction Sound Effects - Shotguns/0019_Shotgun_Pump_Action_1.wav"), 520, 55.0, gap=200.0)
    wj_safe   = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/Wav Junction Sound Effects - Shotguns/0023_Shotgun_Safety_Switch_On_Off_2.wav"), 450, 60.0, gap=200.0)
    rev_cyl   = T(1115, 680, 55.0) + ffx_sw_h
    rev_ld    = T(6877, 680, 55.0)
    mp40_ck   = T(5042, 620, 55.0)
    ppsh_ck   = T(5118, 620, 55.0)
    mg42_cvr  = T(5035, 680, 50.0)
    k98_hdl   = T(5024, 620, 55.0)
    m1903_ck  = T(5095, 550, 55.0)
    tmp_f     = T(1116, 450, 65.0)
    semi_ck   = T(1624, 400, 65.0)
    dry_m1911 = T(4936, 280, 95.0)
    sil_scrw  = T(2537, 480, 75.0)
    tt33_clr  = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/Pole Position - Tokarev TT-33 semi-automatic pistol/TT33, Handling, Clearing, MK012.wav"), 600, 55.0, gap=300.0)
    ak74_asmb = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/FLYSOUND - AK-74/AK0709 AK-74 assemble disassemble.wav"), 650, 55.0, gap=350.0)
    cas_ak    = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/SculpTunes – Cartridges & Casings Shell/7 62 x 39 PPU  - Casings Shell & Cartdridges  - Impact and Roll on Tile Floor - Inside.wav"), 520, 140.0, gap=250.0) + T(5194, 520, 140.0)
    cas_9mm   = T(5195, 480, 160.0) + mf_shdrp
    cas_50    = T(3088, 520, 120.0)
    cas_shg   = T(1273, 550, 120.0)
    cas_wd    = T(2534, 500, 120.0)
    sh_hand   = T(5826, 480, 90.0)
    lynx_ej   = T(6910, 650, 65.0)

    # Impacts & Explosions
    imp_flesh = T(1607, 450, 45.0) + T(4846, 450, 45.0)
    imp_conc  = T(1608, 450, 55.0)
    imp_met   = T(1609, 550, 55.0)
    imp_dirt  = T(4844, 500, 55.0)
    imp_glss  = T(4845, 500, 80.0)
    exp_lg    = (mf_exp +
                 get_long_events(1611, 1500.0, 1850.0, 800.0, 24.0) +
                 get_long_events(4439, 1400.0, 1800.0, 800.0, 24.0) +
                 get_long_events(5828, 1500.0, 1850.0, 800.0, 24.0) +
                 get_long_events(6992, 1500.0, 1900.0, 800.0, 24.0))
    exp_deep  = (get_long_events(5515, 1500.0, 1850.0, 800.0, 24.0) +
                 get_long_events(1613, 1400.0, 1750.0, 800.0, 24.0) +
                 get_long_events(68, 1500.0, 1850.0, 800.0, 24.0))
    rpg_pass  = get_long_events(4440, 650.0, 1100.0, 350.0, 55.0)

    # Non-Weapon World / Foley / UI Pools (106..128)
    fs_conc_w = T(930, 420, 45.0, whole=True) + T(931, 420, 45.0, whole=True)
    fs_conc_r = T(2093, 380, 45.0, gap=250.0)[:4]
    fs_grass  = T(4888, 380, 50.0, whole=True) + T(4889, 380, 50.0, whole=True) + T(2094, 380, 50.0, gap=250.0)[:2]
    fs_dirt   = T(3704, 380, 45.0, whole=True) + T(3705, 380, 45.0, whole=True)
    fs_metal  = T(932, 380, 45.0, whole=True) + T(933, 380, 45.0, whole=True) + T(2095, 380, 45.0, gap=250.0)[:2]
    fs_water  = T(5646, 520, 45.0, gap=350.0)[:3] + T(33, 520, 45.0, gap=450.0)[:2]
    land_imp  = T(3877, 650, 32.0, whole=True) + T(2510, 550, 45.0, whole=True)
    bodyfalls = T(3878, 800, 30.0, whole=True) + T(3879, 800, 30.0, whole=True) + T(3877, 750, 30.0, whole=True)
    melee_sw  = T(4558, 450, 45.0, whole=True) + T(4560, 520, 45.0, whole=True) + T(5891, 520, 45.0, whole=True)
    melee_ht  = mf_knife + T(1631, 420, 35.0, whole=True) + T(5892, 520, 40.0, whole=True) + T(3649, 550, 40.0, gap=350.0)[:2] + T(1192, 420, 40.0, whole=True)
    water_sp  = T(4430, 1200, 35.0, gap=600.0)[:3] + T(4432, 850, 45.0, gap=450.0)[:2]
    pck_ammo  = T(2510, 520, 50.0, whole=True) + T(2522, 420, 50.0, whole=True) + T(972, 480, 65.0, gap=350.0)[:2]
    pck_hlth  = T(250, 550, 65.0, gap=350.0)[:2] + T(2522, 420, 50.0, whole=True)
    snpr_brth = T(1152, 1100, 35.0, gap=850.0)[:2] + T(1707, 900, 35.0, whole=True)
    heart_bt  = T(1151, 880, 26.0, gap=650.0)[:3]
    debris_rb = (T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/Bluezone - Tank - Explosion Sound Effects/Bluezone_BC0271_debris_falling_rock_dust_002.wav"), 1150, 35.0, gap=500.0)[:2] +
                 T(5815, 1150, 35.0, gap=600.0)[:2] + T(5816, 1150, 40.0, gap=500.0)[:2] + T(5822, 1150, 35.0, gap=500.0)[:2])
    car_wind  = T(36, 1100, 45.0, gap=600.0)[:2] + T(6631, 1100, 35.0, gap=600.0)[:2]
    wpn_drop  = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/aXLsound - Metal Sweeteners - 192 kHz/MtSw - Source - Fall - various objects.wav"), 680, 55.0, gap=400.0)[:3] + T(2510, 550, 55.0, whole=True)
    ui_conf   = T(484, 650, 60.0, whole=True) + T(2331, 650, 60.0, whole=True) + T(2140, 550, 60.0, whole=True)
    ui_hint   = T(2139, 550, 60.0, whole=True) + T(2141, 450, 60.0, whole=True) + T(2332, 650, 60.0, gap=300.0)[:2]
    ui_alert  = get_loop_slices(110, dur_ms=1450.0, count=2, xfade_ms=65.0, hpf_hz=60.0) + T(1911, 650, 60.0, whole=True)

    clips = []  # (slot_name, desc, arr, channels, width_pct, st_raw, st_mast, src_fid)

    def add(slot_name, desc, raw_arr, src_fid, **mkw):
        if raw_arr is None or len(raw_arr) < 128:
            raise RuntimeError(f"Empty clip for {slot_name}: {desc}")
        kw = dict(mkw)
        if slot_name.endswith("_FIRE") or slot_name in ("BNS_M203", "EXPLOSION", "FLAME_IGNITE"):
            kw.setdefault("in_room_punch", True)
            kw.setdefault("ceil_db", -0.95)
        elif slot_name.endswith("_SUPP"):
            kw.setdefault("supp_punch", True)
            kw.setdefault("ceil_db", -1.05)
        mastered, ch, width_pct = master_clip(raw_arr, **kw)
        clips.append((slot_name, desc, mastered, ch, width_pct, stats_of(norm_unit(raw_arr)), stats_of(mastered), src_fid))

    # =========================================================================
    # 1. ALL 18 BASE WEAPONS + FLAMETHROWER — 100% UNIQUE PRIMARY RECORDINGS!
    # =========================================================================
    # PST (SP-9mm Service Pistol): صوتيات Modern_Firearms Pistol + Luger + VP70
    eq_pst = [(115, 1.3, 2.2), (2700, 1.5, 2.0), (7000, 1.4, 1.2)]
    for i in range(4):
        add("PST_FIRE", f"Sotiyat Pistol9mm + Luger + GDC1005 VP70 #{i+1}",
            align_mix([(pick(mf_pst, i), 0.78, 0), (pick(luger_c, i), 0.42, 0), (pick(vp70_c, i), 0.34, 1)], 650),
            "sotiyat:Gun_Pistol_9mm+Luger+1005", eq_bands=eq_pst, comp_ratio=2.2, width_pct=96)

    # GK1 (Glock 19): GDC6202 Glock 17 Stereo + GDC6963 Room + GDC1089 P229
    eq_gk1 = [(130, 1.3, 2.0), (2100, 1.5, 2.2), (6200, 1.3, 1.6)]
    for i in range(4):
        add("GK1_FIRE", f"GDC6202 Glock 17 Stereo + GDC6963 + P229 #{i+1}",
            align_mix([(pick(glock_c, i), 0.74, 0), (pick(glock_rm, i), 0.40, 0), (pick(p229_c, i), 0.28, 2)], 700),
            "6202+6963+1089", eq_bands=eq_gk1, comp_ratio=2.2, width_pct=96)

    # MGN (.44/.357 Magnum Revolver): Desert Eagle Stereo + Colt Python Stereo + Dan Wesson + M29
    eq_mgn = [(70, 1.3, 4.2), (1450, 1.4, 2.4), (4400, 1.5, 1.6)]
    for i in range(4):
        add("MGN_FIRE", f"GDC6881 DEagle + 6875 Python + 6879 DW445 + 1095 M29 #{i+1}",
            align_mix([(pick(deagle_c, i), 0.62, 0), (pick(python_c, i), 0.52, 0), (pick(dw445_c, i), 0.36, 1), (pick(mag44_c, i), 0.34, 3)], 950),
            "6881+6875+6879+1095", eq_bands=eq_mgn, comp_ratio=2.1, width_pct=105)

    # SMG (Uzi 9mm): صوتيات M3 Grease Gun + GDC1111 Uzi + GDC1075 Mini Uzi + صوتيات TT33
    eq_smg = [(135, 1.4, 1.8), (2400, 1.5, 2.4), (6400, 1.3, 1.8)]
    for i in range(4):
        add("SMG_FIRE", f"Sotiyat M3GG + GDC1111 Uzi + 1075 MiniUzi + TT33 #{i+1}",
            align_mix([(pick(uzi_c, i), 0.64, 0), (pick(tt33_c, i), 0.44, 0), (pick(m3gg_c, i * 3), 0.36, 0), (pick(muzi_d, i), 0.36, 2)], 560),
            "1111+sotiyat:TT33+M3GG+1075", eq_bands=eq_smg, comp_ratio=2.2, width_pct=96)

    # SM3 (Ingram MAC-10 / Tommy .45): GDC852 Tommy Gun Stereo + GDC1055 M10 + GDC1056 + FFX PPSh Stereo
    eq_sm3 = [(155, 1.4, 1.6), (3100, 1.5, 2.6), (7200, 1.3, 2.0)]
    for i in range(4):
        add("SM3_FIRE", f"GDC852 Tommy + GDC1055 M10 + FFX PPSh Stereo #{i+1}",
            align_mix([(pick(tommy_c, i), 0.64, 0), (pick(mac10_c, i), 0.50, 0), (pick(ffx_ppsh, i), 0.38, 1), (pick(mac10_d, i), 0.30, 3)], 540),
            "852+1055+ffx:P_24", eq_bands=eq_sm3, comp_ratio=2.3, width_pct=98)

    # MP1 (HK MP5): GDC1001 MP5 2m Stereo + GDC1002 + GDC851 MP40 Stereo
    eq_mp1 = [(115, 1.4, 2.0), (2200, 1.5, 2.4), (5800, 1.4, 1.8)]
    for i in range(4):
        add("MP1_FIRE", f"GDC1001 MP5 Stereo + GDC851 MP40 Stereo + 1002 #{i+1}",
            align_mix([(pick(mp5_c, i), 0.72, 0), (pick(mp40_c, i * 2), 0.44, 0), (pick(mp5_d, i), 0.38, 2)], 580),
            "1001+851+1002", eq_bands=eq_mp1, comp_ratio=2.2, width_pct=96)

    # P90 (FN P90 5.7x28mm): GDC1043 P90 + GDC1041 Five-seveN + GDC1003 MP7
    eq_p90 = [(145, 1.4, 1.6), (3400, 1.5, 2.8), (8200, 1.3, 2.2)]
    for i in range(4):
        add("P90_FIRE", f"GDC1043 P90 + GDC1041 5.7 + GDC1003 MP7 + M1911 tail #{i+1}",
            align_mix([(pick(p90_c, i), 0.68, 0), (pick(fn57_c, i), 0.44, 0), (pick(mp7_c, i), 0.36, 1), (pick(t_smg, i)[:int(0.45*SR)], 0.25, 2)], 540),
            "1043+1041+1003", eq_bands=eq_p90, comp_ratio=2.2, width_pct=95)

    # AK1 (AK-47/AKM 7.62x39mm — Iconic BLACK Hero Weapon): صوتيات AK-74 4ch + FFX AK-47 Stereo + GDC1007 AKM + GDC3086
    eq_ak1 = [(85, 1.3, 3.8), (1650, 1.5, 2.4), (3600, 1.4, 2.2)]
    for i in range(4):
        add("AK1_FIRE", f"Sotiyat AK0405 4ch + FFX AK47 + GDC1007 AKM + 3086 #{i+1}",
            align_mix([(pick(akm_c, i), 0.62, 0), (pick(ak0405_d, i), 0.50, 0), (pick(ffx_ak47, i), 0.44, 0), (pick(ak47_bt, i), 0.38, 3), (pick(akm_d, i), 0.26, 6)], 780),
            "sotiyat:AK0405+ffx:C_25+1007+3086", eq_bands=eq_ak1, comp_ratio=2.1, width_pct=102)

    # ASR (G36C / Assault Rifle): صوتيات Modern_Firearms Assault Rifle Stereo + GDC1051 G36C + GDC1034 FAMAS
    eq_asr = [(110, 1.3, 2.6), (2600, 1.5, 2.6), (6200, 1.3, 1.8)]
    for i in range(4):
        add("ASR_FIRE", f"Sotiyat AssaultRifle + GDC1051 G36C + GDC1034 FAMAS #{i+1}",
            align_mix([(pick(mf_asr, i), 0.72, 0), (pick(g36_c, i), 0.48, 0), (pick(famas_c, i), 0.40, 1)], 690),
            "sotiyat:Gun_Assault_Rifle+1051+1034", eq_bands=eq_asr, comp_ratio=2.2, width_pct=100)

    # M16 (M16A2 / M4A1): GDC67 M4 Stereo + FFX AR-15 192k Stereo + GDC997 HK416
    eq_m16 = [(95, 1.3, 3.0), (2400, 1.5, 2.6), (6800, 1.3, 2.0)]
    for i in range(4):
        add("M16_FIRE", f"GDC67 M4 Stereo + FFX AR-15 Stereo + GDC997 HK416 #{i+1}",
            align_mix([(pick(m4_c, i), 0.70, 0), (pick(ffx_ar15, i), 0.46, 0), (pick(hk416_c, i), 0.42, 0)], 720),
            "67+ffx:D_31+997", eq_bands=eq_m16, comp_ratio=2.1, width_pct=100)

    # BNS (Elite Battle Rifle): GDC730 HK416 Room + GDC1026 Enfield L85 + GDC995 HK G3 7.62 + GDC6989 AK5 Armor
    eq_bns = [(82, 1.3, 3.4), (1950, 1.5, 2.4), (5400, 1.4, 2.0)]
    for i in range(4):
        add("BNS_FIRE", f"GDC730 Room416 + 1026 L85 + 995 G3 + 6989 #{i+1}",
            align_mix([(pick(hk416_rm, i), 0.62, 0), (pick(l85_c, i), 0.44, 0), (pick(g3_c, i), 0.40, 1), (pick(ak5_arm, i), 0.28, 3)], 760),
            "730+1026+995+6989", eq_bands=eq_bns, comp_ratio=2.1, width_pct=104)

    # BNS_M203 (Underbarrel 40mm): GDC5829 Smoke GRL Stereo + صوتيات Bluezone Tank Artillery
    eq_m203 = [(65, 1.2, 4.8), (420, 1.4, 2.2), (1600, 1.5, -1.5)]
    for i in range(3):
        add("BNS_M203", f"GDC5829 Smoke GRL + Sotiyat Bluezone Artillery #{i+1}",
            align_mix([(pick(grl_smoke, i), 0.80, 0), (pick(tank_can, i), 0.48, 0)], 760),
            "5829+4441", eq_bands=eq_m203, transient_boost_db=2.0, width_pct=106)

    # SHG (12g Pump Shotgun): صوتيات Modern_Firearms Shotgun + Wav Junction + GDC729 + GDC1303 Stereo
    eq_shg = [(65, 1.2, 4.8), (950, 1.4, 2.2), (3200, 1.4, 2.0)]
    for i in range(4):
        add("SHG_FIRE", f"Sotiyat Shotgun + WavJunction + GDC729 R870 + GDC1303 #{i+1}",
            align_mix([(pick(mf_shg, i), 0.68, 0), (pick(r870_c, i), 0.56, 0), (pick(ts_fire, i), 0.42, 0), (pick(wj_shg, i), 0.30, 1)], 920),
            "sotiyat:Gun_Shotgun+WavJnc+729+1303", eq_bands=eq_shg, comp_ratio=2.1, width_pct=106)

    # RM1 (Combat Semi-Auto 12g): GDC6660 Saiga DESIGNED Stereo + 6859 Beretta 1201F + 6902 SPAS + 6903 Semi
    eq_rm1 = [(72, 1.3, 4.2), (1300, 1.5, 2.4), (4200, 1.4, 1.8)]
    for i in range(4):
        add("RM1_FIRE", f"GDC6660 Saiga Stereo + 6859 Beretta + 6902 SPAS + 6903 #{i+1}",
            align_mix([(pick(saiga_ds, i), 0.64, 0), (pick(ber1201_c, i), 0.46, 0), (pick(spas_c, i), 0.44, 0), (pick(spas_semi, i), 0.34, 1)], 860),
            "6660+6859+6902+6903", eq_bands=eq_rm1, comp_ratio=2.1, width_pct=106)

    # SNR (Heavy Sniper Rifle): صوتيات Sniper + FFX Tikka 192k Stereo + M98B .338 + Lynx .50 + SVD Stereo
    eq_snr = [(58, 1.2, 5.0), (1400, 1.4, 2.4), (3500, 1.4, 2.2)]
    for i in range(4):
        add("SNR_FIRE", f"Sotiyat Sniper + FFX Tikka + M98B .338 + Lynx .50 + SVD #{i+1}",
            align_mix([(pick(mf_snr, i), 0.56, 0), (pick(ffx_tikka, i), 0.48, 0), (pick(m98b_c, i), 0.54, 0), (pick(lynx50_c, i), 0.40, 2), (pick(svd_c, i), 0.30, 3)], 1040),
            "sotiyat:Gun_Sniper+ffx:W_27+1012+6909+6663", eq_bands=eq_snr, transient_boost_db=2.2, comp_ratio=2.0, width_pct=110)

    # HVY (M249 SAW / LMG): GDC1067 M1919A4 ORTF Stereo + صوتيات M1919 + L110A2 + L7A2 + MG4
    eq_hvy = [(82, 1.3, 3.6), (1800, 1.4, 2.6), (4800, 1.4, 2.0)]
    for i in range(5):
        add("HVY_FIRE", f"GDC1067 M1919A4 ORTF + Sotiyat M1919 + 1060 L110 + 1062 L7A2 #{i+1}",
            align_mix([(pick(m1919a4_c, i), 0.56, 0), (pick(m1919_d, i), 0.52, 0), (pick(l110_c, i), 0.48, 0), (pick(l7a2_c, i), 0.36, 1), (pick(mg4_c, i), 0.28, 0)], 660),
            "1067+sotiyat:M1919+1060+1062", eq_bands=eq_hvy, comp_ratio=2.2, width_pct=102)

    # RPG (RPG-7): GDC2894 Rocket Launcher Stereo + GDC6991 RPG + صوتيات Shell Trajectory
    eq_rpg = [(62, 1.2, 4.5), (420, 1.3, 2.0), (2600, 1.4, 1.5)]
    for i in range(3):
        add("RPG_FIRE", f"GDC2894 RocketLauncher + GDC6991 RPG + 4440 Trajectory #{i+1}",
            align_mix([(pick(rpg_ext, i), 0.64, 0), (pick(rpg_fire, i), 0.56, 0), (pick(shell_trj, i), 0.48, 6)], 1150),
            "2894+6991+4440", eq_bands=eq_rpg, width_pct=112)

    # GRL (Standalone M203/Grenade Launcher): GDC850 MK44 Cannon Stereo + GDC2882 GRL 2 Stereo
    eq_grl = [(65, 1.2, 4.6), (380, 1.4, 2.4), (1800, 1.5, -1.8)]
    for i in range(3):
        add("GRL_FIRE", f"GDC850 MK44 Cannon Stereo + GDC2882 GRL2 Stereo #{i+1}",
            align_mix([(pick(mk44_c, i), 0.72, 0), (pick(grl2_c, i), 0.58, 0)], 840),
            "850+2882", eq_bands=eq_grl, width_pct=108)

    # =========================================================================
    # 2. SUPPRESSED WEAPONS (17..26 — ZERO clipped sources, True Stereo!)
    # =========================================================================
    eq_sup = [(160, 1.3, 1.5), (3800, 1.4, 1.8), (650, 1.5, -1.8)]
    supp_map = [
        ("PST_SUPP", mf_sup,     supp_pst,   t_ind_6964, "sotiyat:Silenced+2535+6964 P38 Silenced"),
        ("GK1_SUPP", ffx_sup22,  supp_pst,   ffx_ppq_h,  "ffx:J_2+2535+ffx:X Glock Silenced"),
        ("SMG_SUPP", supp_pst,   supp_hiss,  ppsh_ck,    "2535+sotiyat:SmokeHiss+5118 Uzi SD"),
        ("SM3_SUPP", supp_ppk,   supp_dl,    mp40_ck,    "2781+1021+5042 MAC10 SD"),
        ("MP1_SUPP", mf_sup,     ffx_cg_h,   supp_ppk,   "sotiyat:Silenced+ffx:G_2+2781 MP5-SD"),
        ("P90_SUPP", supp_dl,    supp_hiss,  ffx_sup22,  "1021+sotiyat:SmokeHiss+ffx:J_2 P90 SD"),
        ("AK1_SUPP", ffx_sup300, aksd_813,   t_ind_6962, "ffx:T_15+sotiyat:AKSD0813+6962 AK47 SD"),
        ("ASR_SUPP", supp_dl,    ffx_ar_h,   supp_snp,   "1021+ffx:D_13+2536 G36C SD"),
        ("M16_SUPP", supp_trg,   t_ind_2779, m4_chrg,    "1088+2779+69 M4A1/M16 SD"),
        ("SNR_SUPP", supp_snp,   ffx_sup300, mnsd_315,   "2536+ffx:T_15+sotiyat:MNSD0315 Sniper SD"),
    ]
    for slot_name, pri, sec, air, desc in supp_map:
        for i in range(3):
            layers = [(pick(pri, i), 0.72, 0), (pick(sec, i), 0.42, 0), (pick(air, i)[:int(0.26*SR)], 0.22, 0)]
            add(slot_name, f"{desc} #{i+1}",
                align_mix(layers, 460),
                desc.split()[0], eq_bands=eq_sup, transient_boost_db=1.0, comp_ratio=2.0, width_pct=96)

    # =========================================================================
    # 3. DEDICATED TRUE-STEREO ROLLING GUNSHOT TAILS ("MATTAK"), ECHOES, DISTANT & FLYBY
    # =========================================================================
    eq_tail = [(140, 1.2, 2.0), (850, 1.2, 1.4), (5800, 1.2, -2.5)]
    eq_echo = [(260, 1.3, 1.8), (950, 1.3, 1.2), (5500, 1.2, -2.8)]
    eq_dist = [(220, 1.2, 1.5), (1100, 1.3, 1.0), (4200, 1.2, -3.5)]
    eq_fly  = [(2600, 1.4, 2.8), (6200, 1.3, 2.2)]

    for i in range(5):
        add("TAIL_PISTOL_OUTDOOR", f"GDC4935/6203/6881 Pistol Outdoor Roll #{i+1}",
            pick(t_m1911 + t_glk100, i), "4935+6203+6881", eq_bands=eq_tail, transient_boost_db=0.0, comp_ratio=1.8, fade_out_ms=140.0, width_pct=116)
    for i in range(5):
        add("TAIL_SMG_OUTDOOR", f"GDC3089/1056/1002 SMG Outdoor Tail #{i+1}",
            pick(t_smg, i), "3089+1056", eq_bands=eq_tail, transient_boost_db=0.0, comp_ratio=1.8, fade_out_ms=140.0, width_pct=116)
    for i in range(6):
        add("TAIL_RIFLE_MATTAK", f"GDC2783/3086/1008 Rifle Valley Mattak #{i+1}",
            pick(t_mattak, i), "2783+3086", eq_bands=eq_tail, transient_boost_db=0.0, comp_ratio=1.8, fade_out_ms=180.0, width_pct=120)
    for i in range(6):
        add("TAIL_HEAVY_MATTAK", f"GDC5034/1852/1063 Heavy Mountain Mattak #{i+1}",
            pick(t_heavy, i), "5034+1852", eq_bands=eq_tail, transient_boost_db=0.0, comp_ratio=1.8, fade_out_ms=200.0, width_pct=122)
    for i in range(5):
        add("TAIL_SHOTGUN_OUTDOOR", f"GDC6660/6902/1304 12g Outdoor Boom Tail #{i+1}",
            pick(t_shotgun, i), "6660+6902", eq_bands=eq_tail, transient_boost_db=0.0, comp_ratio=1.8, fade_out_ms=160.0, width_pct=118)
    for i in range(6):
        add("TAIL_INDOOR_ECHO", f"GDC2780/2779/6964/6965 Corridor Reflections #{i+1}",
            pick(t_indoor, i), "2780+2779", eq_bands=eq_echo, transient_boost_db=0.0, comp_ratio=1.8, fade_out_ms=150.0, width_pct=118)
    for i in range(3):
        add("TAIL_SUPPRESSED_CRACK", f"GDC2781/1021 Suppressed Decay #{i+1}",
            pick(t_supp, i), "2781+1021", eq_bands=eq_echo, transient_boost_db=0.0, comp_ratio=1.8, fade_out_ms=110.0, width_pct=112)
    for i in range(6):
        add("DISTANT_BATTLEFIELD", f"GDC1073/5041/1245/1851 Far-Field Gunfire #{i+1}",
            pick(dist_300, i), "1073+5041+1245", eq_bands=eq_dist, transient_boost_db=0.6, comp_ratio=2.0, fade_out_ms=120.0, width_pct=115)
    for i in range(8):
        seg = pick(crk_1853, i * 3 + 2) if (i % 2 == 0) else pick(flyby_whz, i)
        add("ENV_SUPERSONIC_CRACK", f"PM_BBI + GDC1853/4847 Supersonic Crack #{i+1}",
            seg, "PM_BBI+1853+4847", eq_bands=eq_fly, transient_boost_db=1.8, comp_ratio=2.0, fade_out_ms=40.0, width_pct=105)

    for i in range(4):
        add("AI_TAIL", f"GDC2783/1008 AI Rolling Tail #{i+1}", pick(t_mattak, i + 2), "2783+1008", eq_bands=eq_tail, transient_boost_db=0.0, fade_out_ms=150.0, width_pct=115)
    for i in range(4):
        add("ENV_TAIL", f"GDC2780/6964 Environment Reflection Tail #{i+1}", pick(t_indoor, i + 1), "2780+6964", eq_bands=eq_echo, transient_boost_db=0.0, fade_out_ms=140.0, width_pct=115)
    for i in range(5):
        add("ENV_FLYBY", f"PM_BBI + GDC4847/1853 Bullet Flyby #{i+1}", pick(flyby_whz + crk_1853, i), "PM_BBI+4847+1853", eq_bands=eq_fly, transient_boost_db=1.5, width_pct=105)

    # =========================================================================
    # 4. ENEMY AI & DISTANT GUNFIRE SLOTS (74..83 except 79 — Mono 3D point sources!)
    # =========================================================================
    p229_d  = T(1090, 750, 45.0)
    sako_c  = T(1087, 850, 35.0)
    ai_cfg = [
        ("AI_PISTOL",      m1911_c + glock_c + p229_d,     "727+6202+1090", eq_gk1, 3),
        ("AI_SMG_PULSE",   mp5_d + ppsh_fire + mac10_d,    "1002+PPSh+1056", eq_smg, 4),
        ("AI_SMG_START",   mp5_c + muzi_d,                 "1001+1075",      eq_smg, 3),
        ("AI_RIFLE_PULSE", mg42_c + akm_d + r700_d + l110_c, "MG42+1008+1084", eq_ak1, 4),
        ("AI_RIFLE_START", akm_c + hk416_c,                "1007+997",       eq_ak1, 3),
        ("AI_SHOTGUN",     moss_cor + spas_c + ts_slug,    "6964+6902+1304", eq_rm1, 3),
        ("AI_SNIPER",      k98_fire + r700_d + svd_c + sako_c, "sotiyat:K98+1084+6663", eq_snr, 3),
        ("AI_RPG",         rpg_fire + shell_trj,           "6991+4440",      eq_rpg, 3),
        ("AI_SUPP",        supp_dl + supp_ppk + ffx_sup22, "1021+2781+J_2",  eq_sup, 3),
    ]
    for slot_name, pool, fids, eqb, cnt in ai_cfg:
        for i in range(cnt):
            add(slot_name, f"{fids} Distant AI #{i+1}", pick(pool, i), fids, eq_bands=eqb, comp_ratio=2.2, force_mono=True)

    # =========================================================================
    # 5. ALL MECHANICALS, RELOADS & CASINGS (27..73 + 96 — صوتيات + FFX 192k + GDC)
    # =========================================================================
    k98_h = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/Pole Position - Mauser Karabiner 98 kurz K98k bolt-action rifle/K98k, Handling, Various, t2, 1m, Right, MKH8060.wav"), 600, 50.0, gap=300.0)
    m38_h = T(S("Sonniss.com - GDC 2020 - Game Audio Bundle/Pole Position - Mosin-Nagant M38 bolt-action rifle/M38, Handling, Various, t2, 1m, Above, Right, MKH8060.wav"), 600, 50.0, gap=300.0)
    eq_f = [(220, 1.3, 1.0), (3400, 1.4, 2.2), (7600, 1.3, 1.8)]
    foley_defs = {
        "PST_UNLD": (p38_mag + mf_mage, "5149+sotiyat"), "PST_LD": (deag_mag + mf_magi, "6883+sotiyat"), "PST_COCK": (ffx_19_h + tmp_f, "ffx:A+1116"),
        "GK1_UNLD": (deag_mag + ffx_ppq_h, "6883+ffx:X"), "GK1_LD": (mf_magi + p38_mag, "sotiyat+5149"), "GK1_COCK": (semi_ck + mf_bolt, "1624+sotiyat"),
        "MGN_UNLD": (rev_cyl, "1115+ffx:V"), "MGN_LD": (rev_ld, "6877"),
        "SMG_UNLD": (mf_mage + p38_mag, "sotiyat+5149"), "SMG_LD": (deag_mag + mf_magi, "6883+sotiyat"), "SMG_COCK": (ppsh_ck + mf_bolt, "5118+sotiyat"),
        "MP1_UNLD": (g36_mag + ffx_cg_h, "1113+ffx:G"), "MP1_LD": (g36_mag[1:] + ffx_cg_h, "1113+ffx:G"), "MP1_COCK": (mp40_ck + tt33_clr, "5042+TT33"),
        "P90_UNLD": (m4_unld, "70"), "P90_LD": (g36_mag, "1113"), "P90_COCK": (tmp_f[1:] + mf_bolt, "1116"),
        "AK1_UNLD": (ffx_ak_h + svd_mag, "ffx:C+6662"), "AK1_LD": (svd_mag[2:] + ak74_asmb, "6662+AK0709"), "AK1_COCK": (mf_bolt + k98_hdl, "sotiyat+5024"),
        "ASR_UNLD": (g36_mag + mf_mage, "1113+sotiyat"), "ASR_LD": (mf_magi + m4_chrg, "sotiyat+69"), "ASR_COCK": (ffx_ar_h + m4_chrg[1:], "ffx:D+69"),
        "M16_UNLD": (m4_unld + ffx_ar_h, "70+ffx:D"), "M16_LD": (m4_chrg + mf_magi, "69"), "M16_COCK": (m4_chrg[2:] + mf_bolt, "69+sotiyat"),
        "SHG_COCK": (wj_pump + ts_pump + spas_pmp, "WavJnc+1305"), "RM1_LD": (ffx_m12_h + ffx_nov_h + ts_load + sg_load, "ffx:K/O+1302"), "RM1_COCK": (spas_pmp + wj_pump + ts_pump, "6904+WavJnc"),
        "SNR_UNLD": (svd_mag[1:] + ffx_tik_h + m38_h, "6662+ffx:W+M38"), "SNR_LD": (m1903_ck + ffx_tik_h + k98_h, "5095+ffx:W+K98"), "SNR_COCK": (m1903_ck + lynx_ej, "5095+6910"),
        "HVY_UNLD": (mg42_cvr + m38_h, "5035+M38"), "HVY_LD": (sh_hand + m4_chrg, "5826+69"), "HVY_COCK": (mg42_cvr + ppsh_ck, "5035+5118"),
        "RPG_LD": (lynx_ej, "6910"), "RPG_ATTACH": (m1903_ck, "5095"),
        "GRL_OPEN": (rev_cyl, "1115"), "GRL_CASE": (lynx_ej, "6910"), "GRL_COCK": (ts_pump + wj_pump, "1305"),
        "FIRE_SELECT": (wj_safe + tmp_f, "WavJnc+1116"), "SILENCER_ON": (sil_scrw, "2537"), "SILENCER_OFF": (sil_scrw[1:], "2537"),
        "SILENCER_CASE": (cas_9mm, "5195"),
        "DEAD_CLICK": (dry_m1911 + ffx_19_h, "4936+ffx:A"), "CHNG_WPN": (m4_chrg + ak74_asmb + k98_h, "69+AK0709+K98"),
        "CASING_BRASS": (cas_ak + cas_9mm + cas_50, "SculpTunes+5195+3088"),
        "CASING_SHELL": (cas_shg + cas_wd, "1273+2534"),
    }
    for slot_name, (pool, fid_tag) in foley_defs.items():
        n_var = 4 if "CASING_" in slot_name else (3 if any(k in slot_name for k in ("_COCK", "_LD", "_UNLD", "CHNG_WPN", "SILENCER_ON", "SILENCER_OFF", "FIRE_SELECT", "DEAD_CLICK")) else 2)
        for i in range(n_var):
            add(slot_name, f"{fid_tag} {slot_name} #{i+1}", pick(pool, i), fid_tag, eq_bands=eq_f, transient_boost_db=1.2, width_pct=96)

    # =========================================================================
    # 6. IMPACTS & EXPLOSIONS (86..94 except 91)
    # =========================================================================
    for i in range(3):
        add("ENV_RPG_PASS", f"GDC4440 Shell Trajectory #{i+1}", pick(rpg_pass, i), "4440", eq_bands=[(180, 1.3, 2.0), (1600, 1.4, 1.5)], fade_out_ms=65.0, width_pct=112)
    for i in range(3):
        add("IMP_CONCRETE", f"GDC1608 Concrete Impact #{i+1}", pick(imp_conc + imp_dirt, i), "1608", eq_bands=[(180, 1.4, 2.0), (3200, 1.4, 2.2)], width_pct=102)
        add("IMP_METAL", f"GDC1609/6989 Metal Impact #{i+1}", pick(imp_met + ak5_arm, i), "1609+6989", eq_bands=[(900, 1.5, 2.2), (4800, 1.4, 2.6)], width_pct=104)
        add("IMP_WOOD", f"GDC2534/4844 Wood Impact #{i+1}", pick(cas_wd + imp_dirt, i), "2534", eq_bands=[(140, 1.3, 2.2), (2100, 1.4, 1.8)], width_pct=100)
        add("IMP_GLASS", f"PM_BBI + GDC4845 Glass Shatter #{i+1}", pick(imp_glss, i), "PM_BBI+4845", eq_bands=[(4200, 1.4, 2.8), (8500, 1.3, 2.0)], width_pct=108)
        add("IMP_RICOCHET", f"GDC1853/4847 Ricochet Whizz #{i+1}", pick(crk_1853 + flyby_whz, i * 4), "1853+PM_BBI", eq_bands=eq_fly, width_pct=106)
        add("IMP_DIRT", f"PM_BBI + GDC4844 Dirt Impact #{i+1}", pick(imp_dirt + imp_conc, i), "PM_BBI+4844", eq_bands=[(110, 1.3, 2.5), (1800, 1.4, 1.2)], width_pct=102)
        add("IMP_FLESH", f"GDC1607/4846 Body Thump #{i+1}", pick(imp_flesh, i), "1607+PM_BBI", eq_bands=[(100, 1.4, 3.0), (1200, 1.5, -1.5)], width_pct=96)
    eq_exp = [(55, 1.2, 5.2), (140, 1.3, 2.5), (1600, 1.4, 1.6)]
    for i in range(6):
        add("EXPLOSION", f"Sotiyat Frag + GDC1611/4439/5828/5515 Explosion #{i+1}",
            align_mix([(pick(exp_lg, i), 0.85, 0), (pick(exp_deep, i), 0.55, 5)], 1850),
            "sotiyat:Frag+1611+5515", eq_bands=eq_exp, transient_boost_db=1.8, width_pct=120)

    # =========================================================================
    # 7. EXPANDED WORLD, FOLEY, PLAYER, FLAME & HUD SLOTS (BWAR v3: 106..128)
    # =========================================================================
    eq_fs = [(95, 1.3, 1.8), (2400, 1.4, 1.2)]
    for i in range(4):
        add("FS_CONCRETE_WALK", f"GDC930/931 Concrete Walk #{i+1}", pick(fs_conc_w, i), "930+931", eq_bands=eq_fs, transient_boost_db=1.0, fade_out_ms=35.0, width_pct=90)
        add("FS_CONCRETE_RUN",  f"GDC2093 Asphalt Run #{i+1}",     pick(fs_conc_r, i), "2093",    eq_bands=eq_fs, transient_boost_db=1.2, fade_out_ms=35.0, width_pct=92)
    for i in range(3):
        add("FS_GRASS_WALK",   f"GDC4888/4889/2094 Grass Step #{i+1}", pick(fs_grass, i), "4888+4889+2094", eq_bands=[(120, 1.3, 1.0), (5200, 1.3, 1.8)], transient_boost_db=0.6, fade_out_ms=40.0, width_pct=95)
        add("FS_DIRT_GRAVEL",  f"GDC3704/3705 Dirt Gravel #{i+1}",     pick(fs_dirt, i),  "3704+3705", eq_bands=[(140, 1.3, 1.5), (3600, 1.4, 1.5)], transient_boost_db=0.8, fade_out_ms=40.0, width_pct=94)
        add("FS_METAL",        f"GDC932/933/2095 Metal Step #{i+1}",   pick(fs_metal, i), "932+933+2095", eq_bands=[(110, 1.3, 1.8), (2800, 1.4, 2.0)], transient_boost_db=1.2, fade_out_ms=45.0, width_pct=96)
        add("FS_WATER_PUDDLE", f"GDC5646/33 Puddle Step #{i+1}",       pick(fs_water, i), "5646+33", eq_bands=[(220, 1.3, 1.5), (4200, 1.3, 1.8)], transient_boost_db=0.8, fade_out_ms=60.0, width_pct=106)
    for i in range(3):
        add("LAND_IMPACT",     f"GDC3877/2510 Jump Land #{i+1}", pick(land_imp, i), "3877+2510", eq_bands=[(85, 1.3, 3.2), (1800, 1.4, 1.2)], transient_boost_db=1.2, width_pct=102)
        add("BODYFALL",        f"GDC3878/3879/3877 Bodyfall #{i+1}", pick(bodyfalls, i), "3878+3879", eq_bands=[(80, 1.3, 3.5), (1400, 1.4, 1.0)], transient_boost_db=1.0, width_pct=105)
        add("MELEE_SWING",     f"GDC4558/4560/5891 Melee Swing #{i+1}", pick(melee_sw, i), "4558+4560+5891", eq_bands=[(180, 1.3, 1.8), (2200, 1.4, 1.6)], transient_boost_db=1.0, width_pct=110)
        add("MELEE_HIT",       f"Sotiyat Knife + GDC1631/5892 Melee Hit #{i+1}", pick(melee_ht, i), "sotiyat:Knife+1631+5892", eq_bands=[(95, 1.3, 3.2), (2400, 1.4, 2.0)], transient_boost_db=1.6, width_pct=104)
        add("WATER_SPLASH",    f"GDC4430/4432 Water Splash #{i+1}", pick(water_sp, i), "4430+4432", eq_bands=[(180, 1.2, 2.0), (3800, 1.3, 1.6)], transient_boost_db=0.8, fade_out_ms=110.0, width_pct=115)
    for i in range(3):
        add("FIRE_LOOP",       f"GDC1493 Gas Burner Seamless Stereo Loop #{i+1}",
            align_mix([(pick(gas_loops, i), 0.85, 0), (pick(flamer_c, i), 0.22, 0)], len(pick(gas_loops, i)) * 1000.0 / SR),
            "1493+2893", eq_bands=[(75, 1.2, 3.4), (520, 1.3, 1.8), (4200, 1.3, 1.5)],
            transient_boost_db=0.0, comp_ratio=1.6, ceil_db=-3.0, is_loop=True, width_pct=114)
        add("FLAME_IGNITE",    f"GDC2893 Firestorm Flamer + GDC4679 Ignite #{i+1}",
            align_mix([(pick(flamer_c, i + 1), 0.85, 0), (pick(match_ig, i), 0.42, 0)], 720),
            "2893+4679", eq_bands=[(72, 1.2, 4.0), (650, 1.3, 2.0), (4800, 1.3, 1.8)],
            transient_boost_db=1.6, comp_ratio=2.0, width_pct=112)
    for i in range(3):
        add("PICKUP_AMMO",     f"GDC2510/2522/972 Ammo Pickup #{i+1}", pick(pck_ammo, i), "2510+2522+972", eq_bands=eq_f, transient_boost_db=1.2, width_pct=96)
        add("PICKUP_HEALTH",   f"GDC250/2522 Medkit Pickup #{i+1}",    pick(pck_hlth, i), "250+2522",      eq_bands=eq_f, transient_boost_db=1.0, width_pct=96)
        add("SNIPER_BREATH",   f"GDC1152/1707 Sniper Focus Breath #{i+1}", pick(snpr_brth, i), "1152+1707", eq_bands=[(180, 1.2, 2.2), (1600, 1.3, 1.2)], transient_boost_db=0.0, fade_out_ms=95.0, width_pct=92)
    for i in range(3):
        add("HEARTBEAT_LOW_HLTH", f"GDC1151 Low-Health Heartbeat #{i+1}", pick(heart_bt, i), "1151", eq_bands=[(55, 1.2, 4.5), (140, 1.3, 2.0)], transient_boost_db=0.8, ceil_db=-2.0, fade_out_ms=90.0, width_pct=90)
    for i in range(3):
        add("DEBRIS_RUBBLE",   f"Sotiyat Debris + GDC5815/5816 Rubble #{i+1}", pick(debris_rb, i), "sotiyat:Bluezone+5815+5816", eq_bands=[(110, 1.3, 2.6), (2600, 1.4, 2.0)], transient_boost_db=1.2, fade_out_ms=100.0, width_pct=116)
        add("CAR_WINDSHIELD",  f"GDC36/6631 Car Windshield Shatter #{i+1}", pick(car_wind, i), "36+6631", eq_bands=[(180, 1.3, 1.6), (4200, 1.4, 2.8)], transient_boost_db=1.4, fade_out_ms=95.0, width_pct=114)
        add("WEAPON_DROP",     f"Sotiyat aXLsound + GDC2510 Weapon Drop #{i+1}", pick(wpn_drop, i), "sotiyat:aXLsound+2510", eq_bands=eq_f, transient_boost_db=1.2, width_pct=98)
        add("UI_HUD_CONFIRM",  f"GDC484/2331/2140 HUD Confirm #{i+1}", pick(ui_conf, i), "484+2331+2140", eq_bands=[(1400, 1.3, 1.6), (5200, 1.3, 1.2)], transient_boost_db=0.4, ceil_db=-3.0, width_pct=100)
        add("UI_HUD_HINT",     f"GDC2139/2141/2332 HUD Hint #{i+1}",  pick(ui_hint, i), "2139+2141+2332", eq_bands=[(1200, 1.3, 1.4), (4800, 1.3, 1.0)], transient_boost_db=0.4, ceil_db=-3.5, width_pct=100)
    for i in range(3):
        is_lp = (i < 2)
        add("UI_HUD_ALERT",    f"GDC110/1911 Alert Siren/Deny #{i+1}", pick(ui_alert, i), "110+1911",
            eq_bands=[(950, 1.3, 1.8), (3200, 1.3, 1.2)], transient_boost_db=0.2, ceil_db=-2.8, is_loop=is_lp, width_pct=106)

    return clips


def classify_iso_signature(src, name):
    """Canonical ISO signature -> BWAR v3 (slot_name, is_fp_fire, is_world_loop) mapping."""
    sl = src.lower()
    nl = name.lower()

    if sl.endswith("anim.awd"):
        if nl.startswith("chngwpn"): return ("CHNG_WPN", 0, 0)
        if nl.startswith("silencer_on"): return ("SILENCER_ON", 0, 0)
        if nl.startswith("silencer_of"): return ("SILENCER_OFF", 0, 0)
        if nl.startswith(("bodyfall", "fall1", "fall2", "flip1", "recover1", "stumble")): return ("BODYFALL", 0, 0)
        if nl.startswith(("melee", "twist1")): return ("MELEE_SWING", 0, 0)
        if nl.startswith("hit"): return ("MELEE_HIT", 0, 0)
        if nl.startswith("grab"): return ("PICKUP_AMMO", 0, 0)
        if nl.startswith("run"): return ("FS_CONCRETE_RUN", 0, 0)
        if nl.startswith("walk"): return ("FS_CONCRETE_WALK", 0, 0)
        return None

    if "/fpguns/" in sl or "\\fpguns\\" in sl or (sl.startswith("levels/") and sl.endswith(".db")):
        fp_map = {
            "bg1_pstlf": "PST_FIRE", "bg1_ps5lf": "PST_FIRE", "bg1_pstslf": "PST_SUPP",
            "bg1_gk1lf": "GK1_FIRE", "bg1_gk5lf": "GK1_FIRE", "bg1_gk1slf": "GK1_SUPP",
            "bg1_mgnlf": "MGN_FIRE", "bg1_mg5lf": "MGN_FIRE",
            "bg1_smglf": "SMG_FIRE", "bg1_sm5lf": "SMG_FIRE", "bg1_smgslf": "SMG_SUPP",
            "bg1_sm3lf": "SM3_FIRE", "bg1_mc5lf": "SM3_FIRE", "bg1_sm3slf": "SM3_SUPP",
            "bg1_mp1lf": "MP1_FIRE", "bg1_mp5lf": "MP1_FIRE", "bg1_mp1slf": "MP1_SUPP",
            "bg1_p90lf": "P90_FIRE", "bg1_p95lf": "P90_FIRE", "bg1_p90slf": "P90_SUPP",
            "bg1_ak1lf": "AK1_FIRE", "bg1_ak5lf": "AK1_FIRE", "bg1_ak1slf": "AK1_SUPP",
            "bg1_asrlf": "ASR_FIRE", "bg1_as5lf": "ASR_FIRE", "bg1_asrslf": "ASR_SUPP",
            "bg1_m16lf": "M16_FIRE", "bg1_m45lf": "M16_FIRE", "bg1_m16slf": "M16_SUPP",
            "bg1_bnslf": "BNS_FIRE",
            "bg1_shglf": "SHG_FIRE", "bg1_sh5lf": "SHG_FIRE",
            "bg1_rm1lf": "RM1_FIRE", "bg1_rm5lf": "RM1_FIRE",
            "bg1_snrlf": "SNR_FIRE", "bg1_sn5lf": "SNR_FIRE",
            "bg1_hvylf": "HVY_FIRE", "bg1_hv5lf": "HVY_FIRE",
            "bg1_rpglf": "RPG_FIRE", "bg1_grllf": "GRL_FIRE",
        }
        if nl in fp_map:
            return (fp_map[nl], 1, 0)
        if nl == "pstfire": return ("PST_FIRE", 1, 0)
        if nl == "assrflfire": return ("ASR_FIRE", 1, 0)
        if nl == "fireselect": return ("FIRE_SELECT", 0, 0)
        if nl.startswith("silencer_on"): return ("SILENCER_ON", 0, 0)
        if nl == "pstunld0": return ("PST_UNLD", 0, 0)
        if nl.startswith("pstld"): return ("PST_LD", 0, 0)
        if nl.startswith(("pstcock", "pstbrch", "pstbbrch")): return ("PST_COCK", 0, 0)
        if nl == "gkunld0": return ("GK1_UNLD", 0, 0)
        if nl == "gkld0": return ("GK1_LD", 0, 0)
        if nl == "gkcock0": return ("GK1_COCK", 0, 0)
        if nl == "mgnunld0": return ("MGN_UNLD", 0, 0)
        if nl.startswith("mgnld"): return ("MGN_LD", 0, 0)
        if nl == "sbmgn-unld0": return ("SMG_UNLD", 0, 0)
        if nl == "sbmgn-ld0": return ("SMG_LD", 0, 0)
        if nl == "sbmgn-cock": return ("SMG_COCK", 0, 0)
        if nl.startswith("sbmgnpin"): return ("CASING_BRASS", 0, 0)
        if nl == "mp1unld0": return ("MP1_UNLD", 0, 0)
        if nl == "mp1ld0": return ("MP1_LD", 0, 0)
        if nl == "mp1cock0": return ("MP1_COCK", 0, 0)
        if nl == "p90unld0": return ("P90_UNLD", 0, 0)
        if nl.startswith("p90ld"): return ("P90_LD", 0, 0)
        if nl == "p90cock0": return ("P90_COCK", 0, 0)
        if nl == "akunld0": return ("AK1_UNLD", 0, 0)
        if nl.startswith("akld"): return ("AK1_LD", 0, 0)
        if nl.startswith("akcock"): return ("AK1_COCK", 0, 0)
        if nl == "assrfl-unld0": return ("ASR_UNLD", 0, 0)
        if nl == "assrfl-ld0": return ("ASR_LD", 0, 0)
        if nl == "assrfl-cock": return ("ASR_COCK", 0, 0)
        if nl == "m16unld0": return ("M16_UNLD", 0, 0)
        if nl.startswith("m16ld"): return ("M16_LD", 0, 0)
        if nl == "m16cock0": return ("M16_COCK", 0, 0)
        if nl == "shtg01-cock0": return ("SHG_COCK", 0, 0)
        if nl == "shtgrm-ld0": return ("RM1_LD", 0, 0)
        if nl.startswith("shtgrm-cock"): return ("RM1_COCK", 0, 0)
        if nl == "snprfl-unld0": return ("SNR_UNLD", 0, 0)
        if nl == "snprfl-ld0": return ("SNR_LD", 0, 0)
        if nl in ("snprfl-cock0", "snprflrcap"): return ("SNR_COCK", 0, 0)
        if nl.startswith("hvmgnuld"): return ("HVY_UNLD", 0, 0)
        if nl == "hvmgnld0": return ("HVY_LD", 0, 0)
        if nl == "hvmgnck1": return ("HVY_COCK", 0, 0)
        if nl == "rpg-ld0": return ("RPG_LD", 0, 0)
        if nl == "rpg-attach0": return ("RPG_ATTACH", 0, 0)
        if nl == "grdlch-cock": return ("GRL_COCK", 0, 0)
        if nl == "grdlch-case0": return ("GRL_CASE", 0, 0)
        if nl.startswith(("grdlch-c0", "grdlch-st0")): return ("GRL_OPEN", 0, 0)
        return None

    if sl.endswith("aiwpns.awd"):
        if "pstl" in nl: return ("AI_PISTOL", 0, 0)
        if "shtg" in nl: return ("AI_SHOTGUN", 0, 0)
        if "snpr" in nl or "mgndst" in nl: return ("AI_SNIPER", 0, 0)
        if "rpg" in nl: return ("AI_RPG", 0, 0)
        if "silenced" in nl: return ("AI_SUPP", 0, 0)
        if "distant" in nl: return ("DISTANT_BATTLEFIELD", 0, 0)
        if nl.endswith("e0"): return ("AI_TAIL", 0, 0)
        if nl.endswith("s0"):
            return ("AI_SMG_START" if ("sbmchgn" in nl or "uzi" in nl or "mac10" in nl) else "AI_RIFLE_START", 0, 0)
        if "sbmchgn" in nl or "uzi" in nl or "mac10" in nl:
            return ("AI_SMG_PULSE", 0, 0)
        return ("AI_RIFLE_PULSE", 0, 0)

    if sl.endswith("level.awd"):
        if nl == "deadclick": return ("DEAD_CLICK", 0, 0)
        if nl.startswith("bulletby"): return ("ENV_FLYBY", 0, 0)
        if nl.startswith("rpgpass"): return ("ENV_RPG_PASS", 0, 0)
        if nl.startswith("tail"): return ("ENV_TAIL", 0, 0)
        if nl in ("expbass", "explowq0"): return ("EXPLOSION", 0, 0)
        if nl == "fireloop": return ("FIRE_LOOP", 0, 1)
        if nl == "rolling": return ("DEBRIS_RUBBLE", 0, 0)
        if nl in ("pckupam0", "pckup0", "sil_pkp"): return ("PICKUP_AMMO", 0, 0)
        if nl in ("pckuphlth0", "hlth1"): return ("PICKUP_HEALTH", 0, 0)
        if nl.startswith("snprin"): return ("SNIPER_BREATH", 0, 0)
        if nl.startswith("heart") or nl == "low_health": return ("HEARTBEAT_LOW_HLTH", 0, 0)
        if nl in ("hud_arm", "hud_blk", "hud_heal", "hud_rec", "hud_mis", "hud_obj", "objective"):
            return ("UI_HUD_CONFIRM", 0, 0)
        if nl in ("hud_hint", "hud_int"): return ("UI_HUD_HINT", 0, 0)
        if nl == "hud_dam": return ("UI_HUD_ALERT", 0, 0)
        return None

    if sl.endswith("emitter.awd"):
        if nl == "alarm": return ("UI_HUD_ALERT", 0, 1)
        return None

    if sl.endswith("collide.awd"):
        if nl.startswith("ricogen"): return ("IMP_RICOCHET", 0, 0)
        if nl.startswith("genshe"): return ("CASING_BRASS", 0, 0)
        if nl.startswith("crwndscr"): return ("CAR_WINDSHIELD", 0, 0)
        if nl.startswith("glass") or nl.startswith("shatter") or nl.startswith("light1"): return ("IMP_GLASS", 0, 0)
        if nl.startswith(("conc4", "con3", "con4")): return ("FS_CONCRETE_WALK", 0, 0)
        if nl.startswith("grass"): return ("FS_GRASS_WALK", 0, 0)
        if nl.startswith(("dirt3", "dirt4", "wood4")): return ("FS_DIRT_GRAVEL", 0, 0)
        if nl.startswith(("metal1", "metal3", "metal4")): return ("FS_METAL", 0, 0)
        if nl.startswith("water2"): return ("FS_WATER_PUDDLE", 0, 0)
        if nl.startswith("water__"): return ("WATER_SPLASH", 0, 0)
        if nl.startswith(("land", "dmetallnd", "sgnlnd")): return ("LAND_IMPACT", 0, 0)
        if nl.startswith("weapondrop"): return ("WEAPON_DROP", 0, 0)
        if nl.startswith(("pladeb", "ptclstn")): return ("DEBRIS_RUBBLE", 0, 0)
        if nl.startswith("plafal"): return ("BODYFALL", 0, 0)
        if nl.startswith("butt0"): return ("MELEE_HIT", 0, 0)
        if nl.startswith("ammo"): return ("PICKUP_AMMO", 0, 0)
        if nl.startswith("metal") or nl.startswith("dmetal"): return ("IMP_METAL", 0, 0)
        if nl.startswith("wood") or nl.startswith("dwood") or nl.startswith("wdplnkl") or nl.startswith("ptclwod"): return ("IMP_WOOD", 0, 0)
        if nl.startswith("con") or nl.startswith("wall2") or nl.startswith("tile") or nl.startswith("ceramic") or nl.startswith("ptclmsn") or nl.startswith("impactdefa"): return ("IMP_CONCRETE", 0, 0)
        if nl.startswith(("dirt", "ptclgrv", "sandexplod", "enbfl")): return ("IMP_DIRT", 0, 0)
        if nl.startswith(("flesh", "plahit", "plrimpact")): return ("IMP_FLESH", 0, 0)
        if nl.startswith(("explode", "clayexplod")) or nl == "grenade___11": return ("EXPLOSION", 0, 0)
        if nl.startswith("grenadelan"): return ("GRL_FIRE", 0, 0)
        return None
    return None


def main():
    clips = build_all_clips()
    fps_rows = json.load(open(os.path.join(SCRATCH, "iso_wave_fingerprints.json"), encoding="utf-8"))

    sigs = {}
    for entry in fps_rows:
        res = classify_iso_signature(entry["src"], entry["name"])
        if not res:
            continue
        slot_name, is_fp_fire, is_world_loop = res
        h = int(entry["fnv64"], 16)
        sz = int(entry["sz"])
        key = (h, sz)
        stem = slot_stem(slot_name)
        flags = (is_fp_fire & 1) | ((is_world_loop & 1) << 1) | ((stem & 7) << 2)
        if key not in sigs:
            sigs[key] = (SLOTS[slot_name], flags, entry["name"])

    os.makedirs(os.path.dirname(OUT_PAK), exist_ok=True)
    pcm_chunks = []
    clip_recs = []
    cur_pcm_offset = 0  # Offset in int16 samples!
    total_frames = 0
    stereo_clips = 0
    mono_clips = 0
    manifest_clips = []

    for slot_name, desc, arr, ch, width_pct, st_raw, st_mast, src_fid in clips:
        sid = SLOTS[slot_name]
        if ch == 2 and arr.ndim == 2:
            frames = len(arr)
            flat = arr.reshape(-1)
            stereo_clips += 1
        else:
            arr_1d = arr[:, 0] if arr.ndim == 2 else arr
            frames = len(arr_1d)
            flat = arr_1d
            ch = 1
            mono_clips += 1

        pcm16 = np.round(np.clip(flat, -1.0, 1.0) * 32767.0).astype("<i2")
        assert len(pcm16) == frames * ch
        clip_recs.append((sid, cur_pcm_offset, frames, ch, width_pct))
        pcm_chunks.append(pcm16.tobytes())
        cur_pcm_offset += len(pcm16)
        total_frames += frames
        manifest_clips.append({
            "slot": slot_name, "slot_id": sid, "stem": slot_stem(slot_name),
            "channels": ch, "width_pct": width_pct, "frames": frames,
            "src_fid": src_fid, "desc": desc,
            "raw": st_raw, "mastered": st_mast,
        })

    # Verify all 129 slots are populated
    present_sids = {r[0] for r in clip_recs}
    missing_sids = sorted(set(SLOTS.values()) - present_sids)
    if missing_sids:
        raise RuntimeError(f"Unpopulated BWAR slots: {missing_sids}")

    sig_list = sorted([(h, sz, sid, flg, nm) for (h, sz), (sid, flg, nm) in sigs.items()])

    with open(OUT_PAK, "wb") as f:
        f.write(struct.pack("<4s5I", b"BWAR", PACK_VERSION, SR, len(SLOTS), len(clip_recs), len(sig_list)))
        for sid, off, frames, ch, width_pct in clip_recs:
            f.write(struct.pack("<3I2H", sid, off, frames, ch, width_pct))
        for h, sz, sid, flg, _ in sig_list:
            f.write(struct.pack("<QI2H", h, sz, sid, flg))
        for ch_bytes in pcm_chunks:
            f.write(ch_bytes)

    pak_bytes = os.path.getsize(OUT_PAK)
    total_clipped = sum(c["mastered"]["clipped"] for c in manifest_clips)
    manifest = {
        "pak_path": OUT_PAK,
        "pak_version": PACK_VERSION,
        "pak_bytes": pak_bytes,
        "sample_rate": SR,
        "slot_count": len(SLOTS),
        "clip_count": len(clip_recs),
        "stereo_clips": stereo_clips,
        "mono_clips": mono_clips,
        "signature_count": len(sig_list),
        "total_duration_sec": round(total_frames / SR, 2),
        "total_clipped_samples": total_clipped,
        "clips": manifest_clips,
        "slots": SLOTS,
        "signatures_mapped": [
            {"slot_id": sid, "flags": flg, "name": nm, "size": sz, "fnv64": hex(h)}
            for h, sz, sid, flg, nm in sig_list
        ],
    }
    with open(OUT_MANIFEST, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    neg_corr = [c["slot"] for c in manifest_clips if c["mastered"]["corr"] < 0.0]
    prim_rms = [c["mastered"]["rms_db"] for c in manifest_clips if c["slot"].endswith("_FIRE") or c["slot"] in ("EXPLOSION", "BNS_M203")]
    print(
        f"BUILT BWAR v{PACK_VERSION} {OUT_PAK}: {pak_bytes / (1024*1024):.2f} MB | "
        f"{len(clip_recs)} clips ({stereo_clips} stereo, {mono_clips} mono, {total_frames / SR:.2f}s total) | "
        f"{len(sig_list)} ISO sigs | {len(SLOTS)} slots | total_clipped={total_clipped} | "
        f"primary_rms_db=[{min(prim_rms):.2f}, {max(prim_rms):.2f}] | Negative corr (<0.0): {neg_corr}"
    )


if __name__ == "__main__":
    main()
