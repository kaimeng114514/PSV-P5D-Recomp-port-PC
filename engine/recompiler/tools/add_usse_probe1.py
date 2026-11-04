import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()

# 1) extern "C" include of nativ usse public header after <cstring> etc. Place after "#include <string>" if exists else after includes
inc = '\n// M1316: Project Nativ USSE decoder/interpreter (GPL-2+, engine/recompiler/nativ_usse)\nextern "C" {\n#include "nativ_usse/usse.h"\n}\n'
anchor = '#include <string>\n'
if anchor in t and 'nativ_usse/usse.h' not in t:
    t = t.replace(anchor, anchor + inc, 1)
elif 'nativ_usse/usse.h' not in t:
    # fallback: after last #include at top
    pos = t.find('\n\n', t.find('#include <mutex>'))
    t = t[:pos] + inc + t[pos:]

# 2) memory functions after GP definition: find end of GP function by locating "static inline uint8_t* GP(uint32_t va) {" and matching brace
i = t.find('static inline uint8_t* GP(uint32_t va) {')
assert i>=0
j = t.find('\n}', i)
assert j>=0
mem = '''
// M1316: guest-memory accessors for Project Nativ's USSE interpreter/translator (GPL-2+)
extern "C" uint8_t* p5d_usse_G(uint32_t va) { static uint8_t _z[64] = {0}; uint8_t* _p = GP(va); return _p ? _p : _z; }
extern "C" uint32_t p5d_usse_rd32(uint32_t va) { uint32_t v = 0; uint8_t* _p = GP(va); if (_p) std::memcpy(&v,_p,4); return v; }
extern "C" uint16_t p5d_usse_rd16(uint32_t va) { uint16_t v = 0; uint8_t* _p = GP(va); if (_p) std::memcpy(&v,_p,2); return v; }
extern "C" uint8_t  p5d_usse_rd8 (uint32_t va) { uint8_t v = 0; uint8_t* _p = GP(va); if (_p) v = *_p; return v; }
extern "C" uint64_t p5d_usse_rd64(uint32_t va) { uint64_t v = 0; uint8_t* _p = GP(va); if (_p) std::memcpy(&v,_p,8); return v; }
extern "C" void p5d_usse_wr32(uint32_t va, uint32_t v) { uint8_t* _p = GP(va); if (_p) std::memcpy(_p,&v,4); }
extern "C" void p5d_usse_wr16(uint32_t va, uint16_t v) { uint8_t* _p = GP(va); if (_p) std::memcpy(_p,&v,2); }
extern "C" void p5d_usse_wr8 (uint32_t va, uint8_t  v) { uint8_t* _p = GP(va); if (_p) *_p = v; }
'''
t = t[:j+2] + mem + t[j+2:]

# 3) probe inside content-draw packing block: anchor on upload of UBO inside _ok block
anchor2 = '              if (_cur_v_ubo){ uint8_t* _src=GP(_cur_v_ubo); if(_src) p5d::vulkan::update_uniform_buffer_raw(_src,0x1000u); }'
assert anchor2 in t
probe = '''
              // M1316: CPU-verify the nativ USSE path for this draw: decode the vertex GXP, build the
              // SA block by running its secondary program, pack the first vertices into PA records and
              // run the primary program, printing the resulting clip positions.
              { static int _us=-1; if(_us<0){ const char* _e=getenv("P5D_USSE"); _us=(_e&&atoi(_e))?1:0; }
                if(_us){ static int _un=0; if(_un++<12){
                  uint32_t _gxp=0; { auto _gi=_obj2gxp.find(_cur_vobj); if(_gi!=_obj2gxp.end()) _gxp=_gi->second; }
                  printf("[USSE] #%d obj=%08X gxp=%08X vspv=%s vc=%u attrN=%u ufd_v=%08X pend_v=%08X\\n",
                         _un,_cur_vobj,_gxp,_abi_vspv.c_str(),_vc,(unsigned)_av.size(),_ufd_vert_ubo,_pend_vout);
                  if(_gxp){
                    const UsseProgram* _p = usse_get(_gxp);
                    if(_p){
                      UsseDraw _d; std::memset(&_d,0,sizeof(_d));
                      uint32_t _ubuf[14]; std::memset(_ubuf,0,sizeof(_ubuf));
                      uint32_t _def = _ufd_vert_ubo;
                      if(!_def && _pend_vout){ uint8_t* _q=GP(_pend_vout); if(_q) std::memcpy(&_def,_q,4); }
                      usse_setup_registers(_p,&_d,_def,_ubuf);
                      UsseInst _si; std::memset(&_si,0,sizeof(_si));
                      usse_run_secondary(_p,&_si,&_d);
                      printf("[USSE]  kind=%s supported=%d defUBO=%08X sa:", usse_is_fragment(_p)?"frag":"vert", usse_supported(_p), _def);
                      for(int _k=0;_k<10;++_k) printf(" %08X(%.4f)", _d.sa[_k].u, _d.sa[_k].f);
                      printf("\\n");
                      if(!usse_is_fragment(_p)){
                        const UsseVertexLayout* _vl = usse_vertex_layout(_p);
                        printf("[USSE]  vl nattrs=%d total_o=%d o_color0=%d\\n", _vl->nattrs, _vl->total_o, _vl->o_color0);
                        for(unsigned _v=0;_v<_vc && _v<4;++_v){
                          UsseInst _vi; std::memset(&_vi,0,sizeof(_vi));
                          for(int _i=0;_i<_vl->nattrs && (size_t)_i<_av.size();++_i){
                            const UsseAttrSlot* _a=&_vl->attrs[_i];
                            for(int _c=0;_c<_a->comps && _c<4;++_c){
                              float _fv=_vtx[(size_t)_v*32u+(size_t)_i*4u+(size_t)_c];
                              if(_a->reg+_c < 160) _vi.pa[_a->reg+_c].u = usse_float_to_lane(_fv, _a->type);
                            }
                          }
                          usse_run_primary(_p,&_vi,&_d,nullptr,nullptr);
                          printf("[USSE]  v%u pa0=(%.3f,%.3f,%.3f,%.3f) clip=(%.4f,%.4f,%.4f,%.4f) o4=(%.3f,%.3f,%.3f,%.3f)\\n",
                                 _v, _vi.pa[0].f,_vi.pa[1].f,_vi.pa[2].f,_vi.pa[3].f,
                                 _vi.o[0].f,_vi.o[1].f,_vi.o[2].f,_vi.o[3].f,
                                 _vi.o[4].f,_vi.o[5].f,_vi.o[6].f,_vi.o[7].f);
                        }
                      }
                    }
                  }
                } } }
'''
t = t.replace(anchor2, probe + anchor2, 1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("USSE probe installed")
