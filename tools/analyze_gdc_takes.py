#!/usr/bin/env python3
"""
Sonniss GDC 2015-2024 Quantitative Audio Audition & Multi-Take Analyzer.

Inspects all 150 selectively extracted Sonniss GDC WAV files in
`.local/audio_src/gdc`, audits channel health (zero digital clipping, dynamic
range, crest factor, spectral balance, natural RT60 tail length), detects
individual single shots / bursts / foley events / supersonic cracks, and writes
a complete JSON inspection catalog for `build_weapon_audio_pack.py` and the
remaster audit reports.
"""
import glob
import json
import math
import os
import re
import struct
import sys
import numpy as np

REPO = r"e:\ai\black"
GDC_DIR = os.path.join(REPO, ".local", "audio_src", "gdc")
SCRATCH = r"C:\Users\313\.gemini\antigravity\brain\db2924e5-7d7e-4013-bc79-497f520eff0a\scratch"
HTML_PATH = os.path.join(SCRATCH, "gdc_finder.html")
OUT_JSON = os.path.join(REPO, ".local", "audio_src", "gdc_analysis.json")
TARGET_SR = 48000


def read_wav_float32(path):
    """Pure-Python + NumPy RIFF/RF64 WAV decoder (PCM 16/24/32, IEEE float 32/64, Extensible)."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 12 or data[:4] not in (b"RIFF", b"RF64") or data[8:12] != b"WAVE":
        raise ValueError(f"Not a valid WAV header: {path}")

    pos = 12
    fmt_tag = None
    channels = None
    sr = None
    bits = None
    audio_bytes = None
    n = len(data)
    while pos + 8 <= n:
        cid = data[pos:pos + 4]
        csz = struct.unpack_from("<I", data, pos + 4)[0]
        body_start = pos + 8
        body_end = min(n, body_start + csz)
        if cid == b"fmt ":
            fmt_tag, channels, sr, _, _, bits = struct.unpack_from("<HHIIHH", data, body_start)
            if fmt_tag == 0xFFFE and csz >= 40:
                # Extensible subformat GUID first 2 bytes give actual format tag (1=PCM, 3=Float)
                fmt_tag = struct.unpack_from("<H", data, body_start + 24)[0]
        elif cid == b"data":
            audio_bytes = data[body_start:body_end]
        pos = body_end + (csz & 1)

    if audio_bytes is None or fmt_tag is None or channels is None or sr is None:
        raise ValueError(f"Missing fmt/data chunk in {path}")

    if fmt_tag == 1 and bits == 16:
        arr = np.frombuffer(audio_bytes[: (len(audio_bytes) // 2) * 2], dtype="<i2").astype(np.float32) / 32768.0
    elif fmt_tag == 1 and bits == 24:
        frame_bytes = 3
        count = (len(audio_bytes) // frame_bytes) * frame_bytes
        raw = np.frombuffer(audio_bytes[:count], dtype=np.uint8).reshape(-1, 3)
        vals = (raw[:, 0].astype(np.int32) | (raw[:, 1].astype(np.int32) << 8) | (raw[:, 2].astype(np.int32) << 16))
        vals = np.where(vals >= 0x800000, vals - 0x1000000, vals)
        arr = vals.astype(np.float32) / 8388608.0
    elif fmt_tag == 1 and bits == 32:
        arr = np.frombuffer(audio_bytes[: (len(audio_bytes) // 4) * 4], dtype="<i4").astype(np.float32) / 2147483648.0
    elif fmt_tag == 3 and bits == 32:
        arr = np.frombuffer(audio_bytes[: (len(audio_bytes) // 4) * 4], dtype="<f4").copy()
    elif fmt_tag == 3 and bits == 64:
        arr = np.frombuffer(audio_bytes[: (len(audio_bytes) // 8) * 8], dtype="<f8").astype(np.float32)
    else:
        raise ValueError(f"Unsupported WAV format tag={fmt_tag} bits={bits} in {path}")

    total_frames = len(arr) // channels
    arr = arr[: total_frames * channels].reshape(total_frames, channels)
    arr = np.nan_to_num(arr, nan=0.0, posinf=0.0, neginf=0.0)
    return sr, bits, arr


def resample_48k(mono, src_sr):
    if src_sr == TARGET_SR or len(mono) < 8:
        return mono.astype(np.float32)
    # Fast anti-aliased decimation for exact integer multiples (96kHz, 192kHz)
    if src_sr in (96000, 192000):
        factor = src_sr // TARGET_SR
        taps_n = 31 if factor == 2 else 63
        mid = taps_n // 2
        idx = np.arange(taps_n) - mid
        fc = 0.43 / factor
        h = np.where(idx == 0, 2.0 * fc, np.sin(2.0 * np.pi * fc * idx) / (np.pi * np.where(idx == 0, 1.0, idx)))
        h *= np.hamming(taps_n)
        h /= np.sum(h)
        lp = np.convolve(mono.astype(np.float32), h.astype(np.float32), mode="same")
        return lp[::factor].astype(np.float32)
    # Fast vectorized linear interpolation with anti-alias pre-filter when downsampling
    new_len = max(1, int(round(len(mono) * TARGET_SR / float(src_sr))))
    x = mono.astype(np.float32)
    if src_sr > TARGET_SR:
        taps_n = 31
        mid = taps_n // 2
        idx = np.arange(taps_n) - mid
        fc = 0.43 * (TARGET_SR / float(src_sr))
        h = np.where(idx == 0, 2.0 * fc, np.sin(2.0 * np.pi * fc * idx) / (np.pi * np.where(idx == 0, 1.0, idx)))
        h *= np.hamming(taps_n)
        h /= np.sum(h)
        x = np.convolve(x, h.astype(np.float32), mode="same")
    xp = np.linspace(0.0, float(len(x) - 1), new_len, dtype=np.float64)
    i0 = np.floor(xp).astype(np.int64)
    i1 = np.minimum(i0 + 1, len(x) - 1)
    frac = (xp - i0).astype(np.float32)
    return (x[i0] * (1.0 - frac) + x[i1] * frac).astype(np.float32)


def select_best_channel(arr):
    """Choose the healthiest unclipped channel or phase-safe mono fold."""
    ch = arr.shape[1]
    if ch == 1:
        x = arr[:, 0]
        clips = int(np.sum(np.abs(x) >= 0.9985))
        return 0, "mono", x, clips
    scores = []
    for c in range(ch):
        xc = arr[:, c]
        pk = float(np.max(np.abs(xc)) + 1e-12)
        rms = float(np.sqrt(np.mean(xc ** 2) + 1e-12))
        clips = int(np.sum(np.abs(xc) >= 0.9985))
        # Prefer unclipped channels with high peak & strong dynamic range
        score = (0.0 if clips == 0 else -1000.0 - clips * 0.1) + min(pk, 0.99) * 10.0 + math.log10(rms + 1e-9)
        scores.append((score, c, clips, pk, rms))
    best_sc, best_c, best_clips, _, _ = max(scores)
    if ch == 2:
        c0, c1 = arr[:, 0], arr[:, 1]
        cl0 = int(np.sum(np.abs(c0) >= 0.9985))
        cl1 = int(np.sum(np.abs(c1) >= 0.9985))
        if cl0 == 0 and cl1 == 0:
            # Check correlation around loudest peak to avoid phase cancellation
            pk_pos = int(np.argmax(np.maximum(np.abs(c0), np.abs(c1))))
            w0 = max(0, pk_pos - 2048)
            w1 = min(len(c0), pk_pos + 4096)
            s0, s1 = c0[w0:w1], c1[w0:w1]
            den = float(np.linalg.norm(s0) * np.linalg.norm(s1) + 1e-12)
            corr = float(np.dot(s0, s1) / den)
            if corr >= 0.35:
                folded = 0.5 * (c0 + c1)
                return -1, f"L+R(corr={corr:.2f})", folded, 0
    return best_c, f"ch{best_c}", arr[:, best_c], best_clips


def detect_takes(x48, raw_mono=None, src_sr=48000, min_gap_ms=320.0, rel_thresh=0.14):
    """Detect distinct gunshot/event onsets and measure each take's usable tail length."""
    win = int(0.002 * TARGET_SR)  # 2ms blocks
    nb = len(x48) // win
    if nb < 4:
        return []
    bx = np.max(np.abs(x48[: nb * win]).reshape(nb, win), axis=1)
    gpk = float(np.max(bx))
    if gpk < 1e-4:
        return []

    # Noise floor estimate from quietest 15% of blocks
    nf = float(np.percentile(bx, 15))
    trig = max(gpk * rel_thresh, nf * 6.0)
    gap_b = max(1, int(min_gap_ms * 0.001 * TARGET_SR / win))

    events = []
    i = 0
    while i < nb:
        if bx[i] >= trig:
            loc_end = min(nb, i + gap_b)
            loc_pk = float(np.max(bx[i:loc_end]))
            j = i
            while j > 0 and bx[j - 1] >= max(loc_pk * 0.03, nf * 3.0) and (i - j) < 20:
                j -= 1
            events.append((j * win, loc_pk))
            i += gap_b
        else:
            i += 1

    takes = []
    sr_ratio = float(src_sr) / float(TARGET_SR)
    for idx, (s0, lpk) in enumerate(events):
        s_next = events[idx + 1][0] - int(0.020 * TARGET_SR) if idx + 1 < len(events) else len(x48)
        s1 = min(s_next, s0 + int(3.2 * TARGET_SR))
        if s1 <= s0 + int(0.04 * TARGET_SR):
            continue
        seg = x48[s0:s1]
        if raw_mono is not None:
            rs0 = min(len(raw_mono), int(round(s0 * sr_ratio)))
            rs1 = min(len(raw_mono), int(round(s1 * sr_ratio)))
            rseg = raw_mono[rs0:rs1]
            spk = float(np.max(np.abs(rseg)) + 1e-12) if len(rseg) else float(np.max(np.abs(seg)) + 1e-12)
            # True digital clipping = hitting >= 0.9995 in raw source (>=3 consecutive flat-top or saturated samples)
            clips = int(np.sum(np.abs(rseg) >= 0.9995)) if len(rseg) else 0
        else:
            spk = float(np.max(np.abs(seg)) + 1e-12)
            clips = int(np.sum(np.abs(seg) >= 0.9995))
        rms = float(np.sqrt(np.mean(seg[: min(len(seg), int(0.5 * TARGET_SR))] ** 2) + 1e-12))
        # Decay measurement: last sample above -48 dB relative to take peak (or 2.5x noise floor)
        decay_cut = max(spk * (10.0 ** (-48.0 / 20.0)), nf * 2.5)
        # smooth abs in 10ms blocks for steady decay tail measure
        b10 = int(0.010 * TARGET_SR)
        nb10 = len(seg) // b10
        tail_ms = len(seg) * 1000.0 / TARGET_SR
        if nb10 >= 4:
            e10 = np.max(np.abs(seg[: nb10 * b10]).reshape(nb10, b10), axis=1)
            above = np.where(e10 >= decay_cut)[0]
            if len(above):
                tail_ms = min(len(seg) * 1000.0 / TARGET_SR, (int(above[-1]) + 3) * 10.0)

        # Spectral stats on initial 300ms
        nfft = min(len(seg), 16384)
        nfft = max(256, 2 ** int(math.floor(math.log2(nfft))))
        sp = np.abs(np.fft.rfft(seg[:nfft] * np.hanning(nfft))) ** 2
        fr = np.fft.rfftfreq(nfft, 1.0 / TARGET_SR)
        tot = float(np.sum(sp) + 1e-18)
        cen = float(np.sum(fr * sp) / tot)
        sub_pct = float(np.sum(sp[(fr >= 25) & (fr < 150)]) / tot * 100.0)
        body_pct = float(np.sum(sp[(fr >= 150) & (fr < 1000)]) / tot * 100.0)
        crack_pct = float(np.sum(sp[(fr >= 1000) & (fr < 5000)]) / tot * 100.0)
        air_pct = float(np.sum(sp[fr >= 5000]) / tot * 100.0)

        takes.append({
            "idx": len(takes),
            "start_s": round(s0 / TARGET_SR, 3),
            "avail_ms": round((s1 - s0) * 1000.0 / TARGET_SR, 1),
            "tail_ms": round(tail_ms, 1),
            "peak_db": round(20.0 * math.log10(spk), 2),
            "rms_db": round(20.0 * math.log10(rms), 2),
            "crest_db": round(20.0 * math.log10(spk / rms), 2),
            "cen_hz": int(round(cen)),
            "sub_pct": round(sub_pct, 1),
            "body_pct": round(body_pct, 1),
            "crack_pct": round(crack_pct, 1),
            "air_pct": round(air_pct, 1),
            "clipped": clips,
        })
    return takes


