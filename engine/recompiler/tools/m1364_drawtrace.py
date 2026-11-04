import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
B=r"C:\dev\p5dsrc\src\gxm\vulkan_backend.cpp"
t=io.open(B,encoding="utf-8",errors="replace").read()
if "P5D_DRAWTRACE" not in t:
    old='''void cmd_draw(VkCommandBuffer cmd, SceUInt32 vertex_count, SceUInt32 instance_count,
              SceUInt32 first_vertex, SceUInt32 first_instance) {
    if (!cmd) return;'''
    assert old in t, "cmd_draw anchor"
    new=old+'''
    { static int _dt=-1; if(_dt<0) _dt = std::getenv("P5D_DRAWTRACE")?1:0;
      if(_dt){ static long _n=0; if(_n<80){ ++_n; std::printf("[DRAWTR] #%ld vc=%u pipe=%p inRP=%d passCD=%08X\\n",_n,(unsigned)vertex_count,(void*)g_current_real_pipe,(int)g_backend.in_render_pass,(unsigned)g_dbg_pass_color); std::fflush(stdout);} } }'''
    t=t.replace(old,new,1)
    old2='''void cmd_draw_indexed(VkCommandBuffer cmd, SceUInt32 index_count, SceUInt32 instance_count,
                      SceUInt32 first_index, SceInt32 vertex_offset, SceUInt32 first_instance) {
    if (!cmd) return;'''
    assert old2 in t, "cmd_draw_indexed anchor"
    new2=old2+'''
    { static int _dt2=-1; if(_dt2<0) _dt2 = std::getenv("P5D_DRAWTRACE")?1:0;
      if(_dt2){ static long _n2=0; if(_n2<80){ ++_n2; std::printf("[DRAWTR-IDX] #%ld ic=%u pipe=%p inRP=%d passCD=%08X\\n",_n2,(unsigned)index_count,(void*)g_current_real_pipe,(int)g_backend.in_render_pass,(unsigned)g_dbg_pass_color); std::fflush(stdout);} } }'''
    t=t.replace(old2,new2,1)
    io.open(B,"w",encoding="utf-8",newline="\n").write(t)
    print("drawtrace added")
else: print("already")
