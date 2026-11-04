import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
if "TEXCREATE" in t: print("already"); sys.exit(0)
# 1) probes at texture creation chain
anchor='    if ((pc & ~1u) == 0x811866A6u) {   // M1186: app movie-slot state machine'
ins='''    if ((pc & ~1u) == 0x8116332Au) { // TEXCREATE: appGxTexture(img)
      static int _tc2=0; if(_tc2++<8){ uint32_t _p=s->r[0]; printf("[TEXCREATE] #%d img=%08X:", _tc2, _p);
        if(_p){ for(int _k=0;_k<16;++_k){ uint32_t _v=0; uint8_t* _q=GP(_p+(uint32_t)_k*4u); if(_q) std::memcpy(&_v,_q,4); printf(" %08X",_v);} }
        printf("\\n"); fflush(stdout);} }
    if ((pc & ~1u) == 0x811CA6B6u) { // TEXCONV: FUN_811ca6b6(img, param2, fmt, 0)
      static int _cv=0; if(_cv++<12){ uint32_t _p=s->r[0]; printf("[TEXCONV] #%d img=%08X p2=%08X fmt=%u a3=%u:", _cv, _p, s->r[1], s->r[2], s->r[3]);
        if(_p){ for(int _k=0;_k<8;++_k){ uint32_t _v=0; uint8_t* _q=GP(_p+(uint32_t)_k*4u); if(_q) std::memcpy(&_v,_q,4); printf(" %08X",_v);} }
        printf("\\n"); fflush(stdout);} }
'''
t=t.replace(anchor, ins+anchor,1)
# 2) byte/half write watch for P8 index region
a8='extern "C" void wr8_watch(uint32_t a, uint32_t v) {'
ins8='''
  { if (a >= 0x90249000u && a < 0x902C9000u) { static int _ix8=0; if(_ix8++<80){ printf("[ATLASIDX8] a=%08X v=%02X blk=%08X\\n", a, v&0xFF, g_cur_block); fflush(stdout);} } }
'''
t=t.replace(a8, a8+ins8,1)
a16='extern "C" void wr16_watch(uint32_t a, uint32_t v) {'
ins16='''
  { if (a >= 0x90249000u && a < 0x902C9000u) { static int _ix16=0; if(_ix16++<60){ printf("[ATLASIDX16] a=%08X v=%04X blk=%08X\\n", a, v&0xFFFF, g_cur_block); fflush(stdout);} } }
'''
t=t.replace(a16, a16+ins16,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("TEXCREATE/TEXCONV/ATLASIDX probes installed")
