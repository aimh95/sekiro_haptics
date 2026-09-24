#!/usr/bin/env python3
"""READ-ONLY: print a window of a live object located by RTTI class.

Companion to tools/module_watch.py: the watcher says WHICH offset moved, this
says what the neighbourhood holds right now, so a byte can be read as the u8 /
u16 / u32 it actually is instead of as an isolated change.

It only reads. It never writes to the game, never injects, never patches.

Usage:
  python tools/read_field.py --pid <pid> --class EquipMagicData --at 0xf0 --span 16
  python tools/read_field.py --pid <pid> --class EquipGameData --at 0xa0 --span 16
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from module_watch import (ClassObjects, PlayerModules, Reader, VFTABLE_MAP,  # noqa: E402
                          IMAGE_BASE)


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--base", type=lambda x: int(x, 0), default=IMAGE_BASE)
    ap.add_argument("--class", dest="klass", default=None)
    ap.add_argument("--module", default=None)
    ap.add_argument("--at", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--span", type=int, default=16)
    ap.add_argument("--class-depth", type=int, default=4)
    args = ap.parse_args()

    if not args.klass and not args.module:
        sys.exit("--class or --module is required")

    by_rva = {int(k, 16): v for k, v in json.loads(
        VFTABLE_MAP.read_text(encoding="utf-8")).items()}
    reader = Reader(args.pid)
    try:
        if args.module:
            resolver = PlayerModules(reader, args.base, by_rva, [args.module])
            name = args.module
        else:
            resolver = ClassObjects(reader, args.base, by_rva, [args.klass],
                                    depth=args.class_depth)
            name = args.klass
        got, why = resolver.resolve()
        if got is None:
            print(f"cannot resolve {name}: {why}")
            return 1
        addr = got[name][0]
        start = args.at & ~0xF
        span = ((args.at + args.span - start) + 15) & ~0xF
        blk = reader.read(addr + start, span)
        if blk is None:
            print("read failed")
            return 1
        print(f"{name} @ 0x{addr:x}\n")
        for row in range(0, span, 16):
            off = start + row
            chunk = blk[row:row + 16]
            hexpart = " ".join(f"{b:02x}" for b in chunk)
            print(f"  +0x{off:<5x} {hexpart}")
        print()
        for width, fmt, label in ((1, "<B", "u8 "), (2, "<H", "u16"), (4, "<I", "u32")):
            rel = args.at - start
            if rel + width <= len(blk):
                value, = struct.unpack_from(fmt, blk, rel)
                print(f"  +0x{args.at:x} as {label}: {value}  (0x{value:x})")
    finally:
        reader.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
