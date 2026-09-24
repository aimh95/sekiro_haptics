#!/usr/bin/env python3
"""READ-ONLY: look for full EquipParamWeapon prosthetic row IDs inside a live object.

Why this exists
---------------
Switching prosthetics moves a u16 at EquipMagicData +0xf2 between 2449 and
2701. Those are NOT EquipParamWeapon row IDs: the public catalogue puts the
base Loaded Shuriken at 70000 and the base Sabimaru at 75000, and a u16 cannot
hold either. So before anything is named, the question is whether the FULL row
id is stored somewhere in (or near) the same object -- in which case the u16 is
some narrower handle and the real id is what should be read.

This scans an object for 32-bit values that appear in the reference catalogue,
and separately reports every aligned u32/u64 in a neighbourhood so the field
boundary around a candidate can be judged instead of assumed.

It only reads. It never writes to the game, never injects, never patches.

Usage:
  python tools/find_equip_id.py --pid <pid> --class EquipMagicData
  python tools/find_equip_id.py --pid <pid> --class PlayerGameData --width 0x4000
  python tools/find_equip_id.py --pid <pid> --class EquipMagicData --around 0xf0
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from module_watch import (ClassObjects, PlayerModules, Reader, VFTABLE_MAP,  # noqa: E402
                          IMAGE_BASE, plausible)

CATALOG = Path(__file__).resolve().parents[1] / "docs/Sekiro_Prosthetic_ID_Catalog.json"


def load_catalog():
    """(id -> name, id -> family). Reference data, NOT a claim about this build."""
    raw = json.loads(CATALOG.read_text(encoding="utf-8"))
    names, families = {}, {}
    for entry in raw.get("entries", []):
        ident = entry.get("equipParamWeaponId")
        name = entry.get("nameEn")
        if isinstance(ident, int) and isinstance(name, str):
            names[ident] = name
            families[ident] = entry.get("family")
    return names, families


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--base", type=lambda x: int(x, 0), default=IMAGE_BASE)
    ap.add_argument("--class", dest="klass", default=None)
    ap.add_argument("--module", default=None)
    ap.add_argument("--width", type=lambda x: int(x, 0), default=0x1000)
    ap.add_argument("--around", type=lambda x: int(x, 0), default=None,
                    help="also dump aligned u32/u64 around this offset")
    ap.add_argument("--span", type=lambda x: int(x, 0), default=0x40)
    ap.add_argument("--class-depth", type=int, default=4)
    args = ap.parse_args()

    if not args.klass and not args.module:
        sys.exit("--class or --module is required")

    names, families = load_catalog()
    if not names:
        sys.exit("could not read any ids out of the catalogue")
    print(f"catalogue: {len(names)} reference ids "
          f"({min(names)}..{max(names)}) -- public data, not verified on this build")

    by_rva = {int(k, 16): v for k, v in json.loads(
        VFTABLE_MAP.read_text(encoding="utf-8")).items()}
    reader = Reader(args.pid)
    try:
        if args.module:
            resolver = PlayerModules(reader, args.base, by_rva, [args.module])
            label = args.module
        else:
            resolver = ClassObjects(reader, args.base, by_rva, [args.klass],
                                    depth=args.class_depth)
            label = args.klass
        got, why = resolver.resolve()
        if got is None:
            print(f"cannot resolve {label}: {why}")
            return 1
        addr = got[label][0]
        vptr = reader.qword(addr)
        print(f"{label} @ 0x{addr:x}   vptr 0x{vptr:x} "
              f"(rva 0x{vptr - args.base:x} -> {by_rva.get(vptr - args.base, '?')})\n")

        blk = reader.read(addr, args.width)
        if blk is None:
            print("could not read the object")
            return 1

        print(f"-- catalogue ids found as u32 in the first {args.width:#x} bytes --")
        hits = 0
        for off in range(0, len(blk) - 4):
            value, = struct.unpack_from("<I", blk, off)
            if value in names:
                aligned = "aligned" if off % 4 == 0 else "UNALIGNED"
                print(f"  +0x{off:<5x} {value}  {names[value]}  "
                      f"[{families.get(value)}]  ({aligned})")
                hits += 1
        if hits == 0:
            print("  none. The full row id is not stored here, so the observed "
                  "u16 is NOT simply a truncated copy of it.")
        print()

        if args.around is not None:
            start = max(0, (args.around - args.span) & ~7)
            end = min(len(blk), args.around + args.span)
            print(f"-- aligned reads around +0x{args.around:x} --")
            for off in range(start, end - 8, 8):
                q, = struct.unpack_from("<Q", blk, off)
                a, = struct.unpack_from("<I", blk, off)
                b, = struct.unpack_from("<I", blk, off + 4)
                mark = "  <<<" if off <= args.around < off + 8 else ""
                note = ""
                if plausible(q):
                    probe = reader.read(q, 8)
                    note = "  pointer-shaped, " + ("READABLE" if probe else "not readable")
                print(f"  +0x{off:<5x} u64={q:#018x}  u32=[{a:<10} {b:<10}]{note}{mark}")
    finally:
        reader.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
