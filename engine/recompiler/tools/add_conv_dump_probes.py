import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
if "Q0DONE_DUMP" in t: print("already"); sys.exit(0)
# dump memory at Q0CONVDONE
old='if ((pc & ~1u) == 0x811E6936u && tid == 0x40011000u) { static int _q0c=0; if(_q0c++<20){ printf("[Q0CONVDONE] #%d tid=%08X r0=%08X blk=%u\\n", _q0c, tid, s->r[0], (unsigned)g_hf_blocks); fflush(stdout);} }'
assert old in t
new='''if ((pc & ~1u) == 0x811E6936u && tid == 0x40011000u) { static int _q0c=0; if(_q0c++<20){ printf("[Q0CONVDONE] #%d tid=%08X r0=%08X blk=%u\\n", _q0c, tid, s->r[0], (unsigned)g_hf_blocks); fflush(stdout);
        const uint32_t _v[3]={0x85C77090u,0x85CF7090u,0x90249100u};
        for(int _j=0;_j<3;++_j){ printf("[Q0DONE_DUMP] va=%08X:",_v[_j]); for(int _k=0;_k<8;++_k){ uint32_t _w=0; uint8_t* _q=GP(_v[_j]+(uint32_t)_k*4u); if(_q) std::memcpy(&_w,_q,4); printf(" %08X",_w);} printf("\\n"); } fflush(stdout);} }'''
t=t.replace(old,new,1)
# targeted atlas pixel writes (only >= 90249100 and < 902C9100), raise caps
a='extern "C" void wr8_watch(uint32_t a, uint32_t v) {'
ins8='''
  { if (a >= 0x90249100u && a < 0x902C9100u) { static int _p8b=0; if(_p8b++<60){ printf("[ATLASPIX8] a=%08X v=%02X blk=%08X\\n", a, v&0xFF, g_cur_block); fflush(stdout);} } }
'''
t=t.replace(a, a+ins8,1)
a16='extern "C" void wr16_watch(uint32_t a, uint32_t v) {'
ins16='''
  { if (a >= 0x90249100u && a < 0x902C9100u) { static int _p16b=0; if(_p16b++<60){ printf("[ATLASPIX16] a=%08X v=%04X blk=%08X\\n", a, v&0xFFFF, g_cur_block); fflush(stdout);} } }
'''
t=t.replace(a16, a16+ins16,1)
a32='extern "C" void wr32_watch(uint32_t a, uint32_t v) {'
ins32='''
  { if (a >= 0x90249100u && a < 0x902C9100u) { static int _p32b=0; if(_p32b++<100){ printf("[ATLASPIX32] a=%08X v=%08X blk=%08X\\n", a, v, g_cur_block); fflush(stdout);} } }
'''
t=t.replace(a32, a32+ins32,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("Q0DONE_DUMP + ATLASPIX probes installed")
