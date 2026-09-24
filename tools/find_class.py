#!/usr/bin/env python3
"""READ-ONLY: find live objects of a named RTTI class reachable from PlayerIns.

tools/player_module_map.py types the player's MODULE table. Not everything the
player owns is in that table -- equipment and save-side data (EquipGameData,
EquipMagicData, PlayerGameData ...) hang off a different root. This walks
outward from PlayerIns and reports every object whose vftable matches the class
you name, together with the pointer path that reached it, so the path can be
re-derived next session instead of being hardcoded.

It only reads. It never writes to the game, never injects, never patches.

Usage:
  python tools/find_class.py --pid <pid> --class EquipMagicData
  python tools/find_class.py --pid <pid> --class PlayerGameData --depth 4
"""
from __future__ import annotations

import argparse
import ctypes
import json
import struct
import sys
from collections import deque
from pathlib import Path

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400

IMAGE_BASE = 0x140000000
WORLD_CHR_MAN_SLOT_RVA = 0x3D7A1E0
PLAYER_INS_OFFSET = 0x88
WORLD_CHR_MAN_VFTABLE_RVA = 0x2A30058
PLAYER_INS_VFTABLE_RVA = 0x2A2B338

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


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--base", type=lambda x: int(x, 0), default=IMAGE_BASE)
    ap.add_argument("--class", dest="klass", required=True,
                    help="RTTI short name, e.g. EquipMagicData")
    ap.add_argument("--depth", type=int, default=3)
    ap.add_argument("--width", type=lambda x: int(x, 0), default=0x1000,
                    help="bytes scanned per object (default 0x1000)")
    ap.add_argument("--budget", type=int, default=60000, help="max objects visited")
    ap.add_argument("--from-static", type=lambda x: int(x, 0), default=None,
                    help="start from this static slot RVA instead of PlayerIns")
    ap.add_argument("--from-address", type=lambda x: int(x, 0), default=None,
                    help="start from this absolute address (from an earlier run)")
    args = ap.parse_args()

    by_rva = {int(k, 16): v for k, v in json.loads(VFTABLE_MAP.read_text(encoding="utf-8")).items()}
    wanted = [rva for rva, name in by_rva.items() if name.split("::")[-1] == args.klass]
    if not wanted:
        sys.exit(f"{args.klass} is not in the RTTI map")
    want_vptrs = {args.base + rva for rva in wanted}
    print(f"looking for {args.klass}  vftRVA " + ", ".join(f"0x{r:x}" for r in wanted))

    reader = Reader(args.pid)
    try:
        if args.from_address is not None:
            root = args.from_address
            label = f"0x{root:x}"
        elif args.from_static is not None:
            root = reader.qword(args.base + args.from_static)
            if not plausible(root):
                print("static slot is null")
                return 1
            label = f"[base+0x{args.from_static:x}]"
        else:
            world = reader.qword(args.base + WORLD_CHR_MAN_SLOT_RVA)
            if not plausible(world) or reader.qword(world) != args.base + WORLD_CHR_MAN_VFTABLE_RVA:
                print("WorldChrMan unavailable (title screen / loading?)")
                return 1
            root = reader.qword(world + PLAYER_INS_OFFSET)
            if not plausible(root) or reader.qword(root) != args.base + PLAYER_INS_VFTABLE_RVA:
                print("PlayerIns unavailable")
                return 1
            label = "PlayerIns"
        print(f"root {label} 0x{root:x}  depth={args.depth} width={args.width:#x}\n")

        seen = {root}
        queue = deque([(root, [])])
        visited = 0
        found = 0
        while queue and visited < args.budget:
            node, path = queue.popleft()
            if len(path) >= args.depth:
                continue
            blk = reader.read(node, args.width)
            visited += 1
            if blk is None:
                continue
            for off in range(0, len(blk) - 8, 8):
                p, = struct.unpack_from("<Q", blk, off)
                if not plausible(p) or p in seen:
                    continue
                seen.add(p)
                v = reader.qword(p)
                if v is None:
                    continue
                if v in want_vptrs:
                    found += 1
                    chain = " -> ".join(f"+0x{o:x}" for o in path + [off])
                    print(f"  0x{p:012x}   {label} {chain}")
                    if found >= 20:
                        print("  (stopping at 20)")
                        queue.clear()
                        break
                if plausible(v):
                    queue.append((p, path + [off]))

        print(f"\n{found} instance(s); visited {visited} object(s).")
        if not found:
            print("Not reachable within this depth/width. Try --depth 4, a wider "
                  "--width, or a different root -- absence here is NOT proof the "
                  "class has no live instance.")
    finally:
        reader.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
