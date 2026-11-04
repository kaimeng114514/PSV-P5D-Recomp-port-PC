import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
if "ATLASPX8" in t: print("already"); sys.exit(0)
a8='extern "C" void wr8_watch(uint32_t a, uint32_t v) {'
assert a8 in t
ins8='''
  { if ((a >= 0x85C70000u && a < 0x85C80000u) || (a >= 0x99C60000u && a < 0x99E00000u)) { static int _p8=0; if(_p8++<100){ printf("[ATLASPX8] a=%08X v=%02X blk=%08X\\n", a, v&0xFF, g_cur_block); fflush(stdout);} } }
'''
t=t.replace(a8, a8+ins8,1)
a16='extern "C" void wr16_watch(uint32_t a, uint32_t v) {'
ins16='''
  { if ((a >= 0x85C70000u && a < 0x85C80000u) || (a >= 0x99C60000u && a < 0x99E00000u)) { static int _p16=0; if(_p16++<60){ printf("[ATLASPX16] a=%08X v=%04X blk=%08X\\n", a, v&0xFFFF, g_cur_block); fflush(stdout);} } }
'''
t=t.replace(a16, a16+ins16,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("ATLASPX probes installed")
