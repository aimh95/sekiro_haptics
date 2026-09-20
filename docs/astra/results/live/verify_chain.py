"""READ-ONLY: walk the candidate pointer chain node by node, typing each hop by vptr."""
import ctypes, ctypes.wintypes as w, struct, sys

PID = 10132
BASE = 0x140000000
RL, RH = BASE + 0x2929000, BASE + 0x2929000 + 0x11cfe68
WORLD_SLOT_RVA = 0x3d7a1e0
FLAG_OFF = 0xE10

KNOWN = {BASE + 0x2a7da48: "SprjPlayerDamageModule", BASE + 0x2a7d628: "SprjEnemyDamageModule",
         BASE + 0x2a7cd70: "SprjChrDamageModule", BASE + 0x2a72158: "SprjChrActionFlagModule",
         BASE + 0x2a729d8: "SprjChrActionRequestModule", BASE + 0x2a8f2d8: "CSChrToughnessModule",
         BASE + 0x2a790a8: "SprjChrBehaviorModule", BASE + 0x2a83598: "SprjPlayerHitStopModule"}

k = ctypes.WinDLL("kernel32", use_last_error=True)
k.OpenProcess.restype = w.HANDLE
k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
h = k.OpenProcess(0x10 | 0x400, False, PID)
if not h:
    sys.exit("OpenProcess failed")


def rd(a, n):
    b = ctypes.create_string_buffer(n)
    g = ctypes.c_size_t(0)
    if k.ReadProcessMemory(h, ctypes.c_void_p(a), b, n, ctypes.byref(g)):
        return b.raw[:g.value]
    return None


def rq(a):
    v = rd(a, 8)
    return struct.unpack("<Q", v)[0] if v and len(v) == 8 else None


def cls_of(obj):
    v = rq(obj)
    if v is None:
        return "?", None
    if RL <= v < RH:
        return KNOWN.get(v, f"미지(vftable RVA 0x{v-BASE:x})"), v
    return "vptr 아님(일반 구조체/배열)", v


CHAINS = [
    [0x88, 0x10b8, 0x1d0],
    [0x88, 0x1100, 0x190],
    [0x88, 0x1118, 0x150],
    [0x88, 0x1150, 0x130],
    [0x88, 0x1168, 0x0f0],
]

world = rq(BASE + WORLD_SLOT_RVA)
print(f"[sekiro.exe+0x{WORLD_SLOT_RVA:x}] -> WorldChrMan = 0x{world:x}")
c, v = cls_of(world)
print(f"    WorldChrMan vptr=0x{v:x} -> {c}\n")

for chain in CHAINS:
    node = world
    desc = [f"WorldChrMan(0x{world:x})"]
    ok = True
    for i, off in enumerate(chain):
        nxt = rq(node + off)
        if nxt is None or not (0x10000 < nxt < 0x7FFFFFFFFFFF):
            ok = False
            desc.append(f"+0x{off:x} -> BAD")
            break
        c, v = cls_of(nxt)
        desc.append(f"+0x{off:<5x} -> 0x{nxt:012x}  [{c}]")
        node = nxt
    if ok:
        flag = rd(node + FLAG_OFF, 1)
        fv = flag[0] if flag else None
        print(" | ".join(f"+0x{o:x}" for o in chain))
        for d in desc:
            print("      " + d)
        print(f"      +0x{FLAG_OFF:x} = {fv}   <-- 플래그 값\n")
    else:
        print(" | ".join(f"+0x{o:x}" for o in chain), "-> 끊김\n")
