import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
h=io.open(H,encoding="utf-8",errors="replace").read()
if "p5d_va_dump" not in h:
    i=h.find('extern "C" {')
    assert i>0
    impl='''extern "C" void p5d_va_dump(int tag, unsigned va, unsigned n) {
  static int _c[8]={0};
  int t=(tag>=0&&tag<8)?tag:0;
  if (_c[t]++ >= 24) return;
  if (n>32u) n=32u;
  std::printf("[VADUMP] tag=%d va=%08X n=%u words:", tag, va, n);
  for (unsigned i=0;i<n;i++){ uint32_t w=0; uint8_t* q=GP(va+i*4u); if(q) std::memcpy(&w,q,4); std::printf(" %08X", w); }
  std::printf("\\n"); std::fflush(stdout);
}
'''
    h=h[:i]+impl+h[i:]
    io.open(H,"w",encoding="utf-8",newline="\n").write(h)
    print("harness p5d_va_dump added")
else: print("harness already")
P=r"C:\dev\tmp\recomp_split\batch_010.cpp"
t=io.open(P,encoding="utf-8",errors="replace").read()
if "p5d_va_dump" not in t:
    t=t.replace('extern "C" void p5d_geom_dbg(int,float,float,float,float,float,float,float,float);',
                'extern "C" void p5d_geom_dbg(int,float,float,float,float,float,float,float,float);\nextern "C" void p5d_va_dump(int,unsigned,unsigned);',1)
    old='uint32_t blk_811610A4(RecState* s) { g_cur_block = 0x811610A4u;'
    assert old in t, "block start not found"
    new='uint32_t blk_811610A4(RecState* s) { g_cur_block = 0x811610A4u; p5d_va_dump(3, s->r[14], 4); p5d_va_dump(4, s->r[1], 16);'
    t=t.replace(old,new,1)
    io.open(P,"w",encoding="utf-8",newline="\n").write(t)
    print("batch_010 caller/struct dump added")
else: print("batch already")
