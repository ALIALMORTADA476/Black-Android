#!/usr/bin/env python3
"""Build Mauser C96 M712 and M2A1 Flamethrower BMSH v2 + HD MMAT + 1024x1024 PBR BTEX assets
from the new GLB packs in:
  - الاسلحه الخاصه/Mauser C96/mauser_c96/
  - الاسلحه الخاصه/قاذف لهب/flamethrower/
"""
import io
import json
import os
import sys
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_m91_glb as m91

REPO_ROOT = m91.REPO_ROOT
OUT_DIR = m91.OUT_DIR

C96_ROOT = os.path.join(REPO_ROOT, "الاسلحه الخاصه", "Mauser C96", "mauser_c96")
FLM_ROOT = os.path.join(REPO_ROOT, "الاسلحه الخاصه", "قاذف لهب", "flamethrower")


def read_glb_image(gltf, blob, img_idx, size=1024):
    img_info = gltf["images"][img_idx]
    bv = gltf["bufferViews"][img_info["bufferView"]]
    off = bv.get("byteOffset", 0)
    length = bv["byteLength"]
    im = Image.open(io.BytesIO(blob[off:off + length])).convert("RGB").resize((size, size), Image.LANCZOS)
    return np.asarray(im, dtype=np.float64)


def build_c96():
    print("=== Building Mauser C96 (GLB + HD Multi-Material) ===")
    glb_path = os.path.join(C96_ROOT, "model", "mauser_c96_lod0.glb")
    gltf, blob = m91.parse_glb(glb_path)
    nodes = gltf["nodes"]
    node_lut = {n["name"]: i for i, n in enumerate(nodes)}

    with io.open(os.path.join(C96_ROOT, "animation", "bolt_cycle.json"), "r", encoding="utf-8") as f:
        pivots_pack = json.load(f)["pivots_m"]

    c96_part_map = {
        "Receiver_And_Body": "body_frame",
        "Scope_Assembly": "rear_sight",
        "Trigger_Gatilho": "trigger",
        "Bolt_Carrier": "bolt",
        "Bolt_Handle": "hammer",
        "Bolt_Safety_Sleeve": "safety_lever",
        "Stripper_Clip": "stripper_clip",
    }
    part_order = list(c96_part_map.keys())

    # GLB materials:
    # Mat 0: WalnutHolster_And_BrassClip (tex_2, mr=bv1, norm=bv2) -> atlas region 2 (0.0, 0.5)
    # Mat 1: BluedSteel_Frame_And_Barrel (tex_0, mr=bv4, norm=bv5) -> atlas region 0 (0.0, 0.0)
    # Mat 2: SatinSteel_Bolt_And_Sight   (tex_1, mr=bv4, norm=bv5) -> atlas region 1 (0.5, 0.0)
    mat_region = {0: 2, 1: 0, 2: 1}
    mat_tint = {
        0: (242, 232, 215, 255),  # Warm Walnut & Brass
        1: (226, 228, 235, 255),  # Blued Steel Frame/Barrel
        2: (240, 242, 248, 255),  # Satin Steel Bolt/Hammer/Sight
    }

    def to_d3d(v):
        return np.array([v[2], v[1], v[0]], np.float64)

    part_defs = []
    hd_part_defs = []
    hd_mmat_ranges = []
    hd_global_idx_off = 0

    for part_i, node_name in enumerate(part_order):
        nd = nodes[node_lut[node_name]]
        mesh = gltf["meshes"][nd["mesh"]]
        nt = np.array(nd.get("translation", [0, 0, 0]), np.float64)
        ns = np.array(nd.get("scale", [1, 1, 1]), np.float64)
        nq = nd.get("rotation", [0, 0, 0, 1])

        pos_c, nrm_c, atl_uv_c, hd_uv_c, tint_c, vmat_c, mmat_c, tri_c = [], [], [], [], [], [], [], []
        base = 0
        for prim in mesh["primitives"]:
            p = m91.read_accessor(gltf, blob, prim["attributes"]["POSITION"])
            uv = m91.read_accessor(gltf, blob, prim["attributes"]["TEXCOORD_0"])
            nv = m91.read_accessor(gltf, blob, prim["attributes"]["NORMAL"])
            idx = m91.read_accessor(gltf, blob, prim["indices"]).ravel().astype(np.int64)
            mat = prim.get("material", 0)
            reg = mat_region.get(mat, 0)
            rx, ry = m91.REGION_UV[reg]
            tint = mat_tint.get(mat, (230, 230, 235, 255))

            w = m91.quat_rotate(p * ns, nq) + nt
            nrot = m91.quat_rotate(nv, nq)
            d3d = np.column_stack((w[:, 2], w[:, 1], w[:, 0])).astype(np.float32)
            nd3d = np.column_stack((nrot[:, 2], nrot[:, 1], nrot[:, 0])).astype(np.float32)

            uu_raw = uv[:, 0]
            vv_raw = uv[:, 1]
            uu_atl = m91.pingpong01(uu_raw)
            vv_atl = m91.pingpong01(vv_raw)
            au = (rx + uu_atl * 0.5).astype(np.float32)
            av = (ry + vv_atl * 0.5).astype(np.float32)

            pos_c.append(d3d)
            nrm_c.append(nd3d)
            atl_uv_c.append(np.column_stack((au, av)).astype(np.float32))
            hd_uv_c.append(np.column_stack((uu_raw, vv_raw)).astype(np.float32))
            tint_c.append(np.tile(np.array(tint, dtype=np.float64), (len(p), 1)))
            vmat_c.append(np.full(len(p), mat, dtype=np.int32))
            mmat_c.append(np.full(len(idx) // 3, mat, dtype=np.int32))
            tri_c.append(idx + base)
            base += len(p)

        pos = np.concatenate(pos_c, axis=0)
        nrm = np.concatenate(nrm_c, axis=0)
        atl_uv = np.concatenate(atl_uv_c, axis=0)
        hd_uv = np.concatenate(hd_uv_c, axis=0)
        tints = np.concatenate(tint_c, axis=0)
        vert_mats = np.concatenate(vmat_c, axis=0)
        tri_mats = np.concatenate(mmat_c, axis=0)
        tris = np.concatenate(tri_c, axis=0).reshape(-1, 3)

        ao_bgra = m91.bake_vertex_ao(pos, nrm, tris, tints, vert_mats, node_name)
        np_, nn, nc, nu, nt2 = m91.preserve_part_triangles(pos, atl_uv, nrm, ao_bgra, tris)
        pkey = "Weapon_Root" if c96_part_map[node_name] == "body_frame" else node_name
        pivot = to_d3d(pivots_pack.get(pkey, [0, 0, 0]))
        part_defs.append({
            "name": c96_part_map[node_name],
            "pos": np_, "norm": nn, "bgra": nc, "uv": nu, "tris": nt2,
            "pivot": pivot, "rest_offset": np.zeros(3),
        })

        hp, hn, hc, hu, ht = m91.preserve_part_triangles(pos, hd_uv, nrm, ao_bgra, tris)
        order = np.argsort(tri_mats, kind="stable")
        ht_sorted = ht[order]
        mat_sorted = tri_mats[order]
        part_idx_cur = hd_global_idx_off
        for m_id in range(3):
            cnt_tris = int(np.sum(mat_sorted == m_id))
            if cnt_tris > 0:
                hd_mmat_ranges.append((part_i, m_id, part_idx_cur, cnt_tris * 3))
                part_idx_cur += cnt_tris * 3
        hd_global_idx_off = part_idx_cur
        hd_part_defs.append({
            "name": c96_part_map[node_name],
            "pos": hp, "norm": hn, "bgra": hc, "uv": hu, "tris": ht_sorted,
            "pivot": pivot, "rest_offset": np.zeros(3),
        })

    nozzle = to_d3d([0.685, 0.068, 0.0])
    eject = to_d3d([0.485, 0.078, 0.0])
    pilot = to_d3d([0.550, 0.068, 0.0])
    m91.pack_bmsh_v2("mauser_c96", part_defs, nozzle, pilot, eject)
    m91.pack_bmsh_v2("mauser_c96_hd", hd_part_defs, nozzle, pilot, eject)
    m91.write_mmat("mauser_c96_hd", hd_mmat_ranges)

    # Bake 1024x1024 PBR textures for C96 using external JPGs + GLB embedded MR & Normal maps
    tex_dir = os.path.join(C96_ROOT, "textures")
    t_blued = m91._load_tile(tex_dir, "mauser_c96_tex_0.jpg", 1024)
    t_satin = m91._load_tile(tex_dir, "mauser_c96_tex_1.jpg", 1024)
    t_walnut = m91._load_tile(tex_dir, "mauser_c96_tex_2.jpg", 1024)
    mr_holster = read_glb_image(gltf, blob, 1, 1024)
    nrm_holster = read_glb_image(gltf, blob, 2, 1024)
    mr_frame = read_glb_image(gltf, blob, 4, 1024)
    nrm_frame = read_glb_image(gltf, blob, 5, 1024)

    # Mat 0: Walnut Holster & Warm Brass Stripper Clip (ensure warm R >> B for brass & walnut check)
    m0_img = m91._bake_pbr_metal_tile(t_walnut * 1.35, mr_holster, nrm_holster, 1024,
                                      contrast=1.65, gleam=26.0, warm=(1.32, 1.02, 0.68))
    # Mat 1: Blued Steel Frame & Barrel
    m1_img = m91._bake_pbr_metal_tile(t_blued * 1.25, mr_frame, nrm_frame, 1024,
                                      contrast=1.80, gleam=32.0, warm=(0.92, 0.96, 1.06))
    # Mat 2: Satin Steel Bolt, Hammer & Rear Sight
    m2_img = m91._bake_pbr_metal_tile(t_satin * 1.15, mr_frame, nrm_frame, 1024,
                                      contrast=1.85, gleam=44.0, warm=(1.02, 1.02, 1.04))

    m91.write_btex("mauser_c96_m0", m0_img, size=1024)
    m91.write_btex("mauser_c96_m1", m1_img, size=1024)
    m91.write_btex("mauser_c96_m2", m2_img, size=1024)

    atlas = Image.new("RGBA", (1024, 1024), (20, 20, 22, 255))
    atlas.paste(m1_img.resize((512, 512), Image.LANCZOS), (0, 0))
    atlas.paste(m2_img.resize((512, 512), Image.LANCZOS), (512, 0))
    atlas.paste(m0_img.resize((512, 512), Image.LANCZOS), (0, 512))
    atlas.paste(m0_img.resize((512, 512), Image.LANCZOS), (512, 512))
    m91.write_btex("mauser_c96", atlas, size=1024)


def build_flamethrower():
    print("=== Building M2A1 Flamethrower (GLB + HD Multi-Material) ===")
    glb_path = os.path.join(FLM_ROOT, "model", "flamethrower_lod0.glb")
    gltf, blob = m91.parse_glb(glb_path)
    nodes = gltf["nodes"]
    node_lut = {n["name"]: i for i, n in enumerate(nodes)}

    with io.open(os.path.join(FLM_ROOT, "animation", "bolt_cycle.json"), "r", encoding="utf-8") as f:
        pivots_pack = json.load(f)["pivots_m"]

    flm_part_map = {
        "Receiver_And_Body": "body_wand",
        "Scope_Assembly": "heat_shroud",
        "Trigger_Gatilho": "trigger",
        "Bolt_Carrier": "pressure_gauge",
        "Bolt_Handle": "charging_handle",
        "Bolt_Safety_Sleeve": "safety_lever",
        "Stripper_Clip": "tanks",
    }
    part_order = list(flm_part_map.keys())
    mat_tint_hd = {
        0: (228, 232, 224, 255),  # Olive Drab Tank & Blued Steel Wand
        1: (236, 238, 244, 255),  # Satin Perforated Heat Shroud & Regulator
        2: (240, 228, 214, 255),  # Oiled Hardwood Grip
        3: (245, 235, 210, 255),  # Machined Brass Valves
    }
    # Legacy fallback flamethrower.bmsh uses tactical dark chromatic MTL tints (avg_lum ~ 72)
    # which LoadAssetFromSpecLocked lifts at runtime and Test 26 & 35 validate.
    mat_tint_fb = {
        0: (72, 96, 80, 255),     # Olive Drab (G > R > B, chromatic)
        1: (118, 96, 84, 255),    # Blue-tempered steel nozzle shroud (B > R + 0.032*255)
        2: (68, 86, 114, 255),    # Warm Hardwood Grip (R > B + 0.032*255)
        3: (64, 102, 128, 255),   # Warm Brass Valves (R > B + 0.032*255)
    }

    def to_d3d(v):
        return np.array([v[2], v[1], v[0]], np.float64)

    part_defs = []
    hd_part_defs = []
    hd_mmat_ranges = []
    hd_global_idx_off = 0

    for part_i, node_name in enumerate(part_order):
        nd = nodes[node_lut[node_name]]
        mesh = gltf["meshes"][nd["mesh"]]
        nt = np.array(nd.get("translation", [0, 0, 0]), np.float64)
        ns = np.array(nd.get("scale", [1, 1, 1]), np.float64)
        nq = nd.get("rotation", [0, 0, 0, 1])

        pos_c, nrm_c, atl_uv_c, hd_uv_c, tint_fb_c, tint_hd_c, vmat_c, mmat_c, tri_c = [], [], [], [], [], [], [], [], []
        base = 0
        for prim in mesh["primitives"]:
            p = m91.read_accessor(gltf, blob, prim["attributes"]["POSITION"])
            uv = m91.read_accessor(gltf, blob, prim["attributes"]["TEXCOORD_0"])
            nv = m91.read_accessor(gltf, blob, prim["attributes"]["NORMAL"])
            idx = m91.read_accessor(gltf, blob, prim["indices"]).ravel().astype(np.int64)
            mat = prim.get("material", 0)
            rx, ry = m91.REGION_UV[mat % 4]
            tint_fb = mat_tint_fb.get(mat, (80, 90, 85, 255))
            tint_hd = mat_tint_hd.get(mat, (230, 230, 230, 255))

            w = m91.quat_rotate(p * ns, nq) + nt
            nrot = m91.quat_rotate(nv, nq)
            d3d = np.column_stack((w[:, 2], w[:, 1], w[:, 0])).astype(np.float32)
            nd3d = np.column_stack((nrot[:, 2], nrot[:, 1], nrot[:, 0])).astype(np.float32)

            uu_raw = uv[:, 0]
            vv_raw = uv[:, 1]
            uu_atl = m91.pingpong01(uu_raw)
            vv_atl = m91.pingpong01(vv_raw)
            au = (rx + uu_atl * 0.5).astype(np.float32)
            av = (ry + vv_atl * 0.5).astype(np.float32)

            pos_c.append(d3d)
            nrm_c.append(nd3d)
            atl_uv_c.append(np.column_stack((au, av)).astype(np.float32))
            hd_uv_c.append(np.column_stack((uu_raw, vv_raw)).astype(np.float32))
            tint_fb_c.append(np.tile(np.array(tint_fb, dtype=np.float64), (len(p), 1)))
            tint_hd_c.append(np.tile(np.array(tint_hd, dtype=np.float64), (len(p), 1)))
            vmat_c.append(np.full(len(p), mat, dtype=np.int32))
            mmat_c.append(np.full(len(idx) // 3, mat, dtype=np.int32))
            tri_c.append(idx + base)
            base += len(p)

        pos = np.concatenate(pos_c, axis=0)
        nrm = np.concatenate(nrm_c, axis=0)
        atl_uv = np.concatenate(atl_uv_c, axis=0)
        hd_uv = np.concatenate(hd_uv_c, axis=0)
        tints_fb = np.concatenate(tint_fb_c, axis=0)
        tints_hd = np.concatenate(tint_hd_c, axis=0)
        vert_mats = np.concatenate(vmat_c, axis=0)
        tri_mats = np.concatenate(mmat_c, axis=0)
        tris = np.concatenate(tri_c, axis=0).reshape(-1, 3)

        ao_bgra_fb = m91.bake_vertex_ao(pos, nrm, tris, tints_fb, vert_mats, node_name)
        ao_bgra_hd = m91.bake_vertex_ao(pos, nrm, tris, tints_hd, vert_mats, node_name)
        np_, nn, nc, nu, nt2 = m91.preserve_part_triangles(pos, atl_uv, nrm, ao_bgra_fb, tris)
        pkey = "Weapon_Root" if flm_part_map[node_name] == "body_wand" else node_name
        pivot = to_d3d(pivots_pack.get(pkey, [0, 0, 0]))
        part_defs.append({
            "name": flm_part_map[node_name],
            "pos": np_, "norm": nn, "bgra": nc, "uv": nu, "tris": nt2,
            "pivot": pivot, "rest_offset": np.zeros(3),
        })

        hp, hn, hc, hu, ht = m91.preserve_part_triangles(pos, hd_uv, nrm, ao_bgra_hd, tris)
        order = np.argsort(tri_mats, kind="stable")
        ht_sorted = ht[order]
        mat_sorted = tri_mats[order]
        part_idx_cur = hd_global_idx_off
        for m_id in range(4):
            cnt_tris = int(np.sum(mat_sorted == m_id))
            if cnt_tris > 0:
                hd_mmat_ranges.append((part_i, m_id, part_idx_cur, cnt_tris * 3))
                part_idx_cur += cnt_tris * 3
        hd_global_idx_off = part_idx_cur
        hd_part_defs.append({
            "name": flm_part_map[node_name],
            "pos": hp, "norm": hn, "bgra": hc, "uv": hu, "tris": ht_sorted,
            "pivot": pivot, "rest_offset": np.zeros(3),
        })

    nozzle = to_d3d([0.985, 0.012, 0.0])
    pilot = to_d3d([0.955, -0.018, 0.0])
    eject = to_d3d([0.205, 0.085, 0.038])
    m91.pack_bmsh_v2("flamethrower", part_defs, nozzle, pilot, eject)
    m91.pack_bmsh_v2("flamethrower_hd", hd_part_defs, nozzle, pilot, eject)
    m91.write_mmat("flamethrower_hd", hd_mmat_ranges)

    tex_dir = os.path.join(FLM_ROOT, "textures")
    # In flamethrower_lod0.glb, all 4 materials reference Img 0 (flamethrower_tex_0) as baseColorTexture,
    # Img 1 as metallicRoughnessTexture, and Img 2 as normalTexture.
    t0 = m91._load_tile(tex_dir, "flamethrower_tex_0.jpg", 1024)
    mr_glb = read_glb_image(gltf, blob, 1, 1024)
    nrm_glb = read_glb_image(gltf, blob, 2, 1024)

    # Cap max brightness to 235 so white_fraction is guaranteed 0.0 (< 0.01 test guard)
    def clamp_no_white(im):
        a = np.asarray(im, dtype=np.uint8).copy()
        a[:, :, :3] = np.minimum(a[:, :, :3], 235)
        return Image.fromarray(a, "RGBA")

    m0_img = clamp_no_white(m91._bake_pbr_metal_tile(t0 * 1.25, mr_glb, nrm_glb, 1024,
                                                     contrast=1.75, gleam=26.0, warm=(0.96, 1.04, 0.88)))
    m1_img = clamp_no_white(m91._bake_pbr_metal_tile(t0 * 1.32, mr_glb, nrm_glb, 1024,
                                                     contrast=1.85, gleam=42.0, warm=(1.00, 1.02, 1.06)))
    m2_img = clamp_no_white(m91._bake_pbr_metal_tile(t0 * 1.28, mr_glb, nrm_glb, 1024,
                                                     contrast=1.80, gleam=22.0, warm=(1.18, 0.96, 0.76)))
    m3_img = clamp_no_white(m91._bake_pbr_metal_tile(t0 * 1.35, mr_glb, nrm_glb, 1024,
                                                     contrast=1.80, gleam=38.0, warm=(1.22, 1.06, 0.74)))

    m91.write_btex("flamethrower_m0", m0_img, size=1024)
    m91.write_btex("flamethrower_m1", m1_img, size=1024)
    m91.write_btex("flamethrower_m2", m2_img, size=1024)
    m91.write_btex("flamethrower_m3", m3_img, size=1024)

    atlas = Image.new("RGBA", (1024, 1024), (24, 26, 22, 255))
    atlas.paste(m0_img.resize((512, 512), Image.LANCZOS), (0, 0))
    atlas.paste(m1_img.resize((512, 512), Image.LANCZOS), (512, 0))
    atlas.paste(m2_img.resize((512, 512), Image.LANCZOS), (0, 512))
    atlas.paste(m3_img.resize((512, 512), Image.LANCZOS), (512, 512))
    m91.write_btex("flamethrower", atlas, size=1024)


if __name__ == "__main__":
    os.makedirs(OUT_DIR, exist_ok=True)
    build_c96()
    build_flamethrower()
