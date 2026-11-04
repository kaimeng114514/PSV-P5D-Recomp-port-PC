import io,sys,re,glob
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
DST=r"C:\dev\tmp\recomp_split_fp"
OLD="s->apsr=(_a==_b)?0x40000000u:((_a<_b)?0x80000000u:0x20000000u);"
NEW="s->apsr=s->fp_nzcv=(_a==_b)?0x40000000u:((_a<_b)?0x80000000u:0x20000000u);"
nf=0;nv=0;nc=0
for f in glob.glob(DST+r"\batch_*.cpp"):
    t=io.open(f,encoding="utf-8",errors="replace").read()
    c1=t.count(OLD); c2=t.count("/* vmrs */")
    t2=t.replace(OLD,NEW).replace("/* vmrs */","s->apsr = s->fp_nzcv;")
    if t2!=t:
        io.open(f,"w",encoding="utf-8",newline="\n").write(t2)
        nf+=1; nv+=c1; nc+=c2
print("files",nf,"vcmp patched",nv,"vmrs patched",nc)
# harness struct
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
t2=t.replace("uint32_t apsr; uint8_t* base;","uint32_t apsr; uint32_t fp_nzcv; uint8_t* base;")
io.open(H,"w",encoding="utf-8",newline="\n").write(t2)
print("harness struct updated:", t2!=t)
# generator update (for future regeneration)
G=r"C:\dev\tmp\_gen_batch2.py"
g=io.open(G,encoding="utf-8",errors="replace").read()
g2=g.replace('L.append("{ float _a=vfp_sf(s,%d), _b=vfp_sf(s,%d); s->apsr=(_a==_b)?0x40000000u:((_a<_b)?0x80000000u:0x20000000u); }" % (reg(0)-79, reg(1)-79))',
 'L.append("{ float _a=vfp_sf(s,%d), _b=vfp_sf(s,%d); s->apsr=s->fp_nzcv=(_a==_b)?0x40000000u:((_a<_b)?0x80000000u:0x20000000u); }" % (reg(0)-79, reg(1)-79))')
g2=g2.replace('elif m == "vmrs":\n        L.append("/* vmrs */")',
 'elif m == "vmrs":\n        L.append("s->apsr = s->fp_nzcv; /* vmrs APSR_nzcv,FPSCR */")')
io.open(G,"w",encoding="utf-8",newline="\n").write(g2)
print("generator updated:", g2!=g)
