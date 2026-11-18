#!/usr/bin/env python3
"""Build the M91 (Karabiner 98k) BMSH v2 + BTEX v1 asset from the new AAA GLB pack.

Self-contained (does not import build_special_weapons.py) so it can be regenerated
independently. Reads:
    الاسلحه الخاصه/Mauser 1891-12/model/kar_98_lod0.glb
    الاسلحه الخاصه/Mauser 1891-12/textures/kar98_tex_{0,3,6}.jpg
    الاسلحه الخاصه/Mauser 1891-12/animation/bolt_cycle.json   (world pivots)
Writes:
    app/src/main/assets/weapons/mauser_1891_12.bmsh
    app/src/main/assets/weapons/mauser_1891_12.btex
"""
import io
import json
import os
import struct

import numpy as np
from PIL import Image

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PACK_ROOT = os.path.join(REPO_ROOT, "الاسلحه الخاصه", "Mauser 1891-12")
OUT_DIR = os.path.join(REPO_ROOT, "app", "src", "main", "assets", "weapons")

BMSH_MAGIC = 0x48534D42
BTEX_MAGIC = 0x58455442
D3D_FMT = 0x0AA10621

# ---------------------------------------------------------------------------
# BTEX helpers (identical to build_special_weapons.py)
# ---------------------------------------------------------------------------
def _spread1(v):
    v = (v | (v << 8)) & 0x00FF00FF
    v = (v | (v << 4)) & 0x0F0F0F0F
    v = (v | (v << 2)) & 0x33333333
    v = (v | (v << 1)) & 0x55555555
    return v


def morton_swizzle(rgba_img, size=1024):
    a = np.asarray(rgba_img.convert("RGBA").resize((size, size), Image.LANCZOS))
    r, g, b, al = a[:, :, 0].astype(np.uint32), a[:, :, 1].astype(np.uint32), \
        a[:, :, 2].astype(np.uint32), a[:, :, 3].astype(np.uint32)
    bgra = (al << 24) | (r << 16) | (g << 8) | b
    ys, xs = np.mgrid[0:size, 0:size]
    morton = (_spread1(xs.astype(np.uint32)) | (_spread1(ys.astype(np.uint32)) << 1)).ravel()
    out = np.empty(size * size, dtype=np.uint32)
    out[morton] = bgra.ravel()
    return out.tobytes()


def write_btex(name, rgba_img, size=1024):
    payload = morton_swizzle(rgba_img, size=size)
    import hashlib
    key = int.from_bytes(hashlib.sha1(payload).digest()[:8], "little")
    path = os.path.join(OUT_DIR, name + ".btex")
    with open(path, "wb") as f:
        f.write(struct.pack("<IIIIIIQ", BTEX_MAGIC, 1, size, size, D3D_FMT,
                            len(payload), key))
        f.write(payload)
    print("[OK] BTEX %s (%dx%d, %d bytes)" % (path, size, size, os.path.getsize(path)))
    return path


