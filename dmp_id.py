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
    modulelist = rs(4)
    nmod = struct.unpack_from("<I", modulelist, 0)[0]
    modules = []
    off = 4
    for _ in range(nmod):
        ent = modulelist[off:off + 112]
        base, size = struct.unpack_from("<Q I", ent, 0)
        modules.append((base, size, ent))
        off += 108

    f.seek(0)
    blob = f.read(96 * 1024 * 1024)

    def read_path_at(idx, enc):
        if enc == "utf-16-le":
            end = blob.find(b"\x00\x00", idx)
            return blob[idx:end].decode("utf-16-le", errors="replace")
        end = blob.find(b"\x00", idx)
        return blob[idx:end].decode("ascii", errors="replace")

    known = {}
    for key in ("CustomMusicSystem", "SKSEMenuFramework", "cimgui", "ImGui",
                "d3d11", "d3d12", "XAudio2", "x3daudio", "D3DCOMPILER"):
        for enc in ("utf-16-le", "ascii"):
            try:
                needle = key.encode(enc)
            except Exception:
                continue
            i = blob.find(needle)
            if i >= 0:
                s = read_path_at(i, enc)
                # find module entry whose 112 bytes contain 4-byte LE == i
                for b, sz, ent in modules:
                    found = False
                    for o in range(0, 108, 4):
                        if struct.unpack_from("<I", ent, o)[0] == i:
                            found = True; break
                    if found:
                        known[key] = (b, sz, s)
                        break
                break

    print("=== known module identities ===", flush=True)
    for k, v in known.items():
        print(k, "-> base", hex(v[0]), "size", hex(v[1]), "path:", v[2][:90], flush=True)

    chain_bases = {0x7ffad4240000, 0x7ffad3230000, 0x7ffad3820000}
    print("=== modules overlapping the dense chain bases ===", flush=True)
    for b, sz, ent in modules:
        if b in chain_bases:
            # try to read its name via known map first, else scan
            nm = None
            for k, v in known.items():
                if v[0] == b: nm = v[2][:60]
            print("MATCH base", hex(b), "size", hex(sz), "name:", nm, flush=True)

    print("=== plugin-sized modules (base : size) ===", flush=True)
    cnt = 0
    for b, sz, ent in modules:
        if 0x40000 <= sz <= 0x600000:
            tag = ""
            for k, v in known.items():
                if v[0] == b: tag = "  <= " + k
            print(hex(b), ":", hex(sz), tag, flush=True)
            cnt += 1
            if cnt > 90: break

if __name__ == "__main__":
    main()
