import io,sys,os,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
# 1) patch generated batch cpp files: vfp_sf(s,-79) => 0.0f (vcmp.f32 Sd,#0.0 immediate mis-emitted)
dirs=[r"C:\dev\tmp\recomp_split", r"C:\dev\tmp\recomp_gapfix"]
changed=[]
tot=0
for d in dirs:
    for f in sorted(os.listdir(d)):
        if not f.endswith(".cpp"): continue
        p=os.path.join(d,f)
        t=io.open(p,encoding="utf-8",errors="replace").read()
        n=t.count("vfp_sf(s,-79)")
        if n:
            t=t.replace("vfp_sf(s,-79)","0.0f")
            io.open(p,"w",encoding="utf-8",newline="\n").write(t)
            changed.append((p,n)); tot+=n
print("patched files",len(changed),"sites",tot)
for p,n in changed: print("  ",os.path.basename(p),n)
# 2) patch generator vcmp branch for future regen
g=r"C:\dev\tmp\_gen_batch2.py"
t=io.open(g,encoding="utf-8",errors="replace").read()
old='''    elif m == "vcmp.f32" and len(ops) >= 2:
        L.append("{ float _a=vfp_sf(s,%d), _b=vfp_sf(s,%d); s->apsr=s->fp_nzcv=(_a==_b)?0x40000000u:((_a<_b)?0x80000000u:0x20000000u); }" % (reg(0)-79, reg(1)-79))'''
assert old in t
new='''    elif m == "vcmp.f32" and len(ops) >= 2:
        # M1334: operand may be a floating-point IMMEDIATE (e.g. vcmp.f32 sN,#0.0).  The old code
        # emitted vfp_sf(s,-79) for immediates (garbage/out-of-bounds read) so every float-vs-zero
        # comparison in the whole library used random values.  Handle both forms explicitly.
        if ops[1].type == ARM_OP_REG:
            _bexpr = "vfp_sf(s,%d)" % (reg(1)-79)
        else:
            _bf = float(getattr(ops[1], "fp", 0.0))
            _bexpr = "(%.9gf)" % _bf
        L.append("{ float _a=vfp_sf(s,%d), _b=%s; s->apsr=s->fp_nzcv=(_a==_b)?0x40000000u:((_a<_b)?0x80000000u:0x20000000u); }" % (reg(0)-79, _bexpr))'''
t=t.replace(old,new,1)
io.open(g,"w",encoding="utf-8",newline="\n").write(t)
print("generator patched")