# ---------------------------------------------------------------------------
# BMSH writer (identical layout to build_special_weapons.py pack_bmsh_v2)
# ---------------------------------------------------------------------------
def pack_bmsh_v2(name, part_defs, nozzle_pos, pilot_pos, eject_pos):
    """part_defs: list of dicts {name,pos(N,3),norm(N,3),bgra(N,),uv(N,2),tris(M,3),pivot(3),rest_offset(3)}"""
    all_pos = np.concatenate([p["pos"] for p in part_defs], axis=0).astype(np.float32)
    bmin, bmax = all_pos.min(axis=0), all_pos.max(axis=0)
    nverts = sum(len(p["pos"]) for p in part_defs)
    nidx = sum(len(p["tris"]) * 3 for p in part_defs)
    assert nverts < 65535, "too many vertices: %d" % nverts
    path = os.path.join(OUT_DIR, name + ".bmsh")
    v_off = 0
    i_off = 0
    with open(path, "wb") as f:
        f.write(struct.pack("<IIIIII", BMSH_MAGIC, 2, nverts, nidx, len(part_defs), 1))
        f.write(struct.pack("<15f", *(list(bmin) + list(bmax) + list(nozzle_pos) +
                                      list(pilot_pos) + list(eject_pos))))
        for p in part_defs:
            pos = np.asarray(p["pos"], np.float32)
            norm = np.asarray(p["norm"], np.float32)
            bgra = np.asarray(p["bgra"], np.uint32)
            uv = np.asarray(p["uv"], np.float32)
            for i in range(len(pos)):
                f.write(struct.pack("<6fI2f", pos[i, 0], pos[i, 1], pos[i, 2],
                                    norm[i, 0], norm[i, 1], norm[i, 2],
                                    int(bgra[i]), uv[i, 0], uv[i, 1]))
        for p in part_defs:
            tris = np.asarray(p["tris"], np.int64) + v_off
            f.write(tris.astype("<u2").tobytes())
            v_off += len(p["pos"])
        for p in part_defs:
            nm = p["name"].encode("utf-8")[:31]
            f.write(struct.pack("<32sII6f", nm, i_off, len(p["tris"]) * 3,
                                *list(p["pivot"]), *list(p["rest_offset"])))
            i_off += len(p["tris"]) * 3
    print("[OK] BMSH %s: %d verts, %d tris, %d parts" % (path, nverts, nidx // 3, len(part_defs)))
    return path


def preserve_part_triangles(pos, uv, nrm_per_vert, bgra_per_vert, tris):
    """Lossless weld on rounded (pos,uv,normal).

    The source GLB carries authored per-vertex NORMAL data on every primitive
    (hard edges, bevels, flat facets). Averaging those into smooth normals
    destroys the shading and makes the weapon look soft/plastic, so the GLB
    normals are passed through verbatim. Only vertices that agree on all three
    of position, UV and normal are merged.
    """
    pos = np.asarray(pos, np.float64)
    uv = np.asarray(uv, np.float64)
    nrm_in = np.asarray(nrm_per_vert, np.float64)
    bgra = np.asarray(bgra_per_vert, np.uint32)
    tris = np.asarray(tris, np.int64)
    if nrm_in.shape[0] != pos.shape[0]:
        nrm_in = np.zeros((pos.shape[0], 3), np.float64)
    keys = [tuple(np.round(pos[i] * 1e5).astype(np.int64)) +
            tuple(np.round(uv[i] * 1e5).astype(np.int64)) +
            tuple(np.round(nrm_in[i] * 1e4).astype(np.int64)) for i in range(len(pos))]
    lut = {}
    new_pos, new_uv, new_n, new_c = [], [], [], []
    remap = np.empty(len(pos), np.int64)
    for i, k in enumerate(keys):
        if k not in lut:
            lut[k] = len(new_pos)
            new_pos.append(pos[i])
            new_uv.append(uv[i])
            new_n.append(nrm_in[i])
            new_c.append(bgra[i])
        remap[i] = lut[k]
    new_tris = remap[tris]
    nrm = np.asarray(new_n, np.float64)
    ln = np.linalg.norm(nrm, axis=1, keepdims=True)
    # Fallback only for primitives that genuinely ship no NORMAL data.
    if np.any(ln < 1e-9):
        acc = np.zeros_like(nrm)
        for t in new_tris:
            a, b, c = new_pos[t[0]], new_pos[t[1]], new_pos[t[2]]
            n = np.cross(b - a, c - a)
            acc[t[0]] += n
            acc[t[1]] += n
            acc[t[2]] += n
        flat = ln.ravel() < 1e-9
        nrm[flat] = acc[flat]
        ln = np.linalg.norm(nrm, axis=1, keepdims=True)
    ln[ln < 1e-12] = 1.0
    nrm = nrm / ln
    return (np.asarray(new_pos, np.float32), nrm.astype(np.float32),
            np.asarray(new_c, np.uint32), np.asarray(new_uv, np.float32),
            new_tris.astype(np.int64))


# ---------------------------------------------------------------------------
# GLB parser
# ---------------------------------------------------------------------------
def parse_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, ver, total = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67:
        raise ValueError("not a GLB: %s" % path)
    jlen, jtype = struct.unpack_from("<II", data, 12)
    gltf = json.loads(data[20:20 + jlen].decode("utf-8"))
    bin_off = 20 + jlen
    blen, btype = struct.unpack_from("<II", data, bin_off)
    return gltf, data[bin_off + 8:bin_off + 8 + blen]


_COMP = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}
_TYPE_N = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}


def read_accessor(gltf, blob, idx):
    acc = gltf["accessors"][idx]
    bv = gltf["bufferViews"][acc["bufferView"]]
    comp = _COMP[acc["componentType"]]
    dt = np.dtype("<" + comp)
    ncomp = _TYPE_N[acc["type"]]
    n = acc["count"]
    off = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride", dt.itemsize * ncomp)
    return np.ndarray(shape=(n, ncomp), dtype=dt, buffer=blob, offset=off,
                      strides=(stride, dt.itemsize)).astype(np.float64)


