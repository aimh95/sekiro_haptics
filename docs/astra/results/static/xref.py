import numpy as np, sys
from pathlib import Path
import lief
PATH = r"E:/Program Files/Steam/steamapps/common/Sekiro/sekiro.exe"
data = np.frombuffer(Path(PATH).read_bytes(), dtype=np.uint8)
raw  = data.tobytes()
pe = lief.parse(PATH)
IB = pe.optional_header.imagebase
SECS=[(s.name,s.pointerto_raw_data,s.sizeof_raw_data,s.virtual_address,s.virtual_size) for s in pe.sections]

def off2va(off):
    for n,p,rs,rva,vs in SECS:
        if p<=off<p+rs: return IB+rva+(off-p)
    return None
def va2off(va):
    r=va-IB
    for n,p,rs,rva,vs in SECS:
        if rva<=r<rva+rs: return p+(r-rva)
    return None
def sec_of_va(va):
    r=va-IB
    for n,p,rs,rva,vs in SECS:
        if rva<=r<rva+max(rs,vs): return n
    return None

TEXT=[s for s in SECS if s[0]=='.text'][0]
T_OFF, T_SZ, T_RVA = TEXT[1], TEXT[2], TEXT[3]
# u32 at every byte offset in .text
_tb = data[T_OFF:T_OFF+T_SZ].astype(np.uint32)
u32 = (_tb[0:-3] | (_tb[1:-2] << 8) | (_tb[2:-1] << 16) | (_tb[3:] << 24)).astype(np.int64)
pos_va = (IB + T_RVA + np.arange(u32.size, dtype=np.int64))

def rip_xrefs(target_va):
    """positions p (VA of disp32 field) where VA(p)+4+disp32 == target_va"""
    need = (target_va - pos_va - 4) & 0xFFFFFFFF
    idx = np.nonzero(u32 == need)[0]
    return [(int(pos_va[i]), int(T_OFF+i)) for i in idx]

def abs_refs(target_va):
    """8-byte absolute pointers to target anywhere in the image"""
    b = target_va.to_bytes(8,'little')
    out=[]; start=0
    while True:
        i = raw.find(b, start)
        if i<0: break
        out.append((off2va(i), i)); start=i+1
    return out
