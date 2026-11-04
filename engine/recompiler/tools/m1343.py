import io
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
close=307492
# find the exact char at close: it should be '}' of if(_ok). insert before the preceding newline
ins = '''
            // M1323: actually DRAW the content path (its gxm_draw_arrays was only inside the P5D_TPRD
            // diagnostic branch, so normal runs uploaded vertices and never drew them).
            { static int _cd=-1; if(_cd<0){ const char* _e=getenv("P5D_CPUDRAW"); _cd=(_e&&atoi(_e))?1:0; }
              if (_cd) { p5d::vulkan::gxm_draw_arrays(_vc_x);
                static int _cdn=0; if(_cdn++<24){ printf("[CPUDRAW] #%d vc=%u vc_x=%u n=%u\\n", _cdn,_vc,_vc_x,g_main_n); fflush(stdout);} } }
'''
# insert before the closing '}' at position close
t=t[:close]+ins+t[close:]
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("content draw inserted before",close)