def quat_rotate(v, q):
    """Rotate vector v by quaternion [x,y,z,w]."""
    x, y, z, w = q
    if abs(x) + abs(y) + abs(z) < 1e-9:
        return v
    u = np.array([x, y, z])
    return v + 2.0 * w * np.cross(u, v) + 2.0 * np.cross(u, np.cross(u, v))


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
PART_MAP = {
    "Receiver_And_Body": "body_stock",
    "Scope_Assembly": "scope_assembly",
    "Trigger_Gatilho": "trigger",
    "Bolt_Carrier": "bolt_body",
    "Bolt_Handle": "bolt_handle",
    "Bolt_Safety_Sleeve": "safety_lever",
    "Stripper_Clip": "stripper_clip",
}
PART_ORDER = list(PART_MAP.keys())

# Per-material albedo source + vertex tint (B,G,R,A) — bgra MODULATES the texture.
# Glass (4/5) has no baseColorTexture in the GLB; it gets its own procedural
# lens tile in region 3 so the scope lens stops showing steel grain.
MAT_REGION = {0: 0, 1: 1, 2: 2, 3: 2, 4: 3, 5: 3}
MAT_TINT = {
    0: (226, 228, 236, 255),   # Ao_-_Bruto_1 blued/parkerized receiver & optic rings
    1: (240, 242, 248, 255),   # Steel_-_Satin brushed steel bolt & hardware
    2: (242, 234, 224, 255),   # Cherry wood forend/grip accents
    3: (245, 236, 225, 255),   # Walnut stock (UV rotated 90)
    4: (200, 205, 215, 255),   # Glass_Red (dark recessed objective lens coat)
    5: (195, 205, 220, 255),   # Glass_Blue (dark recessed ocular lens coat)
}
# Kept as an explicit knob, but pinned to 1.0. Real Kar98k scale relative to
# receiver/grip sockets; viewmodel screen framing is tuned at the camera so
# the receiver & bolt sit close and imposing like stock AK-47.
MODEL_SCALE = 1.0

# 2x2 atlas half-size = 512. Region corners in normalized UV.
REGION_UV = {0: (0.0, 0.0), 1: (0.5, 0.0), 2: (0.0, 0.5), 3: (0.5, 0.5)}


def _load_tile(tex_dir, fname, size=512):
    im = Image.open(os.path.join(tex_dir, fname)).convert("RGB").resize((size, size), Image.LANCZOS)
    return np.asarray(im, dtype=np.float64)


def _tile_2x2(arr, size=512):
    half = size // 2
    im = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB").resize((half, half), Image.LANCZOS)
    sub = np.asarray(im, dtype=np.float64)
    out = np.empty((size, size, 3), dtype=np.float64)
    out[:half, :half] = sub
    out[:half, half:] = sub
    out[half:, :half] = sub
    out[half:, half:] = sub
    return out


def _bake_pbr_metal_tile(base, mrao, nrm, size=512, contrast=1.35, gleam=28.0, warm=(0.94, 0.96, 1.03)):
    """Bake albedo + AO/roughness/metalness + tangent normal relief into lit gunmetal RGBA."""
    ao = mrao[:, :, 0] / 255.0
    rough = np.clip(mrao[:, :, 1] / 255.0, 0.05, 1.0)
    metal = np.clip(mrao[:, :, 2] / 255.0, 0.0, 1.0)
    ao_mean = float(ao.mean()) + 1e-6
    ao_rel = np.clip(0.48 + 0.52 * (ao / ao_mean), 0.30, 1.32)

    # Multi-directional tangent normal relief (primary key light + rim light + micro-cavity)
    nx = (nrm[:, :, 0] - 128.0) / 127.0
    ny = (nrm[:, :, 1] - 128.0) / 127.0
    nz = np.clip((nrm[:, :, 2] - 128.0) / 127.0, 0.1, 1.0)
    ndotl_key = np.clip(nx * (-0.58) + ny * 0.66 + nz * 0.48, 0.0, 1.40)
    ndotl_rim = np.clip(nx * 0.62 + ny * 0.35 + nz * 0.70, 0.0, 1.30)
    # High-frequency normal slope for machined bevels & micro-engravings
    grad_x = np.roll(nx, -1, axis=1) - np.roll(nx, 1, axis=1)
    grad_y = np.roll(ny, -1, axis=0) - np.roll(ny, 1, axis=0)
    micro_curv = np.clip((grad_x - grad_y) * 1.65, -0.38, 0.45)

    relief = 1.0 + (ndotl_key - 0.48) * 3.25 + (ndotl_rim - 0.65) * 0.85 + micro_curv
    relief = np.clip(relief, 0.48, 1.62)

    # Edge wear / brushed satin specular glint where roughness is lower
    r_mean = float(rough.mean())
    polish = np.clip((r_mean - rough) * 5.4 + 0.22 * metal + np.maximum(0.0, micro_curv * 0.9), -0.18, 0.78)
    b_mean = base.mean(axis=(0, 1), keepdims=True)
    enhanced = b_mean + (base - b_mean) * contrast
    lit = enhanced * ao_rel[:, :, None] * relief[:, :, None]
    lit += (polish * gleam)[:, :, None]
    lit[:, :, 0] *= warm[0]
    lit[:, :, 1] *= warm[1]
    lit[:, :, 2] *= warm[2]

    out = np.zeros((size, size, 4), dtype=np.uint8)
    out[:, :, :3] = np.clip(lit, 8.0, 248.0).astype(np.uint8)
    out[:, :, 3] = 255
    return Image.fromarray(out, "RGBA")


