import io,sys,re,glob,os,shutil
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
SRC=r"C:\dev\tmp\recomp_split"; DST=r"C:\dev\tmp\recomp_split_vfix"
REF=r"C:\dev\tmp\recomp_split_fix"
for f in glob.glob(SRC+r"\batch_*.cpp"):
    shutil.copy2(f,DST)
start_re=re.compile(r"static uint32_t blk_([0-9A-F]{8})\(RecState\* s\) \{")
def blocks_of(t):
    ms=list(start_re.finditer(t)); out={}
    for k,m in enumerate(ms):
        end = ms[k+1].start() if k+1<len(ms) else len(t)
        out[m.group(1)]=t[m.start():end]
    return out
ref={}
for f in glob.glob(REF+r"\batch_*.cpp"):
    ref.update(blocks_of(io.open(f,encoding="utf-8",errors="replace").read()))
old_pat=re.compile(r"vfp_s_bits_set\(s, (\d+), f32_bits\(\(float\)([-+0-9.eE]+)\)\);")
new_pat=re.compile(r"vfp_s_bits_set\(s, (\d+), 0x([0-9A-Fa-f]+)u\);")
tf=0; tb=0
for f in glob.glob(DST+r"\batch_*.cpp"):
    t=io.open(f,encoding="utf-8",errors="replace").read()
    ms=list(start_re.finditer(t))
    out=[]; prev=0; changed=False
    for k,m in enumerate(ms):
        end = ms[k+1].start() if k+1<len(ms) else len(t)
        out.append(t[prev:m.start()]); prev=end
        va=m.group(1); oldblk=t[m.start():end]; nb=oldblk
        r=ref.get(va)
        if r is not None:
            oms=list(old_pat.finditer(oldblk)); nms=list(new_pat.finditer(r))
            if oms and len(oms)==len(nms):
                for om,nm in zip(oms,nms):
                    nb=nb.replace(om.group(0), "vfp_s_bits_set(s, %s, 0x%su);"%(nm.group(1),nm.group(2)),1)
                if nb!=oldblk: tb+=1; changed=True
        out.append(nb)
    out.append(t[prev:])
    if changed:
        io.open(f,"w",encoding="utf-8",newline="\n").write("".join(out))
        tf+=1
print("patched files",tf,"blocks",tb)
# sanity: check header preserved
t=io.open(DST+r"\batch_013.cpp",encoding="utf-8",errors="replace").read()
print("header ok:", t.startswith("// batch 13:"), "| includes:", "#include <cstdint>" in t)
i=t.find("blk_811935C0(RecState* s) {")
print("fixed vmov present:", "vfp_s_bits_set(s, 3, 0x3F800000u)" in t)
