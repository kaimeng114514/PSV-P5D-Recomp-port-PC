import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
B=r"C:\dev\p5dsrc\src\gxm\vulkan_backend.cpp"
t=io.open(B,encoding="utf-8",errors="replace").read()
old='''    VkPipeline _draw_pipe = g_current_real_pipe ? g_current_real_pipe
                          : *reinterpret_cast<VkPipeline*>(&g_backend.graphics_pipeline);
    if (!_draw_pipe) { LOGE("gxm_draw_arrays: no graphics pipeline bound"); return; }'''
assert old in t
new='''    // M1319: the old "fallback to the hard-coded test_triangle pipeline" drew non-original content
    // (a red/green/blue gradient triangle) whenever no real pipeline was selected.  Log it, and skip
    // the draw entirely unless P5D_TESTFALLBACK=1 (diagnostic only).
    static int _fb = -1; if (_fb < 0) { const char* _e = getenv("P5D_TESTFALLBACK"); _fb = (_e && atoi(_e)) ? 1 : 0; }
    if (!g_current_real_pipe) {
      static int _np = 0; if (_np++ < 48) { printf("[NOPIPE] draw vc=%u -> test_triangle fallback %s\\n", (unsigned)vertex_count, _fb ? "DRAWN" : "SKIPPED"); fflush(stdout); }
      if (!_fb) return; }
    VkPipeline _draw_pipe = g_current_real_pipe ? g_current_real_pipe
                          : *reinterpret_cast<VkPipeline*>(&g_backend.graphics_pipeline);
    if (!_draw_pipe) { LOGE("gxm_draw_arrays: no graphics pipeline bound"); return; }'''
t=t.replace(old,new,1)
io.open(B,"w",encoding="utf-8",newline="\n").write(t)
print("backend patched NOPIPE")
