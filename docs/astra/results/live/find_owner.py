"""READ-ONLY: find who points at the confirmed SprjChrActionFlagModule object, then
check whether that owner also holds a SprjPlayerDamageModule (proving the module
belongs to the PLAYER and not to an enemy). Never writes to the target."""
import ctypes, ctypes.wintypes as w, struct, sys

PID = 10132
BASE = 0x140000000
TARGET_OBJ = 0x7ff4a5979470          # the SprjChrActionFlagModule instance
PLAYER_DMG = BASE + 0x2a7da48
ENEMY_DMG = BASE + 0x2a7d628
CHR_DMG = BASE + 0x2a7cd70
KNOWN = {
    PLAYER_DMG: "SprjPlayerDamageModule", ENEMY_DMG: "SprjEnemyDamageModule",
    CHR_DMG: "SprjChrDamageModule", BASE + 0x2a72158: "SprjChrActionFlagModule",
    BASE + 0x2a729d8: "SprjChrActionRequestModule", BASE + 0x2a8f2d8: "CSChrToughnessModule",
    BASE + 0x2a790a8: "SprjChrBehaviorModule", BASE + 0x2a83598: "SprjPlayerHitStopModule",
}

k = ctypes.WinDLL("kernel32", use_last_error=True)
k.OpenProcess.restype = w.HANDLE
k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", w.DWORD), ("__a", w.DWORD),
                ("RegionSize", ctypes.c_size_t), ("State", w.DWORD),
                ("Protect", w.DWORD), ("Type", w.DWORD), ("__b", w.DWORD)]


k.VirtualQueryEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
h = k.OpenProcess(0x10 | 0x400, False, PID)
if not h:
    sys.exit("OpenProcess failed")


def rd(a, n):
    b = ctypes.create_string_buffer(n)
    g = ctypes.c_size_t(0)
    if k.ReadProcessMemory(h, ctypes.c_void_p(a), b, n, ctypes.byref(g)):
        return b.raw[:g.value]
    return None


READABLE = {0x02, 0x04, 0x20, 0x40}  # RO, RW, EXEC_READ, EXEC_RW
regions = []
addr = 0
mbi = MBI()
while addr < 0x7FFFFFFF0000:
    if not k.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        break
    if mbi.State == 0x1000 and (mbi.Protect & 0xFF) in READABLE and not (mbi.Protect & 0x100):
        regions.append((mbi.BaseAddress or 0, mbi.RegionSize))
    nxt = (mbi.BaseAddress or 0) + mbi.RegionSize
    if nxt <= addr:
        break
    addr = nxt

needle = struct.pack("<Q", TARGET_OBJ)
print(f"scanning {len(regions)} regions for pointers to 0x{TARGET_OBJ:x} ...")
refs = []
scanned = 0
for base, size in regions:
    off = 0
    while off < size:
        n = min(1 << 22, size - off)
        blk = rd(base + off, n)
        if blk:
            scanned += len(blk)
            i = blk.find(needle)
            while i >= 0:
                refs.append(base + off + i)
                i = blk.find(needle, i + 1)
        off += n
print(f"scanned {scanned/(1<<30):.2f} GiB, found {len(refs)} pointer(s)\n")

for r in refs[:40]:
    print(f"pointer at 0x{r:x}")
    # treat the holder as an object/array and look for sibling module pointers
    for back in (0x800, 0x2000):
        blk = rd(r - back, back + 0x800)
        if not blk:
            continue
        sibs = []
        for off in range(0, len(blk) - 8, 8):
            q, = struct.unpack_from("<Q", blk, off)
            if 0x10000 < q < 0x7FFFFFFFFFFF:
                v = rd(q, 8)
                if v and len(v) == 8:
                    vp, = struct.unpack("<Q", v)
                    if vp in KNOWN:
                        sibs.append((r - back + off, q, KNOWN[vp]))
        if sibs:
            for at, obj, nm in sibs:
                mark = ""
                if nm == "SprjPlayerDamageModule":
                    mark = "   <<< PLAYER"
                elif nm == "SprjEnemyDamageModule":
                    mark = "   <<< ENEMY"
                print(f"    slot 0x{at:x} (+0x{at-r:x} from our ptr) -> 0x{obj:x}  {nm}{mark}")
            break
    else:
        print("    no sibling modules found nearby")
