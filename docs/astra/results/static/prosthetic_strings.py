#!/usr/bin/env python3
"""Search the shipped EXE's PLAINTEXT sections for prosthetic / wire-action strings.

Why this works at all: the on-disk .text is encrypted by the Steam DRM wrapper
(DEFLECT_EVIDENCE.json TEXT_SECTION_ENCRYPTED, entropy 8.0), but .rdata and
.data are not (entropy 4.3 / 3.7). Debug-menu labels and EzState command names
live there as UTF-16LE, and that is exactly how the deflect flag was first
named: "IsSucceededJustGuard|(前回のガード時に)ジャスガ成功したか[%s]".

This script only READS the file. It never modifies, unpacks or uploads it.

Usage:
  python docs/astra/results/static/prosthetic_strings.py            # default terms
  python docs/astra/results/static/prosthetic_strings.py 鉤縄 Wire
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

EXE = Path(r"E:/Program Files/Steam/steamapps/common/Sekiro/sekiro.exe")
IMAGE_BASE = 0x140000000

# Terms chosen from what the game and its RTTI already call these things.
# Japanese first: every named debug string found so far has been Japanese.
DEFAULT_TERMS = [
    # prosthetic / tool
    "忍義手", "義手", "忍具", "道具", "サブウェポン",
    "ShinobiTool", "Prosthetic", "SubWeapon", "ToolUse", "UseTool",
    # shuriken / sabimaru
    "手裏剣", "錆び丸", "錆丸", "Shuriken", "Sabimaru",
    # wire / grapple
    "鉤縄", "かぎ縄", "ワイヤー", "WireAction", "Wire", "GrapplingHook", "Grapple",
    # selection / equip
    "選択", "装備", "Equip", "Select",
]


def load_sections():
    try:
        import lief
    except ImportError:
        sys.exit("lief is required: python -m pip install lief")
    pe = lief.parse(str(EXE))
    out = []
    for s in pe.sections:
        out.append((s.name, s.pointerto_raw_data, s.sizeof_raw_data,
                    s.virtual_address, s.virtual_size))
    return pe.optional_header.imagebase, out


def off_to_va(off, base, sections):
    for name, ptr, raw_size, rva, _vsize in sections:
        if ptr <= off < ptr + raw_size:
            return base + rva + (off - ptr), name
    return None, None


def utf16_strings(blob, min_chars=3):
    """Yield (offset, text) for NUL-terminated UTF-16LE runs."""
    pattern = re.compile((rb"(?:[\x20-\x7e]\x00|[^\x00][\x01-\xff])"
                          rb"{%d,}" % min_chars))
    for match in pattern.finditer(blob):
        raw = match.group()
        if len(raw) % 2:
            raw = raw[:-1]
        try:
            text = raw.decode("utf-16-le")
        except UnicodeDecodeError:
            continue
        yield match.start(), text


def main() -> int:
    # The console here is cp949; these strings are Japanese.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    terms = sys.argv[1:] or DEFAULT_TERMS
    if not EXE.exists():
        sys.exit(f"not found: {EXE}")
    base, sections = load_sections()
    blob = EXE.read_bytes()

    # Only the sections that are actually plaintext. .text is encrypted, so
    # searching it produces noise, not findings.
    wanted = {".rdata", ".data"}
    hits = 0
    for name, ptr, raw_size, _rva, _vsize in sections:
        if name not in wanted:
            continue
        section = blob[ptr:ptr + raw_size]
        for local_off, text in utf16_strings(section):
            if not any(term in text for term in terms):
                continue
            va, sec = off_to_va(ptr + local_off, base, sections)
            hits += 1
            printable = text.replace("\n", "\\n")
            print(f"0x{va:x}  {sec:<8} {printable}")
    print(f"\n{hits} match(es). These are STRINGS, not confirmed fields: a name "
          f"proves the concept exists in this build, not where its value lives.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
