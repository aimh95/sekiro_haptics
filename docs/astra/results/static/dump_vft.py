import struct, json
exec(open('rtti.py').read())
out={}
lines=[]
for cls,short in [(".?AVSprjPlayerDamageModule@NS_SPRJ@@","SprjPlayerDamageModule"),
                  (".?AVSprjEnemyDamageModule@NS_SPRJ@@","SprjEnemyDamageModule"),
                  (".?AVSprjChrDamageModule@NS_SPRJ@@","SprjChrDamageModule"),
                  (".?AVSprjChrActionFlagModule@NS_SPRJ@@","SprjChrActionFlagModule"),
                  (".?AVCSChrToughnessModule@NS_SPRJ@@","CSChrToughnessModule"),
                  (".?AVSprjChrBehaviorModule@NS_SPRJ@@","SprjChrBehaviorModule"),
                  (".?AVSprjPlayerHitStopModule@NS_SPRJ@@","SprjPlayerHitStopModule")]:
    r=find_class(cls)
    if not r:
        out[short]=None; lines.append(f"{short}: NOT FOUND"); continue
    vft,col,td,n,o0 = r[0]
    fns=[]
    for k in range(n):
        off=rva2off(vft+k*8)
        v=struct.unpack('<Q', d[off:off+8])[0]
        fns.append(v-IB)
    out[short]={"vftableRva":hex(vft),"colRva":hex(col),"typeDescriptorRva":hex(td),
                "methodCount":n,"methodRvas":[hex(x) for x in fns]}
    lines.append(f"{short:26s} vft RVA {hex(vft):12s} methods={n}  first6={', '.join(hex(x) for x in fns[:6])}")
json.dump(out, open("vftables.json","w"), indent=2)
open("vft_report.txt","w",encoding="utf-8").write("\n".join(lines))