def _bake_pbr_wood_tile(wood, grain_map, size=512):
    """Bake Cherry/Walnut diffuse + grain pore map (tex_7) into deep 3D hand-oiled walnut RGBA."""
    pore = grain_map[:, :, 2] / 255.0
    varnish = grain_map[:, :, 1] / 255.0
    p_mean = float(pore.mean()) + 1e-6
    v_mean = float(varnish.mean()) + 1e-6

    # Compute tangent-space pore normal relief from pore heightfield so wood grain has real 3D micro-shadow
    gx = (np.roll(pore, -1, axis=1) - np.roll(pore, 1, axis=1)) * 3.8
    gy = (np.roll(pore, -1, axis=0) - np.roll(pore, 1, axis=0)) * 3.8
    pore_bump = np.clip(1.0 + gx * (-0.55) + gy * 0.65, 0.62, 1.38)

    grain_mod = np.clip(1.0 + (pore - p_mean) * 2.45, 0.28, 1.58) * pore_bump
    sheen_add = np.clip((v_mean - varnish) * 68.0, -24.0, 34.0)

    w_mean = wood.mean(axis=(0, 1), keepdims=True)
    rich = w_mean + (wood - w_mean) * 2.45
    # Authentic German military oiled walnut: deep dark-brown pores with warm amber-burled ridges
    rich[:, :, 0] *= 1.24
    rich[:, :, 1] *= 0.92
    rich[:, :, 2] *= 0.64
    lit = rich * grain_mod[:, :, None] + sheen_add[:, :, None] * np.array([1.15, 0.84, 0.56])[None, None, :]

    out = np.zeros((size, size, 4), dtype=np.uint8)
    out[:, :, :3] = np.clip(lit, 6.0, 232.0).astype(np.uint8)
    out[:, :, 3] = 255
    return Image.fromarray(out, "RGBA")


