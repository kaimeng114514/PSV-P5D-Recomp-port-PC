import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
old='''                  printf("[GXPDIAG] gxp=%08X size=%u pc=%u po=%X spv=%s\\n", _gxp,_sz,_pc,_po,_it->second.c_str());'''
assert old in t
new='''                  uint32_t _bf=0,_tu1=0,_tu2=0; std::memcpy(&_bf,_g+0x18,4); std::memcpy(&_tu1,_g+0x1C,4); std::memcpy(&_tu2,_g+0x20,4);
                  printf("[GXPDIAG] gxp=%08X size=%u pc=%u po=%X bf=%08X tu1=%08X tu2=%08X spv=%s\\n", _gxp,_sz,_pc,_po,_bf,_tu1,_tu2,_it->second.c_str());
                  if ((_tu1|_tu2) && _texprog_n++<60) { printf("[GXPTEX] gxp=%08X tu1=%08X tu2=%08X spv=%s\\n", _gxp, _tu1, _tu2, _it->second.c_str()); fflush(stdout); }'''
t=t.replace(old,new,1)
# declare counter near GXPDIAG block start
anchor='static int _n=0; if (_n++<12) printf("[ABIPGID]'
assert anchor in t
t=t.replace(anchor, 'static int _texprog_n=0;\n              '+anchor,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("GXPTEX installed")
