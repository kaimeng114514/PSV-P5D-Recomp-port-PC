import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
if "TEXUPLOAD4" in t: print("already"); sys.exit(0)
# add blk to ATLASMEM
old='printf("[ATLASMEM] dst=%08X len=%u src=%08X srcw=%08X %08X blk=%08X lr=%08X\\n",_ad,_al,s->r[1],_s0,_s1,g_cur_block,s->r[14]);'
assert old in t
new='printf("[ATLASMEM] dst=%08X len=%u src=%08X srcw=%08X %08X blk=%08X lr=%08X hfblk=%u\\n",_ad,_al,s->r[1],_s0,_s1,g_cur_block,s->r[14],(unsigned)g_hf_blocks);'
t=t.replace(old,new,1)
# extend TEXUPLOAD2 probe with GXT pixel dump
old2='        printf("\\n"); fflush(stdout);} }\n    if ((pc & ~1u) == 0x811ED712u)'
assert old2 in t
new2='''        printf("\\n"); uint32_t _off=0,_sz=0; { uint8_t* _q=GP(_p+0xcu); if(_q) std::memcpy(&_off,_q,4); _q=GP(_p+0x10u); if(_q) std::memcpy(&_sz,_q,4);} 
        printf("[TEXUPLOAD4] #%d GXT off=%X size=%u px:", _tu2, _off, _sz); if(_p && _off<0x1000u){ for(int _k=0;_k<8;++_k){ uint32_t _v=0; uint8_t* _q=GP(_p+_off+(uint32_t)_k*4u); if(_q) std::memcpy(&_v,_q,4); printf(" %08X",_v);} } printf("\\n"); fflush(stdout);} }
    if ((pc & ~1u) == 0x811ED712u)'''
t=t.replace(old2,new2,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("ATLASMEM blk + TEXUPLOAD4 installed")
