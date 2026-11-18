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
    0: (224, 226, 232, 255),   # Ao_-_Bruto_1 blued/parkerized receiver & optic rings
    1: (238, 240, 245, 255),   # Steel_-_Satin brushed steel bolt & hardware
    2: (242, 232, 220, 255),   # Cherry wood forend/grip accents
    3: (245, 235, 222, 255),   # Walnut stock (UV rotated 90)
    4: (160, 185, 220, 220),   # Glass_Red (ruby-violet objective lens coat)
    5: (155, 190, 225, 220),   # Glass_Blue (sapphire ocular lens coat)
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
    ao_rel = np.clip(0.55 + 0.45 * (ao / ao_mean), 0.38, 1.25)

    # Tangent normal relief under key-light dir (-0.52, 0.68, 0.52)
    nx = (nrm[:, :, 0] - 128.0) / 127.0
    ny = (nrm[:, :, 1] - 128.0) / 127.0
    nz = np.clip((nrm[:, :, 2] - 128.0) / 127.0, 0.1, 1.0)
    ndotl = np.clip(nx * (-0.55) + ny * 0.65 + nz * 0.52, 0.0, 1.35)
    # Normalize around resting flat tangent dot (0.52) and amplify crisp micro-relief
    relief = 1.0 + (ndotl - 0.52) * 2.8
    relief = np.clip(relief, 0.62, 1.48)

    # Edge wear / brushed satin specular glint where roughness is lower
    r_mean = float(rough.mean())
    polish = np.clip((r_mean - rough) * 4.5 + 0.18 * metal, -0.15, 0.65)
    # Local contrast enhancement on albedo luminance around mean
    b_mean = base.mean(axis=(0, 1), keepdims=True)
    enhanced = b_mean + (base - b_mean) * contrast
    lit = enhanced * ao_rel[:, :, None] * relief[:, :, None]
    lit += (polish * gleam)[:, :, None]
    lit[:, :, 0] *= warm[0]
    lit[:, :, 1] *= warm[1]
    lit[:, :, 2] *= warm[2]

    out = np.zeros((size, size, 4), dtype=np.uint8)
    out[:, :, :3] = np.clip(lit, 10.0, 245.0).astype(np.uint8)
    out[:, :, 3] = 255
    return Image.fromarray(out, "RGBA")


def _bake_pbr_wood_tile(wood, grain_map, size=512):
    """Bake Cherry/Walnut diffuse + grain pore map (tex_7) into deep oiled walnut RGBA."""
    pore = grain_map[:, :, 2] / 255.0
    varnish = grain_map[:, :, 1] / 255.0
    p_mean = float(pore.mean()) + 1e-6
    v_mean = float(varnish.mean()) + 1e-6
    grain_mod = np.clip(1.0 + (pore - p_mean) * 1.95, 0.36, 1.52)
    sheen_add = np.clip((v_mean - varnish) * 58.0, -18.0, 28.0)

    w_mean = wood.mean(axis=(0, 1), keepdims=True)
    rich = w_mean + (wood - w_mean) * 2.15
    # Rich Mauser oiled walnut saturation & warm grain contrast
    rich[:, :, 0] *= 1.38
    rich[:, :, 1] *= 1.08
    rich[:, :, 2] *= 0.80
    lit = rich * grain_mod[:, :, None] + sheen_add[:, :, None] * np.array([1.18, 0.90, 0.64])[None, None, :]

    out = np.zeros((size, size, 4), dtype=np.uint8)
    out[:, :, :3] = np.clip(lit, 8.0, 236.0).astype(np.uint8)
    out[:, :, 3] = 255
    return Image.fromarray(out, "RGBA")


def bake_vertex_ao(pos, nrm, tris, tints_rgba):
    """Fast local cavity/edge AO from 1-ring edge convexity, modulated into vertex BGRA."""
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
    # Two Jacobi passes on welded neighborhoods to keep creases crisp without noise
    s = mean_conv.copy()
    for _ in range(2):
        s_acc = np.zeros(n, dtype=np.float64)
        np.add.at(s_acc, u, s[v])
        np.add.at(s_acc, v, s[u])
        s = 0.6 * s + 0.4 * (s_acc / np.maximum(cnt, 1.0))
    ao = np.clip(1.0 - s * 0.72, 0.56, 1.12)
    r = np.clip(tints_rgba[:, 0] * ao, 10, 255).astype(np.uint32)
    g = np.clip(tints_rgba[:, 1] * ao, 10, 255).astype(np.uint32)
    b = np.clip(tints_rgba[:, 2] * ao, 10, 255).astype(np.uint32)
    a = np.clip(tints_rgba[:, 3], 0, 255).astype(np.uint32)
    return (a << 24) | (r << 16) | (g << 8) | b


