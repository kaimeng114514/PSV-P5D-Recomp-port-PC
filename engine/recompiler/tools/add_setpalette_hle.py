import io,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
H=r"C:\dev\tmp\recomp_split\recomp_harness.cpp"
t=io.open(H,encoding="utf-8",errors="replace").read()
if "ABIPAL" in t: print("already"); sys.exit(0)
anchor='''        s->r[0] = 0; return s->r[14];
      }
      // 18.4509 M964: REAL sceGxmSetUniformDataF'''
assert anchor in t
ins='''        s->r[0] = 0; return s->r[14];
      }
      // M1306: sceGxmTextureSetPalette(texture, paletteData) - must write palette_addr (=va>>6) into control word 3 bits 0..25
      if (strncmp(_ge->name, "sceGxmTextureSetPalette", 23) == 0) {
        uint32_t _tex = s->r[0], _pal = s->r[1]; uint8_t* _q = GP(_tex);
        if (_q && _pal) { uint32_t _w3 = 0; std::memcpy(&_w3, _q + 12, 4);
          _w3 = (_w3 & 0xFC000000u) | ((_pal >> 6) & 0x03FFFFFFu); std::memcpy(_q + 12, &_w3, 4);
          static int _pn = 0; if (_pn++ < 12) { printf("[ABIPAL] tex=%08X pal=%08X -> w3=%08X n=%u\\n", _tex, _pal, _w3, g_main_n); fflush(stdout); } }
        s->r[0] = 0; return s->r[14];
      }
      // 18.4509 M964: REAL sceGxmSetUniformDataF'''
t=t.replace(anchor, ins, 1)
io.open(H,"w",encoding="utf-8",newline="\n").write(t)
print("ABIPAL handler installed")
