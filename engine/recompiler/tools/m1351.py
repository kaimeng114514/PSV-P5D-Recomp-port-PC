import io
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
old='''static int _cdn=0; if(_cdn++<400){ printf("[CPUDRAW] #%d vc=%u vc_x=%u idx=%d ic=%u cpub=%d v=%s f=%s rt=%08X n=%u\\n", _cdn,_vc,_vc_x,(int)_isidx,(unsigned)_icount,(int)_cpub,_abi_vspv.c_str(),_abi_fspv.c_str(),g_abi_cur_cd,g_main_n); fflush(stdout);} } }'''
assert old in t
new='''static int _cdn=0; if(_cdn++<400){ printf("[CPUDRAW] #%d vc=%u vc_x=%u idx=%d ic=%u cpub=%d v=%s f=%s rt=%08X p0=(%.3f,%.3f,%.3f,%.3f) c0=(%.3f,%.3f,%.3f,%.3f) uv0=(%.3f,%.3f) n=%u\\n", _cdn,_vc,_vc_x,(int)_isidx,(unsigned)_icount,(int)_cpub,_abi_vspv.c_str(),_abi_fspv.c_str(),g_abi_cur_cd,
  _vtx.size()>3?_vtx[0]:0.f,_vtx.size()>3?_vtx[1]:0.f,_vtx.size()>3?_vtx[2]:0.f,_vtx.size()>3?_vtx[3]:0.f,
  _vtx.size()>7?_vtx[4]:0.f,_vtx.size()>7?_vtx[5]:0.f,_vtx.size()>7?_vtx[6]:0.f,_vtx.size()>7?_vtx[7]:0.f,
  _vtx.size()>9?_vtx[8]:0.f,_vtx.size()>9?_vtx[9]:0.f, g_main_n); fflush(stdout);} } }'''
t=t.replace(old,new,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("CPUDRAW vertex logging added")
