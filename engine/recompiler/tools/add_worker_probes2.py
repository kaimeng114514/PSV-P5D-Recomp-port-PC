import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
if "WORKERWK2" in t: print("already"); sys.exit(0)
anchor='    t->pc_ring[(t->pc_n++) & 31u] = pc;'
assert anchor in t
ins='''    t->pc_ring[(t->pc_n++) & 31u] = pc;
    if ((pc & ~1u) == 0x811E68FCu && tid >= 0x40011000u && tid <= 0x40016000u) { // WORKERWK2: worker returned from WaitSema
      static int _wk2=0; if(_wk2++<40){ printf("[WORKERWK2] #%d tid=%08X r0=%08X lr=%08X blk=%u\\n", _wk2, tid, s->r[0], s->r[14], (unsigned)g_hf_blocks); fflush(stdout);} }
    if ((pc & ~1u) == 0x811E6930u && tid >= 0x40011000u && tid <= 0x40016000u) { // WORKERCB2: worker about to call job callback
      static int _wc2=0; if(_wc2++<40){ uint32_t _fn=0; uint8_t* _q=GP(s->r[11]); if(_q) std::memcpy(&_fn,_q,4); printf("[WORKERCB2] #%d tid=%08X fn=%08X r0=%08X r1=%08X lr=%08X blk=%u\\n", _wc2, tid, _fn, s->r[0], s->r[1], s->r[14], (unsigned)g_hf_blocks); fflush(stdout);} }'''
t=t.replace(anchor, ins,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("WORKERWK2/WORKERCB2 installed in guest_thread_run")
