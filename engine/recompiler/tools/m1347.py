import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
B=r"C:\dev\p5dsrc\src\gxm\vulkan_backend.cpp"
t=io.open(B,encoding="utf-8",errors="replace").read()
old='''    // M1326: correlate every host-issued draw with the uploaded vertex data and the pass target.
    if (g_draw_call_count < 80) {'''
assert old in t
new='''    // M1326: correlate every host-issued draw with the uploaded vertex data and the pass target.
    // M1327: also sample a LATE frame window (P5D_GXMDFRAME=<frame>) to compare the loading-loop draws
    // against the first frame.
    static long _gxmdframe = -2; if (_gxmdframe == -2) { const char* _e = std::getenv("P5D_GXMDFRAME"); _gxmdframe = (_e && *_e) ? std::strtol(_e, nullptr, 10) : -1; }
    static long _late_n = 0;
    bool _gxlog = (g_draw_call_count < 80);
    if (_gxmdframe >= 0 && (long)g_backend.frame_count >= _gxmdframe && (long)g_backend.frame_count <= _gxmdframe + 1 && _late_n < 240) { _gxlog = true; ++_late_n; }
    if (_gxlog) {'''
t=t.replace(old,new,1)
t=t.replace('''      std::printf("[GXMD] #%ld vc=%u cmd=%p pipe=%p inRP=%d", g_draw_call_count, (unsigned)vertex_count,
                  (void*)cmd, (void*)g_current_real_pipe, (int)g_backend.in_render_pass);''',
'''      std::printf("[GXMD] fc=%llu #%ld vc=%u cmd=%p pipe=%p inRP=%d", (unsigned long long)g_backend.frame_count, g_draw_call_count, (unsigned)vertex_count,
                  (void*)cmd, (void*)g_current_real_pipe, (int)g_backend.in_render_pass);''',1)
io.open(B,"w",encoding="utf-8",newline="\n").write(t)
print("GXMD late window added")
