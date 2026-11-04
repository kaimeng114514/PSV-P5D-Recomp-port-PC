import io
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
old='''                static int _cdn=0; if(_cdn++<24){ printf("[CPUDRAW] #%d vc=%u vc_x=%u n=%u\\n", _cdn,_vc,_vc_x,g_main_n); fflush(stdout);} } }'''
assert old in t
new='''                static int _cdn=0; if(_cdn++<40){ printf("[CPUDRAW] #%d vc=%u vc_x=%u cpub=%d v=%s f=%s rt=%08X n=%u\\n", _cdn,_vc,_vc_x,(int)_cpub,_abi_vspv.c_str(),_abi_fspv.c_str(),g_abi_cur_cd,g_main_n); fflush(stdout);} } }'''
t=t.replace(old,new,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("CPUDRAW log extended")
