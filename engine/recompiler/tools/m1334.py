import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()

helper = '''
// ===== M1317: CPU vertex-shader bridge =====
// Runs the ORIGINAL vertex program (GXP, decoded by Project Nativ's USSE interpreter) on the CPU
// for every vertex of a draw, then writes the resulting clip position / colour / texcoord back into
// the packed vertex array in the layout the bridge shaders expect (loc0=clip pos, loc1=colour,
// loc2=uv).  The GPU then only rasterises and (optionally) samples unit 0.  This keeps geometry,
// colours, UVs and textures produced by the original game code while bypassing our old, broken
// GXP->SPIR-V translations.
static bool p5d_cpu_vp_apply(std::vector<float>& vtx, unsigned vc, unsigned nattr,
                             uint32_t gxp_va, uint32_t def_ubo, uint32_t ubo0) {
  if (!gxp_va || !vc || !nattr || vtx.size() < (size_t)vc * 32u) return false;
  const UsseProgram* p = usse_get(gxp_va);
  if (!p || !usse_supported(p) || usse_is_fragment(p) || usse_primary_reads_memory(p)) return false;
  const UsseVertexLayout* vl = usse_vertex_layout(p);
  if (!vl || vl->nattrs <= 0 || (unsigned)vl->nattrs > nattr || vl->nattrs > 8) return false;
  // skip the fullscreen clear-style programs (no colour output, single position attribute)
  if (vl->o_color0 < 0 && vl->nattrs <= 1 && vl->index_reg < 0) return false;
  UsseDraw d; std::memset(&d, 0, sizeof(d));
  uint32_t ubuf[14]; std::memset(ubuf, 0, sizeof(ubuf)); ubuf[0] = ubo0;
  usse_setup_registers(p, &d, def_ubo, ubuf);
  UsseInst si; std::memset(&si, 0, sizeof(si));
  usse_run_secondary(p, &si, &d);
  for (unsigned v = 0; v < vc; ++v) {
    UsseInst vi; std::memset(&vi, 0, sizeof(vi));
    for (int a = 0; a < vl->nattrs; ++a) {
      const UsseAttrSlot* slot = &vl->attrs[a];
      float comps[4] = {0,0,0,0};
      for (int c = 0; c < slot->comps && c < 4; ++c) comps[c] = vtx[(size_t)v*32u + (size_t)a*4u + (size_t)c];
      usse_pack_attr(&vi, slot, comps, nullptr, 0);
    }
    usse_run_primary(p, &vi, &d, nullptr, nullptr);
    float* out = &vtx[(size_t)v*32u];
    out[0] = vi.o[0].f; out[1] = vi.o[1].f; out[2] = vi.o[2].f; out[3] = vi.o[3].f;
    if (vl->o_color0 >= 0 && vl->o_color0 + 3 < 160)
      for (int c = 0; c < 4; ++c) out[4+c] = vi.o[vl->o_color0 + c].f;
    if (vl->o_tex[0] >= 0 && vl->ncomp_tex[0] >= 2 && vl->o_tex[0] + 1 < 160) {
      out[8] = vi.o[vl->o_tex[0]].f; out[9] = vi.o[vl->o_tex[0] + 1].f;
      out[10] = 0.0f; out[11] = 1.0f;
    }
  }
  return true;
}
static const char* p5d_bridge_frag_dir() {
  static std::string s; static bool init = false;
  if (!init) { init = true; const char* e = getenv("P5D_BRIDGE_DIR"); s = e ? e : "C:/dev/p5d_build/spv_nativ"; }
  return s.c_str();
}
'''
# insert helper before first "static inline uint8_t* GP(uint32_t va)"? need GP later only. Insert after the usse memory accessors (marker p5d_usse_wr8 line end)
marker = 'extern "C" void p5d_usse_wr8 (uint32_t va, uint8_t  v) { uint8_t* _p = GP(va); if (_p) *_p = v; }\n'
assert marker in t
if 'p5d_cpu_vp_apply' not in t:
    t = t.replace(marker, marker + helper, 1)

