import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
if "WORKERWK" in t: print("already"); sys.exit(0)
anchor='    if ((pc & ~1u) == 0x811866A6u) {   // M1186: app movie-slot state machine'
ins='''    if ((pc & ~1u) == 0x811E68FCu) { // WORKERWK: job worker returned from WaitSema
      static int _wk=0; if(_wk++<40){ printf("[WORKERWK] #%d tid=%08X fib=%08X lr=%08X blk=%u\\n", _wk, g_cur_thr, g_cur_fiber, s->r[14], (unsigned)g_hf_blocks); fflush(stdout);} }
    if ((pc & ~1u) == 0x811E6934u) { // WORKERCB: worker about to call job callback (blx r1)
      static int _wc=0; if(_wc++<40){ printf("[WORKERCB] #%d tid=%08X cb=%08X arg=%08X lr=%08X blk=%u\\n", _wc, g_cur_thr, s->r[1], s->r[0], s->r[14], (unsigned)g_hf_blocks); fflush(stdout);} }
'''
t=t.replace(anchor, ins+anchor,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("WORKERWK/WORKERCB installed")
