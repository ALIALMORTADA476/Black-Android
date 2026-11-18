import struct, sys
MAGIC = b"MICROSOFT*XBOX*MEDIA"
iso = sys.argv[1] if len(sys.argv) > 1 else r"E:\ai\black\assets\black.iso"
want = (sys.argv[2] if len(sys.argv) > 2 else "bg1_ak1").lower()
with open(iso, "rb") as f:
    base = None
    for part in (0, 0x18300000, 0xFD90000, 0x2080000):
        f.seek(part + 32*2048)
        if f.read(20) == MAGIC:
            base = part
            break
    if base is None:
        sys.exit("not xbox iso")
    root_sector, root_size = struct.unpack("<II", f.read(8))
    f.seek(base + root_sector*2048)
    table = f.read(root_size)

    def decode(off):
        if off*4 + 14 > len(table): return None
        left, right, sector, size, attr, nlen = struct.unpack_from("<HHIIBB", table, off*4)
        if left == 0xFFFF: return None
        name = table[off*4+14:off*4+14+nlen].decode("latin-1")
        return name, left, right, sector, size, attr

    def walk(off, path):
        e = decode(off)
        if e is None: return
        name, left, right, sector, size, attr = e
        p = path + "/" + name
        if attr & 0x10:
            # directory: data holds a new node table
            f.seek(base + sector*2048)
            dt = f.read(size)
            # stash global table temporarily
            global table
            old = table
            table = dt
            if left: walk(left, p)
            if right: walk(right, p)
            table = old
        else:
            if want in name.lower():
                print(p, size, "sector", sector)
                out = r"E:\ai\black\tools\iso_out_" + name
                f.seek(base + sector*2048)
                with open(out, "wb") as o:
                    o.write(f.read(size))
                print("wrote", out)
        if left: walk(left, path)
        if right: walk(right, path)

    # first just search the current table for fpguns directories by scanning all names
    def scan_all(off, depth=0):
        e = decode(off)
        if e is None or depth > 16: return
        name, left, right, sector, size, attr = e
        if depth <= 5 and attr & 0x10:
            print("  "*depth + "DIR", name)
        if "fpgun" in name.lower() or want in name.lower():
            print(("DIR " if attr & 0x10 else "FILE"), name, size, "attr", hex(attr), "sector", sector)
        if attr & 0x10:
            f.seek(base + sector*2048)
            dt = f.read(size)
            global table
            old = table; table = dt
            scan_all(0, depth+1)
            table = old
        if left: scan_all(left, depth+1)
        if right: scan_all(right, depth+1)
    e = decode(0)
    print("root", e)
    if e:
        print("root left/right", e[1], e[2])
        l = decode(e[1]) if e[1] else None
        r = decode(e[2]) if e[2] else None
        print("first left", l, "first right", r)
    scan_all(0)
