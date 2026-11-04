import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
if "TXTSTRM" in t: print("already"); sys.exit(0)
anchor='        auto _at = _vobj2attrs.find(_cur_vobj);'
assert anchor in t
ins='''        if (_abi_fspv.find("frag_0084") != std::string::npos) { static int _ts=0; if(_ts++<6){ printf("[TXTSTRM] #%d obj=%08X:", _ts, _cur_vobj);
          auto _stq=_vobj2strms.find(_cur_vobj);
          if(_stq!=_vobj2strms.end()) for(size_t _k=0;_k<_stq->second.size() && _k<4;++_k){ uint32_t _va=(_stm2va.count(_stq->second[_k].index)?_stm2va[_stq->second[_k].index]:0u);
            printf(" strm%zu{idx=%u stride=%u va=%08X bytes:", _k, _stq->second[_k].index, _stq->second[_k].stride, _va);
            for(int _i=0;_i<8;++_i){ uint8_t* _q=GP(_va+(uint32_t)_i); if(_q) printf(" %02X",*_q); }
            printf("}"); }
          printf(" stm2va_n=%zu\\n", _stm2va.size()); fflush(stdout);} }
'''
t=t.replace(anchor, ins+anchor,1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("TXTSTRM installed")
