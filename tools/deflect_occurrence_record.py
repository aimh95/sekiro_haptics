#!/usr/bin/env python3
"""READ-ONLY high-rate recorder for the CONFIRMED player ActionFlagModule path.

Purpose: look for a signal that marks "a new guard resolution happened", which the
already-confirmed result byte (+0xE10) cannot provide on its own -- it holds its
value, so deflect->deflect and block->block produce no transition at all.

This tool ONLY records raw bytes. It never decides that a deflect happened, never
writes to the target process, and never treats a key press as ground truth.

Output is capture schema v3, byte-for-byte compatible with the C++ recorder in
src/process/SekiroCombatCaptureSession.cpp, so tools/deflect_capture.py and
tools/deflect_signal.py can read it back. Continuity rules are the same ones the
C++ session enforces: the pointer chain is re-resolved and the module vptr
re-validated on EVERY tick, a failed resolve/read is recorded as `dropped` (never
as a stale value), an object swap is recorded as `discontinuity`, a late tick is
recorded as `gap`, and every continuity break forces a `rebaseline` before any
further deltas.

Usage (see docs/astra/results/DEFLECT_STATUS.md for the live procedure):
  python tools/deflect_occurrence_record.py --pid <pid> --out capture.jsonl \
      --seconds 240 --interval-ms 5 --window 0x1800
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import struct
import sys
import time
from pathlib import Path

# ---------------------------------------------------------------------------
# Anchors. Only the two marked STABLE are hardcoded as offsets; everything
# below them is located by a vptr-validated search instead.
#
# DEFLECT_STATUS.md 6.8 recorded a fixed chain
# (PlayerIns +0x10B8 -> +0x1D0 -> ActionFlagModule). Re-checking it on
# 2026-09-19 showed it had stopped resolving: +0x10B8 now points at an
# unrelated structure whose +0x1D0 is not a pointer at all, and the module is
# instead reachable via PlayerIns +0x50 -> +0xB50 (and four other offsets).
# So those deep offsets are NOT a structural invariant and must not be
# hardcoded -- see DEFLECT_STATUS.md 6.9.
# ---------------------------------------------------------------------------
IMAGE_BASE = 0x140000000
WORLD_CHR_MAN_SLOT_RVA = 0x3D7A1E0            # STABLE: AOB-derived static slot
PLAYER_INS_OFFSET = 0x88                      # STABLE: WorldChrMan -> PlayerIns
WORLD_CHR_MAN_VFTABLE_RVA = 0x2A30058         # NS_SPRJ::WorldChrManImp
PLAYER_INS_VFTABLE_RVA = 0x2A2B338            # NS_SPRJ::PlayerIns
ACTION_FLAG_VFTABLE_RVA = 0x2A72158           # NS_SPRJ::SprjChrActionFlagModule
RESULT_FLAG_OFFSET = 0xE10                    # outcome: 0 = block, 1 = deflect

# Occurrence signal, found live 2026-09-18 (DEFLECT_STATUS.md 7.6). A ONE-FRAME
# (~15 ms) pulse inside the same module: 0 -> 1 -> 0 on every guard resolution,
# including repeats the outcome byte cannot show. Polling must be fast enough to
# see it -- at the C++ session's 200 ms default it would be missed entirely.
OCCURRENCE_PULSE_OFFSET = 0x3C
# Corroborating outcome, packed as two int16: 0x00010001 deflect, 0xFFFF0001 block.
OUTCOME_PACKED_OFFSET = 0x64

SEARCH_WIDTH = 0x2000                         # bytes of each node scanned
SEARCH_DEPTH = 2                              # hops below PlayerIns

# Modules recorded when --siblings is given. The outcome byte lives in
# ActionFlagModule, but the "a new resolution happened" signal may not -- the
# modules that process an incoming hit are at least as likely. Each is located
# by its own vptr, never by a hardcoded container offset.
# (name, vftable RVA, bytes to record)
SIBLING_MODULES = [
    ("SprjChrActionFlagModule", 0x2A72158, 0x1000),
    ("SprjPlayerDamageModule", 0x2A7DA48, 0x0800),
    ("SprjPlayerHitStopModule", 0x2A83598, 0x0400),
    ("CSChrToughnessModule", 0x2A8F2D8, 0x0400),
    ("SprjChrActionRequestModule", 0x2A729D8, 0x0400),
    ("SprjChrBehaviorModule", 0x2A790A8, 0x0400),
]

CELL = 4  # capture v3 diffs 4 bytes at a time

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.CloseHandle.argtypes = [wt.HANDLE]
user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
user32.GetAsyncKeyState.restype = ctypes.c_short


class Reader:
    def __init__(self, pid: int):
        self.h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
        if not self.h:
            raise SystemExit(f"OpenProcess({pid}) failed: {ctypes.get_last_error()} "
                             "(게임이 실행 중인지, pid가 맞는지 확인)")
        self._got = ctypes.c_size_t(0)
        self._q = ctypes.create_string_buffer(8)

    def read(self, addr: int, size: int):
        buf = ctypes.create_string_buffer(size)
        ok = k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, size,
                                   ctypes.byref(self._got))
        if not ok or self._got.value != size:
            return None
        return buf.raw

    def qword(self, addr: int):
        ok = k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), self._q, 8,
                                   ctypes.byref(self._got))
        if not ok or self._got.value != 8:
            return None
        return struct.unpack("<Q", self._q.raw)[0]

    def close(self):
        if self.h:
            k32.CloseHandle(self.h)
            self.h = None


def plausible(p) -> bool:
    return p is not None and 0x10000 < p < 0x7FFFFFFFFFFF


class ActionFlagLocator:
    """Resolves the player's SprjChrActionFlagModule with a type check at every
    step. The module pointer is cached, but the cache is re-validated by reading
    its vptr on EVERY tick -- a stale or recycled object fails the check and
    forces a fresh search rather than silently yielding someone else's bytes."""

    def __init__(self, reader: Reader, base: int):
        self.r = reader
        self.base = base
        self.cached = None
        self.searches = 0

    def _player_ins(self):
        world = self.r.qword(self.base + WORLD_CHR_MAN_SLOT_RVA)
        if not plausible(world):
            return None, "WorldChrMan null"
        if self.r.qword(world) != self.base + WORLD_CHR_MAN_VFTABLE_RVA:
            return None, "WorldChrMan vptr mismatch"
        player = self.r.qword(world + PLAYER_INS_OFFSET)
        if not plausible(player):
            return None, "PlayerIns null"
        if self.r.qword(player) != self.base + PLAYER_INS_VFTABLE_RVA:
            return None, "PlayerIns vptr mismatch"
        return player, None

    def _search(self, player):
        want = self.base + ACTION_FLAG_VFTABLE_RVA
        seen = {player}
        frontier = [player]
        for depth in range(SEARCH_DEPTH):
            nxt = []
            for node in frontier:
                blk = self.r.read(node, SEARCH_WIDTH)
                if blk is None:
                    continue
                for o in range(0, len(blk) - 8, 8):
                    p, = struct.unpack_from("<Q", blk, o)
                    if not plausible(p) or p in seen:
                        continue
                    if self.r.qword(p) == want:
                        return p
                    seen.add(p)
                    if depth + 1 < SEARCH_DEPTH:
                        nxt.append(p)
            frontier = nxt
        return None

    def resolve(self):
        """Returns (address, None) or (None, reason). Never returns an address
        whose vptr is not SprjChrActionFlagModule.

        A vptr check alone cannot tell a live object from a freed one that still
        holds the old vftable pointer, so PlayerIns is re-read every call and the
        cache is dropped whenever it changes."""
        want = self.base + ACTION_FLAG_VFTABLE_RVA
        player, why = self._player_ins()
        if player is None:
            self.cached = None
            self.last_player_ins = None
            return None, why
        if player != getattr(self, "last_player_ins", None):
            self.cached = None
            self.last_player_ins = player
        if self.cached is not None and self.r.qword(self.cached) == want:
            return self.cached, None
        self.cached = None
        self.searches += 1
        found = self._search(player)
        if found is None:
            return None, "ActionFlagModule not reachable from PlayerIns"
        self.cached = found
        return found, None


