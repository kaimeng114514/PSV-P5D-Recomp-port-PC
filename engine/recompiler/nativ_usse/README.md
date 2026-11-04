# nativ USSE decoder / SPIR-V translator (third-party, GPL-2+)

These files are taken from **Project Nativ** by TheLostPantheon (https://github.com/TheLostPantheon/nativ),
`runtime/gfx/`:

- `usse.c`, `usse.h`, `usse_internal.h` – USSE (Vita GPU) decoder and CPU interpreter
- `usse_spirv.c`, `usse_spirv.h` – USSE -> SPIR-V translator
- `docs/usse-isa.md` in that project is the instruction-set reference these implement

License: GNU General Public License version 2 or later (same as this repository; see the project's LICENSE).
No game code or game data is contained here.

Local modifications (this repository only):

- `hle.h` – a small shim that replaces nativ's `hle.h`/`recompiler/rt.h` guest-memory macros
  (`G/RD8/RD16/RD32/RD64/WR32` + `recomp_mem`) so the decoder and translator build as a standalone tool.
- `usseconv_main.c` – command-line wrapper: `usseconv dump <file.gxp>` disassembles a GXP and prints its
  layouts/instructions; `usseconv spv <file.gxp> <out.spv>` converts it to a SPIR-V module.

Build (MinGW):

```
cd engine/recompiler/nativ_usse
gcc -O2 -I. -o usseconv.exe usseconv_main.c usse.c usse_spirv.c -lm
```

`usseconv spv` uses canonical varyings (`usse_spirv_set_canonical_varyings(1)`) so each GXP can be converted
independently, and passes CPU-packed PA records (`vf = NULL`) as the vertex-fetch mode, which matches this
port's plan of packing guest attributes into PA records at draw time.
