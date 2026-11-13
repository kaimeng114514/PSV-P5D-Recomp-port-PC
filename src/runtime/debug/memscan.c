/*
 * RECOMP_MEMSCAN=frame:op,frame:op,... -- finds a game's state that follows an input, the way a memory scanner does, so
 * a mouse hook knows what to write. In a recorded session (RECOMP_INPUT_PLAY), each step filters the 4-byte-aligned
 * floats of the game's memory by what they did since the previous step:
 *   s (the first step: the snapshot)   c changed   = held still (within RECOMP_MEMSCAN_EPS, default 1e-4)
 *   + changed the same way as at the first c   - changed the other way
 * e.g. "4943:s,4969:c,4978:=,5019:+,5031:=,5047:-" -- a right turn, still, a right turn, still, a left turn: an aim yaw.
 * Each step logs its survivor count; the last, up to 64 survivors with their value at every step. The writer of the
 * right one is the hook point (an lldb watchpoint, see lldb-guest-watchpoints). Called once per presented frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "rt.h"
#include "hle.h"

void memscan_frame(uint64_t frame);
__attribute__((weak)) int datawatch_add(uint32_t addr) { (void)addr; return 0; }   /* verify.c's (a VERIFY build) replaces this: 0 = no write watch here */
uint32_t memscan_best;   /* RECOMP_MEMSCAN_WATCH's best fit, for a debugger to watch exactly (lldb: a hardware watchpoint once it is
                          * non-zero) -- the page-protection watch misses a store whose function wrote the same page just before */

#define MAXSTEP 12
typedef struct { uint32_t addr; float v[MAXSTEP]; int32_t iv[MAXSTEP]; } Cand;
static int int_mode;   /* RECOMP_MEMSCAN_INT=1: scan integers instead of floats (menu selection indices) */
static int int_step;   /* RECOMP_MEMSCAN_ISTEP=n: every change is exactly +-n (a menu index moving one row) */
static int32_t int_lo = -2000000000, int_hi = 2000000000;   /* RECOMP_MEMSCAN_IRANGE=lo,hi: every value in this range */
static int int_width = 4;   /* RECOMP_MEMSCAN_IWIDTH=1|2|4: the integer's size (a menu index is often one byte) */
static Cand *cands; static size_t ncand;
static uint8_t *snap; static uint32_t snap_lo[65], snap_hi[65]; static int nsnap;
static long step_frame[MAXSTEP]; static char step_op[MAXSTEP]; static int nsteps, cur, parsed, first_c = -1;

static float rdf(uint32_t a) { float f; memcpy(&f, G(a), 4); return f; }
static int32_t rdi(uint32_t a) { int32_t i; memcpy(&i, G(a), 4); return i; }
static int32_t rdw(uint32_t a, int w) {
	if (w == 1) return (int32_t)(int8_t)RD8(a);
	if (w == 2) return (int32_t)(int16_t)RD16(a);
	return rdi(a);
}
static int sane(float f) { return isfinite(f) && fabsf(f) < 1.0e5f; }

/* RECOMP_MEMWATCH=addr[,addr...] (hex, up to 8) with RECOMP_MEMWATCH_FROM/_TO=frame: those floats logged every presented
 * frame in the window -- a candidate from a scan followed through an input (does it move when the stick does?) */