class MultiModuleLocator(ActionFlagLocator):
    """Resolves the player's module set.

    TWO bugs were found live on 2026-09-19 and both are fixed here:

    1. A vptr check alone does NOT prove an object is still live. After an area
       transition the old allocation can still contain a valid vftable pointer,
       so a cached module keeps passing its type check while the game has moved
       on. A 180 s recording read such a dead copy: the outcome byte and the
       pulse never left 0 for 35,295 samples while the player was actually
       deflecting. Fix: PlayerIns is re-resolved every tick and the whole cache
       is dropped the moment PlayerIns changes.

    2. Picking "the first object with the wanted vptr" is arbitrary. Fix: the
       modules are taken from ONE container that also holds
       SprjPlayerDamageModule -- the player-specific concrete type -- which is
       the same type-level ownership proof used in DEFLECT_STATUS.md 6.5.
    """

    PLAYER_DAMAGE_VFTABLE_RVA = 0x2A7DA48

    def __init__(self, reader: Reader, base: int, modules):
        super().__init__(reader, base)
        self.modules = modules
        self.addrs = None
        self.player_ins = None
        self.container = None

    def _wants(self):
        return [self.base + rva for _n, rva, _sz in self.modules]

    def _container_from(self, player):
        """A container that holds BOTH every wanted module and a
        SprjPlayerDamageModule. Returns (container, [addrs]) or None."""
        wants = self._wants()
        want_pd = self.base + self.PLAYER_DAMAGE_VFTABLE_RVA
        seen = {player}
        frontier = [player]
        for depth in range(SEARCH_DEPTH):
            nxt = []
            for node in frontier:
                blk = self.r.read(node, SEARCH_WIDTH)
                if blk is None:
                    continue
                found = {}
                has_player_damage = False
                for o in range(0, len(blk) - 8, 8):
                    p, = struct.unpack_from("<Q", blk, o)
                    if not plausible(p):
                        continue
                    v = self.r.qword(p)
                    if v is None:
                        continue
                    if v == want_pd:
                        has_player_damage = True
                    if v in wants and v not in found:
                        found[v] = p
                    if p not in seen:
                        seen.add(p)
                        if depth + 1 < SEARCH_DEPTH:
                            nxt.append(p)
                if has_player_damage and all(w in found for w in wants):
                    return node, [found[w] for w in wants]
            frontier = nxt
        return None

    def resolve_all(self):
        """Returns (list_of_addresses, None) or (None, reason)."""
        player, why = self._player_ins()
        if player is None:
            self.addrs = self.player_ins = self.container = None
            return None, why
        if player != self.player_ins:
            # PlayerIns was replaced -- every cached module is suspect, even if
            # its vftable pointer still reads correctly.
            self.addrs = None
            self.container = None
            self.player_ins = player
        if self.addrs is not None:
            if all(self.r.qword(a) == w for a, w in zip(self.addrs, self._wants())):
                return self.addrs, None
            self.addrs = self.container = None
        self.searches += 1
        got = self._container_from(player)
        if got is None:
            return None, "no player-owned container holding every module"
        self.container, self.addrs = got
        return self.addrs, None


