import io
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
old='''            std::vector<float> _vtx_x; unsigned _vc_x = _vc;
            p5d_topology_expand(_vtx, _vc, _ord, _prim, _vtx_x, _vc_x);
            if (_vc_x == 0u) { _vtx_x = _vtx; _vc_x = _vc; }   // unknown topology: keep old behaviour'''
assert old in t
new='''            std::vector<float> _vtx_x; unsigned _vc_x = _vc;
            { static int _tg=-1; if(_tg<0){ const char* _e=getenv("P5D_TOPOLOGY"); _tg=(_e&&atoi(_e))?1:0; }
              if (_tg) { p5d_topology_expand(_vtx, _vc, _ord, _prim, _vtx_x, _vc_x);
                if (_vc_x == 0u) { _vtx_x = _vtx; _vc_x = _vc; } }
              else { _vtx_x = _vtx; _vc_x = _vc; } }'''
t=t.replace(old,new,1)
# also correct the final content draw call to _vc_x
i=t.find('int _rr = p5d::vulkan::upload_vertex_stream(_vtx_x.data()')
assert i>0
j=t.find('p5d::vulkan::gxm_draw_arrays(_vc);', i+2000)
# find the LAST occurrence after content upload (skip TPRD one which is _vc_x already)
j2=t.find('p5d::vulkan::gxm_draw_arrays(_vc);', t.find('p5d::vulkan::gxm_draw_arrays(_vc_x);', i))
if j2>0:
    t=t[:j2]+'p5d::vulkan::gxm_draw_arrays(_vc_x);'+t[j2+len('p5d::vulkan::gxm_draw_arrays(_vc);'):]
    print("final content draw now _vc_x")
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("topology gated")
