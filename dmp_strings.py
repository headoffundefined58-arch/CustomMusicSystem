"""Look for CRT error-report strings on a thread's stack.

_invoke_watson receives (expression, function, file, line); those are pointers to
strings living in some module's .rdata.  If we can read them, we learn which CRT
call detected the bad parameter.
"""
import struct
import sys

CONTEXT_RSP = 0x98


def pe_sections(path):
    d = open(path, "rb").read()
    e = struct.unpack_from("<I", d, 0x3c)[0]
    coff = e + 4
    nsect = struct.unpack_from("<H", d, coff + 2)[0]
    optsz = struct.unpack_from("<H", d, coff + 16)[0]
    opt = coff + 20
    sect = opt + optsz
    secs = []
    for i in range(nsect):
        o = sect + i * 40
        va, vsize, rawsize, rawptr = struct.unpack_from("<IIII", d, o + 8)
        secs.append((va, vsize, rawptr, rawsize))
    return d, secs


def read_at(mpath, rva, n=96):
    cache = CACHE.get(mpath)
    if cache is None:
        try:
            cache = pe_sections(mpath)
        except Exception:
            cache = (b"", [])
        CACHE[mpath] = cache
    d, secs = cache
    for va, vsize, rawptr, rawsize in secs:
        if va <= rva < va + max(vsize, rawsize):
            off = rawptr + (rva - va)
            return d[off:off + n]
    return b""


CACHE = {}


def main():
    path, tid = sys.argv[1], int(sys.argv[2])
    d = open(path, "rb").read()
    sig, ver, n, dr = struct.unpack_from("<IIII", d, 0)
    streams = {}
    for i in range(n):
        t, s, r = struct.unpack_from("<III", d, dr + i * 12)
        streams[t] = (s, r)

    def rd_str(rva):
        ln = struct.unpack_from("<I", d, rva)[0]
        return d[rva + 4:rva + 4 + ln].decode("utf-16-le", "replace")

    modules = []
    s, r = streams[4]
    cnt = struct.unpack_from("<I", d, r)[0]
    off = r + 4
    for _ in range(cnt):
        base, imgsize = struct.unpack_from("<QI", d, off)
        namerva = struct.unpack_from("<I", d, off + 20)[0]
        modules.append((base, imgsize, rd_str(namerva) if namerva else ""))
        off += 108

    def find(addr):
        for base, size, nm in modules:
            if base <= addr < base + size:
                return base, nm
        return None

    mem = []
    s, r = streams[5]
    cnt = struct.unpack_from("<I", d, r)[0]
    off = r + 4
    for _ in range(cnt):
        start, dsize, drva = struct.unpack_from("<QII", d, off)
        mem.append((start, dsize, drva))
        off += 16

    def read_mem(addr, size):
        for ms, ds, dr2 in mem:
            if ms <= addr and addr + size <= ms + ds:
                return d[dr2 + (addr - ms):dr2 + (addr - ms) + size]
        return None

    s, r = streams[3]
    nth = struct.unpack_from("<I", d, r)[0]
    off = r + 4
    for _ in range(nth):
        ttid, susp, pc, prio, teb, st_start, st_size, st_rva, csize, crva = struct.unpack_from("<IIIIQQIIII", d, off)
        if ttid == tid:
            ctx = d[crva:crva + csize]
            rsp = struct.unpack_from("<Q", ctx, CONTEXT_RSP)[0]
            blob = read_mem(rsp, 0x400)
            print("tid", tid, "rsp", hex(rsp))
            for i in range(0, 0x400 - 8, 8):
                v = struct.unpack_from("<Q", blob, i)[0]
                hit = find(v)
                if not hit:
                    continue
                base, mpath = hit
                raw = read_at(mpath, v - base, 72)
                txt = raw.split(b"\0")[0]
                if len(txt) < 4:
                    continue
                try:
                    s_ = txt.decode("utf-8")
                except Exception:
                    continue
                if all(32 <= ord(c) < 127 for c in s_):
                    print("  rsp+%04x -> %s+%#x : %r" % (i, mpath.rsplit("\\", 1)[-1], v - base, s_[:70]))
            return
        off += 48


if __name__ == "__main__":
    main()
