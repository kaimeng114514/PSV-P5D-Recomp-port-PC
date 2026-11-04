import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
if "TXTUBO" in t: print("already"); sys.exit(0)
anchor='          if (_abi_fspv.find("frag_0084") != std::string::npos) { static int _tv2=0;'
assert anchor in t
ins='''          if (_abi_fspv.find("frag_0084") != std::string::npos) { static int _tu=0; if(_tu++<6){ printf("[TXTUBO] #%d cur_v_ubo=%08X cur_f_ubo=%08X cur_vobj=%08X:", _tu, _cur_v_ubo, _cur_f_ubo, _cur_vobj);
            if(_cur_v_ubo){ for(int _i=0;_i<8;++_i){ float _f=0; uint8_t* _q=GP(_cur_v_ubo+(uint32_t)_i*4u); if(_q) std::memcpy(&_f,_q,4); printf(" %g",_f);} }
            printf("\\n"); fflush(stdout);} }
'''
t=t.replace(anchor, ins+anchor,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("TXTUBO installed")