static void memwatch_frame(uint64_t frame) {
	static uint32_t wa[8]; static int nw = -1; static long from, to;
	if (nw < 0) {
		int_mode = getenv("RECOMP_MEMSCAN_INT") && atoi(getenv("RECOMP_MEMSCAN_INT")) != 0;
		if (getenv("RECOMP_MEMSCAN_ISTEP")) int_step = atoi(getenv("RECOMP_MEMSCAN_ISTEP"));
		if (getenv("RECOMP_MEMSCAN_IRANGE")) sscanf(getenv("RECOMP_MEMSCAN_IRANGE"), "%d,%d", &int_lo, &int_hi);
		if (getenv("RECOMP_MEMSCAN_IWIDTH")) { int w = atoi(getenv("RECOMP_MEMSCAN_IWIDTH")); if (w == 1 || w == 2 || w == 4) int_width = w; }
		nw = 0; const char *e = getenv("RECOMP_MEMWATCH");
		for (const char *p = e; p && *p && nw < 8; ) { wa[nw++] = (uint32_t)strtoul(p, 0, 16); p = strchr(p, ','); if (p) p++; }
		from = getenv("RECOMP_MEMWATCH_FROM") ? atol(getenv("RECOMP_MEMWATCH_FROM")) : 0; to = getenv("RECOMP_MEMWATCH_TO") ? atol(getenv("RECOMP_MEMWATCH_TO")) : (long)1e12;
	}
	if (!nw || (long)frame < from || (long)frame > to) return;
	fprintf(stderr, "    memwatch: frame %llu", (unsigned long long)frame);
	for (int i = 0; i < nw; i++) { if (int_mode) fprintf(stderr, " %#x=%d", wa[i], (int)rdi(wa[i])); else fprintf(stderr, " %#x=%g", wa[i], rdf(wa[i])); }
	fprintf(stderr, "\n");
}
/* RECOMP_GUEST_WRITE="frame:hexaddr=intvalue,...": write guest memory words from the outside at those
 * presented frames -- the test harness for the PC interaction layer's direct-state control (a hover that
 * moves the game's own selection instead of tapping the D-pad). */
static void memwrite_frame(uint64_t frame) {
	static int parsed; static struct { long f; uint32_t a; uint32_t v; } st[32]; static int n;
	if (!parsed) {
		parsed = 1; const char *e = getenv("RECOMP_GUEST_WRITE");
		for (const char *p = e; p && *p && n < 32; ) {
			long f; char hex[16] = {0}; long v = 0;
			if (sscanf(p, "%ld:%15[0-9a-fA-F]=%li", &f, hex, &v) < 3) break;
			st[n].f = f; st[n].a = (uint32_t)strtoul(hex, 0, 16); st[n].v = (uint32_t)v; n++;
			p = strchr(p, ','); if (p) p++;
		}
	}
	for (int i = 0; i < n; i++) if ((long)frame == st[i].f) {
		WR32(st[i].a, st[i].v);
		fprintf(stderr, "    guest write: frame %llu %#x = %#x\n", (unsigned long long)frame, st[i].a, st[i].v);
	}
}
/* RECOMP_MEMFIND="frame:lo-hi": at that presented frame, find every 4-byte word whose value lies in
 * [lo, hi] (hex) and print it -- the pointer hunt for a heap object found by a value scan (a stable
 * pointer in the main module's data, whose field we then write directly). */
static void memfind_frame(uint64_t frame) {
	static int parsed; static long f = -1; static uint32_t lo, hi;
	if (!parsed) {
		parsed = 1; const char *e = getenv("RECOMP_MEMFIND");
		if (e) { char a[16] = {0}, b[16] = {0}; if (sscanf(e, "%ld:%15[0-9a-fA-F]-%15[0-9a-fA-F]", &f, a, b) == 3) { lo = (uint32_t)strtoul(a, 0, 16); hi = (uint32_t)strtoul(b, 0, 16); } }
	}
	if (f < 0 || (long)frame != f) return;
	uint32_t rl[65], rh[65]; int n = guest_mem_ranges(rl, rh, 65); int found = 0;
	for (int i = 0; i < n; i++) {
		uint32_t a = (rl[i] + 3u) & ~3u;
		for (; a + 4 <= rh[i]; a += 4) {
			uint32_t v; memcpy(&v, G(a), 4);
			if (v >= lo && v <= hi) { fprintf(stderr, "    memfind: %#010x = %#x\n", a, v); if (++found >= 200) return; }
		}
	}
	fprintf(stderr, "    memfind: frame %llu range %#x-%#x: %d pointers\n", (unsigned long long)frame, lo, hi, found);
}
/* RECOMP_GUEST_WRITE_LOOP="from-to:hexaddr=intvalue,...": write those words every presented frame in the
 * range. A game that recomputes its UI state each frame overwrites a one-shot write, so the direct
 * mouse->highlight control has to hold the value the way a held selection does. */
