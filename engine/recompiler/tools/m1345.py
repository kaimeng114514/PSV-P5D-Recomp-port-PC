import io
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
old='''  // skip the fullscreen clear-style programs (no colour output, single position attribute)
  if (vl->o_color0 < 0 && vl->nattrs <= 1 && vl->index_reg < 0) return false;'''
assert old in t
new='''  // M1325: clear-style programs (single position attribute, no colour output) ARE bridged: their
  // primary only computes the clip position and the fragment is the flat bridge with an engine
  // clear colour of black (0,0,0,1); the old SPV clear pipeline rendered a bogus gradient triangle.
  const bool _clearish = (vl->o_color0 < 0 && vl->nattrs <= 1);'''
t=t.replace(old,new,1)
old2='''    // M1318: keep the engine's own per-vertex colour / texcoord attributes (the shader may pack the
    // colour into O[4] with a non-obvious lane mapping, while the guest stream is authoritative).
    if (vl->nattrs < 2) { out[4]=0.0f; out[5]=0.0f; out[6]=0.0f; out[7]=1.0f; }
    if (vl->nattrs < 3) { out[8]=0.0f; out[9]=0.0f; out[10]=0.0f; out[11]=1.0f; }'''
assert old2 in t
new2='''    // M1318: keep the engine's own per-vertex colour / texcoord attributes (the shader may pack the
    // colour into O[4] with a non-obvious lane mapping, while the guest stream is authoritative).
    if (_clearish || vl->nattrs < 2) { out[4]=0.0f; out[5]=0.0f; out[6]=0.0f; out[7]=1.0f; }
    if (vl->nattrs < 3) { out[8]=0.0f; out[9]=0.0f; out[10]=0.0f; out[11]=1.0f; }'''
t=t.replace(old2,new2,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("clear draws bridged")
