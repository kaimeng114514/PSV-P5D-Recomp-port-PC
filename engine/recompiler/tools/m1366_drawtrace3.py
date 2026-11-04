import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
B=r"C:\dev\p5dsrc\src\gxm\vulkan_backend.cpp"
t=io.open(B,encoding="utf-8",errors="replace").read()
if "DRAWTR3" not in t:
    old='''void gxm_draw_arrays(SceUInt32 vertex_count) {
    VkCommandBuffer cmd = g_backend.current_cmd_buffer;'''
    assert old in t
    new='''void gxm_draw_arrays(SceUInt32 vertex_count) {
    { static int _d3=-1; if(_d3<0) _d3 = std::getenv("P5D_DRAWTRACE3")?1:0;
      if(_d3){ static long _n3=0; if(_n3<15){ ++_n3; std::printf("[DRAWTR3] #%ld vc=%u ra=%p raofs=%lld\\n",_n3,(unsigned)vertex_count,__builtin_return_address(0),(long long)((char*)__builtin_return_address(0)-(char*)&gxm_draw_arrays)); std::fflush(stdout);} } }
    VkCommandBuffer cmd = g_backend.current_cmd_buffer;'''
    t=t.replace(old,new,1)
    io.open(B,"w",encoding="utf-8",newline="\n").write(t)
    print("DRAWTR3 added")
else: print("already")
