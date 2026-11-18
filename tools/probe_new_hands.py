import json, os, struct, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_m91_glb as m91
import build_hands_glb as bh

p = os.path.join(m91.PACK_ROOT, "hands", "hands_lod0.glb")
with open(p, "rb") as f:
    data = f.read()
magic, ver, length = struct.unpack_from("<III", data, 0)
print("header", hex(magic), ver, length, "file", len(data))
off = 12
chunks = []
while off < len(data):
    clen, ctype = struct.unpack_from("<II", data, off)
    chunks.append((ctype, clen))
    off += 8 + clen
print("chunks", [(hex(t), n) for t, n in chunks])
jlen, jtype = struct.unpack_from("<II", data, 12)
gltf = json.loads(data[20:20+jlen].decode("utf-8"))
blob = data[20+jlen:]
print("keys", list(gltf.keys()))
print("meshes", len(gltf.get("meshes", [])), "nodes", len(gltf.get("nodes", [])),
      "skins", len(gltf.get("skins", [])), "anims", len(gltf.get("animations", [])))
for i, m in enumerate(gltf.get("meshes", [])):
    for j, prim in enumerate(m["primitives"]):
        print("prim", i, j, "attrs", sorted(prim["attributes"].keys()),
              "indices", prim.get("indices"), "material", prim.get("material"))
        for k, ai in prim["attributes"].items():
            acc = gltf["accessors"][ai]
            print("  ", k, "count", acc["count"], "type", acc["type"], "ct", acc["componentType"])
for a in gltf.get("animations", []):
    print("anim", a.get("name"), "channels", len(a.get("channels", [])), "samplers", len(a.get("samplers", [])))

prim = gltf["meshes"][0]["primitives"][0]
pos = m91.read_accessor(gltf, blob, prim["attributes"]["POSITION"])
nrm = m91.read_accessor(gltf, blob, prim["attributes"]["NORMAL"])
uv = m91.read_accessor(gltf, blob, prim["attributes"]["TEXCOORD_0"])
print("pos bbox", pos.min(axis=0), pos.max(axis=0), "size", pos.max(axis=0)-pos.min(axis=0))
print("nrm ok", np.isfinite(nrm).all(), "uv range", uv.min(axis=0), uv.max(axis=0))
if "JOINTS_0" in prim["attributes"]:
    jnt = bh.read_any(gltf, blob, prim["attributes"]["JOINTS_0"]).astype(np.int64)
    wgt = bh.read_any(gltf, blob, prim["attributes"]["WEIGHTS_0"])
    print("jnt max", jnt.max(), "wgt sum range", wgt.sum(axis=1).min(), wgt.sum(axis=1).max())
skin = gltf["skins"][0]
ibm = bh.read_any(gltf, blob, skin["inverseBindMatrices"]).reshape(-1,4,4)
ibm = np.transpose(ibm, (0,2,1))
world = bh.world_matrices(gltf)
joints = skin["joints"]
bind = np.stack([world[joints[i]] @ ibm[i] for i in range(len(joints))])
dev = np.abs(bind[:,:3,:3] - np.eye(3)[None]).max(axis=(1,2))
print("bind dev min/max", dev.min(), dev.max(), "4th rows", bind[0][3])
print("joints", len(joints), "nodes with mesh", [i for i,n in enumerate(gltf["nodes"]) if "mesh" in n])
