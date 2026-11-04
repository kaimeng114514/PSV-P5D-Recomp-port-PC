import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
# 1) GXTSCAN full-size scan
old='uint32_t _limit=_sz; if(_limit>0x80000u)_limit=0x80000u; uint32_t _nz=0,_first=0xFFFFFFFFu; uint32_t _n=0;'
assert old in t
new='uint32_t _limit=_sz; if(_limit>0x200000u)_limit=0x200000u; uint32_t _nz=0,_first=0xFFFFFFFFu,_last=0xFFFFFFFFu; uint32_t _n=0;'
t=t.replace(old,new,1)
old2='for(uint32_t _i=0;_i<_limit;++_i){ uint8_t* _b=GP(_p+_off+_i); if(!_b) break; if(*_b){ ++_nz; if(_first==0xFFFFFFFFu)_first=_i; } if(++_n>=0x20000u) break; }'
assert old2 in t
new2='for(uint32_t _i=0;_i<_limit;++_i){ uint8_t* _b=GP(_p+_off+_i); if(!_b) break; if(*_b){ ++_nz; if(_first==0xFFFFFFFFu)_first=_i; _last=_i; } }'
t=t.replace(old2,new2,1)
old3='printf("[GXTSCAN] img=%08X off=%X sz=%X scanned=%u nz=%u first=%X:", _p,_off,_sz,_n,_nz,_first);'
assert old3 in t
new3='printf("[GXTSCAN] img=%08X off=%X sz=%X scanned=%u nz=%u first=%X last=%X:", _p,_off,_sz,_limit,_nz,_first,_last);'
t=t.replace(old3,new3,1)
# 2) ATLASMEM for VRAM dst: add stack/pcring
old4='printf("[ATLASMEM] dst=%08X len=%u src=%08X srcw=%08X %08X blk=%08X lr=%08X hfblk=%u\\n",_ad,_al,s->r[1],_s0,_s1,g_cur_block,s->r[14],(unsigned)g_hf_blocks);'
assert old4 in t
new4='''printf("[ATLASMEM] dst=%08X len=%u src=%08X srcw=%08X %08X blk=%08X lr=%08X hfblk=%u\\n",_ad,_al,s->r[1],_s0,_s1,g_cur_block,s->r[14],(unsigned)g_hf_blocks);
        if(_ad>=0x8C600000u && _ad<0xA0000000u){ static int _vs=0; if(_vs++<8){ printf("[VRAMCOPY] sp=%08X:", s->r[13]); for(int _k=0;_k<8;++_k){ uint32_t _v=0; uint8_t* _q=GP(s->r[13]+(uint32_t)_k*4u); if(_q) std::memcpy(&_v,_q,4); printf(" %08X",_v);} printf(" pcr:"); for(int _k=0;_k<10;++_k) printf(" %08X", g_pc_ring[(g_pc_ring_n-10u+(uint32_t)_k)&63u]); printf("\\n"); } }'''
t=t.replace(old4,new4,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("GXTSCAN full + VRAMCOPY installed")
