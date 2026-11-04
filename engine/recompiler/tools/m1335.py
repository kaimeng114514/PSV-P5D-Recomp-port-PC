import io
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
# 1) content branch: choose tex frag only for the known textured program; log transformed verts
old='''            if (_cpub) {
              std::string _vp = std::string(p5d_bridge_frag_dir()) + "/bridge_vert.spv";
              std::string _fp = std::string(p5d_bridge_frag_dir()) + (_haztex ? "/bridge_tex_frag.spv" : "/bridge_flat_frag.spv");
              (void)p5d::vulkan::select_real_pipeline(_vp.c_str(), _fp.c_str());
              static int _bn=0; if(_bn++<24){ printf("[CPUVP] bridge select rc done tex=%d n=%u\\n",(int)_haztex,g_main_n); fflush(stdout);} }'''
assert old in t
new='''            if (_cpub) {
              bool _texfrag = (_abi_fspv.find("0084") != std::string::npos);
              std::string _vp = std::string(p5d_bridge_frag_dir()) + "/bridge_vert.spv";
              std::string _fp = std::string(p5d_bridge_frag_dir()) + (_texfrag ? "/bridge_tex_frag.spv" : "/bridge_flat_frag.spv");
              (void)p5d::vulkan::select_real_pipeline(_vp.c_str(), _fp.c_str());
              static int _bn=0; if(_bn++<24){ printf("[CPUVP] bridge select texfrag=%d n=%u vc=%u",(int)_texfrag,g_main_n,_vc);
                for(unsigned _v=0;_v<_vc && _v<4;++_v) printf(" | v%u clip=(%.3f,%.3f,%.3f,%.3f) col=(%.3f,%.3f,%.3f,%.3f) uv=(%.3f,%.3f)",_v,
                  _vtx[(size_t)_v*32u],_vtx[(size_t)_v*32u+1],_vtx[(size_t)_v*32u+2],_vtx[(size_t)_v*32u+3],
                  _vtx[(size_t)_v*32u+4],_vtx[(size_t)_v*32u+5],_vtx[(size_t)_v*32u+6],_vtx[(size_t)_v*32u+7],
                  _vtx[(size_t)_v*32u+8],_vtx[(size_t)_v*32u+9]);
                printf("\\n"); fflush(stdout);} }'''
t=t.replace(old,new,1)
# 2) precomputed branch: same selection change
old2='''              if (_cpub) {
                std::string _vp = std::string(p5d_bridge_frag_dir()) + "/bridge_vert.spv";
                std::string _fp = std::string(p5d_bridge_frag_dir()) + (_haztex ? "/bridge_tex_frag.spv" : "/bridge_flat_frag.spv");
                (void)p5d::vulkan::select_real_pipeline(_vp.c_str(), _fp.c_str());
              } else if'''
assert old2 in t
new2='''              if (_cpub) {
                bool _texfrag = (_abi_fspv.find("0084") != std::string::npos);
                std::string _vp = std::string(p5d_bridge_frag_dir()) + "/bridge_vert.spv";
                std::string _fp = std::string(p5d_bridge_frag_dir()) + (_texfrag ? "/bridge_tex_frag.spv" : "/bridge_flat_frag.spv");
                (void)p5d::vulkan::select_real_pipeline(_vp.c_str(), _fp.c_str());
              } else if'''
t=t.replace(old2,new2,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("patched frag selection")
