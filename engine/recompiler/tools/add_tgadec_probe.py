import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
if "TGADEC" in t: print("already"); sys.exit(0)
# extend LOGOTGA probe: dump buffer content
old='printf("[LOGOTGA] FUN_8119343a name=%s mem=%08X a2=%08X n=%u\\n", _nm, s->r[1], s->r[2], g_main_n); fflush(stdout);} }'
assert old in t
new='''printf("[LOGOTGA] FUN_8119343a name=%s mem=%08X a2=%08X n=%u\\n", _nm, s->r[1], s->r[2], g_main_n);
          { uint32_t _b0=s->r[1]; for(int _r=0;_r<2;++_r){ printf("[LOGOTGA]  +%02X:", _r*16); for(int _k=0;_k<4;++_k){ uint32_t _v=0; uint8_t* _q=GP(_b0+(_r*4+_k)*4u); if(_q) std::memcpy(&_v,_q,4); printf(" %08X",_v);} printf("\\n"); } }
          { uint32_t _b1=s->r[1]+0x28u; printf("[LOGOTGA]  +28:"); for(int _k=0;_k<4;++_k){ uint32_t _v=0; uint8_t* _q=GP(_b1+_k*4u); if(_q) std::memcpy(&_v,_q,4); printf(" %08X",_v);} printf("\\n"); }
          fflush(stdout);} }'''
t=t.replace(old,new,1)
# add TGADEC probe at 0x811934C0
anchor='    if ((pc & ~1u) == 0x811866A6u) {   // M1186: app movie-slot state machine'
assert anchor in t
ins='''    if ((pc & ~1u) == 0x811934C0u) { // TGADEC: after FUN_81174dbc returns, r0 = decoded image struct
      static int _td=0; if(_td++<8){ uint32_t _p=s->r[0]; printf("[TGADEC] #%d img=%08X:", _td, _p);
        if(_p){ for(int _k=0;_k<8;++_k){ uint32_t _v=0; uint8_t* _q=GP(_p+(uint32_t)_k*4u); if(_q) std::memcpy(&_v,_q,4); printf(" %08X",_v);} }
        printf("\\n"); if(_p){ for(int _r=0;_r<2;++_r){ uint32_t _pp=0; uint8_t* _q=GP(_p+(_r?4u:0u)); uint32_t _off=(_r?4u:0u); if(_q){std::memcpy(&_pp,_q,4);} if(_pp){ printf("[TGADEC]  from+%u:",_off); uint8_t* _px=GP(_pp); if(_px){ for(int _k=0;_k<4;++_k){ uint32_t _v; std::memcpy(&_v,_px+_k*4,4); printf(" %08X",_v);} } printf("\\n"); } } }
        fflush(stdout);} }
'''
t=t.replace(anchor, ins+anchor,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("LOGOTGA dump + TGADEC installed")
