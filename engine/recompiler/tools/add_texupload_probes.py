import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
if "TEXUPLOAD2" in t: print("already"); sys.exit(0)
anchor='    if ((pc & ~1u) == 0x811E68FCu && tid >= 0x40011000u && tid <= 0x40016000u) { // WORKERWK2'
assert anchor in t
ins='''    if ((pc & ~1u) == 0x811ED69Au) { // TEXUPLOAD2: FUN_811ed69a(image desc, extra) creates GXM texture and copies image
      static int _tu2=0; if(_tu2++<12){ printf("[TEXUPLOAD2] #%d img=%08X extra=%08X tid=%08X:", _tu2, s->r[0], s->r[1], tid);
        uint32_t _p=s->r[0]; if(_p){ for(int _k=0;_k<12;++_k){ uint32_t _v=0; uint8_t* _q=GP(_p+(uint32_t)_k*4u); if(_q) std::memcpy(&_v,_q,4); printf(" %08X",_v);} }
        printf("\\n"); fflush(stdout);} }
    if ((pc & ~1u) == 0x811ED712u) { // TEXUPLOAD2: right before size/src setup (r10=desc, r8=size, r4=extra)
      static int _tu3=0; if(_tu3++<12){ uint32_t _alloc=0; uint8_t* _q=GP(s->r[7]+0x10u); if(_q) std::memcpy(&_alloc,_q,4); uint32_t _va=0; if(_alloc){ uint8_t* _r=GP(_alloc); if(_r) std::memcpy(&_va,_r,4);} 
        printf("[TEXUPLOAD3] #%d desc=%08X size=%u extra=%u alloc=%08X vram_va=%08X blk=%u\\n", _tu3, s->r[10], s->r[8], s->r[4], _alloc, _va, (unsigned)g_hf_blocks); fflush(stdout);} }
'''
t=t.replace(anchor, ins+anchor,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("TEXUPLOAD2/3 installed")
