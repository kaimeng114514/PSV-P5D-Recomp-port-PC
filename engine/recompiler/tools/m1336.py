import io
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
old='if (!_tri2 || (_alw && _tri2 < 60)) { _tri2++;'
assert old in t
new='if ((!_tri2 && getenv("P5D_ABIDRAWTEST") != nullptr) || (_alw && _tri2 < 60)) { _tri2++;'
t=t.replace(old,new,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("ABIDRAWTEST gated off")
