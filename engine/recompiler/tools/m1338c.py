import io
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
old='uint32_t _sv=(_av[_i].stream<16u)?_ps.streams[_av[_i].stream]:0u;'
assert old in t
new='uint32_t _sv=(_stm2va.count(_av[_i].stream)?_stm2va[_av[_i].stream]:0u);'
t=t.replace(old,new,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("fixed stream va")
