/*
 * P5D (PCSH10109) diagnostics: the title/logo sequence state machines, to see exactly where the run
 * stalls after the S0 logos (the screen stays black while the game keeps drawing an empty frame).
 *
 * From the earlier (AOT harness) investigation of the same eboot:
 *   fn_81193704  worker of the S0 logo job: *puVar1 = state (0..0xd), puVar1[1] = 1 when done,
 *                puVar1[4] = the title/logo.bin container handle, puVar1[5..7] = the three TGA tasks.
 *   fn_81198172  worker of the title sequence job (created by the title code).
 * Addresses are this build's (the pack's FUNCTION_SEEDS makes the analysis treat them as functions).
 * Each hook prints only on a change (and a heartbeat) so a 7600-frame run's log stays readable.
 */
#include "hooks.h"

static uint64_t hook_frames(void) { extern uint64_t display_frames(void); return display_frames(); }

void recomp_hook_p5d_s0state(Cpu *c) {
	uint32_t p = c->r[0];
	static uint32_t last;
	static uint64_t lastf;
	uint32_t st = p ? RD32(p) : 0xffffffffu;
	uint32_t done = p ? RD32(p + 4) : 0;
	uint32_t h = p ? RD32(p + 16) : 0;
	uint32_t t5 = p ? RD32(p + 20) : 0, t6 = p ? RD32(p + 24) : 0, t7 = p ? RD32(p + 28) : 0;
	uint64_t f = hook_frames();
	if (st != last || f - lastf >= 300) {
		fprintf(stderr, "    [S0] frame %llu p=%#x state=%u done=%u h=%#x tga=%#x,%#x,%#x lr=%#x\n",
		        (unsigned long long)f, p, st, done, h, t5, t6, t7, c->r[14]);
		last = st; lastf = f;
	}
}

void recomp_hook_p5d_titlejob(Cpu *c) {
	uint32_t p = c->r[0];
	static uint32_t last[8]; static uint64_t lastf; static int have;
	uint32_t w[8];
	for (int k = 0; k < 8; k++) w[k] = p ? RD32(p + 4 * k) : 0xffffffffu;
	uint64_t f = hook_frames();
	int ch = !have || memcmp(w, last, sizeof w) != 0;
	if (ch || f - lastf >= 300) {
		fprintf(stderr, "    [TITLE] frame %llu p=%#x w=%#x %#x %#x %#x %#x %#x %#x %#x lr=%#x\n",
		        (unsigned long long)f, p, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], c->r[14]);
		memcpy(last, w, sizeof w); lastf = f; have = 1;
	}
}

void recomp_hook_p5d_jobdone(Cpu *c) {
	static uint64_t lastf;
	uint64_t f = hook_frames();
	if (f - lastf >= 30) {
		fprintf(stderr, "    [JOBDONE] frame %llu r0=%#x r1=%#x lr=%#x\n", (unsigned long long)f, c->r[0], c->r[1], c->r[14]);
		lastf = f;
	}
}

/* IHOOK at 0x8119817a (right after the title worker calls FUN_8116f8a8): r4 is the task's work area
 * (the state machine's puVar1). Log state (work[0]), the return-to-title flag (work[1]) and the
 * sub-player handle (work[2]) on change, and a heartbeat. */
void recomp_ihook_p5d_work(Cpu *c, int reg) {
	uint32_t w = c->r[4];
	static uint32_t last[3]; static uint64_t lastf; static int have;
	uint32_t s = w ? RD32(w) : 0xffffffffu, flag = w ? RD32(w + 4) : 0, h = w ? RD32(w + 8) : 0;
	uint64_t f = hook_frames();
	if (!have || s != last[0] || flag != last[1] || h != last[2] || f - lastf >= 300) {
		fprintf(stderr, "    [WORK] frame %llu w=%#x state=%u flag=%u h=%#x lr=%#x\n",
		        (unsigned long long)f, w, s, flag, h, c->r[14]);
		last[0] = s; last[1] = flag; last[2] = h; lastf = f; have = 1;
	}
}
/* IHOOK at 0x81198270 (adds r5,r0,#0): the result of FUN_81193a6a(work[2]), the title/logo sub-player's
 * update -- 0 = keep waiting, 1 = back to state 4, 2..5 = quit paths. */
void recomp_ihook_p5d_seqret(Cpu *c, int reg) {
	static uint32_t last = 0xffffffffu; static uint64_t lastf;
	uint32_t r = c->r[5];
	uint64_t f = hook_frames();
	if (r != last || f - lastf >= 300) {
		fprintf(stderr, "    [SEQRET] frame %llu ret=%u lr=%#x\n", (unsigned long long)f, r, c->r[14]);
		last = r; lastf = f;
	}
}

/* HOOK at 0x81193a90: the 'title' async node's step (title.c's 10-state sub machine, tbh at
 * 0x81193ab4). Its work object (node[+8], 0x18 bytes): +0 flags (bit1 = done), +4 sub state,
 * +8 return value. Log on change; sub state 8 (0x81193dc0 block) is the one that sets bit1. */
void recomp_hook_p5d_tsubstep(Cpu *c) {
	uint32_t node = c->r[0], w = node ? RD32(node + 8) : 0;
	static uint32_t last[3]; static uint64_t lastf; static int have;
	uint32_t fl = w ? RD32(w) : 0xffffffffu, ss = w ? RD32(w + 4) : 0xffffffffu, rv = w ? RD32(w + 8) : 0;
	uint64_t f = hook_frames();
	if (!have || fl != last[0] || ss != last[1] || rv != last[2] || f - lastf >= 300) {
		fprintf(stderr, "    [TSUB] frame %llu node=%#x w=%#x flags=%#x state=%u ret=%#x lr=%#x\n",
		        (unsigned long long)f, node, w, fl, ss, rv, c->r[14]);
		last[0] = fl; last[1] = ss; last[2] = rv; lastf = f; have = 1;
	}
}
/* IHOOK at 0x81193dc4 (movs r0,#1 in the sub state 8 block): the completion path runs. */
void recomp_ihook_p5d_tsubdone(Cpu *c, int reg) {
	static uint64_t lastf;
	uint64_t f = hook_frames();
	if (f - lastf >= 1) {
		fprintf(stderr, "    [TSUBDONE] frame %llu r0=%#x lr=%#x\n", (unsigned long long)f, c->r[0], c->r[14]);
		lastf = f;
	}
}

/* HOOK at 0x81193fbe: the 'title draw' node's step (titleDraw.c), created by the title sub machine's
 * state 2 (0x81193e10). Its work (node[+8]): +0 flags/result byte, +4 sub state; the parent's state 2
 * waits until +4 == 8 and then uses the low byte of +0 as the result. Log on change + heartbeat. */
void recomp_hook_p5d_tdraw(Cpu *c) {
	uint32_t node = c->r[0], w = node ? RD32(node + 8) : 0;
	static uint32_t last[8]; static uint64_t lastf; static int have;
	uint32_t v[8];
	for (int k = 0; k < 8; k++) v[k] = w ? RD32(w + 4 * k) : 0xffffffffu;
	uint64_t f = hook_frames();
	int ch = !have || memcmp(v, last, sizeof v) != 0;
	if (ch || f - lastf >= 300) {
		fprintf(stderr, "    [TDRAW] frame %llu node=%#x w=%#x v=%#x %#x %#x %#x %#x %#x %#x %#x lr=%#x\n",
		        (unsigned long long)f, node, w, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], c->r[14]);
		memcpy(last, v, sizeof v); lastf = f; have = 1;
	}
}
/* HOOK at 0x8119430c: the title draw node's cleanup (it reached its end). */
void recomp_hook_p5d_tdraw_end(Cpu *c) {
	static uint64_t lastf;
	uint64_t f = hook_frames();
	if (f - lastf >= 1) { fprintf(stderr, "    [TDRAW-END] frame %llu r0=%#x lr=%#x\n", (unsigned long long)f, c->r[0], c->r[14]); lastf = f; }
}

/* IHOOKs inside the P5 titleDraw create (0x81196b54): the three gates that make it return 0
 * (keep the parent waiting): fn_8117c3b6 at 0x81196b7c, fn_81197d7a at 0x81196b8a,
 * fn_810f6938 at 0x81196b9a. Log the r0 each returned (on change + heartbeat). */
void recomp_ihook_p5d_tdraw_gate(Cpu *c, int reg) {
	static uint32_t last; static uint64_t lastf; static int have;
	uint32_t v = c->r[0];
	uint64_t f = hook_frames();
	if (!have || v != last || f - lastf >= 300) {
		fprintf(stderr, "    [GATE] frame %llu pc=%#x r0=%#x lr=%#x\n", (unsigned long long)f, c->r[15], v, c->r[14]);
		last = v; lastf = f; have = 1;
	}
}

/* HOOK at 0x81197d7a: titleDraw's poll of the title movie player node (fn_81197d7a returns
 * work[0] > 1). Log the node's step (node[0]), cleanup (node[4]), work (node[8]) and the work's
 * first words, so the movie player's own state machine can be traced. */
void recomp_hook_p5d_moviepoll(Cpu *c) {
	uint32_t node = c->r[0], w = node ? RD32(node + 8) : 0;
	static uint32_t last[6]; static uint64_t lastf; static int have;
	uint32_t v[6];
	uint32_t mgr = w ? RD32(w + 4) : 0;
	uint32_t mobj = mgr ? RD32(mgr + 0x70) : 0, mh = mobj ? RD32(mobj + 8) : 0;
	v[0] = node ? RD32(node) : 0; v[1] = node ? RD32(node + 4) : 0; v[2] = w;
	v[3] = w ? RD32(w) : 0xffffffffu; v[4] = w ? RD32(w + 4) : 0; v[5] = w ? RD32(w + 8) : 0;
	uint32_t v6 = mobj, v7 = mh, v8 = mh ? RD32(mh + 0xc) : 0xffffffffu;
	uint64_t f = hook_frames();
	int ch = !have || memcmp(v, last, sizeof v) != 0;
	if (ch || f - lastf >= 300) {
		fprintf(stderr, "    [MOVIEPOLL] frame %llu node=%#x step=%#x w=%#x st=%#x w4=%#x w8=%#x mobj=%#x mh=%#x mstate=%#x lr=%#x\n",
		        (unsigned long long)f, node, v[0], w, v[3], v[4], v[5], v6, v7, v8, c->r[14]);
		memcpy(last, v, sizeof v); lastf = f; have = 1;
	}
}

/* HOOK at 0x811588f6: the title movie's create (called by the movie player node's state 0 as
 * fn(work[4], work+0x10, work[0x90])). r0 is the manager/owner, r1 the data buffer, r2 its size.
 * Print the args and the first bytes of the buffer: a USM file starts with the CRI magic ("CRID"),
 * a zero buffer means the movie data never reached this point. */
void recomp_hook_p5d_moviecreate(Cpu *c) {
	static int n;
	uint32_t a0 = c->r[0], a1 = c->r[1], a2 = c->r[2];
	if (n++ >= 8) return;
	char hex[3 * 33 + 1]; int k = 0;
	for (int i = 0; i < 32; i++) { unsigned v = (a1 >= 0x10000u && a1 < 0xfff00000u) ? RD8(a1 + (uint32_t)i) : 0; k += snprintf(hex + k, sizeof hex - k, "%02x", v); if (i < 31) hex[k++] = ' '; }
	hex[k] = 0;
	char asc[34]; int m = 0;
	for (int i = 0; i < 32; i++) { unsigned v = (a1 >= 0x10000u && a1 < 0xfff00000u) ? RD8(a1 + (uint32_t)i) : 0; asc[m++] = (v >= 32 && v < 127) ? (char)v : '.'; }
	asc[m] = 0;
	fprintf(stderr, "    [MOVIECREATE] frame %llu r0=%#x r1=%#x r2=%#x bytes[%s] ascii[%s] lr=%#x\n",
	        (unsigned long long)hook_frames(), a0, a1, a2, hex, asc, c->r[14]);
}
/* HOOK at 0x8115423c: the lower-level movie device open/load (r6 = device). Log its entry args. */
void recomp_hook_p5d_movieopen(Cpu *c) {
	static uint32_t last0, last1; static int n;
	uint32_t a0 = c->r[0], a1 = c->r[1];
	if (a0 == last0 && a1 == last1 && n > 0) return;
	if (n++ >= 12) return;
	fprintf(stderr, "    [MOVIEOPEN] frame %llu r0=%#x r1=%#x r2=%#x r3=%#x lr=%#x\n",
	        (unsigned long long)hook_frames(), a0, a1, c->r[2], c->r[3], c->r[14]);
	last0 = a0; last1 = a1;
}

/* HOOK at 0x811589e8: the movie status mapping called every frame by the movie player's state 1.
 * r0 = the movie manager (work[4] of the player). Log the manager's object at +0x70 and that
 * object's device handle (+8) and device state (+0xc): state 4 = playing. */
void recomp_hook_p5d_mstatus(Cpu *c) {
	uint32_t mgr = c->r[0];
	static uint32_t last[5]; static uint64_t lastf; static int have;
	uint32_t obj = mgr ? RD32(mgr + 0x70) : 0, mh = obj ? RD32(obj + 8) : 0;
	uint32_t dev = obj ? RD32(obj + 4) : 0, st = mh ? RD32(mh + 0xc) : 0xffffffffu;
	uint32_t mgr8 = mgr ? RD32(mgr + 8) : 0;
	uint32_t v[5] = { mgr, obj, mh, st, mgr8 };
	uint64_t f = hook_frames();
	int ch = !have || memcmp(v, last, sizeof v) != 0;
	if (ch || f - lastf >= 300) {
		fprintf(stderr, "    [MSTATUS] frame %llu mgr=%#x obj=%#x mh=%#x dev=%#x state=%#x mgr8=%#x lr=%#x\n",
		        (unsigned long long)f, v[0], v[1], v[2], dev, v[3], v[4], c->r[14]);
		memcpy(last, v, sizeof v); lastf = f; have = 1;
	}
}

/* IHOOK at 0x81197da0 (mov r4,#0, right before fn_81197d7a's return): force the title movie
 * player to look ready (return 1) so the title draw's init gate opens while the USM movie decode
 * is still being worked on. The title screen's UI/logo then draw over the (still black) movie
 * background, which tells us how far the rest of the title path gets. RECOMP_P5D_FORCEMOVIE=0
 * disables the force. Logs once a second. */
void recomp_ihook_p5d_moviereadyf(Cpu *c, int reg) {
	static int on = -1; if (on < 0) { const char *e = getenv("RECOMP_P5D_FORCEMOVIE"); on = e && *e == '1'; }
	if (!on) return;
	c->r[4] = 1;
	static uint64_t lastf;
	uint64_t f = hook_frames();
	if (f - lastf >= 60) {
		fprintf(stderr, "    [FORCEMOVIE] frame %llu: movie poll forced ready, r0 was %#x\n", (unsigned long long)f, c->r[0]);
		lastf = f;
	}
}

/* IHOOK at 0x81197cc8 (sxtb r0,r1, the return of fn_81197c9c): titleDraw state 3 waits for this
 * nonzero to move to state 4 (title BGM/UI setup). Forced to 1 while the USM path is being built,
 * so the rest of the title path can be observed. */
void recomp_ihook_p5d_titlephasef(Cpu *c, int reg) {
	static int on = -1; if (on < 0) { const char *e = getenv("RECOMP_P5D_FORCEMOVIE"); on = e && *e == '1'; }
	if (on) c->r[0] = 1;
	static uint64_t lastf;
	uint64_t f = hook_frames();
	if (f - lastf >= 60) { fprintf(stderr, "    [FORCEPHASE] frame %llu: titleDraw state-3 gate forced 1\n", (unsigned long long)f); lastf = f; }
}

/* movie stream creation probe (M1352): fn_812970f2 -> fn_81296e4a(0,a0,a1).
 * Logs args at entry, the inner call, the early check fn_812962ee, and the wrapper's
 * return value (IHOOK after 0x812970fe pop) so the 0-return point that becomes device
 * state 7 is visible. */
void recomp_hook_p5d_streammk(Cpu *c) {
	static uint64_t lastf;
	uint64_t f = hook_frames();
	if (f - lastf >= 1) { fprintf(stderr, "    [STREAMMK] frame %llu a0=%#x a1=%#x lr=%#x\n", (unsigned long long)f, c->r[0], c->r[1], c->r[14]); lastf = f; }
}
void recomp_hook_p5d_streaminner(Cpu *c) {
	fprintf(stderr, "    [STREAMIN] frame %llu r0=%#x r1=%#x r2=%#x lr=%#x\n", (unsigned long long)hook_frames(), c->r[0], c->r[1], c->r[2], c->r[14]);
}
void recomp_hook_p5d_earlychk(Cpu *c) {
	fprintf(stderr, "    [EARLYCHK] frame %llu arg0=%#x val0=%#x lr=%#x\n", (unsigned long long)hook_frames(), c->r[0], RD32(0), c->r[14]);
}
void recomp_ihook_p5d_streamret(Cpu *c, int reg) {
	fprintf(stderr, "    [STREAMRET] frame %llu r0=%#x\n", (unsigned long long)hook_frames(), c->r[0]);
}

/* M1359: where does fn_812970f2's return go? IHOOK after 0x811542aa (str r0,[r6,#8]) logs
 * the handle actually stored; entry hooks on the success-path subcalls of fn_81296e4a show
 * how far creation gets. */
void recomp_ihook_p5d_hwnd(Cpu *c, int reg) {
	fprintf(stderr, "    [HWND] frame %llu handle=%#x dev8=%#x\n", (unsigned long long)hook_frames(), c->r[0], RD32(c->r[6] + 8));
}
void recomp_hook_p5d_sub1(Cpu *c) { fprintf(stderr, "    [SUB1] f=%llu lr=%#x\n", (unsigned long long)hook_frames(), c->r[14]); }
void recomp_hook_p5d_sub2(Cpu *c) { fprintf(stderr, "    [SUB2] f=%llu lr=%#x\n", (unsigned long long)hook_frames(), c->r[14]); }
void recomp_hook_p5d_sub3(Cpu *c) { fprintf(stderr, "    [SUB3] f=%llu lr=%#x\n", (unsigned long long)hook_frames(), c->r[14]); }
void recomp_hook_p5d_sub4(Cpu *c) { fprintf(stderr, "    [SUB4] f=%llu lr=%#x\n", (unsigned long long)hook_frames(), c->r[14]); }

/* M1359: fn_81297100 (Sofdec2 stream status) - log the handle, the [handle+0x84] flag and
 * [handle+0x30] at entry, and the returned status code at the common exit 0x81297162. */
void recomp_hook_p5d_mst2(Cpu *c) {
	uint32_t h = c->r[0];
	fprintf(stderr, "    [MST2] frame %llu h=%#x v84=%#x v30=%#x\n", (unsigned long long)hook_frames(), h, h ? RD32(h + 0x84) : 0, h ? RD32(h + 0x30) : 0);
}
void recomp_ihook_p5d_stret(Cpu *c, int reg) {
	fprintf(stderr, "    [MRET] frame %llu st=%#x\n", (unsigned long long)hook_frames(), c->r[0]);
}