def pingpong01(x):
    """Continuous triangle-wave wrap in [0, 1] so atlas triangles never jump across wrap seams."""
    m2 = np.mod(x, 2.0)
    return np.where(m2 <= 1.0, m2, 2.0 - m2)


def _lens_tile(size=512):
    """Procedural rifle-scope objective lens.

    The GLB glass materials ship no baseColorTexture, and the viewmodel pass has
    alpha blending disabled, so the lens has to read as an opaque coated disc:
    deep bluish coating, bright centre glow, a broad diagonal specular streak and
    a soft anti-aliased rim.
    """
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float64)
    u = (xx + 0.5) / size * 2.0 - 1.0
    v = (yy + 0.5) / size * 2.0 - 1.0
    r = np.sqrt(u * u + v * v)
    coating = np.clip(1.0 - 0.55 * np.power(r, 1.6), 0.0, 1.0)
    glow = np.clip(1.0 - r, 0.0, 1.0) ** 2.2
    streak = np.clip(1.0 - np.abs((u * 0.72 + v * 0.69) + 0.18) * 3.4, 0.0, 1.0) ** 1.6
    streak *= np.clip(1.0 - r * 0.85, 0.0, 1.0)
    edge = np.clip((0.985 - r) * 26.0, 0.0, 1.0)
    dim = 0.35 + 0.65 * coating
    rr = (38.0 + 126.0 * glow + 96.0 * streak) * edge * dim
    gg = (52.0 + 150.0 * glow + 92.0 * streak) * edge * dim
    bb = (72.0 + 158.0 * glow + 88.0 * streak) * edge * dim
    out = np.zeros((size, size, 4), np.uint8)
    out[..., 0] = np.clip(rr, 0, 255)
    out[..., 1] = np.clip(gg, 0, 255)
    out[..., 2] = np.clip(bb, 0, 255)
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
        atlas_uv_chunks, hd_uv_chunks, tint_rgba_chunks, reg_chunks = [], [], [], []
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
            d3d = np.column_stack((w[:, 2], w[:, 1], w[:, 0])).astype(np.float32)
            nrot = quat_rotate(nv, nq)
            nd3d = np.column_stack((nrot[:, 2], nrot[:, 1], nrot[:, 0])).astype(np.float32)

            # Continuous unwrapped UVs for HD multi-material hardware D3DTADDRESS_WRAP
            uu_raw = uv[:, 0] * 0.1
            vv_raw = 1.0 - (uv[:, 1] * 0.1)
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
            reg_chunks.append(np.full(len(idx) // 3, region, dtype=np.int32))
            tri_chunks.append(idx + base)
            base += len(p)

        pos = np.concatenate(pos_chunks, axis=0)
        nrm = np.concatenate(nrm_chunks, axis=0)
        atl_uv = np.concatenate(atlas_uv_chunks, axis=0)
        hd_uv = np.concatenate(hd_uv_chunks, axis=0)
        tints = np.concatenate(tint_rgba_chunks, axis=0)
        tri_reg = np.concatenate(reg_chunks, axis=0)
        tris = np.concatenate(tri_chunks, axis=0).reshape(-1, 3)
        total_raw += len(pos)

        # Fallback atlas mesh (60,000 tris, zero wrap jumps via pingpong)
        raw_bgra = np.zeros(len(pos), dtype=np.uint32)
        t_u32 = tints.astype(np.uint32)
        raw_bgra = (t_u32[:, 3] << 24) | (t_u32[:, 0] << 16) | (t_u32[:, 1] << 8) | t_u32[:, 2]
        np_, nn, nc, nu, nt2 = preserve_part_triangles(pos, atl_uv, nrm, raw_bgra, tris)
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
        ao_bgra = bake_vertex_ao(pos, nrm, tris, tints)
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
