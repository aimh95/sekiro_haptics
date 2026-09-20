#!/usr/bin/env python3
"""Build a complete vftable-RVA -> class-name map from sekiro.exe's plaintext MSVC
RTTI, and (optionally) name every module pointer held by the live player container.

The on-disk .text is Steam-DRM encrypted (DEFLECT_STATUS.md 3.2) but .rdata/.data
are not, so RTTI survives and this works entirely offline. Read-only.

  python vftable_map.py --exe "<path to sekiro.exe>" --out vftable_map.json
"""
from __future__ import annotations

import argparse
import json
import re
import struct
from pathlib import Path

import lief
import numpy as np


def build(exe: Path):
    data = Path(exe).read_bytes()
    pe = lief.parse(str(exe))
    ib = pe.optional_header.imagebase
    secs = [(s.name, s.pointerto_raw_data, s.sizeof_raw_data, s.virtual_address)
            for s in pe.sections]

    def off2rva(off):
        for _n, p, rs, rva in secs:
            if p <= off < p + rs:
                return rva + (off - p)
        return None

    def rva2off(rva):
        for _n, p, rs, sr in secs:
            if sr <= rva < sr + rs:
                return p + (rva - sr)
        return None

    rd = [s for s in secs if s[0] == ".rdata"][0]
    rd_off, rd_sz, rd_rva = rd[1], rd[2], rd[3]
    rdata = data[rd_off:rd_off + rd_sz]

    # 1. every RTTI type descriptor: the struct starts 0x10 before its name
    td_by_rva = {}
    for m in re.finditer(rb"\.\?A[VU][A-Za-z0-9_@\?\$\.\-]{2,400}@@", data):
        td_rva = off2rva(m.start() - 0x10)
        if td_rva is not None:
            td_by_rva[td_rva] = m.group().decode("ascii")

    # 2. CompleteObjectLocators: dword[3] == pTypeDescriptor
    arr = np.frombuffer(rdata[:(len(rdata) // 4) * 4], dtype="<u4")
    td_keys = np.array(sorted(td_by_rva), dtype=np.uint32)
    hit_idx = np.nonzero(np.isin(arr, td_keys))[0]

    col_by_rva = {}
    for i in hit_idx:
        col_off = rd_off + int(i) * 4 - 0xC
        if col_off < rd_off:
            continue
        sig, _off0, _cd = struct.unpack_from("<III", data, col_off)
        if sig not in (0, 1):
            continue
        td_rva = int(arr[i])
        col_by_rva[off2rva(col_off)] = td_by_rva[td_rva]

    # 3. the qword immediately before a vftable points at its COL
    out = {}
    for col_rva, name in col_by_rva.items():
        needle = struct.pack("<Q", ib + col_rva)
        start = 0
        while True:
            k = rdata.find(needle, start)
            if k < 0:
                break
            start = k + 1
            vft_rva = rd_rva + k + 8
            o = rva2off(vft_rva)
            if o is None:
                continue
            first, = struct.unpack_from("<Q", data, o)
            # a real vftable's slot 0 points into .text
            if 0x1000 <= first - ib < 0x291bc47:
                out.setdefault(vft_rva, name)
    return ib, out


def demangle(n: str) -> str:
    """Just enough to turn '.?AVFoo@NS_SPRJ@@' into 'NS_SPRJ::Foo'."""
    m = re.fullmatch(r"\.\?A[VU]([A-Za-z0-9_]+)@([A-Za-z0-9_]+)@@", n)
    if m:
        return f"{m.group(2)}::{m.group(1)}"
    m = re.fullmatch(r"\.\?A[VU]([A-Za-z0-9_]+)@@", n)
    if m:
        return m.group(1)
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--lookup", type=lambda x: int(x, 0), nargs="*",
                    help="vftable RVAs to name")
    args = ap.parse_args()

    ib, m = build(args.exe)
    print(f"imagebase 0x{ib:x}, vftables mapped: {len(m)}")
    if args.out:
        args.out.write_text(json.dumps(
            {f"0x{k:x}": demangle(v) for k, v in sorted(m.items())}, indent=1), encoding="utf-8")
        print(f"wrote {args.out}")
    for rva in (args.lookup or []):
        print(f"  0x{rva:x} -> {demangle(m.get(rva, '<not found>'))}")


if __name__ == "__main__":
    main()
