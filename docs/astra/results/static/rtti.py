import sys, io, struct, re
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
exec(open('xref.py').read())
d = Path(PATH).read_bytes()

def rva2off(rva):
    for n,p,rs,sr,vs in SECS:
        if sr <= rva < sr+rs: return p+(rva-sr)
    return None
def off2rva(off):
    for n,p,rs,sr,vs in SECS:
        if p <= off < p+rs: return sr+(off-p)
    return None

RD = [s for s in SECS if s[0]=='.rdata'][0]
RD_OFF, RD_SZ, RD_RVA = RD[1], RD[2], RD[3]
rdata = d[RD_OFF:RD_OFF+RD_SZ]

def find_class(mangled):
    """Return list of (vftable_rva, col_rva, td_rva, num_methods)"""
    out=[]
    start=0
    b=mangled.encode()
    while True:
        i = d.find(b, start); start = i+1
        if i < 0: break
        # TypeDescriptor begins 0x10 bytes before the name
        td_off = i - 0x10
        td_rva = off2rva(td_off)
        if td_rva is None: continue
        # find COLs whose pTypeDescriptor == td_rva
        pat = struct.pack('<I', td_rva)
        s2=0
        while True:
            j = rdata.find(pat, s2); s2 = j+1
            if j < 0: break
            if j < 0xC: continue
            col_off = RD_OFF + j - 0xC
            col_rva = off2rva(col_off)
            sig, off0, cd = struct.unpack('<III', d[col_off:col_off+12])
            if sig not in (0,1): continue
            # find 8-byte pointer to COL  -> vftable follows
            colptr = struct.pack('<Q', IB + col_rva)
            s3=0
            while True:
                k = rdata.find(colptr, s3); s3 = k+1
                if k < 0: break
                vft_rva = RD_RVA + k + 8
                # count consecutive pointers into .text
                n=0
                while True:
                    o = rva2off(vft_rva + n*8)
                    if o is None: break
                    v = struct.unpack('<Q', d[o:o+8])[0]
                    r = v - IB
                    if not (T_RVA <= r < T_RVA + T_SZ): break
                    n += 1
                out.append((vft_rva, col_rva, td_rva, n, off0))
    return out
