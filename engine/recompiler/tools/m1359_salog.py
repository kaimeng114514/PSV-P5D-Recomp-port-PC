import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
old='''  usse_run_secondary(p, &si, &d);
  for (unsigned v = 0; v < vc; ++v) {'''
assert old in t
new='''  usse_run_secondary(p, &si, &d);
  { static int _sn=-1; if(_sn<0){ const char* _e=std::getenv("P5D_SALOG"); _sn=(_e&&atoi(_e))?1:0; }
    if(_sn){ static int _k=0; if(_k++<6){ std::printf("[SALOG] def=%08X ub0=%08X sa:",def_ubo,ubuf[0]);
      for(int _i=0;_i<8;_i++) std::printf(" %08X",d.sa[_i].u); std::printf(" |");
      for(int _i=0;_i<8;_i++) std::printf(" %.5f",d.sa[_i].f); std::printf("\\n"); std::fflush(stdout);} } }
  for (unsigned v = 0; v < vc; ++v) {'''
t=t.replace(old,new,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("SALOG probe added")
