import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()

anchor='''static const char* p5d_bridge_frag_dir() {
  static std::string s; static bool init = false;
  if (!init) { init = true; const char* e = getenv("P5D_BRIDGE_DIR"); s = e ? e : "C:/dev/p5d_build/spv_nativ"; }
  return s.c_str();
}
'''
assert anchor in t
helper='''
// ===== M1321: GXM primitive topology -> Vulkan triangle list =====
// The bridge/old pipelines are created with VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, while the engine's
// quads arrive as GXM triangle STRIPS (4 vertices) / fans; drawing a 4-vertex strip as a list shows
// only one triangle.  Expand indexed / strip / fan draws on the CPU into a plain triangle list.
static void p5d_topology_expand(const std::vector<float>& vtx, unsigned vc,
                                const std::vector<unsigned>& ord, uint32_t prim,
                                std::vector<float>& out, unsigned& outn) {
  out.clear(); outn = 0;
  unsigned n = ord.empty() ? vc : (unsigned)ord.size();
  auto vid = [&](unsigned k) -> unsigned { return ord.empty() ? k : ord[k]; };
  auto push = [&](unsigned a) { if ((size_t)a * 32u + 32u <= vtx.size()) out.insert(out.end(), vtx.begin() + (size_t)a*32u, vtx.begin() + (size_t)a*32u + 32u); };
  if (prim == 0x0C000000u) {                 // SCE_GXM_PRIMITIVE_TRIANGLE_STRIP
    for (unsigned i = 0; i + 2u < n; ++i) {
      unsigned a = vid(i), b = vid(i+1), c = vid(i+2);
      if (i & 1u) { unsigned t = a; a = b; b = t; }
      push(a); push(b); push(c);
    }
    outn = (n >= 3u) ? (n - 2u) * 3u : 0u;
  } else if (prim == 0x10000000u) {          // SCE_GXM_PRIMITIVE_TRIANGLE_FAN
    for (unsigned i = 1; i + 1u < n; ++i) { push(vid(0)); push(vid(i)); push(vid(i+1)); }
    outn = (n >= 3u) ? (n - 2u) * 3u : 0u;
  } else {                                    // TRIANGLES / default: reorder only
    for (unsigned k = 0; k < n; ++k) push(vid(k));
    outn = n;
  }
}
'''
t=t.replace(anchor, anchor+helper, 1)

old_decl='''        uint32_t _icount = _stk[0];   // sceGxmDraw(ctx,prim,indexType,indexData,indexCount): indexCount = first stack arg
        uint32_t _vc = _icount;'''
assert old_decl in t
new_decl='''        uint32_t _icount = _stk[0];   // sceGxmDraw(ctx,prim,indexType,indexData,indexCount): indexCount = first stack arg
        uint32_t _vc = _icount;
        const uint32_t _prim = s->r[1]; const uint32_t _itype2 = s->r[2]; const uint32_t _idata2 = s->r[3];   // M1321 topology
        std::vector<unsigned> _ord;   // M1321: vertex order (indexed draws reorder the packed vertices)'''
t=t.replace(old_decl,new_decl,1)

anchor_attrs='''        auto _at = _vobj2attrs.find(_cur_vobj);'''
assert anchor_attrs in t
add_ord='''        // M1321: record the index order (for indexed draws) / identity order (non-indexed).
        if (_icount > 0u && _idata2) {
          for (uint32_t _k = 0; _k < _icount && _k < 65536u; ++_k) {
            uint32_t _ix = 0;
            if (_itype2 == 0u) { uint16_t _v16 = 0; uint8_t* _q = GP(_idata2 + _k*2u); if (!_q) break; std::memcpy(&_v16,_q,2); _ix = _v16; }
            else { uint8_t* _q = GP(_idata2 + _k*4u); if (!_q) break; std::memcpy(&_ix,_q,4); }
            _ord.push_back(_ix);
          }
        } else {
          for (uint32_t _k = 0; _k < _vc; ++_k) _ord.push_back(_k);
        }
        auto _at = _vobj2attrs.find(_cur_vobj);'''
t=t.replace(anchor_attrs, add_ord, 1)

old_upload='''            int _rr = p5d::vulkan::upload_vertex_stream(_vtx.data(), (unsigned)(_vc*128u), _vc);'''
assert old_upload in t
new_upload='''            std::vector<float> _vtx_x; unsigned _vc_x = _vc;
            p5d_topology_expand(_vtx, _vc, _ord, _prim, _vtx_x, _vc_x);
            if (_vc_x == 0u) { _vtx_x = _vtx; _vc_x = _vc; }   // unknown topology: keep old behaviour
            int _rr = p5d::vulkan::upload_vertex_stream(_vtx_x.data(), (unsigned)(_vc_x*128u), _vc_x);'''
t=t.replace(old_upload,new_upload,1)
i=t.find(new_upload)
j=t.find('p5d::vulkan::gxm_draw_arrays(_vc);', i)
assert j>0
t=t[:j]+'p5d::vulkan::gxm_draw_arrays(_vc_x);'+t[j+len('p5d::vulkan::gxm_draw_arrays(_vc);'):]
# add a topology log in the DRAWLOG line
t=t.replace('printf("[DRAWLOG] #%d lr=%08X vspv=%s fspv=%s vc=%u avN=%u tex=%d','printf("[DRAWLOG] #%d lr=%08X prim=%08X vspv=%s fspv=%s vc=%u avN=%u tex=%d',1)
t=t.replace('_dn, s->r[14], _abi_vspv.c_str(), _abi_fspv.c_str(), _vc,','_dn, s->r[14], s->r[1], _abi_vspv.c_str(), _abi_fspv.c_str(), _vc,',1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("topology expansion patched OK")