# ---- content branch: insert transform at start of if (_ok) block
anchorA = '''          if (_ok) {
            int _rr = p5d::vulkan::upload_vertex_stream(_vtx.data(), (unsigned)(_vc*128u), _vc);'''
assert anchorA in t
newA = '''          if (_ok) {
            bool _cpub = false; bool _haztex = false;
            { static int _cvp=-1; if(_cvp<0){ const char* _e=getenv("P5D_CPUVP"); _cvp=(_e&&atoi(_e))?1:0; }
              if (_cvp) {
                for (int _u=0;_u<16;++_u){ for(int _k=0;_k<16;++_k) if(_frag_tex[_u][_k]){ _haztex=true; break; } if(_haztex) break; }
                uint32_t _gxp=0; { auto _gi=_obj2gxp.find(_cur_vobj); if(_gi!=_obj2gxp.end()) _gxp=_gi->second; }
                uint32_t _def=_ufd_vert_ubo; if(!_def && _pend_vout){ uint8_t* _q=GP(_pend_vout); if(_q) std::memcpy(&_def,_q,4); }
                _cpub = p5d_cpu_vp_apply(_vtx, _vc, (unsigned)_av.size(), _gxp, _def, _cur_v_ubo);
                static int _cn=0; if(_cn++<24){ printf("[CPUVP] #%d obj=%08X gxp=%08X vc=%u nattr=%u ok=%d tex=%d spv=%s\\n", _cn,_cur_vobj,_gxp,_vc,(unsigned)_av.size(),(int)_cpub,(int)_haztex,_abi_vspv.c_str()); fflush(stdout);} } }
            int _rr = p5d::vulkan::upload_vertex_stream(_vtx.data(), (unsigned)(_vc*128u), _vc);'''
t = t.replace(anchorA, newA, 1)

# ---- precomputed branch: same
anchorB = '''            if (_ok) {
              for (int _u=0;_u<16;++_u){ bool _nz=false; for(int _k=0;_k<16;++_k) if(_frag_tex[_u][_k]){_nz=true;break;} if(!_nz) continue; (void)p5d::vulkan::upload_gxm_control_resolved((unsigned)_u,_frag_tex[_u]); }

              if (_cur_v_ubo){ uint8_t* _src=GP(_cur_v_ubo); if(_src) p5d::vulkan::update_uniform_buffer_raw(_src,0x1000u); }
              if (_cur_f_ubo){ uint8_t* _src=GP(_cur_f_ubo); if(_src) p5d::vulkan::update_fragment_uniform_buffer_raw(_src,0x1000u); }
              if (!_abi_vspv.empty() && !_abi_fspv.empty()) { std::string _vp=_abi_spvdir+"/"+_abi_vspv, _fp=_abi_spvdir+"/"+_abi_fspv; (void)p5d::vulkan::select_real_pipeline(_vp.c_str(),_fp.c_str()); }
              int _rr = p5d::vulkan::upload_vertex_stream(_pvtx.data(),(unsigned)(_vc*128u),_vc);'''
