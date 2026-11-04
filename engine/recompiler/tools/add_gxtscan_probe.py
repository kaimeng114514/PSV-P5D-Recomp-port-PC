import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
if "GXTSCAN" in t: print("already"); sys.exit(0)
anchor='    if ((pc & ~1u) == 0x811ED69Au) { // TEXUPLOAD2'
assert anchor in t
ins='''    if ((pc & ~1u) == 0x811ED69Au) { // GXTSCAN: find where nonzero pixels are inside GXT buffer
      static int _gs=0; if(_gs++<8){ uint32_t _p=s->r[0]; uint32_t _off=0,_sz=0; if(_p){ uint8_t* _q=GP(_p+0xcu); if(_q)std::memcpy(&_off,_q,4); _q=GP(_p+0x10u); if(_q)std::memcpy(&_sz,_q,4);} 
        if(_p && _off<0x1000u && _sz && _sz<0x200000u){ uint32_t _limit=_sz; if(_limit>0x80000u)_limit=0x80000u; uint32_t _nz=0,_first=0xFFFFFFFFu; uint32_t _n=0;
          for(uint32_t _i=0;_i<_limit;++_i){ uint8_t* _b=GP(_p+_off+_i); if(!_b) break; if(*_b){ ++_nz; if(_first==0xFFFFFFFFu)_first=_i; } if(++_n>=0x20000u) break; } 
          printf("[GXTSCAN] img=%08X off=%X sz=%X scanned=%u nz=%u first=%X:", _p,_off,_sz,_n,_nz,_first);
          if(_first!=0xFFFFFFFFu) for(int _k=0;_k<8;++_k){ uint32_t _v=0; uint8_t* _q2=GP(_p+_off+_first+(uint32_t)_k*4u); if(_q2) std::memcpy(&_v,_q2,4); printf(" %08X",_v);} 
          printf("\\n"); fflush(stdout);} }
      static int _gs2=0; if(_gs2++>=8){} }
'''
t=t.replace(anchor, ins,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("GXTSCAN installed")
