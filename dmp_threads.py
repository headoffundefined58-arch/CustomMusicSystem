"""List every thread in the dump: tid, rip, and whether our plugin appears on its stack."""
import struct
import sys

CONTEXT_RSP = 0x98
CONTEXT_RIP = 0xF8


def main():
    path = sys.argv[1]
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
    if 4 in streams:
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

    # memory ranges for stack reads
    mem = []
    if 5 in streams:
        s, r = streams[5]
        cnt = struct.unpack_from("<I", d, r)[0]
        off = r + 4
        for _ in range(cnt):
            start, dsize, drva = struct.unpack_from("<QII", d, off)
            mem.append((start, dsize, drva))
            off += 16

    def stack_bytes(start, size):
        for ms, ds, dr2 in mem:
            if ms <= start and start + size <= ms + ds:
                o = start - ms
                return d[dr2 + o:dr2 + o + size]
        for ms, ds, dr2 in mem:
            if ms <= start < ms + ds:
                o = start - ms
                return d[dr2 + o:dr2 + ds]
        return b""

    s, r = streams[3]
    nthreads = struct.unpack_from("<I", d, r)[0]
    off = r + 4
    print("threads:", nthreads)
    hits = []
    for i in range(nthreads):
        tid, susp, pc, prio, teb, st_start, st_size, st_rva, csize, crva = struct.unpack_from("<IIIIQQIIII", d, off)
        ctx = d[crva:crva + csize]
        rip = struct.unpack_from("<Q", ctx, CONTEXT_RIP)[0] if len(ctx) > CONTEXT_RIP + 8 else 0
        rsp = struct.unpack_from("<Q", ctx, CONTEXT_RSP)[0] if len(ctx) > CONTEXT_RSP + 8 else 0
        nm, o = name_of(rip)
        blob = stack_bytes(st_start, min(st_size, 0x1000))
        cms = []
        for j in range(0, max(0, len(blob) - 8), 8):
            v = struct.unpack_from("<Q", blob, j)[0]
            nn, oo = name_of(v)
            if nn and "CustomMusicSystem" in nn:
                cms.append((v - (st_start), oo))
        line = "tid %-6d rip %-28s rsp %#x" % (tid, "%s+%#x" % (nm, o), rsp)
        if cms:
            line += "   CMS frames: " + ", ".join("+%#x(%#x)" % (a, b) for a, b in cms[:6])
        print(line)
        if cms:
            hits.append(tid)
        off += 48
    print("threads with CMS on stack:", hits)


if __name__ == "__main__":
    main()
