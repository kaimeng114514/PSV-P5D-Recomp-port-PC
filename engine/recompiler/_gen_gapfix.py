# -*- coding: utf-8 -*-
# 18.3634 static-recomp split generation/compile pipeline: emit translated C++ in batches (default 500 functions
# per .cpp) and compile each batch separately (parallel via subprocess), then report timings and object sizes.
# Reuses the normalized generator semantics from _gen_batch2.py (imported by exec of its source is not safe due to
# its top-level main flow, so this file carries a compact copy of the needed logic).
import json, struct, subprocess, os, time, sys, collections, concurrent.futures

ELF = r"C:\dev\p5d_build\p5d_true.elf"
BOUNDS = r"C:\dev\tmp\gapfix_bounds.json"
OUTDIR = r"C:\dev\tmp\recomp_gapfix"
BATCH = int(sys.argv[1]) if len(sys.argv) > 1 else 500
PARALLEL = int(sys.argv[2]) if len(sys.argv) > 2 else 4
os.makedirs(OUTDIR, exist_ok=True)

# import the shared semantics module (execute only function definitions from _gen_batch2 before its main section)
src = open(r"C:\dev\tmp\_gen_batch2.py", encoding="utf-8").read()
main_idx = src.index("out = []")
ns = {}
exec(compile(src[:main_idx], "_gen_batch2_semantics", "exec"), ns)
emit = ns["emit"]; is_term = ns["is_term"]; base_m = ns["base_m"]
R = ns["R"]; COND = ns["COND"]; RIDX = ns["RIDX"]
ridx = RIDX

d = open(ELF, "rb").read()
e_phoff = struct.unpack_from("<I", d, 0x1C)[0]; e_phnum = struct.unpack_from("<H", d, 0x2C)[0]
segs = []
for i in range(e_phnum):
    off = e_phoff + i * 0x20
    t, o, v, pa, fsz, msz, fl, al = struct.unpack_from("<8I", d, off)
    if t == 1 and fsz: segs.append((v, v + fsz, o))
def vo(va):
    for lo, hi, o in segs:
        if lo <= va < hi: return o + (va - lo)
    return None
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN
from capstone.arm import ARM_OP_REG, ARM_OP_IMM, ARM_OP_MEM
funcs = json.load(open(BOUNDS))
# --- 18.3671: fill gaps between function boundaries (truncated tails) ---
funcs.sort(key=lambda f: f["va"])
_md_gap = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_LITTLE_ENDIAN)
_filled = 0
for _i, _f in enumerate(funcs):
    _end = _f["va"] + _f["size"]
    _next = funcs[_i + 1]["va"] if _i + 1 < len(funcs) else 0x812B2AF0
    _gap = _next - _end
    if _gap <= 0 or _gap > 0x100:
        continue
    _o = vo(_end)
    if _o is None:
        continue
    _bs = d[_o:_o + _gap]
    _n = 0
    _ok = True
    for _ins in _md_gap.disasm(_bs, _end):
        _n += 1
    _tot = 0
    for _ins in _md_gap.disasm(_bs, _end):
        _tot += _ins.size
    if _tot != _gap:
        _ok = False
    if _n > 0 and _ok:
        _f["size"] += _gap
        _filled += 1
print("gap-filled functions:", _filled)

# 18.3738: functions whose recorded size is degenerate (<=1 byte) are boundary artifacts; extend them up to the
# next function boundary (max 0x100) when the byte range disassembles exactly, so their instructions are translated.
_ext = 0
for _i, _f in enumerate(funcs):
    if _f["size"] > 1:
        continue
    _next = funcs[_i + 1]["va"] if _i + 1 < len(funcs) else 0x812B2AF0
    _sz = min(0x100, _next - _f["va"])
    if _sz <= 1:
        continue
    _o = vo(_f["va"])
    if _o is None:
        continue
    _tot = 0
    for _ins in _md_gap.disasm(d[_o:_o + _sz], _f["va"]):
        _tot += _ins.size
    if _tot == _sz:
        _f["size"] = _sz
        _ext += 1
