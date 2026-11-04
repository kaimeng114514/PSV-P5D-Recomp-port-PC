import io,sys,json,struct
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN
d=open(r"C:\dev\p5d_build\p5d_true.elf","rb").read()
e_phoff=struct.unpack_from("<I",d,0x1C)[0]; e_phnum=struct.unpack_from("<H",d,0x2C)[0]
segs=[]
for i in range(e_phnum):
    off=e_phoff+i*0x20; t,o,v,pa,fsz,msz,fl,al=struct.unpack_from("<8I",d,off)
    if t==1 and fsz: segs.append((v,v+fsz,o))
def vo(va):
    for lo,hi,o in segs:
        if lo<=va<hi: return o+(va-lo)
funcs=json.load(open(r"C:\dev\tmp\function_bounds.json"))
target=0x811935DE
fn=None
for f in funcs:
    if f["va"]<=target<f["va"]+f["size"]: fn=f; break
print("function:",fn["name"],hex(fn["va"]),"size",fn["size"])
off=vo(fn["va"])
md=Cs(CS_ARCH_ARM, CS_MODE_THUMB|CS_MODE_LITTLE_ENDIAN); md.detail=True
lo=max(fn["va"], target-0x300); hi=min(fn["va"]+fn["size"], target+0x100)
code=d[vo(lo):vo(lo)+(hi-lo)]
for ins in md.disasm(code, lo):
    if lo<=ins.address<=hi:
        print("%08X %-28s %s" % (ins.address, ins.mnemonic, ins.op_str))
