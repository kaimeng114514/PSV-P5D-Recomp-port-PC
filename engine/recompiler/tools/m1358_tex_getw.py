import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
P=r"C:\dev\tmp\recomp_split\gxm_abi.cpp"
t=io.open(P,encoding="utf-8",errors="replace").read()
old1='''extern "C" uint32_t gxm_abi_call_126A3EB3(uint32_t* regs, uint32_t* stack_args) {
    // guest ABI:   unsigned int sceGxmTextureGetWidth(const SceGxmTexture * texture)
    // backend ABI: SceInt32 sceGxmTextureGetWidth(void)   [runtime has a `(void)` placeholder / STUB_LOG only]
    (void)regs;
    (void)stack_args;
#if GXM_ABI_CALL_STUB_IMPLS
    return (uint32_t)sceGxmTextureGetWidth();   // logs [STUB] via STUB_LOG
#else
    return 0u;   // same observable value as the current stub
#endif
}'''
assert old1 in t
new1='''extern "C" uint32_t gxm_abi_call_126A3EB3(uint32_t* regs, uint32_t* stack_args) {
    // guest ABI:   unsigned int sceGxmTextureGetWidth(const SceGxmTexture * texture)
    // M1327: REAL implementation - decode the 16-byte GXM texture control block.
    //   control word 1 bits 12..23 = (width - 1), bits 0..11 = (height - 1)
    // (same formula the harness TEXDUMP uses: w=((w1>>12)&0xFFF)+1, h=(w1&0xFFF)+1)
    (void)stack_args;
    uint8_t* p = reinterpret_cast<uint8_t*>(gxm_abi_xlate(regs[0]));
    if (!p) return 0u;
    uint32_t w1 = 0; std::memcpy(&w1, p + 4, 4);
    return ((w1 >> 12) & 0xFFFu) + 1u;
}'''
t=t.replace(old1,new1,1)
old2='''extern "C" uint32_t gxm_abi_call_5420A086(uint32_t* regs, uint32_t* stack_args) {
    // guest ABI:   unsigned int sceGxmTextureGetHeight(const SceGxmTexture * texture)
    // backend ABI: SceInt32 sceGxmTextureGetHeight(void)   [runtime has a `(void)` placeholder / STUB_LOG only]
    (void)regs;
    (void)stack_args;
#if GXM_ABI_CALL_STUB_IMPLS
    return (uint32_t)sceGxmTextureGetHeight();   // logs [STUB] via STUB_LOG
#else
    return 0u;   // same observable value as the current stub
#endif
}'''
assert old2 in t
new2='''extern "C" uint32_t gxm_abi_call_5420A086(uint32_t* regs, uint32_t* stack_args) {
    // guest ABI:   unsigned int sceGxmTextureGetHeight(const SceGxmTexture * texture)
    // M1327: REAL implementation - decode the 16-byte GXM texture control block (bits 0..11 = height-1).
    (void)stack_args;
    uint8_t* p = reinterpret_cast<uint8_t*>(gxm_abi_xlate(regs[0]));
    if (!p) return 0u;
    uint32_t w1 = 0; std::memcpy(&w1, p + 4, 4);
    return (w1 & 0xFFFu) + 1u;
}'''
t=t.replace(old2,new2,1)
io.open(P,"w",encoding="utf-8",newline="\n").write(t)
print("gxm_abi TextureGetWidth/Height real impl added")
