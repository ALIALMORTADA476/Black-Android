#!/usr/bin/env python3
"""Diagnose why the baked hands look distorted: UV range, node world matrix, normals."""
import os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_hands_glb as h
import build_m91_glb as m91

gltf, blob = m91.parse_glb(h.HANDS_GLB)
mesh = gltf["meshes"][0]
prim = mesh["primitives"][0]
pos = m91.read_accessor(gltf, blob, prim["attributes"]["POSITION"])
nrm = m91.read_accessor(gltf, blob, prim["attributes"]["NORMAL"])
uv = m91.read_accessor(gltf, blob, prim["attributes"]["TEXCOORD_0"])

print("=== UV ===")
print("U range: %.4f .. %.4f" % (uv[:,0].min(), uv[:,0].max()))
print("V range: %.4f .. %.4f" % (uv[:,1].min(), uv[:,1].max()))
u_out = np.mean((uv[:,0] < -0.001) | (uv[:,0] > 1.001))
v_out = np.mean((uv[:,1] < -0.001) | (uv[:,1] > 1.001))
print("frac U outside [0,1]: %.4f" % u_out)
print("frac V outside [0,1]: %.4f" % v_out)

print("\n=== mesh node world matrix ===")
mesh_node = next(i for i, nd in enumerate(gltf["nodes"]) if nd.get("mesh") == 0)
wm = h.world_matrices(gltf)[mesh_node]
print(wm)
print("det =", np.linalg.det(wm[:3,:3]))
print("scale-ish col norms:", np.linalg.norm(wm[:3,0]), np.linalg.norm(wm[:3,1]), np.linalg.norm(wm[:3,2]))

print("\n=== raw normals ===")
ln = np.linalg.norm(nrm, axis=1)
print("norm len min/max/mean: %.4f %.4f %.4f" % (ln.min(), ln.max(), ln.mean()))

print("\n=== raw pos ===")
print("min", pos.min(axis=0), "max", pos.max(axis=0))

# Check for degenerate triangles (zero area) which cause shading artifacts
idx = m91.read_accessor(gltf, blob, prim["indices"]).ravel().astype(np.int64)
tris = idx.reshape(-1,3)
p = pos[tris]
e1 = p[:,1]-p[:,0]; e2 = p[:,2]-p[:,0]
area = 0.5*np.linalg.norm(np.cross(e1,e2), axis=1)
print("\n=== triangles ===")
print("count", len(tris), "zero-area:", int(np.sum(area < 1e-12)), "tiny(<1e-8):", int(np.sum(area < 1e-8)))

# Check if node has a non-identity matrix or scale
node = gltf["nodes"][mesh_node]
print("\nmesh node keys:", sorted(node.keys()))
if "scale" in node: print("node scale:", node["scale"])
if "rotation" in node: print("node rotation:", node["rotation"])
if "translation" in node: print("node translation:", node["translation"])
if "matrix" in node: print("node matrix:", node["matrix"])
