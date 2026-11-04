import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
if "TXTVTX2" in t: print("already"); sys.exit(0)
anchor='          // 18.4521 M976: ENGINE-DRAWN MOVIE - when the bound fragment texture is the 960x544 movie texture,'
assert anchor in t
ins='''          if (_abi_fspv.find("frag_0084") != std::string::npos) { static int _tv2=0; if(_tv2++<6){
            printf("[TXTVTX2] #%d ok=%d obj=%08X attrN=%u vc=%u v=%s f=%s:", _tv2, (int)_ok, _cur_vobj, (unsigned)_av.size(), _vc, _abi_vspv.c_str(), _abi_fspv.c_str());
            for(size_t _i=0;_i<_av.size() && _i<3;++_i) printf(" a%zu{fmt=%u comps=%u off=%u stream=%u}", _i,_av[_i].fmt,_av[_i].comps,_av[_i].offset,_av[_i].stream);
            printf("\\n"); for(unsigned _v=0;_v<4 && _v<_vc;++_v){ printf("[TXTVTX2]  v%u:", _v); for(size_t _i=0;_i<(_av.size()<3?_av.size():3);++_i) printf(" loc%zu=(%.3f,%.3f,%.3f,%.3f)", _i, _vtx[(size_t)_v*32u+_i*4u], _vtx[(size_t)_v*32u+_i*4u+1], _vtx[(size_t)_v*32u+_i*4u+2], _vtx[(size_t)_v*32u+_i*4u+3]); printf("\\n"); } fflush(stdout);} }
'''
t=t.replace(anchor, ins+anchor,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("TXTVTX2 installed")