/* M1360: fn_8115423c's update path stores fn_81297100(status) into device[+0xc]:
 * log the returned status and what lands in device[+0xc] (0x81154356: str r0,[r6,#0xc]). */
void recomp_ihook_p5d_stst(Cpu *c, int reg) {
	fprintf(stderr, "    [STST] frame %llu st=%#x devC=%#x\n", (unsigned long long)hook_frames(), c->r[0], RD32(c->r[6] + 0xc));
}

/* M1360: the two writers that set [stream_obj+0x770]=1 (status 7 trigger):
 * fn_8129d4d4 and fn_8129eb18. Log entry args + the object's flag value. */
void recomp_hook_p5d_err1(Cpu *c) {
	uint32_t o = c->r[0];
	fprintf(stderr, "    [ERR1] frame %llu o=%#x v770=%#x r1=%#x r2=%#x\n", (unsigned long long)hook_frames(), o, o ? RD32(o + 0x770) : 0, c->r[1], c->r[2]);
}
void recomp_hook_p5d_err2(Cpu *c) {
	uint32_t o = c->r[0];
	fprintf(stderr, "    [ERR2] frame %llu o=%#x v770=%#x r1=%#x r2=%#x\n", (unsigned long long)hook_frames(), o, o ? RD32(o + 0x770) : 0, c->r[1], c->r[2]);
}

/* M1361: criMvPly work-buffer allocation. fn_812a42a2 computes a size and calls
 * fn_812a3c40; log the args of the real allocator and the result returned to fn_812a42a2
 * (IHOOK after the bl at 0x812a42c8, next instruction 0x812a42cc). */
void recomp_hook_p5d_alloc1(Cpu *c) {
	uint32_t o = c->r[0];
	fprintf(stderr, "    [ALLOC] frame %llu obj=%#x st4=%#x dac=%#x a1=%#x a2=%#x a3=%#x\n", (unsigned long long)hook_frames(), o, o ? RD32(o + 4) : 0, o ? RD32(o + 0xdac) : 0, c->r[1], c->r[2], c->r[3]);
}
void recomp_ihook_p5d_allocret(Cpu *c, int reg) {
	fprintf(stderr, "    [ALLOCRET] frame %llu ret=%#x\n", (unsigned long long)hook_frames(), c->r[0]);
}

/* M1361b: args entering criMvPly work-buffer allocation and the size computed by
 * fn_812abe0c (IHOOK after the bl at 0x812a42b8 -> next insn 0x812a42bc). */
void recomp_hook_p5d_allocin(Cpu *c) {
	fprintf(stderr, "    [ALLOCIN] frame %llu r0=%#x r1=%#x r2=%#x r3=%#x\n", (unsigned long long)hook_frames(), c->r[0], c->r[1], c->r[2], c->r[3]);
}
void recomp_ihook_p5d_alloccsz(Cpu *c, int reg) {
	fprintf(stderr, "    [ALLOCSZ] frame %llu size=%#x\n", (unsigned long long)hook_frames(), c->r[0]);
}

/* M1362: catch the CRI assert/log calls inside the work-buffer allocation so the exact
 * failing condition message pointer is visible (fn_81278ee0 / fn_812a2412). */
void recomp_hook_p5d_asrt1(Cpu *c) {
	fprintf(stderr, "    [ASRT1] frame %llu r0=%#x r1=%#x r2=%#x\n", (unsigned long long)hook_frames(), c->r[0], c->r[1], c->r[2]);
}
void recomp_hook_p5d_asrt2(Cpu *c) {
	fprintf(stderr, "    [ASRT2] frame %llu r0=%#x r1=%#x r2=%#x\n", (unsigned long long)hook_frames(), c->r[0], c->r[1], c->r[2]);
}

/* M1363: exit-site probes of fn_812a3c40 (which branch returns 0 for the work-buffer alloc). */
static void p5d_exitsite(const char *tag, Cpu *c) { fprintf(stderr, "    [EXIT] frame %llu site=%s r0=%#x\n", (unsigned long long)hook_frames(), tag, c->r[0]); }
void recomp_ihook_p5d_exit1(Cpu *c, int reg) { p5d_exitsite("E0", c); }
void recomp_ihook_p5d_exit2(Cpu *c, int reg) { p5d_exitsite("E1", c); }
void recomp_ihook_p5d_exit3(Cpu *c, int reg) { p5d_exitsite("E2", c); }
void recomp_ihook_p5d_exit4(Cpu *c, int reg) { p5d_exitsite("E3", c); }
void recomp_ihook_p5d_exit5(Cpu *c, int reg) { p5d_exitsite("E4", c); }
void recomp_ihook_p5d_exit6(Cpu *c, int reg) { p5d_exitsite("E5", c); }
void recomp_ihook_p5d_exit7(Cpu *c, int reg) { p5d_exitsite("E6", c); }
void recomp_ihook_p5d_exit8(Cpu *c, int reg) { p5d_exitsite("E7", c); }
void recomp_ihook_p5d_exit9(Cpu *c, int reg) { p5d_exitsite("E8", c); }

/* M1364: CRI video decoder create wrapper fn_812a829a - log the registered backend
 * callback [obj+0xc] and arguments so we know whether the backend was registered. */
void recomp_hook_p5d_dec(Cpu *c) {
	uint32_t o = c->r[0];
	fprintf(stderr, "    [DEC] frame %llu obj=%#x cb=%#x a1=%#x a2=%#x\n", (unsigned long long)hook_frames(), o, o ? RD32(o + 0xc) : 0, c->r[1], c->r[2]);
}

/* M1364: return values of the HLE APIs used by the CRI video-decoder create (fn_812a9140). */
static void p5d_decapi(const char *tag, Cpu *c) { fprintf(stderr, "    [DECAPI] frame %llu %s ret=%#x\n", (unsigned long long)hook_frames(), tag, c->r[0]); }
void recomp_ihook_p5d_decapi1(Cpu *c, int reg) { p5d_decapi("ALLOCMB", c); }
void recomp_ihook_p5d_decapi2(Cpu *c, int reg) { p5d_decapi("GETBASE", c); }
void recomp_ihook_p5d_decapi3(Cpu *c, int reg) { p5d_decapi("QUERYMEM", c); }
void recomp_ihook_p5d_decapi4(Cpu *c, int reg) { p5d_decapi("AVCDEC", c); }
void recomp_ihook_p5d_decapi5(Cpu *c, int reg) { p5d_decapi("GXMMAP", c); }

/* M1365: CRI internal allocator fn_812abcd4 - r0 = allocator object; [ [r0] ] = the malloc
 * callback (E05063002H when missing). Log the chain and the result at both call sites. */
void recomp_hook_p5d_malloc1(Cpu *c) {
	uint32_t o = c->r[0];
	uint32_t vt = o ? RD32(o) : 0;
	uint32_t cb = vt ? RD32(vt) : 0;
	fprintf(stderr, "    [MALLOC] frame %llu obj=%#x vt=%#x cb=%#x\n", (unsigned long long)hook_frames(), o, vt, cb);
}
void recomp_ihook_p5d_mallet1(Cpu *c, int reg) { fprintf(stderr, "    [MALLOCRET1] frame %llu ret=%#x\n", (unsigned long long)hook_frames(), c->r[0]); }
void recomp_ihook_p5d_mallet2(Cpu *c, int reg) { fprintf(stderr, "    [MALLOCRET2] frame %llu ret=%#x\n", (unsigned long long)hook_frames(), c->r[0]); }

/* M1365: fn_812a9140 decoder-init path probes (sb check, globals 0x81540840/0x81540844, cleanup exit). */
static void p5d_probe(const char *tag, Cpu *c) { fprintf(stderr, "    [PROBE] frame %llu %s r0=%#x r1=%#x r2=%#x\n", (unsigned long long)hook_frames(), tag, c->r[0], c->r[1], c->r[2]); }
void recomp_ihook_p5d_probe1(Cpu *c, int reg) { p5d_probe("SBCHK", c); }
void recomp_ihook_p5d_probe2(Cpu *c, int reg) { p5d_probe("G840", c); }
void recomp_ihook_p5d_probe3(Cpu *c, int reg) { p5d_probe("G844", c); }
void recomp_ihook_p5d_probe4(Cpu *c, int reg) { p5d_probe("CLEANUP0", c); }

/* IHOOKs around the two UI-counter reads/increments (0x812584d2/0x812584e4 and 0x8125968a/0x8125969e):
 * the menu systems bump a global 0x8153f9b4 once per processed UI event. This diagnostic prints the
 * registers and the struct fields around the caller's object so the selection index can be located and,
 * later, written directly from the PC pointer (the low-level mouse->menu link). */
static void uiinc_dump(Cpu *c, const char *tag) {
	static uint32_t last; static uint64_t lastf; static int have;
	uint32_t r2 = c->r[2], r1 = c->r[1];
	uint64_t f = hook_frames();
	if (have && r2 == last && f - lastf < 300) return;
	have = 1; last = r2; lastf = f;
	fprintf(stderr, "    [%s] f=%llu r2=%u r1=%#x r3=%#x r4=%#x r5=%#x r7=%#x r9=%#x r10=%#x lr=%#x\n", tag,
	        (unsigned long long)f, r2, r1, c->r[3], c->r[4], c->r[5], c->r[7], c->r[9], c->r[10], c->r[14]);
	/* dump the first words of the candidate object pointers: the selected index is a small int (0..5)
	 * in one of these structures and changes exactly when the highlight moves */
	{ uint32_t pt[4] = { c->r[1], c->r[7], c->r[9], c->r[10] };
	  for (int q = 0; q < 4; q++) {
		if (!pt[q]) continue;
		fprintf(stderr, "        obj[%d] %#x:", q, pt[q]);
		for (int k = 0; k < 12; k++) fprintf(stderr, " %d", (int)RD32(pt[q] + 4 * k));
		fprintf(stderr, "\n");
	  } }
}
void recomp_ihook_p5d_uiinc1(Cpu *c, int reg) { (void)reg; uiinc_dump(c, "UIINC1"); }
void recomp_ihook_p5d_uiinc2(Cpu *c, int reg) { (void)reg; uiinc_dump(c, "UIINC2"); }
void recomp_ihook_p5d_uiread1(Cpu *c, int reg) { (void)reg; uiinc_dump(c, "UIREAD1"); }
void recomp_ihook_p5d_uiread2(Cpu *c, int reg) { (void)reg; uiinc_dump(c, "UIREAD2"); }

/* app process manager: the game looks scene processes up BY NAME (FUN_8116F900) and creates them through the job
 * queue (FUN_8116F9DC). Those names identify the scene in code ("commuMain", "MusicSelectMain", "mainMenuP5", ...).
 * These hooks publish the latest name plus a generation counter; window.c turns it into the scene. */
volatile char p5d_job_name[48];
volatile unsigned p5d_job_name_gen;
static void p5d_publish_job_name(const char *nm) {
    if (!nm) return;
    int i = 0;
    for (; i < 47; i++) { char c = nm[i]; if (!c) break; p5d_job_name[i] = c; }
    p5d_job_name[i] = 0;
    __atomic_add_fetch(&p5d_job_name_gen, 1, __ATOMIC_RELEASE);
}
static void p5d_diag_job_struct(const char *nm, uint32_t node);

void recomp_ihook_p5d_joblookup(Cpu *c, int reg) {
    (void)reg;
    static int once; if (!once) { once = 1; fprintf(stderr, "[JOBHOOK] lookup hook fired r0=%#x\n", c->r[0]); }
    uint32_t a = c->r[0];
    if (a > 0x10000u && a < 0x88000000u) {
        /* M1617: RECOMP_P5D_JOBLOG=1 prints each process/job name the game looks up, once, with its node and the
         * caller: the song-select screens' own app process can then be identified and disassembled. */
        { static int on = -1; if (on < 0) { const char *e = getenv("RECOMP_P5D_JOBLOG"); on = e && atoi(e) != 0; }
          if (on) {
              static char seen[192][40]; static int nseen;
              const char *nm = (const char *)G(a);
              int seen_before = 0;
              for (int k = 0; k < nseen; k++) if (!strcmp(seen[k], nm)) { seen_before = 1; break; }
              if (!seen_before && nseen < 192) {
                  int i = 0; for (; nm[i] && i < 39; i++) seen[nseen][i] = nm[i]; seen[nseen][i] = 0; nseen++;
                  fprintf(stderr, "[JOBLOG] name=%s ptr=%#x r1=%#x r2=%#x lr=%#x n=%d\n", nm, a, c->r[1], c->r[2], c->r[14], nseen);
              }
          } }
        p5d_publish_job_name((const char *)G(a));
        if (strstr((const char *)G(a), "onfig")) {
            static int n; if (n <= 3) { n++;
                fprintf(stderr, "[JOBDIAG] lookup name=%s ptr=%#x r1=%#x r2=%#x r3=%#x lr=%#x\n", (const char *)G(a), a, c->r[1], c->r[2], c->r[3], c->r[14]);
                fprintf(stderr, "    around name:");
                for (int k = -8; k <= 8; k++) fprintf(stderr, " %#x", RD32(a + 4 * k));
                fprintf(stderr, "\n    caller r2 words:");
                for (int k = 0; k < 24; k++) fprintf(stderr, " %#x", RD32(c->r[2] + 4 * k));
                fprintf(stderr, "\n"); }
        }
    }
}

void recomp_ihook_p5d_joblookup_diag(Cpu *c, int reg) {
    (void)reg;
    uint32_t a = c->r[0];
    if (a > 0x10000u && a < 0x88000000u) {
        const char *nm = (const char *)G(a);
        if (strstr(nm, "onfig")) {
            static int n; if (n <= 6) { n++;
                fprintf(stderr, "[JOBDIAG] lookup name=%s r1=%#x r2=%#x r3=%#x lr=%#x\n", nm, c->r[1], c->r[2], c->r[3], c->r[14]); }
        }
    }
}
void recomp_ihook_p5d_jobcreate(Cpu *c, int reg) {
    (void)reg;
    static int once; if (!once) { once = 1; fprintf(stderr, "[JOBHOOK] create hook fired r1=%#x\n", c->r[1]); }
    uint32_t st = c->r[1];
    if (st > 0x10000u && st < 0x88000000u) {
        uint32_t a = RD32(st);
        if (a > 0x10000u && a < 0x88000000u) { p5d_publish_job_name((const char *)G(a)); p5d_diag_job_struct((const char *)G(a), st); }
    }
}

/* the config/pause app's job struct, for the input rework: dump its first words once per name so the object
 * pointer and the per-page state field of the settings app can be found (the mainMenuP5 hook's node[+8]/obj[+4]
 * pattern: the app process node keeps the module object pointer, the module object keeps the page state). */
static void p5d_diag_job_struct(const char *nm, uint32_t node) {
    if (!nm || !node) return;
    int conf = strstr(nm, "onfig") != 0;
    int dance = strstr(nm, "eplay dance") || strstr(nm, "Danc") || strstr(nm, "dance") || strstr(nm, "Dancc");
    /* M1617: the song-select app process (its per-frame entry is in the node words). */
    int menuselect = strstr(nm, "MusicSelect") || strstr(nm, "music_select");
    if (!conf && !dance && !menuselect) return;
    static int n; if (n > 24) return; n++;
    fprintf(stderr, "[JOBDIAG] node=%#x name=%s conf=%d dance=%d\n", node, nm, conf, dance);
    for (int k = 0; k < 16; k++) {
        uint32_t v = RD32(node + 4 * k);
        fprintf(stderr, "    node[%2d] +%#04x = %#x%s\n", k, k * 4, v,
                (v >= 0x81000000u && v < 0x81600000u) ? " (code?)" : "");
    }
    uint32_t obj = RD32(node + 8);
    if (obj > 0x10000u && obj < 0x88000000u) {
        fprintf(stderr, "    obj=%#x code? words:", obj);
        for (int k = 0; k < 32; k++) {
            uint32_t v = RD32(obj + 4 * k);
            if (v >= 0x81000000u && v < 0x81600000u) fprintf(stderr, " [%d]%#x", k, v);
        }
        fprintf(stderr, "\n    obj words:");
        for (int k = 0; k < 32; k++) fprintf(stderr, " %d", (int)RD32(obj + 4 * k));
        fprintf(stderr, "\n");
    }
}

void recomp_ihook_p5d_jobcreate_diag(Cpu *c, int reg) {
    (void)reg;
    uint32_t st = c->r[1];
    if (st > 0x10000u && st < 0x88000000u) {
        uint32_t a = RD32(st);
        if (a > 0x10000u && a < 0x88000000u) p5d_diag_job_struct((const char *)G(a), st);
    }
}

/* the mainMenuP5 app process's own sub-page state: the page body 0x810FD49E runs every frame with r0 = its
 * process node (node[+8] = the 0x1048-byte module object), and dispatches its 35 sub-screens on object[+4].
 * This is the game's own code saying WHICH menu screen is up (main root / COMMU list / COMMUNITY story list /
 * music select / config / ...) - the authoritative code-level screen id the sequence page state cannot give
 * (all of them are page 5). window.c turns it into the Q/E mode. */
volatile int p5d_mp_state = -1;
volatile unsigned p5d_mp_gen;
volatile uint32_t p5d_mp_obj;   /* the main-menu module object, captured from the process entry once */
volatile unsigned p5d_mp_calls;  /* the hooks' call counter: the window sees how fresh the state is */
void recomp_ihook_p5d_menupage(Cpu *c, int reg) {
    (void)reg;
    uint32_t node = c->r[0];
    static int once;
    if (!once) { once = 1; uint32_t o = (node > 0x10000u && node < 0x88000000u) ? RD32(node + 8) : 0;
        fprintf(stderr, "[MPHOOK] fired r0=%#x obj=%#x st=%d\n", node, o, (o > 0x10000u && o < 0x88000000u) ? (int)RD32(o + 4) : -1); }
    if (node < 0x10000u || node >= 0x88000000u) return;
    uint32_t obj = RD32(node + 8);
    if (obj < 0x10000u || obj >= 0x88000000u) return;
    p5d_mp_obj = obj;
    uint32_t st = RD32(obj + 4);
    if (st > 0x3fu) return;
    __atomic_add_fetch(&p5d_mp_calls, 1, __ATOMIC_RELEASE);
    if ((int)st != p5d_mp_state) { p5d_mp_state = (int)st; __atomic_add_fetch(&p5d_mp_gen, 1, __ATOMIC_RELEASE); }
}


/* M1530 diagnostic: calls the analog accessor fn_811da7a8 (0x811991be); RECOMP_XTRACE=1 logs its call rate so the viewer's own input path
 * can be told apart from the menu/other scenes' paths. */
void recomp_hook_p5d_xt_be(Cpu *c) {
	static uint64_t last; static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[XTR] 0x811991be frame %llu lr=%#x\n", (unsigned long long)f, c->r[14]); last = f; }
}

/* M1530 diagnostic: calls fn_811da822/fn_811da900 (0x811db020); RECOMP_XTRACE=1 logs its call rate so the viewer's own input path
 * can be told apart from the menu/other scenes' paths. */
void recomp_hook_p5d_xt_020(Cpu *c) {
	static uint64_t last; static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[XTR] 0x811db020 frame %llu lr=%#x\n", (unsigned long long)f, c->r[14]); last = f; }
}

