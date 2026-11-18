#!/usr/bin/env python3
"""Build 40-bone LBS rigged hands asset (.hskn) + clean BLACK-styled tactical texture (.btex).

Reads:
  الاسلحه الخاصه/Mauser 1891-12/hands/hands_lod0.glb
  الاسلحه الخاصه/Mauser 1891-12/hands/hands_tex_0.jpg
Writes:
  app/src/main/assets/weapons/mauser_1891_12_hands.hskn
  app/src/main/assets/weapons/mauser_1891_12_hands.btex
"""
import os
import struct
import sys
from collections import defaultdict, deque

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_hands_glb as h
import build_m91_glb as m91

HSKN_MAGIC = 0x4E4B5348  # 'HSKN'


def build_black_tactical_hands_texture():
    """Synthesize a clean, streak-free 1024x1024 BLACK tactical leather glove & ripstop sleeve texture."""
    im = Image.open(h.HANDS_TEX).convert("RGB").resize((1024, 1024), Image.LANCZOS)
    a = np.asarray(im, np.float32)
    # Extract only ultra-smooth macro ambient shading from the source map (eliminating all wireframe,
    # digital camo squares, and bright edge-padding rays).
    luma_raw = (0.299 * a[:, :, 0] + 0.587 * a[:, :, 1] + 0.114 * a[:, :, 2]) / 255.0
    clamped_luma = np.clip(luma_raw, 0.15, 0.52)
    macro_shade = np.asarray(
        Image.fromarray(np.clip(clamped_luma * 255.0, 0, 255).astype(np.uint8))
        .filter(ImageFilter.GaussianBlur(28)),
        np.float32,
    ) / 255.0
    macro_mod = np.clip(0.82 + (macro_shade - float(macro_shade.mean())) * 1.15, 0.68, 1.14)

    yy, xx = np.mgrid[0:1024, 0:1024].astype(np.float32)
    is_sleeve = (xx < 512) & (yy < 520)
    is_cuff = (xx < 256) & (yy >= 520)
    is_armplate = (xx >= 256) & (xx < 512) & (yy >= 520)

    # Fine multi-scale leather pebble grain (for gloves) & ripstop crosshatch weave (for sleeves)
    p1 = np.sin(xx * 0.55 + np.cos(yy * 0.42) * 1.8) * np.cos(yy * 0.58 - np.sin(xx * 0.38) * 1.8)
    p2 = np.sin(xx * 1.35 + yy * 0.95) * np.cos(xx * 0.85 - yy * 1.45)
    leather_grain = (p1 * 0.045 + p2 * 0.028)
    ripstop = (np.sin(xx * 1.57) * np.sin(yy * 1.57)) * 0.035 + np.cos((xx + yy) * 0.78) * 0.018
    rib_knit = np.sin(xx * 2.2) * 0.055

    out = np.zeros((1024, 1024, 4), dtype=np.uint8)
    glove_mod = (macro_mod + leather_grain)[:, :, None]
    sleeve_mod = (macro_mod + ripstop)[:, :, None]
    cuff_mod = (macro_mod + rib_knit)[:, :, None]
    plate_mod = (macro_mod + leather_grain * 0.6)[:, :, None]

    # Authentic BLACK first-person palette: matte charcoal leather gloves & dark slate-navy sleeves
    glove_rgb = np.array([58.0, 61.0, 66.0]) * glove_mod
    sleeve_rgb = np.array([46.0, 50.0, 55.0]) * sleeve_mod
    cuff_rgb = np.array([42.0, 45.0, 50.0]) * cuff_mod
    plate_rgb = np.array([52.0, 56.0, 62.0]) * plate_mod

    rgb = glove_rgb.copy()
    rgb[is_sleeve] = sleeve_rgb[is_sleeve]
    rgb[is_cuff] = cuff_rgb[is_cuff]
    rgb[is_armplate] = plate_rgb[is_armplate]

    out[:, :, :3] = np.clip(rgb, 18.0, 128.0).astype(np.uint8)
    out[:, :, 3] = 255
    return Image.fromarray(out, "RGBA")


