import io
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
t=t.replace('static int _cdn=0; if(_cdn++<40){ printf("[CPUDRAW] #%d vc=%u vc_x=%u idx=%d ic=%u cpub=%d',
            'static int _cdn=0; if(_cdn++<400){ printf("[CPUDRAW] #%d vc=%u vc_x=%u idx=%d ic=%u cpub=%d',1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("CPUDRAW cap 400")