/* M1530 diagnostic: calls fn_811da822/fn_811da900 (0x811db3f2); RECOMP_XTRACE=1 logs its call rate so the viewer's own input path
 * can be told apart from the menu/other scenes' paths. */
void recomp_hook_p5d_xt_3f2(Cpu *c) {
	static uint64_t last; static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[XTR] 0x811db3f2 frame %llu lr=%#x\n", (unsigned long long)f, c->r[14]); last = f; }
}

/* M1530 diagnostic: reads the cached stick Y directly (0x811a628e); RECOMP_XTRACE=1 logs its call rate so the viewer's own input path
 * can be told apart from the menu/other scenes' paths. */
void recomp_hook_p5d_xt_28e(Cpu *c) {
	static uint64_t last; static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[XTR] 0x811a628e frame %llu lr=%#x\n", (unsigned long long)f, c->r[14]); last = f; }
}

/* M1530 diagnostic: reads the cached stick X directly (0x811a6818); RECOMP_XTRACE=1 logs its call rate so the viewer's own input path
 * can be told apart from the menu/other scenes' paths. */
void recomp_hook_p5d_xt_818(Cpu *c) {
	static uint64_t last; static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[XTR] 0x811a6818 frame %llu lr=%#x\n", (unsigned long long)f, c->r[14]); last = f; }
}

/* M1530 diagnostic: reads both cached sticks directly (0x811a76c2); RECOMP_XTRACE=1 logs its call rate so the viewer's own input path
 * can be told apart from the menu/other scenes' paths. */
void recomp_hook_p5d_xt_6c2(Cpu *c) {
	static uint64_t last; static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[XTR] 0x811a76c2 frame %llu lr=%#x\n", (unsigned long long)f, c->r[14]); last = f; }
}

/* M1530 diagnostic: reads both cached sticks directly (0x811c36e2); RECOMP_XTRACE=1 logs its call rate so the viewer's own input path
 * can be told apart from the menu/other scenes' paths. */
void recomp_hook_p5d_xt_6e2(Cpu *c) {
	static uint64_t last; static int on = -1, on2 = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE"); on = e && atoi(e) != 0; }
	if (on2 < 0) { const char *e = getenv("RECOMP_XTRACE2"); on2 = e && atoi(e) != 0; }
	extern uint64_t display_frames(void);
	if (on2) {
		uint64_t f = display_frames();
		if (f - last >= 15) { last = f;
			uint32_t sb = c->r[1], fl = (sb ? RD32(sb + 0x28) : 0xffffffffu);
			fprintf(stderr, "[CTRLDATA] f=%llu sb=%#x flags28=%#x lx=%u ly=%u rx=%u ry=%u btn=%#x\n",
			        (unsigned long long)f, sb, fl, RD8(0x816cdaacu), RD8(0x816cdaadu), RD8(0x816cdaaeu), RD8(0x816cdaafu), RD16(0x816cdfe8u));
		}
		return;
	}
	if (!on) return;
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[XTR] 0x811c36e2 frame %llu lr=%#x\n", (unsigned long long)f, c->r[14]); last = f; }
}

/* M1530 diagnostic: reads the cached stick globals (0x8116f4f8); RECOMP_XTRACE=1 logs its call rate so the viewer's own input path
 * can be told apart from the menu/other scenes' paths. */
void recomp_hook_p5d_xt_4f8(Cpu *c) {
	static uint64_t last; static int on = -1, on2 = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE"); on = e && atoi(e) != 0; }
	if (on2 < 0) { const char *e = getenv("RECOMP_XTRACE2"); on2 = e && atoi(e) != 0; }
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (on2) {
		if (f - last >= 15) { last = f;
			fprintf(stderr, "[POSTIN] f=%llu a0=%02x a4=%02x a8=%02x a9=%02x aa=%02x ab=%02x ac=%02x ad=%02x ae=%02x af=%02x b0=%08x b4=%08x\n",
			        (unsigned long long)f, RD8(0x816cdaa0u), RD8(0x816cdaa4u), RD8(0x816cdaa8u), RD8(0x816cdaa9u),
			        RD8(0x816cdaaau), RD8(0x816cdaabu), RD8(0x816cdaacu), RD8(0x816cdaadu), RD8(0x816cdaaeu), RD8(0x816cdaafu),
			        RD32(0x816cdfb0u), RD32(0x816cdfb4u));
		}
		return;
	}
	/* M1531: late-frame republish of the watch-mode sticks into the game's cached analog globals (the input
	 * module leaves them neutral there). Gated by RECOMP_WATCHLS=1 so normal play is untouched. */
	{ static int on3 = -1; if (on3 < 0) { const char *e = getenv("RECOMP_WATCHLS"); on3 = e && atoi(e) != 0; }
	  if (on3) { extern int window_watch_sticks(unsigned char *s); unsigned char ws[4];
		if (window_watch_sticks(ws)) {
			WR8(0x816cdaa8u, ws[0]); WR8(0x816cdaa9u, ws[1]);
			WR8(0x816cdaacu, ws[2]); WR8(0x816cdaadu, ws[3]);
		} } }
	if (!on) return;
	if (f - last >= 6) { fprintf(stderr, "[XTR] 0x8116f4f8 frame %llu lr=%#x\n", (unsigned long long)f, c->r[14]); last = f; }
}

/* M1530 diagnostic: reads the cached stick globals (0x810db1e4); RECOMP_XTRACE=1 logs its call rate so the viewer's own input path
 * can be told apart from the menu/other scenes' paths. */
void recomp_hook_p5d_xt_1e4(Cpu *c) {
	static uint64_t last; static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[XTR] 0x810db1e4 frame %llu lr=%#x\n", (unsigned long long)f, c->r[14]); last = f; }
}

/* M1530 diagnostic: analog accessor (0x811da738); RECOMP_XTRACE=1 logs its call rate so the viewer's own input path
 * can be told apart from the menu/other scenes' paths. */
void recomp_hook_p5d_xt_738(Cpu *c) {
	static uint64_t last; static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[XTR] 0x811da738 frame %llu lr=%#x\n", (unsigned long long)f, c->r[14]); last = f; }
}

/* M1530 diagnostic: analog accessor (0x811da7a8); RECOMP_XTRACE=1 logs its call rate so the viewer's own input path
 * can be told apart from the menu/other scenes' paths. */
void recomp_hook_p5d_xt_7a8(Cpu *c) {
	static uint64_t last; static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[XTR] 0x811da7a8 frame %llu lr=%#x\n", (unsigned long long)f, c->r[14]); last = f; }
}

/* M1530 diagnostic: analog accessor (0x811da822); RECOMP_XTRACE=1 logs its call rate so the viewer's own input path
 * can be told apart from the menu/other scenes' paths. */
void recomp_hook_p5d_xt_822(Cpu *c) {
	static uint64_t last; static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[XTR] 0x811da822 frame %llu lr=%#x\n", (unsigned long long)f, c->r[14]); last = f; }
}

/* M1530 diagnostic: analog accessor (0x811da900); RECOMP_XTRACE=1 logs its call rate so the viewer's own input path
 * can be told apart from the menu/other scenes' paths. */
void recomp_hook_p5d_xt_900(Cpu *c) {
	static uint64_t last; static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[XTR] 0x811da900 frame %llu lr=%#x\n", (unsigned long long)f, c->r[14]); last = f; }
}

/* M1530 diagnostic: the game's input update (pad -> edge/held/analog globals) (0x811da9a4); fires every frame when the game polls the pad. */
void recomp_hook_p5d_xt_9a4(Cpu *c) {
	static uint64_t last; static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[XTR] 0x811da9a4 frame %llu lr=%#x\n", (unsigned long long)f, c->r[14]); last = f; }
}

/* M1530 diagnostic: the game's SceCtrl read helper (0x811e6a78); fires every frame when the game polls the pad. */
void recomp_hook_p5d_xt_6a8(Cpu *c) {
	static uint64_t last; static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[XTR] 0x811e6a78 frame %llu lr=%#x\n", (unsigned long long)f, c->r[14]); last = f; }
}

/* M1532: IHOOK at 0x811c3760, right after fn_811c36e2 has centred the analog bytes: r2=a8-0x80, r3=a9-0x80,
 * r7=ac-0x80, r8=ad-0x80 (the two stick pairs), r14 = flag word [sb+0x28], r9 = sb. */
void recomp_ihook_p5d_ctrlin(Cpu *c, int reg) {
	static uint64_t last; static int on = -1;
	(void)reg;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE3"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 15) { last = f;
		uint32_t sb = c->r[9];
		fprintf(stderr, "[CTRLIN] f=%llu sb=%#x flags=%#x d_a8=%d d_a9=%d d_ac=%d d_ad=%d raw a8=%02x a9=%02x ac=%02x ad=%02x\n",
		        (unsigned long long)f, sb, c->r[14], (int)c->r[2], (int)c->r[3], (int)c->r[7], (int)c->r[8],
		        RD8(0x816cdaa8u), RD8(0x816cdaa9u), RD8(0x816cdaacu), RD8(0x816cdaadu));
	}
}

/* M1533 diagnostic: COLLECTION character viewer per-frame update wrapper (0x8109a192); RECOMP_XTRACE4=1 logs its call rate in the viewer. */
void recomp_hook_p5d_xt_vwr(Cpu *c) {
	static uint64_t last; static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE4"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[VTR] 0x8109a192 f=%llu lr=%#x r0=%#x\n", (unsigned long long)f, c->r[14], c->r[0]); last = f; }
}

/* M1533 diagnostic: engine helper called by the viewer tick (0x8115d43c); RECOMP_XTRACE4=1 logs its call rate in the viewer. */
void recomp_hook_p5d_xt_43c(Cpu *c) {
	static uint64_t last; static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE4"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[VTR] 0x8115d43c f=%llu lr=%#x r0=%#x\n", (unsigned long long)f, c->r[14], c->r[0]); last = f; }
}

/* M1533 diagnostic: engine helper called by the viewer tick (0x8115d614); RECOMP_XTRACE4=1 logs its call rate in the viewer. */
void recomp_hook_p5d_xt_614(Cpu *c) {
	static uint64_t last; static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE4"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[VTR] 0x8115d614 f=%llu lr=%#x r0=%#x\n", (unsigned long long)f, c->r[14], c->r[0]); last = f; }
}

/* M1533 diagnostic: engine helper called by the viewer tick (0x8115f832); RECOMP_XTRACE4=1 logs its call rate in the viewer. */
void recomp_hook_p5d_xt_832(Cpu *c) {
	static uint64_t last; static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE4"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[VTR] 0x8115f832 f=%llu lr=%#x r0=%#x\n", (unsigned long long)f, c->r[14], c->r[0]); last = f; }
}

/* M1533 diagnostic: engine helper called by the viewer tick (0x81179d30); RECOMP_XTRACE4=1 logs its call rate in the viewer. */
void recomp_hook_p5d_xt_79d(Cpu *c) {
	static uint64_t last; static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE4"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 6) { fprintf(stderr, "[VTR] 0x81179d30 f=%llu lr=%#x r0=%#x\n", (unsigned long long)f, c->r[14], c->r[0]); last = f; }
}

/* M1533: IHOOK mechanism check inside fn_811da9a4 (input update); fires every frame and prints the pad bytes
 * the game's own input code is working on. */
void recomp_ihook_p5d_xt_i9a4(Cpu *c, int reg) {
	static uint64_t last; static int on = -1;
	(void)reg;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE4"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 15) { last = f;
		fprintf(stderr, "[I9A4] f=%llu a8=%02x a9=%02x ac=%02x ad=%02x sp4=%#x\n", (unsigned long long)f,
		        RD8(0x816cdaa8u), RD8(0x816cdaa9u), RD8(0x816cdaacu), RD8(0x816cdaadu), RD32(c->r[13] + 4));
	}
}

/* M1534: IHOOK at 0x811daa6e, right after the input update stored the analog bytes at [r7] and [r6]. */
void recomp_ihook_p5d_xt_st9(Cpu *c, int reg) {
	static uint64_t last; static int on = -1;
	(void)reg;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE5"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (f - last >= 15) { last = f;
		uint32_t r7 = c->r[7], r6 = c->r[6], r8 = c->r[8], r10 = c->r[10];
		fprintf(stderr, "[ST9] f=%llu it=%u r7=%#x r6=%#x r8=%#x r10=%#x", (unsigned long long)f, c->r[9], r7, r6, r8, r10);
		fprintf(stderr, " [r7]=%02x%02x [r6]=%02x%02x [r8]=%02x%02x [r10]=%02x%02x", RD8(r7), RD8(r7 + 1), RD8(r6), RD8(r6 + 1), RD8(r8), RD8(r8 + 1), RD8(r10), RD8(r10 + 1));
		fprintf(stderr, " cache a8..af:");
		for (uint32_t a = 0x816cdaa8u; a < 0x816cdab0u; a++) fprintf(stderr, " %02x", RD8(a));
		fprintf(stderr, "\n");
	}
}

/* M1535: IHOOK at 0x811daa0e, right after fn_811e6a78 (the game's pad-read helper) returned; its output
 * structure starts at [sp+4]. RECOMP_XTRACE6=1 dumps it. */
void recomp_ihook_p5d_xt_out(Cpu *c, int reg) {
	static uint64_t last; static int on = -1, restore = -1;
	(void)reg;
	if (on < 0) { const char *e = getenv("RECOMP_XTRACE6"); on = e && atoi(e) != 0; }
	if (restore < 0) { const char *e = getenv("RECOMP_RESTORE"); restore = !(e && atoi(e) == 0); }   /* on by default */
	if (restore) {
		/* M1536: the pad-read helper left the destination-pointer registers r4/r6 at 0, so the input update
		 * below stored the analog bytes through null pointers (left stick never published). Restore them. */
		if (!c->r[6]) c->r[6] = 0x816cdaa8u;
		if (!c->r[4]) c->r[4] = 0x81680148u;
	}
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	if (on) {
		uint32_t p = c->r[13] + 4;
		if (f - last >= 30) { last = f;
			fprintf(stderr, "[OUT] f=%llu r6=%#x r7=%#x r8=%#x r10=%#x r4=%#x buf:", (unsigned long long)f, c->r[6], c->r[7], c->r[8], c->r[10], c->r[4]);
			for (uint32_t i = 0; i < 12; i++) fprintf(stderr, " %02x", RD8(p + i));
			fprintf(stderr, "\n");
		}
	}
}

/* M1540 60 fps investigation: the engine delta time. fn_8119957a returns the float at 0x816cc630+8, the value
 * danceMain_81043FEE (0x81003e70 call) adds to the dance clock and most per-frame systems multiply by. At
 * RECOMP_FRAME_RATE=60 the question is whether it reports the real frame time (1/60) or the fixed 1/30 step;
 * RECOMP_P5D_DT_LOG=1 logs it (with the presentation frame and the frame-rate mode) every 60 frames. */
void recomp_ihook_p5d_enginedt(Cpu *c, int reg) {
	(void)reg;
	static int on = -1; static uint64_t last;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_DT_LOG"); on = e && atoi(e) != 0; }
	if (!on) return;
	uint64_t f = display_frames();
	static unsigned every = 0;
	if (!every) every = 6;   /* M1586: fine-grained dance clock logging for run alignment (was 60) */
	if (f - last < every) return;
	last = f;
	float dt; memcpy(&dt, &c->vfp.s[0], 4);
	fprintf(stderr, "[DT] frame %llu dt=%.6f rate=%s\n", (unsigned long long)f, dt,
	        display_frame_rate_60() ? "60" : "30");
}

/* M1540 experiment: appMframe_8102a780(1, 1, 1) initialises the animation/frame module at boot; the singleton at
 * 0x8132ae64 keeps the three arguments and its +8 word is the frame base index into the game's {1.0, 30.0, 60.0}
 * table. In 60 fps mode pass 2 (60 fps) as the third argument instead of 1 (30 fps), i.e. let the engine itself
 * retime its frame-based animation/timeline code. RECOMP_P5D_MFRAME60=0 turns the experiment off. */
/* M1577/M1578: the boot frame-rate setup calls fn_81154a1c(30,30) right after appMframe init(1,1,1); the PS4 build of
 * the shared engine most likely passes 60 there. RECOMP_P5D_APPFPS60=1 (60 fps presentation only) rewrites the boot
 * call's arguments to 60,60 so the animation calculator knows the presentation rate. */
void recomp_hook_p5d_appfps(Cpu *c) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_APPFPS60"); on = e && atoi(e) != 0; }
	if (!on || !display_frame_rate_60()) return;
	if (c->r[14] == 0x811846b5u) {   /* the boot call site in the app framework */
		/* fn_81154a1c allocates an 8-byte {u32,u32} object and stores it at 0x813af5a8: the two values are the app
		 * framework's frame-rate pair. RECOMP_P5D_APPFPS="a,b" tests candidate pairs (default 60,60). */
		unsigned a = 60, b = 60;
		{ const char *cfg = getenv("RECOMP_P5D_APPFPS"); if (cfg) sscanf(cfg, "%u,%u", &a, &b); }
		fprintf(stderr, "[APPFPS60] boot fn_81154a1c(%u,%u) -> (%u,%u)\n", c->r[0], c->r[1], a, b);
		c->r[0] = a; c->r[1] = b;
	}
}
/* M1603: dance-system entry probes. The job-struct diagnostic identified the dance process entries; this probe logs
 * their call frame, r0, the float input (s0) and the first words of the module object (node+8) so the animation clock can
 * be identified: a value advancing per presented frame (same delta at 30 and 60 fps) is the frame-index time base.
 * RECOMP_P5D_DANCE_PROBE=1, logs every 10 presented frames. */
