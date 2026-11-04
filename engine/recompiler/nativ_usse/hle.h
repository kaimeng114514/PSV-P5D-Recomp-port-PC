/* Local shim replacing nativ's hle.h/rt.h for the standalone USSE tools.
 * Project Nativ (GPL-2+) runtime/gfx/usse.c expects the guest memory macros from
 * recompiler/rt.h: G(a)/RD8/RD16/RD32/RD64/WR32 and recomp_mem.  Here the exported GXP
 * is placed at guest address 0 and recomp_mem points at our buffer. */
#ifndef USSE_HLE_SHIM_H
#define USSE_HLE_SHIM_H
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

extern uint8_t *recomp_mem;

static inline uint8_t *G(uint32_t a) { return recomp_mem + (uint32_t)a; }
static inline uint32_t RD32(uint32_t a) { uint32_t v; memcpy(&v, G(a), 4); return v; }
static inline uint16_t RD16(uint32_t a) { uint16_t v; memcpy(&v, G(a), 2); return v; }
static inline uint8_t  RD8(uint32_t a)  { return *G(a); }
static inline uint64_t RD64(uint32_t a) { uint64_t v; memcpy(&v, G(a), 8); return v; }
static inline void WR32(uint32_t a, uint32_t v) { memcpy(G(a), &v, 4); }
static inline void WR16(uint32_t a, uint16_t v) { memcpy(G(a), &v, 2); }
static inline void WR8(uint32_t a, uint8_t v)   { *G(a) = v; }

/* nativ's runtime allocator; the decoder never calls it but keep the symbol for any stray use. */
static inline uint32_t guest_alloc(uint32_t n) { (void)n; return 0; }

#endif
