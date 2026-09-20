"""READ-ONLY diagnostic: for each surviving candidate byte, walk backwards to find
the containing object's vftable pointer and match it against the vftable RVAs that
were extracted statically from sekiro.exe's plaintext RTTI.

Never writes to the target process. OpenProcess uses VM_READ|QUERY_INFORMATION only.
"""
import ctypes, ctypes.wintypes as w, struct, glob, os, sys
from collections import Counter

PID = int(sys.argv[1]) if len(sys.argv) > 1 else 10132
BASE = 0x140000000
SCAN_DIR = r"E:\sekiro_scan\deflect-live-01"

KNOWN = {
    BASE + 0x2a7da48: "SprjPlayerDamageModule",
    BASE + 0x2a7d628: "SprjEnemyDamageModule",
    BASE + 0x2a7cd70: "SprjChrDamageModule",
    BASE + 0x2a72158: "SprjChrActionFlagModule",
    BASE + 0x2a729d8: "SprjChrActionRequestModule",
    BASE + 0x2a8f2d8: "CSChrToughnessModule",
    BASE + 0x2a790a8: "SprjChrBehaviorModule",
    BASE + 0x2a83598: "SprjPlayerHitStopModule",
}
# .rdata of sekiro.exe -- where every vftable lives
RDATA_LO = BASE + 0x2929000
RDATA_HI = BASE + 0x2929000 + 0x11cfe68

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400

k32.OpenProcess.restype = w.HANDLE
k32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k32.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, PID)
if not h:
    sys.exit(f"OpenProcess failed: {ctypes.get_last_error()}")


def read(addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t(0)
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got))
    if not ok or got.value != size:
        return None
    return buf.raw


files = sorted(glob.glob(os.path.join(SCAN_DIR, "candidates-*.bin")))
path = files[-1]
raw = open(path, "rb").read()
tcode, = struct.unpack_from("<I", raw, 12)
VS = {0: 1, 1: 2, 2: 4, 3: 4, 4: 4}.get(tcode, 1)
body = raw[32:len(raw) - 24]
rec = 8 + VS
n = len(body) // rec
addrs = [struct.unpack_from("<Q", body, i * rec)[0] for i in range(n)]
print(f"file={os.path.basename(path)}  candidates={n}\n")

BACK = 0x1800  # how far back to look for the object header

hits = []
vptr_counter = Counter()
for a in addrs:
    start = a - BACK
    blk = read(start, BACK + 16)
    if blk is None:
        # fall back to a shorter window if the long one crosses an unmapped page
        for shorter in (0xC00, 0x400, 0x100):
            blk = read(a - shorter, shorter + 16)
            if blk is not None:
                start = a - shorter
                break
    if blk is None:
        hits.append((a, None, None, "unreadable"))
        continue
    # nearest preceding qword that points into sekiro.exe .rdata
    found = None
    limit = a - start
    for off in range((limit // 8) * 8, -8, -8):
        if off + 8 > len(blk):
            continue
        q, = struct.unpack_from("<Q", blk, off)
        if RDATA_LO <= q < RDATA_HI:
            found = (start + off, q)
            break
    if found:
        objaddr, vptr = found
        vptr_counter[vptr] += 1
        hits.append((a, objaddr, vptr, KNOWN.get(vptr, "")))
    else:
        hits.append((a, None, None, "no vptr found"))

print(f"{'candidate':>16} {'objStart':>16} {'fieldOff':>9} {'vptr':>16}  class")
named = 0
for a, o, v, name in hits:
    if v is None:
        continue
    if name:
        named += 1
    print(f"0x{a:014x} 0x{o:014x} {a-o:9d} 0x{v:014x}  {name}")

print(f"\n--- candidates with a known-class vptr: {named} ---")
print(f"--- distinct vptrs seen: {len(vptr_counter)} ---")
print("\nmost common vptrs (vptr, how many candidates sit behind it):")
for v, c in vptr_counter.most_common(15):
    print(f"  0x{v:014x}  x{c}  {KNOWN.get(v, 'unknown class (vftable RVA 0x%x)' % (v - BASE))}")

unread = sum(1 for _, o, v, nm in hits if nm == "unreadable")
novptr = sum(1 for _, o, v, nm in hits if nm == "no vptr found")
print(f"\nunreadable={unread}  no-vptr-found={novptr}")
