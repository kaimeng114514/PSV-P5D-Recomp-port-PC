# -*- coding: utf-8 -*-
# 18.3633 static-recomp generator v2: normalized Thumb-2/VFP semantics, fixed VFP register mapping
#   (capstone: ARM_REG_R0=66, ARM_REG_S0=79, ARM_REG_D0=14), wide forms, ubfx/bfi, mul/mla/smull, ror, b.w,
#   tbb/tbh indirect (dispatch marker), 2-operand ALU forms.
# Usage: py _gen_batch2.py [NFUNCS]   (default: all 12917 functions)
import json, struct, subprocess, os, time, sys, collections
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN
from capstone.arm import *

ELF = r"C:\dev\p5d_build\p5d_true.elf"
BOUNDS = r"C:\dev\tmp\function_bounds.json"
OUTDIR = r"C:\dev\tmp\recomp"
NFUNCS = int(sys.argv[1]) if len(sys.argv) > 1 else 12917
os.makedirs(OUTDIR, exist_ok=True)

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
funcs = json.load(open(BOUNDS))[:NFUNCS]

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_LITTLE_ENDIAN); md.detail = True

def R(r):
    # capstone register numbers -> guest register file indexes
    if r >= 79: return "vfpS(%d)" % (r - 79)      # VFP single regs (S0 = 79)
    if 14 <= r <= 45: return "vfpD(%d)" % (r - 14)  # VFP double regs (D0 = 14)
    if 66 <= r <= 78: return "s->r[%d]" % (r - 66)  # R0..R12
    if r == 12: return "s->r[13]"                   # SP
    if r == 10: return "s->r[14]"                   # LR
    if r == 11: return "s->r[15]"                   # PC
    return "s->r[%d]" % (r & 0x0F)

def RIDX(r):
    """guest register index for a capstone register id"""
    if 66 <= r <= 78: return r - 66
    if r == 13: return 13
    if r == 12: return 13
    if r == 10: return 14
    if r == 11: return 15
    return r & 0x0F

def is_vfp_reg(ins):
    for op in ins.operands:
        if op.type == ARM_OP_REG and (op.reg >= 79 or 14 <= op.reg <= 45):
            return True
    return False

COND = {"beq":"cond_eq(s)","bne":"cond_ne(s)","bmi":"cond_mi(s)","bpl":"cond_pl(s)",
 "bcs":"cond_cs(s)","bhs":"cond_cs(s)","bcc":"cond_cc(s)","blo":"cond_cc(s)","bvs":"cond_vs(s)","bvc":"cond_vc(s)",
 "bhi":"cond_hi(s)","bls":"cond_ls(s)","bge":"cond_ge(s)","blt":"cond_lt(s)",
 "bgt":"cond_gt(s)","ble":"cond_le(s)"}

def base_m(m): return m[:-2] if m.endswith(".w") else m

def is_term(ins):
    m = base_m(ins.mnemonic)
    if m in ("b","bx","blx","pop","cbz","cbnz"): return True
    # NOTE: "bics" is a NON-control-flow instruction (bit clear + flags); it was missing from the exclusion
    # list, so blocks whose last instruction was bics silently dropped it (18.3750). Keep this list complete:
    if m.startswith("b") and m not in ("b","bl","bx","blx","bic","bics","bfi","bfc"): return True
    if ins.mnemonic in ("tbb","tbh"): return True
    if m in ("ldr","ldrb","ldrh") and any(o.type==ARM_OP_REG and o.reg==11 for o in ins.operands): return True
    if m in ("svc","bkpt","udf","hlt"): return True
    return False

