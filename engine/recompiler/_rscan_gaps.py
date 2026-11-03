import io,sys,os,re,glob,json,struct
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
RECDIR=r"C:\dev\tmp\recomp_split"
ELF=r"C:\dev\p5d_build\p5d_true.elf"
blockset=set(); conts=set(); rets=set()
files=sorted(glob.glob(os.path.join(RECDIR,"batch_*.cpp")))
bre=re.compile(r"static uint32_t blk_([0-9A-F]{8})\(RecState\* s\) \{")
cre=re.compile(r"s->r\[14\] = 0x([0-9A-F]{8})u; return 0x([0-9A-F]{8})u; /\* blx? \*/")
rre=re.compile(r"return 0x([0-9A-F]{8})u;")
for f in files:
    t=io.open(f,encoding="utf-8",errors="replace").read()
    for m in bre.finditer(t): blockset.add(int(m.group(1),16))
    for m in cre.finditer(t): conts.add(int(m.group(1),16)); conts.add(int(m.group(2),16))
    for m in rre.finditer(t):
        v=int(m.group(1),16)
        if 0x81000000<=v<0x81300000: rets.add(v)
print("blocks",len(blockset),"conts",len(conts),"rets",len(rets))
# ELF exec segment
d=io.open(ELF,"rb").read()
e_phoff=struct.unpack_from("<I",d,0x1C)[0]; e_phnum=struct.unpack_from("<H",d,0x2C)[0]
segs=[]
for i in range(e_phnum):
    off=e_phoff+i*0x20
    t,o,v,pa,fsz,msz,fl,al=struct.unpack_from("<8I",d,off)
    if t==1 and fsz and (fl&1): segs.append((v,v+fsz,o))
def vo(va):
    for lo,hi,o in segs:
        if lo<=va<hi: return o+(va-lo)
    return None
print("exec segs",[(hex(a),hex(b)) for a,b,_ in segs])
def in_text(v): return vo(v) is not None
cand=(conts|rets)-blockset
missing=sorted(v for v in cand if in_text(v))
print("missing candidates",len(missing))
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN
md=Cs(CS_ARCH_ARM, CS_MODE_THUMB|CS_MODE_LITTLE_ENDIAN)
funcs=[]; skipped=[]
for va in missing:
    o=vo(va)
    if o is None: continue
    insns=list(md.disasm(d[o:o+32], va))
    if not insns: skipped.append(va); continue
    size=0; ok=True
    for ins in insns[:6]:
        if ins.address!=va+size: ok=False; break
        if ins.address!=va and ins.address in blockset: break   # reached existing block
        size+=ins.size
        if size>0 and (va+size) in blockset: break
        if ins.mnemonic in ("b","b.w","bl","blx","bx","pop","cbz","cbnz","tbb","tbh") and ins.address!=va:
            break
    if not ok or size==0: skipped.append(va); continue
    funcs.append({"va":va,"size":size})
print("gap funcs",len(funcs),"skipped",len(skipped))
print("sample:",[(hex(f["va"]),f["size"]) for f in funcs[:10]])
json.dump(funcs,open(r"C:\dev\tmp\gapfix_bounds.json","w"))
print("wrote bounds")