static void p5d_dance_probe(const char *tag, Cpu *c) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_DANCE_PROBE"); on = e && atoi(e) != 0; }
	if (!on) return;
	static uint64_t last[5];
	int id = tag[0] == 'p' && tag[1] == 'o' ? 0 : tag[0] == 'p' ? 1 : tag[0] == 'r' ? 2 : tag[0] == 's' ? 3 : 4;
	uint64_t f = display_frames();
	if (f - last[id] < 10) return;
	last[id] = f;
	float s0 = 0; memcpy(&s0, &c->vfp.s[0], 4);
	uint32_t node = c->r[0], obj = 0;
	if (node > 0x10000u && node < 0x88000000u) obj = RD32(node + 8);
	fprintf(stderr, "[DANCE] %s f=%llu r0=%#x obj=%#x s0=%.4f w0=%d w1=%d w2=%d w3=%d w4=%d w5=%d rate=%s\n",
	        tag, (unsigned long long)f, node, obj, s0,
	        obj ? (int)RD32(obj) : -1, obj ? (int)RD32(obj + 4) : -1, obj ? (int)RD32(obj + 8) : -1,
	        obj ? (int)RD32(obj + 12) : -1, obj ? (int)RD32(obj + 16) : -1, obj ? (int)RD32(obj + 20) : -1,
	        display_frame_rate_60() ? "60" : "30");
	/* M1624: full object dump for the frame-based clock hunt (RECOMP_P5D_DANCE_DUMP=1). */
	{
		static int on = -1;
		if (on < 0) { const char *e = getenv("RECOMP_P5D_DANCE_DUMP"); on = e && atoi(e) != 0; }
		if (on && obj > 0x10000u && obj < 0x88000000u) {
			fprintf(stderr, "[DD] %s f=%llu obj=%#x", tag, (unsigned long long)f, obj);
			for (int k = 0; k < 64; k++) fprintf(stderr, " %d", (int)RD32(obj + 4 * k));
			fprintf(stderr, "\n");
		}
	}
}
void recomp_hook_p5d_dance_pose(Cpu *c) { p5d_dance_probe("pose", c); }
void recomp_hook_p5d_dance_proc(Cpu *c) { p5d_dance_probe("proc", c); }
void recomp_hook_p5d_dance_ready(Cpu *c) { p5d_dance_probe("ready", c); }
void recomp_hook_p5d_dance_sys(Cpu *c) { p5d_dance_probe("sys", c); }
void recomp_hook_p5d_dance_voice(Cpu *c) { p5d_dance_probe("voice", c); }
void recomp_hook_p5d_mframe_init(Cpu *c) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_MFRAME60"); on = e ? atoi(e) != 0 : 1; }   /* M1606: promoted - on in the 60 fps mode, RECOMP_P5D_MFRAME60=0 disables */
	/* the boot call is the one this hook is registered on; only in 60 fps presentation mode */
	if (!on || !display_frame_rate_60()) return;
	if (c->r[0] == 1 && c->r[1] == 1) {
		/* M-B experiment: RECOMP_P5D_MFRAME_INIT="a,b,c" sets the three boot init arguments (default 1,1,2) */
		unsigned ia = 1, ib = 1, ic = 2;
		{ const char *e = getenv("RECOMP_P5D_MFRAME_INIT");
		  if (e) sscanf(e, "%u,%u,%u", &ia, &ib, &ic); }
		c->r[0] = ia; c->r[1] = ib; c->r[2] = ic;
		fprintf(stderr, "[MFRAME60] init(1,1,1) -> init(%u,%u,%u) frame base experiment\n", c->r[0], c->r[1], c->r[2]);
	}
}



/* M1627: in-run frame-counter hunter for the song-select character. While the song screen is up, take two memory
 * snapshots GAP frames apart, find the words that advanced exactly +1 per presented frame (the frame-based counters),
 * and then advance the selected ones at half rate (decrement on odd frames) so a character animation driven by one of
 * them runs at real time at 60 fps. RECOMP_P5D_AUTOHALF=1 (with RECOMP_P5D_AUTOHALF_AT default 150, _GAP default 60,
 * _SKIP/_TAKE to select a slice of the candidate list for bisection; default all). Logs the candidate list once. */
extern int window_p5d_song_screen(void);
struct p5d_ah_region { uint32_t addr, len; };
static struct p5d_ah_region p5d_ah_regions[] = { {0x81000000u, 0x00800000u}, {0x10000000u, 0x02000000u}, {0x13000000u, 0x01000000u} };
#define P5D_AH_NREG (sizeof p5d_ah_regions / sizeof p5d_ah_regions[0])
static uint8_t *p5d_ah_snap[P5D_AH_NREG];
static uint32_t p5d_ah_cand[4096]; static int p5d_ah_ncand; static int p5d_ah_active;
static int p5d_ah_istype[4096]; /* 0 int (+1/frame), 1 float time (+1/30 per frame), 2 float time (+1/60) */
static int p5d_ah_want_type = -1;  /* -1 all, 0 ints, 1 floats (either unit) */
static uint32_t p5d_ah_addrs[64]; static int p5d_ah_naddrs;  /* explicit address list (comma separated hex), empty = use discovery */
static float p5d_ah_v0[4096]; static uint64_t p5d_ah_act_f; static int p5d_ah_abs = -1;  /* absolute mode: v = v0 + (inc/2)*elapsed */
static int p5d_ah_vmax = -1;  /* only counters whose value is below this (newly created objects) */
static int p5d_ah_skip = 0, p5d_ah_take = 1 << 30;  /* candidate slice (bisection) */
static int p5d_ah_jumps = -1, p5d_ah_win = 20, p5d_ah_gap = 10;  /* single-counter perturbation probe */
static int p5d_ah_freeze = 0; static float p5d_ah_frz[512];  /* freeze mode: hold the selected counter for its window */
static int p5d_ah_watchfreeze = -1; static void *p5d_ah_fzwatch_h;  /* combined probe: watch the frozen counter's page after its window */
static void p5d_autohalf_apply(void);
static void p5d_wwatch_tick(void);
extern int wwatch_protect(void *host_ptr);
extern int wwatch_unprotect(void *host_ptr);
extern void wwatch_set_target(void *host_ptr);
static void p5d_autohalf_tick(void) {
	static int on = -1, at = -1, gap = -1, skip = -1, take = -1;
	static int phase; static long t1 = -1, t2 = -1; static uint64_t song_start;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_AUTOHALF"); on = e && atoi(e) != 0; }
	if (!on) return;
	if (at < 0) {
		const char *e;
		at = (e = getenv("RECOMP_P5D_AUTOHALF_AT")) ? atoi(e) : 150;
		gap = (e = getenv("RECOMP_P5D_AUTOHALF_GAP")) ? atoi(e) : 60;
		p5d_ah_skip = (e = getenv("RECOMP_P5D_AUTOHALF_SKIP")) ? atoi(e) : 0;
		p5d_ah_take = (e = getenv("RECOMP_P5D_AUTOHALF_TAKE")) ? atoi(e) : 1 << 30;
		{ const char *al = getenv("RECOMP_P5D_AUTOHALF_ADDRS"); p5d_ah_naddrs = 0;
		  if (al && *al) { const char *q = al; while (*q && p5d_ah_naddrs < 64) { p5d_ah_addrs[p5d_ah_naddrs++] = (uint32_t)strtoul(q, (char **)&q, 0); if (*q == ',' || *q == ' ') q++; } } }
		{ const char *ab = getenv("RECOMP_P5D_AUTOHALF_ABS"); p5d_ah_abs = ab ? atoi(ab) : 1; }
		{ const char *vm = getenv("RECOMP_P5D_AUTOHALF_VMAX"); p5d_ah_vmax = vm ? atoi(vm) : -1; }
		{ const char *jp = getenv("RECOMP_P5D_AUTOHALF_JUMPS"); p5d_ah_jumps = jp ? atoi(jp) : 0;
		  { const char *fz = getenv("RECOMP_P5D_AUTOHALF_FREEZE"); p5d_ah_freeze = fz ? atoi(fz) : 0; }
		  { const char *wf = getenv("RECOMP_P5D_WWATCH"); p5d_ah_watchfreeze = wf ? atoi(wf) : 0; }
		  const char *w = getenv("RECOMP_P5D_AUTOHALF_WIN"); if (w) p5d_ah_win = atoi(w);
		  const char *g = getenv("RECOMP_P5D_AUTOHALF_GAP"); if (g) p5d_ah_gap = atoi(g); }
		{ const char *te = getenv("RECOMP_P5D_AUTOHALF_TYPE"); p5d_ah_want_type = te ? (!strcmp(te, "float") ? 1 : !strcmp(te, "f30") ? 2 : 0) : -1; }
	}
	uint64_t f = display_frames();
	if (phase == 0) {
		if (!window_p5d_song_screen()) return;
		song_start = f; t1 = (long)f + at; t2 = t1 + gap; phase = 1;
	} else if (phase == 1) {
		if ((long)f >= t1) {
			for (unsigned i = 0; i < P5D_AH_NREG; i++) {
				if (!p5d_ah_snap[i]) p5d_ah_snap[i] = malloc(p5d_ah_regions[i].len);
				if (p5d_ah_snap[i]) memcpy(p5d_ah_snap[i], G(p5d_ah_regions[i].addr), p5d_ah_regions[i].len);
			}
			phase = 2;
		}
	} else if (phase == 2) {
		if ((long)f >= t2) {
			if (p5d_ah_naddrs) { p5d_ah_active = 1; phase = 3; fprintf(stderr, "[AUTOHALF] explicit addrs=%d\n", p5d_ah_naddrs); return; }
			p5d_ah_ncand = 0;
			for (unsigned i = 0; i < P5D_AH_NREG && p5d_ah_ncand < 4000; i++) {
				if (!p5d_ah_snap[i]) continue;
				const uint32_t *a = (const uint32_t *)p5d_ah_snap[i];
				const uint32_t *b = (const uint32_t *)G(p5d_ah_regions[i].addr);
				uint32_t n = p5d_ah_regions[i].len / 4;
				for (uint32_t k = 0; k < n && p5d_ah_ncand < 4000; k++) {
					uint32_t da = b[k] - a[k];
					if (da == (uint32_t)gap && b[k] < 4000000u) {
						p5d_ah_istype[p5d_ah_ncand] = 0;
						p5d_ah_cand[p5d_ah_ncand++] = p5d_ah_regions[i].addr + 4 * k;
						continue;
					}
					float fa, fb;
					memcpy(&fa, &a[k], 4); memcpy(&fb, &b[k], 4);
					float d = fb - fa;
					float t30 = (float)gap / 30.0f, t60 = (float)gap / 60.0f;
					if (d > 0.0f && (fabsf(d - t30) < 0.002f || fabsf(d - t60) < 0.002f)) {
						p5d_ah_istype[p5d_ah_ncand] = (fabsf(d - t30) < 0.002f) ? 1 : 2;
						p5d_ah_cand[p5d_ah_ncand++] = p5d_ah_regions[i].addr + 4 * k;
					}
				}
			}
			fprintf(stderr, "[AUTOHALF] candidates=%d skip=%d take=%d type=%d:", p5d_ah_ncand, p5d_ah_skip, p5d_ah_take, p5d_ah_want_type);
			for (int k = 0; k < p5d_ah_ncand; k++) fprintf(stderr, " %s%#x", p5d_ah_istype[k]==0?"i":(p5d_ah_istype[k]==1?"f30:":"f60:"), p5d_ah_cand[k]);
			fprintf(stderr, "\n");
			for (int k = 0; k < p5d_ah_ncand; k++) { float v; uint32_t w = RD32(p5d_ah_cand[k]); memcpy(&v, &w, 4); p5d_ah_v0[k] = v; }
			if (p5d_ah_naddrs) for (int k = 0; k < p5d_ah_naddrs; k++) { float v; uint32_t w = RD32(p5d_ah_addrs[k]); memcpy(&v, &w, 4); p5d_ah_v0[k] = v; }
			p5d_ah_act_f = f;
			p5d_ah_active = 1; phase = 3;
		}
	}
	if (p5d_ah_jumps && phase == 3 && p5d_ah_active) {
		long seg = (long)f - t2;
		long per = (long)p5d_ah_win + (long)p5d_ah_gap;
		long wi = seg / per, pos = seg % per;
		int nsel = 0; int sel[512];
		for (int k = 0; k < p5d_ah_ncand && nsel < 512; k++) {
			if (p5d_ah_want_type == 0 && p5d_ah_istype[k] != 0) continue;
			if (p5d_ah_want_type == 1 && p5d_ah_istype[k] == 0) continue;
			if (p5d_ah_want_type == 2 && p5d_ah_istype[k] != 1) continue;
			sel[nsel++] = k;
		}
		if (p5d_ah_freeze) {
			if (wi < nsel && pos == 0) {
				int k = sel[wi];
				uint32_t a = p5d_ah_cand[k];
				float v; uint32_t w = RD32(a); memcpy(&v, &w, 4);
				p5d_ah_frz[wi] = v;
				if (p5d_ah_fzwatch_h) { wwatch_unprotect(p5d_ah_fzwatch_h); p5d_ah_fzwatch_h = 0; }
				fprintf(stderr, "[FREEZE] start wi=%ld idx=%d addr=%#x v=%.4f f=%llu\n", wi, k, a, v, (unsigned long long)f);
				/* M1639: dump the object around the counter and the globals that point at it, to find its writer. */
				{
					uint32_t obase = a & ~0x3fu;
					fprintf(stderr, "[FRZDUMP] wi=%ld base=%#x", wi, obase);
					for (int q = -16; q < 48; q++) fprintf(stderr, " %d", (int)RD32(obase + 4 * q));
					fprintf(stderr, "\n");
					fprintf(stderr, "[FRZREF] wi=%ld", wi);
					for (uint32_t ga = 0x81000000u; ga < 0x81800000u; ga += 4) {
						uint32_t wv = RD32(ga);
						if (wv > obase - 0x1000u && wv < obase + 0x1000u) fprintf(stderr, " %#x->%#x", ga, wv);
					}
					fprintf(stderr, "\n");
				}
			} else if (wi < nsel && pos < (long)p5d_ah_win) {
				int k = sel[wi];
				uint32_t a = p5d_ah_cand[k];
				uint32_t w; float v = p5d_ah_frz[wi]; memcpy(&w, &v, 4); WR32(a, w);
			} else if (wi < nsel && pos == (long)p5d_ah_win) {
				fprintf(stderr, "[FREEZE] end wi=%ld f=%llu\n", wi, (unsigned long long)f);
				if (p5d_ah_watchfreeze) {
					int k = sel[wi];
					void *hp = G(p5d_ah_cand[k]);
					wwatch_set_target(hp);
					if (wwatch_protect(hp)) {
						p5d_ah_fzwatch_h = hp;
						fprintf(stderr, "[FZWATCH] wi=%ld cand=%#x host=%p f=%llu\n", wi, p5d_ah_cand[k], hp, (unsigned long long)f);
					}
				}
			}
		} else if (pos == 0 && wi < nsel) {
			int k = sel[wi];
			uint32_t a = p5d_ah_cand[k];
			float v; uint32_t w = RD32(a); memcpy(&v, &w, 4);
			v += 1.0f; memcpy(&w, &v, 4); WR32(a, w);
			fprintf(stderr, "[JUMP] wi=%ld idx=%d addr=%#x from=%.4f to=%.4f f=%llu\n", wi, k, a, v - 1.0f, v, (unsigned long long)f);
		}
		/* M1647: after the freeze pass, watch each frozen candidate's page (target-address mode) so the exact writer of
		 * the counter that stopped the character is logged. */
		if (p5d_ah_watchfreeze && p5d_ah_freeze && wi >= (long)nsel) {
			static int wi2; static void *h2;
			if (wi2 < nsel) {
				int k = sel[wi2];
				int cand = k;
				(void)cand;
				uint32_t a = p5d_ah_cand[k];
				if (h2) { wwatch_unprotect(h2); wwatch_set_target(0); h2 = 0; }
				void *hp = G(a);
				wwatch_set_target(hp);
				if (wwatch_protect(hp)) {
					h2 = hp;
					fprintf(stderr, "[FZWATCH2] i=%d cand=%#x host=%p f=%llu\n", wi2, a, hp, (unsigned long long)f);
				}
				wi2++;
			}
		}
	}
	p5d_autohalf_apply();
	p5d_wwatch_tick();
}
/* M1633: the half-rate application runs from the draw path (gxm_sony o_draw), i.e. after the guest's
 * per-frame animation update and before the present: decrementing there halves the clock for the next
 * frame without fighting the game's own writes in the same frame (which jittered when applied from the
 * menu tick). */
void p5d_autohalf_apply(void) {
	static int apply_on = -1;
	if (apply_on < 0) { const char *e = getenv("RECOMP_P5D_AUTOHALF_APPLY"); apply_on = e && !atoi(e) ? 0 : 1; }
	if (!apply_on || p5d_ah_jumps) return;
	uint64_t f = display_frames();
	static uint64_t last_f = 0xffffffffffffffffull;
	if (!p5d_ah_active || f == last_f) return;
	last_f = f;
	if (1) {
		if (f & 1u) {   /* odd presented frame: undo the extra increment for the selected candidates */
			if (p5d_ah_naddrs) {
				for (int k = 0; k < p5d_ah_naddrs; k++) {
					uint32_t a = p5d_ah_addrs[k];
					float v; uint32_t w = RD32(a); memcpy(&v, &w, 4);
					v -= 1.0f / 30.0f; memcpy(&w, &v, 4); WR32(a, w);
				}
				return;
			}
			uint64_t el = f - p5d_ah_act_f;
			for (int k = p5d_ah_skip; k < p5d_ah_ncand && k < p5d_ah_skip + p5d_ah_take; k++) {
				uint32_t a = p5d_ah_cand[k];
				if (p5d_ah_want_type == 0 && p5d_ah_istype[k] != 0) continue;
				if (p5d_ah_want_type == 1 && p5d_ah_istype[k] == 0) continue;
				if (p5d_ah_want_type == 2 && p5d_ah_istype[k] != 1) continue;
				if (p5d_ah_vmax >= 0 && p5d_ah_istype[k] != 0) { float cv; uint32_t cw = RD32(a); memcpy(&cv, &cw, 4); if (cv < 0.0f || cv > (float)p5d_ah_vmax) continue; }
				if (p5d_ah_istype[k] == 0) {
					uint32_t v = RD32(a);
					if (v) WR32(a, v - 1);
				} else {
					float v; uint32_t w = RD32(a); memcpy(&v, &w, 4);
					float inc = (p5d_ah_istype[k] == 1) ? (1.0f / 30.0f) : (1.0f / 60.0f);
					v -= inc; memcpy(&w, &v, 4); WR32(a, w);
				}
			}
		}
	}
}
/* M1626: memory snapshots for the frame-clock hunt. RECOMP_P5D_MEMDUMP=1 + RECOMP_P5D_MEMDUMP_AT (frames after the
 * song screen appears) and RECOMP_P5D_MEMDUMP_GAP (frames between the two snapshots) dump the guest heap regions where
 * the menu/app objects live; diffing the two snapshots offline gives the words that advance one per presented frame
 * (the frame-based animation counters). Files: C:\dev\tmp\memdump_<frame>_<a|b>.bin. */
extern int window_p5d_song_screen(void);
static void p5d_memdump_snapshot(long f, const char *tag) {
	struct { uint32_t addr, len; } regions[] = { {0x81000000u, 0x00800000u}, {0x10000000u, 0x02000000u}, {0x13000000u, 0x01000000u} };
	for (unsigned i = 0; i < sizeof regions / sizeof regions[0]; i++) {
		char fn[256];
		snprintf(fn, sizeof fn, "C:\\dev\\tmp\\memdump_%ld_%s_%u.bin", f, tag, i);
		FILE *o = fopen(fn, "wb");
		if (!o) { fprintf(stderr, "[MEMDUMP] open failed %s\n", fn); continue; }
		size_t n = fwrite(G(regions[i].addr), 1, regions[i].len, o);
		fclose(o);
		fprintf(stderr, "[MEMDUMP] %s addr=%#x len=%#x wrote=%zu\n", fn, regions[i].addr, regions[i].len, n);
	}
}
static void p5d_memdump_tick(void) {
	static int on = -1; static long at = -1, gap = -1;
	static int shot; static long f1 = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_MEMDUMP"); on = e && atoi(e) != 0; }
	if (!on) return;
	if (at < 0) {
		const char *e = getenv("RECOMP_P5D_MEMDUMP_AT"); at = e ? atol(e) : 150;
		e = getenv("RECOMP_P5D_MEMDUMP_GAP"); gap = e ? atol(e) : 60;
	}
	uint64_t f = display_frames();
	if (!shot) { if (window_p5d_song_screen()) { shot = 1; f1 = (long)f + at; } return; }
	if ((long)f == f1) p5d_memdump_snapshot((long)f, "a");
	if ((long)f == f1 + gap) { p5d_memdump_snapshot((long)f, "b"); shot = 0; }
}

