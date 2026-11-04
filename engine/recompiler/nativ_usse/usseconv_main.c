/* Standalone GXP decoder / USSE disassembler / SPIR-V converter built on Project Nativ's
 * runtime/gfx/usse.c + usse_spirv.c (GPL-2+, see THIRD_PARTY.md).  Usage:
 *    usseconv dump <file.gxp>
 *    usseconv spv  <file.gxp> <out.spv>
 * The GXP is mapped at guest address 0 (project's memory shim, hle.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "hle.h"
#include "usse.h"
#include "usse_internal.h"
#include "usse_spirv.h"

uint8_t *recomp_mem = NULL;

static uint8_t *load_file(const char *path, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)calloc((size_t)n + 0x100, 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f); *size = (uint32_t)n; return b;
}

static const char *bk_name(int b) {
    static const char *n[] = {"TEMP","PA","SA","O","FPI","FPCONST","IMM","IDX","IDXD1","IDXD2","GLOBAL","SPECIAL","NONE"};
    return (b >= 0 && b < 13) ? n[b] : "?";
}
static const char *op_name(int op) {
    static const char *n[] = {"NOP","VMAD2","VNMAD","VMAD","VDP","VDUAL","VCOMP","VMOV","VPCK","VTST","VTSTMSK",
        "VBW","SOP2","SOP2M","SOP3","I8MAD","I16MAD","I32MAD","I32MAD2","SMP","BR","SMLSI","SMBO","KILL","LIMM",
        "DEPTHF","LDST","UNSUPPORTED"};
    return (op >= 0 && op < 28) ? n[op] : "?";
}
static void dump_operand(const UOperand *o) {
    if (!o) { printf("-"); return; }
    if (o->bank == BK_NONE) { printf("-"); return; }
    if (o->negabs & 1) printf("-");
    if (o->negabs & 2) printf("|");
    if (o->bank == BK_IMM) { printf("#%u", o->num); }
    else if (o->bank == BK_FPCONST) { printf("fc%u", o->num); }
    else printf("%s[%u].%u%u%u%u", bk_name(o->bank), o->num, o->swiz[0],o->swiz[1],o->swiz[2],o->swiz[3]);
}
static void dump_program(const char *path) {
    uint32_t size = 0; uint8_t *b = load_file(path, &size);
    if (!b) return;
    recomp_mem = b;
    int sane = usse_gxp_sane(b, size);
    printf("[GXP] %s size=%u sane=%d\n", path, size, sane);
    const UsseProgram *p = usse_get(0);
    int frag = usse_is_fragment(p);
    printf("[GXP] kind=%s supported=%d primary_reads_memory=%d pa_words=%d\n",
           frag ? "fragment" : "vertex", usse_supported(p), usse_primary_reads_memory(p), usse_program_pa_words(p));
    if (!frag) {
        const UsseVertexLayout *vl = usse_vertex_layout(p);
        printf("[VL] nattrs=%d index_reg=%d instance_reg=%d total_o=%d o_color0=%d o_color1=%d o_fog=%d o_psize=%d\n",
               vl->nattrs, vl->index_reg, vl->instance_reg, vl->total_o, vl->o_color0, vl->o_color1, vl->o_fog, vl->o_psize);
        for (int i = 0; i < vl->nattrs; ++i) {
            const UsseAttrSlot *a = &vl->attrs[i];
            printf("[VL]  attr%d reg=%d type=%u comps=%u words=%u array_size=%d untyped=%d\n",
                   i, a->reg, a->type, a->comps, a->words, a->array_size, a->untyped);
        }
        for (int t = 0; t < 10; ++t) if (vl->o_tex[t] >= 0) printf("[VL]  texcoord%d: O reg %d comps %d\n", t, vl->o_tex[t], vl->ncomp_tex[t]);
    } else {
        const UsseFragLayout *fl = usse_fragment_layout(p);
        printf("[FL] niters=%d native_color=%d frag_color_used=%d color_pa_start=%d color_type=%d comps=%d discard=%d\n",
               fl->niters, fl->native_color, fl->frag_color_used, fl->color_pa_start, fl->color_type, fl->color_comps, fl->discard_used);
        for (int i = 0; i < fl->niters; ++i) {
            const UsseIter *it = &fl->iters[i];
            printf("[FL]  iter%d is_tex=%d semantic=%d storage=%d comps=%d tex_unit=%d tex_proj=%d result_fmt=%d pa=%d point=%d\n",
                   i, it->is_tex, it->semantic, it->storage, it->comps, it->tex_unit, it->tex_proj, it->result_fmt, it->pa, it->point_coord);
        }
    }
    for (int sec = 0; sec < 2; ++sec) {
        int n = 0; const UOp *ops = usse_program_code(p, sec, &n);
        if (!ops || !n) continue;
        printf("[CODE %s] %d instructions\n", sec ? "secondary" : "primary", n);
        for (int i = 0; i < n; ++i) {
            const UOp *o = &ops[i];
            printf("  %3d %-9s mask=%X reps=%u rm=%u sub=%u,%u,%u dst=", i, op_name(o->op), o->mask, o->reps, o->repmode, o->sub,o->sub2,o->sub3);
            dump_operand(&o->dst); printf(" s0="); dump_operand(&o->s0); printf(" s1="); dump_operand(&o->s1);
            printf(" s2="); dump_operand(&o->s2); if (o->imm) printf(" imm=%d", o->imm);
            if (o->op == OP_BR) printf(" target=%d", o->args[0]);
            printf("\n");
        }
    }
    free(b); recomp_mem = NULL;
}
static int convert_spv(const char *path, const char *out) {
    uint32_t size = 0; uint8_t *b = load_file(path, &size);
    if (!b) return 1;
    recomp_mem = b;
    const UsseProgram *p = usse_get(0);
    int frag = usse_is_fragment(p);
    if (!usse_supported(p)) { fprintf(stderr, "%s: unsupported instruction\n", path); free(b); return 2; }
    usse_spirv_set_canonical_varyings(1);
    uint32_t sa_tex_unit[256]; memset(sa_tex_unit, 0, sizeof(sa_tex_unit));
    int smp_units[16]; int nsmp = 0;
    SpirvModule m = usse_spirv_translate(p, frag ? NULL : usse_vertex_layout(p), frag ? usse_fragment_layout(p) : NULL,
                                         sa_tex_unit, 16, smp_units, &nsmp, 0, 0, 0, NULL);
    if (!m.ok) {
        fprintf(stderr, "%s: translation failed: %s\n", path, m.err);
        usse_spirv_free(&m); free(b); return 3;
    }
    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); usse_spirv_free(&m); free(b); return 4; }
    fwrite(m.words, 4, m.nwords, f);
    fclose(f);
    printf("%s -> %s words=%u frag=%d fetch=%d pa_words=%d nsmp=%d smp_units=", path, out, m.nwords, frag, m.frag_fetch, m.pa_words, nsmp);
    for (int i = 0; i < nsmp; ++i) printf("%d,", smp_units[i]);
    printf("\n");
    usse_spirv_free(&m); free(b); recomp_mem = NULL;
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "dump") == 0) { dump_program(argv[2]); return 0; }
    if (argc == 4 && strcmp(argv[1], "spv") == 0) return convert_spv(argv[2], argv[3]);
    fprintf(stderr, "usage: %s dump <file.gxp> | spv <file.gxp> <out.spv>\n", argv[0]);
    return 64;
}
