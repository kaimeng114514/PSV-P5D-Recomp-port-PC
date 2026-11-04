import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()

# 1) declare backend indexed draw
if 'void gxm_draw_indexed_real' not in t:
    anchor='namespace p5d { namespace vulkan { void gxm_draw_arrays(unsigned int vertexCount); } }   // M1142: real vkCmdDraw for the uploaded vertex stream'
    assert anchor in t
    decl='\n// M1329: backend indexed draw (uploads its own vertex+index data, handles STRIP/FAN + z clamp)\nnamespace p5d { namespace vulkan { void gxm_draw_indexed_real(const void*, unsigned int, const void*, unsigned int, unsigned int, unsigned int); } }\n'
    t=t.replace(anchor, anchor+decl, 1)

# 2) content branch: indexed draws go through the backend indexed path (no expansion needed)
old='''            std::vector<float> _vtx_x; unsigned _vc_x = _vc;
            { static int _tg=-1; if(_tg<0){ const char* _e=getenv("P5D_TOPOLOGY"); _tg=(_e&&atoi(_e))?1:0; }
              if (_tg) { p5d_topology_expand(_vtx, _vc, _ord, _prim, _vtx_x, _vc_x);
                if (_vc_x == 0u) { _vtx_x = _vtx; _vc_x = _vc; } }
              else { _vtx_x = _vtx; _vc_x = _vc; } }
            int _rr = p5d::vulkan::upload_vertex_stream(_vtx_x.data(), (unsigned)(_vc_x*128u), _vc_x);'''
assert old in t
new='''            std::vector<float> _vtx_x; unsigned _vc_x = _vc;
            const bool _isidx = (_icount > 0u && _idata2 != 0u);
            { static int _tg=-1; if(_tg<0){ const char* _e=getenv("P5D_TOPOLOGY"); _tg=(_e&&atoi(_e))?1:0; }
              if (_isidx) { _vtx_x = _vtx; _vc_x = _vc; }   // M1329: indexed draws keep vertex-order geometry
              else if (_tg) { p5d_topology_expand(_vtx, _vc, _ord, _prim, _vtx_x, _vc_x);
                if (_vc_x == 0u) { _vtx_x = _vtx; _vc_x = _vc; } }
              else { _vtx_x = _vtx; _vc_x = _vc; } }
            int _rr = 0;
            if (!_isidx) _rr = p5d::vulkan::upload_vertex_stream(_vtx_x.data(), (unsigned)(_vc_x*128u), _vc_x);'''
t=t.replace(old,new,1)

# 3) CPUDRAW block: indexed -> backend indexed real
old2='''            { static int _cd=-1; if(_cd<0){ const char* _e=getenv("P5D_CPUDRAW"); _cd=(_e&&atoi(_e))?1:0; }
              if (_cd) { p5d::vulkan::gxm_draw_arrays(_vc_x);
                static int _cdn=0; if(_cdn++<40){ printf("[CPUDRAW] #%d vc=%u vc_x=%u cpub=%d v=%s f=%s rt=%08X n=%u\\n", _cdn,_vc,_vc_x,(int)_cpub,_abi_vspv.c_str(),_abi_fspv.c_str(),g_abi_cur_cd,g_main_n); fflush(stdout);} } }'''
assert old2 in t
new2='''            { static int _cd=-1; if(_cd<0){ const char* _e=getenv("P5D_CPUDRAW"); _cd=(_e&&atoi(_e))?1:0; }
              if (_cd) {
                if (_isidx) {
                  // M1329: indexed draw - upload our vertex-order records plus the engine's own index buffer
                  // and submit an indexed draw so quads (icount=6 over 4 vertices) render both triangles.
                  const bool _i16 = (_itype2 == 0u);
                  p5d::vulkan::gxm_draw_indexed_real(_vtx_x.data(), (unsigned)(_vc_x*128u), GP(_idata2),
                                                     (unsigned)(_icount * (_i16 ? 2u : 4u)), (unsigned)_icount, _i16 ? 1u : 0u);
                } else {
                  p5d::vulkan::gxm_draw_arrays(_vc_x);
                }
                static int _cdn=0; if(_cdn++<40){ printf("[CPUDRAW] #%d vc=%u vc_x=%u idx=%d ic=%u cpub=%d v=%s f=%s rt=%08X n=%u\\n", _cdn,_vc,_vc_x,(int)_isidx,(unsigned)_icount,(int)_cpub,_abi_vspv.c_str(),_abi_fspv.c_str(),g_abi_cur_cd,g_main_n); fflush(stdout);} } }'''
t=t.replace(old2,new2,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("indexed draw path patched")