/* M1617: keep the engine's frame-base index (singleton 0x8132ae64, +8 = index into {1,30,60}) in step with the screen.
 * The song list runs on the game's own 30 fps cadence (display.c swap_interval: its 3D character is animated by a
 * presented-frame counter and ran 2x at 60), so its animation conversions must use the 30 fps base there; every other
 * screen (main menu, title, the dance stage itself, UI pages) uses the 60 base. Called once per frame from window.c. */
extern int window_p5d_song_screen(void);
extern int window_p5d_dance_screen(void);
/* M1685: the engine callback runner fn_8127b3a2 can't be counted from its entry (it is re-entered at its jump table
 * when resumed after a blocking callback), so count the instruction that ends each run (0x8127b3ea: node+0x40 = 1).
 * RECOMP_P5D_CBRUN=1 prints the frame of every completion while the dance stage is up; gaps of 1 = 60 Hz runner,
 * gaps of 2 = the runner itself is the 30 Hz period. */
void recomp_ihook_p5d_cbrun(Cpu *c) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_CBRUN"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern int window_p5d_dance_screen(void);
	if (!window_p5d_dance_screen()) return;
	fprintf(stderr, "[CBR] f=%llu node=%#x\n", (unsigned long long)display_frames(), c->r[4]);
}

/* M1684: per-frame diff of the 3D render callback node (0x816a0ac0, found from the boot callback dump): which of its
 * words change every frame and which every other frame tells where the 30 Hz period lives (a tick counter, an interval
 * field, a state). RECOMP_P5D_NODEDUMP=1. */
/* M1632: the dance-stage scene matrix is kept in THREE copies, one per display buffer (the 30 Hz-looking blocks are
 * these per-buffer copies: each copy is written once every 3 frames). Sample the same element of all three copies each
 * presented frame to reconstruct what the frame being shown actually displays: if consecutive shown values repeat on
 * every other frame, the animation steps at 30 Hz even though 60 frames are presented. RECOMP_P5D_MAT3=1. */
/* M1633: sample a few candidate CPU-written buffers each presented frame (no GPU sync, no I/O beyond one log line):
 * the dynamic vertex streams and scene matrices seen changing every frame earlier. If the skinning/pose data changes
 * every frame the character's animation data is 60 Hz; if it repeats on alternate frames it is 30 Hz. RECOMP_P5D_VS=1. */
/* M1637: sample the frame-counter candidates every presented frame during the dance stage and print them with the
 * frame number, so the one that steps on the "new content" frames (the app/simulation frame tick) can be picked out.
 * RECOMP_P5D_CNT=1. */
static void p5d_cnt_tick(void) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_CNT"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern int window_p5d_dance_screen(void);
	static const uint32_t addrs[11] = {
		0x10380954u, 0x102e30b0u, 0x1034f49cu, 0x1034f4bcu, 0x1034f8dcu, 0x1034f8fcu,
		0x10360244u, 0x10360284u, 0x10381450u, 0x10381478u, 0x105d7b30u };
	uint64_t f = display_frames();
	fprintf(stderr, "[CNT] f=%llu dance=%d", (unsigned long long)f, window_p5d_dance_screen());
	for (int i = 0; i < 11; i++) fprintf(stderr, " %d", (int)RD32(addrs[i]));
	fprintf(stderr, "\n");
}

static void p5d_vs_tick(void) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_VS"); on = e && atoi(e) != 0; }
	if (!on) return;
	static const uint32_t addrs[4] = {0x10c138b0u, 0x10c131e0u, 0x10810800u, 0x11078c80u};
	uint64_t f = display_frames();
	fprintf(stderr, "[VS] f=%llu", (unsigned long long)f);
	for (int i = 0; i < 4; i++) {
		uint32_t w0 = RD32(addrs[i]), w1 = RD32(addrs[i] + 4);
		float a, b; memcpy(&a, &w0, 4); memcpy(&b, &w1, 4);
		fprintf(stderr, " %d:%.4f/%.4f/%08x", i, a, b, w0);
	}
	fprintf(stderr, "\n");
}

static void p5d_mat3_tick(void) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_MAT3"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern int window_p5d_dance_screen(void);
	if (!window_p5d_dance_screen()) return;
	uint64_t f = display_frames();
	float v[3][4];
	for (int k = 0; k < 3; k++)
		for (int j = 0; j < 4; j++) {
			uint32_t w = RD32(0x10e52ea8u + (uint32_t)k * 0xC00u + 4u * j);
			memcpy(&v[k][j], &w, 4);
		}
	fprintf(stderr, "[M3] f=%llu a=%.5f b=%.5f c=%.5f a1=%.5f a3=%.5f\n",
		(unsigned long long)f, v[0][0], v[1][0], v[2][0], v[0][1], v[0][3]);
}

static void p5d_nodedump_tick(void) {
	static int on = -1; static uint32_t prev[32]; static int have;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_NODEDUMP"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern int window_p5d_dance_screen(void);
	if (!window_p5d_dance_screen()) { have = 0; return; }
	uint64_t f = display_frames();
	if (!have) { for (int k = 0; k < 32; k++) prev[k] = RD32(0x816a0ac0u + 4 * k); have = 1; return; }
	for (int k = 0; k < 32; k++) {
		uint32_t v = RD32(0x816a0ac0u + 4 * k);
		if (v != prev[k]) { fprintf(stderr, "[ND] f=%llu [%d] %#x -> %#x\n", (unsigned long long)f, k, prev[k], v); prev[k] = v; }
	}
}

static void p5d_memdump2_tick(void);
static void p5d_ww_page_tick(void);
void p5d_mframe_screen_tick(void) {
	p5d_memdump_tick();
	p5d_memdump2_tick();
	p5d_ww_page_tick();
	p5d_nodedump_tick();
	p5d_mat3_tick();
	p5d_vs_tick();
	p5d_cnt_tick();
	p5d_autohalf_tick();
	if (!display_frame_rate_60()) return;
	uint32_t p = RD32(0x8132ae64u);
	if (p < 0x10000u || p >= 0x88000000u) return;
	{ static int sg60 = -1;
	  if (sg60 < 0) { const char *e = getenv("RECOMP_P5D_SONGFPS60"); sg60 = e && atoi(e) != 0; }
	  uint32_t want = (window_p5d_song_screen() && !window_p5d_dance_screen() && !sg60) ? 1u : 2u;
	  if (RD32(p + 8) != want) WR32(p + 8, want);
	}
}


/* M1620: candidate probes for the song-select 3D character animation. RECOMP_P5D_CAND=1 logs each candidate function's
 * arguments (r0..r3, s0 float) every 30 presented frames, at 30 and 60 fps (with RECOMP_P5D_SONGFPS60=1), so a frame-based
 * time input (advancing one unit per presented frame regardless of rate) can be identified among the MusicSelectMain /
 * DanceReady animation-path callees. */
static void p5d_cand_log(const char *tag, uint64_t *lastp, Cpu *c) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_CAND"); on = e && atoi(e) != 0; }
	if (!on) return;
	uint64_t f = display_frames();
	if (f - *lastp < 30) return;
	*lastp = f;
	float s0; memcpy(&s0, &c->vfp.s[0], 4);
	fprintf(stderr, "[CAND] %s f=%llu r0=%#x r1=%#x r2=%#x r3=%#x s0=%.5f rate=%s\n", tag,
	        (unsigned long long)f, c->r[0], c->r[1], c->r[2], c->r[3], s0,
	        display_frame_rate_60() ? "60" : "30");
}
void recomp_hook_p5d_cand1(Cpu *c) { static uint64_t last; p5d_cand_log("c1", &last, c); }
void recomp_hook_p5d_cand2(Cpu *c) { static uint64_t last; p5d_cand_log("c2", &last, c); }
void recomp_hook_p5d_cand3(Cpu *c) { static uint64_t last; p5d_cand_log("c3", &last, c); }
void recomp_hook_p5d_cand4(Cpu *c) { static uint64_t last; p5d_cand_log("c4", &last, c); }
void recomp_hook_p5d_cand5(Cpu *c) { static uint64_t last; p5d_cand_log("c5", &last, c); }
void recomp_hook_p5d_cand6(Cpu *c) { static uint64_t last; p5d_cand_log("c6", &last, c); }
void recomp_hook_p5d_cand7(Cpu *c) { static uint64_t last; p5d_cand_log("c7", &last, c); }
void recomp_hook_p5d_cand8(Cpu *c) { static uint64_t last; p5d_cand_log("c8", &last, c); }
void recomp_hook_p5d_cand9(Cpu *c) { static uint64_t last; p5d_cand_log("c9", &last, c); }
void recomp_hook_p5d_cand10(Cpu *c) { static uint64_t last; p5d_cand_log("c10", &last, c); }
void recomp_hook_p5d_cand11(Cpu *c) { static uint64_t last; p5d_cand_log("c11", &last, c); }
void recomp_hook_p5d_cand12(Cpu *c) { static uint64_t last; p5d_cand_log("c12", &last, c); }


/* M1621: test hack for the song-select character animation. DanceReady_P5 / MusicSelectMain keep their 3D character
 * animation on presented-frame counters (object +0x20 and +0x120, +1 per presented frame). At 60 fps with the test
 * switch RECOMP_P5D_SONGFPS60=1 the character runs 2x; RECOMP_P5D_ANIMHALF=1 skips every other increment on the
 * +0x20 counter (the two adds sites) so the animation time advances at half the presented rate. If the character
 * slows to real time this is its clock; then the proper fix halves the value where it is used. Test only. */
extern int window_p5d_song_screen(void);
extern int window_p5d_dance_screen(void);
static int p5d_animhalf_on(void) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_ANIMHALF"); on = e && atoi(e) != 0; }
	if (!on || !display_frame_rate_60()) return 0;
	if (!window_p5d_song_screen() || window_p5d_dance_screen()) return 0;
	static uint64_t n;
	n++;
	return (int)(n & 1);
}
void recomp_ihook_p5d_animhalf_r1(Cpu *c, int reg) { (void)reg; if (p5d_animhalf_on()) c->r[1] -= 1;
	static int log = -1; if (log < 0) { const char *e = getenv("RECOMP_P5D_ANIMHALF_LOG"); log = e && atoi(e) != 0; }
	if (log) { static uint64_t n; static uint64_t f; uint64_t now = hook_frames(); if (++n % 60 == 0) fprintf(stderr, "[ANIMHALF] r1 site calls=%llu f=%llu\n", (unsigned long long)n, (unsigned long long)now); (void)f; } }
void recomp_ihook_p5d_animhalf_r0(Cpu *c, int reg) { (void)reg; if (p5d_animhalf_on()) c->r[0] -= 1; }
void recomp_ihook_p5d_animhalf2_r0(Cpu *c, int reg) { (void)reg; if (p5d_animhalf_on()) c->r[0] -= 1; }


/* M1623: dump the MusicSelectMain process object (sl) once per 30 presented frames so the words that advance one per
 * presented frame (the frame-based animation clocks) can be found by comparing a 30 fps run with a 60 fps run
 * (RECOMP_P5D_SONGFPS60=1): a counter that is +1 per presented frame advances 2x per second at 60 fps. */
void recomp_ihook_p5d_msdump(Cpu *c, int reg) {
	(void)reg;
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_MSDUMP"); on = e && atoi(e) != 0; }
	if (!on) return;
	static uint64_t last;
	uint64_t f = display_frames();
	if (f - last < 30) return;
	last = f;
	uint32_t sl = c->r[10];
	if (sl < 0x10000u || sl >= 0x88000000u) return;
	fprintf(stderr, "[MSDUMP] f=%llu sl=%#x", (unsigned long long)f, sl);
	for (int k = 0; k < 112; k++) fprintf(stderr, " %d", (int)RD32(sl + 4 * k));
	fprintf(stderr, "\n");
}


void recomp_hook_p5d_cand13(Cpu *c) { static uint64_t last; p5d_cand_log("c13", &last, c); }
void recomp_hook_p5d_cand14(Cpu *c) { static uint64_t last; p5d_cand_log("c14", &last, c); }
void recomp_hook_p5d_cand15(Cpu *c) { static uint64_t last; p5d_cand_log("c15", &last, c); }
void recomp_hook_p5d_cand16(Cpu *c) { static uint64_t last; p5d_cand_log("c16", &last, c); }
void recomp_hook_p5d_cand17(Cpu *c) { static uint64_t last; p5d_cand_log("c17", &last, c); }


/* M1634: the app-fps pair (object at 0x813af5a8, fn_81154a1c(a,b)) is what the animation clock divides by:
 * fn_81154a94 / fn_81154b04 read it, fn_81154ad0 / fn_81154b36 write it. If the song list sets 30 while the
 * presentation is 60, its frame-based animation runs 2x. RECOMP_P5D_APPFPS60=1 forces the written values to 60
 * in 60 fps mode, test first. */
static int p5d_fpsset_on(void) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_APPFPS60"); on = e && atoi(e) != 0; }
	return on && display_frame_rate_60();
}
void recomp_hook_p5d_fpsset1(Cpu *c) { if (p5d_fpsset_on()) { if (c->r[0] != 60) fprintf(stderr, "[FPSPAIR] set0 %u -> 60\n", c->r[0]); c->r[0] = 60; } }
void recomp_hook_p5d_fpsset2(Cpu *c) { if (p5d_fpsset_on()) { if (c->r[0] != 60) fprintf(stderr, "[FPSPAIR] set1 %u -> 60\n", c->r[0]); c->r[0] = 60; } }


void recomp_hook_p5d_cand18(Cpu *c) { static uint64_t last; p5d_cand_log("c18", &last, c); }
void recomp_hook_p5d_cand19(Cpu *c) { static uint64_t last; p5d_cand_log("c19", &last, c); }
void recomp_hook_p5d_cand20(Cpu *c) { static uint64_t last; p5d_cand_log("c20", &last, c); }
void recomp_hook_p5d_cand21(Cpu *c) { static uint64_t last; p5d_cand_log("c21", &last, c); }
void recomp_hook_p5d_cand22(Cpu *c) { static uint64_t last; p5d_cand_log("c22", &last, c); }
void recomp_hook_p5d_cand23(Cpu *c) { static uint64_t last; p5d_cand_log("c23", &last, c); }


/* M1640: page write-watch probe. RECOMP_P5D_WWATCH=1 protects, one per frame, the page of each discovered
 * frame-clock candidate and lets the host's vectored handler print the code that writes it. The candidate's host
 * address is printed so the logged write address can be matched exactly. */
extern int wwatch_protect(void *host_ptr);
extern int wwatch_unprotect(void *host_ptr);
static void p5d_wwatch_tick(void) {
	static int on = -1; static int next; static void *h;
	if (p5d_ah_freeze) return;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_WWATCH"); on = e && atoi(e) != 0; }
	if (!on || !p5d_ah_active) return;
	while (next < p5d_ah_ncand) {
		/* only the frame-based floats of the newly created objects: the song screen's own animation clocks */
		if (p5d_ah_istype[next] == 1) {
			float cv; uint32_t cw = RD32(p5d_ah_cand[next]); memcpy(&cv, &cw, 4);
			if (cv >= 0.0f && cv < 65.0f) break;
		}
		next++;
	}
	if (next >= p5d_ah_ncand) return;
	if (next > 0) { wwatch_unprotect(h); }
	uint32_t a = p5d_ah_cand[next];
	float cv; { uint32_t cw = RD32(a); memcpy(&cv, &cw, 4); }
	h = G(a);
	if (wwatch_protect(h))
		fprintf(stderr, "[WWATCH] page cand=%#x host=%p v=%.3f f=%llu\n", a, h, cv, (unsigned long long)display_frames());
	next++;
}


/* M1641: the song select's dance-preview animation runs through the callback fn_81199f7a (no direct caller in the
 * recompiled code: it is registered as a function pointer). If its float argument is the per-frame delta it will be
 * the frame duration computed for the assumed 30 fps (1/30 per presented frame), so halving it restores real time at
 * 60 fps. RECOMP_P5D_PREVDT=1: halve s0 on the song screen in 60 fps mode, log the first values. */
void recomp_hook_p5d_prevdt(Cpu *c) {
	static int on = -1; static int n;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_PREVDT"); on = e && atoi(e) != 0; }
	if (!on) return;
	{ float v; memcpy(&v, &c->vfp.s[0], 4); static uint64_t lastf; uint64_t f = display_frames(); if (f != lastf) { static int m; if ((m++ % 30) == 0) fprintf(stderr, "[PREVDT] s0=%.6f f=%llu\n", v, (unsigned long long)f); } n++; (void)v; }
	if (display_frame_rate_60()) c->vfp.f[0] *= 0.5f;
}


/* M1648: the song-select animation clock. fn_81097f7a advances a timer object as
 *     time += 1.0f / (float)fn_811730da(...)
 * fn_811730da is the frame-rate helper and at 60 fps it still reports 30, so every presented frame adds 1/30 s and
 * every consumer of that timer (the dance-preview character first) animates at double speed. Patch the value right
 * after the helper returns: 60 on the song screen in 60 fps mode. The 30 fps path and every other screen are
 * untouched. IHOOK at 0x81097fda (the vmov s0, r0 that follows the bl). Logs the first hits so the run proves it
 * fires with 30. */
void recomp_ihook_p5d_timefps(Cpu *c) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_TIMEFPS"); on = (e && *e && *e != '0') ? 1 : (e ? 0 : 1); }
	if (!on) return;
	/* M1636: every screen in the 60 fps mode (the song-screen check was the same fragile screen-model dependency
	 * that M1622/M1634 removed from the vblank cap: in a real 2K fullscreen run the model can fail and the retiming
	 * silently did not apply). The other screens already run 60 in the 60 mode, so their timers step 1/60 s per
	 * presented frame at real-time speed; the 30 fps mode is untouched. */
	if (!display_frame_rate_60()) return;
	if (c->r[0] != 60) { static int n; if (n < 12) { n++; fprintf(stderr, "[TIMEFPS] r0=%u -> 60 lr=%#x f=%llu\n", c->r[0], c->r[14], (unsigned long long)display_frames()); } }
	c->r[0] = 60;
}


/* M1649: fn_811730da is a one-call wrapper around fn_81173118, the frame-rate helper the animation timers use to
 * step their clocks (time += 1/rate). Hook its return and report 60 on the song screen in 60 fps mode: every timer
 * that steps in real time there then advances 1/60 s per presented frame instead of 1/30 s. 30 fps and the other
 * screens are untouched. */
