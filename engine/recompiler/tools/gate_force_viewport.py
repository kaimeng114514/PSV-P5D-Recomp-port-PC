import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
pat='p5d::vulkan::set_gxm_viewport(0.0f, 960.0f, 0.0f, 544.0f, 0.0f, 1.0f); p5d::vulkan::set_gxm_viewport_enable(1);'
cnt=t.count(pat)
print("exact inline occurrences:",cnt)
# generic: line-based gate around any forced viewport call
old_lines=[
 '            p5d::vulkan::set_gxm_viewport(0.0f, 960.0f, 0.0f, 544.0f, 0.0f, 1.0f);\n            p5d::vulkan::set_gxm_viewport_enable(1);\n',
 '        p5d::vulkan::set_gxm_viewport(0.0f, 960.0f, 0.0f, 544.0f, 0.0f, 1.0f);\n        p5d::vulkan::set_gxm_viewport_enable(1);\n',
]
for ol in old_lines:
    c=t.count(ol)
    if c:
        gate='        if (getenv("P5D_FORCEVP")) { p5d::vulkan::set_gxm_viewport(0.0f, 960.0f, 0.0f, 544.0f, 0.0f, 1.0f); p5d::vulkan::set_gxm_viewport_enable(1); }\n'
        t=t.replace(ol,gate)
        print("gated block occurrences:",c)
# also the inline one with different spacing
oldt='p5d::vulkan::set_gxm_viewport(0.0f,960.0f,0.0f,544.0f,0.0f,1.0f); p5d::vulkan::set_gxm_viewport_enable(1);'
c=t.count(oldt)
print("inline2:",c)
if c:
    t=t.replace(oldt,'if (getenv("P5D_FORCEVP")) { p5d::vulkan::set_gxm_viewport(0.0f,960.0f,0.0f,544.0f,0.0f,1.0f); p5d::vulkan::set_gxm_viewport_enable(1); }')
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("done")
