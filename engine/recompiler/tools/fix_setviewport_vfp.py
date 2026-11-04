import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
old='''        _f[0] = 0.0f; { uint32_t _b = s->r[1]; std::memcpy(&_f[0], &_b, 4); }
        _f[1] = 0.0f; { uint32_t _b = s->r[2]; std::memcpy(&_f[1], &_b, 4); }
        _f[2] = 0.0f; { uint32_t _b = s->r[3]; std::memcpy(&_f[2], &_b, 4); }
        for (int _i = 3; _i < 6; ++_i) { uint32_t _b = 0; uint8_t* _q = GP(s->r[13] + (uint32_t)(_i - 3) * 4u); if (_q) std::memcpy(&_b, _q, 4); std::memcpy(&_f[_i], &_b, 4); }'''
assert old in t
new='''        // M1314: GXM float args arrive in VFP s0..s5 (ARM EABI), NOT r1..r3.
        // RecState.d[] holds guest d14..d45; vfp_s_get maps sN -> d[N>>1] low/high word,
        // so s0..s5 = s->d[0] low/high, s->d[1] low/high, s->d[2] low/high.
        for (int _i = 0; _i < 6; ++_i) {
          uint64_t _dd = s->d[_i >> 1];
          uint32_t _bits = (_i & 1) ? (uint32_t)(_dd >> 32) : (uint32_t)_dd;
          std::memcpy(&_f[_i], &_bits, 4);
        }
        printf("[ABIVPF] raw s0..s5 = %.3f %.3f %.3f %.3f %.3f %.3f (r1..r3=%08X %08X %08X)\\n", _f[0],_f[1],_f[2],_f[3],_f[4],_f[5], s->r[1],s->r[2],s->r[3]); fflush(stdout);'''
t=t.replace(old,new,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("SetViewport VFP decode patched")