void recomp_ihook_p5d_timefps2(Cpu *c) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_TIMEFPS"); on = (e && *e && *e != '0') ? 1 : (e ? 0 : 1); }
	if (!on) return;
	/* (no screen-model check: see the M1636 note in recomp_ihook_p5d_timefps) */
	/* M1622 experiment: the dance stage's 3D scene/camera/pose is still updated only every other presented frame even
	 * after the vblank pacing fix (render pass now runs every frame, but the animation timers that step the scene
	 * still advance 1/30 s only on the app-frame boundaries). RECOMP_P5D_DANCEFPS60=1 lets this same retiming cover
	 * the dance screen so the scene clocks step 1/60 s per presented frame - the visible 3D then changes every frame
	 * at the same real-time speed. Default off until measured on the dance stage. */
	if (!display_frame_rate_60()) return;
	if (c->r[0] != 60) { static int n; if (n < 8) { n++; fprintf(stderr, "[TIMEFPS2] r0=%u -> 60 f=%llu\n", c->r[0], (unsigned long long)display_frames()); } }
	c->r[0] = 60;
}


/* M1651: per-frame call counters for the dance-stage update functions, to find which 3D update runs at 30 Hz while
 * the judgement UI runs at 60. RECOMP_P5D_FRAMECNT=1 prints once per 60 presented frames the calls seen in that
 * window; a value near 60 means "called every presented frame (60 Hz)", near 30 means "every other frame (30 Hz)".
 * Counters are env-gated, inert otherwise. */
static uint64_t p5d_fc_count[64];
/* M1667: watch the render-request queue page during the dance stage and log every write to it, so the producer of
 * the (30 Hz dispatched) draw callbacks is identified by its return address and cadence.
 * RECOMP_P5D_WW_PAGE=0x8153fd60 with RECOMP_WWATCH_ALL=1. */
static void p5d_ww_page_tick(void) {
	static int on = -1; static unsigned long long addr; static int done;
	extern int window_p5d_dance_screen(void);
	extern int wwatch_protect(void *host_ptr);
	if (on < 0) { const char *e = getenv("RECOMP_P5D_WW_PAGE"); on = e && *e; addr = on ? strtoull(e, 0, 0) : 0; }
	if (!on || done) return;
	if (!window_p5d_dance_screen()) return;
	{ static int all = -1; if (all < 0) { const char *e = getenv("RECOMP_P5D_WW_PAGE_ALL"); all = e && atoi(e) != 0; }
	  extern void wwatch_set_target(void *host_ptr);
	  if (!all) wwatch_set_target((void *)G((uint32_t)addr)); else wwatch_set_target(0); }
	{ static int every = -1; if (every < 0) { const char *e = getenv("RECOMP_P5D_WW_PAGE_EVERY"); every = e && atoi(e) != 0; }
	  if (every) { static uint64_t lastf = ~0ull; extern uint64_t display_frames(void); uint64_t f = display_frames();
	    if (window_p5d_dance_screen() && f != lastf) { extern int wwatch_protect(void *host_ptr); extern void wwatch_set_target(void *host_ptr);
	      lastf = f; wwatch_set_target(0); wwatch_protect((void *)G((uint32_t)addr)); return; } } }
	void *hp = (void *)G((uint32_t)addr);
	if (wwatch_protect(hp)) { done = 1;
		fprintf(stderr, "[WWPAGE] guest=%#llx host=%p delta=%#llx f=%llu\n", addr, hp,
			(unsigned long long)((uintptr_t)hp - (uintptr_t)(uint32_t)addr), (unsigned long long)display_frames()); }
}
/* M1660: one-shot guest stack scan when the 3D render dispatch (fn_81236c14) runs, to see which job/system
 * call path reaches it (the addresses on the stack in the code range are the call chain's return addresses).
 * RECOMP_P5D_BT=1. */
extern int window_p5d_dance_screen(void);
static void p5d_fc_fclog(Cpu *c, int which) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_FCLOG"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern int window_p5d_dance_screen(void);
	if (!window_p5d_dance_screen()) return;
	if (which != 8 && which != 13 && which != 14 && which != 16) return;
	fprintf(stderr, "[FCL] c%d f=%llu r0=%#x\n", which, (unsigned long long)display_frames(), c->r[0]);
}
static void p5d_fc_bt(Cpu *c, int which) {
	p5d_fc_fclog(c, which);
	static int on = -1; static int n;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_BT"); on = e && atoi(e) != 0; }
	if (!on) return;
	if (which != 16) return;
	{ extern int window_p5d_dance_screen(void); if (!window_p5d_dance_screen()) return; }
	{ static long from = -2; if (from == -2) { const char *e = getenv("RECOMP_P5D_BT_FROM"); from = e ? atol(e) : -1; }
	  if (from > 0 && display_frames() < (uint64_t)from) return; }
	if (n >= 20) return;
	if (which == 19) {
		fprintf(stderr, "[CB] f=%llu r0=%#x r1=%#x words:", (unsigned long long)display_frames(), c->r[0], c->r[1]);
		if (c->r[0] > 0x10000u && c->r[0] < 0x88000000u) for (int k = 0; k < 24; k++) fprintf(stderr, " %#x", RD32(c->r[0] + 4 * k));
		fprintf(stderr, "\n");
	}
	n++;
	{ static long lo = -2, hi = -2;
	  if (lo == -2) { const char *a = getenv("RECOMP_P5D_BT_LO"), *b = getenv("RECOMP_P5D_BT_HI");
	    lo = a ? strtol(a, 0, 0) : 0x81000000; hi = b ? strtol(b, 0, 0) : 0x81300000; }
	  int shown = 0;
	  fprintf(stderr, "[BT] f=%llu which=%d lr=%#x", (unsigned long long)display_frames(), which, c->r[14]);
	  for (int k = 0; k < 512 && shown < 14; k++) {
	    uint32_t v = RD32(c->r[13] + 4 * k);
	    if ((long)v >= lo && (long)v < hi && (v & 1u) == 0) { fprintf(stderr, " %#x", v); shown++; }
	  }
	  if (which == 16) {
	    fprintf(stderr, " [QUEUE]");
	    for (int k = 0; k < 16; k++) fprintf(stderr, " %#x", RD32(0x8153fd60u + 4 * k));
	    fprintf(stderr, " job=%s", (const char *)p5d_job_name);
	  }
	  fprintf(stderr, "\n"); }
}
/* M1671: per-call float/register probe on the animation candidates: RECOMP_P5D_FCPROBE=1 logs, for the dance
 * pose/update hooks only, the frame and s0 (the usual time/value argument) plus r0-r3, so a value that only advances
 * on every other presented frame (the 30 Hz character pose behind the 60 Hz note UI) shows up directly. */
/* M1640: compact statistic for the dance pose-update entry (c20 = fn_81041740): the animation time left in s0
 * changes once every two presented frames at 30 Hz. Print once per 60 presented frames how many times the value
 * changed and how many of its steps equal 1/30 s, so candidate retiming knobs can be compared quickly.
 * RECOMP_P5D_C20STAT=1. */
static void p5d_c20stat(Cpu *c) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_C20STAT"); on = e && atoi(e) != 0; }
	if (!on) return;
	static uint32_t last; static int have; static unsigned changes, steps30, frames;
	static uint64_t lastf;
	frames++;
	uint32_t cur; memcpy(&cur, &c->vfp.s[0], 4);
	if (have && cur != last) {
		changes++;
		float a, b; memcpy(&a, &last, 4); memcpy(&b, &cur, 4);
		float d = b - a;
		if (d > 0.030f && d < 0.036f) steps30++;
	}
	last = cur; have = 1;
	{ extern uint64_t display_frames(void); uint64_t f = display_frames(); if (f - lastf >= 60) {
		lastf = f;
		fprintf(stderr, "[C20STAT] f=%llu frames=%u changes=%u steps30=%u\n", (unsigned long long)f, frames, changes, steps30);
		changes = steps30 = frames = 0; } }
}
static void p5d_fc_probe(Cpu *c, int which) {
	if (which == 20) p5d_c20stat(c);
	/* M1639: c20 = fn_81041740, the dance-stage pose/scene update. Its float input s0 is the animation time, and the
	 * engine feeds it in 1/30 s steps, one step every second presented frame (measured before this patch: 449 of 898
	 * consecutive frames carried the same value, steps of 0.03333), so the character pose only advances at 30 Hz.
	 * Here we make the value advance 1/60 s on EVERY presented frame: when the engine repeats last frame's value we
	 * continue from the value we handed out plus 1/60; when the engine steps to a new value we pass it through. The
	 * result is monotonic, drift-free 60 Hz animation time at exactly the same real-time speed (the engine's motion
	 * sampler interpolates between its keys at that time). Per-node table keyed by the node pointer in r0; the 30 fps
	 * mode and the other screens are untouched (patch only runs when frame_rate_60()). RECOMP_P5D_POSEFPS=1. */
	{
		static int posefps = -1;
		if (posefps < 0) { const char *e = getenv("RECOMP_P5D_POSEFPS"); posefps = e && atoi(e) != 0; }
		if (posefps && which == 20 && display_frame_rate_60()) {
			static struct { uint32_t node; uint32_t last_native; float out; } slots[64];
			float cur; memcpy(&cur, &c->vfp.s[0], 4);
			uint32_t cw; memcpy(&cw, &cur, 4);
			int i = 0; for (; i < 64; i++) if (slots[i].node == c->r[0]) break;
			if (i == 64) { i = 0; for (; i < 64; i++) if (!slots[i].node) break; }
			if (i < 64) {
				if (!slots[i].node) { slots[i].node = c->r[0]; slots[i].last_native = cw; slots[i].out = cur; }
				else if (cw != slots[i].last_native) { slots[i].last_native = cw; slots[i].out = cur; }
				else slots[i].out += 0.0166666675f;
				memcpy(&c->vfp.s[0], &slots[i].out, 4);
			}
		}
	}
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_FCPROBE"); on = e && atoi(e) != 0; }
	if (!on) return;
	if (!(which == 0 || which == 1 || which == 2 || which == 5 || which == 7 || which >= 20)) return;
	if (which >= 20) { extern int window_p5d_dance_screen(void); if (!window_p5d_dance_screen()) return; }
	fprintf(stderr, "[FCP] c%d f=%llu s0=%08x r0=%08x r1=%08x r2=%08x r3=%08x\n", which,
		(unsigned long long)display_frames(), c->vfp.s[0], c->r[0], c->r[1], c->r[2], c->r[3]);
}
static void p5d_fc_report(void) {
	static int on = -1; static uint64_t last;
	static long seq_from = -2, seq_to = -2;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_FRAMECNT"); on = e && atoi(e) != 0; }
	if (!on) return;
	if (seq_from == -2) {
		const char *a = getenv("RECOMP_P5D_FRAMECNT_SEQ_FROM"), *b = getenv("RECOMP_P5D_FRAMECNT_SEQ_TO");
		seq_from = a ? atol(a) : -1; seq_to = b ? atol(b) : -1;
	}
	uint64_t f = display_frames();
	/* M1658: per-frame call counts in a frame window (RECOMP_P5D_FRAMECNT_SEQ_FROM/_TO): one line per frame shows
	 * whether an update runs on every frame or only on every other one (the 30 Hz 3D updater hunt). */
	if (seq_from >= 0 && f >= (uint64_t)seq_from && (seq_to < 0 || f <= (uint64_t)seq_to)) {
		fprintf(stderr, "[FCSEQ] f=%llu", (unsigned long long)f);
		for (int i = 0; i < 64; i++) { if (p5d_fc_count[i]) fprintf(stderr, " c%d=%llu", i, (unsigned long long)p5d_fc_count[i]); p5d_fc_count[i] = 0; }
		fprintf(stderr, "\n");
		return;
	}
	if (f - last < 60) return;
	last = f;
	fprintf(stderr, "[FRAMECNT] f=%llu", (unsigned long long)f);
	for (int i = 0; i < 64; i++) if (p5d_fc_count[i]) { fprintf(stderr, " c%d=%llu", i, (unsigned long long)p5d_fc_count[i]); p5d_fc_count[i] = 0; }
	fprintf(stderr, "\n");
}
#define P5D_FC(n) void recomp_hook_p5d_fc##n(Cpu *c) { p5d_fc_count[n]++; p5d_fc_bt(c, n); p5d_fc_probe(c, n); p5d_fc_report(); }
P5D_FC(0) P5D_FC(1) P5D_FC(2) P5D_FC(3) P5D_FC(4) P5D_FC(5) P5D_FC(6) P5D_FC(7)
P5D_FC(8) P5D_FC(9) P5D_FC(10) P5D_FC(11)
P5D_FC(12) P5D_FC(13) P5D_FC(14) P5D_FC(15)
P5D_FC(16) P5D_FC(17) P5D_FC(18) P5D_FC(19)
P5D_FC(20) P5D_FC(21) P5D_FC(22) P5D_FC(23)
P5D_FC(24) P5D_FC(25) P5D_FC(26) P5D_FC(27)
P5D_FC(28) P5D_FC(29) P5D_FC(30) P5D_FC(31)
P5D_FC(32) P5D_FC(33) P5D_FC(34) P5D_FC(35)
P5D_FC(36) P5D_FC(37) P5D_FC(38) P5D_FC(39)
P5D_FC(40) P5D_FC(41) P5D_FC(42) P5D_FC(43)
P5D_FC(44) P5D_FC(45) P5D_FC(46) P5D_FC(47)
P5D_FC(48) P5D_FC(49) P5D_FC(50) P5D_FC(51)
P5D_FC(52) P5D_FC(53) P5D_FC(54) P5D_FC(55)
P5D_FC(56) P5D_FC(57) P5D_FC(58) P5D_FC(59)
P5D_FC(60) P5D_FC(61) P5D_FC(62) P5D_FC(63)



/* M1654: three consecutive-frame snapshots on the dance stage, to find the 3D state that only advances on every
 * other presented frame (the 30 Hz character/scene behind the 60 Hz judgement UI). RECOMP_P5D_MEMDUMP2=1 with
 * RECOMP_P5D_MEMDUMP2_AT (frames after the dance screen is first seen) snapshots the regions into RAM at
 * at / at+1 / at+2 first (memcpy only, no disk I/O between the frames) and writes the three files after the third
 * copy, so the snapshots really are consecutive frames. */
extern int window_p5d_dance_screen(void);
struct p5d_md2_region { uint32_t addr, len; };
static struct p5d_md2_region p5d_md2_regions[] = {
	{0x81000000u, 0x00800000u}, {0x10000000u, 0x02000000u},
	{0x12000000u, 0x01000000u}, {0x13000000u, 0x01000000u} };
#define P5D_MD2_NREG (sizeof p5d_md2_regions / sizeof p5d_md2_regions[0])
#define P5D_MD2_TOTAL (8u * 1024u * 1024u + 32u * 1024u * 1024u + 16u * 1024u * 1024u + 16u * 1024u * 1024u)
static uint8_t *p5d_md2_buf[8];
static unsigned p5d_md2_nreg = (unsigned)P5D_MD2_NREG;
static int p5d_md2_small = -1;
#define P5D_MD2_WATCH_MAX 96
static uint32_t p5d_md2_cand[P5D_MD2_WATCH_MAX]; static int p5d_md2_ncand, p5d_md2_watch_phase; static long p5d_md2_watch_from;
static int p5d_md2_wnext; static int p5d_md2_watch_n = P5D_MD2_WATCH_MAX;
static void p5d_md2_watch_apply(void) {
	if (!p5d_md2_watch_phase) return;
	uint64_t f = display_frames();
	if ((long)f < p5d_md2_watch_from) return;
	if (p5d_md2_wnext >= p5d_md2_ncand) { p5d_md2_watch_phase = 0; return; }
	uint32_t addr = p5d_md2_cand[p5d_md2_wnext];
	extern int wwatch_protect(void *host_ptr);
	extern void wwatch_set_target(void *host_ptr);
	void *hp = G(addr);
	wwatch_set_target(hp);
	if (wwatch_protect(hp)) fprintf(stderr, "[MD2W] i=%d addr=%#x host=%p f=%llu\n", p5d_md2_wnext, addr, hp, (unsigned long long)f);
	p5d_md2_wnext++;
}
static void p5d_memdump2_tick(void) {
	static int on = -1; static long at = -1; static int shot, ncap; static long t0 = -1;
	p5d_md2_watch_apply();
	if (on < 0) { const char *e = getenv("RECOMP_P5D_MEMDUMP2"); on = e && atoi(e) != 0; }
	if (!on) return;
	if (at < 0) { const char *e = getenv("RECOMP_P5D_MEMDUMP2_AT"); at = e ? atol(e) : 500; }
	if (!p5d_md2_buf[0]) { for (int i = 0; i < 8; i++) p5d_md2_buf[i] = malloc(P5D_MD2_TOTAL); }
	if (p5d_md2_small < 0) {
		const char *e = getenv("RECOMP_P5D_MD2_SMALL"); p5d_md2_small = e ? atoi(e) : 0;
		if (p5d_md2_small) {
			p5d_md2_nreg = 1;
			if (p5d_md2_small == 1) { p5d_md2_regions[0].addr = 0x10000000u; p5d_md2_regions[0].len = 0x02000000u; }        /* game heap */
			else if (p5d_md2_small == 2) { p5d_md2_regions[0].addr = 0x12000000u; p5d_md2_regions[0].len = 0x01000000u; }   /* second heap */
			else if (p5d_md2_small == 3) { p5d_md2_regions[0].addr = 0x13000000u; p5d_md2_regions[0].len = 0x01000000u; }   /* third heap */
			else if (p5d_md2_small == 5) { p5d_md2_regions[0].addr = 0x25000000u; p5d_md2_regions[0].len = 0x01000000u; }  /* GPU-visible buffers (uniform pages) */
			else if (p5d_md2_small == 6) { p5d_md2_regions[0].addr = 0x24000000u; p5d_md2_regions[0].len = 0x02000000u; }  /* GPU memory window */
			else { p5d_md2_regions[0].addr = 0x81000000u; p5d_md2_regions[0].len = 0x00800000u; }                            /* globals */
		}
	}
	if (!p5d_md2_buf[0]) return;
	uint64_t f = display_frames();
	if (!shot) {
		if (window_p5d_dance_screen()) { shot = 1; ncap = 0; t0 = (long)f + at; fprintf(stderr, "[MD2] dance seen f=%llu dump at %ld..%ld\n", (unsigned long long)f, t0, t0 + 2); }
		return;
	}
	static int nsnap = -1;
	if (nsnap < 0) { const char *e = getenv("RECOMP_P5D_MD2_N"); nsnap = e ? atoi(e) : 3; if (nsnap < 1) nsnap = 1; if (nsnap > 8) nsnap = 8; }
	if (ncap < nsnap && (long)f == t0 + ncap) {
		uint8_t *w = p5d_md2_buf[ncap];
		for (unsigned i = 0; i < p5d_md2_nreg; i++) { memcpy(w, G(p5d_md2_regions[i].addr), p5d_md2_regions[i].len); w += p5d_md2_regions[i].len; }
		ncap++;
		if (ncap >= nsnap) {
			for (int k = 0; k < nsnap; k++) {
				const char *tag = "abcdefgh" + k;
				uint8_t *r = p5d_md2_buf[k];
				for (unsigned i = 0; i < p5d_md2_nreg; i++) {
					char fn[256];
					snprintf(fn, sizeof fn, "C:\\dev\\tmp\\md2_%ld_%s_%u.bin", t0 + k, tag, i);
					FILE *o = fopen(fn, "wb");
					if (o) { fwrite(r, 1, p5d_md2_regions[i].len, o); fclose(o); }
					r += p5d_md2_regions[i].len;
				}
			}
			fprintf(stderr, "[MD2] wrote %d snapshots at %ld..\n", nsnap, t0);
			shot = 0;
			/* M1655: from the second steady snapshot set, take the words that changed on frame 1 but not on frame 2
			 * (the 30 Hz state candidates) and write-watch a spread of them one per frame, so the host VEH logs the
			 * exact code that writes the 30 Hz 3D state. Needs RECOMP_WWATCH_ALL=1 to see every write to the page. */
			{ static int setno; setno++;
			  if (setno == 2) {
				int nc = 0;
				for (unsigned i = 1; i < P5D_MD2_NREG && nc < p5d_md2_watch_n; i++) {
					uint8_t *a0 = p5d_md2_buf[0], *b0 = p5d_md2_buf[1], *c0 = p5d_md2_buf[2];
					for (unsigned r = 0; r < i; r++) { a0 += p5d_md2_regions[r].len; b0 += p5d_md2_regions[r].len; c0 += p5d_md2_regions[r].len; }
					uint32_t n = p5d_md2_regions[i].len / 4;
					const uint32_t *A = (const uint32_t *)a0, *B = (const uint32_t *)b0, *C = (const uint32_t *)c0;
					for (uint32_t k = 0; k < n && nc < p5d_md2_watch_n; k += 7) {
						if (A[k] != B[k] && B[k] == C[k]) p5d_md2_cand[nc++] = p5d_md2_regions[i].addr + 4 * k;
					}
				}
				/* M1675: the large contiguous runs among those words are the interesting ones (a bone-matrix array
				 * is tens of words long and updates as a block): print the biggest runs with their float words, and
				 * put their starts first in the write-watch list. */
				{ int big_shown = 0;
				  for (unsigned i = 1; i < p5d_md2_nreg && big_shown < 10; i++) {
					uint8_t *a0 = p5d_md2_buf[0], *b0 = p5d_md2_buf[1], *c0 = p5d_md2_buf[2];
					for (unsigned r = 0; r < i; r++) { a0 += p5d_md2_regions[r].len; b0 += p5d_md2_regions[r].len; c0 += p5d_md2_regions[r].len; }
					uint32_t n = p5d_md2_regions[i].len / 4;
					const uint32_t *A = (const uint32_t *)a0, *B = (const uint32_t *)b0, *C = (const uint32_t *)c0;
					uint32_t k = 0;
					while (k < n && big_shown < 10) {
						if (A[k] != B[k] && B[k] == C[k]) {
							uint32_t st = k, last = k;
							for (uint32_t j = k + 1; j < n && j < k + 200000; j++) {
								if (A[j] != B[j] && B[j] == C[j]) last = j;
								else if (j - last > 6) break;
							}
							uint32_t len = last - st + 1;
							if (len >= 16) {
								float f0, f1, f2, f3; memcpy(&f0, &A[st], 4); memcpy(&f1, &A[st+1], 4);
								memcpy(&f2, &A[st+2], 4); memcpy(&f3, &A[st+3], 4);
								fprintf(stderr, "[MD2G] reg=%u addr=%#x len=%u startfloats=%.4f,%.4f,%.4f,%.4f hex=%08x,%08x\n",
									i, p5d_md2_regions[i].addr + 4 * st, len, f0, f1, f2, f3, A[st], A[st+1]);
								if (nc < 24) p5d_md2_cand[nc++] = p5d_md2_regions[i].addr + 4 * st;
								big_shown++;
							}
							k = last + 1;
						} else k++;
					}
				  }
				}
				fprintf(stderr, "[MD2W] candidates=%d here=%d\n", nc, p5d_md2_watch_n);
				p5d_md2_ncand = nc; p5d_md2_wnext = 0; p5d_md2_watch_phase = 1; p5d_md2_watch_from = (long)f + 4;
			  }
			}
		}
	}
}

