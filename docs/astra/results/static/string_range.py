#!/usr/bin/env python3
"""Dump every UTF-16LE string in a VA range of the shipped EXE's plaintext data.

Companion to prosthetic_strings.py. That one searches by term; this one dumps a
NEIGHBOURHOOD, which is how the deflect work identified the owning translation
unit: the debug labels a page shows sit next to each other in .rdata, so the
strings around a known field name tell you what else that same debug page (and
therefore that same object) exposes.

Read-only. Never modifies, unpacks or uploads the EXE.

Usage:
  python docs/astra/results/static/string_range.py 0x142a71000 0x142a78000
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

EXE = Path(r"E:/Program Files/Steam/steamapps/common/Sekiro/sekiro.exe")


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    lo = int(sys.argv[1], 0)
    hi = int(sys.argv[2], 0)

    try:
        import lief
    except ImportError:
        sys.exit("lief is required: python -m pip install lief")
    pe = lief.parse(str(EXE))
    base = pe.optional_header.imagebase
    blob = EXE.read_bytes()

    for section in pe.sections:
        ptr = section.pointerto_raw_data
        size = section.sizeof_raw_data
        rva = section.virtual_address
        start_va = base + rva
        end_va = start_va + size
        if hi <= start_va or lo >= end_va:
            continue
        chunk_lo = max(lo, start_va)
        chunk_hi = min(hi, end_va)
        off_lo = ptr + (chunk_lo - start_va)
        off_hi = ptr + (chunk_hi - start_va)
        data = blob[off_lo:off_hi]
        pattern = re.compile(rb"(?:[\x20-\x7e]\x00|[^\x00][\x01-\xff]){2,}")
        for match in pattern.finditer(data):
            raw = match.group()
            if len(raw) % 2:
                raw = raw[:-1]
            try:
                text = raw.decode("utf-16-le")
            except UnicodeDecodeError:
                continue
            if len(text) < 2:
                continue
            va = chunk_lo + match.start()
            print(f"0x{va:x}  {section.name:<8} {text}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
