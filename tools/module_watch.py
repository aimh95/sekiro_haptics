#!/usr/bin/env python3
"""READ-ONLY live watcher for named modules on the PLAYER's module container.

This is the discovery instrument for the prosthetic-tool and wire-action work.
It resolves the player's module table exactly the way the verified guard reader
does, then reports every byte that CHANGES inside the modules you name, with a
timestamp and the value before and after.

It decides nothing. It never says "a tool was used" -- it says "offset 0x1C of
SprjPlayerMagicModule went 0 -> 4 at t=12.480s". Turning that into an event is a
later step, and only after the same change has been seen to line up with the
same action across separate play sessions.

It only reads. It never writes to the game, never injects, never patches.

Continuity rules, same as tools/deflect_occurrence_record.py:
  - PlayerIns is re-read EVERY tick; if it changes the whole cache is dropped.
  - Every module's vptr is re-validated EVERY tick.
  - A failed resolve/read is reported as a drop, never as a stale value.
  - After any break, the next sample re-baselines instead of emitting deltas,
    so an object swap cannot manufacture a change.

Usage
-----
  # what can I watch?
  python tools/module_watch.py --pid <pid> --list

  # watch the tool module while you switch prosthetics
  python tools/module_watch.py --pid <pid> --module SprjPlayerMagicModule \
      --seconds 60 --window 0x400

  # watch several at once, and log to a file for offline analysis
  python tools/module_watch.py --pid <pid> \
      --module SprjPlayerMagicModule --module CSWireActionModule \
      --module SprjPlayerFallModule --seconds 180 --out capture.jsonl

Press F5 (or --mark-key) to drop a labelled marker into the log while playing.
A marker is when YOU pressed a key. It is NOT the time the game did anything,
and nothing here treats it as ground truth.
"""
from __future__ import annotations

import argparse
import ctypes
import json
import struct
import sys
import time
from pathlib import Path

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32 = ctypes.WinDLL("user32", use_last_error=True)
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400

IMAGE_BASE = 0x140000000
WORLD_CHR_MAN_SLOT_RVA = 0x3D7A1E0
PLAYER_INS_OFFSET = 0x88
WORLD_CHR_MAN_VFTABLE_RVA = 0x2A30058
PLAYER_INS_VFTABLE_RVA = 0x2A2B338
PLAYER_DAMAGE_VFTABLE_RVA = 0x2A7DA48
ACTION_FLAG_VFTABLE_RVA = 0x2A72158

SEARCH_WIDTH = 0x2000
SEARCH_DEPTH = 2
CONTAINER_WIDTH = 0x400

VFTABLE_MAP = Path(__file__).resolve().parents[1] / "docs/astra/results/static/vftable_map.json"


class Reader:
    def __init__(self, pid: int):
        self.h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
        if not self.h:
            raise SystemExit(f"OpenProcess({pid}) failed: {ctypes.get_last_error()}")
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