static void memwriteloop_frame(uint64_t frame) {
	static int parsed; static struct { long f0, f1; uint32_t a, v; } st[16]; static int n;
	if (!parsed) {
		parsed = 1; const char *e = getenv("RECOMP_GUEST_WRITE_LOOP");
		for (const char *p = e; p && *p && n < 16; ) {
			long a0, a1; char hex[16] = {0}; long v = 0;
			if (sscanf(p, "%ld-%ld:%15[0-9a-fA-F]=%li", &a0, &a1, hex, &v) < 4) break;
			st[n].f0 = a0; st[n].f1 = a1; st[n].a = (uint32_t)strtoul(hex, 0, 16); st[n].v = (uint32_t)v; n++;
			p = strchr(p, ','); if (p) p++;
		}
	}
	for (int i = 0; i < n; i++) if ((long)frame >= st[i].f0 && (long)frame <= st[i].f1) WR32(st[i].a, st[i].v);
}
/* RECOMP_MEMDUMP="frame:path,fram:path,..." (RECOMP_MEMDUMP_ADDR="lo-hi" hex: only ranges inside):
 * dump the guest memory ranges in use at those presented frames as a simple binary (header + raw bytes),
 * so the menu/UI object layout can be diffed offline between two known highlight rows. */
static void memdump_frame(uint64_t frame) {
	static int parsed; static struct { long f; char path[256]; } st[8]; static int n; static int ok;
	if (!parsed) {
		parsed = 1; const char *e = getenv("RECOMP_MEMDUMP");
		for (const char *p = e; p && *p && n < 8; ) {
			long f; char path[256] = {0};
			if (sscanf(p, "%ld:%255[^,]", &f, path) < 2) break;
			st[n].f = f; snprintf(st[n].path, sizeof st[n].path, "%s", path); n++;
			p = strchr(p, ','); if (p) p++;
		}
		const char *r = getenv("RECOMP_MEMDUMP_ADDR");
		if (r && *r) { char a[16]={0}, b[16]={0}; if (sscanf(r, "%15[0-9a-fA-F]-%15[0-9a-fA-F]", a, b)==2) { static uint32_t lo, hi; lo=(uint32_t)strtoul(a,0,16); hi=(uint32_t)strtoul(b,0,16); (void)lo; (void)hi; } }
		ok = 1;
	}
	if (!ok) return;
	/* (the address filter accepts a comma-separated list of lo-hi ranges: heap, then module data) */
	static uint32_t fl[8], fh[8]; static int nf;
	if (!nf) { const char *r = getenv("RECOMP_MEMDUMP_ADDR"); nf = 1; fl[0] = 0; fh[0] = 0xffffffffu;
		if (r && *r) { nf = 0; for (const char *q = r; q && *q && nf < 8; ) { char a[16]={0}, b[16]={0}; if (sscanf(q, "%15[0-9a-fA-F]-%15[0-9a-fA-F]", a, b) != 2) break; fl[nf]=(uint32_t)strtoul(a,0,16); fh[nf]=(uint32_t)strtoul(b,0,16); nf++; q = strchr(q, ','); if (q) q++; } } }
	for (int i = 0; i < n; i++) if ((long)frame == st[i].f) {
		uint32_t rl[65], rh[65]; int nr = guest_mem_ranges(rl, rh, 65);
		FILE *o = fopen(st[i].path, "wb"); if (!o) continue;
		uint32_t magic = 0x31444d50u; uint32_t cnt = 0; fwrite(&magic,4,1,o); fwrite(&cnt,4,1,o);   /* fixed up below */
		long pos = 8; uint32_t written = 0;
		for (int fi = 0; fi < nf; fi++) {
		uint32_t lo = fl[fi], hi = fh[fi];
		for (int k = 0; k < nr; k++) {
			uint32_t a = rl[k] < lo ? lo : rl[k], b = rh[k] > hi ? hi : rh[k];
			if (b <= a) continue;
			fwrite(&a,4,1,o); fwrite(&b,4,1,o); fwrite(G(a),1,b-a,o);
			pos += 8 + (long)(b - a); written++;
		}
		}
		fseek(o, 4, SEEK_SET); fwrite(&written,4,1,o); fclose(o);
		fprintf(stderr, "    memdump: frame %llu -> %s (%u ranges, %ld bytes)\n", (unsigned long long)frame, st[i].path, written, pos);
	}
}
/* RECOMP_HEAPSCAN="f0,f1,f2": snapshots of the UI heap (0x10000000..0x13000000) at those presented
 * frames; the candidates are the 4-byte ints that increase by exactly 1 between f0->f1 AND f1->f2 and stay
 * small (0..10) -- the menu's selection index during two scripted DOWN presses. The address found this way
 * is what the mouse hover will write directly (per session: the heap layout can move between runs). */
