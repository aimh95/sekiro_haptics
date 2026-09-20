"""READ-ONLY: resolve WorldChrMan from the live (decrypted) .text using the repo's
AOB, then find a stable pointer path down to the player's module table. Never writes."""
import ctypes, ctypes.wintypes as w, struct, sys, collections

PID = 10132
BASE = 0x140000000
TEXT_RVA, TEXT_SZ = 0x1000, 0x291bc47
MODULE_TABLE = 0x7ff4a5979260
ACTIONFLAG_OBJ = 0x7ff4a5979470
RL, RH = BASE + 0x2929000, BASE + 0x2929000 + 0x11cfe68
DATA_LO, DATA_HI = BASE + 0x3af9000, BASE + 0x3af9000 + 0x44ff64

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


# ---- 1. WorldChrMan via the repo's AOB, read from LIVE (decrypted) .text -----
print("=== 1. WorldChrMan 해석 (라이브 .text에서 AOB) ===")
text = bytearray()
off = 0
while off < TEXT_SZ:
    n = min(1 << 22, TEXT_SZ - off)
    blk = rd(BASE + TEXT_RVA + off, n)
    if blk is None:
        blk = b"\x00" * n
    text += blk
    off += n
print(f"    .text {len(text)/1e6:.1f} MB 읽음 (디스크와 달리 복호화된 상태)")

# 48 8B 35 ?? ?? ?? ?? 44 0F 28 18
pat_pre, pat_post = b"\x48\x8b\x35", b"\x44\x0f\x28\x18"
hits = []
i = 0
while True:
    i = text.find(pat_pre, i)
    if i < 0:
        break
    if text[i + 7:i + 11] == pat_post:
        disp = struct.unpack_from("<i", text, i + 3)[0]
        static_va = BASE + TEXT_RVA + i + 7 + disp
        hits.append((BASE + TEXT_RVA + i, static_va))
    i += 1
print(f"    AOB 일치: {len(hits)}개")
world = None
for site, sva in hits:
    val = rq(sva)
    print(f"    site=0x{site:x} (RVA 0x{site-BASE:x})  static=0x{sva:x} (RVA 0x{sva-BASE:x})  -> 0x{val if val else 0:x}")
    if val and 0x10000 < val < 0x7FFFFFFFFFFF:
        world = (sva, val)
if not world:
    print("    !! WorldChrMan 확정 실패")
else:
    print(f"    => WorldChrMan static slot RVA 0x{world[0]-BASE:x}, instance 0x{world[1]:x}")

# ---- 2. module table 주변 구조 ------------------------------------------------
print("\n=== 2. 모듈 테이블 주변 (ChrIns 시작점 찾기) ===")
for back in (0x200, 0x800, 0x2000, 0x8000):
    blk = rd(MODULE_TABLE - back, back)
    if not blk:
        continue
    found = None
    for o in range(len(blk) - 8, -8, -8):
        if o + 8 > len(blk):
            continue
        q, = struct.unpack_from("<Q", blk, o)
        if RL <= q < RH:
            found = (MODULE_TABLE - back + o, q)
            break
    if found:
        print(f"    가장 가까운 앞쪽 vftable 포인터: 0x{found[0]:x} (테이블-0x{MODULE_TABLE-found[0]:x})  vptr=0x{found[1]:x} rva=0x{found[1]-BASE:x}")
        print(f"    => 이것이 ChrIns 객체 시작일 가능성. class={KNOWN.get(found[1],'미지')}")
        break
else:
    print("    앞쪽 32KB에 vftable 없음")

# ---- 3. WorldChrMan에서 내려가는 경로 BFS -------------------------------------
if world:
    print("\n=== 3. WorldChrMan -> 모듈 테이블 경로 탐색 (BFS, 깊이 4) ===")
    targets = {MODULE_TABLE: "모듈테이블", ACTIONFLAG_OBJ: "ActionFlagModule"}
    root = world[1]
    # node -> (path list of offsets)
    seen = {root: []}
    frontier = [(root, [])]
    WIDTH = 0x1200  # bytes of each object to walk
    found_paths = []
    for depth in range(4):
        nxt = []
        for node, path in frontier:
            blk = rd(node, WIDTH)
            if not blk:
                continue
            for o in range(0, len(blk) - 8, 8):
                q, = struct.unpack_from("<Q", blk, o)
                if q in targets:
                    found_paths.append((path + [o], q, targets[q]))
                    continue
                if not (0x10000 < q < 0x7FFFFFFFFFFF):
                    continue
                if q in seen:
                    continue
                if depth < 3:
                    seen[q] = path + [o]
                    nxt.append((q, path + [o]))
        print(f"    depth {depth+1}: 방문 {len(nxt)} 노드, 누적 {len(seen)}")
        if found_paths:
            break
        frontier = nxt
        if len(seen) > 400000:
            print("    (탐색 폭 한계 도달)")
            break
    if found_paths:
        print(f"\n    *** 경로 {len(found_paths)}개 발견 ***")
        for path, tgt, name in found_paths[:10]:
            chain = " -> ".join(f"+0x{o:x}" for o in path)
            print(f"    WorldChrMan(0x{root:x}) {chain}  => 0x{tgt:x} ({name})")
    else:
        print("    깊이 4 안에서 경로 없음")
