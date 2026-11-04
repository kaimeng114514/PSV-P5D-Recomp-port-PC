import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
# fix _ps.attrs -> _av.size() in precomputed patch
t=t.replace("unsigned _na=0; for(size_t _i=0;_i<_ps.attrs.size() && _i<8;++_i) _na++;",
            "unsigned _na=(unsigned)_av.size(); if(_na>8u) _na=8u;",1)
# content branch override before upload
anchor='''            int _rr = p5d::vulkan::upload_vertex_stream(_vtx.data(), (unsigned)(_vc*128u), _vc);'''
assert anchor in t
new='''            if (_cpub) {
              std::string _vp = std::string(p5d_bridge_frag_dir()) + "/bridge_vert.spv";
              std::string _fp = std::string(p5d_bridge_frag_dir()) + (_haztex ? "/bridge_tex_frag.spv" : "/bridge_flat_frag.spv");
              (void)p5d::vulkan::select_real_pipeline(_vp.c_str(), _fp.c_str());
              static int _bn=0; if(_bn++<24){ printf("[CPUVP] bridge select rc done tex=%d n=%u\\n",(int)_haztex,g_main_n); fflush(stdout);} }
            int _rr = p5d::vulkan::upload_vertex_stream(_vtx.data(), (unsigned)(_vc*128u), _vc);'''
t=t.replace(anchor,new,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("fixed")
