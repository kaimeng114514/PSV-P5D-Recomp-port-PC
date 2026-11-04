import io
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
old='''    out[0] = vi.o[0].f; out[1] = vi.o[1].f; out[2] = vi.o[2].f; out[3] = vi.o[3].f;
    if (vl->o_color0 >= 0 && vl->o_color0 + 3 < 160)
      for (int c = 0; c < 4; ++c) out[4+c] = vi.o[vl->o_color0 + c].f;
    if (vl->o_tex[0] >= 0 && vl->ncomp_tex[0] >= 2 && vl->o_tex[0] + 1 < 160) {
      out[8] = vi.o[vl->o_tex[0]].f; out[9] = vi.o[vl->o_tex[0] + 1].f;
      out[10] = 0.0f; out[11] = 1.0f;
    }'''
assert old in t
new='''    out[0] = vi.o[0].f; out[1] = vi.o[1].f; out[2] = vi.o[2].f; out[3] = vi.o[3].f;
    // M1318: keep the engine's own per-vertex colour / texcoord attributes (the shader may pack the
    // colour into O[4] with a non-obvious lane mapping, while the guest stream is authoritative).
    if (vl->nattrs < 2) { out[4]=0.0f; out[5]=0.0f; out[6]=0.0f; out[7]=1.0f; }
    if (vl->nattrs < 3) { out[8]=0.0f; out[9]=0.0f; out[10]=0.0f; out[11]=1.0f; }'''
t=t.replace(old,new,1)
# log all CPUVP bridge lines with preserved colour/uv
t=t.replace('if(_bn++<24){ printf("[CPUVP] bridge select texfrag=%d','if(_bn++<64){ printf("[CPUVP] bridge select texfrag=%d',1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("patched preserve attrs")