def to_hex(b: bytes) -> str:
    return b.hex()


class V3Writer:
    """Emits exactly the record shapes src/process/SekiroCombatCaptureSession.cpp
    writes, so tools/deflect_capture.py accepts the file."""

    def __init__(self, path: Path, window: int, interval_us: int, scope: str, t0_us: int):
        self.f = open(path, "w", encoding="utf-8", newline="\n")
        self.window = window
        self.sequence = 0
        self._w({"schemaVersion": 3, "recordKind": "capture_start", "timestampUs": t0_us,
                 "scope": scope, "windowSizeBytes": window, "intervalUs": interval_us,
                 "interpretation": "unvalidated_raw_memory"})

    def _w(self, obj):
        self.f.write(json.dumps(obj, separators=(",", ":")) + "\n")

    def baseline(self, ts, generation, base_addr, reason, data: bytes):
        self._w({"schemaVersion": 3, "recordKind": "baseline", "timestampUs": ts,
                 "generation": generation, "baseAddressHex": f"0x{base_addr:x}",
                 "reason": reason, "bytesHex": to_hex(data)})

    def delta(self, ts, scheduled, generation, offset, prev: bytes, cur: bytes):
        self._w({"schemaVersion": 3, "recordKind": "delta", "timestampUs": ts,
                 "sequence": self.sequence, "generation": generation,
                 "scheduledTimestampUs": scheduled, "offset": offset,
                 "cellSizeBytes": CELL, "previousBytesHex": to_hex(prev),
                 "currentBytesHex": to_hex(cur)})

    def sample(self, ts, scheduled, generation, status):
        self._w({"schemaVersion": 3, "recordKind": "sample", "timestampUs": ts,
                 "sequence": self.sequence, "generation": generation,
                 "scheduledTimestampUs": scheduled, "status": status})
        self.sequence += 1

    def dropped(self, ts, generation, reason):
        self._w({"schemaVersion": 3, "recordKind": "dropped", "timestampUs": ts,
                 "sequence": self.sequence, "generation": generation, "reason": reason})
        self.sequence += 1

    def gap(self, ts, from_ts, missed):
        self._w({"schemaVersion": 3, "recordKind": "gap", "timestampUs": ts,
                 "fromTimestampUs": from_ts, "missedCycles": missed})

    def discontinuity(self, ts, old_gen, new_gen):
        self._w({"schemaVersion": 3, "recordKind": "discontinuity", "timestampUs": ts,
                 "oldGeneration": old_gen, "newGeneration": new_gen})

    def marker(self, ts, processed_ts, label):
        self._w({"schemaVersion": 3, "recordKind": "marker", "timestampUs": ts,
                 "processedTimestampUs": processed_ts,
                 "source": "manual_or_input_annotation", "label": label})

    def end(self, ts):
        self._w({"schemaVersion": 3, "recordKind": "capture_end", "timestampUs": ts,
                 "samplesTaken": self.sequence})
        self.f.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--seconds", type=float, default=240.0)
    ap.add_argument("--interval-ms", type=float, default=5.0,
                    help="polling period; 5ms matches the C++ session's floor")
    ap.add_argument("--window", type=lambda x: int(x, 0), default=0x1800,
                    help="bytes of SprjChrActionFlagModule to record (must cover +0xE10)")
    ap.add_argument("--base", type=lambda x: int(x, 0), default=IMAGE_BASE)
    ap.add_argument("--mark-key", type=lambda x: int(x, 0), default=0x74,
                    help="virtual-key polled for phase markers (default 0x74 = F5)")
    ap.add_argument("--siblings", action="store_true",
                    help="also record the player's damage/hitstop/toughness/action-request/"
                         "behavior modules; writes a <out>.layout.json describing the window")
    args = ap.parse_args()

    if args.out.exists():
        raise SystemExit(f"거부: {args.out} 이미 존재합니다 (기존 기록을 덮어쓰지 않습니다)")
    interval_us = max(1, int(args.interval_ms * 1000))
    reader = Reader(args.pid)

    if args.siblings:
        mods = SIBLING_MODULES
        window = sum(((sz // CELL) * CELL) for _n, _r, sz in mods)
        locator = MultiModuleLocator(reader, args.base, mods)
        scope = "PlayerModuleSet"
        layout, cursor = [], 0
        for name, rva, sz in mods:
            sz = (sz // CELL) * CELL
            layout.append({"name": name, "vftableRva": f"0x{rva:x}",
                           "windowOffset": cursor, "sizeBytes": sz})
            cursor += sz
    else:
        mods = None
        window = (args.window // CELL) * CELL
        locator = ActionFlagLocator(reader, args.base)
        scope = "PlayerActionFlagModule"
        layout = [{"name": "SprjChrActionFlagModule",
                   "vftableRva": f"0x{ACTION_FLAG_VFTABLE_RVA:x}",
                   "windowOffset": 0, "sizeBytes": window}]
    if not (CELL <= window <= 65536):
        raise SystemExit("window must be 4..65536 bytes")
    if window < RESULT_FLAG_OFFSET + CELL:
        raise SystemExit(f"window must cover the result flag at +0x{RESULT_FLAG_OFFSET:x}")

    def acquire():
        """Returns (addresses_tuple, bytes, None) or (None, None, reason)."""
        if mods is None:
            a, why = locator.resolve()
            if a is None:
                return None, None, why
            b = reader.read(a, window)
            return ((a,), b, None) if b is not None else (None, None, "read failed")
        addrs, why = locator.resolve_all()
        if addrs is None:
            return None, None, why
        chunks = []
        for (name, _rva, sz), a in zip(mods, addrs):
            sz = (sz // CELL) * CELL
            c = reader.read(a, sz)
            if c is None:
                return None, None, f"read failed ({name})"
            chunks.append(c)
        return tuple(addrs), b"".join(chunks), None

    t_start = time.perf_counter()

    def now_us():
        return int((time.perf_counter() - t_start) * 1_000_000)

    addrs, data, why = acquire()
    if addrs is None:
        raise SystemExit(f"모듈 해석 실패: {why}\n"
                         "게임이 로딩 중이거나 타이틀 화면이면 인게임으로 들어간 뒤 다시 실행하세요.")
    addr = addrs[0]
    Path(str(args.out) + ".layout.json").write_text(
        json.dumps({"scope": scope, "windowSizeBytes": window,
                    "resultFlagWindowOffset": RESULT_FLAG_OFFSET,
                    "modules": layout}, indent=1), encoding="utf-8")

    generation = 1
    t0 = now_us()
    writer = V3Writer(args.out, window, interval_us, scope, t0)
    writer.baseline(t0, generation, addr, "initial", data)

    print(f"recording -> {args.out}")
    print(f"  scope={scope}  window=0x{window:x}  interval={args.interval_ms}ms")
    for m, a in zip(layout, addrs):
        print(f"    +0x{m['windowOffset']:05x} 0x{a:012x} {m['sizeBytes']:#7x}  {m['name']}")
    print(f"  result flag +0x{RESULT_FLAG_OFFSET:x} = {data[RESULT_FLAG_OFFSET]}")
    print(f"  F5(0x{args.mark_key:x}) 를 누르면 phase 마커가 기록됩니다. Ctrl+C 로 조기 종료.")

    prev_bytes = bytearray(data)
    prev_addrs = addrs
    have_baseline = True
    next_sched = t0 + interval_us
    mark_down = False
    mark_index = 0
    stats = {"samples": 0, "dropped": 0, "gaps": 0, "disc": 0, "markers": 0}
    deadline = args.seconds

    try:
        while (time.perf_counter() - t_start) < deadline:
            # ---- schedule grid, same late/gap policy as the C++ session ----
            now = now_us()
            if now < next_sched:
                time.sleep(min(0.002, (next_sched - now) / 1_000_000))
                continue
            scheduled = next_sched
            missed = (now - scheduled) // interval_us
            next_sched = scheduled + (missed + 1) * interval_us
            if missed > 0:
                writer.gap(now, scheduled, int(missed))
                stats["gaps"] += 1
                have_baseline = False
                prev_bytes = None

            # ---- markers only at a tick boundary, never mid group ----
            pressed = (user32.GetAsyncKeyState(args.mark_key) & 0x8000) != 0
            if pressed and not mark_down:
                mark_index += 1
                writer.marker(now, now, f"phase_{mark_index}")
                stats["markers"] += 1
                print(f"  [{now/1e6:7.2f}s] phase_{mark_index}")
            mark_down = pressed

            # ---- re-resolve and re-type-check EVERY tick, fail closed ----
            addrs, cur, why = acquire()
            if addrs is None:
                writer.dropped(now, generation, why)
                stats["dropped"] += 1
                have_baseline = False
                prev_bytes = None
                continue
            if addrs != prev_addrs:
                writer.discontinuity(now, generation, generation + 1)
                generation += 1
                prev_addrs = addrs
                addr = addrs[0]
                stats["disc"] += 1
                have_baseline = False
                prev_bytes = None
                print(f"  [{now/1e6:7.2f}s] object swapped -> 0x{addrs[0]:x} (generation {generation})")
            addr = addrs[0]

            if not have_baseline:
                writer.baseline(now, generation, addr, "rebaseline", cur)
                writer.sample(now, scheduled, generation, "baseline")
                prev_bytes = bytearray(cur)
                have_baseline = True
                stats["samples"] += 1
                continue

            for off in range(0, window, CELL):
                a = prev_bytes[off:off + CELL]
                b = cur[off:off + CELL]
                if a != b:
                    writer.delta(now, scheduled, generation, off, bytes(a), b)
                    prev_bytes[off:off + CELL] = b
            writer.sample(now, scheduled, generation, "observed")
            stats["samples"] += 1
    except KeyboardInterrupt:
        print("\n  중단됨 (Ctrl+C) -- capture_end 를 기록하고 정상 종료합니다")

    writer.end(now_us())
    reader.close()
    print(f"done. samples={stats['samples']} dropped={stats['dropped']} "
          f"gaps={stats['gaps']} objectSwaps={stats['disc']} markers={stats['markers']} "
          f"moduleSearches={locator.searches}")
    print(f"검증: python tools/deflect_capture.py {args.out}")


if __name__ == "__main__":
    main()
