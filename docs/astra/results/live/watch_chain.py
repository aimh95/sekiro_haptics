"""READ-ONLY: resolve the full pointer chain EVERY tick (never cache an absolute
address), re-validate the module's vptr, and log flag transitions plus every
resolution failure / object swap. This is the behaviour a real detector needs."""
import ctypes, ctypes.wintypes as w, struct, sys, time, os

PID = int(sys.argv[1]) if len(sys.argv) > 1 else 10132
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 300.0
BASE = 0x140000000
WORLD_SLOT_RVA = 0x3d7a1e0
CHAIN = [0x88, 0x10b8, 0x1d0]
FLAG_OFF = 0xE10
EXPECT_VPTR = BASE + 0x2a72158            # SprjChrActionFlagModule
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "chain_log.txt")

k = ctypes.WinDLL("kernel32", use_last_error=True)
k.OpenProcess.restype = w.HANDLE
k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
h = k.OpenProcess(0x10 | 0x400, False, PID)
if not h:
    sys.exit("OpenProcess failed")

b8 = ctypes.create_string_buffer(8)
b1 = ctypes.create_string_buffer(1)
g = ctypes.c_size_t(0)


def rq(a):
    if k.ReadProcessMemory(h, ctypes.c_void_p(a), b8, 8, ctypes.byref(g)) and g.value == 8:
        return struct.unpack("<Q", b8.raw)[0]
    return None


def rb(a):
    if k.ReadProcessMemory(h, ctypes.c_void_p(a), b1, 1, ctypes.byref(g)) and g.value == 1:
        return b1.raw[0]
    return None


def resolve():
    n = rq(BASE + WORLD_SLOT_RVA)
    if not n or not (0x10000 < n < 0x7FFFFFFFFFFF):
        return None, "WorldChrMan null"
    for i, off in enumerate(CHAIN):
        n = rq(n + off)
        if not n or not (0x10000 < n < 0x7FFFFFFFFFFF):
            return None, f"hop {i} (+0x{off:x}) null"
    v = rq(n)
    if v != EXPECT_VPTR:
        return None, f"vptr mismatch 0x{v if v else 0:x}"
    return n, None


log = open(OUT, "w", encoding="utf-8")
log.write(f"# chain: [exe+0x{WORLD_SLOT_RVA:x}] -> " + " -> ".join(f"+0x{o:x}" for o in CHAIN) + f" -> +0x{FLAG_OFF:x}\n")
log.write(f"# re-resolved every tick; vptr re-validated against SprjChrActionFlagModule\n")

prev_val = None
prev_obj = None
prev_err = None
t0 = time.perf_counter()
ticks = resolved = failed = 0

while time.perf_counter() - t0 < SECONDS:
    ticks += 1
    t = time.perf_counter() - t0
    obj, err = resolve()
    if obj is None:
        failed += 1
        if err != prev_err:
            log.write(f"[{t:8.3f}] RESOLVE FAIL: {err}\n")
            log.flush()
            prev_err = err
            prev_val = None
        time.sleep(0.03)
        continue
    resolved += 1
    if err != prev_err:
        prev_err = None
    if obj != prev_obj:
        log.write(f"[{t:8.3f}] OBJECT = 0x{obj:x}" + (" (교체됨)" if prev_obj else " (최초 확인)") + "\n")
        log.flush()
        prev_obj = obj
        prev_val = None
    v = rb(obj + FLAG_OFF)
    if v != prev_val:
        if prev_val is not None:
            log.write(f"[{t:8.3f}] FLAG {prev_val} -> {v}   ({'패링 성공' if v == 1 else '일반 방어'})\n")
            log.flush()
        prev_val = v
    time.sleep(0.03)

log.write(f"\n# ticks={ticks} resolved={resolved} failed={failed}\n")
log.close()
print("done ->", OUT)