/* M1652 probe: the dance-stage animation time is snapped to a 1/30 s grid by the quantizer fn_81049976
 * (obj[0] = frame_index/30). At 60 fps the snapped value changes only every other presented frame, so the 3D
 * character/scene updates at 30 Hz while the judgement UI runs at 60. IHOOK right after the vcvt, before the
 * multiply, so the scale can be replaced: 1/60 in 60 fps mode on the dance stage only.
 * RECOMP_P5D_DQS60=1 enables the experiment (log first hits). */
void recomp_ihook_p5d_dqs60(Cpu *c) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_DQS60"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern int window_p5d_dance_screen(void);
	if (!display_frame_rate_60() || !window_p5d_dance_screen()) return;
	c->vfp.f[1] = 1.0f / 60.0f;
	{ static int n; if (n < 10) { n++; fprintf(stderr, "[DQS60] quantizer scale -> 1/60 f=%llu\n", (unsigned long long)display_frames()); } }
}

/* M1645: fn_81045cc4 converts the animation time to frame indices with a literal 30.0 (vmov.f32 s0,#30.0 at
 * 0x81045cdc: three separate time*fps conversions follow). If the dance motions are authored at 60 keys per second,
 * feeding 60 here (together with the 1/60 per presented frame time continuation, RECOMP_P5D_POSE60) makes the sampler
 * pick a distinct key on every presented frame at the SAME real-time speed. If the motions are 30 keys per second this
 * makes the animation play 2x fast, which the A/B capture will show immediately (total motion per second doubles), so
 * this is strictly an experiment: RECOMP_P5D_POSE60_FPS=1. */
void recomp_ihook_p5d_posefps(Cpu *c, int reg) {   /* after "vmov.f32 s0, #30.0" in fn_81045cc4 */
	(void)reg;
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_POSE60_FPS"); on = e && atoi(e) != 0; }
	if (!on || !display_frame_rate_60()) return;
	c->vfp.f[0] = 60.0f;
}

/* M1650: THE animation time setter. fn_8103e904(array, count) loops over the character's animation nodes and does
 * "node->time = s0" (vstr s0,[r0,#0x20] at 0x8103e912, flag node+0x18 |= 2). The dance main function calls it once per
 * presented frame with the dance clock snapped to 1/30 s, so every node's time only moves on every second frame - this
 * is the node-level 30 Hz lock (found by write-watching node+0x20: writer fn_8103e904+0x9d, caller fn_810481ce+0x644,
 * danceMain fn_81043fee+0x539). In the 60 fps mode, smooth this value: keep our own 1/60-step continuation and resync to
 * the game's value when it steps. All nodes in one frame get the same value. The sampler (fn_81045cc4) then receives a
 * time that advances every presented frame at the same real-time speed (RECOMP_P5D_NODEFPS=1; 30 fps mode untouched). */
void recomp_ihook_p5d_nodefps(Cpu *c, int reg) {   /* after "vstr s0,[r0,#0x20]" (0x8103e912) */
	(void)reg;
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_NODEFPS"); on = e ? atoi(e) != 0 : 1; }   /* M1624: promoted (on in the 60 fps mode, RECOMP_P5D_NODEFPS=0 disables) */
	if (!on || !display_frame_rate_60()) return;
	extern uint64_t display_frames(void);
	uint64_t f = display_frames();
	static uint64_t lastf = ~0ull; static float out, last_s0; static int have; static unsigned changes, steps60, frames; static uint64_t lastst;
	if (f != lastf) {
		float s0; memcpy(&s0, &c->vfp.s[0], 4);
		static unsigned repeat_streak;
		if (!have) { out = s0; last_s0 = s0; have = 1; repeat_streak = 0; }
		else if (s0 == last_s0) {
			/* The game repeats its value on every second frame while a 30 fps clock runs, but it repeats FOREVER when the
			 * dance is paused (pause menu, screen freeze): only continue our 1/60 step for the normal one-frame repeat and
			 * freeze the value after that, so pausing really freezes the dancers. */
			repeat_streak++;
			if (repeat_streak <= 2) out += 0.0166666675f;
		} else {
			out = s0;                                  /* game advanced (or restarted/looped/jumped): adopt its value */
			repeat_streak = 0;
		}
		last_s0 = s0;
		lastf = f;
		{ static int st = -1; if (st < 0) { const char *e = getenv("RECOMP_P5D_NODEFPS_STAT"); st = e && atoi(e) != 0; }
		  static float lastout; static int havelast;
		  if (st) {
			frames++;
			if (havelast && out != lastout) { changes++; float d = out - lastout; if (d > 0.014f && d < 0.018f) steps60++; }
			lastout = out; havelast = 1;
			if (frames >= 60) { fprintf(stderr, "[NODEFPSSTAT] f=%llu changes=%u steps60=%u out=%.5f\n",
				(unsigned long long)f, changes, steps60, out); changes = steps60 = frames = 0; } } }
	}
	memcpy(&c->vfp.s[0], &out, 4);
}

/* M1635: THE "AUTO PLAY" CHEAT PORT (PCSH10109, the Vita cheat file's 自动演奏).
 * The PSV code is
 *     $B200 00000000 00000000        (addresses relative to eboot segment 0)
 *     $A100 0011B6FC 00001C00        (16-bit instruction patch: beq #0x8111b764 -> 0x1C00 = adds r0,r0,#0 = NOP)
 * so the conditional branch right after "ands r1, fp, #0x40" is never taken: the dance code's auto-hit path that the
 * branch would skip always runs. The recompiled host cannot patch guest code, so the same effect comes from an
 * instruction hook right AFTER that ands (0x8111b6f8): with the cheat on, clear Z so the following beq falls through.
 * F5 toggles it in game (window.c), default off. */
int p5d_autoplay_enabled;
void recomp_ihook_p5d_autoplay(Cpu *c, int reg) {
	(void)reg;
	if (p5d_autoplay_enabled) c->z = 0;   /* the beq at 0x8111b6fc tests Z; clearing it = the NOP'ed branch */
}

/* M1649: fn_81045fb4(node, time_bits, r2) is the per-frame pose update entry: it forwards its r1 (float bits of the
 * animation time) to the sampler fn_81045cc4. Log the time each presentation frame and put a write watch on the field it
 * is read from (node+0x22c) to find the writer that steps it, so the 30 Hz stepping locates to one function.
 * RECOMP_P5D_POSEUPD=1. */
void recomp_hook_p5d_poseupd(Cpu *c) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_POSEUPD"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	extern void wwatch_set_target(void *host_ptr);
	extern int wwatch_protect(void *host_ptr);
	uint32_t node = c->r[0];
	/* fn_81045cc4 does "mov fp, r0" and then "vldr s16,[fp,#0x20]", so the animation time the sampler uses is the float
	 * at node+0x20 (not r1). Log it and write-watch that field to catch its writer. */
	float t = 0.0f;
	if (node > 0x10000u && node < 0x88000000u) { uint32_t w = RD32(node + 0x20u); memcpy(&t, &w, 4); }
	fprintf(stderr, "[PU] f=%llu node=%#x t=%.5f r1=%#x\n", (unsigned long long)display_frames(), node, t, c->r[1]);
	if (node > 0x10000u && node < 0x88000000u && node + 0x24u < 0x88000000u) {
		/* range mode + single-step: every write to the page faults and only writes into [node+0x20, node+0x24) are
		 * logged, so the first write of the frame cannot hide the field's writer behind another page write. */
		extern void wwatch_set_range(uint32_t lo, uint32_t hi);
		wwatch_set_range(node + 0x20u, node + 0x24u);
		wwatch_set_target(0);
		wwatch_protect((void *)G(node + 0x20u));
	}
}

/* M1643: per-call probe for the other functions that carry a float 30.0 constant in the recompiled code (candidates for
 * the pose sampler's own 30 Hz clock). Logs, for the dance frames only, the entry s0 and r0-r3 so a value that steps
 * 1/30 s every second presented frame (or a frame index that steps every second frame) is visible directly.
 * RECOMP_P5D_CD=1, optional RECOMP_P5D_CD_FROM/_TO frame window (defaults to the dance screen). */
static void p5d_cd_probe(const char *name, Cpu *c) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_CD"); on = e && atoi(e) != 0; }
	if (!on) return;
	extern uint64_t display_frames(void);
	extern int window_p5d_dance_screen(void);
	static long cf = -2, ct = -2;
	if (cf == -2) { const char *a = getenv("RECOMP_P5D_CD_FROM"), *b = getenv("RECOMP_P5D_CD_TO"); cf = a ? atol(a) : -1; ct = b ? atol(b) : -1; }
	uint64_t f = display_frames();
	if (cf >= 0 ? ((long)f < cf || (ct >= 0 && (long)f > ct)) : !window_p5d_dance_screen()) return;
	fprintf(stderr, "[CD] %s f=%llu s0=%08x r0=%08x r1=%08x r2=%08x r3=%08x\n", name,
		(unsigned long long)f, c->vfp.s[0], c->r[0], c->r[1], c->r[2], c->r[3]);
}
void recomp_hook_p5d_cdA(Cpu *c) { p5d_cd_probe("A_81042e24", c); }
void recomp_hook_p5d_cdB(Cpu *c) { p5d_cd_probe("B_8104590e", c); }
void recomp_hook_p5d_cdC(Cpu *c) {
	p5d_cd_probe("C_81045cc4", c);
	/* M1644: fn_81045cc4 is the motion sampler/blender on the dance pose path; its float argument s0 is the animation
	 * time and the engine hands it in 1/30 s steps, one step every second presented frame (measured: 447 of 893
	 * consecutive frames carried the same value, steps of 0.03333). In the 60 fps mode continue the value by 1/60 on
	 * every presented frame (when the engine repeats last frame's value, hand out previous_out + 1/60; when the engine
	 * steps, pass its new value through). That gives every presented frame its own animation time at exactly the same
	 * real-time speed, so the sampler can interpolate between its keys. Per-node table keyed by r0. 30 fps mode and the
	 * other screens are untouched. RECOMP_P5D_POSE60=1 (experiment; off by default until the picture A/B passes). */
	{
		static int on = -1;
		if (on < 0) { const char *e = getenv("RECOMP_P5D_POSE60"); on = e && atoi(e) != 0; }
		if (on && display_frame_rate_60()) {
			static struct { uint32_t node; uint32_t last_native; float out; } slots[64];
			float cur; memcpy(&cur, &c->vfp.s[0], 4);
			uint32_t cw; memcpy(&cw, &cur, 4);
			int i = 0; for (; i < 64; i++) if (slots[i].node == c->r[0]) break;
			if (i == 64) { i = 0; for (; i < 64; i++) if (!slots[i].node) break; }
			if (i < 64) {
				if (!slots[i].node) { slots[i].node = c->r[0]; slots[i].last_native = cw; slots[i].out = cur; }
				else if (cw != slots[i].last_native) { slots[i].last_native = cw; slots[i].out = cur; }
				else slots[i].out += 0.0166666675f;
				memcpy(&c->vfp.s[0], &slots[i].out, 4);
				/* statistics: per 60 presented frames count changes and 1/60 steps of the handed-out value */
				{ static int st = -1; if (st < 0) { const char *e = getenv("RECOMP_P5D_POSE60_STAT"); st = e && atoi(e) != 0; }
				  if (st) {
					static float last; static int have; static unsigned changes, steps60, frames; static uint64_t lastf;
					extern uint64_t display_frames(void);
					frames++;
					if (have && slots[i].out != last) { changes++; float d = slots[i].out - last; if (d > 0.014f && d < 0.018f) steps60++; }
					last = slots[i].out; have = 1;
					uint64_t f = display_frames();
					if (f - lastf >= 60) { lastf = f;
						fprintf(stderr, "[POSE60STAT] f=%llu frames=%u changes=%u steps60=%u val=%.4f\n",
							(unsigned long long)f, frames, changes, steps60, slots[i].out);
						changes = steps60 = frames = 0; } } }
			}
		}
	}
}
void recomp_hook_p5d_cdD(Cpu *c) { p5d_cd_probe("D_810481ce", c); }
void recomp_hook_p5d_cdE(Cpu *c) { p5d_cd_probe("E_8103a32e", c); }
void recomp_hook_p5d_cdF(Cpu *c) { p5d_cd_probe("F_8103a916", c); }
void recomp_hook_p5d_cdG(Cpu *c) { p5d_cd_probe("G_8103c320", c); }

/* M1641: the dance animation clock stepper fn_81049976 (see the exact disassembly in the docs):
 *   s0 = 30.0; s1 = [r0](time seconds); r2 = [r0+8](last frame index); s0 = s1*s0; s0 = (int)s0  -> 30 fps frame index
 *   if (frame <= last) frame += 1 (advance at least one authored frame); last = frame
 *   s1 = 1/30; s0 = (float)frame; s0 = s0 * s1; [r0] = s0 (time snapped to a 30 fps keyframe)
 * Called once per presented frame; the real-time clock advances 1/60 s per call, so floor(t*30) only changes every
 * second call -> the pose advances 30 times a second (the 30 Hz the user sees) while the port presents 60.
 * In the 60 fps mode use 60 instead of 30 in both places: the frame index advances on every presented frame and the
 * returned time is a 1/60 s step, i.e. true 60 Hz animation at the same real-time speed. 30 fps mode untouched.
 * RECOMP_P5D_ANIM60 (on in the 60 mode once promoted; env=0 disables). */
static int p5d_anim60_on(void) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_ANIM60"); on = e && atoi(e) != 0; }   /* experiment: default off
		* until the rendered-pose cadence actually improves (the clock is correct at 60 Hz but the pose sampler showed no
		* visible change in the A/B captures, so it re-quantizes or uses another time source - see the docs) */
	return on && display_frame_rate_60();
}
void recomp_ihook_p5d_anim60_fps(Cpu *c, int reg) {   /* after "vmov.f32 s0, #30.0" */
	(void)reg;
	if (!p5d_anim60_on()) return;
	c->vfp.f[0] = 60.0f;
	{ static int n; if (n < 3) { n++; extern uint64_t display_frames(void); fprintf(stderr, "[ANIM60] fps 30 -> 60 at f=%llu\n", (unsigned long long)display_frames()); } }
}
void recomp_ihook_p5d_anim60_scale(Cpu *c, int reg) {  /* after "vmov s1, r2" with r2 = 0x3d088889 (1/30) */
	(void)reg;
	if (!p5d_anim60_on()) return;
	c->vfp.s[1] = 0x3c888889u;   /* 1/60 */
}
/* M1642: statistic for the 1/60 clock: sample the time this function computes each call (after vstr) and print once
 * per 60 presented frames how many times it changed and how many of the steps were ~1/60 (ideal: 60 changes / 60). */
void recomp_ihook_p5d_anim60_stat(Cpu *c, int reg) {   /* after "vstr s0, [r0]" */
	(void)reg;
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_ANIM60_STAT"); on = e && atoi(e) != 0; }
	if (!on) return;
	static float last; static int have; static unsigned changes, steps60, frames;
	static uint64_t lastf;
	frames++;
	float t = c->vfp.f[0];
	if (have && t != last) { changes++; float d = t - last; if (d > 0.014f && d < 0.018f) steps60++; }
	last = t; have = 1;
	{ extern uint64_t display_frames(void); uint64_t f = display_frames(); if (f - lastf >= 60) {
		lastf = f; fprintf(stderr, "[ANIM60STAT] f=%llu frames=%u changes=%u steps60=%u t=%.4f\n",
			(unsigned long long)f, frames, changes, steps60, t); changes = steps60 = frames = 0; } }
}

