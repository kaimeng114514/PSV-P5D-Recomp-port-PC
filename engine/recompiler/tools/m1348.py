import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
anchor='''    if ((pc & ~1u) == 0x8115A36Au) {   // M1189: app per-frame callback'''
assert anchor in t
probe='''    // M1328: loading-state dump - the game sits in a loading loop for thousands of frames;
    // dump the loader/manager objects so the total/progress counters and waiting condition are visible.
    if ((pc & ~1u) == 0x8115A36Au) { static int _ld=-1; if(_ld<0){ const char* _e=getenv("P5D_LOADDBG"); _ld=(_e&&atoi(_e))?1:0; }
      if(_ld){ static int _ln=0; if((_ln++ % 100)==0){
        uint32_t _root=0; { uint8_t* _q=GP(0x813366CCu); if(_q) std::memcpy(&_root,_q,4); }
        printf("[LOADDBG] n=%d root=%08X flagsAF820=", _ln, _root);
        for(int _k=0;_k<8;++_k){ uint32_t _v=0; uint8_t* _q=GP(0x813AF820u+(uint32_t)_k*4u); if(_q) std::memcpy(&_v,_q,4); printf(" %08X",_v);} printf("\\n");
        for(int _r=0;_r<2;++_r){ uint32_t _base=_r?0x813AF820u:_root; if(!_base) continue;
          printf("[LOADDBG]  %s %08X:", _r?"flags":"root", _base);
          for(int _k=0;_k<32;++_k){ uint32_t _v=0; uint8_t* _q=GP(_base+(uint32_t)_k*4u); if(_q) std::memcpy(&_v,_q,4); printf(" %08X",_v);} printf("\\n"); }
        fflush(stdout); } } }
    if ((pc & ~1u) == 0x8115A36Au) {   // M1189: app per-frame callback'''
t=t.replace(anchor,probe,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("LOADDBG probe added")
