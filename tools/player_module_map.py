#!/usr/bin/env python3
"""READ-ONLY: type every module hanging off the PLAYER's module container.

Why this exists
---------------
docs/astra/results/DEFLECT_STATUS.md 6.5 proved ownership of the guard signal by
typing a handful of the container's slots (+0x00 ActionFlag, +0x28 Behavior,
+0x48 Toughness, +0x80 ActionRequest, +0x90 PlayerHitStop, +0x98 PlayerDamage).
It never typed the REST of that table. For prosthetic-tool and wire-action work
the question is exactly "is there a module for this, and where in the table",
and the plaintext RTTI already names two strong candidates:

    NS_SPRJ::SprjChrMagicModule    RVA 0x2A84F80   (Sekiro calls tools "magic";
                                                    cf. the EzState command name
                                                    GetCurrEquipMagicID)
    NS_SPRJ::CSWireActionModule    RVA 0x2A90870

This script answers it by observation rather than by assumption: it walks the
container the same proven way the guard reader does (a container that also holds
SprjPlayerDamageModule, the player-specific concrete type), then looks up each
slot's vftable RVA in the complete RTTI map extracted in
docs/astra/results/static/vftable_map.json.

It only reads. It never writes to the game, never injects, never patches.

Usage:
  python tools/player_module_map.py --pid <sekiro pid>
  python tools/player_module_map.py --pid <pid> --grep Wire
"""
from __future__ import annotations

import argparse
import ctypes
import json
import struct
import sys
from pathlib import Path

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
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


def player_ins(reader: Reader, base: int):
    world = reader.qword(base + WORLD_CHR_MAN_SLOT_RVA)
    if not plausible(world):
        return None, "WorldChrMan null (title screen / loading?)"
    if reader.qword(world) != base + WORLD_CHR_MAN_VFTABLE_RVA:
        return None, "WorldChrMan vptr mismatch"
    player = reader.qword(world + PLAYER_INS_OFFSET)
    if not plausible(player):
        return None, "PlayerIns null"
    if reader.qword(player) != base + PLAYER_INS_VFTABLE_RVA:
        return None, "PlayerIns vptr mismatch"
    return player, None


def find_container(reader: Reader, base: int, player: int):
    """The container that holds BOTH a SprjPlayerDamageModule (player-specific
    concrete type -- the ownership proof) and the known ActionFlagModule anchor.
    Same rule as SekiroPlayerGuardReader; nothing new is assumed here."""
    want_damage = base + PLAYER_DAMAGE_VFTABLE_RVA
    want_flag = base + ACTION_FLAG_VFTABLE_RVA
    seen = {player}
    frontier = [player]
    for depth in range(SEARCH_DEPTH):
        nxt = []
        for node in frontier:
            blk = reader.read(node, SEARCH_WIDTH)
            if blk is None:
                continue
            has_damage = has_flag = False
            for off in range(0, len(blk) - 8, 8):
                p, = struct.unpack_from("<Q", blk, off)
                if not plausible(p):
                    continue
                v = reader.qword(p)
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


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--base", type=lambda x: int(x, 0), default=IMAGE_BASE)
    ap.add_argument("--width", type=lambda x: int(x, 0), default=0x400,
                    help="bytes of the container to type (default 0x400)")
    ap.add_argument("--grep", default=None, help="only print classes matching this text")
    args = ap.parse_args()

    if not VFTABLE_MAP.exists():
        sys.exit(f"missing {VFTABLE_MAP}")
    raw = json.loads(VFTABLE_MAP.read_text(encoding="utf-8"))
    by_rva = {int(k, 16): v for k, v in raw.items()}

    reader = Reader(args.pid)
    try:
        player, why = player_ins(reader, args.base)
        if player is None:
            print(f"cannot resolve PlayerIns: {why}")
            return 1
        print(f"PlayerIns          0x{player:x}")
        container = find_container(reader, args.base, player)
        if container is None:
            print("no player-owned container found (needs SprjPlayerDamageModule "
                  "AND SprjChrActionFlagModule in the same table)")
            return 1
        print(f"module container   0x{container:x}")
        print(f"typing {args.width:#x} bytes of it against "
              f"{len(by_rva)} RTTI vftable entries\n")

        blk = reader.read(container, args.width)
        if blk is None:
            print("could not read the container")
            return 1

        printed = 0
        for off in range(0, len(blk) - 8, 8):
            p, = struct.unpack_from("<Q", blk, off)
            if not plausible(p):
                continue
            vptr = reader.qword(p)
            if vptr is None or not plausible(vptr):
                continue
            rva = vptr - args.base
            name = by_rva.get(rva)
            if name is None:
                continue
            if args.grep and args.grep.lower() not in name.lower():
                continue
            print(f"  +0x{off:<5x} 0x{p:012x}  vftRVA 0x{rva:<8x} {name}")
            printed += 1

        print(f"\n{printed} typed slot(s).")
        print("A slot appearing here proves the PLAYER owns an object of that "
              "class.\nIt says nothing yet about which field inside it means what.")
    finally:
        reader.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
