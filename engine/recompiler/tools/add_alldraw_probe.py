import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
# 1) global map
anchor='static std::map<std::string,long> g_gxm_counts;'
assert anchor in t
if 'g_spv_tu' not in t:
    t=t.replace(anchor, anchor+'\nstatic std::map<std::string,uint32_t> g_spv_tu;   // M1304: spv name -> GXP texunit flags (from RegisterProgram)',1)
# 2) populate in GXPTEX
old='printf("[GXPTEX] gxp=%08X tu1=%08X tu2=%08X spv=%s\\n", _gxp, _tu1, _tu2, _it->second.c_str()); fflush(stdout); } }'
assert old in t
t=t.replace(old,'g_spv_tu[_it->second] = (_tu1|_tu2); printf("[GXPTEX] gxp=%08X tu1=%08X tu2=%08X spv=%s\\n", _gxp, _tu1, _tu2, _it->second.c_str()); fflush(stdout); } }',1)
# 3) ALLDRAW probe in draw branch after select_real_pipeline line
anchor2='        int32_t _rc = p5d::vulkan::select_real_pipeline(_vp.c_str(), _fp.c_str());'
assert anchor2 in t
ins='''        int32_t _rc = p5d::vulkan::select_real_pipeline(_vp.c_str(), _fp.c_str());
        { static int _ad=-1; if(_ad<0){ const char* e=getenv("P5D_ALLDRAWS"); _ad=(e&&atoi(e))?1:0; }
          if(_ad){ static int _an=0; if(_an++<400){ uint32_t _vt=0,_ft=0; auto _i1=g_spv_tu.find(_abi_vspv); if(_i1!=g_spv_tu.end()) _vt=_i1->second; auto _i2=g_spv_tu.find(_abi_fspv); if(_i2!=g_spv_tu.end()) _ft=_i2->second;
            printf("[ALLDRAW] #%d lr=%08X v=%s(tu=%X) f=%s(tu=%X) r1=%08X r2=%08X r3=%08X r4=%08X n=%u\\n", _an, s->r[14], _abi_vspv.c_str(), _vt, _abi_fspv.c_str(), _ft, s->r[1],s->r[2],s->r[3],s->r[4], g_main_n); fflush(stdout);} } }'''
t=t.replace(anchor2, ins,1)
# 4) log textured program becoming current
anchor3='if (_pfrag && _fm != _fobj2spv.end()) { if (_abi_fspv != _fm->second) { _abi_fspv = _fm->second;'
if anchor3 in t and 'TEXPROGCUR' not in t:
    t=t.replace(anchor3, anchor3+'''
              { auto _tq=g_spv_tu.find(_fm->second); static int _tq_n=0; if(_tq!=g_spv_tu.end() && _tq->second && _tq_n++<40){ printf("[TEXPROGCUR] frag spv=%s tu=%X lr=%08X n=%u\\n", _fm->second.c_str(), _tq->second, s->r[14], g_main_n); fflush(stdout);} }''',1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("ALLDRAW/TEXPROGCUR installed")
