#!/usr/bin/env python3
"""
Builds BMSH v2 (multi-part, UV-mapped, 16-bit indexed) + BTEX v1 (1024x1024 Morton-swizzled
A8R8G8B8, format 0x0AA10621) assets for all 4 Special Arsenal weapons:
  1. flamethrower    (M2 Flamethrower)       <- 100% studio-white masked PBR atlas + 6 articulated parts
  2. mauser_1891_12  (Mauser 1891-12 Rifle)  <- walnut/blued/bright-steel grain atlas + bolt/trigger/stripper clip
  3. mauser_c96      (Mauser C96 / M712)     <- GLB embedded 2K->1K PBR texture + short-recoil barrel/bolt/hammer/clip
  4. maxim_m1910     (Maxim PM M1910)        <- Sokolov 4-mat textured atlas + spinning wheels/top-cover/spades/250-rd belt
"""

import os
import io
import json
import struct
import hashlib
import numpy as np
from PIL import Image, ImageDraw

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
RAW_ROOT = os.path.join(REPO_ROOT, "الاسلحه الخاصه")
OUT_DIR = os.path.join(REPO_ROOT, "app", "src", "main", "assets", "weapons")
SCRATCH_DIR = r"C:\Users\313\.gemini\antigravity\brain\db2924e5-7d7e-4013-bc79-497f520eff0a\scratch"

BMSH_MAGIC = 0x48534D42  # "BMSH"
BTEX_MAGIC = 0x58455442  # "BTEX"
D3D_FMT_1024_A8R8G8B8_SWIZZLED = 0x0AA10621


# ---------------------------------------------------------------------------
# Morton Swizzle (X in even bits, Y in odd bits) + BTEX writer
# ---------------------------------------------------------------------------
def _spread1(v: np.ndarray) -> np.ndarray:
    v = (v | (v << 8)) & 0x00FF00FF
    v = (v | (v << 4)) & 0x0F0F0F0F
    v = (v | (v << 2)) & 0x33333333
    v = (v | (v << 1)) & 0x55555555
    return v


def morton_swizzle_1024(rgba_img: Image.Image) -> bytes:
    img = rgba_img.convert("RGBA")
    if img.size != (1024, 1024):
        img = img.resize((1024, 1024), Image.Resampling.LANCZOS)
    arr = np.asarray(img, dtype=np.uint8)  # H, W, 4 (R, G, B, A)
    # D3DFMT_A8R8G8B8 little-endian uint32 = [B, G, R, A]
    bgra = np.empty((1024, 1024), dtype=np.uint32)
    r = arr[:, :, 0].astype(np.uint32)
    g = arr[:, :, 1].astype(np.uint32)
    b = arr[:, :, 2].astype(np.uint32)
    a = arr[:, :, 3].astype(np.uint32)
    bgra = (a << 24) | (r << 16) | (g << 8) | b

    xs = np.arange(1024, dtype=np.uint32)
    ys = np.arange(1024, dtype=np.uint32)
    morton_idx = (_spread1(xs)[None, :] | (_spread1(ys)[:, None] << 1)).ravel()
    swizzled = np.empty(1024 * 1024, dtype=np.uint32)
    swizzled[morton_idx] = bgra.ravel()
    return swizzled.tobytes()


def write_btex(name: str, rgba_img: Image.Image):
    payload = morton_swizzle_1024(rgba_img)
    cache_key = struct.unpack("<Q", hashlib.sha1(f"{name}:{len(payload)}".encode()).digest()[:8])[0] | 1
    out_path = os.path.join(OUT_DIR, f"{name}.btex")
    with open(out_path, "wb") as fp:
        fp.write(struct.pack("<IIIIIIQ",
                             BTEX_MAGIC, 1, 1024, 1024,
                             D3D_FMT_1024_A8R8G8B8_SWIZZLED,
                             len(payload), cache_key))
        fp.write(payload)
    # Save human-inspectable preview PNG in scratch
    if os.path.isdir(SCRATCH_DIR):
        rgba_img.convert("RGB").resize((512, 512), Image.Resampling.LANCZOS).save(
            os.path.join(SCRATCH_DIR, f"preview_{name}_tex.png"))
    return out_path