static void heapscan_frame(uint64_t frame) {
	static int parsed; static long fr[3]; static int nf; static uint8_t *snap[3];
	static const uint32_t LO = 0x10000000u, HI = 0x13000000u;
	if (!parsed) {
		parsed = 1; const char *e = getenv("RECOMP_HEAPSCAN");
		for (const char *p = e; p && *p && nf < 3; ) { fr[nf++] = atol(p); p = strchr(p, ','); if (p) p++; }
	}
	int idx = -1; for (int i = 0; i < nf; i++) if ((long)frame == fr[i]) idx = i;
	if (idx < 0) return;
	snap[idx] = malloc(HI - LO);
	if (!snap[idx]) return;
	memcpy(snap[idx], G(LO), HI - LO);
	fprintf(stderr, "    heapscan: snapshot %d at frame %llu\n", idx, (unsigned long long)frame);
	if (idx == 2 && snap[0] && snap[1] && snap[2]) {
		uint8_t *hit = calloc(((HI - LO) / 4), 1); int n = 0, iso = 0; if (!hit) return;
		
		for (uint32_t o = 0; o + 4 <= HI - LO; o += 4) {
			int32_t a, b, c; memcpy(&a, snap[0] + o, 4); memcpy(&b, snap[1] + o, 4); memcpy(&c, snap[2] + o, 4);
			if (a >= 0 && a <= 10 && b == a + 1 && c == b + 1) { hit[o / 4] = 1; n++; }
		}
		for (uint32_t o = 0; o + 4 <= HI - LO; o += 4) {
			if (!hit[o / 4]) continue;
			int neighbour = 0;
			for (int d = -16; d <= 16 && !neighbour; d++) { if (!d) continue; int64_t q = (int64_t)o / 4 + d; if (q >= 0 && q < (int64_t)((HI - LO) / 4) && hit[q]) neighbour = 1; }
			if (neighbour) continue;
			int32_t a, b, c; memcpy(&a, snap[0] + o, 4); memcpy(&b, snap[1] + o, 4); memcpy(&c, snap[2] + o, 4);
			fprintf(stderr, "    heapscan: isolated %#010x = %d,%d,%d\n", LO + o, a, b, c);
			if (++iso >= 32) break;
		}
		fprintf(stderr, "    heapscan: %d candidates, %d isolated (0x40-byte window)\n", n, iso); free(hit);
		free(snap[0]); free(snap[1]); free(snap[2]);
	}
}
void memscan_frame(uint64_t frame) {
	memwatch_frame(frame);
	memwrite_frame(frame);
	memfind_frame(frame);
	memwriteloop_frame(frame);
	memdump_frame(frame);
	heapscan_frame(frame);
	if (!parsed) {
		parsed = 1; const char *e = getenv("RECOMP_MEMSCAN");
		for (const char *p = e; p && *p && nsteps < MAXSTEP; ) {
			long f; char op; if (sscanf(p, "%ld:%c", &f, &op) != 2) break;
			step_frame[nsteps] = f; step_op[nsteps++] = op;
			p = strchr(p, ','); if (p) p++;
		}
	}
	if (cur >= nsteps || (long)frame != step_frame[cur]) return;
	float eps = getenv("RECOMP_MEMSCAN_EPS") ? (float)atof(getenv("RECOMP_MEMSCAN_EPS")) : 1e-4f;
	char op = step_op[cur];
	if (cur == 0) {   /* a raw snapshot of every range in use */
		nsnap = guest_mem_ranges(snap_lo, snap_hi, 65); size_t bytes = 0;
		for (int i = 0; i < nsnap; i++) bytes += snap_hi[i] - snap_lo[i];
		free(snap); snap = malloc(bytes);
		size_t o = 0; for (int i = 0; i < nsnap; i++) { memcpy(snap + o, G(snap_lo[i]), snap_hi[i] - snap_lo[i]); o += snap_hi[i] - snap_lo[i]; }
		ncand = bytes / 4;
	} else if (cur == 1) {   /* the candidates: what changed since the snapshot (the first step after it must be 'c') */
		size_t cap = 1 << 20, o = 0; cands = malloc(cap * sizeof *cands); ncand = 0;
		for (int i = 0; i < nsnap; i++) {
			uint32_t st = int_mode ? ((snap_lo[i] + (uint32_t)int_width - 1) & ~((uint32_t)int_width - 1)) : (snap_lo[i] & ~3u);
			for (uint32_t a = st; a + (uint32_t)(int_mode ? int_width : 4) <= snap_hi[i]; a += (uint32_t)(int_mode ? int_width : 4)) {
				if (int_mode) {
					int32_t was = rdw(a, int_width); { uint8_t b[4] = {0,0,0,0}; memcpy(b, snap + o + (a - snap_lo[i]), int_width); if (int_width == 1) was = (int8_t)b[0]; else if (int_width == 2) { int16_t s16; memcpy(&s16, b, 2); was = s16; } else memcpy(&was, b, 4); }
					int32_t now = rdw(a, int_width);
					if (now == was) continue;
					if (int_step && (now - was > int_step || was - now > int_step)) continue;
					if (now < int_lo || now > int_hi || was < int_lo || was > int_hi) continue;
					if (ncand == cap) { cap *= 2; cands = realloc(cands, cap * sizeof *cands); }
					cands[ncand].addr = a; cands[ncand].iv[0] = was; cands[ncand].iv[1] = now; ncand++;
					continue;
				}
				float was; memcpy(&was, snap + o + (a - snap_lo[i]), 4); float now = rdf(a);
				if (!sane(was) || !sane(now) || fabsf(now - was) <= eps) continue;
				if (ncand == cap) { cap *= 2; cands = realloc(cands, cap * sizeof *cands); }
				cands[ncand].addr = a; cands[ncand].v[0] = was; cands[ncand].v[1] = now; ncand++;
			}
			o += snap_hi[i] - snap_lo[i];
		}
		free(snap); snap = 0; first_c = 1;
	} else {
		size_t k = 0;
		for (size_t i = 0; i < ncand; i++) {
			if (int_mode) {
				int32_t now = rdw(cands[i].addr, int_width), was = cands[i].iv[cur - 1], d = now - was; int keep = 1;
				if (now < int_lo || now > int_hi) keep = 0;
				if (int_step && d != 0 && (d > int_step || -d > int_step)) keep = 0;
				if (op == 'c') keep = keep && d != 0;
				else if (op == '=') keep = d == 0;
				else if (op == '+' || op == '-') {
					int32_t d0 = cands[i].iv[first_c] - cands[i].iv[first_c - 1];
					keep = d != 0 && ((d > 0) == (d0 > 0)) == (op == '+');
				}
				if (keep) { cands[k] = cands[i]; cands[k].iv[cur] = now; k++; }
				continue;
			}
			float now = rdf(cands[i].addr), was = cands[i].v[cur - 1], d = now - was; int keep = sane(now);
			if (op == 'c') keep = keep && fabsf(d) > eps;
			else if (op == '=') keep = keep && fabsf(d) <= eps;
			else if (op == '+' || op == '-') {
				float d0 = cands[i].v[first_c] - cands[i].v[first_c - 1];
				keep = keep && fabsf(d) > eps && ((d > 0) == (d0 > 0)) == (op == '+');
			}
			if (keep) { cands[k] = cands[i]; cands[k].v[cur] = now; k++; }
		}
		ncand = k;
		if (op == 'c' && first_c < 0) first_c = cur;
	}
	fprintf(stderr, "    memscan: frame %llu step %d '%c': %zu candidates\n", (unsigned long long)frame, cur, op, ncand);
	if (++cur == nsteps) {
		/* RECOMP_MEMSCAN_RATIO=r,r,...: each change step's expected change relative to the first one's (how far the stick
		 * moved over it, say), within a factor of 2; RECOMP_MEMSCAN_MAG=lo,hi: the first change's size. Survivors only. */
		const char *re = getenv("RECOMP_MEMSCAN_RATIO"), *me = getenv("RECOMP_MEMSCAN_MAG");
		float ratio[MAXSTEP]; int nr = 0; for (const char *p = re; p && *p && nr < MAXSTEP; ) { ratio[nr++] = (float)atof(p); p = strchr(p, ','); if (p) p++; }
		float mlo = 0, mhi = 1e9f; if (me) sscanf(me, "%f,%f", &mlo, &mhi);
		size_t k = 0;
		for (size_t i = 0; i < ncand; i++) {
			float d0 = cands[i].v[first_c] - cands[i].v[first_c - 1]; int keep = fabsf(d0) >= mlo && fabsf(d0) <= mhi, r = 0;
			for (int s2 = first_c + 1; s2 < nsteps && keep; s2++) {
				if (step_op[s2] != '+' && step_op[s2] != '-' && step_op[s2] != 'c') continue;
				if (r >= nr) break;
				float want = ratio[r++] * d0, got = cands[i].v[s2] - cands[i].v[s2 - 1];
				keep = (want > 0) == (got > 0) && fabsf(got) >= fabsf(want) / 2 && fabsf(got) <= fabsf(want) * 2;
			}
			if (keep) cands[k++] = cands[i];
		}
		if (re || me) { ncand = k; fprintf(stderr, "    memscan: %zu fit the ratios %s and size %s\n", ncand, re ? re : "-", me ? me : "-"); }
		/* RECOMP_MEMSCAN_WATCH=n (a VERIFY build): the n best fits that also leave [-1, 1] somewhere (an angle, not a direction
		 * component) get write watches (verify.c datawatch_add) for the rest of the run: which function writes them */
		int nwatch = getenv("RECOMP_MEMSCAN_WATCH") ? atoi(getenv("RECOMP_MEMSCAN_WATCH")) : 0;
		if (nwatch > 0) {
			typedef struct { float score; size_t i; } Sc; Sc *sc = malloc((ncand ? ncand : 1) * sizeof *sc); size_t ns = 0;
			for (size_t i = 0; i < ncand; i++) {
				float m = 0, score = 0, d0 = cands[i].v[first_c] - cands[i].v[first_c - 1]; int r = 0;
				for (int s2 = 0; s2 < nsteps; s2++) if (fabsf(cands[i].v[s2]) > m) m = fabsf(cands[i].v[s2]);
				if (m <= 1.01f) continue;
				for (int s2 = first_c + 1; s2 < nsteps && r < nr; s2++) if (step_op[s2] == '+' || step_op[s2] == '-' || step_op[s2] == 'c') score += fabsf((cands[i].v[s2] - cands[i].v[s2 - 1]) / d0 - ratio[r++]);
				sc[ns].score = score; sc[ns].i = i; ns++;
			}
			for (size_t a2 = 1; a2 < ns; a2++) for (size_t b2 = a2; b2 > 0 && sc[b2].score < sc[b2 - 1].score; b2--) { Sc t = sc[b2]; sc[b2] = sc[b2 - 1]; sc[b2 - 1] = t; }
			for (size_t w = 0; w < ns && (int)w < nwatch; w++) {
				Cand *cw = &cands[sc[w].i];
				if (!w) __atomic_store_n(&memscan_best, cw->addr, __ATOMIC_RELEASE);
				fprintf(stderr, "    memscan: watching %#010x (fit %.3f):", cw->addr, sc[w].score);
				for (int s2 = 0; s2 < nsteps; s2++) fprintf(stderr, " %g", cw->v[s2]);
				fprintf(stderr, "\n");
				if (!datawatch_add(cw->addr)) { fprintf(stderr, "    memscan: (no write watch in this build: RECOMP_MEMSCAN_WATCH needs the verify build)\n"); break; }
			}
			free(sc);
		}
		const char *op2 = getenv("RECOMP_MEMSCAN_OUT");   /* every survivor, one line each: address and its value at each step */
		FILE *of = op2 ? fopen(op2, "w") : 0;
		for (size_t i = 0; of && i < ncand; i++) {
			fprintf(of, "%#010x", cands[i].addr);
			if (int_mode) for (int s2 = 0; s2 < nsteps; s2++) fprintf(of, " %d", cands[i].iv[s2]);
			else for (int s2 = 0; s2 < nsteps; s2++) fprintf(of, " %g", cands[i].v[s2]);
			fprintf(of, "\n");
		}
		if (of) { fclose(of); fprintf(stderr, "    memscan: %zu survivors written to %s\n", ncand, op2); }
		for (size_t i = 0; i < ncand && i < 64; i++) {
			fprintf(stderr, "    memscan:   %#010x", cands[i].addr);
			if (int_mode) for (int s = 0; s < nsteps; s++) fprintf(stderr, " %d", cands[i].iv[s]);
			else for (int s = 0; s < nsteps; s++) fprintf(stderr, " %g", cands[i].v[s]);
			fprintf(stderr, "\n");
		}
		free(cands); cands = 0;
	}
}