print("degenerate-size extended functions:", _ext)

IMPORT_JSON = r"C:\dev\p5dsrc\docs\eboot_imports_with_nid.json"
STUBS = {}
try:
    for im in json.load(open(IMPORT_JSON, encoding="utf-8")):
        STUBS[int(im["stub_vaddr"])] = (im.get("module",""), int(im.get("func_nid",0)))
except Exception as e:
    print("WARN: no import table:", e)

HEADER = '''// batch {}: functions {}..{} ({} blocks)
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
struct RecState {{ uint32_t r[16]; uint32_t apsr; uint8_t* base; uint32_t mem_size; uint64_t d[32]; }};
extern "C" uint32_t g_cur_block;
extern "C" void harness_badmem(uint32_t va);
static inline uint8_t* P(RecState* s, uint32_t a){{ uint32_t off; if (a >= 0x80600000u && a < 0x84600000u) off = a - 0x80600000u; else if (a >= 0xA0000000u && a < 0xA1000000u) off = (a - 0xA0000000u) + 0x7000000u; else if (a >= 0x84600000u && a < 0x8C600000u) off = (a - 0x84600000u) + 0x4000000u; else {{ std::fprintf(stderr, "[BADMEM] guest va=0x%08X unmapped block=0x%08X", a, g_cur_block); harness_badmem(a); std::abort(); }} if (off >= s->mem_size) {{ std::fprintf(stderr, "[BADMEM] guest va=0x%08X off=0x%08X memsize=0x%08X block=0x%08X", a, off, s->mem_size, g_cur_block); std::abort(); }} return s->base + off; }}
static inline uint32_t rd32(RecState* s,uint32_t a){{ uint32_t v; std::memcpy(&v,P(s,a),4); return v; }}
static inline void wr32(RecState* s,uint32_t a,uint32_t v){{ std::memcpy(P(s,a),&v,4); }}
static inline uint16_t rd16(RecState* s,uint32_t a){{ uint16_t v; std::memcpy(&v,P(s,a),2); return v; }}
static inline void wr16(RecState* s,uint32_t a,uint16_t v){{ std::memcpy(P(s,a),&v,2); }}
static inline uint64_t rd64(RecState* s,uint32_t a){{ uint64_t v; std::memcpy(&v,P(s,a),8); return v; }}
static inline void wr64(RecState* s,uint32_t a,uint64_t v){{ std::memcpy(P(s,a),&v,8); }}
static inline void cmp_flags(RecState* s,uint32_t a,uint32_t b){{ s->apsr=(a==b)?0x40000000u:((a<b)?0x80000000u:0u); }}
static inline uint32_t f32_bits(float f){{ uint32_t b; std::memcpy(&b,&f,4); return b; }}
static inline uint64_t vfp_s_bits(RecState* s,int n){{ return (uint32_t)((s->d[n>>1] >> ((n&1)?32:0)) & 0xFFFFFFFFu); }}
static inline void vfp_s_bits_set(RecState* s,int n,uint32_t v){{ s->d[n>>1] = (n&1)? ((s->d[n>>1] & 0xFFFFFFFFull) | ((uint64_t)v<<32)) : ((s->d[n>>1] & 0xFFFFFFFF00000000ull) | v); }}
static inline uint32_t vfp_s_get(RecState* s,int n){{ return (uint32_t)((s->d[n>>1] >> ((n&1)?32:0)) & 0xFFFFFFFFu); }}
static inline void vfp_s_set(RecState* s,int n,uint32_t v){{ int i=n>>1; uint64_t d=s->d[i]; if(n&1) d=(d&0xFFFFFFFFull)|((uint64_t)v<<32); else d=(d&0xFFFFFFFF00000000ull)|v; s->d[i]=d; }}
static inline float vfp_sf(RecState* s,int n){{ uint32_t b=vfp_s_get(s,n); float f; std::memcpy(&f,&b,4); return f; }}
static inline void vfp_sf_set(RecState* s,int n,float f){{ uint32_t b; std::memcpy(&b,&f,4); vfp_s_set(s,n,b); }}
static inline float vfpS(int n){{ float f=0; return f; }}
static inline uint64_t vfpD_get(RecState* s,int n){{ return s->d[n]; }}
static inline void vfp_d_set(RecState* s,int n,uint64_t v){{ s->d[n]=v; }}
extern "C" uint32_t hle_call(uint32_t stub_va, RecState* s);
extern "C" uint32_t id_reg(int idx, RecState* s);
extern "C" uint32_t id_tbb(uint32_t insn_pc, uint32_t addr, RecState* s);
extern "C" uint32_t id_tbh(uint32_t insn_pc, uint32_t base, uint32_t index, RecState* s);
extern "C" uint32_t id_mem(uint32_t addr, RecState* s);
typedef uint32_t (*BlockFn)(RecState*);
// APSR: N=0x80000000 Z=0x40000000 C=0x20000000 V=0x10000000
static inline void setf_state(RecState* s, uint32_t r, uint32_t c, uint32_t v) {{
  s->apsr = (s->apsr & 0x0FFFFFFFu) | (r & 0x80000000u) | ((r == 0) ? 0x40000000u : 0) | c | v;
}}
static inline void setf_add(RecState* s, uint32_t a, uint32_t b, uint32_t r) {{
  uint32_t c = ((uint64_t)a + (uint64_t)b > 0xFFFFFFFFull) ? 0x20000000u : 0;
  uint32_t v = ((~(a ^ b) & (a ^ r)) & 0x80000000u) >> 3;
  setf_state(s, r, c, v);
}}
static inline void setf_sub(RecState* s, uint32_t a, uint32_t b, uint32_t r) {{
  uint32_t c = (a >= b) ? 0x20000000u : 0;
  uint32_t v = (((a ^ b) & (a ^ r)) & 0x80000000u) >> 3;
  setf_state(s, r, c, v);
}}
static inline void setf_logic(RecState* s, uint32_t r) {{ setf_state(s, r, 0, 0); }}
static inline void setf_nz(RecState* s, uint32_t r) {{ setf_state(s, r, 0, 0); }}
static inline int cond_eq(RecState* s){{ return (s->apsr & 0x40000000u) != 0; }}
static inline int cond_ne(RecState* s){{ return (s->apsr & 0x40000000u) == 0; }}
static inline int cond_cs(RecState* s){{ return (s->apsr & 0x20000000u) != 0; }}
static inline int cond_cc(RecState* s){{ return (s->apsr & 0x20000000u) == 0; }}
static inline int cond_mi(RecState* s){{ return (s->apsr & 0x80000000u) != 0; }}
static inline int cond_pl(RecState* s){{ return (s->apsr & 0x80000000u) == 0; }}
static inline int cond_vs(RecState* s){{ return (s->apsr & 0x10000000u) != 0; }}
static inline int cond_vc(RecState* s){{ return (s->apsr & 0x10000000u) == 0; }}
static inline int cond_hi(RecState* s){{ return ((s->apsr & 0x20000000u) != 0) && ((s->apsr & 0x40000000u) == 0); }}
static inline int cond_ls(RecState* s){{ return ((s->apsr & 0x20000000u) == 0) || ((s->apsr & 0x40000000u) != 0); }}
static inline int cond_ge(RecState* s){{ return ((s->apsr & 0x80000000u) != 0) == ((s->apsr & 0x10000000u) != 0); }}
static inline int cond_lt(RecState* s){{ return ((s->apsr & 0x80000000u) != 0) != ((s->apsr & 0x10000000u) != 0); }}
static inline int cond_gt(RecState* s){{ return ((s->apsr & 0x40000000u) == 0) && (((s->apsr & 0x80000000u) != 0) == ((s->apsr & 0x10000000u) != 0)); }}
static inline int cond_le(RecState* s){{ return ((s->apsr & 0x40000000u) != 0) || (((s->apsr & 0x80000000u) != 0) != ((s->apsr & 0x10000000u) != 0)); }}
'''

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_LITTLE_ENDIAN); md.detail = True
batches = [funcs[i:i+BATCH] for i in range(0, len(funcs), BATCH)]
print("functions:", len(funcs), "batches:", len(batches), "batch size:", BATCH)

