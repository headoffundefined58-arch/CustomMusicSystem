"""Call-site filtered stack scan: keeps only qwords that look like return addresses."""
import struct
import sys

_PE = {}


def pe(path):
    if path in _PE:
        return _PE[path]
    try:
        img = open(path, "rb").read()
        e = struct.unpack_from("<I", img, 0x3c)[0]
        coff = e + 4
        nsect = struct.unpack_from("<H", img, coff + 2)[0]
        optsz = struct.unpack_from("<H", img, coff + 16)[0]
        sect = coff + 20 + optsz
        secs = []
        for i in range(nsect):
            o = sect + i * 40
            va, vs, rs, rp = struct.unpack_from("<IIII", img, o + 8)
            secs.append((va, vs, rp, rs))
        _PE[path] = (img, secs)
    except Exception:
        _PE[path] = (b"", [])
    return _PE[path]


def byte_at(path, rva):
    img, secs = pe(path)
    for va, vs, rp, rs in secs:
        if va <= rva < va + max(vs, rs):
            o = rp + (rva - va)
            if 0 <= o < len(img):
                return img[o]
    return None


def is_call_site(path, rva):
    """Return the call instruction length if [rva-len, rva) ends in a call."""
    for back, kind in ((5, "rel32"), (2, "ind")):
        b = byte_at(path, rva - back)
        if b is None:
            continue
        if kind == "rel32" and b == 0xE8:
            return back, "call rel32"
        if kind == "ind" and b == 0xFF:
            modrm = byte_at(path, rva - 1)
            if modrm is not None and ((modrm >> 3) & 7) in (2, 3):
                return back, "call [reg]"
    return None


def main():
    path, tid = sys.argv[1], int(sys.argv[2])
    span = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0xC00
    d = open(path, "rb").read()
    sig, ver, n, dr = struct.unpack_from("<IIII", d, 0)
    streams = {}
    for i in range(n):
        t, s, r = struct.unpack_from("<III", d, dr + i * 12)
        streams[t] = (s, r)

    def rd(rva):
        ln = struct.unpack_from("<I", d, rva)[0]
        return d[rva + 4:rva + 4 + ln].decode("utf-16-le", "replace")

    mods = []
    s, r = streams[4]
    cnt = struct.unpack_from("<I", d, r)[0]
    off = r + 4
    for _ in range(cnt):
        base, size = struct.unpack_from("<QI", d, off)
        nr = struct.unpack_from("<I", d, off + 20)[0]
        mods.append((base, size, rd(nr) if nr else ""))
        off += 108

    mem = []
    s, r = streams[5]
    cnt = struct.unpack_from("<I", d, r)[0]
    off = r + 4
    for _ in range(cnt):
        st, ds, rva = struct.unpack_from("<QII", d, off)
        mem.append((st, ds, rva))
        off += 16

    def read_mem(addr, size):
        for ms, ds, r2 in mem:
            if ms <= addr and addr + size <= ms + ds:
                return d[r2 + (addr - ms):r2 + (addr - ms) + size]
        return None

    s, r = streams[3]
    nth = struct.unpack_from("<I", d, r)[0]
    off = r + 4
    rsp = None
    for _ in range(nth):
        t, susp, pc, prio, teb, st0, sz, srva, cs, cr = struct.unpack_from("<IIIIQQIIII", d, off)
        if t == tid:
            ctx = d[cr:cr + cs]
            rsp = struct.unpack_from("<Q", ctx, 0x98)[0]
            break
        off += 48
    if rsp is None:
        print("no thread")
        return

    low = rsp - span
    blob = read_mem(low, span)
    print("tid %d rsp %#x  (scanning %#x..%#x)" % (tid, rsp, low, rsp))
    for i in range(0, span - 8, 8):
        v = struct.unpack_from("<Q", blob, i)[0]
        for base, size, mp in mods:
            if base <= v < base + size:
                hit = is_call_site(mp, v - base)
                if hit:
                    print("  -%04x %s+%#x  (%s)" % (span - i, mp.rsplit("\\", 1)[-1], v - base, hit[1]))
                break


if __name__ == "__main__":
    main()
