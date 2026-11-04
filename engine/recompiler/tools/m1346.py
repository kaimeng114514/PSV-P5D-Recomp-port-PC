import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
B=r"C:\dev\p5dsrc\src\gxm\vulkan_backend.cpp"
t=io.open(B,encoding="utf-8",errors="replace").read()
old='''void gxm_draw_arrays(SceUInt32 vertex_count) {
    VkCommandBuffer cmd = g_backend.current_cmd_buffer;
    if (!cmd) { LOGE("gxm_draw_arrays: no current command buffer"); return; }'''
assert old in t
new='''void gxm_draw_arrays(SceUInt32 vertex_count) {
    VkCommandBuffer cmd = g_backend.current_cmd_buffer;
    if (!cmd) { LOGE("gxm_draw_arrays: no current command buffer"); return; }
    // M1326: correlate every host-issued draw with the uploaded vertex data and the pass target.
    if (g_draw_call_count < 80) {
      const float* _v = (const float*)g_backend.vertex_buffer_mapped;
      std::printf("[GXMD] #%ld vc=%u cmd=%p pipe=%p inRP=%d passCD=%08X", g_draw_call_count, (unsigned)vertex_count,
                  (void*)cmd, (void*)g_current_real_pipe, (int)g_backend.in_render_pass, (unsigned)g_dbg_pass_color);
      if (_v) for (int _k = 0; _k < 3 && (SceUInt32)_k < vertex_count; ++_k)
        std::printf(" v%d=(%.2f,%.2f,%.2f,%.2f)", _k, _v[_k*32+0], _v[_k*32+1], _v[_k*32+2], _v[_k*32+3]);
      std::printf("\\n"); std::fflush(stdout);
    }'''
t=t.replace(old,new,1)
io.open(B,"w",encoding="utf-8",newline="\n").write(t)
print("GXMD log added")
