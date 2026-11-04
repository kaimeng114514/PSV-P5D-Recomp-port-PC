import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
old='''uint32_t id_tbb(uint32_t insn_pc, uint32_t addr, RecState* s) {
  uint32_t off = addr - IMG_VA_BASE;
  uint32_t b = (off < IMG_SIZE) ? gp_img[off] : 0;
  uint32_t t = (((insn_pc + 4) & ~3u) + (b << 1)) & ~1u;'''
assert old in t, "id_tbb block not found"
new='''uint32_t id_tbb(uint32_t insn_pc, uint32_t addr, RecState* s) {
  // M1302 FIX: ARM TBB [PC,Rm] target = (PC_value) + 2*Byte[PC_value + Rm], where PC_value = insn_pc+4
  // (NO align4: compiler emits the byte table immediately after the 4-byte TBB at insn+4, unaligned).
  // Previous code aligned the base to 4, shifting every switch target by -2 when (insn+4)%4 != 0,
  // so many switch dispatches landed on the previous block (wrong case) or a no-op block.
  uint32_t off = addr - IMG_VA_BASE;
  uint32_t b = (off < IMG_SIZE) ? gp_img[off] : 0;
  uint32_t t = (insn_pc + 4u + (b << 1)) & ~1u;'''
t=t.replace(old,new,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("id_tbb FIX applied")
