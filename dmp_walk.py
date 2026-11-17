"""Proper x64 stack walk of a minidump via dbghelp (StackWalk64) + our memory reader."""
import ctypes as C
import struct
import sys

dbghelp = C.WinDLL("dbghelp.dll")
kernel32 = C.WinDLL("kernel32.dll")

HANDLE = C.c_void_p(0xFFFFFFFFFFFFFFFF)
MAX_SYM_NAME = 2000
MACHINE_AMD64 = 0x8664

CONTEXT_RSP = 0x98
CONTEXT_RBP = 0xA0
CONTEXT_RIP = 0xF8


class ADDRESS64(C.Structure):
    _fields_ = [("Offset", C.c_ulonglong), ("Segment", C.c_ushort), ("Mode", C.c_uint)]


class STACKFRAME64(C.Structure):
    _fields_ = [
        ("AddrPC", ADDRESS64),
        ("AddrReturn", ADDRESS64),
        ("AddrFrame", ADDRESS64),
        ("AddrStack", ADDRESS64),
        ("AddrBStore", ADDRESS64),
        ("FuncTableEntry", C.c_void_p),
        ("Params", C.c_ulonglong * 4),
        ("Far", C.c_int),
        ("Virtual", C.c_int),
        ("Reserved", C.c_ulonglong * 3),
        ("KdHelp", C.c_ulonglong * 4),
    ]


class SYMBOL_INFO(C.Structure):
    _fields_ = [
        ("SizeOfStruct", C.c_ulong),
        ("TypeIndex", C.c_ulong),
        ("Reserved", C.c_ulonglong * 2),
        ("Index", C.c_ulong),
        ("Size", C.c_ulong),
        ("ModBase", C.c_ulonglong),
        ("Flags", C.c_ulong),
        ("Value", C.c_ulonglong),
        ("Address", C.c_ulonglong),
        ("Register", C.c_ulong),
        ("Scope", C.c_ulong),
        ("Tag", C.c_ulong),
        ("NameLen", C.c_ulong),
        ("MaxNameLen", C.c_ulong),
        ("Name", C.c_char * MAX_SYM_NAME),
    ]


class IMAGEHLP_LINE64(C.Structure):
    _fields_ = [
        ("SizeOfStruct", C.c_ulong),
        ("Key", C.c_void_p),
        ("LineNumber", C.c_ulong),
        ("FileName", C.c_char_p),
        ("Address", C.c_ulonglong),
    ]




_PE_CACHE = {}


def _pe(path):
    if path in _PE_CACHE:
        return _PE_CACHE[path]
    try:
        img = open(path, "rb").read()
        e = struct.unpack_from("<I", img, 0x3c)[0]
        coff = e + 4
        nsect = struct.unpack_from("<H", img, coff + 2)[0]
        optsz = struct.unpack_from("<H", img, coff + 16)[0]
        opt = coff + 20
        sect = opt + optsz
        secs = []
        for i in range(nsect):
            o = sect + i * 40
            va, vs, rs, rp = struct.unpack_from("<IIII", img, o + 8)
            secs.append((va, vs, rp, rs))
        _PE_CACHE[path] = (img, secs)
    except Exception:
        _PE_CACHE[path] = (b"", [])
    return _PE_CACHE[path]


READFN = C.WINFUNCTYPE(C.c_int, C.c_void_p, C.c_ulonglong, C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong), C.c_void_p)


class Dump:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        sig, ver, n, dr = struct.unpack_from("<IIII", self.d, 0)
        self.streams = {}
        for i in range(n):
            t, s, r = struct.unpack_from("<III", self.d, dr + i * 12)
            self.streams[t] = (s, r)
        self.modules = []
        self.mem = []
        self._parse()

    def _str(self, rva):
        ln = struct.unpack_from("<I", self.d, rva)[0]
        return self.d[rva + 4:rva + 4 + ln].decode("utf-16-le", "replace")

    def _parse(self):
        s, r = self.streams[4]
        cnt = struct.unpack_from("<I", self.d, r)[0]
        off = r + 4
        for _ in range(cnt):
            base, size = struct.unpack_from("<QI", self.d, off)
            nr = struct.unpack_from("<I", self.d, off + 20)[0]
            self.modules.append((base, size, self._str(nr) if nr else ""))
            off += 108
        if 5 in self.streams:
            s, r = self.streams[5]
            cnt = struct.unpack_from("<I", self.d, r)[0]
            off = r + 4
            for _ in range(cnt):
                st, ds, rva = struct.unpack_from("<QII", self.d, off)
                self.mem.append((st, ds, rva))
                off += 16

    def read(self, addr, size):
        for ms, ds, r2 in self.mem:
            if ms <= addr and addr + size <= ms + ds:
                return self.d[r2 + (addr - ms):r2 + (addr - ms) + size]
        # module image pages are not dumped: fall back to the file on disk so
        # dbghelp can read .pdata and actually unwind
        for base, msize, path in self.modules:
            if base <= addr < base + msize:
                img, secs = _pe(path)
                if not img:
                    return None
                rva = addr - base
                for va, vs, rp, rs in secs:
                    if va <= rva < va + max(vs, rs):
                        o = rp + (rva - va)
                        return img[o:o + size]
                return None
        return None

    def thread_ctx(self, tid=None):
        s, r = self.streams[6]
        etid = struct.unpack_from("<I", self.d, r)[0]
        cs, cr = struct.unpack_from("<II", self.d, r + 160)
        if tid is None or tid == etid:
            return etid, bytearray(self.d[cr:cr + cs])
        s, r = self.streams[3]
        nth = struct.unpack_from("<I", self.d, r)[0]
        off = r + 4
        for _ in range(nth):
            t, susp, pc, prio, teb, st0, sz, srva, cs, cr = struct.unpack_from("<IIIIQQIIII", self.d, off)
            if t == tid:
                return t, bytearray(self.d[cr:cr + cs])
            off += 48
        return None, None