def bake_vertex_ao(pos, nrm, tris, tints_rgba, mat_ids=None, node_name=""):
    """Multi-scale 3D cavity/curvature AO + inter-part proximity shadows + studio hemisphere relief."""
    n = len(pos)
    acc = np.zeros(n, dtype=np.float64)
    cnt = np.zeros(n, dtype=np.float64)
    u = np.concatenate([tris[:, 0], tris[:, 1], tris[:, 2]])
    v = np.concatenate([tris[:, 1], tris[:, 2], tris[:, 0]])
    d = pos[v] - pos[u]
    dl = np.linalg.norm(d, axis=1) + 1e-9
    du = d / dl[:, None]
    # Concave if neighbor rises in normal direction (+), convex bevel if drops (-).
    conv_u = np.einsum("ij,ij->i", nrm[u], du)
    conv_v = np.einsum("ij,ij->i", nrm[v], -du)
    np.add.at(acc, u, conv_u)
    np.add.at(cnt, u, 1.0)
    np.add.at(acc, v, conv_v)
    np.add.at(cnt, v, 1.0)
    mean_conv = acc / np.maximum(cnt, 1.0)

    # Fine 1-ring bevel/crease + broader 4-ring cavity occlusion
    s_fine = mean_conv.copy()
    s_broad = mean_conv.copy()
    for p_idx in range(4):
        s_acc = np.zeros(n, dtype=np.float64)
        np.add.at(s_acc, u, s_broad[v])
        np.add.at(s_acc, v, s_broad[u])
        s_broad = 0.5 * s_broad + 0.5 * (s_acc / np.maximum(cnt, 1.0))
        if p_idx == 0:
            s_fine = 0.65 * s_fine + 0.35 * (s_acc / np.maximum(cnt, 1.0))

    # Concave crevices darken deeply (down to 0.34); sharp convex machined edges gleam (up to 1.24)
    curv_ao = np.clip(1.0 - s_fine * 0.95 - s_broad * 0.55, 0.34, 1.24)

    # Directional 3D hemisphere + rim sculpting in D3D space (X_d3d=right, Y_d3d=up, Z_d3d=fwd)
    nx, ny, nz = nrm[:, 0], nrm[:, 1], nrm[:, 2]
    px, py, pz = pos[:, 0], pos[:, 1], pos[:, 2]
    # Top sky light (+Y) + left camera-side key light (-X_d3d) + subtle rear-top rim (-Z_d3d, +Y_d3d)
    hemi = 0.84 + 0.18 * ny - 0.08 * nx + 0.06 * np.clip(-0.55 * nx + 0.65 * ny - 0.52 * nz, 0.0, 1.0)
    # Darken downward-facing underbellies so the stock & barrel have strong 3D cylindrical volume
    hemi = np.where(ny < -0.15, hemi * (0.82 + 0.18 * (1.0 + ny)), hemi)

    # Inter-part proximity & contact shadows on Receiver_And_Body
    prox = np.ones(n, dtype=np.float64)
    if node_name == "Receiver_And_Body":
        # 1. Scope tube & dual mount rings cast contact shadow onto top of receiver/stock (Z_d3d in [0.36, 0.59])
        under_scope = (pz >= 0.365) & (pz <= 0.585) & (py >= 0.022) & (py <= 0.052) & (np.abs(px) <= 0.013)
        ring_dist = np.minimum(np.abs(pz - 0.415), np.abs(pz - 0.535))
        scope_occ = np.where(ring_dist < 0.024, 0.54, 0.72)
        prox = np.where(under_scope & (ny > 0.15), prox * scope_occ, prox)

        # 2. Wood-to-steel bedding channel shadow along top edge of walnut stock (py in [0.018, 0.035])
        if mat_ids is not None:
            is_wood = (mat_ids == 2) | (mat_ids == 3)
            bedding_lip = is_wood & (py >= 0.018) & (py <= 0.036) & (pz >= 0.25) & (pz <= 0.78)
            prox = np.where(bedding_lip & (ny > 0.25), prox * 0.74, prox)
            # Subtle checkering & palm shadow on the pistol grip neck (pz in [0.16, 0.24], py < 0.005)
            grip_check = is_wood & (pz >= 0.165) & (pz <= 0.238) & (py <= 0.008) & (np.abs(px) >= 0.005)
            check_pat = 0.90 + 0.10 * np.sin(pz * 620.0 + py * 620.0) * np.sin(pz * 620.0 - py * 620.0)
            prox = np.where(grip_check, prox * check_pat, prox)

        # 3. Bolt handle shadow on right cheek of stock (pz in [0.275, 0.315], px > 0.006)
        bolt_shadow = (pz >= 0.275) & (pz <= 0.315) & (py >= -0.005) & (py <= 0.030) & (px >= 0.005)
        prox = np.where(bolt_shadow, prox * 0.68, prox)

    elif node_name == "Scope_Assembly":
        # Darken the inside of the front/rear scope hoods and underside of the scope tube
        prox = np.where(ny < -0.25, prox * 0.72, prox)
        if mat_ids is not None:
            is_glass = (mat_ids == 4) | (mat_ids == 5)
            # Keep scope glass dark and recessed, never blown out
            prox = np.where(is_glass, 0.78, prox)

    total_mod = np.clip(curv_ao * hemi * prox, 0.24, 1.22)
    r = np.clip(tints_rgba[:, 0] * total_mod, 8, 255).astype(np.uint32)
    g = np.clip(tints_rgba[:, 1] * total_mod, 8, 255).astype(np.uint32)
    b = np.clip(tints_rgba[:, 2] * total_mod, 8, 255).astype(np.uint32)
    a = np.clip(tints_rgba[:, 3], 0, 255).astype(np.uint32)
    return (a << 24) | (r << 16) | (g << 8) | b


def pingpong01(x):
    """Continuous triangle-wave wrap in [0, 1] so atlas triangles never jump across wrap seams."""
    m2 = np.mod(x, 2.0)
    return np.where(m2 <= 1.0, m2, 2.0 - m2)


