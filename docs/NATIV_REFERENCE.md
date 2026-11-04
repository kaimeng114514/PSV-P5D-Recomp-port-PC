# Project Nativ source reference (GPL-2), cloned for architecture study (NOT vendored into this repo)

Source: https://github.com/TheLostPantheon/nativ (GPL-2), cloned to C:\dev\nativ_src via gh-proxy mirror.
Reason: architectural reference for "recompile Sony's user-mode libraries instead of hand-writing libgxm HLE".

Key facts used in docs/P5D_MASTER.md section 18.4876:
- recompiler/analyze.py, recompiler/recomp.py, recompiler/rt.h : AOT ARM->C for game AND Sony user libs
- decrypt/nidmap.tsv : import NID map; decrypt/ : firmware decryption tools
- runtime/gfx/gxm_sony.c : observes libgxm state in guest memory -> API-neutral draws
- runtime/gfx/gfx_dispatch.c : dispatch to gfx_gpu.c (SDL3 GPU: Vulkan/Metal) or gfx_soft.c (USSE CPU interpreter)
- runtime/gfx/usse.c / usse_spirv.c : USSE interpreter + SPIR-V translation (we already ported these: sizes 127223 / 186952)
- packs/<title>/ : per-game manifest (releases by SHA-256, Sony libs to load, hooks); no game data
This repo contains no game/firmware code or data.