def compute_clean_skin_weights(gltf, rest_pos, tris, wms):
    """Build smooth, side-isolated, UV-seam-welded 4-bone weights for the 40 GLB joints."""
    nodes = gltf["nodes"]
    joints = gltf["skins"][0]["joints"]
    jidx_of_node = {ni: k for k, ni in enumerate(joints)}
    n = len(rest_pos)

    # Topological adjacency + weld duplicate seam positions so UV splits share identical weights
    pos_map = defaultdict(list)
    for i in range(n):
        key = tuple(np.round(rest_pos[i] * 1e5).astype(np.int64))
        pos_map[key].append(i)

    adj = defaultdict(set)
    for a, b, c in tris:
        adj[a].update((b, c))
        adj[b].update((a, c))
        adj[c].update((a, b))
    for grp in pos_map.values():
        if len(grp) > 1:
            g0 = grp[0]
            for gi in grp[1:]:
                adj[g0].update(adj[gi])
                adj[gi].update(adj[g0])

    comp = np.full(n, -1, dtype=np.int64)
    cid = 0
    for s in range(n):
        if comp[s] >= 0:
            continue
        q = deque([s])
        comp[s] = cid
        while q:
            u = q.popleft()
            for v in adj[u]:
                if comp[v] < 0:
                    comp[v] = cid
                    q.append(v)
        cid += 1

    jl_pos = np.array([wms[joints[k]][:3, 3] for k in range(3, 22)])
    jr_pos = np.array([wms[joints[k]][:3, 3] for k in range(22, 40)])
    comp_is_left = {}
    for c in range(cid):
        pts = rest_pos[comp == c]
        dl = np.linalg.norm(jl_pos - pts.mean(0), axis=1).min()
        dr = np.linalg.norm(jr_pos - pts.mean(0), axis=1).min()
        comp_is_left[c] = (dl < dr)

    segs_l, segs_r = [], []
    for k, ni in enumerate(joints):
        if k < 3:
            continue
        name = nodes[ni]["name"]
        is_l = (3 <= k <= 21)
        pk = None
        for pni, pnd in enumerate(nodes):
            if ni in pnd.get("children", []) and pni in jidx_of_node and jidx_of_node[pni] >= 3:
                pk = jidx_of_node[pni]
                break
        if pk is None:
            continue
        p0 = wms[joints[pk]][:3, 3]
        p1 = wms[ni][:3, 3]
        is_f = ("Finger" in name or "Thumb" in name)
        sig = 0.015 if is_f else 0.050
        (segs_l if is_l else segs_r).append((pk, k, p0, p1, sig, is_f, nodes[joints[pk]]["name"]))

    def seg_dist(pts, a, b):
        ab = b - a
        l2 = float(np.dot(ab, ab))
        if l2 < 1e-10:
            return np.linalg.norm(pts - a, axis=1), np.zeros(len(pts))
        t = np.clip(np.dot(pts - a, ab) / l2, 0.0, 1.0)
        return np.linalg.norm(pts - (a[None, :] + t[:, None] * ab[None, :]), axis=1), t

    full_w = np.zeros((n, 40), dtype=np.float64)
    for side_left, segs in [(True, segs_l), (False, segs_r)]:
        mask = np.array([comp_is_left[comp[i]] == side_left for i in range(n)])
        idxs = np.where(mask)[0]
        pts = rest_pos[idxs]
        wrist_x = float(wms[joints[6 if side_left else 24]][0, 3])
        scores = np.zeros((len(pts), 40), dtype=np.float64)
        for pk, ck, a, b, sig, is_f, pname in segs:
            d, t = seg_dist(pts, a, b)
            w = np.exp(- (d / sig) ** 2)
            if not is_f:
                arm_mask = (pts[:, 0] <= wrist_x + 0.045)
                w = np.where(arm_mask, w, w * 0.04)
                scores[:, pk] += w * (1.0 - 0.55 * t)
                scores[:, ck] += w * (0.55 * t)
            else:
                hand_mask = (pts[:, 0] >= wrist_x - 0.012)
                w = np.where(hand_mask, w, 0.0)
                if "Wrist" in pname:
                    scores[:, pk] += w * (1.0 - 0.50 * t)
                    scores[:, ck] += w * (0.50 * t)
                else:
                    scores[:, pk] += w * (1.0 - 0.65 * t)
                    scores[:, ck] += w * (0.65 * t)
        scores[:, 3 if side_left else 22] += 1e-8
        scores /= np.maximum(scores.sum(1, keepdims=True), 1e-12)
        full_w[idxs] = scores

    # Synchronize duplicate seam verts & apply 6 Jacobi smoothing passes
    nbrs = [list(adj[i]) for i in range(n)]
    for _ in range(6):
        for grp in pos_map.values():
            if len(grp) > 1:
                full_w[grp] = full_w[grp].mean(axis=0, keepdims=True)
        nxt = full_w.copy()
        for i in range(n):
            ns = nbrs[i]
            if ns:
                nxt[i] = 0.55 * full_w[i] + 0.45 * full_w[ns].mean(axis=0)
        full_w = nxt
    for grp in pos_map.values():
        if len(grp) > 1:
            full_w[grp] = full_w[grp].mean(axis=0, keepdims=True)

    top4 = np.argsort(-full_w, axis=1)[:, :4]
    w4 = np.take_along_axis(full_w, top4, axis=1)
    w4 /= np.maximum(w4.sum(1, keepdims=True), 1e-12)
    return top4.astype(np.uint8), w4.astype(np.float32)