def _lens_tile(size=512):
    """Realistic dark recessed ZF39 optical scope lens with subtle multi-coated crescent reflection.

    Replaces the bright cyan/white disc with a deep dark optical cavity (#090E16),
    a dark matte-anodized inner retaining ring, and a restrained off-axis Carl Zeiss
    sapphire/emerald anti-reflective crescent reflection.
    """
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float64)
    u = (xx + 0.5) / size * 2.0 - 1.0
    v = (yy + 0.5) / size * 2.0 - 1.0
    r = np.sqrt(u * u + v * v)

    # Dark anodized retaining bezel outside r > 0.84
    bezel = np.clip((r - 0.82) / 0.08, 0.0, 1.0)
    # Deep recessed optical glass cavity inside r <= 0.84
    cavity_vig = np.clip(1.0 - 0.45 * (r / 0.85) ** 2, 0.25, 1.0)

    # Subtle off-axis crescent reflection in upper-left quadrant
    du = u + 0.28
    dv = v + 0.26
    dr = np.sqrt(du * du + dv * dv)
    crescent = np.exp(-((dr - 0.34) / 0.11) ** 2) * np.clip((-u - v) * 0.85 + 0.2, 0.0, 1.0)
    crescent *= (1.0 - bezel)

    # Tiny secondary pinpoint reflection in lower-right
    p2 = np.exp(-(((u - 0.32) ** 2 + (v - 0.30) ** 2) / (0.06 ** 2))) * (1.0 - bezel) * 0.45

    # Base deep optical glass: RGB ~(12, 18, 26), crescent adds subtle sapphire/teal glint
    rr = (12.0 * cavity_vig + 38.0 * crescent + 48.0 * p2) * (1.0 - bezel) + 22.0 * bezel
    gg = (18.0 * cavity_vig + 74.0 * crescent + 62.0 * p2) * (1.0 - bezel) + 24.0 * bezel
    bb = (28.0 * cavity_vig + 115.0 * crescent + 78.0 * p2) * (1.0 - bezel) + 27.0 * bezel

    out = np.zeros((size, size, 4), np.uint8)
    out[..., 0] = np.clip(rr, 6, 180)
    out[..., 1] = np.clip(gg, 8, 195)
    out[..., 2] = np.clip(bb, 10, 215)
    out[..., 3] = 255
    return Image.fromarray(out, "RGBA")


def build_pbr_tiles(size=512):
    """Build all 4 PBR material tiles at requested resolution."""
    tex_dir = os.path.join(PACK_ROOT, "textures")
    t0 = _load_tile(tex_dir, "kar98_tex_0.jpg", size)
    t1 = _load_tile(tex_dir, "kar98_tex_1.jpg", size)
    t2 = _load_tile(tex_dir, "kar98_tex_2.jpg", size)
    r0 = _bake_pbr_metal_tile(t0, t1, t2, size, contrast=1.85, gleam=30.0, warm=(0.89, 0.94, 1.07))

    t3 = _load_tile(tex_dir, "kar98_tex_3.jpg", size)
    t4 = _load_tile(tex_dir, "kar98_tex_4.jpg", size)
    t5 = _tile_2x2(_load_tile(tex_dir, "kar98_tex_5.jpg", size), size)
    r1 = _bake_pbr_metal_tile(t3 * 1.45, t4, t5, size, contrast=2.05, gleam=52.0, warm=(1.02, 1.02, 1.04))

    t6 = _load_tile(tex_dir, "kar98_tex_6.jpg", size)
    t7 = _load_tile(tex_dir, "kar98_tex_7.jpg", size)
    r2 = _bake_pbr_wood_tile(t6, t7, size)

    r3 = _lens_tile(min(size, 512))
    return r0, r1, r2, r3


def build_atlas():
    atlas = Image.new("RGBA", (1024, 1024), (16, 16, 18, 255))
    r0, r1, r2, r3 = build_pbr_tiles(512)
    atlas.paste(r0, (0, 0))
    atlas.paste(r1, (512, 0))
    atlas.paste(r2, (0, 512))
    atlas.paste(r3.resize((512, 512), Image.LANCZOS), (512, 512))
    return atlas


