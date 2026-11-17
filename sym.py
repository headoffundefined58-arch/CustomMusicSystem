"""Resolve addresses inside CustomMusicSystem.dll to function names via dbghelp + our PDB."""
import ctypes as C
import sys

dbghelp = C.WinDLL("dbghelp.dll")

MAX_SYM_NAME = 2000


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


HANDLE = C.c_void_p(0x1234)
dbghelp.SymInitializeW(HANDLE, C.c_wchar_p(r"D:\dev\CMS\build\Release;D:\dev\CMS"), False)


def load(dll_path, base):
    r = dbghelp.SymLoadModuleExW(HANDLE, None, C.c_wchar_p(dll_path), None,
                                 C.c_ulonglong(base), 0x200000, None, 0)
    if not r:
        print("SymLoadModuleEx #%d for %s" % (C.GetLastError(), dll_path))
    return r


def resolve(addr):
    buf = SYMBOL_INFO()
    buf.SizeOfStruct = 88
    buf.MaxNameLen = MAX_SYM_NAME
    disp = C.c_ulonglong(0)
    if dbghelp.SymFromAddrW(HANDLE, C.c_ulonglong(addr), C.byref(disp), C.byref(buf)):
        return buf.Name.decode("utf-8", "replace"), disp.value, buf.ModBase
    return None, 0, 0


def line_of(addr):
    ln = IMAGEHLP_LINE64()
    ln.SizeOfStruct = C.sizeof(IMAGEHLP_LINE64)
    disp = C.c_ulong(0)
    if dbghelp.SymGetLineFromAddr64(HANDLE, C.c_ulonglong(addr), C.byref(disp), C.byref(ln)):
        return ln.FileName.decode("utf-8", "replace") if ln.FileName else "?", ln.LineNumber
    return None, 0


if __name__ == "__main__":
    dll = sys.argv[1]
    base = int(sys.argv[2], 0)
    load(dll, base)
    for a in sys.argv[3:]:
        addr = int(a, 0)
        nm, disp, mb = resolve(addr)
        f, lno = line_of(addr)
        if nm:
            print("%#x  %s + %#x   (%s:%d)" % (addr, nm, disp, f, lno))
        else:
            print("%#x  <no symbol>" % addr)