sup_all = []
def gen_batch(bi, batch):
    tbl = 0
    decls = []; table = []; defs = []
    emitted = set()
    ARM_STUB = bytes.fromhex("0000e0e31eff2fe1")   # MOV r0,#0; BX lr (ARM mode HLE import stub)
    for f in batch:
        o = vo(f["va"])
        if o is None: continue
        if f["va"] in STUBS or (f["size"] >= 8 and d[o:o+8] == ARM_STUB):
            if f["va"] in emitted: continue
            emitted.add(f["va"])
            tbl += 1
            decls.append("static uint32_t blk_%08X(RecState* s);" % f["va"])
            table.append("  { 0x%08Xu, blk_%08X }," % (f["va"], f["va"]))
            defs.append("static uint32_t blk_%08X(RecState* s) { g_cur_block = 0x%08Xu; return hle_call(0x%08Xu, s); }" % (f["va"], f["va"], f["va"]))
            continue
        insns = list(md.disasm(d[o:o+f["size"]], f["va"]))
        if not insns: continue
        leaders = set([f["va"]])
        for ins in insns:
            m = base_m(ins.mnemonic)
            tg = [(op.imm & 0xFFFFFFFF) for op in ins.operands if op.type == ARM_OP_IMM]
            tgt = ((tg[0] & 0xFFFFFFFF) & 0xFFFFFFFE) if tg else 0
            if ins.mnemonic in ("b","bl","blx","b.w") and tgt:
                leaders.add(tgt)
                if ins.mnemonic in ("bl","blx","b.w"): leaders.add(ins.address + ins.size)
            if m in ("cbz","cbnz") and tgt:
                leaders.add(tgt); leaders.add(ins.address + ins.size)
            if m.startswith("b") and m not in ("b","bl","bx","blx") and tgt:
                leaders.add(tgt); leaders.add(ins.address + ins.size)
            if ins.mnemonic in ("bx","cbz","cbnz") or m == "pop":
                leaders.add(ins.address + ins.size)
            if m in ("tbb","tbh"):
                mm = [op for op in ins.operands if op.type == ARM_OP_MEM]
                if mm and mm[0].mem.base == 11:  # PC-relative table base
                    base = (ins.address + 4) & ~3
                    for ii in range(256):
                        if m == "tbb":
                            bo = vo(base + ii)
                            if bo is None: break
                            t2 = (base + 2 * d[bo]) & ~1
                        else:
                            bo = vo(base + 2 * ii)
                            if bo is None or bo + 1 >= len(d): break
                            hv = d[bo] | (d[bo + 1] << 8)
                            t2 = (base + 2 * hv) & ~1
                        if 0x81000000 <= t2 < 0x812B2AF0:
                            leaders.add(t2)
        blocks = []; cur = None
        for ins in insns:
            if cur is None or ins.address in leaders:
                cur = {"start": ins.address, "insns": []}; blocks.append(cur)
            cur["insns"].append(ins)
            if is_term(ins): cur = None
        for b in blocks:
            if b["start"] in emitted: continue
            emitted.add(b["start"])
            tbl += 1
            decls.append("static uint32_t blk_%08X(RecState* s);" % b["start"])
            table.append("  { 0x%08Xu, blk_%08X }," % (b["start"], b["start"]))
            last = b["insns"][-1]; lm = base_m(last.mnemonic)
            tg = [(op.imm & 0xFFFFFFFF) for op in last.operands if op.type == ARM_OP_IMM]
            tgt = ((tg[0] & 0xFFFFFFFF) & 0xFFFFFFFE) if tg else 0
            fall = last.address + last.size
            if last.mnemonic in ("b","b.w") and tgt: term = "jmp %08X" % tgt
            elif last.mnemonic in ("bl","blx") and tgt: term = "call %08X %08X" % (tgt, fall)
            elif lm.startswith("b") and lm not in ("b","bl","bx","blx") and tgt:
                c = COND.get(lm); term = ("cond %s %08X %08X" % (c, tgt, fall)) if c else ("ret %08X" % fall)
            elif lm == "blx" and not tgt:
                term = "idreg %d" % (last.operands[0].reg if last.operands else -1)
            elif lm == "bx":
                reg = last.operands[0].reg if last.operands else -1
                term = "retlr" if reg == 10 else ("idreg %d" % reg)
            elif lm == "pop" and any(op.type == ARM_OP_REG and op.reg == 11 for op in last.operands):
                term = "pcret"

            elif lm in ("cbz","cbnz") and tgt:
                term = "cbz %s %d %08X %08X" % ("0" if lm=="cbz" else "1", last.operands[0].reg, tgt, fall)
            elif last.mnemonic in ("tbb","tbh"):
                mm = [op for op in last.operands if op.type == ARM_OP_MEM]
                if mm:
                    ms = mm[0]
                    base = ms.mem.base; idx = ms.mem.index
                    term = "tbl %s %d %d" % (last.mnemonic, base, idx)
                else: term = "indirect"
            elif lm in ("ldr","ldrb","ldrh") and any(op.type == ARM_OP_REG and op.reg == 11 for op in last.operands):
                mm = [op for op in last.operands if op.type == ARM_OP_MEM]
                term = ("idmem %d %d" % (mm[0].mem.base, mm[0].mem.disp & 0xFFFFFFFF)) if mm else "indirect"
            else: term = "ret %08X" % fall
            body = []
            _only = b["insns"][-1]
            _om = base_m(_only.mnemonic)
            if (len(b["insns"]) == 1 and not is_term(_only) and _om in ("add","adds","sub","subs")
                    and _only.operands and _only.operands[0].type == ARM_OP_REG and _only.operands[0].reg == 12
                    and len(_only.operands) >= 2 and _only.operands[1].type == ARM_OP_IMM):
                body += emit(_only, sup_all)
                body.append("return 0x%08Xu;" % (b["start"] + _only.size))
                defs.append("static uint32_t blk_%08X(RecState* s) { g_cur_block = 0x%08Xu; " % (b["start"], b["start"]) + " ".join(body) + " }")
                continue
            _last_ctrl = (is_term(_only) or _only.mnemonic in ("b","b.w","bl","blx","bx","pop","cbz","cbnz"))
            _seq = b["insns"] if not _last_ctrl else b["insns"][:-1]
            for ins in _seq:
                body += emit(ins, sup_all)
            if term.startswith("call"):
                _, t, ff = term.split()
                body.append("s->r[14] = 0x%su; return 0x%su; /* bl */" % (ff, t))
            elif term == "retlr":
                body.append("return s->r[14]; /* bx lr */")
            elif term == "pcret":
                regs = [op.reg for op in last.operands if op.type == ARM_OP_REG]
                for r in sorted(regs, key=RIDX):
                    if r == 11:   # pc
                        body.append("{ uint32_t _pc = rd32(s, s->r[13]); s->r[13] += 4; return _pc; }")
                    else:
                        body.append("%s = rd32(s, s->r[13]); s->r[13] += 4;" % R(r))
                if not any(r == 11 for r in regs):
                    body.append("return 0u;")
            elif term.startswith("jmp"): body.append("return 0x%su;" % term.split()[1])
            elif term.startswith("ret "):
                v = term.split()[1]; body.append("return %s;" % ("0u" if v == "0" else "0x%su" % v))
            elif term.startswith("idreg"):
                ri = int(term.split()[1])
                body.append("return id_reg(%d, s);" % ridx(ri))
            elif term.startswith("idmem"):
                parts = term.split(); base = int(parts[1]); disp = int(parts[2])
                body.append("return id_mem(s->r[%d] + 0x%Xu, s);" % (ridx(base), disp))
            elif term.startswith("tbl"):
                parts = term.split(); mnem = parts[1]; base = int(parts[2]); idx = int(parts[3])
                if base == 11: base_expr = "0x%08Xu" % (last.address + 4)   # 18.3835: tbb/tbh table base = insn+4 (NOT align4; verified via 0x811E597E: base=0x811E5982, entries 9/11/53...)
                else: base_expr = "s->r[%d]" % ridx(base)
                if last.mnemonic == "tbh":
                    body.append("return id_tbh(0x%08Xu, %s, s->r[%d], s);" % (last.address, base_expr, ridx(idx)))
                else:
                    body.append("return id_tbb(0x%08Xu, %s + s->r[%d], s);" % (last.address, base_expr, ridx(idx)))
            elif term == "indirect": body.append("return 0xFFFFFFFEu;")
            elif term.startswith("cond"):
                _, c, t, ff = term.split(" ", 3); body.append("if (%s) return 0x%su; return 0x%su;" % (c, t, ff))
            elif term.startswith("cbz"):
                _, z, reg, t, ff = term.split(); body.append("if (%s %s 0) return 0x%su; return 0x%su;" % (R(int(reg)), "==" if z=="0" else "!=", t, ff))
            defs.append("static uint32_t blk_%08X(RecState* s) { g_cur_block = 0x%08Xu; " % (b["start"], b["start"]) + " ".join(body) + " }")
    path = os.path.join(OUTDIR, "batch_%03d.cpp" % bi)
    txt = HEADER.format(bi, batch[0]["va"], batch[-1]["va"], len(table))
    txt += "\n".join(decls) + "\n\n"
    txt += "static const struct { uint32_t va; BlockFn fn; } g_blocks_%03d[] = {\n" % bi
    txt += "\n".join(table) + "\n};\n"
    txt += "struct BlkEntry { uint32_t va; BlockFn fn; };\n"
    txt += "extern \"C\" const BlkEntry* get_table_%03d(unsigned* cnt){ *cnt = sizeof(g_blocks_%03d)/sizeof(g_blocks_%03d[0]); return (const BlkEntry*)g_blocks_%03d; }\n\n" % (bi, bi, bi, bi)
    txt += "\n".join(defs) + "\n"
    txt += "extern \"C\" BlockFn find_gapblk_%03d(uint32_t pc){ for (unsigned i=0;i<sizeof(g_blocks_%03d)/sizeof(g_blocks_%03d[0]);++i) if (g_blocks_%03d[i].va==pc) return g_blocks_%03d[i].fn; return nullptr; }\n" % (bi, bi, bi, bi, bi)
    open(path, "w", encoding="utf-8").write(txt)
    return path, tbl