class PlayerModules:
    """Resolves the player's module container and the named modules in it.

    The container is only accepted when it holds a SprjPlayerDamageModule -- the
    player-specific concrete type -- together with the known ActionFlagModule
    anchor. That is the same ownership proof DEFLECT_STATUS.md 6.5 used, and it
    is what keeps "the first object with the right vptr" from being someone
    else's."""

    def __init__(self, reader: Reader, base: int, by_rva: dict, wanted_names):
        self.r = reader
        self.base = base
        self.by_rva = by_rva
        self.wanted = list(wanted_names)
        self.rva_of = {}
        for rva, name in by_rva.items():
            short = name.split("::")[-1]
            if short in self.wanted and short not in self.rva_of:
                self.rva_of[short] = rva
        missing = [n for n in self.wanted if n not in self.rva_of]
        if missing:
            raise SystemExit("not in the RTTI map: " + ", ".join(missing))
        self.player = None
        self.container = None
        self.addrs = {}
        self.generation = 0
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

    def _find_container(self, player):
        want_damage = self.base + PLAYER_DAMAGE_VFTABLE_RVA
        want_flag = self.base + ACTION_FLAG_VFTABLE_RVA
        seen = {player}
        frontier = [player]
        for depth in range(SEARCH_DEPTH):
            nxt = []
            for node in frontier:
                blk = self.r.read(node, SEARCH_WIDTH)
                if blk is None:
                    continue
                has_damage = has_flag = False
                for off in range(0, len(blk) - 8, 8):
                    p, = struct.unpack_from("<Q", blk, off)
                    if not plausible(p):
                        continue
                    v = self.r.qword(p)
                    if v is None:
                        continue
                    if v == want_damage:
                        has_damage = True
                    elif v == want_flag:
                        has_flag = True
                    if p not in seen:
                        seen.add(p)
                        if depth + 1 < SEARCH_DEPTH:
                            nxt.append(p)
                if has_damage and has_flag:
                    return node
            frontier = nxt
        return None

    def _modules_in(self, container):
        blk = self.r.read(container, CONTAINER_WIDTH)
        if blk is None:
            return None
        found = {}
        for off in range(0, len(blk) - 8, 8):
            p, = struct.unpack_from("<Q", blk, off)
            if not plausible(p):
                continue
            v = self.r.qword(p)
            if v is None:
                continue
            rva = v - self.base
            name = self.by_rva.get(rva)
            if name is None:
                continue
            short = name.split("::")[-1]
            if short in self.rva_of and short not in found:
                found[short] = (p, off)
        return found if all(n in found for n in self.wanted) else None

    def resolve(self):
        """(dict name -> (address, container offset), None) or (None, reason)."""
        player, why = self._player_ins()
        if player is None:
            self.player = self.container = None
            self.addrs = {}
            return None, why
        if player != self.player:
            # PlayerIns replaced: every cached module is suspect even if its
            # vftable pointer still reads correctly (a freed allocation keeps
            # the old vptr -- DEFLECT_STATUS.md 7.8 bug 1).
            self.player = player
            self.container = None
            self.addrs = {}
            self.generation += 1
        if self.addrs:
            ok = all(self.r.qword(addr) == self.base + self.rva_of[name]
                     for name, (addr, _o) in self.addrs.items())
            if ok:
                return self.addrs, None
            self.addrs = {}
            self.container = None
            self.generation += 1
        self.searches += 1
        container = self._find_container(player)
        if container is None:
            return None, "no player-owned container"
        found = self._modules_in(container)
        if found is None:
            return None, "container does not hold every requested module"
        self.container = container
        self.addrs = found
        return self.addrs, None


class ClassObjects:
    """Resolves objects by RTTI class that are NOT in the module table.

    Equipment and save-side data (EquipMagicData, PlayerGameData ...) hang off
    a different root than the ChrIns module table, so PlayerModules cannot see
    them. This walks outward from PlayerIns breadth-first, which makes the
    SHALLOWEST path win -- that matters because several instances of these
    classes exist (a live one plus save/menu copies), and "whichever we met
    first" would otherwise depend on traversal order.

    The path is remembered and re-walked every tick, with the final object's
    vptr re-validated. Any failure forces a fresh walk rather than a stale read.
    """

    def __init__(self, reader: Reader, base: int, by_rva: dict, wanted_names,
                 depth=4, width=0x1000, budget=20000):
        self.r = reader
        self.base = base
        self.by_rva = by_rva
        self.wanted = list(wanted_names)
        self.depth = depth
        self.width = width
        self.budget = budget
        self.vptr_of = {}
        for rva, name in by_rva.items():
            short = name.split("::")[-1]
            if short in self.wanted:
                self.vptr_of.setdefault(short, base + rva)
        missing = [n for n in self.wanted if n not in self.vptr_of]
        if missing:
            raise SystemExit("not in the RTTI map: " + ", ".join(missing))
        self.paths = {}
        self.player = None
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

    def _walk(self, player, path):
        node = player
        for off in path:
            node = self.r.qword(node + off)
            if not plausible(node):
                return None
        return node

    def _search(self, player):
        from collections import deque
        remaining = dict(self.vptr_of)
        found = {}
        seen = {player}
        queue = deque([(player, [])])
        visited = 0
        while queue and visited < self.budget and remaining:
            node, path = queue.popleft()
            if len(path) >= self.depth:
                continue
            blk = self.r.read(node, self.width)
            visited += 1
            if blk is None:
                continue
            for off in range(0, len(blk) - 8, 8):
                p, = struct.unpack_from("<Q", blk, off)
                if not plausible(p) or p in seen:
                    continue
                seen.add(p)
                v = self.r.qword(p)
                if v is None:
                    continue
                for name, want in list(remaining.items()):
                    if v == want:
                        found[name] = path + [off]
                        del remaining[name]
                if plausible(v):
                    queue.append((p, path + [off]))
        return found

    def resolve(self):
        """(dict name -> (address, path), None) or (None, reason)."""
        player, why = self._player_ins()
        if player is None:
            self.player = None
            self.paths = {}
            return None, why
        if player != self.player:
            self.player = player
            self.paths = {}
        if self.paths:
            out = {}
            ok = True
            for name, path in self.paths.items():
                addr = self._walk(player, path)
                if addr is None or self.r.qword(addr) != self.vptr_of[name]:
                    ok = False
                    break
                out[name] = (addr, path)
            if ok:
                return out, None
            self.paths = {}
        self.searches += 1
        found = self._search(player)
        if len(found) != len(self.wanted):
            missing = [n for n in self.wanted if n not in found]
            return None, "not reachable from PlayerIns: " + ", ".join(missing)
        self.paths = found
        out = {}
        for name, path in found.items():
            addr = self._walk(player, path)
            if addr is None:
                return None, f"path to {name} went null"
            out[name] = (addr, path)
        return out, None


