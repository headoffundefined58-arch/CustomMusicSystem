"""Raw stack dump for one thread, with module annotation."""
import struct
import sys

CONTEXT_RSP = 0x98


def main():
    path, want_tid = sys.argv[1], int(sys.argv[2])
    span = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x400
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
        modules.append((base, imgsize, rd_str(namerva).rsplit("\\", 1)[-1] if namerva else "?"))
        off += 108

    def name_of(addr):
        for base, size, nm in modules:
            if base <= addr < base + size:
                return nm, addr - base
        return None, 0

    mem = []
    if 5 in streams:
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
        tid, susp, pc, prio, teb, st_start, st_size, st_rva, csize, crva = struct.unpack_from("<IIIIQQIIII", d, off)
        if tid == want_tid:
            ctx = d[crva:crva + csize]
            rsp = struct.unpack_from("<Q", ctx, CONTEXT_RSP)[0]
            rip = struct.unpack_from("<Q", ctx, 0xF8)[0]
            print("tid %d rip %s+%#x rsp %#x" % (tid, *name_of(rip), rsp))
            blob = read_mem(rsp, span)
            if blob is None:
                print("stack not in dump")
                return
            for i in range(0, span - 8, 8):
                v = struct.unpack_from("<Q", blob, i)[0]
                nm, o = name_of(v)
                if nm:
                    print("  rsp+%04x %#018x  %s + %#x" % (i, v, nm, o))
            return
        off += 48


if __name__ == "__main__":
    main()