def main():
    path = sys.argv[1]
    tid = int(sys.argv[2]) if len(sys.argv) > 2 else None
    dm = Dump(path)
    tid, ctx = dm.thread_ctx(tid)
    if ctx is None:
        print("thread not found")
        return
    rip = struct.unpack_from("<Q", ctx, CONTEXT_RIP)[0]
    rsp = struct.unpack_from("<Q", ctx, CONTEXT_RSP)[0]
    rbp = struct.unpack_from("<Q", ctx, CONTEXT_RBP)[0]
    if len(sys.argv) > 3:
        rip = int(sys.argv[3], 0)
    if len(sys.argv) > 4:
        rsp = int(sys.argv[4], 0)
    if len(sys.argv) > 5:
        rbp = int(sys.argv[5], 0)
    print("tid %d rip %#x rsp %#x rbp %#x" % (tid, rip, rsp, rbp))

    dbghelp.SymSetOptions(0x2 | 0x4 | 0x10)  # UNDNAME|DEFERRED_LOADS|LOAD_LINES
    dbghelp.SymInitializeW(HANDLE, C.c_wchar_p(r"D:\dev\CMS\build\Release"), False)
    loaded = 0
    for base, size, nm in dm.modules:
        if nm and nm.lower().endswith(".dll") or nm.lower().endswith(".exe"):
            r = dbghelp.SymLoadModuleExW(HANDLE, None, C.c_wchar_p(nm), None,
                                         C.c_ulonglong(base), size, None, 0)
            if r:
                loaded += 1
    print("modules loaded:", loaded)

    def _read(h, addr, buf, size, done, userctx):
        b = dm.read(addr, size)
        if b is None:
            return 0
        C.memmove(buf, b, len(b))
        done[0] = len(b)
        return 1

    cb = READFN(_read)
    dbghelp.StackWalk64.argtypes = [C.c_ulong, C.c_void_p, C.c_void_p, C.POINTER(STACKFRAME64),
                                    C.c_void_p, READFN, C.c_void_p, C.c_void_p, C.c_void_p]
    dbghelp.StackWalk64.restype = C.c_int
    frame = STACKFRAME64()
    C.memset(C.byref(frame), 0, C.sizeof(frame))
    frame.AddrPC.Offset = rip
    frame.AddrPC.Mode = 3
    frame.AddrStack.Offset = rsp
    frame.AddrStack.Mode = 3
    frame.AddrFrame.Offset = rbp
    frame.AddrFrame.Mode = 3

    for i in range(40):
        ok = dbghelp.StackWalk64(MACHINE_AMD64, HANDLE, C.c_void_p(0xFFFFFFFFFFFFFFFF),
                                 C.byref(frame), C.cast(C.c_char_p(bytes(ctx)), C.c_void_p), cb, None, None, None)
        if not ok or not frame.AddrPC.Offset:
            break
        addr = frame.AddrPC.Offset
        sym = SYMBOL_INFO()
        sym.SizeOfStruct = 88
        sym.MaxNameLen = MAX_SYM_NAME
        disp = C.c_ulonglong(0)
        name = "?"
        mod = ""
        for base, size, nm in dm.modules:
            if base <= addr < base + size:
                mod = nm.rsplit("\\", 1)[-1]
                break
        if dbghelp.SymFromAddrW(HANDLE, C.c_ulonglong(addr), C.byref(disp), C.byref(sym)):
            name = "%s+%#x" % (sym.Name.decode("utf-8", "replace"), disp.value)
        ln = IMAGEHLP_LINE64()
        ln.SizeOfStruct = C.sizeof(IMAGEHLP_LINE64)
        d2 = C.c_ulong(0)
        loc = ""
        if dbghelp.SymGetLineFromAddr64(HANDLE, C.c_ulonglong(addr), C.byref(d2), C.byref(ln)):
            if ln.FileName:
                loc = "  [%s:%d]" % (ln.FileName.decode("utf-8", "replace").rsplit("\\", 1)[-1], ln.LineNumber)
        print("%02d %#018x %-28s %s%s" % (i, addr, mod, name, loc))


if __name__ == "__main__":
    main()
