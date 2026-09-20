"""READ-ONLY live watcher: polls every surviving candidate byte at ~30 Hz and logs
each transition with a monotonic timestamp, so a scripted deflect/block sequence can
be matched against what each byte actually did. Never writes to the target."""
import ctypes, ctypes.wintypes as w, struct, glob, os, sys, time

PID = int(sys.argv[1])
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 180.0
BASE = 0x140000000
SCAN_DIR = r"E:\sekiro_scan\deflect-live-01"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "watch_log.txt")

FOCUS = 0x7ff4a597a280           # candidate inside SprjChrActionFlagModule
FOCUS_OBJ = 0x7ff4a5979470       # its object start
FOCUS_VPTR = BASE + 0x2a72158    # expected vftable

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = w.HANDLE
k32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k32.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
h = k32.OpenProcess(0x0010 | 0x0400, False, PID)
if not h:
    sys.exit(f"OpenProcess failed: {ctypes.get_last_error()}")

buf1 = ctypes.create_string_buffer(1)
buf8 = ctypes.create_string_buffer(8)
got = ctypes.c_size_t(0)


def rb(addr):
    if k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf1, 1, ctypes.byref(got)) and got.value == 1:
        return buf1.raw[0]
    return None


def rq(addr):
    if k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf8, 8, ctypes.byref(got)) and got.value == 8:
        return struct.unpack("<Q", buf8.raw)[0]
    return None


files = sorted(glob.glob(os.path.join(SCAN_DIR, "candidates-*.bin")))
raw = open(files[-1], "rb").read()
body = raw[32:len(raw) - 24]
addrs = [struct.unpack_from("<Q", body, i * 9)[0] for i in range(len(body) // 9)]
if FOCUS not in addrs:
    addrs.append(FOCUS)

log = open(OUT, "w")
log.write(f"# watching {len(addrs)} candidates, pid={PID}, {SECONDS}s\n")
log.write(f"# FOCUS 0x{FOCUS:x} (SprjChrActionFlagModule +0xE10)\n")

prev = {a: rb(a) for a in addrs}
changes = {a: 0 for a in addrs}
t0 = time.perf_counter()
vptr_ok = True
ticks = 0

while time.perf_counter() - t0 < SECONDS:
    ticks += 1
    now = time.perf_counter() - t0
    v = rq(FOCUS_OBJ)
    if v != FOCUS_VPTR and vptr_ok:
        log.write(f"[{now:8.3f}] !! FOCUS object vptr changed: 0x{v if v else 0:x}\n")
        vptr_ok = False
    for a in addrs:
        cur = rb(a)
        if cur != prev[a]:
            changes[a] += 1
            tag = "  <<< FOCUS" if a == FOCUS else ""
            log.write(f"[{now:8.3f}] 0x{a:012x} {prev[a]} -> {cur}{tag}\n")
            prev[a] = cur
    time.sleep(0.03)

log.write(f"\n# ticks={ticks} elapsed={time.perf_counter()-t0:.1f}s\n")
log.write("# transitions per candidate (top 20):\n")
for a, c in sorted(changes.items(), key=lambda kv: -kv[1])[:20]:
    tag = "  <<< FOCUS" if a == FOCUS else ""
    log.write(f"#   0x{a:012x}  {c}{tag}\n")
quiet = sum(1 for c in changes.values() if c == 0)
log.write(f"# candidates that never changed: {quiet}/{len(addrs)}\n")
log.close()
print("done ->", OUT)
