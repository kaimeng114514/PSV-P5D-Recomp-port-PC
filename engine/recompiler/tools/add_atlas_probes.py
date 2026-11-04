import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
if "ATLASWR" not in t:
    anchor='extern "C" void wr32_watch(uint32_t a, uint32_t v) {'
    assert anchor in t
    ins='''
  { if (a >= 0x90240000u && a < 0x903D0000u) { static int _aw=0; if(_aw++<80){ printf("[ATLASWR] a=%08X v=%08X blk=%08X\\n", a, v, g_cur_block); fflush(stdout);} } }
'''
    t=t.replace(anchor, anchor+ins,1)
    print("ATLASWR installed")
if "ATLASMEM" not in t:
    anchor2='    case 0x812B2270u: {   // NODEWR-MEMCPY'
    assert anchor2 in t
    ins2='''
      { uint32_t _ad=s->r[0], _al=s->r[2]; if (_ad>=0x90240000u && _ad<0x903D0000u) { static int _am=0; if(_am++<40){ uint32_t _s0=0,_s1=0; uint8_t* _p=GP(s->r[1]); if(_p){std::memcpy(&_s0,_p,4);std::memcpy(&_s1,_p+4,4);} printf("[ATLASMEM] dst=%08X len=%u srcw=%08X %08X blk=%08X\\n",_ad,_al,_s0,_s1,g_cur_block); fflush(stdout);} } }'''
    t=t.replace(anchor2, anchor2+ins2,1)
    print("ATLASMEM installed")
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