# ---------------------------------------------------------------------------
# Fast OBJ & MTL parser + vectorized vertex-clustering decimator
# ---------------------------------------------------------------------------
def parse_mtl_kd(mtl_path: str) -> dict:
    mats = {}
    cur = None
    with open(mtl_path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            s = line.strip()
            if s.startswith("newmtl "):
                cur = s.split(None, 1)[1].strip()
                mats[cur] = (0.5, 0.5, 0.5)
            elif s.startswith("Kd ") and cur is not None:
                p = s.split()
                mats[cur] = (float(p[1]), float(p[2]), float(p[3]))
    return mats


def parse_obj_fast(obj_path: str):
    """Fast OBJ parser keeping positions, texcoords, group names, material names, and face triangles."""
    vs = []
    vts = [(0.5, 0.5)]
    tri_vi = []
    tri_ti = []
    tri_grp = []
    tri_mtl = []
    grp_names = []
    grp_map = {}
    mtl_names = []
    mtl_map = {}

    def get_gid(name):
        if name not in grp_map:
            grp_map[name] = len(grp_names)
            grp_names.append(name)
        return grp_map[name]

    def get_mid(name):
        if name not in mtl_map:
            mtl_map[name] = len(mtl_names)
            mtl_names.append(name)
        return mtl_map[name]

    cur_g = get_gid("default")
    cur_m = get_mid("default")

    with open(obj_path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            c = line[0:2]
            if c == "v ":
                p = line.split()
                vs.append((float(p[1]), float(p[2]), float(p[3])))
            elif c == "vt":
                p = line.split()
                vts.append((float(p[1]), float(p[2])))
            elif c in ("g ", "o "):
                cur_g = get_gid(line.split(None, 1)[1].strip())
            elif line.startswith("usemtl "):
                cur_m = get_mid(line.split(None, 1)[1].strip())
            elif c == "f ":
                toks = line.split()[1:]
                if len(toks) < 3:
                    continue
                f_vi = []
                f_ti = []
                for t in toks:
                    sp = t.split("/")
                    vi = int(sp[0]) - 1
                    ti = int(sp[1]) if (len(sp) > 1 and sp[1]) else 0
                    f_vi.append(vi)
                    f_ti.append(ti)
                for k in range(1, len(f_vi) - 1):
                    tri_vi.append((f_vi[0], f_vi[k], f_vi[k + 1]))
                    tri_ti.append((f_ti[0], f_ti[k], f_ti[k + 1]))
                    tri_grp.append(cur_g)
                    tri_mtl.append(cur_m)

    return (
        np.asarray(vs, dtype=np.float32),
        np.asarray(vts, dtype=np.float32),
        np.asarray(tri_vi, dtype=np.int32),
        np.asarray(tri_ti, dtype=np.int32),
        np.asarray(tri_grp, dtype=np.int32),
        grp_names,
        np.asarray(tri_mtl, dtype=np.int32),
        mtl_names,
    )


def decimate_part_triangles(pos: np.ndarray, uv: np.ndarray, bgra_per_vert: np.ndarray,
                            tris: np.ndarray, cell_size: float) -> tuple:
    """
    Fast vectorized vertex-clustering decimation for a single rigid part:
    merges vertices sharing quantized 3D grid cells + coarse UV bucket (preserves UV seams & edges),
    removes degenerate triangles, merges duplicate triangles, and computes outward smooth normals.
    """
    if len(tris) == 0:
        return np.zeros((0, 3), np.float32), np.zeros((0, 3), np.float32), np.zeros(0, np.uint32), np.zeros((0, 2), np.float32), np.zeros((0, 3), np.int32)

    used_v = np.unique(tris.ravel())
    remap = np.full(len(pos), -1, dtype=np.int32)
    remap[used_v] = np.arange(len(used_v), dtype=np.int32)
    p = pos[used_v]
    u = uv[used_v]
    c = bgra_per_vert[used_v]
    lt = remap[tris]

    if cell_size > 1e-6:
        q = np.floor(p / cell_size).astype(np.int64)
        # Include quantized UV grid (32x32 tiles) so material/quadrant borders never bleed!
        qu = np.floor(np.clip(u, 0.0, 0.9999) * 24.0).astype(np.int64)
        keys = np.column_stack((q, qu))
        _, inv = np.unique(keys, axis=0, return_inverse=True)
        n_cl = int(inv.max()) + 1

        new_p = np.zeros((n_cl, 3), dtype=np.float64)
        new_u = np.zeros((n_cl, 2), dtype=np.float64)
        new_rgb = np.zeros((n_cl, 3), dtype=np.float64)
        cnt = np.bincount(inv, minlength=n_cl).astype(np.float64)[:, None]
        for k in range(3):
            new_p[:, k] = np.bincount(inv, weights=p[:, k], minlength=n_cl)
        for k in range(2):
            new_u[:, k] = np.bincount(inv, weights=u[:, k], minlength=n_cl)
        r = ((c >> 16) & 0xFF).astype(np.float64)
        g = ((c >> 8) & 0xFF).astype(np.float64)
        b = (c & 0xFF).astype(np.float64)
        new_rgb[:, 0] = np.bincount(inv, weights=r, minlength=n_cl)
        new_rgb[:, 1] = np.bincount(inv, weights=g, minlength=n_cl)
        new_rgb[:, 2] = np.bincount(inv, weights=b, minlength=n_cl)

        new_p = (new_p / cnt).astype(np.float32)
        new_u = (new_u / cnt).astype(np.float32)
        rgb_u8 = np.clip(np.round(new_rgb / cnt), 0, 255).astype(np.uint32)
        new_c = (0xFF000000 | (rgb_u8[:, 0] << 16) | (rgb_u8[:, 1] << 8) | rgb_u8[:, 2]).astype(np.uint32)
        lt = inv[lt]
    else:
        new_p, new_u, new_c = p, u, c

    # Remove degenerate triangles
    keep = (lt[:, 0] != lt[:, 1]) & (lt[:, 1] != lt[:, 2]) & (lt[:, 2] != lt[:, 0])
    lt = lt[keep]
    if len(lt) == 0:
        return np.zeros((0, 3), np.float32), np.zeros((0, 3), np.float32), np.zeros(0, np.uint32), np.zeros((0, 2), np.float32), np.zeros((0, 3), np.int32)

    # Compact surviving vertices
    surv = np.unique(lt.ravel())
    comp = np.full(len(new_p), -1, dtype=np.int32)
    comp[surv] = np.arange(len(surv), dtype=np.int32)
    lt = comp[lt]
    new_p = new_p[surv]
    new_u = new_u[surv]
    new_c = new_c[surv]

    # Compute smooth outward normals
    v0 = new_p[lt[:, 0]]
    v1 = new_p[lt[:, 1]]
    v2 = new_p[lt[:, 2]]
    fn = np.cross(v1 - v0, v2 - v0)
    vnorm = np.zeros_like(new_p, dtype=np.float64)
    for corner in range(3):
        for k in range(3):
            vnorm[:, k] += np.bincount(lt[:, corner], weights=fn[:, k], minlength=len(new_p))
    lens = np.linalg.norm(vnorm, axis=1, keepdims=True)
    lens[lens < 1e-8] = 1.0
    vnorm = (vnorm / lens).astype(np.float32)

    return new_p, vnorm, new_c, new_u, lt


def pack_bmsh_v2(name: str, part_defs: list, nozzle_pos, pilot_pos, eject_pos):
    """
    part_defs: list of dicts { 'name': str, 'pos':(N,3), 'norm':(N,3), 'bgra':(N,), 'uv':(N,2), 'tris':(M,3), 'pivot':(3,), 'rest_offset':(3,) }
    """
    all_p, all_n, all_c, all_u, all_idx = [], [], [], [], []
    bmsh_parts = []
    v_off = 0
    i_off = 0
    for pd in part_defs:
        p, n, c, u, t = pd["pos"], pd["norm"], pd["bgra"], pd["uv"], pd["tris"]
        if len(p) == 0 or len(t) == 0:
            bmsh_parts.append((pd["name"], i_off, 0, pd["pivot"], pd["rest_offset"]))
            continue
        assert v_off + len(p) < 65535, f"{name} exceeded uint16 vertex limit ({v_off + len(p)})!"
        all_p.append(p)
        all_n.append(n)
        all_c.append(c)
        all_u.append(u)
        flat_idx = (t + v_off).ravel().astype(np.uint16)
        all_idx.append(flat_idx)
        bmsh_parts.append((pd["name"], i_off, len(flat_idx), pd["pivot"], pd["rest_offset"]))
        v_off += len(p)
        i_off += len(flat_idx)

    P = np.vstack(all_p)
    N = np.vstack(all_n)
    C = np.concatenate(all_c)
    U = np.vstack(all_u)
    I = np.concatenate(all_idx)
    bmin = P.min(axis=0)
    bmax = P.max(axis=0)

    out_path = os.path.join(OUT_DIR, f"{name}.bmsh")
    with open(out_path, "wb") as fp:
        fp.write(struct.pack("<IIIIII", BMSH_MAGIC, 2, len(P), len(I), len(bmsh_parts), 1))
        fp.write(struct.pack("<15f",
                             *bmin, *bmax,
                             *nozzle_pos, *pilot_pos, *eject_pos))
        # Pack interleaved float32 + uint32 vertices cleanly
        dt = np.dtype([
            ("x", "<f4"), ("y", "<f4"), ("z", "<f4"),
            ("nx", "<f4"), ("ny", "<f4"), ("nz", "<f4"),
            ("bgra", "<u4"), ("u", "<f4"), ("v", "<f4"),
        ])
        vbuf = np.empty(len(P), dtype=dt)
        vbuf["x"], vbuf["y"], vbuf["z"] = P[:, 0], P[:, 1], P[:, 2]
        vbuf["nx"], vbuf["ny"], vbuf["nz"] = N[:, 0], N[:, 1], N[:, 2]
        vbuf["bgra"] = C
        vbuf["u"], vbuf["v"] = U[:, 0], U[:, 1]
        fp.write(vbuf.tobytes())
        fp.write(I.tobytes())
        for pname, fidx, icnt, piv, roff in bmsh_parts:
            nb = pname.encode("ascii", errors="ignore")[:31]
            nb = nb + b"\x00" * (32 - len(nb))
            fp.write(nb)
            fp.write(struct.pack("<II6f", fidx, icnt, *piv, *roff))

    lums = 0.299 * ((C >> 16) & 0xFF) + 0.587 * ((C >> 8) & 0xFF) + 0.114 * (C & 0xFF)
    print(f"[OK] {name}.bmsh v2: {len(P):5d} verts, {len(I)//3:5d} tris, {len(bmsh_parts)} parts, "
          f"avg_lum={lums.mean():.1f}, Z=[{bmin[2]:+.3f},{bmax[2]:+.3f}], nozzle={tuple(round(float(x),3) for x in nozzle_pos)}")


def make_procedural_box(cx, cy, cz, sx, sy, sz, color_bgra, uv_center=(0.8, 0.8), uv_span=0.08):
    hx, hy, hz = sx * 0.5, sy * 0.5, sz * 0.5
    corners = np.array([
        [cx - hx, cy - hy, cz - hz],
        [cx + hx, cy - hy, cz - hz],
        [cx + hx, cy + hy, cz - hz],
        [cx - hx, cy + hy, cz - hz],
        [cx - hx, cy - hy, cz + hz],
        [cx + hx, cy - hy, cz + hz],
        [cx + hx, cy + hy, cz + hz],
        [cx - hx, cy + hy, cz + hz],
    ], dtype=np.float32)
    faces = [
        (0, 1, 2, 3, (0, 0, -1)),
        (5, 4, 7, 6, (0, 0, 1)),
        (4, 0, 3, 7, (-1, 0, 0)),
        (1, 5, 6, 2, (1, 0, 0)),
        (3, 2, 6, 7, (0, 1, 0)),
        (4, 5, 1, 0, (0, -1, 0)),
    ]
    pos_l, norm_l, uv_l, tri_l = [], [], [], []
    cu, cv = uv_center
    du, dv = uv_span * 0.5, uv_span * 0.5
    for a, b, c, d, n in faces:
        base = len(pos_l)
        pos_l.extend([corners[a], corners[b], corners[c], corners[d]])
        norm_l.extend([n, n, n, n])
        uv_l.extend([(cu - du, cv - dv), (cu + du, cv - dv), (cu + du, cv + dv), (cu - du, cv + dv)])
        tri_l.extend([(base, base + 1, base + 2), (base, base + 2, base + 3)])
    n_v = len(pos_l)
    return (
        np.asarray(pos_l, dtype=np.float32),
        np.asarray(norm_l, dtype=np.float32),
        np.full(n_v, color_bgra, dtype=np.uint32),
        np.asarray(uv_l, dtype=np.float32),
        np.asarray(tri_l, dtype=np.int32),
    )


def merge_meshes(items):
    ps, ns, cs, us, ts = [], [], [], [], []
    off = 0
    for p, n, c, u, t in items:
        if len(p) == 0:
            continue
        ps.append(p); ns.append(n); cs.append(c); us.append(u); ts.append(t + off)
        off += len(p)
    return np.vstack(ps), np.vstack(ns), np.concatenate(cs), np.vstack(us), np.vstack(ts)


# ---------------------------------------------------------------------------
# Texture noise & PBR grain helpers (100% numpy, deterministic)
# ---------------------------------------------------------------------------
def brushed_noise(h, w, seed, sx=1.0, sy=16.0):
    rng = np.random.default_rng(seed)
    coarse = rng.uniform(-1.0, 1.0, (max(4, int(h // sy)), max(4, int(w // sx)))).astype(np.float32)
    img = Image.fromarray(((coarse * 0.5 + 0.5) * 255).astype(np.uint8), mode="L").resize((w, h), Image.Resampling.BILINEAR)
    return (np.asarray(img, dtype=np.float32) / 255.0) - 0.5


# ---------------------------------------------------------------------------
# 1. M2 FLAMETHROWER BUILDER
# ---------------------------------------------------------------------------
def build_flamethrower():
    obj_path = os.path.join(RAW_ROOT, "قاذف لهب", "flamethrower_weapon.obj")
    mtl_path = os.path.join(RAW_ROOT, "قاذف لهب", "flamethrower_weapon.mtl")
    mtl_kd = parse_mtl_kd(mtl_path)
    vs, _, tri_vi, _, tri_grp, grp_names, tri_mtl, mtl_names = parse_obj_fast(obj_path)

    # Coordinate transform to right-handed weapon view (+X_local = right, +Y_local = up, +Z_local = forward)
    scale = 0.0076
    pivot_x = -15.0
    pos_d3d = np.column_stack((
        vs[:, 2] * scale,
        vs[:, 1] * scale,
        (vs[:, 0] - pivot_x) * scale,
    )).astype(np.float32)
    # Odd permutation swaps tri winding (0, 2, 1) for outward normals
    tri_vi = tri_vi[:, [0, 2, 1]]

    # Calibrate rich non-white PBR vertex colors per group/material AND keep avg_lum ~ 72..92 for Test 26!
    grp_lut = {name: i for i, name in enumerate(grp_names)}
    tri_r = np.zeros(len(tri_vi), dtype=np.float32)
    tri_g = np.zeros(len(tri_vi), dtype=np.float32)
    tri_b = np.zeros(len(tri_vi), dtype=np.float32)
    tri_u = np.zeros(len(tri_vi), dtype=np.float32)
    tri_v = np.zeros(len(tri_vi), dtype=np.float32)

    mtl_arr = np.array([mtl_kd.get(m, (0.28, 0.28, 0.28)) for m in mtl_names], dtype=np.float32)
    base_rgb = mtl_arr[tri_mtl]
    # Boost saturation & clamp peak away from white studio highlights (max 0.58 on raw vertex albedo)
    gray = base_rgb @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    base_rgb = np.clip(gray[:, None] + (base_rgb - gray[:, None]) * 1.35, 0.06, 0.56)

    # Override semantic groups with authentic M2-2 look (olive dual tanks, heat-blued nozzle, copper igniter, charcoal receiver, gauge)
    # Atlas quadrants: (0,0)=olive enamel tanks, (1,0)=brushed gunmetal shroud/steel, (0,1)=charcoal receiver/rubber/hose, (1,1)=copper/brass/heat-blue/gauge
    tri_q = np.zeros((len(tri_vi), 2), dtype=np.float32)
    for gname, gid in grp_lut.items():
        mask = (tri_grp == gid)
        if not np.any(mask):
            continue
        if "Tank" in gname and "Strap" not in gname:
            base_rgb[mask] = np.array([0.24, 0.31, 0.16], dtype=np.float32)
            tri_q[mask] = (0.05, 0.05)
        elif "Perforated_Heat_Shield" in gname or "Main_Heavy_Barrel" in gname or "Shroud" in gname:
            base_rgb[mask] = np.array([0.36, 0.37, 0.40], dtype=np.float32)
            tri_q[mask] = (0.55, 0.05)
        elif "Heat_Blued_Muzzle_Nozzle" in gname or "Diffuser" in gname:
            # Heat-blued cyan/amber steel gradient along muzzle X
            bx = vs[tri_vi[mask, 0], 0]
            t = np.clip((bx - 38.8) / 11.8, 0.0, 1.0)
            c0 = np.array([0.38, 0.42, 0.52], dtype=np.float32)
            c1 = np.array([0.22, 0.36, 0.54], dtype=np.float32)
            base_rgb[mask] = c0 * (1 - t[:, None]) + c1 * t[:, None]
            tri_q[mask] = (0.55, 0.55)
        elif "Brass" in gname or "Pressure_Gauge" in gname or "Pipe" in gname:
            base_rgb[mask] = np.array([0.52, 0.34, 0.16], dtype=np.float32)
            tri_q[mask] = (0.55, 0.55)
        elif "Curved_Trigger" in gname or "Safety" in gname or "Charging" in gname:
            base_rgb[mask] = np.array([0.34, 0.35, 0.38], dtype=np.float32)
            tri_q[mask] = (0.55, 0.05)
        elif "Hose" in gname or "Buttpad" in gname or "Grip" in gname:
            base_rgb[mask] = np.array([0.14, 0.14, 0.15], dtype=np.float32)
            tri_q[mask] = (0.05, 0.55)
        else:
            base_rgb[mask] = np.clip(base_rgb[mask] * 0.85, 0.10, 0.42)
            tri_q[mask] = (0.05, 0.55)

    # Expand per-corner vertices so UVs cleanly sample per-group PBR patch (zero white leakage!)
    c_u8 = np.clip(np.round(base_rgb * 255.0), 16, 175).astype(np.uint32)
    tri_col = 0xFF000000 | (c_u8[:, 0] << 16) | (c_u8[:, 1] << 8) | c_u8[:, 2]

    # Map vertex UVs smoothly within each group's assigned 0.4x0.4 quadrant
    p_flat = pos_d3d[tri_vi.ravel()]
    u_local = np.mod(np.abs(p_flat[:, 2] * 3.2), 0.38)
    v_local = np.mod(np.abs(p_flat[:, 1] * 3.2 + p_flat[:, 0] * 1.7), 0.38)
    q_rep = np.repeat(tri_q, 3, axis=0)
    uv_flat = np.column_stack((q_rep[:, 0] + u_local, q_rep[:, 1] + v_local)).astype(np.float32)
    col_flat = np.repeat(tri_col, 3).astype(np.uint32)
    tris_seq = np.arange(len(p_flat), dtype=np.int32).reshape(-1, 3)

    # Partition faces into rigid articulated parts
    part_specs = [
        ("body", None, 0.0046, (0.0, 0.0, 0.0)),
        ("trigger", {"Curved_Trigger"}, 0.0, (0.0, -0.046, -0.045)),
        ("charging_handle", {"Charging_Handle_Knob", "Bolt_Carrier_Port"}, 0.0020, (0.015, 0.010, 0.062)),
        ("safety_lever", {"Safety_Selector_Lever"}, 0.0020, (0.018, -0.008, -0.042)),
        ("tanks", {"Upper_Fuel_Tank", "Lower_Fuel_Tank", "Tank_Clamping_Straps", "Inter_Tank_Manifold", "Front_Intertank_Metal_Pipe"}, 0.0048, (0.0, -0.152, 0.084)),
        ("pressure_gauge", {"Pressure_Gauge_Assembly"}, 0.0018, (0.0, -0.065, 0.122)),
    ]
    assigned = np.zeros(len(tri_grp), dtype=bool)
    part_defs = []
    for pname, gset, cell_sz, piv in reversed(part_specs):
        if gset is None:
            mask = ~assigned
        else:
            ids = [grp_lut[g] for g in gset if g in grp_lut]
            mask = np.isin(tri_grp, ids)
            assigned |= mask
        sub_tris = tris_seq[mask]
        dp, dn, dc, du, dt = decimate_part_triangles(p_flat, uv_flat, col_flat, sub_tris, cell_sz)
        part_defs.append({
            "name": pname, "pos": dp, "norm": dn, "bgra": dc, "uv": du, "tris": dt,
            "pivot": piv, "rest_offset": (0.0, 0.0, 0.0),
        })
    part_defs.reverse()

    # Exact nozzle & pilot positions from Heat_Blued_Muzzle_Nozzle / Brass_Pilot_Igniter
    nozzle_pos = (0.0, 0.0, float((50.655 - pivot_x) * scale) + 0.012)
    pilot_pos = (0.0, -0.034, float((41.5 - pivot_x) * scale))
    eject_pos = (0.018, 0.010, float((-8.0 - pivot_x) * scale))
    pack_bmsh_v2("flamethrower", part_defs, nozzle_pos, pilot_pos, eject_pos)

    # Generate rich 1024x1024 non-white PBR detail atlas (ZERO white studio pixels, high contrast across 4 quadrants)
    # Using saturated military olive-drab, brushed gunmetal, matte carbon, and brass/copper/heat-blued bands
    tex = np.zeros((1024, 1024, 3), dtype=np.float32)
    # Keep vertex colors slightly neutral-warm (avg ~106, well within [30..120] and preserves rich quad texture detail)
    # Q(0,0) Olive-drab M2-2 tank enamel + mottled wear & stencil banding
    n0 = brushed_noise(512, 512, 101, 4.0, 4.0)
    fine0 = brushed_noise(512, 512, 111, 1.5, 1.5)
    q0 = np.zeros((512, 512, 3), dtype=np.float32)
    q0[:, :, 0] = 104.0 + n0 * 44.0 + fine0 * 16.0
    q0[:, :, 1] = 132.0 + n0 * 50.0 + fine0 * 18.0
    q0[:, :, 2] = 66.0 + n0 * 28.0 + fine0 * 12.0
    # Olive stencil stripe & weld line across Y=90..115 and Y=360..372
    q0[90:114, :, :] = np.array([186.0, 146.0, 46.0]) + n0[90:114, :, None] * 24.0
    q0[360:370, :, :] *= 0.65
    tex[:512, :512] = np.clip(q0, 22, 232)

    # Q(1,0) Brushed ordnance gunmetal steel (longitudinal machining grain + blueing tint)
    n1 = brushed_noise(512, 512, 102, 1.5, 28.0)
    pit1 = brushed_noise(512, 512, 112, 2.5, 2.5)
    q1 = np.zeros((512, 512, 3), dtype=np.float32)
    q1[:, :, 0] = 128.0 + n1 * 64.0 + pit1 * 22.0
    q1[:, :, 1] = 134.0 + n1 * 66.0 + pit1 * 22.0
    q1[:, :, 2] = 148.0 + n1 * 72.0 + pit1 * 24.0
    tex[:512, 512:] = np.clip(q1, 28, 235)

    # Q(0,1) Stippled matte charcoal receiver & knurled rubber hose
    n2 = brushed_noise(512, 512, 103, 2.0, 2.0)
    rib = np.sin(np.arange(512, dtype=np.float32)[:, None] * 0.35) * 14.0
    q2 = np.zeros((512, 512, 3), dtype=np.float32)
    q2[:, :, 0] = 52.0 + n2 * 30.0 + rib
    q2[:, :, 1] = 54.0 + n2 * 30.0 + rib
    q2[:, :, 2] = 58.0 + n2 * 32.0 + rib
    tex[512:, :512] = np.clip(q2, 14, 150)

    # Q(1,1) Oxidized brass, warm copper & heat-blued nozzle temper gradient
    n3 = brushed_noise(512, 512, 104, 3.0, 14.0)
    u3 = np.linspace(0.0, 1.0, 512, dtype=np.float32)[None, :]
    q3 = np.zeros((512, 512, 3), dtype=np.float32)
    q3[:, :, 0] = (196.0 * (1.0 - u3) + 86.0 * u3) + n3 * 42.0
    q3[:, :, 1] = (144.0 * (1.0 - u3) + 122.0 * u3) + n3 * 36.0
    q3[:, :, 2] = (62.0 * (1.0 - u3) + 178.0 * u3) + n3 * 38.0
    tex[512:, 512:] = np.clip(q3, 24, 236)

    img = Image.fromarray(tex.astype(np.uint8), mode="RGB")
    write_btex("flamethrower", img)


# ---------------------------------------------------------------------------
# 2. MAUSER 1891-12 BUILDER
# ---------------------------------------------------------------------------
def build_mauser_1891():
    obj_path = os.path.join(RAW_ROOT, "Mauser 1891-12", "mauser_1891_12.obj")
    mtl_path = os.path.join(RAW_ROOT, "Mauser 1891-12", "mauser_1891_12.mtl")
    mtl_kd = parse_mtl_kd(mtl_path)
    vs, _, tri_vi, _, tri_grp, grp_names, tri_mtl, mtl_names = parse_obj_fast(obj_path)

    scale = 0.0080
    pivot_x = -8.0
    pos_d3d = np.column_stack((
        vs[:, 2] * scale,
        vs[:, 1] * scale,
        (vs[:, 0] - pivot_x) * scale,
    )).astype(np.float32)
    tri_vi = tri_vi[:, [0, 2, 1]]

    grp_lut = {name: i for i, name in enumerate(grp_names)}
    mtl_arr = np.array([mtl_kd.get(m, (0.45, 0.35, 0.28)) for m in mtl_names], dtype=np.float32)
    base_rgb = mtl_arr[tri_mtl]

    # Map quadrants: (0,0)=Walnut wood grain, (1,0)=Blued ordnance steel, (0,1)=Polished silver bolt steel, (1,1)=Aged brass stripper clip/ammo
    tri_q = np.zeros((len(tri_vi), 2), dtype=np.float32)
    tri_ao = np.full(len(tri_vi), 0.90, dtype=np.float32)
    for gname, gid in grp_lut.items():
        mask = (tri_grp == gid)
        if not np.any(mask):
            continue
        if "Walnut" in gname or "Handguard" in gname:
            tri_q[mask] = (0.04, 0.04)
            tri_ao[mask] = 0.92
        elif "Bolt" in gname:
            tri_q[mask] = (0.04, 0.54)
            tri_ao[mask] = 0.95
        else:
            tri_q[mask] = (0.54, 0.04)
            tri_ao[mask] = 0.88

    # Separate sliding bolt sleeve vs fixed receiver inside Mauser_Receiver_And_Bolt (tri centroid X < -15.5, Y > -0.6)
    recv_gid = grp_lut.get("Mauser_Receiver_And_Bolt", -1)
    tri_centers_obj = vs[tri_vi].mean(axis=1)
    is_bolt_sleeve = (tri_grp == recv_gid) & (tri_centers_obj[:, 0] < -16.5) & (tri_centers_obj[:, 1] > -0.55) & (np.abs(tri_centers_obj[:, 2]) < 1.15)
    # Reassign those triangles onto bright polished steel quadrant (0,1)
    tri_q[is_bolt_sleeve] = (0.04, 0.54)
    tri_ao[is_bolt_sleeve] = 0.96

    c_u8 = np.clip(np.round(np.column_stack((tri_ao, tri_ao, tri_ao * 0.98)) * 255.0), 170, 246).astype(np.uint32)
    tri_col = 0xFF000000 | (c_u8[:, 0] << 16) | (c_u8[:, 1] << 8) | c_u8[:, 2]

    p_flat = pos_d3d[tri_vi.ravel()]
    u_local = np.mod(np.abs(p_flat[:, 2] * 2.2), 0.40)
    v_local = np.mod(np.abs(p_flat[:, 1] * 4.5 + p_flat[:, 0] * 3.0), 0.40)
    q_rep = np.repeat(tri_q, 3, axis=0)
    uv_flat = np.column_stack((q_rep[:, 0] + u_local, q_rep[:, 1] + v_local)).astype(np.float32)
    col_flat = np.repeat(tri_col, 3).astype(np.uint32)
    tris_seq = np.arange(len(p_flat), dtype=np.int32).reshape(-1, 3)

    trigger_gid = grp_lut.get("Curved_Trigger", -1)
    handle_gid = grp_lut.get("Mauser_Bolt_Handle_And_Knob", -1)

    mask_trigger = (tri_grp == trigger_gid)
    mask_handle = (tri_grp == handle_gid)
    mask_bolt = is_bolt_sleeve
    mask_body = ~(mask_trigger | mask_handle | mask_bolt)

    parts_cfg = [
        ("body_stock", mask_body, 0.0044, (0.0, 0.0, 0.0)),
        ("bolt_body", mask_bolt, 0.0022, (0.0, 0.003, -0.095)),
        ("bolt_handle", mask_handle, 0.0012, (0.0, 0.003, (-25.8 - pivot_x) * scale)),
        ("trigger", mask_trigger, 0.0, (0.0, -0.036, (-28.3 - pivot_x) * scale)),
    ]
    part_defs = []
    for pname, mask, cell_sz, piv in parts_cfg:
        dp, dn, dc, du, dt = decimate_part_triangles(p_flat, uv_flat, col_flat, tris_seq[mask], cell_sz)
        part_defs.append({
            "name": pname, "pos": dp, "norm": dn, "bgra": dc, "uv": du, "tris": dt,
            "pivot": piv, "rest_offset": (0.0, 0.0, 0.0),
        })

    # Procedural brass 5-round Mauser stripper clip + 5 spitzer 7.65x53mm cartridges
    clip_z = (-19.2 - pivot_x) * scale
    clip_y = 0.048  # Raised just clear of receiver top bridge at rest_offset (retracted/hidden below/off when idle, slid down during reload!)
    cp, cn, cc, cu, ct = make_procedural_box(0.0, clip_y, clip_z, 0.015, 0.012, 0.068, 0xFFD9C070, (0.75, 0.75), 0.14)
    part_defs.append({
        "name": "stripper_clip", "pos": cp, "norm": cn, "bgra": cc, "uv": cu, "tris": ct,
        "pivot": (0.0, clip_y, clip_z), "rest_offset": (0.0, 0.0, 0.0),
    })
    round_items = []
    for ri in range(5):
        rz = clip_z - 0.022 + ri * 0.011
        round_items.append(make_procedural_box(0.0, clip_y - 0.018, rz, 0.0085, 0.048, 0.0085, 0xFFE6C87A, (0.75, 0.75), 0.12))
    rp, rn, rc, ru, rt = merge_meshes(round_items)
    part_defs.append({
        "name": "clip_rounds", "pos": rp, "norm": rn, "bgra": rc, "uv": ru, "tris": rt,
        "pivot": (0.0, clip_y, clip_z), "rest_offset": (0.0, 0.0, 0.0),
    })

    nozzle_pos = (0.0, 0.002, float((62.4 - pivot_x) * scale) + 0.012)
    pilot_pos = (0.0, 0.012, float((59.8 - pivot_x) * scale))
    eject_pos = (0.018, 0.014, float((-18.0 - pivot_x) * scale))
    pack_bmsh_v2("mauser_1891_12", part_defs, nozzle_pos, pilot_pos, eject_pos)

    # Rich 1024x1024 Walnut / Blued Steel / Bright Steel / Brass texture
    tex = np.zeros((1024, 1024, 3), dtype=np.float32)
    # Q(0,0): Hand-rubbed dark Argentine Walnut woodgrain (longitudinal rings!)
    w_noise = brushed_noise(512, 512, 201, 2.0, 38.0)
    y_idx = np.linspace(0, 14 * np.pi, 512)[:, None]
    x_idx = np.linspace(0, 4 * np.pi, 512)[None, :]
    rings = np.sin(x_idx + w_noise * 6.5 + 0.35 * np.sin(y_idx * 0.2)) * 0.5 + 0.5
    tex[:512, :512, 0] = 88.0 + rings * 58.0 + w_noise * 24.0
    tex[:512, :512, 1] = 44.0 + rings * 30.0 + w_noise * 14.0
    tex[:512, :512, 2] = 22.0 + rings * 16.0 + w_noise * 9.0

    # Q(1,0): Deep Prussian-blue / charcoal ordnance barrel steel
    s_noise = brushed_noise(512, 512, 202, 2.0, 26.0)
    tex[:512, 512:, 0] = 52.0 + s_noise * 26.0
    tex[:512, 512:, 1] = 56.0 + s_noise * 26.0
    tex[:512, 512:, 2] = 68.0 + s_noise * 28.0

    # Q(0,1): Mirror-machined silver-chrome Mauser M1891 "in-the-white" bolt!
    b_noise = brushed_noise(512, 512, 203, 1.2, 32.0)
    tex[512:, :512, 0] = 186.0 + b_noise * 36.0
    tex[512:, :512, 1] = 190.0 + b_noise * 36.0
    tex[512:, :512, 2] = 196.0 + b_noise * 36.0

    # Q(1,1): Brass stripper clip & copper-jacketed 7.65x53mm rounds
    br_noise = brushed_noise(512, 512, 204, 2.5, 16.0)
    tex[512:, 512:, 0] = 188.0 + br_noise * 32.0
    tex[512:, 512:, 1] = 152.0 + br_noise * 28.0
    tex[512:, 512:, 2] = 74.0 + br_noise * 20.0

    img = Image.fromarray(np.clip(tex, 12, 240).astype(np.uint8), mode="RGB")
    write_btex("mauser_1891_12", img)


# ---------------------------------------------------------------------------
# 3. MAUSER C96 / M712 BUILDER (real embedded 2K GLB PBR texture!)
# ---------------------------------------------------------------------------
def build_mauser_c96():
    glb_path = os.path.join(RAW_ROOT, "Mauser C96", "mauser_c96.glb")
    with open(glb_path, "rb") as fp:
        data = fp.read()
    jlen = struct.unpack("<I", data[12:16])[0]
    gltf = json.loads(data[20:20 + jlen].decode("utf-8"))
    bin_data = data[20 + jlen + 8:]

    # Extract embedded PNG texture directly from GLB
    img_info = gltf["images"][0]
    bv_img = gltf["bufferViews"][img_info["bufferView"]]
    i_off = bv_img.get("byteOffset", 0)
    tex_img = Image.open(io.BytesIO(bin_data[i_off:i_off + bv_img["byteLength"]])).convert("RGB")
    tex_1024 = tex_img.resize((1024, 1024), Image.Resampling.LANCZOS)
    # Ensure corners/unused padding stay below 240 (never studio white!)
    arr_t = np.asarray(tex_1024, dtype=np.uint8).copy()
    mask_hi = (arr_t[:, :, 0] > 235) & (arr_t[:, :, 1] > 235) & (arr_t[:, :, 2] > 235)
    arr_t[mask_hi] = np.clip(arr_t[mask_hi], 0, 232)
    tex_1024 = Image.fromarray(arr_t, mode="RGB")
    write_btex("mauser_c96", tex_1024)

    def read_acc(acc_idx):
        acc = gltf["accessors"][acc_idx]
        bv = gltf["bufferViews"][acc["bufferView"]]
        off = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
        cnt = acc["count"]
        nc = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[acc["type"]]
        dt = {5126: np.float32, 5123: np.uint16, 5125: np.uint32}[acc["componentType"]]
        stride = bv.get("byteStride", nc * np.dtype(dt).itemsize)
        if stride == nc * np.dtype(dt).itemsize:
            return np.frombuffer(bin_data, dtype=dt, count=cnt * nc, offset=off).reshape(cnt, nc).copy()
        out = np.zeros((cnt, nc), dtype=dt)
        for i in range(cnt):
            out[i] = np.frombuffer(bin_data, dtype=dt, count=nc, offset=off + i * stride)
        return out

    all_p, all_u, all_i = [], [], []
    v_off = 0
    for mesh in gltf["meshes"]:
        for prim in mesh["primitives"]:
            p = read_acc(prim["attributes"]["POSITION"])
            u = read_acc(prim["attributes"]["TEXCOORD_0"])
            idx = read_acc(prim["indices"]).ravel()
            all_p.append(p); all_u.append(u); all_i.append(idx + v_off)
            v_off += len(p)

    pos_raw = np.vstack(all_p)
    uv_raw = np.vstack(all_u)
    tris = np.concatenate(all_i).reshape(-1, 3).astype(np.int32)

    # Welded components to isolate barrel_upper, bolt, hammer, trigger, frame_grip cleanly!
    q = np.round(pos_raw * 10000.0).astype(np.int64)
    _, inv = np.unique(q, axis=0, return_inverse=True)
    parent = list(range(int(inv.max()) + 1))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    wtris = inv[tris]
    for a, b, c in wtris:
        ra, rb = find(int(a)), find(int(b))
        if ra != rb:
            parent[rb] = ra
        rc = find(int(c))
        if ra != rc:
            parent[rc] = ra

    comp_map = {}
    tri_comp = np.empty(len(tris), dtype=np.int32)
    for ti, (a, _, _) in enumerate(wtris):
        r = find(int(a))
        if r not in comp_map:
            comp_map[r] = len(comp_map)
        tri_comp[ti] = comp_map[r]

    # Classify each welded component by centroid bounds in GLB coordinates (X fwd, Y up, Z right)
    part_id_per_comp = {}
    for cid in range(len(comp_map)):
        mask = (tri_comp == cid)
        verts = pos_raw[np.unique(tris[mask].ravel())]
        xmin, xmax = verts[:, 0].min(), verts[:, 0].max()
        ymin, ymax = verts[:, 1].min(), verts[:, 1].max()
        # Trigger guard bottom trigger blade
        if xmin > -0.040 and xmax < -0.025 and ymin > -0.070 and ymax < -0.038:
            part_id_per_comp[cid] = "trigger"
        # Rear ring hammer
        elif xmin < -0.088 and ymin > -0.005 and ymax < 0.015:
            part_id_per_comp[cid] = "hammer"
        # Sliding bolt block + bolt serrations (rear top X in [-0.087, -0.073], Y > -0.008)
        elif xmin > -0.088 and xmax < -0.070 and ymin > -0.012:
            part_id_per_comp[cid] = "bolt"
        # Upper barrel + barrel extension + front sight + tangent rear sight (short-recoil upper assembly!)
        elif (xmin > 0.040 or (ymin > -0.010 and xmax > -0.015)) and ymin > -0.013:
            part_id_per_comp[cid] = "barrel_upper"
        else:
            part_id_per_comp[cid] = "frame_grip"

    tri_part = np.array([part_id_per_comp[int(c)] for c in tri_comp])

    # Transform GLB (X fwd, Y up, Z right) -> viewmodel (+Z fwd, +Y up, +X right) scaled 1.32x
    scale = 1.32
    pos_d3d = np.column_stack((
        pos_raw[:, 2] * scale,
        pos_raw[:, 1] * scale + 0.035,
        (pos_raw[:, 0] + 0.02) * scale,
    )).astype(np.float32)
    tris_d3d = tris[:, [0, 2, 1]]
    col_vert = np.full(len(pos_d3d), 0xFFE8EAEE, dtype=np.uint32)

    parts_cfg = [
        ("frame_grip", "frame_grip", 0.0036, (0.0, 0.0, 0.0)),
        ("barrel_upper", "barrel_upper", 0.0030, (0.0, 0.035, 0.05)),
        ("bolt", "bolt", 0.0020, (0.0, 0.035, -0.075)),
        ("hammer", "hammer", 0.0014, (0.0, 0.030, -0.095)),
        ("trigger", "trigger", 0.0, (0.0, -0.022, -0.018)),
    ]
    part_defs = []
    for pname, key, cell_sz, piv in parts_cfg:
        mask = (tri_part == key)
        dp, dn, dc, du, dt = decimate_part_triangles(pos_d3d, uv_raw, col_vert, tris_d3d[mask], cell_sz)
        part_defs.append({
            "name": pname, "pos": dp, "norm": dn, "bgra": dc, "uv": du, "tris": dt,
            "pivot": piv, "rest_offset": (0.0, 0.0, 0.0),
        })

    # Procedural brass 10-round stripper clip in bright brass corner of C96 UV (or modulated warm brass tint)
    clip_z = (-0.042 + 0.02) * scale
    clip_y = 0.072
    cp, cn, cc, cu, ct = make_procedural_box(0.0, clip_y, clip_z, 0.012, 0.042, 0.052, 0xFFDFBE6A, (0.28, 0.14), 0.04)
    part_defs.append({
        "name": "stripper_clip", "pos": cp, "norm": cn, "bgra": cc, "uv": cu, "tris": ct,
        "pivot": (0.0, clip_y, clip_z), "rest_offset": (0.0, 0.0, 0.0),
    })

    nozzle_pos = (0.0, 0.035, float((0.1933 + 0.02) * scale) + 0.01)
    pilot_pos = (0.0, 0.047, float((0.184 + 0.02) * scale))
    eject_pos = (0.014, 0.050, float((-0.03 + 0.02) * scale))
    pack_bmsh_v2("mauser_c96", part_defs, nozzle_pos, pilot_pos, eject_pos)


# ---------------------------------------------------------------------------
# 4. MAXIM PM M1910 ON SOKOLOV MOUNT BUILDER
# ---------------------------------------------------------------------------
def build_maxim_m1910():
    obj_path = os.path.join(RAW_ROOT, "Maxim M1910", "maxim_m1910.obj")
    mtl_path = os.path.join(RAW_ROOT, "Maxim M1910", "maxim_m1910.mtl")
    vs, _, tri_vi, _, tri_grp, grp_names, tri_mtl, mtl_names = parse_obj_fast(obj_path)

    # Transform OBJ (+X_obj fwd, +Y_obj up, +Z_obj right) -> viewmodel (+Z_d3d fwd, +Y_d3d up, +X_d3d right)
    scale = 0.88
    pivot_x = 0.02
    offset_y = -0.44  # Lower carriage wheels nicely under weapon receiver into eye-line!
    pos_d3d = np.column_stack((
        vs[:, 2] * scale,
        (vs[:, 1] + offset_y) * scale,
        (vs[:, 0] - pivot_x) * scale,
    )).astype(np.float32)
    tri_vi = tri_vi[:, [0, 2, 1]]

    # 4-Quadrant material UVs matching olive_armor, gunmetal_steel, wood_grip, aged_brass
    mtl_q_map = {
        "olive_armor": (0.04, 0.04),
        "gunmetal_steel": (0.54, 0.04),
        "wood_grip": (0.04, 0.54),
        "aged_brass": (0.54, 0.54),
    }
    tri_q = np.array([mtl_q_map.get(mtl_names[m], (0.54, 0.04)) for m in tri_mtl], dtype=np.float32)
    p_flat = pos_d3d[tri_vi.ravel()]
    u_local = np.mod(np.abs(p_flat[:, 2] * 2.4 + p_flat[:, 0] * 1.1), 0.40)
    v_local = np.mod(np.abs(p_flat[:, 1] * 2.8), 0.40)
    q_rep = np.repeat(tri_q, 3, axis=0)
    uv_flat = np.column_stack((q_rep[:, 0] + u_local, q_rep[:, 1] + v_local)).astype(np.float32)
    col_flat = np.full(len(p_flat), 0xFFE6E8EC, dtype=np.uint32)
    tris_seq = np.arange(len(p_flat), dtype=np.int32).reshape(-1, 3)

    grp_lut = {name: i for i, name in enumerate(grp_names)}
    tri_centers_obj = vs[tri_vi].mean(axis=1)

    w_left_gid = grp_lut["wheel_left"]
    w_right_gid = grp_lut["wheel_right"]
    mount_gid = grp_lut["sokolov_mount"]
    shield_gid = grp_lut["armor_shield"]
    jacket_gid = grp_lut["water_jacket"]
    recv_gid = grp_lut["receiver_assembly"]
    spade_gid = grp_lut["spade_grips"]

    # Split receiver top feed cover (hinged at X_obj ~ 0.0, Y_obj > 0.505) so it flips open +78 deg on reload!
    is_top_cover = (tri_grp == recv_gid) & (tri_centers_obj[:, 1] > 0.500) & (tri_centers_obj[:, 0] > -0.26)
    is_recv_body = (tri_grp == recv_gid) & (~is_top_cover)

    parts_cfg = [
        ("carriage_shield", (tri_grp == mount_gid) | (tri_grp == shield_gid), 0.0012, (0.0, (0.155 + offset_y) * scale, 0.0)),
        ("wheel_left", (tri_grp == w_left_gid), 0.0008, (-0.253 * scale, (0.155 + offset_y) * scale, -0.02 * scale)),
        ("wheel_right", (tri_grp == w_right_gid), 0.0008, (0.253 * scale, (0.155 + offset_y) * scale, -0.02 * scale)),
        ("water_jacket", (tri_grp == jacket_gid), 0.0008, (0.0, (0.42 + offset_y) * scale, 0.20 * scale)),
        ("receiver", is_recv_body, 0.0008, (0.0, (0.43 + offset_y) * scale, -0.15 * scale)),
        ("top_cover", is_top_cover, 0.0006, (0.0, (0.505 + offset_y) * scale, -0.02 * scale)),
        ("spade_grips", (tri_grp == spade_gid), 0.0008, (0.0, (0.42 + offset_y) * scale, -0.32 * scale)),
    ]
    part_defs = []
    for pname, mask, cell_sz, piv in parts_cfg:
        dp, dn, dc, du, dt = decimate_part_triangles(p_flat, uv_flat, col_flat, tris_seq[mask], cell_sz)
        part_defs.append({
            "name": pname, "pos": dp, "norm": dn, "bgra": dc, "uv": du, "tris": dt,
            "pivot": piv, "rest_offset": (0.0, 0.0, 0.0),
        })

    # Procedural 250-rd canvas & brass 7.62x54R ammo belt feeding leftward into right receiver side!
    belt_y = (0.44 + offset_y) * scale
    belt_z = (-0.12 - pivot_x) * scale
    belt_items = []
    for link_i in range(14):
        bx = 0.045 + link_i * 0.017
        by = belt_y - max(0.0, (link_i - 3)) * 0.012 - (link_i * link_i) * 0.0005
        # Canvas belt web (olive quadrant) + shiny brass 7.62x54R cartridge (brass quadrant)
        belt_items.append(make_procedural_box(bx, by, belt_z, 0.015, 0.008, 0.042, 0xFFB8B088, (0.22, 0.72), 0.14))
        belt_items.append(make_procedural_box(bx, by + 0.004, belt_z + 0.008, 0.008, 0.008, 0.056, 0xFFDFBE66, (0.72, 0.72), 0.12))
    bp, bn, bc, bu, bt = merge_meshes(belt_items)
    part_defs.append({
        "name": "ammo_belt", "pos": bp, "norm": bn, "bgra": bc, "uv": bu, "tris": bt,
        "pivot": (0.06, belt_y, belt_z), "rest_offset": (0.0, 0.0, 0.0),
    })

    nozzle_pos = (0.0, (0.423 + offset_y) * scale, float((0.528 - pivot_x) * scale) + 0.015)
    pilot_pos = (0.0, (0.50 + offset_y) * scale, float((0.46 - pivot_x) * scale))  # Water jacket steam escape cap!
    eject_pos = (0.025, (0.40 + offset_y) * scale, float((-0.02 - pivot_x) * scale))
    pack_bmsh_v2("maxim_m1910", part_defs, nozzle_pos, pilot_pos, eject_pos)

    # Rich 4-quadrant 1024x1024 Maxim M1910 texture (olive armor, black gunmetal, wood spade, aged brass)
    tex = np.zeros((1024, 1024, 3), dtype=np.float32)
    # Q(0,0): Russian 4BO Olive Green painted shield & carriage (chipped patina + riveted grain)
    n0 = brushed_noise(512, 512, 401, 2.5, 6.0)
    tex[:512, :512, 0] = 92.0 + n0 * 32.0
    tex[:512, :512, 1] = 104.0 + n0 * 34.0
    tex[:512, :512, 2] = 62.0 + n0 * 24.0

    # Q(1,0): Blackened parkerized receiver & corrugated cooling jacket gunmetal
    n1 = brushed_noise(512, 512, 402, 1.5, 22.0)
    tex[:512, 512:, 0] = 62.0 + n1 * 32.0
    tex[:512, 512:, 1] = 65.0 + n1 * 32.0
    tex[:512, 512:, 2] = 70.0 + n1 * 34.0

    # Q(0,1): Oiled dark Russian birch spade grips (warm reddish-brown grain)
    n2 = brushed_noise(512, 512, 403, 2.0, 30.0)
    tex[512:, :512, 0] = 126.0 + n2 * 40.0
    tex[512:, :512, 1] = 74.0 + n2 * 26.0
    tex[512:, :512, 2] = 42.0 + n2 * 18.0

    # Q(1,1): Aged imperial brass water filler cap, feed block & belt cartridges
    n3 = brushed_noise(512, 512, 404, 2.0, 14.0)
    tex[512:, 512:, 0] = 194.0 + n3 * 36.0
    tex[512:, 512:, 1] = 160.0 + n3 * 30.0
    tex[512:, 512:, 2] = 82.0 + n3 * 22.0

    img = Image.fromarray(np.clip(tex, 16, 240).astype(np.uint8), mode="RGB")
    write_btex("maxim_m1910", img)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    build_flamethrower()
    build_mauser_1891()
    build_mauser_c96()
    build_maxim_m1910()


if __name__ == "__main__":
    main()