def emit(ins, sup):
    m = base_m(ins.mnemonic); ops = ins.operands; L = []
    _CONDSUF = ("eq","ne","cs","hs","cc","lo","mi","pl","vs","vc","hi","ls","ge","lt","gt","le")
    _KNOWN = {"add","adds","sub","subs","rsb","rsbs","mov","movs","movw","movt","ldr","ldrb","ldrh","ldrsh",
              "ldrsb","ldrd","str","strb","strh","strd","cmp","cmn","tst","teq","and","ands","orr","orrs","eor",
              "eors","bic","bics","mvn","mvns","lsl","lsls","lsr","lsrs","asr","asrs","ror","rors","mul","mls",
              "adc","adcs","sbc","sbcs","adr","rev","rev16","rev32",
              "mla","ubfx","sbfx","bfi","bfc","ldm","ldmia","ldmdb","stm","stmia","stmdb","push","pop","vldr",
              "vstr","vmov","vadd.f32","vsub.f32","vmul.f32","vdiv.f32","vmla.f32","vmls.f32","vcmp.f32","vmrs",
              "vpush","vpop","nop","it","tbb","tbh","bx","blx","bl","b"}
    if m not in _KNOWN and len(m) > 2 and m[-2:] in _CONDSUF and m[:-2] in _KNOWN:
        m = m[:-2]  # condition-suffixed instruction (inside IT block): emit unconditionally in the prototype
    def A(i): return ops[i] if i < len(ops) else None
    def reg(i): return ops[i].reg
    def _shreg(i):
        r = R(ops[i].reg)
        sh = getattr(ops[i], 'shift', None)
        if sh is None: return r
        if isinstance(sh, (tuple, list)):
            st = sh[0] if len(sh) > 0 else 0; sv = sh[1] if len(sh) > 1 else 0
        else:
            st = getattr(sh, 'type', 0) or 0; sv = getattr(sh, 'value', 0) or 0
        if st == 2: return '(%s << %d)' % (r, sv)             # 18.4129 LSL (capstone5: 2)
        if st == 3: return '(%s >> %d)' % (r, sv)             # 18.4129 LSR (3)
        if st == 1: return '(((int32_t)%s) >> %d)' % (r, sv)  # 18.4129 ASR (1)
        if st == 4: return '(((%s >> %d) | (%s << (32-%d))))' % (r, sv, r, sv)  # ROR (4)
        return r
    def imm(i): return ops[i].imm & 0xFFFFFFFF
    def mem(i):
        o = ops[i].mem
        e = R(o.base)
        if o.index:
            ridx = R(o.index)
            sh = getattr(ops[i], "shift", None)   # 18.4129: shift lives on the OPERAND, not on o.mem
            st = 0; sv = 0
            if sh is not None:
                if isinstance(sh, (tuple, list)):
                    st = sh[0] if len(sh) > 0 else 0; sv = sh[1] if len(sh) > 1 else 0
                else:
                    st = getattr(sh, "type", 0) or 0; sv = getattr(sh, "value", 0) or 0
            sc = getattr(o, "scale", 1) or 1
            if st == 2: e += " + (%s << %d)" % (ridx, sv)             # LSL
            elif st == 3: e += " + (%s >> %d)" % (ridx, sv)           # LSR
            elif st == 1: e += " + (((int32_t)%s) >> %d)" % (ridx, sv)  # ASR
            elif sc and sc > 1: e += " + (%s * %d)" % (ridx, sc)
            else: e += " + " + ridx
        if o.disp:
            e += " + 0x%Xu" % (o.disp & 0xFFFFFFFF)
        return (e, 0)
    def isr(i): return A(i) is not None and ops[i].type == ARM_OP_REG
    def isi(i): return A(i) is not None and ops[i].type in (2, 4)
    def _wb(i):   # 18.4183: post-index/pre-index base writeback for ldr/str
        if i >= len(ops) or not getattr(ins, "writeback", False): return []
        o = ops[i]
        if o.type != ARM_OP_MEM: return []
        base = R(o.mem.base)
        _wbo = []
        if len(ops) > i + 1 and ops[i+1].type == ARM_OP_IMM:
            d = ops[i+1].imm
            if d != 0: _wbo.append("%s = %s %s 0x%Xu;" % (base, base, "+" if d >= 0 else "-", abs(d) & 0xFFFFFFFF))
        elif len(ops) > i + 1 and ops[i+1].type == ARM_OP_REG:
            _wbo.append("%s = %s + %s;" % (base, base, R(ops[i+1].reg)))
        elif o.mem.disp:
            d = o.mem.disp
            _wbo.append("%s = %s %s 0x%Xu;" % (base, base, "+" if d >= 0 else "-", abs(d) & 0xFFFFFFFF))
        return _wbo
    def _isgpr(i):
        return i < len(ops) and ops[i].type == ARM_OP_REG and (66 <= ops[i].reg <= 78 or ops[i].reg in (10, 11, 12))
    def _basereg(i):
        if i >= len(ops): return None
        def _gpr(v): return (66 <= v <= 78) or v in (10, 11, 12)
        if ops[i].type == ARM_OP_MEM:
            b = ops[i].mem.base
            return b if _gpr(b) else None
        if ops[i].type == ARM_OP_REG:
            r = ops[i].reg
            return r if _gpr(r) else None
        return None
    # --- NEON/vector guard: NEON ops on D/Q regs are not implemented yet -> comment out (prototype) ---
    _dregs = [op.reg for op in ops if op.type == ARM_OP_REG and (14 <= op.reg <= 45 or 50 <= op.reg <= 65)]
    _vfp_ok = ("vldr","vstr","vcvt.f64.f32","vcvt.f32.f64","vcmp.f64","vneg.f64","vabs.f64")
    if _dregs and not (m in _vfp_ok and len(ops) <= 3):
        # 18.3921: NEON subset implementation (integer/bitwise first). D regs 14..45 -> s->d[0..31].
        if m in ("vnmls.f32","vnmla.f32","vrsqrts.f32","vrsqrte.f32","vsqrt.f32","vrecpe.f32","vrecps.f32","vceq.i32","vtst.32","vmax.f32","vmin.f32","vacgt.f32","vadd.i32","vsub.i32","vtrn.32","vbit","vext.64") and len(ops) >= 2:
            def _lanes(i):
                if i >= len(ops) or ops[i].type != 1: return None
                r = ops[i].reg
                if 14 <= r <= 45: return [r - 14]
                if 50 <= r <= 65: return [2*(r-50), 2*(r-50)+1]
                return None
            _L0 = _lanes(0)
            _L1 = _lanes(1) if len(ops) > 1 else None
            _L2 = _lanes(2) if len(ops) > 2 else None
            _NR = int(getattr(ops[0], "neo", 0)) if False else 0
            if _L0 and _L1:
                _n = min(len(_L0), len(_L1), len(_L2) if _L2 else len(_L0))
                _st = []
                for _k in range(_n):
                    _a, _b = _L0[_k], _L1[_k]
                    _c = _L2[_k] if _L2 else _a
                    if m in ("vadd.i32","vsub.i32"):
                        _op = "+" if m.startswith("vadd") else "-"
                        _st.append("{ uint32_t _x[2], _y[2], _z[2]; memcpy(_x,&s->d[%d],8); memcpy(_y,&s->d[%d],8); _z[0]=_x[0]%s_y[0]; _z[1]=_x[1]%s_y[1]; uint64_t _r; memcpy(&_r,_z,8); s->d[%d]=_r; }" % (_a,_b,_op,_op,_a))
                    elif m in ("vmax.f32","vmin.f32"):
                        _op = ">" if m.startswith("vmax") else "<"
                        _st.append("{ uint32_t _x[2], _y[2], _z[2]; memcpy(_x,&s->d[%d],8); memcpy(_y,&s->d[%d],8); for(int _i=0;_i<2;_i++){ float _fa,_fb; memcpy(&_fa,(char*)_x+_i*4,4); memcpy(&_fb,(char*)_y+_i*4,4); float _fo = (_fa %s _fb) ? _fa : _fb; memcpy((char*)_z+_i*4,&_fo,4);} uint64_t _r; memcpy(&_r,_z,8); s->d[%d]=_r; }" % (_a,_b,_op,_a))
                    elif m == "vrsqrts.f32":
                        _st.append("{ uint32_t _x[2], _y[2], _z[2]; memcpy(_x,&s->d[%d],8); memcpy(_y,&s->d[%d],8); for(int _i=0;_i<2;_i++){ float _fa,_fb; memcpy(&_fa,(char*)_x+_i*4,4); memcpy(&_fb,(char*)_y+_i*4,4); float _fo = (3.0f - _fa*_fb)*0.5f; memcpy((char*)_z+_i*4,&_fo,4);} uint64_t _r; memcpy(&_r,_z,8); s->d[%d]=_r; }" % (_a,_b,_a))
                    elif m == "vrsqrte.f32":
                        _st.append("{ uint32_t _x[2], _z[2]; memcpy(_x,&s->d[%d],8); for(int _i=0;_i<2;_i++){ float _fa; memcpy(&_fa,(char*)_x+_i*4,4); float _fo = 1.0f / __builtin_sqrtf(_fa); memcpy((char*)_z+_i*4,&_fo,4);} uint64_t _r; memcpy(&_r,_z,8); s->d[%d]=_r; }" % (_a,_a))
                    elif m == "vsqrt.f32":
                        _st.append("{ uint32_t _x[2], _z[2]; memcpy(_x,&s->d[%d],8); for(int _i=0;_i<2;_i++){ float _fa; memcpy(&_fa,(char*)_x+_i*4,4); float _fo = __builtin_sqrtf(_fa); memcpy((char*)_z+_i*4,&_fo,4);} uint64_t _r; memcpy(&_r,_z,8); s->d[%d]=_r; }" % (_a,_a))
                    elif m == "vrecpe.f32":
                        _st.append("{ uint32_t _x[2], _z[2]; memcpy(_x,&s->d[%d],8); for(int _i=0;_i<2;_i++){ float _fa; memcpy(&_fa,(char*)_x+_i*4,4); float _fo = 1.0f / _fa; memcpy((char*)_z+_i*4,&_fo,4);} uint64_t _r; memcpy(&_r,_z,8); s->d[%d]=_r; }" % (_a,_a))
                    elif m == "vrecps.f32":
                        _st.append("{ uint32_t _x[2], _y[2], _z[2]; memcpy(_x,&s->d[%d],8); memcpy(_y,&s->d[%d],8); for(int _i=0;_i<2;_i++){ float _fa,_fb; memcpy(&_fa,(char*)_x+_i*4,4); memcpy(&_fb,(char*)_y+_i*4,4); float _fo = 2.0f - _fa*_fb; memcpy((char*)_z+_i*4,&_fo,4);} uint64_t _r; memcpy(&_r,_z,8); s->d[%d]=_r; }" % (_a,_b,_a))
                    elif m in ("vnmls.f32","vnmla.f32"):
                        _sgn = "-" if m == "vnmls.f32" else "+"
                        _st.append("{ uint32_t _x[2], _y[2], _z[2]; memcpy(_x,&s->d[%d],8); memcpy(_y,&s->d[%d],8); memcpy(_z,&s->d[%d],8); for(int _i=0;_i<2;_i++){ float _fa,_fb,_fc; memcpy(&_fa,(char*)_x+_i*4,4); memcpy(&_fb,(char*)_y+_i*4,4); memcpy(&_fc,(char*)_z+_i*4,4); float _fo = _fc %s _fa*_fb; memcpy((char*)_z+_i*4,&_fo,4);} uint64_t _r; memcpy(&_r,_z,8); s->d[%d]=_r; }" % (_c,_a,_b,_a,_sgn,_a))
                    elif m == "vtrn.32":
                        _st.append("{ uint32_t _x=(uint32_t)(s->d[%d]&0xFFFFFFFFu), _y=(uint32_t)(s->d[%d]&0xFFFFFFFFu); s->d[%d]=(s->d[%d]&0xFFFFFFFF00000000ull)|_y; s->d[%d]=(s->d[%d]&0xFFFFFFFF00000000ull)|_x; }" % (_a,_b,_a,_a,_b,_b))
                    elif m == "vbit":
                        _st.append("s->d[%d] = (s->d[%d] & ~s->d[%d]) | (s->d[%d] & s->d[%d]);" % (_a,_a,_b,_c,_b))
                if _st:
                    L.append(" ".join(_st))
                    return L
        def _dq(i):
            if i >= len(ops) or ops[i].type != 1: return None
            r = ops[i].reg
            if 14 <= r <= 45: return [r - 14]
            if 50 <= r <= 65: return [2 * (r - 50), 2 * (r - 50) + 1]
            return None
        # [LAST18] final 18: vext.32 Q form (32-bit lane concat) + vcvt.s32.f32 fixed-point form
        if m == "vext.32" and len(ops) >= 3 and _dq(0) and _dq(1) and _dq(2):
            _im = imm(3) if len(ops) > 3 else 0
            _a, _b, _c = _dq(0), _dq(1), _dq(2)
            def _lanes_of_ds(_ds):
                _o = []
                for _d in _ds:
                    _o.append((_d, 0)); _o.append((_d, 1))
                return _o
            _ln = _lanes_of_ds(_b) + _lanes_of_ds(_c)
            _ld = _lanes_of_ds(_a)
            _st = []
            for _k in range(len(_ld)):
                _src = _ln[_k + _im] if (_k + _im) < len(_ln) else _ln[-1]
                _st.append("{ uint32_t _v = (uint32_t)(s->d[%d] >> %d); memcpy((char*)&s->d[%d] + %d, &_v, 4); }"
                           % (_src[0], _src[1]*32, _ld[_k][0], _ld[_k][1]*32))
            L.append(" ".join(_st)); return L
        if m in ("vcvt.s32.f32","vcvt.u32.f32") and len(ops) == 3 and _dq(0) and _dq(1) and ops[2].type in (2, 4):
            _fb = imm(2) & 0x3F
            _mul = (1 << _fb) if _fb < 31 else 0x7FFFFFFF
            _st = []
            for _d0, _d1 in zip(_dq(0), _dq(1)):
                _st.append("{ float _a[2]; memcpy(_a, &s->d[%d], 8); int32_t _i0=(int32_t)(_a[0]*(float)%d), _i1=(int32_t)(_a[1]*(float)%d); s->d[%d]=(uint64_t)(uint32_t)_i0|((uint64_t)(uint32_t)_i1<<32); }"
                           % (_d1, _mul, _mul, _d0))
            L.append(" ".join(_st)); return L

            if i >= len(ops) or ops[i].type != 1: return None
            r = ops[i].reg
            if 14 <= r <= 45: return [r - 14]
            if 50 <= r <= 65: return [2 * (r - 50), 2 * (r - 50) + 1]
            return None
        if m in ("vorr","vand","veor","vbsl","vbic","vbif","vorn") and len(ops) == 3:
            _a, _b, _c = _dq(0), _dq(1), _dq(2)
            if _a and _b and _c:
                _n = min(len(_a), len(_b), len(_c))
                _st = []
                for _k in range(_n):
                    if m == "vorr": _st.append("s->d[%d] = s->d[%d] | s->d[%d];" % (_a[_k], _b[_k], _c[_k]))
                    elif m == "vand": _st.append("s->d[%d] = s->d[%d] & s->d[%d];" % (_a[_k], _b[_k], _c[_k]))
                    elif m == "veor": _st.append("s->d[%d] = s->d[%d] ^ s->d[%d];" % (_a[_k], _b[_k], _c[_k]))
                    elif m == "vbic": _st.append("s->d[%d] = s->d[%d] & ~s->d[%d];" % (_a[_k], _b[_k], _c[_k]))
                    elif m == "vbif": _st.append("s->d[%d] = (s->d[%d] & s->d[%d]) | (~s->d[%d] & s->d[%d]);" % (_a[_k], _b[_k], _a[_k], _b[_k], _c[_k]))
                    elif m == "vbsl": _st.append("{ uint64_t _m = s->d[%d]; s->d[%d] = (_m & s->d[%d]) | (~_m & s->d[%d]); }" % (_a[_k], _a[_k], _b[_k], _c[_k]))
                L.append(" ".join(_st))
                return L

        # [QFORM] Q/S forms for the newly classified NEON tail (18.3936)
        def _lanes2(i):
            if i >= len(ops): return None
            op = ops[i]
            if op.type == 1:
                r = op.reg
                if 14 <= r <= 45: return [r - 14]
                if 50 <= r <= 65: return [2*(r-50), 2*(r-50)+1]
                if r >= 79: return ["S%d" % (r - 79)]
            return None
        if m in ("vmov.i32","vmov.u32","vmov.i64") and len(ops) == 2 and _dq(0) and ops[1].type in (2, 4):
            _v = imm(1) & 0xFFFFFFFF
            L.append(" ".join(["s->d[%d] = 0x%Xull | (0x%Xull << 32);" % (d, _v, _v) for d in _dq(0)])); return L
        if m in ("vmov.f32","vmov.f64") and len(ops) == 2 and _dq(0) and ops[1].type in (2, 4):
            import re as _re
            _mm = _re.search(r"#([-+]?[0-9]+(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?)", ins.op_str)
            if _mm:
                _fv = float(_mm.group(1)); _fs = ("%.9g" % _fv)
                if "." not in _fs and "e" not in _fs and "E" not in _fs: _fs += ".0"
                L.append(" ".join(["{ float _q=%sf; uint32_t _bb; memcpy(&_bb,&_q,4); s->d[%d]=_bb|((uint64_t)_bb<<32); }" % (_fs, d) for d in _dq(0)])); return L
        if m in ("vneg.f32","vabs.f32") and len(ops) == 2 and _dq(0) and _dq(1):
            if m == "vneg.f32":
                L.append(" ".join(["s->d[%d] = s->d[%d] ^ 0x8000000080000000ull;" % (d0, d1) for d0, d1 in zip(_dq(0), _dq(1))]))
            else:
                L.append(" ".join(["s->d[%d] = s->d[%d] & 0x7FFFFFFF7FFFFFFFull;" % (d0, d1) for d0, d1 in zip(_dq(0), _dq(1))]))
            return L
        if m in ("vcvt.s32.f32","vcvt.f32.s32") and len(ops) == 2 and _dq(0) and _dq(1):
            _st = []
            for _d0, _d1 in zip(_dq(0), _dq(1)):
                if m == "vcvt.s32.f32":
                    _st.append("{ float _a[2]; memcpy(_a,&s->d[%d],8); int32_t _i0=(int32_t)_a[0],_i1=(int32_t)_a[1]; s->d[%d]=(uint64_t)(uint32_t)_i0|((uint64_t)(uint32_t)_i1<<32); }" % (_d1, _d0))
                else:
                    _st.append("{ uint32_t _a[2]; memcpy(_a,&s->d[%d],8); float _f0=(float)(int32_t)_a[0],_f1=(float)(int32_t)_a[1]; uint32_t _b0,_b1; memcpy(&_b0,&_f0,4); memcpy(&_b1,&_f1,4); s->d[%d]=(uint64_t)_b0|((uint64_t)_b1<<32); }" % (_d1, _d0))
            L.append(" ".join(_st)); return L
        if m == "vext.64" and len(ops) >= 3 and _dq(0) and _dq(1) and _dq(2):
            _im = imm(3) if len(ops) > 3 else 1
            _a, _b, _c = _dq(0), _dq(1), _dq(2)
            _src = _b if (_im % 2 == 0) else _c
            L.append(" ".join(["s->d[%d] = s->d[%d];" % (d0, d1) for d0, d1 in zip(_a, _src)])); return L
        if m in ("vceq.i32","vcle.f32","vtst.32","vacgt.f32") and len(ops) == 3 and _dq(0) and _dq(1):
            if ops[2].type in (2, 4):
                _imv = imm(2)
                _st = []
                for _d0, _d1 in zip(_dq(0), _dq(1)):
                    if m == "vceq.i32": _cond = "_a[_i]==%du" % _imv
                    elif m == "vcle.f32": _cond = "((int32_t)_a[_i] <= (int32_t)%d)" % _imv
                    elif m == "vtst.32": _cond = "((_a[_i] & %du) != 0u)" % _imv
                    else: _cond = "((_a[_i] & 0x7FFFFFFFu) > (%du & 0x7FFFFFFFu))" % _imv
                    _st.append("{ uint32_t _a[2]; memcpy(_a,&s->d[%d],8); uint64_t _o=0; for(int _i=0;_i<2;_i++){ if(%s){ uint32_t _m=0xFFFFFFFFu; memcpy((char*)&_o+_i*4,&_m,4);} } s->d[%d]=_o; }" % (_d1, _cond, _d0))
                L.append(" ".join(_st)); return L
            elif _dq(2):
                _d2 = _dq(2)
                _st = []
                for _k, (_d0, _d1) in enumerate(zip(_dq(0), _dq(1))):
                    _dd2 = _d2[min(_k, len(_d2)-1)]
                    if m == "vceq.i32": _cond = "_a[_i]==_b[_i]"
                    elif m == "vtst.32": _cond = "((_a[_i] & _b[_i]) != 0u)"
                    elif m == "vacgt.f32": _cond = "((_a[_i] & 0x7FFFFFFFu) > (_b[_i] & 0x7FFFFFFFu))"
                    else: _cond = "((int32_t)_a[_i] <= (int32_t)_b[_i])"
                    _st.append("{ uint32_t _a[2], _b[2]; memcpy(_a,&s->d[%d],8); memcpy(_b,&s->d[%d],8); uint64_t _o=0; for(int _i=0;_i<2;_i++){ if(%s){ uint32_t _m=0xFFFFFFFFu; memcpy((char*)&_o+_i*4,&_m,4);} } s->d[%d]=_o; }" % (_d1, _dd2, _cond, _d0))
                L.append(" ".join(_st)); return L
        def _D(i):
            o = ops[i]
            return "s->d[%d]" % (o.reg - 14)
        def _isd(i):
            return i < len(ops) and ops[i].type == ARM_OP_REG and 14 <= ops[i].reg <= 45
        def _isr2(i):
            return i < len(ops) and ops[i].type == ARM_OP_REG and 0 <= ops[i].reg <= 12
        if m == "vorr" and len(ops) == 3 and _isd(0) and _isd(1) and _isd(2):
            L.append("%s = %s | %s;" % (_D(0), _D(1), _D(2)))
        elif m == "vand" and len(ops) == 3 and _isd(0) and _isd(1) and _isd(2):
            L.append("%s = %s & %s;" % (_D(0), _D(1), _D(2)))
        elif m == "veor" and len(ops) == 3 and _isd(0) and _isd(1) and _isd(2):
            L.append("%s = %s ^ %s;" % (_D(0), _D(1), _D(2)))
        elif m == "vbsl" and len(ops) == 3 and _isd(0) and _isd(1) and _isd(2):
            L.append("{ uint64_t _m = %s; %s = (_m & %s) | (~_m & %s); }" % (_D(0), _D(0), _D(1), _D(2)))
        elif m.startswith("vdup") and len(ops) == 2 and _isd(0) and _isr2(1):
            L.append("%s = (uint64_t)%s | ((uint64_t)%s << 32);" % (_D(0), R(reg(1)), R(reg(1))))
        elif m in ("vmov.i32","vmov.i64") and len(ops) == 2 and _isd(0) and isi(1):
            v = imm(1) & 0xFFFFFFFF
            L.append("%s = 0x%Xull | (0x%Xull << 32);" % (_D(0), v, v))
        elif m in ("vmov") and len(ops) == 2 and _isd(0) and _isd(1):
            L.append("%s = %s;" % (_D(0), _D(1)))
        elif m in ("vsli.64","vsri.64") and len(ops) == 3 and _isd(0) and _isd(1) and isi(2):
            sh = imm(2) & 63
            if m.startswith("vsli"):
                L.append("%s = (%s << %d) | (%s & 0x%Xull);" % (_D(0), _D(1), sh, _D(0), (1 << sh) - 1 if sh < 64 else 0))
            else:
                L.append("%s = (%s >> %d) | (%s & ~0x%Xull);" % (_D(0), _D(1), sh, _D(0), ((1 << sh) - 1) if sh < 64 else 0))
        elif m in ("vshl.i64","vshr.u64","vshl.i32","vshr.u32") and len(ops) == 3 and _isd(0) and _isd(1) and isi(2):
            sh = imm(2) & 63
            op_s = "<<" if m.startswith("vshl") else ">>"
            L.append("%s = %s %s %d;" % (_D(0), _D(1), op_s, sh))
        elif m in ("vldmia","vstmia") and len(ops) >= 2 and _basereg(0) is not None:
            base = R(_basereg(0))
            rr = [o.reg for o in ops[1:] if o.type == ARM_OP_REG and 14 <= o.reg <= 45]
            if rr:
                off = 0
                for q in rr:
                    if m == "vldmia":
                        L.append("s->d[%d] = (uint64_t)rd32(s, %s + 0x%Xu) | ((uint64_t)rd32(s, %s + 0x%Xu) << 32);"
                                 % (q-14, base, off, base, off+4))
                    else:
                        L.append("wr32(s, %s + 0x%Xu, (uint32_t)(s->d[%d] & 0xFFFFFFFFu)); wr32(s, %s + 0x%Xu, (uint32_t)(s->d[%d] >> 32));"
                                 % (base, off, q-14, base, off+4, q-14))
                    off += 8
                if getattr(ins, "writeback", False):
                    L.append("%s += 0x%Xu;" % (base, off))
            else:
                sup.append((ins.address, ins.mnemonic, ins.op_str))
                L.append("/* UNSUPPORTED_NEON %s %s */" % (ins.mnemonic, ins.op_str))
        elif m == "vdup.32" and len(ops) == 2:
            _dc = None
            if ops[0].type == 1:
                if 14 <= ops[0].reg <= 45: _dc = ("D", ops[0].reg - 14)
                elif 50 <= ops[0].reg <= 65: _dc = ("Q", 2 * (ops[0].reg - 50))
            _sv = None
            if ops[1].type == 1:
                if 14 <= ops[1].reg <= 45: _sv = ("D", ops[1].reg - 14)
                elif 50 <= ops[1].reg <= 65: _sv = ("Q", 2 * (ops[1].reg - 50))
                elif 66 <= ops[1].reg <= 78: _sv = ("GPR", ops[1].reg - 66)
                elif ops[1].reg in (10, 11, 12): _sv = ("GPR", {10: 14, 11: 15, 12: 13}[ops[1].reg])
            if _dc and _sv:
                lane = getattr(ops[1], "neon_lane", 0)
                lane = lane if isinstance(lane, int) and lane >= 0 else 0
                if _sv[0] == "GPR":
                    _v = "s->r[%d]" % _sv[1]
                else:
                    _v = "((uint32_t)(s->d[%d] >> %d))" % (_sv[1], (lane % 2) * 32)
                _st = ["s->d[%d] = (uint64_t)(%s) | ((uint64_t)(%s) << 32);" % (_dc[1], _v, _v)]
                if _dc[0] == "Q":
                    _st.append("s->d[%d] = (uint64_t)(%s) | ((uint64_t)(%s) << 32);" % (_dc[1] + 1, _v, _v))
                L.append("{ %s }" % " ".join(_st))
            else:
                sup.append((ins.address, ins.mnemonic, ins.op_str))
                L.append("/* UNSUPPORTED_NEON %s %s */" % (ins.mnemonic, ins.op_str))
        elif m in ("vmul.f32","vadd.f32","vsub.f32","vmla.f32","vmls.f32","vdiv.f32") and len(ops) >= 3:
            def _V(regno):
                if 14 <= regno <= 45: return ("D", regno - 14)
                if 50 <= regno <= 65: return ("Q", regno - 50)
                return None
            d0 = _V(ops[0].reg) if ops[0].type == 1 else None
            n0 = _V(ops[1].reg) if ops[1].type == 1 else None
            b0 = _V(ops[2].reg) if ops[2].type == 1 else None
            scalar = (getattr(ops[2], "neon_lane", None) is not None)
            if d0 and n0 and b0:
                opc = {"vmul.f32":"*","vadd.f32":"+","vsub.f32":"-","vdiv.f32":"/","vmla.f32":"+","vmls.f32":"-"}[m]
                lanes = 4 if (d0[0] == "Q") else 2
                dbase = 2*d0[1] if d0[0] == "Q" else d0[1]
                nbase = 2*n0[1] if n0[0] == "Q" else n0[1]
                bbase = 2*b0[1] if b0[0] == "Q" else b0[1]
                lane = getattr(ops[2], "neon_lane", 0) or 0
                stmts = []
                for k in range(lanes):
                    dk = dbase + (k // 2)
                    half = k % 2
                    nin = nbase + (k // 2)
                    stmts.append("__builtin_memcpy(&_f, (char*)&s->d[%d] + %d, 4); _af = _f;" % (nin, half*4))
                    if scalar:
                        stmts.append("__builtin_memcpy(&_f, (char*)&s->d[%d] + %d, 4); _bf = _f;" % (bbase, (lane % 2)*4))
                    else:
                        bin_ = bbase + (k // 2)
                        stmts.append("__builtin_memcpy(&_f, (char*)&s->d[%d] + %d, 4); _bf = _f;" % (bin_, half*4))
                    if m in ("vmla.f32","vmls.f32"):
                        stmts.append("__builtin_memcpy(&_f, (char*)&s->d[%d] + %d, 4); _cf = _f;" % (dk, half*4))
                        stmts.append("_of = _cf %s (_af * _bf);" % ("+" if m == "vmla.f32" else "-"))
                    else:
                        stmts.append("_of = _af %s _bf;" % opc)
                    stmts.append("__builtin_memcpy((char*)&s->d[%d] + %d, &_of, 4);" % (dk, half*4))
                L.append("{ float _af, _bf, _cf, _of, _f; %s }" % " ".join(stmts))
            else:
                sup.append((ins.address, ins.mnemonic, ins.op_str))
                L.append("/* UNSUPPORTED_NEON %s %s */" % (ins.mnemonic, ins.op_str))
        elif m == "vext.32" and len(ops) >= 3 and _isd(0) and _isd(1) and _isd(2):
            imm4 = imm(3) if len(ops) > 3 else 1
            sh = (imm4 % 4) * 32
            if sh == 0:
                L.append("s->d[%d] = s->d[%d];" % (ops[0].reg-14, ops[1].reg-14))
            else:
                L.append("s->d[%d] = (s->d[%d] >> %d) | (s->d[%d] << (64-%d));"
                         % (ops[0].reg-14, ops[1].reg-14, sh, ops[2].reg-14, sh))
        elif m == "vpadd.f32" and len(ops) == 3 and _isd(0) and _isd(1) and _isd(2):
            L.append("{ uint32_t _a[2], _b[2]; memcpy(_a, &s->d[%d], 8); memcpy(_b, &s->d[%d], 8); float _r0 = ((float*)&_a)[0] + ((float*)&_a)[1]; float _r1 = ((float*)&_b)[0] + ((float*)&_b)[1]; uint64_t _o; memcpy(&_o, &_r0, 4); memcpy((char*)&_o+4, &_r1, 4); s->d[%d] = _o; }"
                     % (ops[1].reg-14, ops[2].reg-14, ops[0].reg-14))
        elif m == "vrev64.32" and len(ops) == 2 and _isd(0) and _isd(1):
            L.append("s->d[%d] = (s->d[%d] >> 32) | (s->d[%d] << 32);" % (ops[0].reg-14, ops[1].reg-14, ops[1].reg-14))
        elif m in ("vmov.f32","vmov.32") and len(ops) == 2 and _isd(0) and _isd(1):
            L.append("s->d[%d] = s->d[%d];" % (ops[0].reg-14, ops[1].reg-14))
        elif m == "vmov" and len(ops) == 3 and _isgpr(0) and _isgpr(1) and _isd(2):
            L.append("{ uint64_t _v = s->d[%d]; %s = (uint32_t)(_v & 0xFFFFFFFFu); %s = (uint32_t)(_v >> 32); }"
                     % (ops[2].reg-14, R(reg(0)), R(reg(1))))
        elif m == "vmov" and len(ops) == 3 and _isd(0) and _isgpr(1) and _isgpr(2):
            L.append("s->d[%d] = (uint64_t)%s | ((uint64_t)%s << 32);" % (ops[0].reg-14, R(reg(1)), R(reg(2))))
        elif m == "vcvt.f64.u32" and len(ops) == 2 and _isd(0) and ops[1].type == 1 and ops[1].reg >= 79:
            L.append("{ double _v = (double)(uint32_t)vfp_s_get(s, %d); memcpy(&s->d[%d], &_v, 8); }" % (ops[1].reg-79, ops[0].reg-14))
        elif m == "vcvt.f64.s32" and len(ops) == 2 and _isd(0) and ops[1].type == 1 and ops[1].reg >= 79:
            L.append("{ double _v = (double)(int32_t)vfp_s_get(s, %d); memcpy(&s->d[%d], &_v, 8); }" % (ops[1].reg-79, ops[0].reg-14))
        elif m == "vcvt.u32.f64" and len(ops) == 2 and ops[0].type == 1 and ops[0].reg >= 79 and _isd(1):
            L.append("{ double _v; memcpy(&_v, &s->d[%d], 8); vfp_s_set(s, %d, (uint32_t)_v); }" % (ops[1].reg-14, ops[0].reg-79))
        elif m == "vcvt.s32.f64" and len(ops) == 2 and ops[0].type == 1 and ops[0].reg >= 79 and _isd(1):
            L.append("{ double _v; memcpy(&_v, &s->d[%d], 8); vfp_s_set(s, %d, (uint32_t)(int32_t)_v); }" % (ops[1].reg-14, ops[0].reg-79))
        elif m == "vmov.32" and len(ops) == 2 and ops[0].type == 1 and _isd(1):
            rr = ops[0].reg
            if 66 <= rr <= 78:
                L.append("s->r[%d] = (uint32_t)(s->d[%d] & 0xFFFFFFFFu);" % (rr-66, ops[1].reg-14))
            elif rr >= 79:
                L.append("vfp_s_set(s, %d, (uint32_t)(s->d[%d] & 0xFFFFFFFFu));" % (rr-79, ops[1].reg-14))
        elif m == "vcgt.f32" and len(ops) == 3 and _isd(0) and _isd(1):
            immv = imm(2) if len(ops) > 2 and ops[2].type in (2, 4) else 0
            L.append("{ uint32_t _a[2]; memcpy(_a, &s->d[%d], 8); uint64_t _o = 0; float _b = (float)%d; for (int _i=0;_i<2;_i++){ float _f; memcpy(&_f, (char*)_a + _i*4, 4); if (_f > _b) { uint32_t _m = 0xFFFFFFFFu; memcpy((char*)&_o + _i*4, &_m, 4); } } s->d[%d] = _o; }"
                     % (ops[1].reg-14, immv, ops[0].reg-14))
        elif m == "vtrn.32" and len(ops) == 2 and _isd(0) and _isd(1):
            L.append("{ uint32_t _a = (uint32_t)(s->d[%d] & 0xFFFFFFFFu), _b = (uint32_t)(s->d[%d] & 0xFFFFFFFFu); s->d[%d] = (s->d[%d] & 0xFFFFFFFF00000000ull) | _b; s->d[%d] = (s->d[%d] & 0xFFFFFFFF00000000ull) | _a; }"
                     % (ops[0].reg-14, ops[1].reg-14, ops[0].reg-14, ops[0].reg-14, ops[1].reg-14, ops[1].reg-14))
        elif m in ("vmul.f64","vadd.f64","vsub.f64","vdiv.f64") and len(ops) == 3 and _isd(0) and _isd(1) and _isd(2):
            opc = {"vmul.f64":"*","vadd.f64":"+","vsub.f64":"-","vdiv.f64":"/"}[m]
            L.append("{ double _a, _b, _o; memcpy(&_a, &s->d[%d], 8); memcpy(&_b, &s->d[%d], 8); _o = _a %s _b; memcpy(&s->d[%d], &_o, 8); }"
                     % (ops[1].reg-14, ops[2].reg-14, opc, ops[0].reg-14))
        elif m in ("vld1.32","vld1.8","vld1.16","vst1.32","vst1.8","vst1.16") and len(ops) >= 2 and _basereg(0) is not None:
            base = R(_basereg(0))
            rr = [o.reg for o in ops[1:] if o.type == ARM_OP_REG and 14 <= o.reg <= 45]
            if rr:
                off = 0
                for q in rr:
                    if m.startswith("vld1"):
                        L.append("s->d[%d] = (uint64_t)rd32(s, %s + 0x%Xu) | ((uint64_t)rd32(s, %s + 0x%Xu) << 32);"
                                 % (q-14, base, off, base, off+4))
                    else:
                        L.append("wr32(s, %s + 0x%Xu, (uint32_t)(s->d[%d] & 0xFFFFFFFFu)); wr32(s, %s + 0x%Xu, (uint32_t)(s->d[%d] >> 32));"
                                 % (base, off, q-14, base, off+4, q-14))
                    off += 8
            else:
                sup.append((ins.address, ins.mnemonic, ins.op_str))
                L.append("/* UNSUPPORTED_NEON %s %s */" % (ins.mnemonic, ins.op_str))
        elif m in ("vld1.32","vld1.8","vld1.16") and len(ops) == 2 and _isd(0) and ops[1].type == 3:
            base = R(ops[1].mem.base)
            L.append("{ uint32_t _v = rd32(s, %s); s->d[%d] = (uint64_t)_v | ((uint64_t)_v << 32); }" % (base, ops[0].reg-14))
        elif m == "vst1.32" and len(ops) == 2 and _isd(0) and ops[1].type == 3:
            base = R(ops[1].mem.base)
            L.append("wr32(s, %s, (uint32_t)(s->d[%d] & 0xFFFFFFFFu));" % (base, ops[0].reg-14))
        elif m in ("vld1.8","vld1.16","vld1.32") and len(ops) == 3 and _isd(0) and _isd(1) and ops[2].type == 3:
            base = R(ops[2].mem.base)
            L.append("s->d[%d] = (uint64_t)rd32(s, %s) | ((uint64_t)rd32(s, %s + 4u) << 32);" % (ops[0].reg-14, base, base))
            L.append("s->d[%d] = (uint64_t)rd32(s, %s + 8u) | ((uint64_t)rd32(s, %s + 12u) << 32);" % (ops[1].reg-14, base, base))
        elif m in ("vcgt.s32","vcgt.u32","vceq.i32","vceq.i16") and len(ops) == 3 and _isd(0) and _isd(1):
            if ops[2].type in (2, 4):
                L.append("{ uint32_t _a[2]; memcpy(_a, &s->d[%d], 8); int32_t _b = (int32_t)%d; uint64_t _o = 0; for (int _i=0;_i<2;_i++){ int32_t _f = ((int32_t*)_a)[_i]; if (_f > _b) { uint32_t _m = 0xFFFFFFFFu; memcpy((char*)&_o + _i*4, &_m, 4); } } s->d[%d] = _o; }"
                         % (ops[1].reg-14, imm(2) if len(ops)>2 else 0, ops[0].reg-14))
            else:
                L.append("{ uint32_t _a[2], _b[2]; memcpy(_a, &s->d[%d], 8); memcpy(_b, &s->d[%d], 8); uint64_t _o = 0; for (int _i=0;_i<2;_i++){ if (((int32_t*)_a)[_i] > ((int32_t*)_b)[_i]) { uint32_t _m = 0xFFFFFFFFu; memcpy((char*)&_o + _i*4, &_m, 4); } } s->d[%d] = _o; }"
                         % (ops[1].reg-14, ops[2].reg-14, ops[0].reg-14))
        elif m in ("vsub.i32","vsub.u32","vadd.i32") and len(ops) == 3 and _isd(0) and _isd(1) and _isd(2):
            opc = "+" if m.startswith("vadd") else "-"
            L.append("{ uint32_t _a[2], _b[2], _o[2]; memcpy(_a, &s->d[%d], 8); memcpy(_b, &s->d[%d], 8); _o[0] = _a[0] %s _b[0]; _o[1] = _a[1] %s _b[1]; uint64_t _r; memcpy(&_r, _o, 8); s->d[%d] = _r; }"
                     % (ops[1].reg-14, ops[2].reg-14, opc, opc, ops[0].reg-14))
        elif m == "vqmovn.s32" and len(ops) == 2 and _isd(0) and 50 <= ops[1].reg <= 65:
            qn = ops[1].reg - 50
            L.append("{ uint32_t _a[4]; memcpy(_a, &s->d[%d], 8); memcpy((char*)_a+8, &s->d[%d], 8); uint64_t _o = 0; for (int _i=0;_i<4;_i++){ int32_t _v = ((int32_t*)_a)[_i]; if (_v > 32767) _v = 32767; if (_v < -32768) _v = -32768; uint16_t _h = (uint16_t)(int16_t)_v; memcpy((char*)&_o + _i*2, &_h, 2); } s->d[%d] = _o; }"
                     % (2*qn, 2*qn+1, ops[0].reg-14))
        elif m == "vcvt.f32.s32" and len(ops) == 2 and ops[0].type == 1 and ops[0].reg >= 79 and ops[1].type == 1 and ops[1].reg >= 79:
            L.append("vfp_sf_set(s, %d, (float)(int32_t)vfp_s_get(s, %d));" % (ops[0].reg-79, ops[1].reg-79))
        elif m == "vmov.f32" and len(ops) == 2 and ops[0].type == 1 and ops[0].reg >= 79 and ops[1].type in (2, 4):
            import re as _re
            _mm = _re.search(r"#([-+]?[0-9]+(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?)", ins.op_str)
            if _mm:
                try:
                    _fv = float(_mm.group(1))
                    L.append("vfp_sf_set(s, %d, %s);" % (ops[0].reg - 79, ("%.9g" % _fv) + ("." if ("." not in ("%.9g" % _fv) and "e" not in ("%.9g" % _fv) and "E" not in ("%.9g" % _fv)) else "") + "f"))
                except Exception:
                    sup.append((ins.address, ins.mnemonic, ins.op_str))
                    L.append("/* UNSUPPORTED_NEON %s %s */" % (ins.mnemonic, ins.op_str))
            else:
                sup.append((ins.address, ins.mnemonic, ins.op_str))
                L.append("/* UNSUPPORTED_NEON %s %s */" % (ins.mnemonic, ins.op_str))
        elif m in ("vmov.f32",) and len(ops) == 2 and ops[0].type == 1 and ops[0].reg >= 79 and ops[1].type == 1 and ops[1].reg >= 79:
            L.append("vfp_s_bits_set(s, %d, vfp_s_get(s, %d));" % (ops[0].reg-79, ops[1].reg-79))
        elif m == "vmla.f64" and len(ops) == 3 and _isd(0) and _isd(1) and _isd(2):
            L.append("{ double _a, _b, _c; memcpy(&_a, &s->d[%d], 8); memcpy(&_b, &s->d[%d], 8); memcpy(&_c, &s->d[%d], 8); _a = _a + _b * _c; memcpy(&s->d[%d], &_a, 8); }"
                     % (ops[0].reg-14, ops[1].reg-14, ops[2].reg-14, ops[0].reg-14))
        elif m == "vrecps.f32" and len(ops) == 3:
            def _QV(regno):
                if 50 <= regno <= 65: return 2*(regno-50)
                if 14 <= regno <= 45: return regno-14
                return None
            d0, d1, d2 = _QV(ops[0].reg), _QV(ops[1].reg), _QV(ops[2].reg)
            if d0 is not None and d1 is not None and d2 is not None:
                L.append("{ float _a[4], _b[4], _o[4]; memcpy(_a, &s->d[%d], 8); memcpy((char*)_a+8, &s->d[%d], 8); memcpy(_b, &s->d[%d], 8); memcpy((char*)_b+8, &s->d[%d], 8); uint64_t _c; memcpy(&_c, &s->d[%d], 8); float _cc[2]; memcpy(_cc, &_c, 8); for (int _i=0;_i<4;_i++){ _o[_i] = 2.0f - _a[_i] * (_i<2?_cc[_i]:0.0f) * _b[_i]; } uint64_t _r0, _r1; memcpy(&_r0, _o, 8); memcpy(&_r1, (char*)_o+8, 8); s->d[%d] = _r0; s->d[%d] = _r1; }"
                         % (d1, d1+1, d2, d2+1, ops[2].reg-14 if 14<=ops[2].reg<=45 else 0, d0, d0+1))
        elif m == "vmov.f32" and len(ops) == 2 and _isd(0) and ops[1].type in (2, 4):
            import re as _re
            _mm = _re.search(r"#([-+]?[0-9]+(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?)", ins.op_str)
            if _mm:
                _fv = float(_mm.group(1))
                L.append("{ float _f = %s; uint32_t _b; memcpy(&_b, &_f, 4); s->d[%d] = _b | ((uint64_t)_b << 32); }" % (("%.9g" % _fv) + ("." if ("." not in ("%.9g" % _fv) and "e" not in ("%.9g" % _fv) and "E" not in ("%.9g" % _fv)) else "") + "f", ops[0].reg-14))
            else:
                sup.append((ins.address, ins.mnemonic, ins.op_str)); L.append("/* UNSUPPORTED_NEON %s %s */" % (ins.mnemonic, ins.op_str))
        elif m == "vmov.f64" and len(ops) == 2 and _isd(0) and ops[1].type in (2, 4):
            import re as _re
            _mm = _re.search(r"#([-+]?[0-9]+(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?)", ins.op_str)
            if _mm:
                _dv = float(_mm.group(1))
                L.append("{ double _v = %s; memcpy(&s->d[%d], &_v, 8); }" % (("%.17g" % _dv) + ("." if ("." not in ("%.17g" % _dv) and "e" not in ("%.17g" % _dv) and "E" not in ("%.17g" % _dv)) else ""), ops[0].reg-14))
            else:
                sup.append((ins.address, ins.mnemonic, ins.op_str)); L.append("/* UNSUPPORTED_NEON %s %s */" % (ins.mnemonic, ins.op_str))
        elif m in ("vst1.32","vst1.16","vst1.8") and len(ops) == 3 and _isd(0) and _isd(1) and ops[2].type == 3:
            base = R(ops[2].mem.base)
            L.append("wr32(s, %s, (uint32_t)(s->d[%d] & 0xFFFFFFFFu)); wr32(s, %s + 4u, (uint32_t)(s->d[%d] >> 32));" % (base, ops[0].reg-14, base, ops[0].reg-14))
            L.append("wr32(s, %s + 8u, (uint32_t)(s->d[%d] & 0xFFFFFFFFu)); wr32(s, %s + 12u, (uint32_t)(s->d[%d] >> 32));" % (base, ops[1].reg-14, base, ops[1].reg-14))
        elif m in ("vst1.16",) and len(ops) == 2 and _isd(0) and ops[1].type == 3:
            base = R(ops[1].mem.base)
            L.append("{ uint32_t _a[2]; memcpy(_a, &s->d[%d], 8); for (int _i=0;_i<4;_i++) wr32(s, %s + _i*2u, (_a[_i>>1] >> ((_i&1)*16)) & 0xFFFFu); }" % (ops[0].reg-14, base))
        elif m == "vst2.16" and len(ops) == 3 and _isd(0) and _isd(1) and ops[2].type == 3:
            base = R(ops[2].mem.base)
            L.append("{ uint32_t _a[2], _b[2]; memcpy(_a, &s->d[%d], 8); memcpy(_b, &s->d[%d], 8); for (int _i=0;_i<4;_i++){ wr32(s, %s + _i*2u, (_a[_i>>1] >> ((_i&1)*16)) & 0xFFFFu); wr32(s, %s + (_i*2u+1u), (_b[_i>>1] >> ((_i&1)*16)) & 0xFFFFu); } }" % (ops[0].reg-14, ops[1].reg-14, base, base))
        elif m == "vcvt.f32.s32" and len(ops) == 2 and _isd(0) and _isd(1):
            L.append("{ uint32_t _a[2]; memcpy(_a, &s->d[%d], 8); float _f0 = (float)(int32_t)_a[0], _f1 = (float)(int32_t)_a[1]; uint32_t _b0, _b1; memcpy(&_b0, &_f0, 4); memcpy(&_b1, &_f1, 4); s->d[%d] = (uint64_t)_b0 | ((uint64_t)_b1 << 32); }" % (ops[1].reg-14, ops[0].reg-14))
        elif m == "vcvt.s32.f32" and len(ops) == 2 and _isd(0) and _isd(1):
            L.append("{ uint32_t _a[2]; memcpy(_a, &s->d[%d], 8); int32_t _i0; float _f0, _f1; memcpy(&_f0, &_a[0], 4); memcpy(&_f1, &_a[1], 4); _i0 = (int32_t)_f0; int32_t _i1 = (int32_t)_f1; s->d[%d] = (uint64_t)(uint32_t)_i0 | ((uint64_t)(uint32_t)_i1 << 32); }" % (ops[1].reg-14, ops[0].reg-14))
        elif m in ("vclt.f32","vcle.f32","vcge.f32","vcgt.f32","vceq.f32","vclt.s32","vcle.s32","vcge.s32","vcgt.s32","vceq.i32") and len(ops) == 3 and _isd(0) and _isd(1):
            opname = m.split(".")[0]
            if ops[2].type in (2, 4):
                L.append("{ uint32_t _a[2]; memcpy(_a, &s->d[%d], 8); uint64_t _o = 0; for (int _i=0;_i<2;_i++){ int _ok; { float _fa; float _fb; memcpy(&_fa, (char*)_a + _i*4, 4); _fb = (float)%d; _ok = (_fa %s _fb); } if (_ok) { uint32_t _m = 0xFFFFFFFFu; memcpy((char*)&_o + _i*4, &_m, 4); } } s->d[%d] = _o; }"
                         % (ops[1].reg-14, imm(2) if len(ops)>2 else 0, {"vclt":"<","vcle":"<=","vcge":">=","vcgt":">","vceq":"=="}.get(opname,"=="), ops[0].reg-14))
            else:
                L.append("{ uint32_t _a[2], _b[2]; memcpy(_a, &s->d[%d], 8); memcpy(_b, &s->d[%d], 8); uint64_t _o = 0; for (int _i=0;_i<2;_i++){ float _fa, _fb; memcpy(&_fa, (char*)_a + _i*4, 4); memcpy(&_fb, (char*)_b + _i*4, 4); if (_fa %s _fb) { uint32_t _m = 0xFFFFFFFFu; memcpy((char*)&_o + _i*4, &_m, 4); } } s->d[%d] = _o; }"
                         % (ops[1].reg-14, ops[2].reg-14, {"vclt":"<","vcle":"<=","vcge":">=","vcgt":">","vceq":"=="}.get(opname,"=="), ops[0].reg-14))
        elif m == "vmovn.i64" and len(ops) == 2 and _isd(0) and 50 <= ops[1].reg <= 65:
            qn = ops[1].reg - 50
            L.append("s->d[%d] = (uint64_t)(uint32_t)(s->d[%d] & 0xFFFFFFFFu) | ((uint64_t)(uint32_t)(s->d[%d] & 0xFFFFFFFFu) << 32);" % (ops[0].reg-14, 2*qn, 2*qn+1))
        elif m == "vrecpe.f32" and len(ops) == 2 and _isd(0) and _isd(1):
            L.append("{ uint32_t _a[2]; memcpy(_a, &s->d[%d], 8); float _f0, _f1; memcpy(&_f0, &_a[0], 4); memcpy(&_f1, &_a[1], 4); _f0 = 1.0f / _f0; _f1 = 1.0f / _f1; uint32_t _b0, _b1; memcpy(&_b0, &_f0, 4); memcpy(&_b1, &_f1, 4); s->d[%d] = (uint64_t)_b0 | ((uint64_t)_b1 << 32); }" % (ops[1].reg-14, ops[0].reg-14))
        elif m in ("vneg.f32",) and len(ops) == 2 and _isd(0) and _isd(1):
            L.append("{ uint64_t _a = s->d[%d]; _a ^= 0x8000000080000000ull; s->d[%d] = _a; }" % (ops[1].reg-14, ops[0].reg-14))
        elif m == "vbic" and len(ops) == 3 and _isd(0) and _isd(1) and _isd(2):
            L.append("s->d[%d] = s->d[%d] & ~s->d[%d];" % (ops[0].reg-14, ops[1].reg-14, ops[2].reg-14))
        elif m == "vmvn.i32" and len(ops) == 2 and _isd(0) and ops[1].type in (2, 4):
            v = imm(1) & 0xFFFFFFFF
            L.append("s->d[%d] = (uint64_t)(~0x%Xu) | ((uint64_t)(~0x%Xu) << 32);" % (ops[0].reg-14, v, v))
        elif m == "vbif" and len(ops) == 3 and _isd(0) and _isd(1) and _isd(2):
            L.append("{ uint64_t _d = s->d[%d], _n = s->d[%d], _m = s->d[%d]; s->d[%d] = (_n & _d) | (~_n & _m); }"
                     % (ops[0].reg-14, ops[1].reg-14, ops[2].reg-14, ops[0].reg-14))
        elif m in ("vldmia","vstmia","vldr","vstr","vpush","vpop"):
            sup.append((ins.address, ins.mnemonic, ins.op_str))
            L.append("/* UNSUPPORTED_NEON %s %s */" % (ins.mnemonic, ins.op_str))
        else:
            sup.append((ins.address, ins.mnemonic, ins.op_str))
            L.append("/* UNSUPPORTED_NEON %s %s */" % (ins.mnemonic, ins.op_str))
        return L
    # --- VFP ---
    if m.startswith("vmov") and len(ops) >= 2:
        if isi(1):
            if ops[1].type == 4:
                # M1300 FIX: VFP floating-point immediate operand (type ARM_OP_FP) carries .fp;
                # capstone's .imm is 0 for these, which generated every vmov.f32 sN,#imm as 0.0f
                # and broke ALL float constants passed as function arguments (e.g. sprite scale 1.0).
                import struct as _struct
                _fv = float(getattr(ops[1], "fp", 0.0))
                _bits = _struct.unpack("<I", _struct.pack("<f", _fv))[0]
                if reg(0) >= 79: L.append("vfp_s_bits_set(s, %d, 0x%Xu);" % (reg(0)-79, _bits))
                else: L.append("%s = 0x%Xu;" % (R(reg(0)), _bits))
            elif reg(0) >= 79: L.append("vfp_s_bits_set(s, %d, f32_bits((float)%.9g));" % (reg(0)-79, float(ops[1].imm)))
            else: L.append("%s = f32_bits((float)%.9g);" % (R(reg(0)), float(ops[1].imm)))
        elif isr(1):
            if reg(0) >= 79:
                L.append("vfp_s_set(s, %d, %s);" % (reg(0)-79, R(reg(1)) if reg(1) < 79 else "vfp_s_get(s,%d)" % (reg(1)-79)))
            elif reg(1) >= 79:
                L.append("%s = vfp_s_get(s, %d);" % (R(reg(0)), reg(1)-79))
            elif 14 <= reg(0) <= 45 and 14 <= reg(1) <= 45:
                L.append("vfp_d_set(s, %d, vfp_d_get(s, %d));" % (reg(0)-14, reg(1)-14))
            elif 14 <= reg(0) <= 45:
                L.append("s->d[%d] = (s->d[%d] & 0xFFFFFFFF00000000ull) | (uint64_t)%s;" % (reg(0)-14, reg(0)-14, R(reg(1))))
            elif 14 <= reg(1) <= 45:
                L.append("%s = (uint32_t)(s->d[%d] & 0xFFFFFFFFu);" % (R(reg(0)), reg(1)-14))
            else:
                L.append("%s = %s;" % (R(reg(0)), R(reg(1))))
    elif m in ("vldr",) and len(ops) >= 2 and ops[1].type == ARM_OP_MEM:
        base, disp = mem(1)
        if reg(0) >= 79:
            L.append("vfp_s_bits_set(s, %d, rd32(s, %s + 0x%Xu));" % (reg(0)-79, base, disp))
        elif 14 <= reg(0) <= 45:
            L.append("s->d[%d] = rd64(s, %s + 0x%Xu);" % (reg(0)-14, base, disp))
        else:
            L.append("%s = rd32(s, %s + 0x%Xu);" % (R(reg(0)), base, disp))
    elif m in ("vstr",) and len(ops) >= 2 and ops[1].type == ARM_OP_MEM:
        base, disp = mem(1)
        if reg(0) >= 79:
            L.append("wr32(s, %s + 0x%Xu, vfp_s_bits(s, %d));" % (base, disp, reg(0)-79))
        elif 14 <= reg(0) <= 45:
            L.append("wr64(s, %s + 0x%Xu, s->d[%d]);" % (base, disp, reg(0)-14))
        else:
            L.append("wr32(s, %s + 0x%Xu, %s);" % (base, disp, R(reg(0))))
    elif m in ("vadd.f32","vsub.f32","vmul.f32","vdiv.f32") and len(ops) >= 3:
        op2 = {"vadd.f32":"+","vsub.f32":"-","vmul.f32":"*","vdiv.f32":"/"}[m]
        L.append("vfp_sf_set(s, %d, vfp_sf(s,%d) %s vfp_sf(s,%d));" % (reg(0)-79, reg(1)-79, op2, reg(2)-79))
    elif m in ("vmla.f32","vmls.f32","vnmul.f32") and len(ops) >= 3:
        if m == "vmla.f32": L.append("vfp_sf_set(s,%d, vfp_sf(s,%d) + vfp_sf(s,%d)*vfp_sf(s,%d));" % (reg(0)-79, reg(0)-79, reg(1)-79, reg(2)-79))
        elif m == "vmls.f32": L.append("vfp_sf_set(s,%d, vfp_sf(s,%d) - vfp_sf(s,%d)*vfp_sf(s,%d));" % (reg(0)-79, reg(0)-79, reg(1)-79, reg(2)-79))
        else: L.append("vfp_sf_set(s,%d, -(vfp_sf(s,%d)*vfp_sf(s,%d)));" % (reg(0)-79, reg(1)-79, reg(2)-79))
    elif m in ("vcvt.f32.u32","vcvt.f32.s32") and len(ops) >= 2:
        if m.endswith("u32"): L.append("vfp_sf_set(s,%d,(float)vfp_s_bits(s,%d));" % (reg(0)-79, reg(1)-79))
        else: L.append("vfp_sf_set(s,%d,(float)(int32_t)vfp_s_bits(s,%d));" % (reg(0)-79, reg(1)-79))
    elif m in ("vcvt.u32.f32","vcvt.s32.f32") and len(ops) >= 2:
        if m.startswith("vcvt.u32"): L.append("vfp_s_bits_set(s,%d,(uint32_t)vfp_sf(s,%d));" % (reg(0)-79, reg(1)-79))
        else: L.append("vfp_s_bits_set(s,%d,(uint32_t)(int32_t)vfp_sf(s,%d));" % (reg(0)-79, reg(1)-79))
    elif m == "vcmp.f32" and len(ops) >= 2:
        L.append("{ float _a=vfp_sf(s,%d), _b=vfp_sf(s,%d); s->apsr=s->fp_nzcv=(_a==_b)?0x40000000u:((_a<_b)?0x80000000u:0x20000000u); }" % (reg(0)-79, reg(1)-79))
    elif m == "vmrs":
        L.append("s->apsr = s->fp_nzcv; /* vmrs APSR_nzcv,FPSCR */")
    elif m in ("vpush","vpop"):
        regs = [op.reg for op in ops if op.type == ARM_OP_REG]
        def vbits(r):
            if r >= 79: return "vfp_s_bits(s, %d)" % (r-79)
            return R(r)
        if m == "vpush":
            for r in sorted(regs, key=RIDX, reverse=True):
                L.append("s->r[13] -= 4; wr32(s, s->r[13], %s);" % vbits(r))
        else:
            for r in sorted(regs, key=RIDX):
                if r >= 79: L.append("vfp_s_bits_set(s, %d, rd32(s, s->r[13])); s->r[13] += 4;" % (r-79))
                else: L.append("%s = rd32(s, s->r[13]); s->r[13] += 4;" % R(r))
    # --- integer ALU with normalization ---
    elif m in ("movs","mov","movw","movt"):
        if isi(1) and m == "movw": L.append("%s = 0x%Xu;" % (R(reg(0)), imm(1) & 0xFFFF))
        elif isi(1) and m == "movt": L.append("%s = ((%s & 0xFFFF) | (0x%Xu << 16));" % (R(reg(0)), R(reg(0)), imm(1) & 0xFFFF))
        elif isi(1):
            L.append("%s = 0x%Xu;" % (R(reg(0)), imm(1)))
            if m == "movs": L.append("setf_nz(s, %s);" % R(reg(0)))
        elif isr(1):
            L.append("%s = %s;" % (R(reg(0)), R(reg(1))))
            if m == "movs": L.append("setf_nz(s, %s);" % R(reg(0)))
    elif m in ("rsbs","rsb"):
        # 18.4131: RSB Rd,Rn,op2 = op2 - Rn (reverse subtract); the add/sub branch emitted +Rn+op2
        _S = (m == "rsbs")
        def rflag():
            return ["setf_sub(s, _a, _b, _r);"] if _S else []
        if isr(1) and isi(2):
            L.append("{ uint32_t _a = 0x%Xu; uint32_t _b = %s; uint32_t _r = _a - _b; %s = _r; %s }" % (imm(2), R(reg(1)), R(reg(0)), " ".join(rflag())))
        elif isr(1) and isr(2):
            L.append("{ uint32_t _a = %s; uint32_t _b = %s; uint32_t _r = _a - _b; %s = _r; %s }" % (_shreg(2), R(reg(1)), R(reg(0)), " ".join(rflag())))
        elif len(ops) >= 3 and ops[2].type == 2:
            L.append("{ uint32_t _a = 0x%Xu; uint32_t _b = %s; uint32_t _r = _a - _b; %s = _r; %s }" % (imm(2), R(reg(1)), R(reg(0)), " ".join(rflag())))
        elif len(ops) == 2 and ops[1].type == 2:
            L.append("{ uint32_t _a = 0x%Xu; uint32_t _b = %s; uint32_t _r = _a - _b; %s = _r; %s }" % (imm(1), R(reg(0)), R(reg(0)), " ".join(rflag())))
        elif len(ops) == 2 and isr(0) and isr(1):
            L.append("{ uint32_t _a = %s; uint32_t _b = %s; uint32_t _r = _a - _b; %s = _r; %s }" % (_shreg(1), R(reg(0)), R(reg(0)), " ".join(rflag())))
    elif m in ("adds","add","addw","subs","sub","subw"):
        neg = m in ("subs","sub","subw")
        _S = m in ("adds","subs")   # flag-setting variants
        def flagcall():
            if not _S: return []
            return ["setf_add(s, _a, _b, _r);" if not neg else "setf_sub(s, _a, _b, _r);"]
        if isi(1) and len(ops) == 2:
            L.append("{ uint32_t _a = %s; uint32_t _b = 0x%Xu; uint32_t _r = _a %s _b; %s = _r; %s }"
                     % (R(reg(0)), imm(1), "-" if neg else "+", R(reg(0)), " ".join(flagcall())))
        elif isr(1) and isi(2):
            L.append("{ uint32_t _a = %s; uint32_t _b = 0x%Xu; uint32_t _r = _a %s _b; %s = _r; %s }"
                     % (R(reg(1)), imm(2), "-" if neg else "+", R(reg(0)), " ".join(flagcall())))
        elif isr(1) and isr(2):
            L.append("{ uint32_t _a = %s; uint32_t _b = %s; uint32_t _r = _a %s _b; %s = _r; %s }"
                     % (R(reg(1)), _shreg(2), "-" if neg else "+", R(reg(0)), " ".join(flagcall())))
        elif len(ops) >= 3 and ops[2].type == 2:
            L.append("{ uint32_t _a = %s; uint32_t _b = 0x%Xu; uint32_t _r = _a %s _b; %s = _r; %s }"
                     % (R(reg(1)), imm(2), "-" if neg else "+", R(reg(0)), " ".join(flagcall())))
        elif len(ops) == 2 and ops[1].type == 2:
            L.append("{ uint32_t _a = 0x%Xu; uint32_t _b = %s; uint32_t _r = _a %s _b; %s = _r; %s }"
                     % (imm(1), R(reg(0)), "-" if neg else "+", R(reg(0)), " ".join(flagcall())))
        elif len(ops) == 2 and isr(0) and isr(1):
            # 2-operand high-register form: add rd, rm  ->  rd = rd + rm
            L.append("{ uint32_t _a = %s; uint32_t _b = %s; uint32_t _r = _a %s _b; %s = _r; %s }"
                     % (R(reg(0)), _shreg(1), "-" if neg else "+", R(reg(0)), " ".join(flagcall())))
    elif m in ("adc","adcs","sbc","sbcs"):
        is_adc = m.startswith("adc"); _S = m in ("adcs","sbcs")   # 18.4224: only S variants update flags
        C = "(uint32_t)((s->apsr >> 29) & 1u)"
        if len(ops) == 3 and isr(1):
            _a = R(reg(1)); b = ("0x%Xu" % imm(2)) if isi(2) else R(reg(2))   # 3-register form uses Rn
        elif len(ops) >= 3 and isi(2):
            _a = R(reg(1)); b = "0x%Xu" % imm(2)
        elif len(ops) == 2 and isi(1):
            _a = R(reg(0)); b = "0x%Xu" % imm(1)
        elif len(ops) == 2 and isr(1):
            _a = R(reg(0)); b = R(reg(1))
        else:
            _a = R(reg(0)); b = "0u"
        if is_adc:
            if _S:
                L.append("{ uint32_t _c=%s; uint32_t _a=%s; uint32_t _b=%s; uint64_t _t=(uint64_t)_a+(uint64_t)_b+(uint64_t)_c; uint32_t _r=(uint32_t)_t; uint32_t _b2=_b+_c; %s=_r; s->apsr=(s->apsr & 0x0FFFFFFFu) | ((_r>>31)<<31) | ((_r==0u)?0x40000000u:0u) | (((uint32_t)(_t>>32)&1u)<<29) | ((((_a ^ _r) & (_b2 ^ _r))>>31)<<28); }" % (C, _a, b, R(reg(0))))
            else:
                L.append("{ uint32_t _c=%s; uint32_t _a=%s; uint32_t _b=%s; uint32_t _r=(uint32_t)((uint64_t)_a+(uint64_t)_b+(uint64_t)_c); %s=_r; }" % (C, _a, b, R(reg(0))))
        else:
            if _S:
                L.append("{ uint32_t _c=%s; uint32_t _a=%s; uint32_t _b=%s; uint32_t _b2=_b+(1u-_c); uint64_t _t=(uint64_t)_a-(uint64_t)_b2; uint32_t _r=(uint32_t)_t; %s=_r; s->apsr=(s->apsr & 0x0FFFFFFFu) | ((_r>>31)<<31) | ((_r==0u)?0x40000000u:0u) | (((_a>=_b2)?1u:0u)<<29) | ((((_a ^ _b2) & (_a ^ _r))>>31)<<28); }" % (C, _a, b, R(reg(0))))
            else:
                L.append("{ uint32_t _c=%s; uint32_t _a=%s; uint32_t _b=%s; uint32_t _r=(uint32_t)((uint64_t)_a-(uint64_t)(_b+(1u-_c))); %s=_r; }" % (C, _a, b, R(reg(0))))
    elif m == "adr":
        L.append("%s = 0x%Xu;" % (R(reg(0)), (((ins.address + 4) & 0xFFFFFFFC) + imm(1)) & 0xFFFFFFFF))
    elif m in ("rev","rev32","rev16"):
        src = R(reg(1)) if len(ops) > 1 else R(reg(0))
        if m == "rev16":
            L.append("%s = ((%s >> 8) & 0x00FF00FFu) | ((%s & 0x00FF00FFu) << 8);" % (R(reg(0)), src, src))
        else:
            L.append("%s = (((%s) >> 24) & 0xFFu) | (((%s) >> 8) & 0xFF00u) | (((%s) & 0xFF00u) << 8) | (((%s) & 0xFFu) << 24);"
                     % (R(reg(0)), src, src, src, src))
    elif m in ("ands","and","orrs","orr","eors","eor","bics","bic"):
        op2 = {"ands":"&","and":"&","orrs":"|","orr":"|","eors":"^","eor":"^","bics":"&~","bic":"&~"}[m]
        _F = m in ("ands","orrs","eors","bics")
        def _fl():
            return ["setf_logic(s, _r);"] if _F else []
        if len(ops) >= 3 and isi(2):
            L.append("{ uint32_t _r = %s %s 0x%Xu; %s = _r; %s }" % (R(reg(1)), op2, imm(2), R(reg(0)), " ".join(_fl())))
        elif len(ops) >= 3 and isr(2):
            L.append("{ uint32_t _r = %s %s %s; %s = _r; %s }" % (R(reg(1)), op2, _shreg(2), R(reg(0)), " ".join(_fl())))
        elif len(ops) == 2 and isi(1):
            L.append("{ uint32_t _r = %s %s 0x%Xu; %s = _r; %s }" % (R(reg(0)), op2, imm(1), R(reg(0)), " ".join(_fl())))
        elif len(ops) == 2 and isr(1):
            L.append("{ uint32_t _r = %s %s %s; %s = _r; %s }" % (R(reg(0)), op2, _shreg(1), R(reg(0)), " ".join(_fl())))
    elif m == "ldrex":
        # ldrex Rt, [Rn] : ops[1] is a MEMORY operand (was silently dropped -> spin loops on strex retry)
        if isr(0) and len(ops) >= 2 and ops[1].type == ARM_OP_MEM:
            L.append("%s = rd32(s, %s);" % (R(reg(0)), R(ops[1].mem.base)))
        elif isr(0) and isr(1):
            L.append("%s = rd32(s, %s);" % (R(reg(0)), R(reg(1))))
    elif m == "strex":
        # strex Rd, Rt, [Rn] : ops[2] is a MEMORY operand (was silently dropped -> r3 stayed set forever)
        if len(ops) >= 3 and isr(0) and isr(1) and ops[2].type == ARM_OP_MEM:
            L.append("wr32(s, %s, %s); %s = 0;" % (R(ops[2].mem.base), R(reg(1)), R(reg(0))))
        elif isr(0) and isr(1) and isr(2):
            L.append("wr32(s, %s, %s); %s = 0;" % (R(reg(2)), R(reg(1)), R(reg(0))))
    elif m == "clz":
        if isr(0) and isr(1):
            L.append("%s = (uint32_t)__builtin_clz(%s | 1u);" % (R(reg(0)), R(reg(1))))
    elif m == "cmn":
        if isi(1):
            L.append("{ uint32_t _a = %s; uint32_t _b = 0x%Xu; uint32_t _r = _a + _b; setf_add(s, _a, _b, _r); }" % (R(reg(0)), imm(1)))
        else:
            L.append("{ uint32_t _a = %s; uint32_t _b = %s; uint32_t _r = _a + _b; setf_add(s, _a, _b, _r); }" % (R(reg(0)), _shreg(1)))
    elif m == "mcrr":
        L.append("/* mcrr ignored */")
    # [SFLOAT] VFP scalar (S-register 79+) forms of vsqrt/vnmla/vnmls
    elif m in ("vsqrt.f32","vnmla.f32","vnmls.f32") and len(ops) >= 2 and all(op.type == 1 and op.reg >= 79 for op in ops[:3] if op is not None):
        if m == "vsqrt.f32":
            L.append("vfp_sf_set(s, %d, __builtin_sqrtf(vfp_sf(s, %d)));" % (ops[0].reg - 79, ops[1].reg - 79))
        elif m == "vnmla.f32":
            L.append("vfp_sf_set(s, %d, -(vfp_sf(s, %d)) + vfp_sf(s, %d) * vfp_sf(s, %d));"
                     % (ops[0].reg - 79, ops[0].reg - 79, ops[1].reg - 79, ops[2].reg - 79))
        else:
            L.append("vfp_sf_set(s, %d, -(vfp_sf(s, %d) + vfp_sf(s, %d) * vfp_sf(s, %d)));"
                     % (ops[0].reg - 79, ops[0].reg - 79, ops[1].reg - 79, ops[2].reg - 79))
    elif m in ("cmp","cmpw"):
        if isi(1): L.append("{ uint32_t _a = %s; uint32_t _b = 0x%Xu; uint32_t _r = _a - _b; setf_sub(s, _a, _b, _r); }" % (R(reg(0)), imm(1)))
        else: L.append("{ uint32_t _a = %s; uint32_t _b = %s; uint32_t _r = _a - _b; setf_sub(s, _a, _b, _r); }" % (R(reg(0)), _shreg(1)))
    elif m in ("tst",):
        if isi(1): L.append("{ uint32_t _r = %s & 0x%Xu; setf_logic(s, _r); }" % (R(reg(0)), imm(1)))
        else: L.append("{ uint32_t _r = %s & %s; setf_logic(s, _r); }" % (R(reg(0)), _shreg(1)))
    elif m in ("lsls","lsl","lsls.w","lsrs","lsr","lsrs.w","asrs","asr","asr.w","rors","ror"):
        if len(ops) >= 3 and isi(2):
            _i2 = imm(2)
            sh = _i2 & 0x1F
            if m.startswith("lsl"): L.append("%s = %s << 0x%Xu;" % (R(reg(0)), R(reg(1)), sh))
            elif m.startswith("lsr"): L.append("%s = %s;" % (R(reg(0)), "0u" if _i2 == 0 else ("%s >> 0x%Xu" % (R(reg(1)), sh))))
            elif m.startswith("asr"): L.append("%s = %s;" % (R(reg(0)), ("(%s & 0x80000000u) ? 0xFFFFFFFFu : 0u" % R(reg(1))) if _i2 == 0 else ("(uint32_t)((int32_t)%s >> 0x%Xu)" % (R(reg(1)), sh))))
            else: L.append("%s = ((%s >> %d) | (%s << (32-%d)));" % (R(reg(0)), R(reg(1)), sh, R(reg(1)), sh))
            if m.endswith("s") or m.endswith("s.w"): L.append("setf_nz(s, %s);" % R(reg(0)))
        elif len(ops) >= 3 and isr(2):
            if m.startswith("lsl"): L.append("{ uint32_t _sh = %s; %s = (_sh >= 32u) ? 0u : (%s << _sh); }" % (R(reg(2)), R(reg(0)), R(reg(1))))
            elif m.startswith("lsr"): L.append("{ uint32_t _sh = %s; %s = (_sh >= 32u) ? 0u : (%s >> _sh); }" % (R(reg(2)), R(reg(0)), R(reg(1))))
            elif m.startswith("asr"): L.append("{ uint32_t _sh = %s; %s = (_sh >= 32u) ? ((%s & 0x80000000u) ? 0xFFFFFFFFu : 0u) : (uint32_t)((int32_t)%s >> _sh); }" % (R(reg(2)), R(reg(0)), R(reg(1)), R(reg(1))))
            else: L.append("{ uint32_t _sh = %s & 0x1Fu; %s = _sh ? ((%s >> _sh) | (%s << (32u - _sh))) : %s; }" % (R(reg(2)), R(reg(0)), R(reg(1)), R(reg(1)), R(reg(1))))
            if m.endswith("s") or m.endswith("s.w"): L.append("setf_nz(s, %s);" % R(reg(0)))   # 18.4190b: 3-reg shift must set flags for the following bne
        elif len(ops) == 2 and isr(1):
            # 18.4146: two-operand register-shift form (Thumb LSL/LSR/ASR/ROR reg) - 18.4129 fix did not cover it,
            # which silently DROPPED instructions like "lsrs r0, r1" (dest is also the source, shift in Rs).
            if m.startswith("lsl"): L.append("{ uint32_t _sh = %s; %s = (_sh >= 32u) ? 0u : (%s << _sh); }" % (R(reg(1)), R(reg(0)), R(reg(0))))
            elif m.startswith("lsr"): L.append("{ uint32_t _sh = %s; %s = (_sh >= 32u) ? 0u : (%s >> _sh); }" % (R(reg(1)), R(reg(0)), R(reg(0))))
            elif m.startswith("asr"): L.append("{ uint32_t _sh = %s; %s = (_sh >= 32u) ? ((%s & 0x80000000u) ? 0xFFFFFFFFu : 0u) : (uint32_t)((int32_t)%s >> _sh); }" % (R(reg(1)), R(reg(0)), R(reg(0)), R(reg(0))))
            else: L.append("{ uint32_t _sh = %s & 0x1Fu; %s = _sh ? ((%s >> _sh) | (%s << (32u - _sh))) : %s; }" % (R(reg(1)), R(reg(0)), R(reg(0)), R(reg(0)), R(reg(0))))
            if m.endswith("s") or m.endswith("s.w"): L.append("setf_nz(s, %s);" % R(reg(0)))
        else:
            sup.append((ins.address, ins.mnemonic, ins.op_str)); L.append("/* UNSUPPORTED_SHIFT %s %s */" % (ins.mnemonic, ins.op_str))
    elif m in ("ubfx","sbfx") and len(ops) >= 4:
        lsb = imm(2); w = imm(3); mask = ((1 << w) - 1) if w < 32 else 0xFFFFFFFF
        if m == "ubfx": L.append("%s = (%s >> %d) & 0x%Xu;" % (R(reg(0)), R(reg(1)), lsb, mask))
        else: L.append("%s = (uint32_t)(((int32_t)(%s << (32-%d-%d))) >> (32-%d));" % (R(reg(0)), R(reg(1)), lsb, w, w))
    elif m == "bfi" and len(ops) >= 4:
        lsb = imm(2); w = imm(3); mask = ((1 << w) - 1) << lsb
        L.append("%s = (%s & ~0x%Xu) | ((%s << %d) & 0x%Xu);" % (R(reg(0)), R(reg(0)), mask, R(reg(1)), lsb, mask))
    elif m == "bfc" and len(ops) >= 3:
        lsb = imm(1); w = imm(2); mask = ((1 << w) - 1) << lsb
        L.append("%s = %s & ~0x%Xu;" % (R(reg(0)), R(reg(0)), mask))
    elif m in ("mul","muls"):
        if len(ops) >= 3: L.append("%s = %s * %s;" % (R(reg(0)), R(reg(1)), R(reg(2))))
        elif len(ops) == 2: L.append("%s = %s * %s;" % (R(reg(0)), R(reg(0)), R(reg(1))))
        else: L.append("%s = %s * %s;" % (R(reg(0)), R(reg(0)), R(reg(0))))
    elif m == "mla" and len(ops) >= 4:
        L.append("%s = %s * %s + %s;" % (R(reg(0)), R(reg(1)), R(reg(2)), R(reg(3))))
    elif m == "mls" and len(ops) >= 4:
        L.append("%s = %s - %s * %s;" % (R(reg(0)), R(reg(3)), R(reg(1)), R(reg(2))))
    elif m in ("smull","umull") and len(ops) >= 4:
        L.append("{ int64_t _p = (int64_t)%s * (int64_t)%s; %s = (uint32_t)_p; %s = (uint32_t)(_p>>32); }" % (R(reg(2)), R(reg(3)), R(reg(0)), R(reg(1))))
    elif m in ("sxtb","sxth","uxtb","uxth"):
        w = 8 if m.endswith("b") else 16
        if m.startswith("s"): L.append("%s = (uint32_t)(int32_t)(int%d_t)(%s & 0x%Xu);" % (R(reg(0)), w, R(reg(1)), (1 << w) - 1))
        else: L.append("%s = %s & 0x%Xu;" % (R(reg(0)), R(reg(1)), (1 << w) - 1))
    elif m in ("ldr","ldrb","ldrh","ldrsh","ldrsb","ldrd"):
        if m == "ldrd" and len(ops) >= 3 and ops[2].type == ARM_OP_MEM:
            base, disp = mem(2)
            L.append("%s = rd32(s, %s + 0x%Xu);" % (R(reg(0)), base, disp))
            L.append("%s = rd32(s, %s + 0x%Xu);" % (R(reg(1)), base, (disp + 4) & 0xFFFFFFFF))
            L.extend(_wb(2))
        elif len(ops) >= 2 and ops[1].type == ARM_OP_MEM:
            base, disp = mem(1)
            if m == "ldr": L.append("%s = rd32(s, %s + 0x%Xu);" % (R(reg(0)), base, disp))
            elif m == "ldrb": L.append("%s = (uint32_t)P(s, %s + 0x%Xu)[0];" % (R(reg(0)), base, disp))
            elif m == "ldrh": L.append("%s = rd16(s, %s + 0x%Xu);" % (R(reg(0)), base, disp))
            elif m == "ldrsh": L.append("%s = (uint32_t)(int32_t)(int16_t)rd16(s, %s + 0x%Xu);" % (R(reg(0)), base, disp))
            elif m == "ldrsb": L.append("%s = (uint32_t)(int32_t)(int8_t)P(s, %s + 0x%Xu)[0];" % (R(reg(0)), base, disp))
            L.extend(_wb(1))

    elif m in ("str","strb","strh","strd"):
        if m == "strd" and len(ops) >= 3 and ops[2].type == ARM_OP_MEM:
            base, disp = mem(2)
            L.append("wr32(s, %s + 0x%Xu, %s);" % (base, disp, R(reg(0))))
            L.append("wr32(s, %s + 0x%Xu, %s);" % (base, (disp + 4) & 0xFFFFFFFF, R(reg(1))))
            L.extend(_wb(2))
        elif len(ops) >= 2 and ops[1].type == ARM_OP_MEM:
            base, disp = mem(1)
            if m == "str": L.append("wr32(s, %s + 0x%Xu, %s);" % (base, disp, R(reg(0))))
            elif m == "strb": L.append("P(s, %s + 0x%Xu)[0] = (uint8_t)%s;" % (base, disp, R(reg(0))))
            elif m == "strh": L.append("wr16(s, %s + 0x%Xu, (uint16_t)%s);" % (base, disp, R(reg(0))))
            L.extend(_wb(1))

    elif m in ("push","pop","pushw","popw"):
        regs = [op.reg for op in ops if op.type == ARM_OP_REG]
        if m.startswith("push"):
            for r in sorted(regs, key=RIDX, reverse=True): L.append("s->r[13] -= 4; wr32(s, s->r[13], %s);" % R(r))
        else:
            for r in sorted(regs, key=RIDX): L.append("%s = rd32(s, s->r[13]); s->r[13] += 4;" % R(r))
    elif m in ("mvns","mvn") and len(ops) >= 2:
        if isi(1): L.append("%s = ~0x%Xu;" % (R(reg(0)), imm(1)))
        else: L.append("%s = ~%s;" % (R(reg(0)), R(reg(1))))
    elif m in ("ldm","ldmia","ldmdb","ldm.w","ldmia.w"):
        regs = [op.reg for op in ops if op.type == ARM_OP_REG]
        if regs:
            base = regs[0]
            lst = sorted(regs[1:], key=RIDX)
            lmode = "db" if "db" in ins.mnemonic else "ia"
            wb = ("!" in ins.op_str)
            L.append("{ uint32_t _lb = %s;" % R(base))
            if lmode == "ia":
                for i, r in enumerate(lst):
                    L.append("%s = rd32(s, _lb + %du);" % (R(r), i*4))
                if wb:
                    L.append("%s = _lb + %du; }" % (R(base), len(lst)*4))
                else:
                    L.append("}")
            else:
                for i, r in enumerate(lst):
                    L.append("%s = rd32(s, _lb - %du);" % (R(r), (len(lst)-i)*4))
                if wb:
                    L.append("%s = _lb - %du; }" % (R(base), len(lst)*4))
                else:
                    L.append("}")
    elif m in ("stm","stmia","stmdb","stm.w","stmia.w"):
        regs = [op.reg for op in ops if op.type == ARM_OP_REG]
        if regs:
            base = regs[0]
            lst = sorted(regs[1:], key=RIDX)
            lmode = "db" if "db" in ins.mnemonic else "ia"
            wb = ("!" in ins.op_str)
            L.append("{ uint32_t _sb = %s;" % R(base))
            if lmode == "ia":
                for i, r in enumerate(lst):
                    L.append("wr32(s, _sb + %du, %s);" % (i*4, R(r)))
                if wb:
                    L.append("%s = _sb + %du; }" % (R(base), len(lst)*4))
                else:
                    L.append("}")
            else:
                for i, r in enumerate(lst):
                    L.append("wr32(s, _sb - %du, %s);" % ((len(lst)-i)*4, R(r)))
                if wb:
                    L.append("%s = _sb - %du; }" % (R(base), len(lst)*4))
                else:
                    L.append("}")
    elif m in ("vneg.f64","vabs.f64") and len(ops) >= 2:
        L.append("{ uint64_t _b = s->d[%d]; double _d; std::memcpy(&_d,&_b,8); _d = %s; std::memcpy(&s->d[%d],&_d,8); }" % (reg(1)-14, "(_d < 0 ? -_d : _d)" if m.startswith("vabs") else "-_d", reg(0)-14))
    elif m in ("vneg.f32","vabs.f32") and len(ops) >= 2:
        if m.startswith("vneg"): L.append("vfp_sf_set(s, %d, -vfp_sf(s,%d));" % (reg(0)-79, reg(1)-79))
        else: L.append("vfp_sf_set(s, %d, vfp_sf(s,%d) < 0 ? -vfp_sf(s,%d) : vfp_sf(s,%d));" % (reg(0)-79, reg(1)-79, reg(1)-79, reg(1)-79))
    elif m in ("vcvt.f64.f32","vcvt.f32.f64") and len(ops) >= 2:
        if m.startswith("vcvt.f64"):
            L.append("{ double _d = (double)vfp_sf(s,%d); std::memcpy(&s->d[%d], &_d, 8); }" % (reg(1)-79, reg(0)-14))
        else:
            L.append("{ double _d; std::memcpy(&_d, &s->d[%d], 8); vfp_sf_set(s, %d, (float)_d); }" % (reg(1)-14, reg(0)-79))
    elif m == "vcmp.f64" and len(ops) >= 2:
        L.append("{ double _a,_b; std::memcpy(&_a,&s->d[%d],8); std::memcpy(&_b,&s->d[%d],8); s->apsr=(_a==_b)?0x40000000u:((_a<_b)?0x80000000u:0x20000000u); }" % (reg(0)-14, reg(1)-14))
    elif m in ("nop","it","itt","ite","itet","itettt","ittt","itee","itte","hint","yield","wfi","wfe","sev","dsb","dmb","isb","cpsid","cpsie","nop.w","pld","pldw","clrex"):
        L.append("/* %s */" % ins.mnemonic)
    elif m in ("tbb","tbh"):
        sup.append((ins.address, ins.mnemonic, ins.op_str)); L.append("/* UNSUPPORTED_TABLE %s %s */" % (ins.mnemonic, ins.op_str))
    else:
        sup.append((ins.address, ins.mnemonic, ins.op_str)); L.append("/* UNSUPPORTED %s %s */" % (ins.mnemonic, ins.op_str))
    return L

out = []
out.append("// generated by _gen_batch2.py: %d functions (normalized Thumb-2/VFP)" % NFUNCS)
out.append("#include <cstdint>")
out.append("#include <cstring>")
out.append("struct RecState { uint32_t r[16]; uint32_t apsr; uint8_t* base; uint32_t mem_size; uint64_t d[32]; };")
out.append("static inline uint8_t* P(RecState* s, uint32_t a){ uint32_t off; if (a >= 0x80600000u && a < 0x84600000u) off = a - 0x80600000u; else if (a >= 0xA0000000u && a < 0xA1000000u) off = (a - 0xA0000000u) + 0x1000000u; else off = a & 0x1FFFFFFFu; return s->base + off; }")
out.append("static inline uint32_t rd32(RecState* s,uint32_t a){ uint32_t v; std::memcpy(&v,P(s,a),4); return v; }")
out.append("static inline void wr32(RecState* s,uint32_t a,uint32_t v){ std::memcpy(P(s,a),&v,4); }")
out.append("static inline uint16_t rd16(RecState* s,uint32_t a){ uint16_t v; std::memcpy(&v,P(s,a),2); return v; }")
out.append("extern \"C\" void wr16_watch(uint32_t a, uint32_t v);")
out.append("static inline void wr16(RecState* s,uint32_t a,uint16_t v){ if (g_watch_on) wr16_watch(a,(uint32_t)v); std::memcpy(P(s,a),&v,2); }")
out.append("static inline uint64_t rd64(RecState* s,uint32_t a){ uint64_t v; std::memcpy(&v,P(s,a),8); return v; }")
out.append("static inline void wr64(RecState* s,uint32_t a,uint64_t v){ std::memcpy(P(s,a),&v,8); }")
out.append("static inline void cmp_flags(RecState* s,uint32_t a,uint32_t b){ s->apsr=(a==b)?0x40000000u:((a<b)?0x80000000u:0u); }")
out.append("static inline uint32_t f32_bits(float f){ uint32_t b; std::memcpy(&b,&f,4); return b; }")
out.append("static inline uint32_t vfp_s_get(RecState* s,int n){ return (uint32_t)((s->d[n>>1] >> ((n&1)?32:0)) & 0xFFFFFFFFu); }")
out.append("static inline void vfp_s_set(RecState* s,int n,uint32_t v){ int i=n>>1; uint64_t d=s->d[i]; if(n&1) d=(d&0xFFFFFFFFull)|((uint64_t)v<<32); else d=(d&0xFFFFFFFF00000000ull)|v; s->d[i]=d; }")
out.append("static inline uint64_t vfp_s_bits(RecState* s,int n){ return (uint32_t)((s->d[n>>1] >> ((n&1)?32:0)) & 0xFFFFFFFFu); }")
out.append("static inline void vfp_s_bits_set(RecState* s,int n,uint32_t v){ s->d[n>>1] = (n&1)? ((s->d[n>>1] & 0xFFFFFFFFull) | ((uint64_t)v<<32)) : ((s->d[n>>1] & 0xFFFFFFFF00000000ull) | v); }")
out.append("static inline float vfp_sf(RecState* s,int n){ uint32_t b=(uint32_t)vfp_s_bits(s,n); float f; std::memcpy(&f,&b,4); return f; }")
out.append("static inline void vfp_sf_set(RecState* s,int n,float f){ uint32_t b; std::memcpy(&b,&f,4); vfp_s_bits_set(s,n,b); }")
out.append("static inline float vfpS(int n){ float f; uint32_t b=0; (void)b; std::memset(&f,0,4); return f; }")
out.append("static inline uint64_t vfpD_get(RecState* s,int n){ return s->d[n]; }")
out.append("static inline void vfp_d_set(RecState* s,int n,uint64_t v){ s->d[n]=v; }")
out.append("typedef uint32_t (*BlockFn)(RecState*);")
out.append("")
sup_all = []
decls = []; table = []; defs = []
_EMITTED = set()
tb = 0; ti = 0
for f in funcs:
    o = vo(f["va"])
    if o is None: continue
    insns = list(md.disasm(d[o:o+f["size"]], f["va"]))
    if not insns: continue
    ti += len(insns)
    leaders = set([f["va"]])
    for ins in insns:
        m = base_m(ins.mnemonic)
        tg = [(op.imm & 0xFFFFFFFF) for op in ins.operands if op.type == ARM_OP_IMM]
        tgt = ((tg[0] & 0xFFFFFFFF) & 0xFFFFFFFE) if tg else 0
        if ins.mnemonic in ("b","bl","blx","b.w") and tgt:
            leaders.add(tgt)
            if ins.mnemonic in ("bl","blx","b.w"): leaders.add(ins.address + ins.size)
        if m.startswith("b") and m not in ("b","bl","bx","blx") and tgt:
            leaders.add(tgt); leaders.add(ins.address + ins.size)
        if ins.mnemonic in ("bx","cbz","cbnz") or (m == "pop"):
            leaders.add(ins.address + ins.size)
    blocks = []; cur = None
    for ins in insns:
        if cur is None or ins.address in leaders:
            cur = {"start": ins.address, "insns": []}; blocks.append(cur)
        cur["insns"].append(ins)
        if is_term(ins): cur = None
    tb += len(blocks)
    for b in blocks:
        if b["start"] in _EMITTED: continue
        _EMITTED.add(b["start"])
        decls.append("static uint32_t blk_%08X(RecState* s);" % b["start"])
        table.append("  { 0x%08Xu, blk_%08X }," % (b["start"], b["start"]))
        last = b["insns"][-1]; lm = base_m(last.mnemonic)
        tg = [(op.imm & 0xFFFFFFFF) for op in last.operands if op.type == ARM_OP_IMM]
        tgt = ((tg[0] & 0xFFFFFFFF) & 0xFFFFFFFE) if tg else 0
        fall = last.address + last.size
        term = None
        if last.mnemonic in ("b","b.w") and tgt: term = "jmp %08X" % tgt
        elif last.mnemonic in ("bl","blx","b.w") and tgt: term = "ret %08X" % fall
        elif lm.startswith("b") and lm not in ("b","bl","bx","blx") and tgt:
            c = COND.get(lm)
            if c: term = "cond %s %08X %08X" % (c, tgt, fall)
            else:
                sup_all.append((last.address, last.mnemonic, last.op_str))
                term = "ret %08X" % fall
        elif lm == "bx":
            reg = last.operands[0].reg if last.operands else -1
            term = "ret 0" if reg == 10 else "indirect"
        elif last.mnemonic in ("blx",): term = "indirect"
        elif lm == "pop" and any(op.type == ARM_OP_REG and op.reg == 11 for op in last.operands): term = "ret 0"
        elif lm in ("cbz","cbnz") and tgt:
            reg = last.operands[0].reg
            term = "cbz %s %d %08X %08X" % ("0" if lm == "cbz" else "1", reg, tgt, fall)
        elif last.mnemonic in ("tbb","tbh"): term = "indirect"
        elif lm in ("svc","bkpt","udf","hlt"): term = "ret 0"
        else: term = "ret %08X" % fall
        body = []
        for ins in b["insns"][:-1]:
            body += emit(ins, sup_all)
        if term.startswith("jmp"): body.append("return 0x%su;" % term.split()[1])
        elif term.startswith("ret "):
            v = term.split()[1]
            body.append("return %s;" % ("0u" if v == "0" else "0x%su" % v))
        elif term == "indirect": body.append("return 0xFFFFFFFEu; /* indirect dispatch */")
        elif term.startswith("cond"):
            _, c, t, ff = term.split(" ", 3)
            body.append("if (%s) return 0x%su; return 0x%su;" % (c, t, ff))
        elif term.startswith("cbz"):
            _, z, reg, t, ff = term.split()
            body.append("if (s->r[%s] %s 0) return 0x%su; return 0x%su;" % (reg, "==" if z == "0" else "!=", t, ff))
        defs.append("static uint32_t blk_%08X(RecState* s) { " % b["start"] + " ".join(body) + " }")

out.append("\n".join(decls)); out.append("")
out.append("static const struct { uint32_t va; BlockFn fn; } g_blocks[] = {")
out += table; out.append("};"); out.append("")
out.append("\n".join(defs)); out.append("")
out.append("uint32_t recomp_dispatch(RecState* s, uint32_t pc, int max_blocks){")
out.append("  while (max_blocks-- > 0){ BlockFn fn=nullptr;")
out.append("    for (unsigned i=0;i<sizeof(g_blocks)/sizeof(g_blocks[0]);++i) if (g_blocks[i].va==pc){fn=g_blocks[i].fn;break;}")
out.append("    if (!fn) return 0xFFFFFFFFu; uint32_t nxt=fn(s);")
out.append("    if (nxt==0u||nxt==0xFFFFFFFFu||nxt==0xFFFFFFFEu) return nxt; pc=nxt; }")
out.append("  return 0xFFFFFFFDu; }")
cpp = "\n".join(out) + "\n"
path = os.path.join(OUTDIR, "recomp_all_%d.cpp" % NFUNCS)
open(path, "w", encoding="utf-8").write(cpp)
print("functions:", len(funcs), "blocks:", tb, "insns:", ti, "unsupported:", len(sup_all))
print("generated:", path, "lines:", cpp.count("\n"))
if sup_all:
    c = collections.Counter(m for (_, m, _) in sup_all)
    print("unsupported mnemonics:", c.most_common(20))
gxx = r"C:\dev\mingw64\bin\g++.exe"
obj = os.path.join(OUTDIR, "recomp_all_%d.o" % NFUNCS)
t0 = time.time()
r = subprocess.run([gxx, "-std=c++17", "-O2", "-c", path, "-o", obj], capture_output=True, text=True)
dt = time.time() - t0
print("compile rc:", r.returncode, "time: %.1fs" % dt)
if r.stderr: print(r.stderr[:1500])
print("obj size:", os.path.getsize(obj) if os.path.exists(obj) else "MISSING")