gxx = r"C:\dev\mingw64\bin\g++.exe"
results = []
t0 = time.time()
# generate all batches first (sequential is fine)
gen = []
for bi, batch in enumerate(batches):
    gen.append(gen_batch(bi, batch))
print("generated %d batch files in %.1fs" % (len(gen), time.time()-t0))
# compile in parallel
def compile_one(item):
    bi = int(os.path.basename(item[0]).split("_")[1].split(".")[0])
    path = item[0]; obj = path[:-4] + ".o"
    t = time.time()
    r = subprocess.run([gxx, "-std=c++17", "-O2", "-c", path, "-o", obj], capture_output=True, text=True)
    return (bi, r.returncode, time.time()-t, os.path.getsize(obj) if os.path.exists(obj) else 0, (r.stderr or "")[:300])
t1 = time.time()
with concurrent.futures.ThreadPoolExecutor(max_workers=PARALLEL) as ex:
    results = list(ex.map(compile_one, gen))
dt = time.time()-t1
ok = [r for r in results if r[1] == 0]
print("compiled: %d/%d ok in %.1fs (parallel %d)" % (len(ok), len(results), dt, PARALLEL))
print("total obj bytes:", sum(r[3] for r in ok))
bad = [r for r in results if r[1] != 0]
for b in bad[:5]: print("FAIL batch %d rc=%d err=%s" % (b[0], b[1], b[4]))
print("total batch lines:", sum(sum(1 for _ in open(g[0], encoding="utf-8")) for g in gen))