def write_mmat(name, ranges):
    """Write binary material batch list: magic='MMAT', uint32 count, {part_idx, mat_id, first_index, index_count}."""
    path = os.path.join(OUT_DIR, name + ".mmat")
    with open(path, "wb") as f:
        f.write(struct.pack("<II", 0x54414D4D, len(ranges)))
        for p_idx, m_id, first_i, cnt_i in ranges:
            f.write(struct.pack("<IIII", int(p_idx), int(m_id), int(first_i), int(cnt_i)))
    print("[OK] MMAT %s (%d batches)" % (path, len(ranges)))
    return path


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    glb_path = os.path.join(PACK_ROOT, "model", "kar_98_lod0.glb")
    gltf, blob = parse_glb(glb_path)
    nodes = gltf["nodes"]
    root = nodes[10]
    root_t = np.array(root.get("translation", [0, 0, 0]), np.float64)
    node_lut = {n["name"]: i for i, n in enumerate(nodes)}

    with io.open(os.path.join(PACK_ROOT, "animation", "bolt_cycle.json"), "r", encoding="utf-8") as f:
        pivots_pack = json.load(f)["pivots_m"]

    def to_d3d(v):
        return np.array([v[2], v[1], v[0]], np.float64)

    part_defs = []
    hd_part_defs = []
    hd_mmat_ranges = []
    hd_global_idx_off = 0
    total_raw = 0
    for part_i, node_name in enumerate(PART_ORDER):
        ni = node_lut[node_name]
        node = nodes[ni]
        mesh = gltf["meshes"][node["mesh"]]
        nt = np.array(node.get("translation", [0, 0, 0]), np.float64)
        ns = np.array(node.get("scale", [1, 1, 1]), np.float64)
        nq = node.get("rotation", [0, 0, 0, 1])
        # Stripper clip: ignore the pack's 0.001 rest scale; full-size rest geometry.
        if node_name == "Stripper_Clip":
            ns = np.ones(3, np.float64)
        world_off = nt + root_t

        pos_chunks, nrm_chunks, tri_chunks = [], [], []
        atlas_uv_chunks, hd_uv_chunks, tint_rgba_chunks, reg_chunks, vmat_chunks = [], [], [], [], []
        base = 0
        for prim in mesh["primitives"]:
            p = read_accessor(gltf, blob, prim["attributes"]["POSITION"])
            uv = read_accessor(gltf, blob, prim["attributes"]["TEXCOORD_0"])
            if "NORMAL" in prim["attributes"]:
                nv = read_accessor(gltf, blob, prim["attributes"]["NORMAL"])
            else:
                nv = np.zeros((len(p), 3), np.float64)
            idx = read_accessor(gltf, blob, prim["indices"]).ravel().astype(np.int64)
            mat = prim.get("material", 0)
            region = MAT_REGION.get(mat, 0)
            tint = MAT_TINT.get(mat, (230, 230, 235, 255))
            rx, ry = REGION_UV[region]
            w = p * ns
            w = quat_rotate(w, nq)
            w = (w + world_off) * MODEL_SCALE
            nrot = quat_rotate(nv, nq)

            # Fix internal sear z-fighting on Trigger_Gatilho against wooden stock sides
            if node_name == "Trigger_Gatilho":
                upper_sear = w[:, 1] > -0.002
                w[upper_sear, 2] = np.clip(w[upper_sear, 2], -0.0038, 0.0038)

            d3d = np.column_stack((w[:, 2], w[:, 1], w[:, 0])).astype(np.float32)
            nd3d = np.column_stack((nrot[:, 2], nrot[:, 1], nrot[:, 0])).astype(np.float32)

            # Continuous unwrapped UVs for HD multi-material hardware D3DTADDRESS_WRAP
            uu_raw = uv[:, 0] * 0.1
            vv_raw = 1.0 - (uv[:, 1] * 0.1)
            if mat in (2, 3):
                # Fix planar UV stretching on top/bottom stock bevels (|ny| > 0.35) by blending Z_pack
                top_w = np.clip((np.abs(nrot[:, 1]) - 0.35) / 0.45, 0.0, 1.0)
                vv_box = 1.0 - ((w[:, 1] + w[:, 2] * 1.35 * np.sign(nrot[:, 1] + 1e-6)) * 2.8)
                vv_raw = vv_raw * (1.0 - top_w) + vv_box * top_w
            if mat == 3:
                uu_raw, vv_raw = vv_raw, 1.0 - uu_raw

            # Seamless triangle-wave mirror UVs for 2x2 fallback atlas
            uu_atl = pingpong01(uu_raw)
            vv_atl = pingpong01(vv_raw)
            au = (rx + uu_atl * 0.5).astype(np.float32)
            av = (ry + vv_atl * 0.5).astype(np.float32)

            t_rgba = np.tile(np.array(tint, dtype=np.float64), (len(p), 1))
            pos_chunks.append(d3d)
            nrm_chunks.append(nd3d)
            atlas_uv_chunks.append(np.column_stack((au, av)).astype(np.float32))
            hd_uv_chunks.append(np.column_stack((uu_raw, vv_raw)).astype(np.float32))
            tint_rgba_chunks.append(t_rgba)
            vmat_chunks.append(np.full(len(p), mat, dtype=np.int32))
            reg_chunks.append(np.full(len(idx) // 3, region, dtype=np.int32))
            tri_chunks.append(idx + base)
            base += len(p)

        pos = np.concatenate(pos_chunks, axis=0)
        nrm = np.concatenate(nrm_chunks, axis=0)
        atl_uv = np.concatenate(atlas_uv_chunks, axis=0)
        hd_uv = np.concatenate(hd_uv_chunks, axis=0)
        tints = np.concatenate(tint_rgba_chunks, axis=0)
        vert_mats = np.concatenate(vmat_chunks, axis=0)
        tri_reg = np.concatenate(reg_chunks, axis=0)
        tris = np.concatenate(tri_chunks, axis=0).reshape(-1, 3)
        total_raw += len(pos)

        # Bake rich 3D multi-scale cavity/bevel AO + inter-part proximity shadows into vertex BGRA
        ao_bgra = bake_vertex_ao(pos, nrm, tris, tints, vert_mats, node_name)

        # Fallback atlas mesh (60,000 tris, zero wrap jumps via pingpong)
        np_, nn, nc, nu, nt2 = preserve_part_triangles(pos, atl_uv, nrm, ao_bgra, tris)
        pkey = "Weapon_Root" if PART_MAP[node_name] == "body_stock" else node_name
        if pkey not in pivots_pack:
            pkey = node_name
        pivot = to_d3d(pivots_pack[pkey]) * MODEL_SCALE
        part_defs.append({
            "name": PART_MAP[node_name],
            "pos": np_, "norm": nn, "bgra": nc, "uv": nu, "tris": nt2,
            "pivot": pivot, "rest_offset": np.zeros(3),
        })

        # HD mesh: group triangles by material ID + bake per-vertex geometric cavity/edge AO
        hp, hn, hc, hu, ht = preserve_part_triangles(pos, hd_uv, nrm, ao_bgra, tris)
        # Reorder ht by tri_reg (0..3) and record MMAT batches
        order = np.argsort(tri_reg, kind="stable")
        ht_sorted = ht[order]
        reg_sorted = tri_reg[order]
        part_idx_cur = hd_global_idx_off
        for m_id in range(4):
            cnt_tris = int(np.sum(reg_sorted == m_id))
            if cnt_tris > 0:
                hd_mmat_ranges.append((part_i, m_id, part_idx_cur, cnt_tris * 3))
                part_idx_cur += cnt_tris * 3
        hd_global_idx_off = part_idx_cur
        hd_part_defs.append({
            "name": PART_MAP[node_name],
            "pos": hp, "norm": hn, "bgra": hc, "uv": hu, "tris": ht_sorted,
            "pivot": pivot, "rest_offset": np.zeros(3),
        })
        print("  %-22s -> %-16s %5d raw -> %5d welded / %5d HD, %5d tris" %
              (node_name, PART_MAP[node_name], len(pos), len(np_), len(hp), len(ht_sorted)))

    nozzle = to_d3d([0.85, 0.039, 0.0]) * MODEL_SCALE
    eject = to_d3d([0.34, 0.045, 0.01]) * MODEL_SCALE
    pilot = to_d3d([0.50, 0.0, 0.0]) * MODEL_SCALE

    pack_bmsh_v2("mauser_1891_12", part_defs, nozzle, pilot, eject)
    write_btex("mauser_1891_12", build_atlas(), size=1024)

    pack_bmsh_v2("mauser_1891_12_hd", hd_part_defs, nozzle, pilot, eject)
    write_mmat("mauser_1891_12_hd", hd_mmat_ranges)
    m0, m1, m2, m3 = build_pbr_tiles(1024)
    write_btex("mauser_1891_12_m0", m0, size=1024)
    write_btex("mauser_1891_12_m1", m1, size=1024)
    write_btex("mauser_1891_12_m2", m2, size=1024)
    write_btex("mauser_1891_12_m3", m3, size=512)

    reticle_path = os.path.join(PACK_ROOT, "ui", "sniper_scope_reticle_1024.png")
    if os.path.isfile(reticle_path):
        ret_img = Image.open(reticle_path).convert("RGBA").resize((1024, 1024), Image.LANCZOS)
        write_btex("sniper_reticle", ret_img, size=1024)
    print("DONE: raw=%d, welded=%d" % (total_raw, sum(len(p["pos"]) for p in part_defs)))


if __name__ == "__main__":
    main()
