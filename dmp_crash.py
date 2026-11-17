"""Minimal minidump reader: exception record + register context + stack scan.

No dbghelp / no cdb on this machine, so we parse the dump by hand:
  * ModuleListStream (4)   -> module base/size/name
  * ExceptionStream (6)    -> exception code, fault address, thread id, context
  * ThreadListStream  (3)  -> thread contexts (for the faulting tid)
  * Memory64ListStream(9)  -> stack bytes so we can scan for return addresses

Usage: python dmp_crash.py <dump.dmp>
"""
import struct
import sys

MODULE_ENTRY = 108
CONTEXT_RSP = 0x98
CONTEXT_RIP = 0xF8


def parse(path):
    data = open(path, "rb").read()
    (sig, ver, nstreams, dir_rva) = struct.unpack_from("<IIII", data, 0)
    assert sig == 0x504D444D, "not a minidump"
    streams = {}
    for i in range(nstreams):
        t, size, rva = struct.unpack_from("<III", data, dir_rva + i * 12)
        streams[t] = (size, rva)

    def read(name_rva):
        ln = struct.unpack_from("<I", data, name_rva)[0]
        return data[name_rva + 4:name_rva + 4 + ln].decode("utf-16-le", "replace")

    # ---- modules
    modules = []
    if 4 in streams:
        size, rva = streams[4]
        n = struct.unpack_from("<I", data, rva)[0]
        off = rva + 4
        for _ in range(n):
            base, imgsize = struct.unpack_from("<QI", data, off)
            namerva = struct.unpack_from("<I", data, off + 20)[0]
            modules.append((base, imgsize, read(namerva) if namerva else "?"))
            off += MODULE_ENTRY

    # ---- exception
    exc = None
    if 6 in streams:
        size, rva = streams[6]
        tid = struct.unpack_from("<I", data, rva)[0]
        code, flags = struct.unpack_from("<II", data, rva + 8)
        fault = struct.unpack_from("<Q", data, rva + 24)[0]
        nparam = struct.unpack_from("<I", data, rva + 32)[0]
        params = list(struct.unpack_from("<15Q", data, rva + 40))[:nparam]
        csize, crva = struct.unpack_from("<II", data, rva + 160)
        ctx = data[crva:crva + csize]
        rip = struct.unpack_from("<Q", ctx, CONTEXT_RIP)[0]
        rsp = struct.unpack_from("<Q", ctx, CONTEXT_RSP)[0]
        exc = dict(tid=tid, code=code, flags=flags, fault=fault, params=params,
                   rip=rip, rsp=rsp, ctx=ctx)

    # ---- memory ranges (for the stack)
    ranges = []
    if 5 in streams:
        size, rva = streams[5]
        n = struct.unpack_from("<I", data, rva)[0]
        off = rva + 4
        for _ in range(n):
            start, dsize, drva = struct.unpack_from("<QII", data, off)
            ranges.append((start, dsize, drva))
            off += 16
    if 9 in streams:
        size, rva = streams[9]
        n = struct.unpack_from("<Q", data, rva)[0]
        base_rva = struct.unpack_from("<Q", data, rva + 8)[0]
        off = rva + 16
        for _ in range(n):
            start, dsize, drva = struct.unpack_from("<QII", data, off)
            ranges.append((start, dsize, drva))
            off += 16
    return modules, exc, ranges, data


def name_of(modules, addr):
    for base, size, name in modules:
        if base <= addr < base + size:
            return name.rsplit("\\", 1)[-1], addr - base
    return None, 0


def main():
    path = sys.argv[1]
    modules, exc, ranges, data = parse(path)
    print("modules:", len(modules), " ranges:", len(ranges))

    if exc:
        print("\n=== exception ===")
        print("thread      :", exc["tid"])
        print("code        : %#010x" % exc["code"])
        print("flags       :", exc["flags"])
        print("fault addr  : %#x" % exc["fault"])
        print("rip         : %#x  rsp: %#x" % (exc["rip"], exc["rsp"]))
        print("params      :", [hex(p) for p in exc["params"]])
        nm, off = name_of(modules, exc["fault"])
        print("fault module: %s + %#x" % (nm, off))
        nm, off = name_of(modules, exc["rip"])
        print("rip module  : %s + %#x" % (nm, off))

    # ---- stack scan around rsp
    if exc:
        rsp = exc["rsp"]
        blob = None
        for start, dsize, drva in ranges:
            if start <= rsp < start + dsize:
                blob = data[drva:drva + dsize]
                base = start
                break
        if blob is None:
            print("stack range for rsp not in dump")
            return
        print("\n=== stack scan from rsp (module!offset, deduped) ===")
        seen = 0
        idx0 = rsp - base
        for i in range(idx0, min(len(blob) - 8, idx0 + 0x1200), 8):
            val = struct.unpack_from("<Q", blob, i)[0]
            nm, off = name_of(modules, val)
            if nm:
                print("  rsp+%04x  %#018x  %s + %#x" % (i - idx0, val, nm, off))
                seen += 1
                if seen >= 80:
                    break


if __name__ == "__main__":
    main()