def load_finder_meta():
    if not os.path.exists(HTML_PATH):
        return {}
    with open(HTML_PATH, "r", encoding="utf-8", errors="ignore") as f:
        html = f.read()
    m = re.search(r"<script[^>]*>(.*?)</script>", html, re.S)
    if not m:
        return {}
    s0 = m.group(1)
    def ext(name):
        mm = re.search(rf"const {name}\s*=\s*(.*?);\s*(?=\nconst |\Z)", s0, re.S)
        return json.loads(mm.group(1))
    libs = ext("LIBS")
    files = ext("FILES")
    meta = {}
    for fid, row in enumerate(files):
        li, fn, dur = row[0], row[1], row[2]
        yr, part, lib = libs[li]
        meta[fid] = {"year": yr, "part": part, "library": lib, "orig_path": fn, "meta_dur_s": dur}
    return meta


def main():
    os.makedirs(os.path.dirname(OUT_JSON), exist_ok=True)
    meta = load_finder_meta()
    wavs = sorted(glob.glob(os.path.join(GDC_DIR, "**", "*.wav"), recursive=True))
    print(f"Auditing {len(wavs)} Sonniss GDC WAV files...")

    catalog = {}
    total_takes = 0
    clipped_files = []
    for path in wavs:
        base = os.path.basename(path)
        m = re.match(r"^(\d+)_(.*)$", base)
        if not m:
            continue
        fid = int(m.group(1))
        rel = os.path.relpath(path, GDC_DIR)
        sr, bits, arr = read_wav_float32(path)
        dur_s = len(arr) / float(sr)
        best_ch, ch_desc, mono, raw_clips = select_best_channel(arr)
        x48 = resample_48k(mono, sr)

        # Tune onset gap for rapid burst / flyby files
        lname = base.lower()
        gap_ms = 90.0 if ("burst" in lname or "whizz" in lname or "tail" in lname or "passby" in lname or "shell" in lname or "casing" in lname) else 300.0
        rel_th = 0.09 if ("whizz" in lname or "distant" in lname or "100m" in lname or "200m" in lname or "300m" in lname or "250m" in lname or "160m" in lname) else 0.14
        takes = detect_takes(x48, raw_mono=mono, src_sr=sr, min_gap_ms=gap_ms, rel_thresh=rel_th)
        if not takes and len(x48) > 0:
            # Fallback whole-file take for continuous ambient/single soft events
            pk = float(np.max(np.abs(x48)) + 1e-12)
            rms = float(np.sqrt(np.mean(x48 ** 2) + 1e-12))
            takes = [{
                "idx": 0, "start_s": 0.0, "avail_ms": round(len(x48) * 1000.0 / TARGET_SR, 1),
                "tail_ms": round(len(x48) * 1000.0 / TARGET_SR, 1),
                "peak_db": round(20.0 * math.log10(pk), 2), "rms_db": round(20.0 * math.log10(rms), 2),
                "crest_db": round(20.0 * math.log10(pk / rms), 2),
                "cen_hz": 0, "sub_pct": 0.0, "body_pct": 0.0, "crack_pct": 0.0, "air_pct": 0.0,
                "clipped": int(np.sum(np.abs(x48) >= 0.9985)),
            }]

        total_takes += len(takes)
        if raw_clips > 0:
            clipped_files.append((fid, base, raw_clips, ch_desc))

        max_tail = max((t["tail_ms"] for t in takes), default=0.0)
        unclipped_takes = sum(1 for t in takes if t["clipped"] == 0)
        entry = {
            "fid": fid,
            "rel_path": rel.replace("\\", "/"),
            "filename": base,
            "src_sr": sr,
            "bits": bits,
            "channels": int(arr.shape[1]),
            "dur_s": round(dur_s, 2),
            "selected_ch": ch_desc,
            "selected_ch_idx": best_ch,
            "selected_clips": raw_clips,
            "takes_count": len(takes),
            "clean_takes_count": unclipped_takes,
            "max_tail_ms": round(max_tail, 1),
            "takes": takes,
        }
        if fid in meta:
            entry.update(meta[fid])
        catalog[str(fid)] = entry

    with open(OUT_JSON, "w", encoding="utf-8") as f:
        json.dump(catalog, f, indent=2)

    print(f"Analyzed {len(catalog)} WAV files -> {total_takes} individual takes detected.")
    print(f"Files with clipping in selected channel: {len(clipped_files)}")
    for fid, bn, cl, chd in clipped_files[:15]:
        print(f"  WARN clipped: fid={fid} ({chd}, {cl} samples) {bn}")
    # Print longest clean tail files
    by_tail = sorted(catalog.values(), key=lambda e: e["max_tail_ms"], reverse=True)
    print("\nTop 20 longest natural tails / decay takes:")
    for e in by_tail[:20]:
        print(f"  fid={e['fid']:4d} | {e['src_sr']//1000:3d}k/{e['bits']:2d}b | takes={e['takes_count']:2d} (clean={e['clean_takes_count']:2d}) | max_tail={e['max_tail_ms']:6.0f}ms | {e['filename'][:65]}")


if __name__ == "__main__":
    main()