def main():
    os.makedirs(m91.OUT_DIR, exist_ok=True)
    gltf, blob = m91.parse_glb(h.HANDS_GLB)
    nodes = gltf["nodes"]
    prim = gltf["meshes"][0]["primitives"][0]
    pos = m91.read_accessor(gltf, blob, prim["attributes"]["POSITION"])
    nrm = m91.read_accessor(gltf, blob, prim["attributes"]["NORMAL"])
    uv = m91.read_accessor(gltf, blob, prim["attributes"]["TEXCOORD_0"])
    tris = m91.read_accessor(gltf, blob, prim["indices"]).ravel().astype(np.int64).reshape(-1, 3)

    mn = next(i for i, nd in enumerate(nodes) if nd.get("mesh") == 0)
    wms = h.world_matrices(gltf)
    node_w = wms[mn]
    rest_pos = (pos @ node_w[:3, :3].T + node_w[:3, 3]).astype(np.float64)
    rest_nrm = (nrm @ node_w[:3, :3].T).astype(np.float64)
    nl = np.linalg.norm(rest_nrm, axis=1, keepdims=True)
    nl[nl < 1e-9] = 1.0
    rest_nrm /= nl

    skin = gltf["skins"][0]
    joints = skin["joints"]
    jidx_of_node = {ni: k for k, ni in enumerate(joints)}
    assert len(joints) == 40, "expected 40 joints"

    b_idx, b_wgt = compute_clean_skin_weights(gltf, rest_pos, tris, wms)

    # Conform right & left hands to the Kar98k stock envelope so zero fingers penetrate wood
    gltf_k, blob_k = m91.parse_glb(os.path.join(m91.PACK_ROOT, "model", "kar_98_lod0.glb"))
    nodes_k = gltf_k["nodes"]
    root_t = np.array(nodes_k[10].get("translation", [0, 0, 0]), np.float64)
    rec_nd = next(nd for nd in nodes_k if nd["name"] == "Receiver_And_Body")
    rec_mesh = gltf_k["meshes"][rec_nd["mesh"]]
    rec_nt = np.array(rec_nd.get("translation", [0, 0, 0]), np.float64)
    rec_ns = np.array(rec_nd.get("scale", [1, 1, 1]), np.float64)
    rec_nq = rec_nd.get("rotation", [0, 0, 0, 1])
    rec_pts = []
    for pr in rec_mesh["primitives"]:
        pk_p = m91.read_accessor(gltf_k, blob_k, pr["attributes"]["POSITION"])
        w = m91.quat_rotate(pk_p * rec_ns, rec_nq) + rec_nt + root_t
        rec_pts.append(w)
    rec_pts = np.concatenate(rec_pts, axis=0)
    stock_x, stock_y, stock_z = rec_pts[:, 0], rec_pts[:, 1], rec_pts[:, 2]

    xbins = np.linspace(0.05, 0.75, 141)
    ymin_lut = np.full(140, -0.02)
    ymax_lut = np.full(140, +0.03)
    zmin_lut = np.full(140, -0.011)
    zmax_lut = np.full(140, +0.011)
    for b in range(140):
        x0, x1 = xbins[b] - 0.005, xbins[b + 1] + 0.005
        m_s = (stock_x >= x0) & (stock_x <= x1)
        if np.any(m_s):
            ymin_lut[b] = float(np.percentile(stock_y[m_s], 2))
            ymax_lut[b] = float(np.percentile(stock_y[m_s], 98))
            zmin_lut[b] = float(np.percentile(stock_z[m_s], 2))
            zmax_lut[b] = float(np.percentile(stock_z[m_s], 98))

    def stock_bounds_at(x_val):
        bi = int(np.clip((x_val - 0.05) / (0.70 / 140.0), 0, 139))
        return ymin_lut[bi], ymax_lut[bi], zmin_lut[bi], zmax_lut[bi]

    n = len(rest_pos)
    W_mat = np.zeros((n, 40), dtype=np.float64)
    for k in range(4):
        np.add.at(W_mat, (np.arange(n), b_idx[:, k]), b_wgt[:, k])
    left_w = W_mat[:, 3:22].sum(axis=1)
    right_w = W_mat[:, 22:40].sum(axis=1)
    w_f1_rt = W_mat[:, [25, 26, 27]].sum(axis=1)
    w_f234_rt = W_mat[:, 28:37].sum(axis=1)
    w_thumb_rt = W_mat[:, [37, 38, 39]].sum(axis=1)
    w_thumb_lt = W_mat[:, [19, 20, 21]].sum(axis=1)
    w_fingers_lt = W_mat[:, 7:19].sum(axis=1)

    for i in range(n):
        rw = right_w[i]
        if rw > 1e-4:
            p = rest_pos[i]
            arm_t = np.clip((p[0] - (-0.15)) / (0.18 - (-0.15)), 0.0, 1.0)
            f1 = w_f1_rt[i]
            f234 = w_f234_rt[i]
            th = w_thumb_rt[i]
            dx = -0.044 * arm_t + f1 * (+0.028) + f234 * (-0.010) + th * (-0.018)
            dy = -0.003 * arm_t + f1 * (-0.008) + f234 * (-0.014) + th * (+0.006)
            dz = +0.014 * arm_t + f1 * (-0.004) + f234 * (+0.004) + th * (-0.006)
            rest_pos[i] = p + rw * np.array([dx, dy, dz])
        lw = left_w[i]
        if lw > 1e-4:
            p = rest_pos[i]
            arm_t = np.clip((p[0] - 0.10) / (0.46 - 0.10), 0.0, 1.0)
            f_lt = w_fingers_lt[i]
            th_lt = w_thumb_lt[i]
            dx = -0.008 * arm_t
            dy = -0.024 * arm_t - 0.008 * f_lt - 0.006 * th_lt
            dz = +0.006 * arm_t + 0.008 * f_lt - 0.004 * th_lt
            rest_pos[i] = p + lw * np.array([dx, dy, dz])

    pos_map = defaultdict(list)
    for i in range(n):
        pos_map[tuple(np.round(rest_pos[i] * 1e4).astype(np.int64))].append(i)
    adj = defaultdict(set)
    for a, b, c in tris:
        adj[a].update((b, c)); adj[b].update((a, c)); adj[c].update((a, b))
    for grp in pos_map.values():
        if len(grp) > 1:
            g0 = grp[0]
            for gi in grp[1:]:
                adj[g0].update(adj[gi]); adj[gi].update(adj[g0])
    nbrs = [list(adj[i]) for i in range(n)]

    pad = 0.0025
    for pass_i in range(4):
        for i in range(n):
            x, y, z = rest_pos[i]
            if x < 0.12 or x > 0.68:
                continue
            ymin_s, ymax_s, zmin_s, zmax_s = stock_bounds_at(x)
            if right_w[i] >= 0.5:
                if w_f1_rt[i] > 0.35 and x >= 0.255 and y < ymin_s + 0.004:
                    # Index finger on trigger blade
                    if z < 0.001:
                        z = 0.001
                    if y > ymin_s - 0.001 and z < zmax_s + pad:
                        y = ymin_s - 0.001
                elif w_thumb_rt[i] > 0.35 and x <= 0.228 and y >= ymax_s - 0.004:
                    # Right thumb wrapping over the top of the pistol-grip wrist saddle
                    y = max(y, ymax_s + 0.0018)
                    z = max(z, -0.006)
                elif w_f234_rt[i] > 0.30 and x <= 0.242 and y <= ymin_s + 0.006:
                    # Right middle/ring/pinky fingers curling underneath the pistol grip
                    y = min(y, ymin_s - 0.0018)
                    z = max(z, zmin_s - 0.001)
                else:
                    if z < zmin_s + 0.002:
                        z = zmin_s + 0.002
                    if (ymin_s - pad) < y < (ymax_s + pad) and z < (zmax_s + pad):
                        if w_f234_rt[i] > 0.25 and y < (0.65 * ymin_s + 0.35 * ymax_s):
                            y = ymin_s - pad
                        else:
                            z = zmax_s + pad
                rest_pos[i] = [x, y, z]
            elif left_w[i] >= 0.5:
                if y > ymax_s - 0.005:
                    y = ymax_s - 0.005
                if y > (ymin_s - pad) and (zmin_s - pad) < z < (zmax_s + pad):
                    if w_thumb_lt[i] > 0.2 or z < 0.0:
                        z = zmin_s - pad
                    elif w_fingers_lt[i] > 0.25 and z > 0.0:
                        z = zmax_s + pad
                    else:
                        y = ymin_s - pad
                rest_pos[i] = [x, y, z]
        if pass_i < 3:
            nxt = rest_pos.copy()
            for i in range(n):
                ns = nbrs[i]
                if ns:
                    nxt[i] = 0.68 * rest_pos[i] + 0.32 * rest_pos[ns].mean(axis=0)
            for grp in pos_map.values():
                if len(grp) > 1:
                    nxt[grp] = nxt[grp].mean(axis=0, keepdims=True)
            rest_pos = nxt

    # Recompute smooth vertex normals & 3D finger-crevice/knuckle AO for tactile depth
    acc_n = np.zeros_like(rest_pos)
    for t_tri in tris:
        a, b, c = rest_pos[t_tri[0]], rest_pos[t_tri[1]], rest_pos[t_tri[2]]
        fn = np.cross(b - a, c - a)
        acc_n[t_tri[0]] += fn
        acc_n[t_tri[1]] += fn
        acc_n[t_tri[2]] += fn
    for grp in pos_map.values():
        if len(grp) > 1:
            acc_n[grp] = acc_n[grp].sum(axis=0, keepdims=True)
    nl = np.linalg.norm(acc_n, axis=1, keepdims=True)
    nl[nl < 1e-9] = 1.0
    rest_nrm = acc_n / nl

    tints_h = np.full((n, 4), [235.0, 238.0, 242.0, 255.0], dtype=np.float64)
    pos_d3d_h = np.column_stack((rest_pos[:, 2], rest_pos[:, 1], rest_pos[:, 0])).astype(np.float32)
    nrm_d3d_h = np.column_stack((rest_nrm[:, 2], rest_nrm[:, 1], rest_nrm[:, 0])).astype(np.float32)
    hand_bgra = m91.bake_vertex_ao(pos_d3d_h, nrm_d3d_h, tris, tints_h, None, "Hands")

    uu = np.mod(uv[:, 0], 1.0).astype(np.float32)
    vv = np.mod(uv[:, 1], 1.0).astype(np.float32)

    anims = gltf.get("animations", [])
    hskn_path = os.path.join(m91.OUT_DIR, "mauser_1891_12_hands.hskn")
    with open(hskn_path, "wb") as f:
        f.write(struct.pack("<8I", HSKN_MAGIC, 1, len(rest_pos), len(tris) * 3,
                            len(joints), len(anims), 0, 0))
        for i in range(len(rest_pos)):
            f.write(struct.pack(
                "<8fI4B4f",
                float(rest_pos[i, 0]), float(rest_pos[i, 1]), float(rest_pos[i, 2]),
                float(rest_nrm[i, 0]), float(rest_nrm[i, 1]), float(rest_nrm[i, 2]),
                float(uu[i]), float(vv[i]),
                int(hand_bgra[i]),
                int(b_idx[i, 0]), int(b_idx[i, 1]), int(b_idx[i, 2]), int(b_idx[i, 3]),
                float(b_wgt[i, 0]), float(b_wgt[i, 1]), float(b_wgt[i, 2]), float(b_wgt[i, 3]),
            ))
        f.write(tris.ravel().astype("<u2").tobytes())

        for k, ni in enumerate(joints):
            nd = nodes[ni]
            parent_k = -1
            for pni, pnd in enumerate(nodes):
                if ni in pnd.get("children", []) and pni in jidx_of_node:
                    parent_k = jidx_of_node[pni]
                    break
            t = nd.get("translation", [0.0, 0.0, 0.0])
            q = nd.get("rotation", [0.0, 0.0, 0.0, 1.0])
            s = nd.get("scale", [1.0, 1.0, 1.0])
            inv_b = np.linalg.inv(wms[ni]).astype(np.float32)
            f.write(struct.pack("<i", parent_k))
            f.write(struct.pack("<3f4f3f", *(list(t) + list(q) + list(s))))
            f.write(inv_b.ravel().tobytes())

        path_id_map = {"translation": 0, "rotation": 1, "scale": 2}
        for anim in anims:
            samplers = anim["samplers"]
            valid_chs = [c for c in anim["channels"]
                         if c["target"]["node"] in jidx_of_node and c["target"]["path"] in path_id_map]
            max_t = 0.0
            for c in valid_chs:
                ti = m91.read_accessor(gltf, blob, samplers[c["sampler"]]["input"]).ravel()
                if len(ti):
                    max_t = max(max_t, float(ti[-1]))
            nm = anim["name"].encode("utf-8")[:31]
            f.write(struct.pack("<32sfI", nm, max_t, len(valid_chs)))
            for c in valid_chs:
                jk = jidx_of_node[c["target"]["node"]]
                pk = path_id_map[c["target"]["path"]]
                samp = samplers[c["sampler"]]
                ti = m91.read_accessor(gltf, blob, samp["input"]).ravel().astype(np.float32)
                vo = m91.read_accessor(gltf, blob, samp["output"]).astype(np.float32)
                f.write(struct.pack("<III", jk, pk, len(ti)))
                f.write(ti.tobytes())
                f.write(vo.ravel().tobytes())

    print("[OK] HSKN %s (%d verts, %d tris, %d joints, %d clips, %d bytes)" %
          (hskn_path, len(rest_pos), len(tris), len(joints), len(anims), os.path.getsize(hskn_path)))

    tex = build_black_tactical_hands_texture()
    m91.write_btex("mauser_1891_12_hands", tex, size=1024)


if __name__ == "__main__":
    main()
