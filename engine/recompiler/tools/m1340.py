import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
anchor='''          if (_ok) {
            bool _cpub = false; bool _haztex = false;'''
assert anchor in t
block='''          // M1320: per-draw inventory for the CPUVP bridge work (first 220 draws): pipeline names,
          // vertex count and the engine's own colour/UV attributes of vertex 0.
          { static int _dl=-1; if(_dl<0){ const char* _e=getenv("P5D_DRAWLOG"); _dl=(_e&&atoi(_e))?1:0; }
            if(_dl){ static int _dn=0; if(_dn++<220){
              bool _haztex=false; for(int _u=0;_u<16 && !_haztex;++_u) for(int _k=0;_k<16;++_k) if(_frag_tex[_u][_k]){_haztex=true;break;}
              printf("[DRAWLOG] #%d lr=%08X vspv=%s fspv=%s vc=%u avN=%u tex=%d c0=(%.3f,%.3f,%.3f,%.3f) uv0=(%.3f,%.3f) p0=(%.3f,%.3f,%.3f,%.3f)\\n",
                _dn, s->r[14], _abi_vspv.c_str(), _abi_fspv.c_str(), _vc, (unsigned)_av.size(), (int)_haztex,
                _vtx.size()>7?_vtx[4]:0.f,_vtx.size()>7?_vtx[5]:0.f,_vtx.size()>7?_vtx[6]:0.f,_vtx.size()>7?_vtx[7]:0.f,
                _vtx.size()>9?_vtx[8]:0.f,_vtx.size()>9?_vtx[9]:0.f,
                _vtx.size()>3?_vtx[0]:0.f,_vtx.size()>3?_vtx[1]:0.f,_vtx.size()>3?_vtx[2]:0.f,_vtx.size()>3?_vtx[3]:0.f);
              fflush(stdout);} } }
          if (_ok) {
            bool _cpub = false; bool _haztex = false;'''
t=t.replace(anchor,block,1)
# expand CPUVP content-branch log cap and always show colour/uv for ok draws
t=t.replace('static int _cn=0; if(_cn++<24){ printf("[CPUVP] #%d obj=%08X','static int _cn=0; if(_cn++<200){ printf("[CPUVP] #%d obj=%08X',1)
t=t.replace('static int _bn=0; if(_bn++<64){ printf("[CPUVP] bridge select texfrag=%d','static int _bn=0; if(_bn++<200){ printf("[CPUVP] bridge select texfrag=%d',1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("DRAWLOG added")
