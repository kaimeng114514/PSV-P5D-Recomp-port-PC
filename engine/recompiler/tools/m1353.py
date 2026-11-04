import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
# 1) add per-index uniform buffer arrays next to _cur_v_ubo declaration
anchor='static uint32_t _cur_v_ubo = 0, _cur_f_ubo = 0, _pend_vout = 0, _pend_fout = 0;'
assert anchor in t
new=anchor+'\n      static uint32_t _vubuf[16] = {0};   // M1332: per-index vertex user uniform buffers (sceGxmSetVertexUniformBuffer)\n      static uint32_t _fubuf[16] = {0};   // M1332: per-index fragment user uniform buffers'
t=t.replace(anchor,new,1)
# 2) capture per-index in SetVertexUniformBuffer + PrecomputedVertexStateSetUniformBuffer
old='if (strstr(_ge->name, "SetVertexUniformBuffer")) { _cur_v_ubo = s->r[2];'
if old not in t:
    old='if (strstr(_ge->name, "SetVertexUniformBuffer")) { _cur_v_ubo = s->r[2];'
assert old in t
new='if (strstr(_ge->name, "SetVertexUniformBuffer")) { if (s->r[1] < 16u) _vubuf[s->r[1]] = s->r[2]; _cur_v_ubo = s->r[2];'
t=t.replace(old,new,1)
old2='if (strstr(_ge->name, "PrecomputedVertexStateSetUniformBuffer")) { uint32_t _o=s->r[0], _bi=s->r[1], _b=s->r[2]; if(_bi<8u) _preVertObj[_o].ubos[_bi]=_b; _cur_v_ubo=_b;'
assert old2 in t
new2='if (strstr(_ge->name, "PrecomputedVertexStateSetUniformBuffer")) { uint32_t _o=s->r[0], _bi=s->r[1], _b=s->r[2]; if(_bi<8u) _preVertObj[_o].ubos[_bi]=_b; if(_bi<16u) _vubuf[_bi]=_b; _cur_v_ubo=_b;'
t=t.replace(old2,new2,1)
# 3) also fragment
old3='if (strstr(_ge->name, "PrecomputedFragmentStateSetUniformBuffer")) { uint32_t _o=s->r[0], _bi=s->r[1], _b=s->r[2]; if(_bi<8u) _preFragObj[_o].ubos[_bi]=_b; _cur_f_ubo=_b;'
assert old3 in t
new3='if (strstr(_ge->name, "PrecomputedFragmentStateSetUniformBuffer")) { uint32_t _o=s->r[0], _bi=s->r[1], _b=s->r[2]; if(_bi<8u) _preFragObj[_o].ubos[_bi]=_b; if(_bi<16u) _fubuf[_bi]=_b; _cur_f_ubo=_b;'
t=t.replace(old3,new3,1)
# 4) p5d_cpu_vp_apply: accept ubuf array pointer instead of single ubo0
old4='''static bool p5d_cpu_vp_apply(std::vector<float>& vtx, unsigned vc, unsigned nattr,
                             uint32_t gxp_va, uint32_t def_ubo, uint32_t ubo0) {'''
assert old4 in t
new4='''static bool p5d_cpu_vp_apply(std::vector<float>& vtx, unsigned vc, unsigned nattr,
                             uint32_t gxp_va, uint32_t def_ubo, const uint32_t* ubuf_in) {'''
t=t.replace(old4,new4,1)
old5='''  uint32_t ubuf[14]; std::memset(ubuf, 0, sizeof(ubuf)); ubuf[0] = ubo0;
  usse_setup_registers(p, &d, def_ubo, ubuf);'''
assert old5 in t
new5='''  uint32_t ubuf[14]; std::memset(ubuf, 0, sizeof(ubuf));
  if (ubuf_in) std::memcpy(ubuf, ubuf_in, sizeof(ubuf));
  usse_setup_registers(p, &d, def_ubo, ubuf);'''
t=t.replace(old5,new5,1)
# 5) content-branch caller: pass _vubuf
old6='_cpub = p5d_cpu_vp_apply(_vtx, _vc, (unsigned)_av.size(), _gxp, _def, _cur_v_ubo);'
assert old6 in t
new6='_cpub = p5d_cpu_vp_apply(_vtx, _vc, (unsigned)_av.size(), _gxp, _def, _vubuf);'
t=t.replace(old6,new6,1)
# 6) precomputed path caller (uses _pvtx)
old7='_cpub = p5d_cpu_vp_apply(_pvtx, _vc, _na, _gxp, _def, _cur_v_ubo);'
if old7 in t:
    t=t.replace(old7,'_cpub = p5d_cpu_vp_apply(_pvtx, _vc, _na, _gxp, _def, _vubuf);',1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("per-index uniform buffers patched")
