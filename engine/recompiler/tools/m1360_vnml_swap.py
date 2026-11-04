import io,sys,os,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
D=r"C:\dev\tmp\recomp_split"
files=sorted([f for f in os.listdir(D) if f.endswith(".cpp") and f.startswith("batch_")])
pat_a=re.compile(r"vfp_sf_set\(s, (\d+), -\(vfp_sf\(s, \1\)\) \+ vfp_sf\(s, (\d+)\) \* vfp_sf\(s, (\d+)\)\);")
pat_b=re.compile(r"vfp_sf_set\(s, (\d+), -\(vfp_sf\(s, \1\) \+ vfp_sf\(s, (\d+)\) \* vfp_sf\(s, (\d+)\)\)\);")
def a_to_b(m):
    d,a,b=m.group(1),m.group(2),m.group(3)
    return "vfp_sf_set(s, %s, -(vfp_sf(s, %s) + vfp_sf(s, %s) * vfp_sf(s, %s)));" % (d,d,a,b)
def b_to_a(m):
    d,a,b=m.group(1),m.group(2),m.group(3)
    return "vfp_sf_set(s, %s, -(vfp_sf(s, %s)) + vfp_sf(s, %s) * vfp_sf(s, %s));" % (d,d,a,b)
tot=0; files_changed=0
for f in files:
    p=os.path.join(D,f)
    t=io.open(p,encoding="utf-8",errors="replace").read()
    na=len(pat_a.findall(t)); nb=len(pat_b.findall(t))
    if not (na or nb): continue
    # 1) vnmla-emitted -> temp
    t=pat_a.sub(lambda m: "vfp_sf_set(s, %s, __VNMLA_TMP__(s, %s, %s, %s));" % (m.group(1),m.group(1),m.group(2),m.group(3)), t)
    # 2) vnmls-emitted -> correct vnmls (a-form)
    t=pat_b.sub(b_to_a, t)
    # 3) temp -> correct vnmla (b-form)
    t=re.sub(r"vfp_sf_set\(s, (\d+), __VNMLA_TMP__\(s, (\d+), (\d+), (\d+)\)\);",
             lambda m: "vfp_sf_set(s, %s, -(vfp_sf(s, %s) + vfp_sf(s, %s) * vfp_sf(s, %s)));" % (m.group(1),m.group(1),m.group(3),m.group(4)), t)
    io.open(p,"w",encoding="utf-8",newline="\n").write(t)
    files_changed+=1; tot+=na+nb
print("swapped files",files_changed,"sites",tot)
# sanity: no temp markers left
left=0
for f in files:
    t=io.open(os.path.join(D,f),encoding="utf-8",errors="replace").read()
    left+=t.count("__VNMLA_TMP__")
print("tmp leftovers",left)
# 3) patch generator formulas
g=r"C:\dev\tmp\_gen_batch2.py"
s=io.open(g,encoding="utf-8",errors="replace").read()
old='''        elif m == "vnmla.f32":
            L.append("vfp_sf_set(s, %d, -(vfp_sf(s, %d)) + vfp_sf(s, %d) * vfp_sf(s, %d));"
                     % (ops[0].reg - 79, ops[0].reg - 79, ops[1].reg - 79, ops[2].reg - 79))
        else:
            L.append("vfp_sf_set(s, %d, -(vfp_sf(s, %d) + vfp_sf(s, %d) * vfp_sf(s, %d)));"
                     % (ops[0].reg - 79, ops[0].reg - 79, ops[1].reg - 79, ops[2].reg - 79))'''
assert old in s
new='''        elif m == "vnmla.f32":
            # M1327: ARM VNMLA = -(dst) - op1*op2   (the previous emission was VNMLS semantics)
            L.append("vfp_sf_set(s, %d, -(vfp_sf(s, %d) + vfp_sf(s, %d) * vfp_sf(s, %d)));"
                     % (ops[0].reg - 79, ops[0].reg - 79, ops[1].reg - 79, ops[2].reg - 79))
        else:
            # M1327: ARM VNMLS = -(dst) + op1*op2  (the previous emission was VNMLA semantics)
            L.append("vfp_sf_set(s, %d, -(vfp_sf(s, %d)) + vfp_sf(s, %d) * vfp_sf(s, %d));"
                     % (ops[0].reg - 79, ops[0].reg - 79, ops[1].reg - 79, ops[2].reg - 79))'''
s=s.replace(old,new,1)
io.open(g,"w",encoding="utf-8",newline="\n").write(s)
print("generator vnmla/vnmls formulas swapped")
