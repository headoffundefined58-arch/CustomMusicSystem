import struct

PATH = r"D:/Lain/My Games/Skyrim Special Edition/SKSE/crash-2026-10-29-20-33-35.dmp"

def main():
    f = open(PATH, "rb")
    hdr = f.read(32)
    num_streams = struct.unpack_from("<I", hdr, 8)[0]
    dir_rva = struct.unpack_from("<I", hdr, 12)[0]
    f.seek(dir_rva)
    dirs = [struct.unpack("<III", f.read(12)) for _ in range(num_streams)]
    def rs(t):
        for tt, ds, r in dirs:
            if tt == t:
                f.seek(r); return f.read(ds)
        return None
    exc = rs(6); threadlist = rs(3); modulelist = rs(4); mem64 = rs(9)
    nrange = struct.unpack_from("<Q", mem64, 0)[0]
    datablock_start = dir_rva_of(9, dirs) + 8 + nrange * 16
    ethread = struct.unpack_from("<I", exc, 0)[0]
    excaddr = struct.unpack_from("<Q", exc, 24)[0]
    print("FaultAddress", hex(excaddr), "ThreadId", ethread, flush=True)

    nmod = struct.unpack_from("<I", modulelist, 0)[0]
    modules = []
    off = 4
    for _ in range(nmod):
        ent = modulelist[off:off + 108]
        base, size = struct.unpack_from("<Q I", ent, 0)
        name_rva = struct.unpack_from("<I", ent, 76)[0]
        modules.append((base, size, name_rva))
        off += 108

    # locate names by searching the first 64MB of the file for UTF-16 / ASCII module paths
    f.seek(0)
    blob = f.read(64 * 1024 * 1024)
    known = {}
    for key in ("CustomMusicSystem", "SKSEMenuFramework", "cimgui", "ImGui", "d3d11", "XAudio", "BSMusic", "D3D12"):
        for enc in ("utf-16-le", "ascii"):
            try:
                needle = key.encode(enc)
            except Exception:
                continue
            i = blob.find(needle)
            if i >= 0:
                rva = i  # file offset == rva for minidump name strings
                for b, s, nr in modules:
                    if nr == rva:
                        known[key] = b
                        break
                break
    for k, v in known.items():
        print("FOUND", k, "=> base", hex(v), flush=True)

    def modof(addr):
        for b, s, nr in modules:
            if b <= addr < b + s:
                nm = None
                for k, v in known.items():
                    if v == b:
                        nm = k
                return b, addr - b, nm
        return (None, None, None)

    # crash thread context
    nthr = struct.unpack_from("<I", threadlist, 0)[0]
    off = 4; crash = None
    for _ in range(nthr):
        td = threadlist[off:off + 48]
        tid = struct.unpack_from("<I", td, 0)[0]
        start = struct.unpack_from("<Q", td, 24)[0]
        ssize = struct.unpack_from("<I", td, 32)[0]
        srva = struct.unpack_from("<I", td, 36)[0]
        ctx_rva = struct.unpack_from("<I", td, 44)[0]
        if tid == ethread:
            crash = (tid, start, ssize, srva, ctx_rva)
        off += 48
    tid, start, ssize, srva, ctx_rva = crash
    f.seek(ctx_rva); ctx = f.read(1232)
    rsp = struct.unpack_from("<Q", ctx, 152)[0]
    print("RSP", hex(rsp), "stackStart", hex(start), "stackSize", ssize, flush=True)

    # RSP raw top 24 qwords (unfiltered) to see the true return address
    f.seek(datablock_start + srva)
    stack = f.read(min(ssize, 16 * 1024 * 1024))
    rel = rsp - start
    print("--- RSP raw top (offset : value) ---", flush=True)
    for i in range(24):
        p = rel + i * 8
        if p + 8 <= len(stack):
            v = struct.unpack_from("<Q", stack, p)[0]
            print(hex(start + p), ":", hex(v), flush=True)

    print("=== call chain (module base+offset, name if known) ===", flush=True)
    pos = rel - (rel % 8)
    seen = 0
    while pos + 8 <= len(stack) and seen < 200:
        val = struct.unpack_from("<Q", stack, pos)[0]
        if val >= 0x180000000:
            b, delta, nm = modof(val)
            if b is not None:
                tag = ("  <== " + nm) if nm else ""
                mark = "  [RSP]" if pos == rel else ""
                print(hex(start + pos), ":", hex(b) + "+" + hex(delta), tag, mark, flush=True)
                seen += 1
        pos += 8

def dir_rva_of(stype, dirs):
    for t, ds, r in dirs:
        if t == stype:
            return r
    return 0

if __name__ == "__main__":
    main()
