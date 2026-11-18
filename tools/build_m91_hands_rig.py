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
    """Strip UV wireframe lines & decals from hands_tex_0.jpg and grade to BLACK tactical style."""
    im = Image.open(h.HANDS_TEX).convert("RGB").resize((1024, 1024), Image.LANCZOS)
    a = np.asarray(im, np.float32)
    lum = 0.299 * a[:, :, 0] + 0.587 * a[:, :, 1] + 0.114 * a[:, :, 2]
    sat = a.max(2) - a.min(2)
    # Remove white UV wireframe overlays and bright baking outlines
    wire_mask = (lum > 148.0) & (sat < 42.0)
    clean = a.copy()
    valid = (~wire_mask).astype(np.float32)
    for _ in range(5):
        num = np.asarray(
            Image.fromarray(np.clip(clean * valid[:, :, None], 0, 255).astype(np.uint8))
            .filter(ImageFilter.BoxBlur(5)),
            np.float32,
        )
        den = np.asarray(
            Image.fromarray(np.clip(valid * 255.0, 0, 255).astype(np.uint8))
            .filter(ImageFilter.BoxBlur(5)),
            np.float32,
        ) / 255.0
        fill = np.where(den[:, :, None] > 0.05, num / np.maximum(den[:, :, None], 1e-4), 56.0)
        clean = np.where(wire_mask[:, :, None], fill, clean)
        valid = np.clip(valid + (den > 0.2).astype(np.float32), 0, 1)

    # Local detail preservation from desaturated luma (eliminates orange blotches!)
    luma = (0.299 * clean[:, :, 0] + 0.587 * clean[:, :, 1] + 0.114 * clean[:, :, 2]) / 255.0
    luma_blur = np.asarray(
        Image.fromarray(np.clip(luma * 255.0, 0, 255).astype(np.uint8))
        .filter(ImageFilter.GaussianBlur(8)),
        np.float32,
    ) / 255.0
    micro = np.clip((luma - luma_blur) * 2.4, -0.25, 0.25)

    yy, xx = np.mgrid[0:1024, 0:1024]
    is_sleeve = (xx < 512) & (yy < 520)
    is_cuff = (xx < 256) & (yy >= 520)
    is_armplate = (xx >= 256) & (xx < 512) & (yy >= 520)

    out = np.zeros((1024, 1024, 4), dtype=np.uint8)
    # Matte black/charcoal tactical leather gloves with crisp fabric/grain contrast
    glove_rgb = np.array([62.0, 64.0, 68.0]) * (0.68 + 0.65 * luma[:, :, None] + micro[:, :, None] * 1.45)
    # Dark tactical combat sleeves (matching BLACK's navy-slate-charcoal uniform sleeves)
    sleeve_rgb = np.array([54.0, 58.0, 60.0]) * (0.65 + 0.70 * luma[:, :, None] + micro[:, :, None] * 1.35)
    # Knit wrist cuff & ballistic nylon guard (overwriting white eyeball graphic cleanly)
    cuff_rgb = np.array([48.0, 50.0, 54.0]) * (0.72 + 0.55 * luma[:, :, None] + micro[:, :, None])
    plate_rgb = np.array([52.0, 55.0, 60.0]) * (0.85 + 0.40 * micro[:, :, None])

    rgb = glove_rgb.copy()
    rgb[is_sleeve] = sleeve_rgb[is_sleeve]
    rgb[is_cuff] = cuff_rgb[is_cuff]
    rgb[is_armplate] = plate_rgb[is_armplate]

    out[:, :, :3] = np.clip(rgb, 14.0, 220.0).astype(np.uint8)
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

    uu = np.mod(uv[:, 0], 1.0).astype(np.float32)
    vv = (1.0 - np.mod(uv[:, 1], 1.0)).astype(np.float32)

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
                0xFFFFFFFF,
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
