import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
old='''                  uint32_t _ubuf[14]; std::memset(_ubuf,0,sizeof(_ubuf));
                  uint32_t _def = _ufd_vert_ubo;
                  if(!_def && _pend_vout){ uint8_t* _q=GP(_pend_vout); if(_q) std::memcpy(&_def,_q,4); }'''
assert old in t
new='''                  uint32_t _ubuf[14]; std::memset(_ubuf,0,sizeof(_ubuf));
                  _ubuf[0] = _cur_v_ubo;   // M1316: GXP ubinfo reside=0 -> user uniform buffer 0
                  uint32_t _def = _ufd_vert_ubo;
                  if(!_def && _pend_vout){ uint8_t* _q=GP(_pend_vout); if(_q) std::memcpy(&_def,_q,4); }'''
t=t.replace(old,new,1)
# also print _cur_v_ubo in USSE header line
old2='printf("[USSE] #%d obj=%08X gxp=%08X vspv=%s vc=%u attrN=%u ufd_v=%08X pend_v=%08X\\n",\n                     _un,_cur_vobj,_gxp,_abi_vspv.c_str(),_vc,(unsigned)_av.size(),_ufd_vert_ubo,_pend_vout);'
assert old2 in t
new2='printf("[USSE] #%d obj=%08X gxp=%08X vspv=%s vc=%u attrN=%u ufd_v=%08X pend_v=%08X cur_v_ubo=%08X\\n",\n                     _un,_cur_vobj,_gxp,_abi_vspv.c_str(),_vc,(unsigned)_av.size(),_ufd_vert_ubo,_pend_vout,_cur_v_ubo);'
t=t.replace(old2,new2,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("patched")
