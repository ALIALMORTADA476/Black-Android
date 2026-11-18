import numpy as np, struct
p = r"E:\ai\black\tools\iso_out_bg1_ak1.db"
d = open(p, "rb").read()
a = np.frombuffer(d[:len(d)//4*4], dtype="<f4")
mask = np.isfinite(a) & (np.abs(a) < 3.0) & (np.abs(a) > 1e-7)
runs = []
i = 0
while i < len(mask):
    if not mask[i]:
        i += 1
        continue
    j = i
    while j < len(mask) and mask[j]:
        j += 1
    if j - i >= 24:
        v = a[i:j]
        v = v[:len(v)//3*3].reshape(-1,3)
        runs.append((j-i, i*4, v.min(axis=0), v.max(axis=0)))
    i = j
runs.sort(reverse=True)
for n, off, mn, mx in runs[:25]:
    print("n=%4d off=0x%X  min=%s max=%s" % (n, off, np.array2string(mn, precision=3), np.array2string(mx, precision=3)))
