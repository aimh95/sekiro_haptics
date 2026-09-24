#!/usr/bin/env python3
"""READ-ONLY: the player's equipped prosthetic slots and which one is selected.

What is OBSERVED here (on this build, this session)
---------------------------------------------------
  PlayerGameData + 0x5e8   an array of 8-byte records {u32 ?, u32 equipId}.
                           Slot 0 and slot 1 held 70000 and 75000, which the
                           public catalogue names Loaded Shuriken and Sabimaru.
                           These are FULL EquipParamWeapon row ids -- unlike the
                           u16 2449/2701 seen earlier at EquipMagicData +0xf2,
                           which are NOT row ids and are still unexplained.
  PlayerGameData + 0x518   an EquipGameData (vptr-checked)
  PlayerGameData + 0x500   an EquipMagicData (vptr-checked)

WHY THE OFFSETS AND NOT A CLASS SEARCH
--------------------------------------
Searching for "the nearest EquipMagicData reachable from PlayerIns" returned a
DIFFERENT instance on each run (0x7ff49e7ced30, then 0x7ff4a211aa50 whose
fields were all 0xFF). There are several live instances of these classes and
breadth-first order is not a rule about which one is the player's. So the two
offsets above are used instead and the object at each is vptr-checked before
anything is read out of it -- a wrong offset then fails loudly instead of
returning a plausible-looking wrong number.

PlayerGameData itself is still located by class, because exactly one instance
was reachable from PlayerIns.

WHAT THIS DOES NOT ESTABLISH
----------------------------
  - That the record's first u32 is an inventory handle, a durability, or
    anything else. It is printed raw and left unnamed.
  - Which field holds the SELECTED slot. Candidates are printed with their
    values so the answer can be decided by switching prosthetics and looking,
    not by assumption.
  - Anything at all about a tool being USED. This is selection state only.

It only reads. It never writes to the game, never injects, never patches.

Usage:
  python tools/prosthetic_state.py --pid <pid>
  python tools/prosthetic_state.py --pid <pid> --watch 60
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from module_watch import (ClassObjects, Reader, VFTABLE_MAP, IMAGE_BASE,  # noqa: E402
                          plausible)

CATALOG = Path(__file__).resolve().parents[1] / "docs/Sekiro_Prosthetic_ID_Catalog.json"

# Observed on this build, 2026-09-20. Offsets from PlayerGameData.
EQUIP_SLOT_ARRAY = 0x5E8      # 8-byte records {u32 unknown, u32 equipParamWeaponId}
EQUIP_SLOT_STRIDE = 8
EQUIP_SLOT_ID_OFFSET = 4
EQUIP_GAME_DATA = 0x518
EQUIP_MAGIC_DATA = 0x500
# Candidate "which slot is selected" fields, printed rather than trusted.
SELECTION_CANDIDATES = [("EquipGameData+0xa0", EQUIP_GAME_DATA, 0xA0),
                        ("EquipGameData+0xa4", EQUIP_GAME_DATA, 0xA4)]


def load_catalog():
    raw = json.loads(CATALOG.read_text(encoding="utf-8"))
    names, families = {}, {}
    for entry in raw.get("entries", []):
        ident, name = entry.get("equipParamWeaponId"), entry.get("nameEn")
        if isinstance(ident, int) and isinstance(name, str):
            names[ident] = name
            families[ident] = entry.get("family")
    return names, families


class ProstheticState:
    def __init__(self, reader, base, by_rva):
        self.r = reader
        self.base = base
        self.by_rva = by_rva
        self.names, self.families = load_catalog()
        self.gamedata = ClassObjects(reader, base, by_rva, ["PlayerGameData"], depth=4)
        self.vptr_of = {}
        for rva, name in by_rva.items():
            short = name.split("::")[-1]
            self.vptr_of.setdefault(short, base + rva)

    def _child(self, player_game_data, offset, expect):
        """Pointer at PlayerGameData+offset, only returned if its vptr matches."""
        ptr = self.r.qword(player_game_data + offset)
        if not plausible(ptr):
            return None, f"+0x{offset:x} is not a pointer"
        vptr = self.r.qword(ptr)
        if vptr != self.vptr_of.get(expect):
            actual = self.by_rva.get((vptr or 0) - self.base, "unreadable/unknown")
            return None, f"+0x{offset:x} is a {actual}, not {expect}"
        return ptr, None

    def read(self, slots=8):
        got, why = self.gamedata.resolve()
        if got is None:
            return None, why
        pgd = got["PlayerGameData"][0]
        out = {"playerGameData": pgd, "slots": [], "selection": {}, "children": {}}

        blk = self.r.read(pgd + EQUIP_SLOT_ARRAY, slots * EQUIP_SLOT_STRIDE)
        if blk is None:
            return None, "could not read the equipped-slot array"
        for index in range(slots):
            first, ident = struct.unpack_from("<II", blk, index * EQUIP_SLOT_STRIDE)
            out["slots"].append({"index": index, "raw0": first, "equipId": ident,
                                 "name": self.names.get(ident),
                                 "family": self.families.get(ident)})

        for label, child_off, field_off in SELECTION_CANDIDATES:
            expect = "EquipGameData" if child_off == EQUIP_GAME_DATA else "EquipMagicData"
            child, why = self._child(pgd, child_off, expect)
            out["children"][expect] = child if child else why
            if child is None:
                out["selection"][label] = None
                continue
            raw = self.r.read(child + field_off, 4)
            out["selection"][label] = struct.unpack("<I", raw)[0] if raw else None

        # Raw windows, diffed by the watcher. Guessing which field holds the
        # selected slot has already cost one empty 60 s run, so the whole
        # neighbourhood is compared instead.
        out["raw"] = {}
        for label, offset, expect in (("PlayerGameData", 0, None),
                                      ("EquipGameData", EQUIP_GAME_DATA, "EquipGameData"),
                                      ("EquipMagicData", EQUIP_MAGIC_DATA, "EquipMagicData")):
            if expect is None:
                blob = self.r.read(pgd + EQUIP_SLOT_ARRAY - 0x40, 0x100)
                out["raw"][label + "+0x5a8"] = blob.hex() if blob else None
                continue
            child, _why = self._child(pgd, offset, expect)
            if child is None:
                continue
            blob = self.r.read(child, 0x120)
            out["raw"][label + "+0x0"] = blob.hex() if blob else None
        return out, None


def describe(state, names):
    lines = [f"PlayerGameData 0x{state['playerGameData']:x}"]
    for label, value in state["children"].items():
        lines.append(f"  {label}: " + (f"0x{value:x}" if isinstance(value, int) else str(value)))
    lines.append("  equipped prosthetic slots (PlayerGameData+0x5e8, 8-byte records):")
    for slot in state["slots"]:
        ident = slot["equipId"]
        if ident == 0xFFFFFFFF:
            lines.append(f"    slot {slot['index']}: empty")
            continue
        name = slot["name"] or "unknown (not in the reference catalogue)"
        lines.append(f"    slot {slot['index']}: {ident:<8} {name}"
                     f"   [{slot['family'] or 'unknown'}]   raw0={slot['raw0']}")
    lines.append("  selection candidates:")
    for label, value in state["selection"].items():
        lines.append(f"    {label} = {value}")
    return "\n".join(lines)


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--base", type=lambda x: int(x, 0), default=IMAGE_BASE)
    ap.add_argument("--slots", type=int, default=6)
    ap.add_argument("--watch", type=float, default=0.0,
                    help="seconds to keep printing whenever something changes")
    args = ap.parse_args()

    by_rva = {int(k, 16): v for k, v in json.loads(
        VFTABLE_MAP.read_text(encoding="utf-8")).items()}
    reader = Reader(args.pid)
    try:
        state = ProstheticState(reader, args.base, by_rva)
        got, why = state.read(args.slots)
        if got is None:
            print(f"cannot read: {why}")
            return 1
        print(describe(got, state.names))
        print("\nNames come from the public reference catalogue. An id present there is "
              "NOT\nthe same fact as having seen that prosthetic on this build.")

        if args.watch > 0:
            print(f"\nwatching for {args.watch}s -- switch prosthetics now\n")
            previous = json.dumps(got, sort_keys=True)
            started = time.perf_counter()
            while time.perf_counter() - started < args.watch:
                time.sleep(0.05)
                now, why = state.read(args.slots)
                if now is None:
                    continue
                current = json.dumps(now, sort_keys=True)
                if current == previous:
                    continue
                elapsed = time.perf_counter() - started
                old_raw = json.loads(previous).get("raw", {})
                previous = current
                print(f"[{elapsed:7.3f}] change")
                for label, hexed in (now.get("raw") or {}).items():
                    before = old_raw.get(label)
                    if not before or not hexed or before == hexed:
                        continue
                    a, b = bytes.fromhex(before), bytes.fromhex(hexed)
                    base = 0x5A8 if label.endswith("0x5a8") else 0
                    for i in range(min(len(a), len(b))):
                        if a[i] != b[i]:
                            print(f"    {label.split('+')[0]} +0x{base + i:<5x} "
                                  f"{a[i]:02x} -> {b[i]:02x}   ({a[i]} -> {b[i]})")
                print(describe(now, state.names))
                print()
    finally:
        reader.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
