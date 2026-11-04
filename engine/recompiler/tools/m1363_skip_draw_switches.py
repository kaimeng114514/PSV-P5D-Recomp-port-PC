import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
old='if (_cd && (!getenv("P5D_ONLYTEX") || _abi_fspv.find("0084") != std::string::npos)) {'
assert old in t
new='if (_cd && (!getenv("P5D_ONLYTEX") || _abi_fspv.find("0084") != std::string::npos) && !getenv("P5D_SKIPALLDRAW") && !(getenv("P5D_SKIPTEXDRAW") && _abi_fspv.find("0084") != std::string::npos)) {'
t=t.replace(old,new,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("skip switches added")