assert anchorB in t
newB = '''            if (_ok) {
              for (int _u=0;_u<16;++_u){ bool _nz=false; for(int _k=0;_k<16;++_k) if(_frag_tex[_u][_k]){_nz=true;break;} if(!_nz) continue; (void)p5d::vulkan::upload_gxm_control_resolved((unsigned)_u,_frag_tex[_u]); }

              if (_cur_v_ubo){ uint8_t* _src=GP(_cur_v_ubo); if(_src) p5d::vulkan::update_uniform_buffer_raw(_src,0x1000u); }
              if (_cur_f_ubo){ uint8_t* _src=GP(_cur_f_ubo); if(_src) p5d::vulkan::update_fragment_uniform_buffer_raw(_src,0x1000u); }
              bool _cpub = false; bool _haztex = false;
              { static int _cvp2=-1; if(_cvp2<0){ const char* _e=getenv("P5D_CPUVP"); _cvp2=(_e&&atoi(_e))?1:0; }
                if (_cvp2) {
                  for (int _u=0;_u<16;++_u){ for(int _k=0;_k<16;++_k) if(_frag_tex[_u][_k]){ _haztex=true; break; } if(_haztex) break; }
                  uint32_t _gxp=0; { auto _gi=_obj2gxp.find(_cur_vobj); if(_gi!=_obj2gxp.end()) _gxp=_gi->second; }
                  uint32_t _def=_ufd_vert_ubo; if(!_def && _pend_vout){ uint8_t* _q=GP(_pend_vout); if(_q) std::memcpy(&_def,_q,4); }
                  unsigned _na=0; for(size_t _i=0;_i<_ps.attrs.size() && _i<8;++_i) _na++;
                  _cpub = p5d_cpu_vp_apply(_pvtx, _vc, _na, _gxp, _def, _cur_v_ubo);
                  static int _cn2=0; if(_cn2++<24){ printf("[CPUVP] pre #%d obj=%08X gxp=%08X vc=%u nattr=%u ok=%d tex=%d spv=%s\\n", _cn2,_cur_vobj,_gxp,_vc,_na,(int)_cpub,(int)_haztex,_abi_vspv.c_str()); fflush(stdout);} } }
              if (_cpub) {
                std::string _vp = std::string(p5d_bridge_frag_dir()) + "/bridge_vert.spv";
                std::string _fp = std::string(p5d_bridge_frag_dir()) + (_haztex ? "/bridge_tex_frag.spv" : "/bridge_flat_frag.spv");
                (void)p5d::vulkan::select_real_pipeline(_vp.c_str(), _fp.c_str());
              } else if (!_abi_vspv.empty() && !_abi_fspv.empty()) { std::string _vp=_abi_spvdir+"/"+_abi_vspv, _fp=_abi_spvdir+"/"+_abi_fspv; (void)p5d::vulkan::select_real_pipeline(_vp.c_str(),_fp.c_str()); }
              int _rr = p5d::vulkan::upload_vertex_stream(_pvtx.data(),(unsigned)(_vc*128u),_vc);'''
t = t.replace(anchorB, newB, 1)

# content branch: pipeline selection override (second occurrence with _vtx upload anchor sequence)
old_sel = '''              if (!_abi_vspv.empty() && !_abi_fspv.empty()) { std::string _vp=_abi_spvdir+"/"+_abi_vspv, _fp=_abi_spvdir+"/"+_abi_fspv; (void)p5d::vulkan::select_real_pipeline(_vp.c_str(),_fp.c_str()); }
              int _rr = p5d::vulkan::upload_vertex_stream(_vtx.data(),(unsigned)(_vc*128u),_vc);'''
new_sel = '''              if (_cpub) {
                std::string _vp = std::string(p5d_bridge_frag_dir()) + "/bridge_vert.spv";
                std::string _fp = std::string(p5d_bridge_frag_dir()) + (_haztex ? "/bridge_tex_frag.spv" : "/bridge_flat_frag.spv");
                (void)p5d::vulkan::select_real_pipeline(_vp.c_str(), _fp.c_str());
              } else if (!_abi_vspv.empty() && !_abi_fspv.empty()) { std::string _vp=_abi_spvdir+"/"+_abi_vspv, _fp=_abi_spvdir+"/"+_abi_fspv; (void)p5d::vulkan::select_real_pipeline(_vp.c_str(),_fp.c_str()); }
              int _rr = p5d::vulkan::upload_vertex_stream(_vtx.data(),(unsigned)(_vc*128u),_vc);'''
if old_sel in t:
    t=t.replace(old_sel,new_sel,1)
    print("content selection patched")
else:
    print("WARN content selection anchor not found")
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("patched harness CPUVP")
