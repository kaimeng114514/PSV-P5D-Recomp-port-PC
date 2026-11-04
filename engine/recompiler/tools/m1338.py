import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
anchor='''                _cpub = p5d_cpu_vp_apply(_vtx, _vc, (unsigned)_av.size(), _gxp, _def, _cur_v_ubo);
                static int _cn=0; if(_cn++<24){ printf("[CPUVP] #%d obj=%08X gxp=%08X vc=%u nattr=%u ok=%d tex=%d spv=%s\\n", _cn,_cur_vobj,_gxp,_vc,(unsigned)_av.size(),(int)_cpub,(int)_haztex,_abi_vspv.c_str()); fflush(stdout);} } }'''
assert anchor in t
new='''                _cpub = p5d_cpu_vp_apply(_vtx, _vc, (unsigned)_av.size(), _gxp, _def, _cur_v_ubo);
                static int _cn=0; if(_cn++<24){ printf("[CPUVP] #%d obj=%08X gxp=%08X vc=%u nattr=%u ok=%d tex=%d spv=%s\\n", _cn,_cur_vobj,_gxp,_vc,(unsigned)_av.size(),(int)_cpub,(int)_haztex,_abi_vspv.c_str());
                  for(size_t _i=0;_i<_av.size() && _i<8;++_i){ uint32_t _st=0; if(_st2b!=_vobj2strms.end()) for(size_t _k=0;_k<_st2b->second.size();++_k) if(_st2b->second[_k].index==_av[_i].stream){_st=_st2b->second[_k].stride;break;}
                    uint32_t _sv=(_av[_i].stream<16u)?_ps.streams[_av[_i].stream]:0u;
                    printf("[AVDUMP] #%d attr%zu stream=%u off=%u fmt=%u comps=%u stride=%u sva=%08X raw=",_cn,_i,_av[_i].stream,_av[_i].offset,_av[_i].fmt,_av[_i].comps,_st,_sv);
                    uint8_t* _q=GP(_sv+_av[_i].offset); if(_q){ for(int _w=0;_w<4;++_w){ uint32_t _x=0; std::memcpy(&_x,_q+_w*4,4); printf(" %08X",_x);} }
                    printf(" packed="); for(int _c=0;_c<4;++_c) printf(" %.3f",_vtx[(size_t)_i*4u+(size_t)_c]); printf("\\n"); }
                  fflush(stdout);} } }'''
t=t.replace(anchor,new,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("AVDUMP probe added")
