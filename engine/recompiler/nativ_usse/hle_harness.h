/* Harness shim for Project Nativ's USSE decoder/interpreter (GPL-2+).
 * Replaces nativ's recompiler/rt.h guest-memory macros with calls into the P5D harness. */
#ifndef USSE_HARNESS_HLE_H
#define USSE_HARNESS_HLE_H
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
uint8_t *p5d_usse_G(uint32_t va);
uint32_t p5d_usse_rd32(uint32_t va);
uint16_t p5d_usse_rd16(uint32_t va);
uint8_t  p5d_usse_rd8(uint32_t va);
uint64_t p5d_usse_rd64(uint32_t va);
void     p5d_usse_wr32(uint32_t va, uint32_t v);
void     p5d_usse_wr16(uint32_t va, uint16_t v);
void     p5d_usse_wr8(uint32_t va, uint8_t v);
#ifdef __cplusplus
}
#endif

#define G(a)   p5d_usse_G((uint32_t)(a))
#define RD32(a) p5d_usse_rd32((uint32_t)(a))
#define RD16(a) p5d_usse_rd16((uint32_t)(a))
#define RD8(a)  p5d_usse_rd8((uint32_t)(a))
#define RD64(a) p5d_usse_rd64((uint32_t)(a))
#define WR32(a,v) p5d_usse_wr32((uint32_t)(a),(uint32_t)(v))
#define WR16(a,v) p5d_usse_wr16((uint32_t)(a),(uint16_t)(v))
#define WR8(a,v)  p5d_usse_wr8((uint32_t)(a),(uint8_t)(v))

static inline uint32_t guest_alloc(uint32_t n) { (void)n; return 0; }
#endif