class CombinedResolver:
    """Whatever mix of module-table modules and class-located objects was asked
    for, resolved together so one tick sees one consistent set."""

    def __init__(self, modules, classes):
        self.modules = modules
        self.classes = classes
        self.generation = 0
        self.container = None
        self.searches = 0
        self._last = None

    def resolve(self):
        out = {}
        if self.modules is not None:
            got, why = self.modules.resolve()
            if got is None:
                return None, why
            out.update({n: (a, o) for n, (a, o) in got.items()})
            self.container = self.modules.container
        if self.classes is not None:
            got, why = self.classes.resolve()
            if got is None:
                return None, why
            for n, (a, path) in got.items():
                out[n] = (a, path[-1] if path else 0)
        self.searches = ((self.modules.searches if self.modules else 0)
                         + (self.classes.searches if self.classes else 0))
        gen = ((self.modules.generation if self.modules else 0),
               tuple(sorted((n, a) for n, (a, _o) in out.items())))
        if self._last is None or gen[0] != self._last[0]:
            self.generation += 1
        self._last = gen
        return out, None


def list_modules(reader: Reader, base: int, by_rva: dict):
    resolver = PlayerModules(reader, base, by_rva, [])
    player, why = resolver._player_ins()
    if player is None:
        print(f"cannot resolve PlayerIns: {why}")
        return 1
    container = resolver._find_container(player)
    if container is None:
        print("no player-owned container found")
        return 1
    blk = reader.read(container, CONTAINER_WIDTH)
    print(f"PlayerIns 0x{player:x}   container 0x{container:x}\n")
    for off in range(0, len(blk) - 8, 8):
        p, = struct.unpack_from("<Q", blk, off)
        if not plausible(p):
            continue
        v = reader.qword(p)
        if v is None or not plausible(v):
            continue
        name = by_rva.get(v - base)
        if name:
            print(f"  +0x{off:<5x} {name.split('::')[-1]}")
    return 0


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--base", type=lambda x: int(x, 0), default=IMAGE_BASE)
    ap.add_argument("--module", action="append", default=[],
                    help="module-table class short name, e.g. SprjPlayerMagicModule "
                         "(repeatable)")
    ap.add_argument("--class", dest="classes", action="append", default=[],
                    help="class short name located by walking out from PlayerIns, "
                         "for objects that are NOT in the module table, e.g. "
                         "EquipMagicData (repeatable)")
    ap.add_argument("--class-depth", type=int, default=4)
    ap.add_argument("--window", type=lambda x: int(x, 0), default=0x400,
                    help="bytes of each module to watch (default 0x400)")
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--interval-ms", type=float, default=5.0)
    ap.add_argument("--out", type=Path, default=None, help="also write JSONL here")
    ap.add_argument("--mark-key", type=lambda x: int(x, 0), default=0x74,
                    help="virtual-key code for the marker hotkey (default 0x74 = F5)")
    ap.add_argument("--list", action="store_true", help="print the module table and exit")
    ap.add_argument("--quiet-pointers", action="store_true", default=True,
                    help="hide 8-byte-aligned changes that look like pointers (default on)")
    ap.add_argument("--show-pointers", dest="quiet_pointers", action="store_false")
    args = ap.parse_args()

    if not VFTABLE_MAP.exists():
        sys.exit(f"missing {VFTABLE_MAP}")
    by_rva = {int(k, 16): v for k, v in json.loads(VFTABLE_MAP.read_text(encoding="utf-8")).items()}

    reader = Reader(args.pid)
    log = args.out.open("w", encoding="utf-8") if args.out else None

    def emit(obj):
        if log:
            log.write(json.dumps(obj, ensure_ascii=False) + "\n")

    try:
        if args.list:
            return list_modules(reader, args.base, by_rva)
        if not args.module and not args.classes:
            sys.exit("--module or --class is required (or --list)")

        modules = PlayerModules(reader, args.base, by_rva, args.module) if args.module else None
        classes = (ClassObjects(reader, args.base, by_rva, args.classes, depth=args.class_depth)
                   if args.classes else None)
        resolver = CombinedResolver(modules, classes)
        watched = args.module + args.classes
        print(f"watching {', '.join(watched)}  window={args.window:#x} "
              f"interval={args.interval_ms}ms for {args.seconds}s")
        print("press F5 to drop a marker. Ctrl+C to stop.\n")
        emit({"type": "header", "modules": args.module, "window": args.window,
              "intervalMs": args.interval_ms})

        previous = {}
        last_generation = -1
        started = time.perf_counter()
        next_tick = started
        mark_down = False
        marks = 0
        drops = 0
        changes = 0

        while time.perf_counter() - started < args.seconds:
            now = time.perf_counter()
            if now < next_tick:
                time.sleep(0.0005)
                continue
            next_tick = max(now, next_tick + args.interval_ms / 1000.0)
            t = now - started

            down = bool(user32.GetAsyncKeyState(args.mark_key) & 0x8000)
            if down and not mark_down:
                marks += 1
                print(f"  [{t:8.3f}] ---- MARKER {marks} (a key press, not a game event)")
                emit({"type": "marker", "t": t, "index": marks})
            mark_down = down

            addrs, why = resolver.resolve()
            if addrs is None:
                drops += 1
                if drops % 200 == 1:
                    print(f"  [{t:8.3f}] dropped: {why}")
                emit({"type": "dropped", "t": t, "reason": why})
                previous = {}          # force a re-baseline; never diff across a gap
                continue

            if resolver.generation != last_generation:
                where = (f"container 0x{resolver.container:x} "
                         if resolver.container else "")
                print(f"  [{t:8.3f}] === generation {resolver.generation} " + where
                      + "  ".join(f"{n}@0x{a:x}" for n, (a, _o) in addrs.items()))
                emit({"type": "generation", "t": t, "generation": resolver.generation,
                      "container": resolver.container,
                      "modules": {n: {"addr": a, "containerOffset": o}
                                  for n, (a, o) in addrs.items()}})
                last_generation = resolver.generation
                previous = {}

            for name, (addr, _off) in addrs.items():
                blk = reader.read(addr, args.window)
                if blk is None:
                    drops += 1
                    previous.pop(name, None)
                    emit({"type": "dropped", "t": t, "reason": f"read {name}"})
                    continue
                old = previous.get(name)
                previous[name] = blk
                if old is None:
                    emit({"type": "baseline", "t": t, "module": name,
                          "addr": addr, "data": blk.hex()})
                    continue
                if old == blk:
                    continue
                run_start = None
                for i in range(len(blk)):
                    if blk[i] != old[i]:
                        if run_start is None:
                            run_start = i
                    elif run_start is not None:
                        _report(name, run_start, i, old, blk, t, emit, args)
                        changes += 1
                        run_start = None
                if run_start is not None:
                    _report(name, run_start, len(blk), old, blk, t, emit, args)
                    changes += 1

        print(f"\ndone. changes={changes} markers={marks} drops={drops} "
              f"searches={resolver.searches} generations={resolver.generation}")
        print("Nothing above is an event. It is raw movement in memory.")
        emit({"type": "end", "changes": changes, "markers": marks, "drops": drops})
    except KeyboardInterrupt:
        print("\nstopped.")
    finally:
        reader.close()
        if log:
            log.close()
    return 0


def _report(name, start, end, old, new, t, emit, args):
    width = end - start
    # An 8-byte aligned qword that changed wholesale is usually a pointer or a
    # timestamp; those dominate the output and are rarely the flag being hunted.
    if args.quiet_pointers and width == 8 and start % 8 == 0:
        o, = struct.unpack_from("<Q", old, start)
        n, = struct.unpack_from("<Q", new, start)
        if plausible(o) or plausible(n):
            emit({"type": "change", "t": t, "module": name, "offset": start,
                  "old": old[start:end].hex(), "new": new[start:end].hex(),
                  "pointerLike": True})
            return
    ohex = old[start:end].hex()
    nhex = new[start:end].hex()
    extra = ""
    if width <= 4:
        ov = int.from_bytes(old[start:end], "little")
        nv = int.from_bytes(new[start:end], "little")
        extra = f"   ({ov} -> {nv})"
    print(f"  [{t:8.3f}] {name:<26} +0x{start:<4x} {width}B  {ohex} -> {nhex}{extra}")
    emit({"type": "change", "t": t, "module": name, "offset": start,
          "old": ohex, "new": nhex})


if __name__ == "__main__":
    raise SystemExit(main())
