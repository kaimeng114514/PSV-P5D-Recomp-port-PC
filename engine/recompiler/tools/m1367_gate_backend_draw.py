import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
P=r"C:\dev\tmp\recomp_split\gxm_abi.cpp"
t=io.open(P,encoding="utf-8",errors="replace").read()
if "P5D_ABIREAL_DRAW" not in t:
    if "#include <cstdlib>" not in t:
        t=t.replace("#include <cstdint>\n#include <cstring>","#include <cstdint>\n#include <cstring>\n#include <cstdlib>",1)
    old='''    int32_t rv = sceGxmDraw(p0, (uint32_t)a1, (uint32_t)a2, (uint32_t)a4, p3);
    return (uint32_t)rv;'''
    assert old in t
    new='''    // M1336: the harness owns all Vulkan drawing (bridge pipelines + CPUVP).  The backend's own
    // sceGxmDraw drives a SEPARATE GXM state machine whose vertex streams are never set up by the
    // harness, so it kept drawing a stale fullscreen test triangle - the "gradient triangle" that
    // polluted every frame even when all harness content draws were skipped.  Default: no backend draw.
    int32_t rv = 0;
    { static int _rd = -1; if (_rd < 0) _rd = (std::getenv("P5D_ABIREAL_DRAW") != nullptr) ? 1 : 0;
      if (_rd) rv = sceGxmDraw(p0, (uint32_t)a1, (uint32_t)a2, (uint32_t)a4, p3); }
    return (uint32_t)rv;'''
    t=t.replace(old,new,1)
    io.open(P,"w",encoding="utf-8",newline="\n").write(t)
    print("backend draw gated (default off)")
else: print("already")
