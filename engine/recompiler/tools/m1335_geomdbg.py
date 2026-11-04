import io,sys,re
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
h=io.open(H,encoding="utf-8",errors="replace").read()
if "p5d_geom_dbg" not in h:
    i=h.find('extern "C" {')
    assert i>0
    impl='''extern "C" void p5d_geom_dbg(int tag, float a, float b, float c, float d,
                             float e, float f, float g, float h2) {
  static int _n1=0,_n2=0;
  int* n = (tag==1)?&_n1:&_n2;
  if (*n < 8) { ++*n;
    std::printf("[GEOMDBG] tag=%d n=%d v=(%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f)\\n",
                tag,*n,a,b,c,d,e,f,g,h2); std::fflush(stdout); }
}
'''
    h=h[:i]+impl+h[i:]
    io.open(H,"w",encoding="utf-8",newline="\n").write(h)
    print("harness geom dbg added")
else: print("harness already has geom dbg")

P=r"C:\dev\tmp\recomp_split\batch_010.cpp"
t=io.open(P,encoding="utf-8",errors="replace").read()
if "p5d_geom_dbg" not in t:
    t=t.replace('#include <cstdlib>','#include <cstdlib>\nextern "C" void p5d_geom_dbg(int,float,float,float,float,float,float,float,float);',1)
    old1='vfp_s_set(s, 8, vfp_s_get(s,19)); wr32(s, s->r[13] + 0x28u + 0x0u, vfp_s_bits(s, 2));'
    assert old1 in t, "old1 missing"
    new1='vfp_s_set(s, 8, vfp_s_get(s,19)); p5d_geom_dbg(1, vfp_sf(s,0),vfp_sf(s,1),vfp_sf(s,2),vfp_sf(s,3),vfp_sf(s,4),vfp_sf(s,5),vfp_sf(s,6),vfp_sf(s,7)); wr32(s, s->r[13] + 0x28u + 0x0u, vfp_s_bits(s, 2));'
    t=t.replace(old1,new1,1)
    old2='vfp_s_bits_set(s, 19, 0x3F800000u); wr32(s, s->r[13] + 0x14u + 0x0u, vfp_s_bits(s, 3));'
    assert old2 in t, "old2 missing"
    new2='vfp_s_bits_set(s, 19, 0x3F800000u); p5d_geom_dbg(2, vfp_sf(s,1),vfp_sf(s,2),vfp_sf(s,3),vfp_sf(s,4),vfp_sf(s,19),vfp_sf(s,20),vfp_sf(s,21),vfp_sf(s,22)); wr32(s, s->r[13] + 0x14u + 0x0u, vfp_s_bits(s, 3));'
    t=t.replace(old2,new2,1)
    io.open(P,"w",encoding="utf-8",newline="\n").write(t)
    print("batch_010 probes added")
else: print("batch already patched")