/* M1540 plan B probes (RECOMP_P5D_TICK_LOG=1) */
void recomp_ihook_p5d_mframe_conv(Cpu *c, int reg) {
	(void)reg;
	static int on = -1; static uint64_t last;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_TICK_LOG"); on = e && atoi(e) != 0; }
	if (!on) return;
	uint64_t f = display_frames();
	if (f - last < 60) return;
	last = f;
	fprintf(stderr, "[MFCONV] f=%llu in=%u out=%u rate=%s\n", (unsigned long long)f, c->r[4], c->r[0],
	        display_frame_rate_60() ? "60" : "30");
}
void recomp_ihook_p5d_title_tick(Cpu *c, int reg) {
	(void)reg;
	static int on = -1; static uint64_t last;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_TICK_LOG"); on = e && atoi(e) != 0; }
	if (!on) return;
	uint64_t f = display_frames();
	if (f - last < 30) return;
	last = f;
	fprintf(stderr, "[TTICK] f=%llu counter=%u threshold=%u rate=%s\n", (unsigned long long)f, c->r[5], c->r[0],
	        display_frame_rate_60() ? "60" : "30");
}

/* M1540 plan B: double the frame count the game's appMframe conversion returns while the effective rate is 60.
 * At 30 the conversion is the identity (input==output, probed); at 60 a count authored for the 30 fps base must
 * be twice as many presented frames to cover the same time. The frame-rate / hybrid scope rules are the ones
 * display.c uses for the swap interval. */
extern int window_p5d_dance_screen(void);
void recomp_ihook_p5d_mframe_scale(Cpu *c, int reg) {
	(void)reg;
	/* M-B1 code-level verification: log the appMframe conversion input/output (r4 = authored frame count, r0 = the
	 * converted value) and the singleton's frame-base index, every 30 presented frames. */
	{
		static int logc = -1; static uint64_t last;
		if (logc < 0) { const char *e = getenv("RECOMP_P5D_MFCONV_LOG"); logc = e && atoi(e) != 0; }
		if (logc) {
			uint64_t f = display_frames();
			if (f - last >= 30) {
				last = f;
				uint32_t p = RD32(0x8132ae64u);
				uint32_t base = p ? RD32(p + 8) : 0u;
				float out; memcpy(&out, &c->vfp.s[0], 4);
				fprintf(stderr, "[MFCONV] f=%llu in=%u out_int=%u out_float=%.4f base=%u rate=%s\n",
				        (unsigned long long)f, c->r[4], c->r[0], out, base,
				        display_frame_rate_60() ? "60" : "30");
			}
		}
	}
	if (!display_frame_rate_60()) return;
	static int on = -1;   /* experiment, off by default until its effect on the dance stage is verified */
	if (on < 0) { const char *e = getenv("RECOMP_P5D_MFRAME_SCALE"); on = e && atoi(e) != 0; }
	if (!on) return;
	static int scope = -1;
	if (scope < 0) { const char *e = getenv("RECOMP_FPS_SCOPE"); scope = !(e && *e && !strcmp(e, "all")); }
	if (scope && !window_p5d_dance_screen()) return;
	{ static int n; if (n < 20) { n++; fprintf(stderr, "[MFSCALE] f=%llu in=%u -> in*2=%u\n", (unsigned long long)display_frames(), c->r[4], c->r[4] * 2u); } }
	c->r[0] = c->r[4] * 2u;   /* r4 = the count passed in, r0 = the conversion's result */
}

/* M1540 verification: the dance stage's clock. danceMain_81043FEE keeps proc+0x458 (float seconds, added from
 * the engine delta time or the CRI music position) and proc+0x474 (last music playing time). RECOMP_P5D_DANCE_LOG=1
 * logs both every 60 presented frames; comparing 30 vs 60 fps shows whether the stage advances in real time. */
/* M1628: the dance stage's own clock, readable by the window (the pause menu freezes the stage: a clock that starts
 * advancing again means the game is playing once more). Updated on every danceMain step, whether or not logging is on. */
static volatile float p5d_dance_clock_export;
float p5d_dance_clock_now(void) { return p5d_dance_clock_export; }
void recomp_ihook_p5d_dance_clock(Cpu *c, int reg) {
	(void)reg;
	{ uint32_t p0 = c->r[4];
	  if (p0 > 0x10000u && p0 < 0x88000000u) { float cv; uint32_t w = RD32(p0 + 0x458); memcpy(&cv, &w, 4); p5d_dance_clock_export = cv; } }
	static int on = -1; static uint64_t last;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_DANCE_LOG"); on = e && atoi(e) != 0; }
	if (!on) return;
	uint64_t f = display_frames();
	if (f - last < 60) return;
	last = f;
	uint32_t p = c->r[4];
	float clk = 0.0f, mt = 0.0f;
	if (p) { uint32_t cv = RD32(p + 0x458), mv = RD32(p + 0x474); memcpy(&clk, &cv, 4); memcpy(&mt, &mv, 4); }
	fprintf(stderr, "[DCLK] f=%llu clk=%.4f music=%.4f rate=%s\n", (unsigned long long)f, clk, mt,
	        display_frame_rate_60() ? "60" : "30");
}

/* M1545 full-60 experiment: fn_81178dce / fn_81178e84 are the small-count job steps the S0/title workers call
 * (r2 = 10 in the S0 calls). RECOMP_P5D_WAIT_SCALE=1 doubles r1, =2 doubles r2, only at 60 fps with the
 * all-screen scope; the title-sequence wall time measured against the 30 fps baseline tells whether this is the
 * frame-count wait the full-60 work needs. Off by default. */
static void p5d_wait_scale(Cpu *c) {
	static int mode = -1;
	if (mode < 0) { const char *e = getenv("RECOMP_P5D_WAIT_SCALE"); mode = e ? atoi(e) : 0; }
	if (!mode || !display_frame_rate_60()) return;
	const char *scope = getenv("RECOMP_FPS_SCOPE");
	if (!(scope && !strcmp(scope, "all"))) return;
	if (mode == 1) c->r[1] *= 2u;
	else if (mode == 2) c->r[2] *= 2u;
}
void recomp_hook_p5d_wait_a(Cpu *c) { p5d_wait_scale(c); }
void recomp_hook_p5d_wait_b(Cpu *c) { p5d_wait_scale(c); }

/* M1546: RECOMP_P5D_MFSTATE=1 logs the appMframe singleton from a per-frame title/menu animation function.
 * p = *(u32*)0x8132ae64; fields [+0],[+4],[+8] are the boot init arguments; +8 indexes the game's frame-rate
 * table {1.0, 30.0, 60.0}. */
static void p5d_mfstate(Cpu *c) {
	static int on = -1; static uint64_t last;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_MFSTATE"); on = e && atoi(e) != 0; }
	if (!on) return;
	uint64_t f = display_frames();
	if (f - last < 120) return;
	last = f;
	uint32_t p = RD32(0x8132ae64u);
	uint32_t a = p ? RD32(p) : 0, b = p ? RD32(p + 4) : 0, d = p ? RD32(p + 8) : 0;
	fprintf(stderr, "[MFSTATE] f=%llu p=%#x fields=%u/%u/%u rate=%s\n", (unsigned long long)f, p, a, b, d,
	        display_frame_rate_60() ? "60" : "30");
	(void)c;
}
void recomp_hook_p5d_mfstate_a(Cpu *c) { p5d_mfstate(c); }
void recomp_hook_p5d_mfstate_b(Cpu *c) { p5d_mfstate(c); }

/* Phase A: RECOMP_P5D_JOBTIME=1 logs the job-time getter fn_81158c20's result (s0) every 30 presented frames. */
void recomp_ihook_p5d_jobtime(Cpu *c, int reg) {
	(void)reg;
	static int on = -1; static uint64_t last;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_JOBTIME"); on = e && atoi(e) != 0; }
	if (!on) return;
	uint64_t f = display_frames();
	if (f - last < 30) return;
	last = f;
	float v; memcpy(&v, &c->vfp.s[0], 4);
	fprintf(stderr, "[JOBTIME] f=%llu v=%.4f rate=%s\n", (unsigned long long)f, v,
	        display_frame_rate_60() ? "60" : "30");
	/* Phase A experiment: this getter is frames/30 (frame-index seconds). At 60 fps it advances 2x real time;
	 * halving it makes the frame-index animation clock advance in real time. Opt-in. */
	static int slow = -1;
	if (slow < 0) { const char *e = getenv("RECOMP_P5D_JOBSLOW"); slow = e && atoi(e) != 0; }
	if (slow && display_frame_rate_60()) {
		v *= 0.5f;
		memcpy(&c->vfp.s[0], &v, 4);
		fprintf(stderr, "[JOBSLOW] f=%llu v->%.4f\n", (unsigned long long)f, v);
	}
}

/* Phase A: RECOMP_P5D_PREVIEW=1 logs the arguments of the main-menu preview-model path (0x8110e200) and the
 * appGxModel entry (0x810059a4) every 30 presented frames. */
void recomp_hook_p5d_preview_a(Cpu *c) {
	static int on = -1; static uint64_t last;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_PREVIEW"); on = e && atoi(e) != 0; }
	if (!on) return;
	uint64_t f = display_frames();
	if (f - last < 30) return;
	last = f;
	float s0; memcpy(&s0, &c->vfp.s[0], 4);
	fprintf(stderr, "[PREVIEW] f=%llu r0=%#x r1=%#x r2=%#x r3=%#x s0=%.4f rate=%s\n",
	        (unsigned long long)f, c->r[0], c->r[1], c->r[2], c->r[3], s0,
	        display_frame_rate_60() ? "60" : "30");
}
void recomp_hook_p5d_preview_b(Cpu *c) {
	static int on = -1; static uint64_t last;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_PREVIEW"); on = e && atoi(e) != 0; }
	if (!on) return;
	uint64_t f = display_frames();
	if (f - last < 30) return;
	last = f;
	float s0; memcpy(&s0, &c->vfp.s[0], 4);
	fprintf(stderr, "[GXMODEL] f=%llu r0=%#x r1=%#x r2=%#x r3=%#x s0=%.4f rate=%s\n",
	        (unsigned long long)f, c->r[0], c->r[1], c->r[2], c->r[3], s0,
	        display_frame_rate_60() ? "60" : "30");
}

/* Phase B M-B1: the appCalculator keyframe evaluator (0x81154b9c) takes its input time as a float (s0). At a 60 fps
 * presentation the frame-index drivers make that time advance 2x real time; halving it here is the dt-ization of
 * the animation-evaluation layer. Opt-in while under test. */
void recomp_hook_p5d_evalslow(Cpu *c) {
	static int mode = -1, log = -1; static uint64_t last;
	if (mode < 0) { const char *e = getenv("RECOMP_P5D_EVALSLOW"); mode = e && atoi(e) != 0; }
	if (log < 0) { const char *e = getenv("RECOMP_P5D_EVAL_LOG"); log = e && atoi(e) != 0; }
	float t; memcpy(&t, &c->vfp.s[0], 4);
	if (log) {
		uint64_t f = display_frames();
		if (f - last >= 30) {
			last = f;
			fprintf(stderr, "[EVAL] f=%llu t=%.4f r1=%#x r2=%#x lr=%#x rate=%s\n", (unsigned long long)f, t,
			        c->r[1], c->r[2], c->r[14], display_frame_rate_60() ? "60" : "30");
		}
	}
	if (mode && display_frame_rate_60()) {
		t *= 0.5f;
		memcpy(&c->vfp.s[0], &t, 4);
	}
}

/* Phase B M-B1: RECOMP_P5D_EASE_LOG=1 logs the easing helper (0x81154e06) caller LR and input value every N calls:
 * the real consumers of the float-time keyframe evaluator (main-menu background/camera, UI fades, ...). */
void recomp_hook_p5d_easelog(Cpu *c) {
	static int on = -1, dt = -1; static uint64_t last;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_EASE_LOG"); on = e && atoi(e) != 0; }
	if (dt < 0) { const char *e = getenv("RECOMP_P5D_EASE_DT"); dt = e && atoi(e) != 0; }
	uint32_t lr = c->r[14];
	/* Consumers whose keyframe-evaluator input is a live presented-frame counter (slope 1 unit per presented frame
	 * measured in the dance log): their animations run 2x real time at 60 fps until the input is halved. */
	int menu_consumer = lr == 0x810EE5E7u || lr == 0x810EFD31u || lr == 0x810F6DA3u ||
	                    lr == 0x810F07A3u || lr == 0x8113858Bu || lr == 0x810C5FD7u || lr == 0x811376B7u ||
	                    /* M1618: the DanceReady_P5 song-select character's second evaluator call (0x81138693): its
	                     * input is the presented-frame counter too, so it needs the same real-time halving. */
	                    lr == 0x81138693u;
	if (on) {
		uint64_t f = display_frames();
		if (f - last >= 30) {
			last = f;
			float s0; memcpy(&s0, &c->vfp.s[0], 4);
			fprintf(stderr, "[EASE] f=%llu s0=%.4f r0=%#x r1=%#x lr=%#x rate=%s\n", (unsigned long long)f, s0,
			        c->r[0], c->r[1], lr, display_frame_rate_60() ? "60" : "30");
		}
	}
	if (dt && menu_consumer && display_frame_rate_60()) {
		float t; memcpy(&t, &c->vfp.s[0], 4);
		t *= 0.5f;
		memcpy(&c->vfp.s[0], &t, 4);
	}
}

/* Phase B M-B1: RECOMP_P5D_DANCE_QP=1 logs the 1/30 quantizer fn_81049976 (it snaps a float time to 1/30 s:
 * frames=ceil(t*30), t'=frames/30) every 30 presented frames, so its per-frame advance can be compared at 30/60. */
void recomp_hook_p5d_danceqp(Cpu *c) {
	static int on = -1; static uint64_t last;
	if (on < 0) { const char *e = getenv("RECOMP_P5D_DANCE_QP"); on = e && atoi(e) != 0; }
	if (!on) return;
	uint64_t f = display_frames();
	if (f - last < 30) return;
	last = f;
	uint32_t obj = c->r[0];
	float t = RD32(obj);
	uint32_t n = RD32(obj + 8);
	fprintf(stderr, "[DQP] f=%llu obj=%#x t=%.5f n=%u rate=%s\n", (unsigned long long)f, obj, t, n,
	        display_frame_rate_60() ? "60" : "30");
}

/* Phase B M-B1 multi-site probe: one function registered at many dance-path entries. Every 30 presented frames it
 * logs the site PC, r0..r3 and the s0 float, so per-site slopes can be compared at 30 vs 60 fps without screenshots.
 * A value that advances 1.0 per presented frame at 60 fps is a frame-count clock (needs dt-ization); 0.5 means the
 * site is already dt-aware. */
void recomp_hook_p5d_multiprobe(Cpu *c) {
	static int on = -1;
	static uint64_t last[32];
	if (on < 0) { const char *e = getenv("RECOMP_P5D_MULTI"); on = e && atoi(e) != 0; }
	if (!on) return;
	uint32_t pc = c->r[15];
	int slot = (pc >> 2) & 31u;
	uint64_t f = display_frames();
	if (f - last[slot] < 30) return;
	last[slot] = f;
	float s0; memcpy(&s0, &c->vfp.s[0], 4);
	fprintf(stderr, "[MULTI] f=%llu pc=%#x r0=%#x r1=%#x r2=%#x r3=%#x s0=%.5f rate=%s\n",
	        (unsigned long long)f, pc, c->r[0], c->r[1], c->r[2], c->r[3], s0,
	        display_frame_rate_60() ? "60" : "30");
}

/* Phase B M-B1 multi-site probe v2: explicit per-site wrappers (c->r[15] is not the entry VA at --hooks time). */
static void p5d_site_log(int id, Cpu *c) {
	static int on = -1;
	static uint64_t last[16];
	if (on < 0) { const char *e = getenv("RECOMP_P5D_MULTI"); on = e && atoi(e) != 0; }
	if (!on || id < 0 || id >= 16) return;
	uint64_t f = display_frames();
	if (last[id] && f - last[id] < 30) return;
	last[id] = f;
	float s0; memcpy(&s0, &c->vfp.s[0], 4);
	uint32_t obj = c->r[0];
	uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0, w4 = 0, w5 = 0, w6 = 0, w7 = 0;
	if (obj >= 0x10000000u && obj < 0x90000000u) {
		w0 = RD32(obj); w1 = RD32(obj+4); w2 = RD32(obj+8); w3 = RD32(obj+12);
		w4 = RD32(obj+16); w5 = RD32(obj+20); w6 = RD32(obj+24); w7 = RD32(obj+28);
	}
	fprintf(stderr, "[SITE] id=%d f=%llu r0=%#x r1=%#x r2=%#x r3=%#x s0=%.5f w=%u,%u,%u,%u,%u,%u,%u,%u rate=%s\n",
	        id, (unsigned long long)f, c->r[0], c->r[1], c->r[2], c->r[3], s0,
	        w0, w1, w2, w3, w4, w5, w6, w7,
	        display_frame_rate_60() ? "60" : "30");
}
void recomp_hook_p5d_site_0(Cpu *c) { p5d_site_log(0, c); }
void recomp_hook_p5d_site_1(Cpu *c) { p5d_site_log(1, c); }
void recomp_hook_p5d_site_2(Cpu *c) { p5d_site_log(2, c); }
void recomp_hook_p5d_site_3(Cpu *c) { p5d_site_log(3, c); }
void recomp_hook_p5d_site_4(Cpu *c) { p5d_site_log(4, c); }
void recomp_hook_p5d_site_5(Cpu *c) { p5d_site_log(5, c); }
void recomp_hook_p5d_site_6(Cpu *c) { p5d_site_log(6, c); }
void recomp_hook_p5d_site_7(Cpu *c) { p5d_site_log(7, c); }
void recomp_hook_p5d_site_8(Cpu *c) { p5d_site_log(8, c); }
void recomp_hook_p5d_site_9(Cpu *c) { p5d_site_log(9, c); }
void recomp_hook_p5d_site_10(Cpu *c) { p5d_site_log(10, c); }
void recomp_hook_p5d_site_11(Cpu *c) { p5d_site_log(11, c); }
void recomp_hook_p5d_site_12(Cpu *c) { p5d_site_log(12, c); }
void recomp_hook_p5d_site_13(Cpu *c) { p5d_site_log(13, c); }
void recomp_hook_p5d_site_14(Cpu *c) { p5d_site_log(14, c); }
void recomp_hook_p5d_site_15(Cpu *c) { p5d_site_log(15, c); }
void recomp_hook_p5d_site_16(Cpu *c) { p5d_site_log(16, c); }
void recomp_hook_p5d_site_17(Cpu *c) { p5d_site_log(17, c); }
void recomp_hook_p5d_site_18(Cpu *c) { p5d_site_log(18, c); }
void recomp_hook_p5d_site_19(Cpu *c) { p5d_site_log(19, c); }
void recomp_hook_p5d_site_20(Cpu *c) { p5d_site_log(20, c); }
void recomp_hook_p5d_site_21(Cpu *c) { p5d_site_log(21, c); }
