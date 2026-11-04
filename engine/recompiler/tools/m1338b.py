import io
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
t=t.replace('uint32_t _st=0; if(_st2b!=_vobj2strms.end())','uint32_t _st=0; auto _st2b=_vobj2strms.find(_cur_vobj); if(_st2b!=_vobj2strms.end())',1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("fixed _st2b")
