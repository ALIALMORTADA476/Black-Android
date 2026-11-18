#!/usr/bin/env python3
"""Bake the AAA first-person hands GLB into an engine BMSH v2 + BTEX v1 pair.

Source (inside the weapon pack):
    الاسلحه الخاصه/Mauser 1891-12/hands/hands_lod0.glb   skinned mesh, 40 bones, rest pose == grip pose
    الاسلحه الخاصه/Mauser 1891-12/hands/hands_tex_0.jpg  1024x1024 albedo

Output:
    app/src/main/assets/weapons/weapon_hands.bmsh
    app/src/main/assets/weapons/weapon_hands.btex

The engine has no skeletal skinning path for viewmodels. The GLB's inverse bind
matrices are not valid affine inverses (bad 4th row), but its raw POSITION/NORMAL
attributes ARE the authored grip pose, so we bake those directly through the mesh
node's world matrix into a static mesh in weapon-root space, drawn with the same
world matrix as the gun.
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_m91_glb as m91  # noqa: E402  (reuse the proven BMSH/BTEX writers)

PACK_ROOT = m91.PACK_ROOT
HANDS_GLB = os.path.join(PACK_ROOT, "hands", "hands_lod0.glb")
HANDS_TEX = os.path.join(PACK_ROOT, "hands", "hands_tex_0.jpg")
OUT_NAME = "weapon_hands"

_COMP = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16,
         5125: np.uint32, 5126: np.float32}
_NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def read_any(gltf, blob, idx):
    """Accessor reader that also handles MAT4 (inverse bind matrices)."""
    acc = gltf["accessors"][idx]
    bv = gltf["bufferViews"][acc["bufferView"]]
    dt = np.dtype(_COMP[acc["componentType"]]).newbyteorder("<")
    n = _NCOMP[acc["type"]]
    off = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride", dt.itemsize * n)
    return np.ndarray(shape=(acc["count"], n), dtype=dt, buffer=blob,
                      strides=(stride, dt.itemsize)).astype(np.float64)


def node_local(node):
    """TRS -> 4x4 local matrix (column-vector convention, like glTF)."""
    if "matrix" in node:
        return np.array(node["matrix"], np.float64).reshape(4, 4).T
    t = np.array(node.get("translation", [0.0, 0.0, 0.0]), np.float64)
    q = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    s = np.array(node.get("scale", [1.0, 1.0, 1.0]), np.float64)
    x, y, z, w = q
    r = np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ], np.float64)
    m = np.eye(4)
    m[:3, :3] = r * s[None, :]
    m[:3, 3] = t
    return m


def world_matrices(gltf):
    nodes = gltf["nodes"]
    out = [None] * len(nodes)

    def walk(i, parent):
        out[i] = parent @ node_local(nodes[i])
        for c in nodes[i].get("children", []):
            walk(c, out[i])

    for scene in gltf.get("scenes", []):
        for root in scene.get("nodes", []):
            walk(root, np.eye(4))
    return out


def main():
    for path in (HANDS_GLB, HANDS_TEX):
        if not os.path.exists(path):
            raise SystemExit("missing %s" % path)

    gltf, blob = m91.parse_glb(HANDS_GLB)

    mesh = gltf["meshes"][0]
    prim = mesh["primitives"][0]
    pos = m91.read_accessor(gltf, blob, prim["attributes"]["POSITION"])
    nrm = m91.read_accessor(gltf, blob, prim["attributes"]["NORMAL"])
    uv = m91.read_accessor(gltf, blob, prim["attributes"]["TEXCOORD_0"])
    jnt = read_any(gltf, blob, prim["attributes"]["JOINTS_0"]).astype(np.int64)
    wgt = read_any(gltf, blob, prim["attributes"]["WEIGHTS_0"])
    idx = m91.read_accessor(gltf, blob, prim["indices"]).ravel().astype(np.int64)

    # The skin maths in this GLB are unusable: world @ inverseBindMatrix
    # deviates 1.07-1.36 from identity for every joint and the 4th row is
    # garbage, i.e. they are not proper affine inverses. Nothing is lost by
    # ignoring them, because the file carries no animations and its rest pose
    # is already the authored grip pose -- the raw POSITION/NORMAL attributes
    # (bbox 0.74 x 0.26 x 0.38 m, matching the pack's real hand dimensions) are
    # the posed geometry. So we only need the mesh node's world matrix.
    joints = gltf["skins"][0]["joints"]
    mesh_node = next(i for i, nd in enumerate(gltf["nodes"]) if nd.get("mesh") == 0)
    node_world = world_matrices(gltf)[mesh_node]

    n = len(pos)
    out_pos = pos @ node_world[:3, :3].T + node_world[:3, 3][None, :]
    out_nrm = nrm @ node_world[:3, :3].T

    ln = np.linalg.norm(out_nrm, axis=1, keepdims=True)
    ln[ln < 1e-9] = 1.0
    out_nrm /= ln

    # pack (x fwd, y up, z right) -> d3d (z, y, x); viewmodel pass is CULL_NONE
    d3d_pos = np.column_stack((out_pos[:, 2], out_pos[:, 1], out_pos[:, 0])).astype(np.float32)
    d3d_nrm = np.column_stack((out_nrm[:, 2], out_nrm[:, 1], out_nrm[:, 0])).astype(np.float32)

    # BTEX sampling matches the weapon builder convention (V flipped).
    uu = np.mod(uv[:, 0], 1.0).astype(np.float32)
    vv = (1.0 - np.mod(uv[:, 1], 1.0)).astype(np.float32)
    auv = np.column_stack((uu, vv)).astype(np.float32)

    tint = np.uint32((255 << 24) | (255 << 16) | (255 << 8) | 255)
    bgra = np.full(n, tint, np.uint32)

    tris = idx.reshape(-1, 3)
    part = {
        "name": "hands",
        "pos": d3d_pos, "norm": d3d_nrm, "bgra": bgra, "uv": auv, "tris": tris,
        "pivot": np.zeros(3), "rest_offset": np.zeros(3),
    }
    zero = np.zeros(3)
    m91.pack_bmsh_v2(OUT_NAME, [part], zero, zero, zero)

    m91.write_btex(OUT_NAME, Image.open(HANDS_TEX).convert("RGBA"))

    size = d3d_pos.max(axis=0) - d3d_pos.min(axis=0)
    print("DONE: %d verts / %d tris / %d bones, bbox %.3f x %.3f x %.3f m" %
          (n, len(tris), len(joints), size[0], size[1], size[2]))


if __name__ == "__main__":
    main()
