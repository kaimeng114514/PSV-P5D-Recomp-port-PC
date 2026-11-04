import io
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
# revert the wrongly changed precomputed draw back to _vc (the occurrence before position 300000)
i=t.find('p5d::vulkan::gxm_draw_arrays(_vc_x);')
# identify the problematic one: it's before 'std::vector<float> _vtx_x' region? Actually precomputed path is early; find first _vc_x and check context
first=t.find('p5d::vulkan::gxm_draw_arrays(_vc_x);')
print("first _vc_x draw at",first, "context:",t[max(0,first-200):first+80].replace("\n"," | ")[:300])
# find the precomputed draw that we changed: look for line with _vc_x near 'PREDRAW' printf context
cand=t.find('p5d::vulkan::gxm_draw_arrays(_vc_x);\n              static int _pn=0;')
if cand>0:
    t=t[:cand]+'p5d::vulkan::gxm_draw_arrays(_vc);\n              static int _pn=0;'+t[cand+len('p5d::vulkan::gxm_draw_arrays(_vc_x);\n              static int _pn=0;'):]
    print("reverted precomputed draw")
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
