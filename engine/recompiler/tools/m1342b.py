import io
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
old='p5d::vulkan::gxm_draw_arrays(_vc);'
n=t.count(old)
print("occurrences",n)
assert n==1
t=t.replace(old,'p5d::vulkan::gxm_draw_arrays(_vc_x);',1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("patched final draw")
