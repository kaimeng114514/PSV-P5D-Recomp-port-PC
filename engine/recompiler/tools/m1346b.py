import io
B=r"C:\dev\p5dsrc\src\gxm\vulkan_backend.cpp"
t=io.open(B,encoding="utf-8",errors="replace").read()
t=t.replace('''      std::printf("[GXMD] #%ld vc=%u cmd=%p pipe=%p inRP=%d passCD=%08X", g_draw_call_count, (unsigned)vertex_count,
                  (void*)cmd, (void*)g_current_real_pipe, (int)g_backend.in_render_pass, (unsigned)g_dbg_pass_color);''',
'''      std::printf("[GXMD] #%ld vc=%u cmd=%p pipe=%p inRP=%d", g_draw_call_count, (unsigned)vertex_count,
                  (void*)cmd, (void*)g_current_real_pipe, (int)g_backend.in_render_pass);''',1)
io.open(B,"w",encoding="utf-8",newline="\n").write(t)
print("patched")
