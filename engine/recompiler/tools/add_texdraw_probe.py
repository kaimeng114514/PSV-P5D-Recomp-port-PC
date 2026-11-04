import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
# remove wrongly placed block
bad='''
          if(_ad && _ft){ static int _tx=0; if(_tx++<24){ uint32_t _w[4]={0,0,0,0}; for(int _k=0;_k<4;++_k) std::memcpy(&_w[_k],_frag_tex[0]+_k*4,4);
            uint32_t _dv=0; std::memcpy(&_dv,_frag_tex[0]+8,4); uint32_t _s0=0,_s1=0; uint8_t* _q=GP(_dv); if(_q){ std::memcpy(&_s0,_q,4); std::memcpy(&_s1,_q+4,4); }
            printf("[TEXDRAW] #%d unit0 w=%08X %08X %08X %08X data=%08X px=%08X %08X\\n", _tx, _w[0],_w[1],_w[2],_w[3], _dv, _s0, _s1); fflush(stdout);} } }'''
assert bad in t
t=t.replace(bad,'',1)
# locate ALLDRAW printf line, replace trailing 'n=%u\\n", ... g_main_n); fflush(stdout);} } }' pattern with correct insertion inside
old='printf("[ALLDRAW] #%d lr=%08X v=%s(tu=%X) f=%s(tu=%X) r1=%08X r2=%08X r3=%08X r4=%08X n=%u\\n", _an, s->r[14], _abi_vspv.c_str(), _vt, _abi_fspv.c_str(), _ft, s->r[1],s->r[2],s->r[3],s->r[4], g_main_n); fflush(stdout);} } }'
assert old in t
new='printf("[ALLDRAW] #%d lr=%08X v=%s(tu=%X) f=%s(tu=%X) r1=%08X r2=%08X r3=%08X r4=%08X n=%u\\n", _an, s->r[14], _abi_vspv.c_str(), _vt, _abi_fspv.c_str(), _ft, s->r[1],s->r[2],s->r[3],s->r[4], g_main_n); fflush(stdout);\n            if(_ft){ static int _tx=0; if(_tx++<24){ uint32_t _w[4]={0,0,0,0}; for(int _k=0;_k<4;++_k) std::memcpy(&_w[_k],_frag_tex[0]+_k*4,4);\n              uint32_t _dv=0; std::memcpy(&_dv,_frag_tex[0]+8,4); uint32_t _s0=0,_s1=0; uint8_t* _q=GP(_dv); if(_q){ std::memcpy(&_s0,_q,4); std::memcpy(&_s1,_q+4,4); }\n              printf("[TEXDRAW] #%d unit0 w=%08X %08X %08X %08X data=%08X px=%08X %08X\\n", _tx, _w[0],_w[1],_w[2],_w[3], _dv, _s0, _s1); fflush(stdout);} } } } }'
t=t.replace(old,new,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("fixed placement")
