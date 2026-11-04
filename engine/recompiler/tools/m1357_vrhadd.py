import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
p=r"C:\dev\tmp\_gen_batch2.py"
t=io.open(p,encoding="utf-8",errors="replace").read()
if "vrhadd.u16" not in t:
    old='if m in ("vnmls.f32","vnmla.f32","vrsqrts.f32","vrsqrte.f32","vsqrt.f32","vrecpe.f32","vrecps.f32","vceq.i32","vtst.32","vmax.f32","vmin.f32","vacgt.f32","vadd.i32","vsub.i32","vtrn.32","vbit","vext.64") and len(ops) >= 2:'
    assert old in t
    new='if m in ("vnmls.f32","vnmla.f32","vrsqrts.f32","vrsqrte.f32","vsqrt.f32","vrecpe.f32","vrecps.f32","vceq.i32","vtst.32","vmax.f32","vmin.f32","vacgt.f32","vadd.i32","vsub.i32","vtrn.32","vbit","vext.64","vrhadd.u16") and len(ops) >= 2:'
    t=t.replace(old,new,1)
    old2='''                    elif m == "vbit":'''
    assert old2 in t
    add='''                    elif m == "vrhadd.u16":
                        # M1326: rounding halving add per u16 lane: dst=(src1+src2+1)>>1
                        _st.append("{ uint64_t _a=s->d[%d],_b=s->d[%d],_r=0; for(int _i=0;_i<4;_i++){ uint32_t _x=(uint32_t)((_a>>(16*_i))&0xFFFFu),_y=(uint32_t)((_b>>(16*_i))&0xFFFFu); uint32_t _s=(_x+_y+1u)>>1u; _r|=((uint64_t)(uint16_t)_s)<<(16*_i);} s->d[%d]=_r; }" % (_b,_c,_a))
'''
    t=t.replace(old2,add+old2,1)
    io.open(p,"w",encoding="utf-8",newline="\n").write(t)
    print("vrhadd.u16 handler added")
else: print("already has vrhadd")
# sanity: emit test for vrhadd via exec
src=io.open(p,encoding="utf-8").read(); ns={}
exec(compile(src[:src.index("out = []")],"_g","exec"),ns)
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
md=Cs(CS_ARCH_ARM, CS_MODE_ARM); md.detail=True
# find a vrhadd encoding sample from elf
import struct,json
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
found=0
for f in funcs:
    off=vo(f["va"])
    if off is None: continue
    for ins in md.disasm(d[off:off+f["size"]], f["va"]):
        if ins.mnemonic=="vrhadd.u16":
            sup=[]; st=ns["emit"](ins,sup)
            print("sample",hex(ins.address),ins.mnemonic,ins.op_str,"->",(st if st else "EMPTY")[:200],"unsup" if sup else "")
            found=1; break
    if found: break
