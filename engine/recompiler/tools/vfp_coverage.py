import io,sys,json,collections,struct
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN
ELF=r"C:\dev\p5d_build\p5d_true.elf"
d=open(ELF,"rb").read()
e_phoff=struct.unpack_from("<I",d,0x1C)[0]; e_phnum=struct.unpack_from("<H",d,0x2C)[0]
segs=[]
for i in range(e_phnum):
    off=e_phoff+i*0x20
    t,o,v,pa,fsz,msz,fl,al=struct.unpack_from("<8I",d,off)
    if t==1 and fsz: segs.append((v,v+fsz,o))
def vo(va):
    for lo,hi,o in segs:
        if lo<=va<hi: return o+(va-lo)
    return None
funcs=json.load(open(r"C:\dev\tmp\function_bounds.json"))
md=Cs(CS_ARCH_ARM, CS_MODE_THUMB|CS_MODE_LITTLE_ENDIAN)
handled=("vmov","vldr","vstr","vadd.f32","vsub.f32","vmul.f32","vdiv.f32","vmla.f32","vmls.f32","vnmul.f32","vcvt","vcmp.f32","vmrs","vpush","vpop")
cnt=collections.Counter(); n=0
for f in funcs:
    off=vo(f["va"])
    if off is None: continue
    code=d[off:off+f["size"]]
    for ins in md.disasm(code, f["va"]):
        m=ins.mnemonic
        if m.startswith("v"): cnt[m]+=1
        n+=1
print("instrs",n,"vfp-ops",sum(cnt.values()))
unh=[(m,c) for m,c in cnt.most_common() if not any(m.startswith(h) for h in handled)]
print("UNHANDLED VFP mnemonics:",len(unh))
for m,c in unh[:50]: print("  ",m,c)
print("--- handled ---")
for m,c in cnt.most_common(30): print("  ",m,c)
