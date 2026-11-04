import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
if "CONVJOB" in t: print("already"); sys.exit(0)
anchor='    if ((pc & ~1u) == 0x811866A6u) {   // M1186: app movie-slot state machine'
ins='''    if ((pc & ~1u) == 0x811CB0D9u) { // CONVJOB: texture conversion job callback
      static int _cj=0; bool _pl=(_cj<24); ++_cj; if(_pl){ printf("[CONVJOB] #%d job=%08X n=%u\\n", _cj, s->r[0], g_main_n); fflush(stdout);} }
    if ((pc & ~1u) == 0x811E649Au) { // JOBDISP: FUN_811e649a(queue,desc)
      static int _jd=0; if(_jd++<24){ printf("[JOBDISP] #%d q=%u desc=%08X cb=%08X n=%u\\n", _jd, s->r[0], s->r[1], s->r[2], g_main_n); fflush(stdout);} }
'''
t=t.replace(anchor, ins+anchor,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("CONVJOB/JOBDISP installed")
