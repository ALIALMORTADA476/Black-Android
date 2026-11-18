import struct
p = r"E:\ai\black\tools\iso_out_bg1_ak1.db"
d = open(p, "rb").read()
print("size", len(d))
for magic in (0x1000,0x1001,0x1002,0x1003,0x0010,0x0011,0x0012,0x0016,0x0500,0x0501,0x0800,0x2000,0x0B):
    pat = struct.pack("<I", magic)
    hits = []
    start = 0
    while True:
        i = d.find(pat, start)
        if i < 0: break
        hits.append(i)
        start = i + 1
        if len(hits) > 8: break
    if hits:
        print(hex(magic), "hits", hits[:8])
# float triples plausibility
import numpy as np
a = np.frombuffer(d[:len(d)//4*4], dtype="<f4")
mask = np.isfinite(a) & (np.abs(a) < 4.0) & (np.abs(a) > 1e-6)
print("plausible floats", int(mask.sum()), "of", len(a))
# look for long runs
run = 0; best = (0,0)
for i,v in enumerate(mask):
    if v: run += 1
    else:
        if run > best[0]: best = (run, i-run)
        run = 0
print("longest run", best)
print("sample region", a[best[1]:best[1]+24] if best[0] else None)
