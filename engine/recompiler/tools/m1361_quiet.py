import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
t=t.replace("snQLOG(","snprintf(")
t=t.replace("fQLOG(","fprintf(")
t=t.replace("#include <cstdio>\nstatic int g_p5d_quiet = -1;",
            "#include <cstdio>\n#include <cstdlib>\nstatic int g_p5d_quiet = -1;")
t=t.replace('g_p5d_quiet = std::getenv("P5D_QUIET") ? 1 : 0;',
            'g_p5d_quiet = (::getenv("P5D_QUIET") != nullptr) ? 1 : 0;')
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("fixed")
