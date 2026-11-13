/*
 * The host window (SDL3): shows presented frames and turns keyboard and gamepad input into the
 * Vita's controller state. SDL wants the process's main thread, so in windowed mode the game runs
 * on its own thread (window_run) and hands each frame over through a shared buffer. Presentation
 * goes through SDL's GPU API (SDL_GPU): the software backend's frame is uploaded as a sampled
 * texture and blitted to the swapchain. This also stands up the SDL_GPUDevice/SDL_Window that a
 * future RECOMP_GFX=gpu backend (gfx_gpu.c) will render into directly instead of blitting.
 *
 * keys: arrows d-pad, Z cross, X circle, A square, S triangle, Enter start, Backspace select,
 *       Q/W L/R triggers; any SDL gamepad (A/B/X/Y as cross/circle/square/triangle); F1 cycles the
 *       performance overlay off / basic (FPS + frame time) / full (metrics.c); F10 (or a gamepad's
 *       Back+Start held together, or its Guide button) opens/closes the in-game menu (overlay.c*,
 *       Dear ImGui), Escape also closes it -- while it's open every key/mouse/gamepad event above
 *       goes to the menu instead, not the game (see the event loop's own comment on why). The mouse
 *       resting at the window's top edge slides the menu BAR down (overlay.cpp: the same settings,
 *       without pausing; every item shows its hotkey); mouse events always reach ImGui so it can see
 *       that hover, and one it captures (the bar, a menu, the pull tab) is never a touch. Escape
 *       hides the bar. The OS cursor hides after a few seconds still over the picture (overlay.cpp).
 * env:  RECOMP_HEADLESS=1   no window (tests, the checker); RECOMP_SCALE=n window scale (default 1)
 *       RECOMP_RENDER_SCALE=n the GPU backend's internal resolution (1-6, default 1; F2 cycles them: gfx_gpu.c)
 *       RECOMP_OSD=0|1|2    the overlay's level at start-up (default 0: see metrics.c and window_present_gpu's own comment on why the GPU path doesn't default it on)
 *       RECOMP_METRICS=1    one performance line to stderr a second (metrics.c), with or without a window
 *       RECOMP_OSD_DUMP=f   writes the overlay's own bitmap to f once, to check the font (see metrics.c)
 *       RECOMP_OVERLAY_DUMP=f.ppm  headless: renders the in-game menu once, open, over the next
 *                           presented frame, into an offscreen texture, and writes it to f -- so the
 *                           menu can be looked at without a screen (see overlay_dump_maybe below)
 *       RECOMP_OVERLAY_DUMP_UI=menu|menu:<tab>|bar|Game|View|Controls|Help|toast|intro[:pad|:touch][@<s>]|quit|layout[,...]  what that dump
 *                           shows (overlay.h: overlay_render_forced); several, comma-separated, write
 *                           one file each, f_<name>.ppm
 */
#include <SDL3/SDL.h>
#include "gpulock.h"   /* every SDL submit/fence call below is the locked one: Homebrew SDL 3.4.16's Metal fence race, see that header */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "rt.h"
#include "hle.h"
#include "metrics.h"
#include "settings.h"
#include "overlay.h"
#include "third_party/stb/stb_image.h"   /* decode the P5 banner PNGs (the implementation lives in runtime/media/jpeg.c) */
#include "banner_art.h"                /* M1636: the P5-style auto-play banner, RGBA PNG with its text baked in */

/* M1636: the F5 cheat's P5-style notification. It is an overlay of the PORT, not part of the game: the frame's CPU
 * round trip that already composites the on-screen stats (metrics_osd_composite in osd_round_trip below) also blends
 * this banner, so it appears whenever OSD would - and forces that round trip while it is visible, even with the OSD off. */
void p5d_banner_show(int on, double seconds);
static const uint8_t *p5d_banner_rgba[2]; static int p5d_banner_w[2], p5d_banner_h[2];
static int p5d_banner_cur = -1; static uint64_t p5d_banner_until;
static void p5d_banner_load(void) {
	static int done;
	if (done) return;
	done = 1;
	const unsigned char *src[2] = { p5_banner_on_png, p5_banner_off_png };
	const size_t len[2] = { sizeof p5_banner_on_png, sizeof p5_banner_off_png };
	for (int i = 0; i < 2; i++) {
		int w = 0, h = 0, n = 0;
		unsigned char *px = stbi_load_from_memory(src[i], (int)len[i], &w, &h, &n, 4);
		if (px && w > 0 && h > 0) { p5d_banner_rgba[i] = px; p5d_banner_w[i] = w; p5d_banner_h[i] = h; }
	}
}
void p5d_banner_show(int on, double seconds) {
	p5d_banner_load();
	p5d_banner_cur = on ? 0 : 1;
	p5d_banner_until = (uint64_t)(SDL_GetTicksNS() / 1000) + (uint64_t)(seconds * 1e6);
	fprintf(stderr, "[BANNER] %s (%.1f s)\n", on ? "auto play ON" : "auto play OFF", seconds);
}
static int p5d_banner_visible(void) { return p5d_banner_cur >= 0 && (uint64_t)(SDL_GetTicksNS() / 1000) < p5d_banner_until && p5d_banner_rgba[p5d_banner_cur]; }
/* bilinear sample of the banner RGBA, alpha-blended onto the RGBA frame at the top centre */
static void p5d_banner_composite(uint8_t *rgba, uint32_t stride_px, uint32_t w, uint32_t h) {
	if (!p5d_banner_visible()) return;
	int i = p5d_banner_cur; const uint8_t *s = p5d_banner_rgba[i]; int sw = p5d_banner_w[i], sh = p5d_banner_h[i];
	if (sw <= 0 || sh <= 0) return;
	uint32_t bh = (uint32_t)(h * 0.10f); uint32_t bw = (uint32_t)((double)bh * sw / sh);
	if (bw > (uint32_t)(w * 0.62f)) { bw = (uint32_t)(w * 0.62f); bh = (uint32_t)((double)bw * sh / sw); }
	if (bh < 8 || bw < 8) return;
	uint32_t x0 = (w > bw) ? (w - bw) / 2 : 0, y0 = (uint32_t)(h * 0.025f);
	for (uint32_t y = 0; y < bh && y0 + y < h; y++) {
		double fy = ((double)y + 0.5) * sh / bh - 0.5; int iy = (int)fy; double ty = fy - iy;
		if (iy < 0) { iy = 0; ty = 0; } if (iy >= sh - 1) { iy = sh - 1; ty = 0; }
		for (uint32_t x = 0; x < bw && x0 + x < w; x++) {
			double fx = ((double)x + 0.5) * sw / bw - 0.5; int ix = (int)fx; double fx2 = fx - ix;
			if (ix < 0) { ix = 0; fx2 = 0; } if (ix >= sw - 1) { ix = sw - 1; fx2 = 0; }
			const uint8_t *p00 = s + 4 * ((size_t)iy * sw + ix), *p10 = p00 + 4, *p01 = p00 + 4 * sw, *p11 = p01 + 4;
			double a = (1 - ty) * ((1 - fx2) * p00[3] + fx2 * p10[3]) + ty * ((1 - fx2) * p01[3] + fx2 * p11[3]);
			if (a <= 0.5) continue;
			uint8_t *d = rgba + 4 * ((size_t)(y0 + y) * stride_px + (x0 + x));
			double af = a / 255.0;
			for (int ch = 0; ch < 3; ch++) {
				double v = (1 - ty) * ((1 - fx2) * p00[ch] + fx2 * p10[ch]) + ty * ((1 - fx2) * p01[ch] + fx2 * p11[ch]);
				d[ch] = (uint8_t)(v * af + d[ch] * (1 - af) + 0.5);
			}
		}
	}
	if ((uint64_t)(SDL_GetTicksNS() / 1000) >= p5d_banner_until) p5d_banner_cur = -1;
}

enum { SELECT = 0x1, START = 0x8, UP = 0x10, RIGHT = 0x20, DOWN = 0x40, LEFT = 0x80, LTRIG = 0x100, RTRIG = 0x200,
       TRIANGLE = 0x1000, CIRCLE = 0x2000, CROSS = 0x4000, SQUARE = 0x8000,
       /* a PS TV's pad only (window_input_pad2; the handheld's reads never see them): the stick clicks, and its L2/R2 here
        * (0x100/0x200 in its reads, where LTRIG/RTRIG become its L1/R1) */
       L3 = 0x2, R3 = 0x4, L1 = 0x400, R1 = 0x800, PAD_L2 = 0x10000, PAD_R2 = 0x20000 };

void (*window_f9)(void);   /* F9: set by a VERIFY build (verify.c's count window); none otherwise */
void (*window_f6)(void);   /* F6: set by a VERIFY build (verify.c's 60/30 A/B capture); none otherwise */
/* The game's own scene state, from its sequence manager (static analysis of eboot 0x81185616/0x811857C2 and the
 * 0x813B01B4 app-state table): node = *(0x813B01A8); work = node[+8]; slot = work[+4], where
 * 0 = DEBUG MENU, 1 = initread (boot), 2 = FREE MODE (dance), 3 = COMMU, 4 = TUTORIAL, 5 = MAIN MENU, 6 = TITLE.
 * This is a direct memory read (three RD32s), not picture recognition. -1 while the manager is not up yet. */
static int p5d_app_state_code(void);
static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }   /* metrics.c: the window loop's own upload/blit cost */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *frame;                 /* RGBA, the latest presented frame */
static uint32_t fw, fh, new_frame;
static volatile uint32_t buttons;
static volatile uint8_t sticks[4] = {128, 128, 128, 128};
static int active;
static int self_paced; static uint64_t last_paced_us;   /* a present mode that doesn't wait for vsync: the present loop paces itself */
static volatile int touch_down, touch_x, touch_y;   /* the mouse as the front touch panel (1920x1088) */
/* Gamepad touch -- the agreed scheme (memory: input-touch-mapping-design): the triggers are L and R;
 * the bumpers are touch modifiers. A DS4/DualSense touchpad's fingers touch the front panel finger
 * for finger, or the rear one when RB is held as the finger lands (it stays on that panel until it
 * lifts). Holding LB (front) or RB (rear) on any pad turns the sticks into a cursor on that panel
 * (either one moves it) and Cross into the touch (hold to drag); the game sees both sticks centred meanwhile,
 * and a marker shows where the cursor is (overlay_marks). The right mouse button touches the rear
 * panel as the left one does the front. Panels: front 0..1919 x 0..1087, rear 0..1919 x 108..889
 * (display.c's sceTouchGetPanelInfo). Written by this thread, read by the game's through
 * window_touch_contacts. */
enum { TP_FINGERS = 2, REAR_Y0 = 108, REAR_Y1 = 889 };
static volatile int tp_down[TP_FINGERS], tp_port[TP_FINGERS], tp_x[TP_FINGERS], tp_y[TP_FINGERS];
/* a touchscreen's own fingers (screen_finger): the front panel where each touches the picture, the rear with RB held as it lands */
enum { ST_FINGERS = 6 };
static volatile int st_down[ST_FINGERS], st_port[ST_FINGERS], st_x[ST_FINGERS], st_y[ST_FINGERS]; static SDL_FingerID st_id[ST_FINGERS];
static volatile int cur_mode;              /* 0 off, 1 front (LB held), 2 rear (RB held) */
static volatile int cur_down, cur_x[2] = {960, 960}, cur_y[2] = {544, (REAR_Y0 + REAR_Y1) / 2};
/* Two cursors (two-finger gestures -- Killzone's valves): while a bumper is held, once BOTH sticks have moved the cursor
 * splits in two -- the left stick keeps the first, the right stick steers a second that starts where the first was -- and
 * the press (Cross, L2 or R2) puts both down. Until then either stick moves the one cursor, as before, so one-finger use
 * (menus, drawing) never grows a stray second touch. Reset when the bumper is let go. */
static volatile int cur2_on, cur2_x[2], cur2_y[2];
static int cur_used_l, cur_used_r;
static int cur_cross, cur_trig[2];   /* what holds the cursor touch down: Cross, L2, R2 (cur_down = any of them) */
static int trig_touch[2];            /* this trigger pressed a cursor touch: no L/R to the game until it is let go */
static volatile int mouse_rear_down, mouse_rear_x, mouse_rear_y;
static int lb_held, rb_held, ltrig_on, rtrig_on;   /* the pair that holds the touch cursor; the pair that is L and R (pad_second, pad_main) */
static int ptrig[2];                                /* the physical triggers past their press point (hysteresis) */
/* "shoulder_buttons" (shared): "triggers", the default: the triggers are the game's L and R and the bumpers the other pair
 * (the touch cursor; a PS TV's L2/R2); "bumpers": the other way round, as a PS TV's own pad has it. Read with the bindings
 * (kb_reload: at start and as the menu closes, when nothing is held) */
static int shoulders_swapped;
static volatile int pad_gyro;               /* the pad has a gyroscope and accelerometer streaming (motion.c) */
void motion_feed_accel(const float s[3], uint64_t t_ns); void motion_feed_gyro(const float s[3], uint64_t t_ns);
void motion_recenter(void); void motion_pause(void); void motion_lost(void); void motion_feed_virtual(float angle, uint64_t t_ns); int motion_use_virtual(int on, float *roll);
int window_gyro_present(void) { return pad_gyro; }
static SDL_Window *win;

/* ---- keyboard + mouse (settings "input_mode": auto | pad | kbm, default auto; "mouse_look" 1, "mouse_sens"
 * 1.0, "mouse_invert" 0, "bind_<action>" an SDL key name). auto: the scheme follows the device last used --
 * a bound key switches to keyboard+mouse, a pad button or stick back to the pad. With keyboard+mouse on
 * (and the menu closed, the window focused, Alt not held) the mouse is captured: its motion is the look,
 * the left button R (fire), the right L (aim); holding Alt frees the cursor, which is then the touch panel
 * as before (left button front, right rear). The keys are a PC layout by default (WASD the left stick,
 * Space Cross, ...), every one rebindable on the menu's Controls page.
 * The look: a game with a raw-look hook (hooks.c, e.g. uc_mouse_look) takes the motion each game frame
 * through window_mouse_delta and turns it into its camera directly, no stick curve in between; with no
 * such hook taking it (none for the game, or not in play) the motion is a right-stick stand-in instead.
 * The window thread writes, game threads read: the motion is two fixed-point accumulators, exchanged. */
static int kbm_auto;                       /* auto mode's current choice: 1 keyboard+mouse, 0 pad */
static char kbm_why[48];                   /* what made that choice last (logged as the scheme changes) */
static volatile int kbm_on, rel_on;
static uint64_t rel_since_us;              /* when the capture last began (rel_on) */        /* keyboard+mouse in use; the mouse captured (looking) */
static int mouse_dx_q8, mouse_dy_q8;       /* motion since a raw-look hook last took it, x256 (atomics) */
static int look_dx_q8, look_dy_q8;         /* the same motion, for the right-stick stand-in (this thread) */
static volatile uint64_t raw_taken_us;     /* when a raw-look hook last took the motion */
static int kbm_lmb, kbm_rmb;               /* captured mouse buttons held (R, L) */
/* Tilt (motion.c's virtual pad): Uncharted's balance beams and river slide are steered by tilting the Vita, and
 * keyboard and mouse have no gyro. In a tilt section (window_tilt_tick: the section's per-frame update, hooked in
 * hooks.c) movement is restricted, so the move-left/right keys (A / D) tilt -- easing to a full tilt while held,
 * back to level on release -- and a pad without a gyro tilts with its left stick; that stick's left/right is held
 * back from the game there (window_input). The mouse stays the look. A gyro pad in hand tilts itself. */
static volatile uint64_t tilt_tick_ns;     /* the game's last tilt-section tick (window_tilt_tick, any thread; SDL_GetTicksNS) */
static int tilt_section;                   /* in a tilt section: a tick within the last 0.25 s (this thread, each frame) */
static volatile int tilt_kind;             /* what the section is (window_tilt_tick): 1 a balance beam, 2 a slide -- the toast's wording */
static float tilt;                         /* radians, positive = the right side down */
#define TILT_MAX 0.70f                     /* ~40 degrees either way */
/* the game is in a tilt section this frame (a hook on the section's per-frame update, hooks.c): the section lasts
 * while these keep coming and lapses 0.25 s after the last (a paused game stops them too) */
void window_tilt_tick(int kind) { tilt_kind = kind; __atomic_store_n(&tilt_tick_ns, SDL_GetTicksNS(), __ATOMIC_RELAXED); }
enum { KB_MOVE_UP, KB_MOVE_DOWN, KB_MOVE_LEFT, KB_MOVE_RIGHT };   /* the first four bindings: the left stick */
typedef struct { const char *key, *label, *def; uint32_t bit; SDL_Keycode kc; int held; } KbBind;
static KbBind kb_binds[] = {
	{"bind_move_up", "Move forward (left stick)", "W", 0}, {"bind_move_down", "Move back", "S", 0},
	{"bind_move_left", "Move left", "A", 0}, {"bind_move_right", "Move right", "D", 0},
	{"bind_cross", "Cross", "Space", CROSS}, {"bind_circle", "Circle", "C", CIRCLE},
	{"bind_square", "Square", "F", SQUARE}, {"bind_triangle", "Triangle", "E", TRIANGLE},
	{"bind_l", "L (also the right mouse button)", "Q", LTRIG}, {"bind_r", "R (also the left mouse button)", "R", RTRIG},
	{"bind_start", "Start", "Return", START}, {"bind_select", "Select", "Tab", SELECT},
	{"bind_up", "D-pad up", "Up", UP}, {"bind_down", "D-pad down", "Down", DOWN},
	{"bind_left", "D-pad left", "Left", LEFT}, {"bind_right", "D-pad right", "Right", RIGHT},
};
enum { KB_N = (int)(sizeof kb_binds / sizeof kb_binds[0]) };
static void tilt_section_update(void) {
	uint64_t t = __atomic_load_n(&tilt_tick_ns, __ATOMIC_RELAXED);
	int on = t && SDL_GetTicksNS() - t < 250000000ull;
	if (on && !tilt_section) {   /* what tilts, as a badge (overlay.cpp) */
		const char *title = tilt_kind == 2 ? "Slide" : "Balance", *verb = tilt_kind == 2 ? "steer" : "tilt";
		char msg[160];
		if (kbm_on) snprintf(msg, sizeof msg, "[%s] [%s] %s left / right", SDL_GetKeyName(kb_binds[KB_MOVE_LEFT].kc), SDL_GetKeyName(kb_binds[KB_MOVE_RIGHT].kc), verb);
		else if (!pad_gyro) snprintf(msg, sizeof msg, "Left stick: %s left / right", verb);
		else snprintf(msg, sizeof msg, "Tilt the controller to %s", verb);
		overlay_toast_titled(title, msg, 5.0);
	}
	tilt_section = on;
}
static void kb_reload(void) {   /* the bindings from settings (at start, and when the menu closes: a rebind may have changed them) */
	for (int i = 0; i < KB_N; i++) {
		char b[48]; const char *name = settings_str(kb_binds[i].key, kb_binds[i].def, b, sizeof b);
		SDL_Keycode k = SDL_GetKeyFromName(name);
		kb_binds[i].kc = k != SDLK_UNKNOWN ? k : SDL_GetKeyFromName(kb_binds[i].def);
	}
	char sb[16]; shoulders_swapped = !strcmp(settings_str("shoulder_buttons", "triggers", sb, sizeof sb), "bumpers");
}
int window_kb_count(void) { return KB_N; }
const char *window_kb_label(int i) { return i >= 0 && i < KB_N ? kb_binds[i].label : ""; }
const char *window_kb_key_name(int i) { return i >= 0 && i < KB_N ? SDL_GetKeyName(kb_binds[i].kc) : ""; }
void window_kb_bind(int i, SDL_Keycode k) {   /* the menu's Rebind: that action now on that key (a key another action had is taken from it) */
	if (i < 0 || i >= KB_N || k == SDLK_UNKNOWN) return;
	for (int j = 0; j < KB_N; j++) if (j != i && kb_binds[j].kc == k) { kb_binds[j].kc = SDLK_UNKNOWN; settings_set_str(kb_binds[j].key, ""); }
	kb_binds[i].kc = k; settings_set_str(kb_binds[i].key, SDL_GetKeyName(k)); settings_save();
}
void window_kb_reset(void) { for (int i = 0; i < KB_N; i++) settings_set_str(kb_binds[i].key, kb_binds[i].def); settings_save(); kb_reload(); }
int window_kbm_active(void) { return kbm_on; }
/* a game's raw-look hook, once per game frame: the mouse's motion since its last call, in counts (y down),
 * and whether the mouse is looking at all (0: nothing to apply, the deltas are 0) */
/* RECOMP_INPUT_MOUSE="from-to:dx,dy;...": mouse counts per call over presented-frame ranges -- a headless
 * test's mouse (the raw-look hooks), as RECOMP_INPUT_STICKS is its sticks (display.c) */
extern uint64_t display_frames(void);
static int mouse_script(float *ix, float *iy) {
	static const char *s = (const char *)-1; if (s == (const char *)-1) s = getenv("RECOMP_INPUT_MOUSE");
	int hit = 0; uint64_t f = display_frames();
	for (const char *q = s; q && *q; ) {
		unsigned long a = strtoul(q, (char **)&q, 10), z = a; float vx = 0, vy = 0;
		if (*q == '-') z = strtoul(q + 1, (char **)&q, 10);
		if (*q == ':') { vx = strtof(q + 1, (char **)&q); if (*q == ',') vy = strtof(q + 1, (char **)&q); }
		if (f >= a && f <= z) { *ix += vx; *iy += vy; hit = 1; }
		if (*q == ';') q++; else break;
	}
	return hit;
}
int window_mouse_delta(float *dx, float *dy) {
	int x = __atomic_exchange_n(&mouse_dx_q8, 0, __ATOMIC_RELAXED), y = __atomic_exchange_n(&mouse_dy_q8, 0, __ATOMIC_RELAXED);
	raw_taken_us = now_us();
	float ix = 0, iy = 0; int scripted = mouse_script(&ix, &iy);
	if (!rel_on && !scripted) { *dx = *dy = 0; return 0; }
	*dx = (rel_on ? x / 256.0f : 0) + ix; *dy = (rel_on ? y / 256.0f : 0) + iy; return 1;
}
float window_mouse_sens(void) { return (float)settings_float("mouse_sens", 1.0); }
int window_mouse_invert(void) { return settings_int("mouse_invert", 0) != 0; }
/* direct mouse aim (raw look: a game's hook writing mouse counts into its camera, hooks.c), the menu's Mouse aim and F8
 * live (setting raw_look): on by default in a played run -- a game with no such hook, or a hook not in play, leaves the
 * motion to the stick stand-in above by itself, so it never takes the mouse look away. Off: always the stand-in, which
 * the stick's own curve and speed make stepped and late (a fresh install's Killzone, 2026-10-22). A scripted run
 * (RECOMP_HEADLESS) keeps the stand-in, as its recorded input was made with */
static int raw_look = -1;
int window_raw_look(void) {
	if (raw_look < 0) raw_look = getenv("RECOMP_RAW_LOOK") ? atoi(getenv("RECOMP_RAW_LOOK")) != 0 : settings_int("raw_look", getenv("RECOMP_HEADLESS") ? 0 : 1) != 0;   /* (RECOMP_RAW_LOOK: a test's) */
	return raw_look;
}
void window_set_raw_look(int on) { raw_look = on != 0; settings_set_int("raw_look", raw_look); settings_save(); }
static void raw_look_toggle(void) {
	window_set_raw_look(!window_raw_look());
	overlay_toast_titled("Mouse aim", raw_look ? "Direct: the game turns as far as the mouse moves [F8]" : "Stick: the mouse drives the right stick [F8]", 3.0);
}
static int kb_find(SDL_Keycode k) { for (int i = 0; i < KB_N; i++) if (kb_binds[i].kc == k && k != SDLK_UNKNOWN) return i; return -1; }
/* ---- PC keyboard map (input.ini): keys -> Vita pad buttons, in a P5D-friendly layout -- WASD for the
 * D-pad (the dance notes' directions), IJKL for the face buttons (I triangle, J square, K cross,
 * L circle), Space/Return/ Escape/Tab for confirm/start/cancel/select, Q/E for L/R. A config file
 * beside the game overrides any of them, several key names per button, and F11 reloads it. This is a
 * host input translation only: the game still sees the same SceCtrl pad it sees on a Vita. */
typedef struct { const char *name; uint32_t bit; SDL_Keycode kc; int held; } P5dKey;
enum { P5D_MENU = 0, P5D_GAME = 1, P5D_CONFIG = 2, P5D_COMMU = 3, P5D_SONG = 4, P5D_PROFILES = 5 };   /* CONFIG: its own map, so the settings screen never inherits the dance or the submenu shoulder keys; COMMU: Q/E are its L/R shoulders (character switching), so the song list keeps Q/E = the D-pad */
static P5dKey p5d_keys[P5D_PROFILES][96]; static int n_p5d_keys[P5D_PROFILES];
static int p5d_key_bound(int pr, SDL_Keycode k) {   /* M1627: is this keycode in that profile's map? */
	for (int i = 0; i < n_p5d_keys[pr]; i++) if (p5d_keys[pr][i].kc == k) return 1;
	return 0;
}
static int p5d_profile = P5D_MENU;   /* input.ini's [buttons] play in menus/story; [buttons_game] in the dance stage */
static uint32_t p5d_bits, p5d_mouse_bits;
/* M1522: PC-only input -- a PS pad (and its scheme) is disabled from launch; RECOMP_ALLOW_PAD=1 re-enables it for tests. */
/* M1524: the COLLECTION character-flow depth. The collection pages carry their own job (collection main /
 * collection modeler / collection character viewer); inside that flow each confirm we send steps one page deeper
 * (tab page -> character list -> the costume screen) and each back one shallower. The costume screen is depth 3,
 * where Enter = START (watch mode) and Ctrl = TRIANGLE (random outfit); leaving the flow (job gone) clears it. */
static uint64_t p5d_collection_job_us;   /* (real definition with the other menu state below) */
static int p5d_collection_depth;
static int p5d_collection_watching;   /* inside the costume screen's watch mode (we sent START) */
static int p5d_collection_active(void) { return p5d_collection_job_us && now_us() - p5d_collection_job_us < 1200000; }
/* every back we send inside the collection flow walks one level up; a back while the watch mode is up only leaves
 * the watch mode (the game's own START opened it, Circle/back closes it) so the depth must not change there */
static void p5d_collection_note_back(void) {
	if (!p5d_collection_active()) return;
	if (p5d_collection_watching) p5d_collection_watching = 0;
	else if (p5d_collection_depth > 1) p5d_collection_depth--;
}
int window_in_collection_flow(void) { return p5d_collection_active(); }
int window_in_collection_costume(void) { return p5d_collection_active() && p5d_collection_depth >= 3; }
/* M1526: the watch mode's sticks - WASD drive the left stick (move), the arrow keys the right stick (rotate);
 * the mouse wheel zooms (the game's own d-pad left/right) and Alt is the SELECT reset. Held flags, cleared on exit. */
static int p5d_watch_keys[8];   /* W A S D, Up Down Left Right */
static void p5d_watch_keys_clear(void) { for (int i = 0; i < 8; i++) p5d_watch_keys[i] = 0; }
int window_in_collection_watching(void) { return p5d_collection_active() && p5d_collection_watching; }
static int p5d_watch_enter_held;   /* (the viewer Enter's latch lives across calls: one press = one toggle) */
int p5d_watch_enter_up_clear(void) { p5d_watch_enter_held = 0; return 0; }
static int p5d_pad_allowed(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_ALLOW_PAD"); v = e && atoi(e) != 0; } return v; }
static uint32_t p5d_mouse_left_btn = CIRCLE, p5d_mouse_right_btn = CROSS;   /* what the mouse buttons press (the [mouse] table) */
static uint32_t p5d_mouse_left_bit;   /* the bit the left button pressed: released by name, not by the screen's current formula
                                         (the screen changed between the press and the release and a stray Cross stayed held forever --
                                         it made the game's edge-triggered back/confirm ignore every later press, 2026-11-08) */
static uint64_t p5d_mouse_cross_until;   /* a right-click's back: held a few frames, so the game's one-frame menu poll cannot miss it */
static int p5d_mouse_on = 1;
static const struct { const char *name; uint32_t bit; } p5d_button_names[] = {
	{"up", UP}, {"down", DOWN}, {"left", LEFT}, {"right", RIGHT},
	{"triangle", TRIANGLE}, {"circle", CIRCLE}, {"cross", CROSS}, {"square", SQUARE},
	{"start", START}, {"select", SELECT}, {"l", LTRIG}, {"r", RTRIG}, {"l2", PAD_L2}, {"r2", PAD_R2},
};
static uint32_t p5d_bit_of(const char *n) {
	for (size_t i = 0; i < sizeof p5d_button_names / sizeof p5d_button_names[0]; i++) if (!strcmp(p5d_button_names[i].name, n)) return p5d_button_names[i].bit;
	return 0;
}
static void p5d_recompute(void) {
	p5d_bits = 0;
	int pr = p5d_profile;
	for (int i = 0; i < n_p5d_keys[pr]; i++) if (p5d_keys[pr][i].held) p5d_bits |= p5d_keys[pr][i].bit;
}
static void p5d_add(int pr, const char *name, const char *keyname) {
	uint32_t bit = p5d_bit_of(name); if (!bit) return;
	SDL_Keycode kc = SDL_GetKeyFromName(keyname);
	if (kc == SDLK_UNKNOWN) { fprintf(stderr, "input.ini: unknown key name '%s' for %s\n", keyname, name); return; }
	if (n_p5d_keys[pr] < (int)(sizeof p5d_keys[pr] / sizeof p5d_keys[pr][0])) { p5d_keys[pr][n_p5d_keys[pr]].name = name; p5d_keys[pr][n_p5d_keys[pr]].bit = bit; p5d_keys[pr][n_p5d_keys[pr]].kc = kc; n_p5d_keys[pr]++; }
}
static void p5d_defaults(void) {
	/* menus and story: arrows + Space/Enter/Esc (a PC game's usual keys), with WASD as a second D-pad */
	for (int pr = 0; pr < P5D_PROFILES; pr++) n_p5d_keys[pr] = 0;
	p5d_add(P5D_MENU, "up", "Up"); p5d_add(P5D_MENU, "down", "Down"); p5d_add(P5D_MENU, "left", "Left"); p5d_add(P5D_MENU, "right", "Right");
	p5d_add(P5D_MENU, "circle", "Space"); p5d_add(P5D_MENU, "circle", "Return"); p5d_add(P5D_MENU, "cross", "Backspace");
	p5d_add(P5D_MENU, "square", "J"); p5d_add(P5D_MENU, "square", "Tab");   /* the song list's sort hint now names Tab */
	p5d_add(P5D_MENU, "start", "Escape");   /* the pause menu is START everywhere: in the story scenes too (reported 2026-11-07); Return is the confirm (Circle) above, not a second Start */
	p5d_add(P5D_MENU, "select", "Tab");   /* (Backspace is Cross below: one key, one Vita button) */
	p5d_add(P5D_MENU, "l", "Q"); p5d_add(P5D_MENU, "r", "E");   /* the COMMU pages' L R switch-character (their hints name Q/E) */
	p5d_add(P5D_MENU, "l", "["); p5d_add(P5D_MENU, "r", "]");   /* a second pair */
	/* the dance stage, per the user's layout: Q/A/Z = the up/left/down directions (D stays as right),
	 * O/K/M = triangle/circle/cross (J = square); Escape = the pause menu (START), Enter also starts;
	 * U/I = the L/R triggers (Q is a direction now) */
	p5d_add(P5D_GAME, "up", "Q"); p5d_add(P5D_GAME, "down", "Z"); p5d_add(P5D_GAME, "left", "A"); p5d_add(P5D_GAME, "right", "D");
	p5d_add(P5D_GAME, "up", "W"); p5d_add(P5D_GAME, "down", "S");
	p5d_add(P5D_GAME, "triangle", "O"); p5d_add(P5D_GAME, "square", "J"); p5d_add(P5D_GAME, "square", "Tab"); p5d_add(P5D_GAME, "circle", "K"); p5d_add(P5D_GAME, "cross", "M");
	p5d_add(P5D_GAME, "start", "Escape"); p5d_add(P5D_GAME, "start", "Return");
	p5d_add(P5D_GAME, "select", "Backspace");
	p5d_add(P5D_GAME, "l", "U"); p5d_add(P5D_GAME, "r", "I");
	/* the song list: Q/E are its D-pad left/right (the difficulty tabs); the COMMU pages keep Q/E = L/R */
	p5d_add(P5D_SONG, "up", "Up"); p5d_add(P5D_SONG, "down", "Down");
	p5d_add(P5D_SONG, "left", "Left"); p5d_add(P5D_SONG, "left", "Q");
	p5d_add(P5D_SONG, "right", "Right"); p5d_add(P5D_SONG, "right", "E");
	p5d_add(P5D_SONG, "triangle", "I"); p5d_add(P5D_SONG, "square", "J"); p5d_add(P5D_SONG, "square", "Tab");
	p5d_add(P5D_SONG, "circle", "Space"); p5d_add(P5D_SONG, "circle", "Return");
	p5d_add(P5D_SONG, "cross", "Backspace"); p5d_add(P5D_SONG, "cross", "Escape");
	p5d_add(P5D_SONG, "start", "Escape");
	p5d_add(P5D_SONG, "select", "Tab");
	p5d_add(P5D_SONG, "l", "["); p5d_add(P5D_SONG, "r", "]");
	/* the COMMU pages: Q/E are the L/R shoulders there ("L R 切換角色"), so this map has no Q/E on the
	 * D-pad; the song list keeps the menu map's Q/E = D-pad left/right and the dance stage keeps its own Q */
	p5d_add(P5D_COMMU, "up", "Up"); p5d_add(P5D_COMMU, "down", "Down");
	p5d_add(P5D_COMMU, "left", "Left"); p5d_add(P5D_COMMU, "right", "Right");
	p5d_add(P5D_COMMU, "triangle", "I"); p5d_add(P5D_COMMU, "square", "J"); p5d_add(P5D_COMMU, "square", "Tab");
	p5d_add(P5D_COMMU, "circle", "Space"); p5d_add(P5D_COMMU, "circle", "Return");
	p5d_add(P5D_COMMU, "cross", "Backspace");
	p5d_add(P5D_COMMU, "start", "Escape");
	p5d_add(P5D_COMMU, "select", "Tab");
	p5d_add(P5D_COMMU, "l", "Q"); p5d_add(P5D_COMMU, "r", "E");
	/* the CONFIG (settings) screen has its own map: the list is the game's D-pad list and its hints
	 * ("[Q][E] 變更", the "變更" badge is the left/right d-pad) name Q/E for changing a value, so Q/E
	 * drive D-pad left/right HERE -- not the submenu shoulders and not the dance notes. */
	p5d_add(P5D_CONFIG, "up", "Up"); p5d_add(P5D_CONFIG, "down", "Down");
	p5d_add(P5D_CONFIG, "left", "Left"); p5d_add(P5D_CONFIG, "left", "Q");
	p5d_add(P5D_CONFIG, "right", "Right"); p5d_add(P5D_CONFIG, "right", "E");
	p5d_add(P5D_CONFIG, "circle", "Space"); p5d_add(P5D_CONFIG, "circle", "Return");
	p5d_add(P5D_CONFIG, "cross", "Backspace"); p5d_add(P5D_CONFIG, "cross", "Escape");   /* Esc also closes the pause menu's settings page (the pause menu is this page) */
	p5d_add(P5D_CONFIG, "square", "J"); p5d_add(P5D_CONFIG, "square", "Tab");
	p5d_add(P5D_CONFIG, "start", "Left Alt"); p5d_add(P5D_CONFIG, "select", "Backspace");   /* the CONFIG hint names ALT for START (reset to defaults); Return stays CIRCLE (confirm) */
}
static const char *p5d_default_ini =
	"# Persona 5 Dancing in Starlight - PC keyboard / mouse map. Edit, then press F11 in the game to reload.\n"
	"# layout 5: Q/E are the menus' D-pad left/right; shoulders [ ]; Esc = START (pause); Esc also backs out of the settings page.\n"
	"# Two profiles: [buttons] for menus and story, [buttons_game] for the dance stage. F9 swaps them by hand\n"
	"# (scene detection is automatic once it is in; F9 is the override).\n"
	"# Key names are SDL's: A-Z, 0-9, Space, Escape, Return, Tab, Left, Right, Up, Down, Left Shift...\n"
	"# Several keys may share a button (comma separated).\n\n"
	"[buttons]\n"
	"up       = Up\n"
	"down     = Down\n"
	"left     = Left, Q\n"
	"right    = Right, E\n"
	"triangle = I\n"
	"square   = J, Tab\n"
	"circle   = Space, Return\n"
	"cross    = Backspace\n"
	"start    = Return, Escape\n"
	"select   = Backspace\n"
	"l        = [\n"
	"r        = ]\n\n"
	"[buttons_game]\n"
	"# the dance stage (user layout): Q/A/Z = up/left/down, D = right;\n"
	"# O/K/M = triangle/circle/cross, J = square; Escape = the pause menu (START)\n"
	"up       = Q, W\n"
	"down     = Z, S\n"
	"left     = A\n"
	"right    = D\n"
	"triangle = O\n"
	"square   = J, Tab\n"
	"circle   = K\n"
	"cross    = M\n"
	"start    = Escape, Return\n"
	"select   = Backspace\n"
	"l        = U\n"
	"r        = I\n\n"
	"[buttons_config]\n"
	"# the CONFIG (settings) screen: the list is a D-pad list; Q/E change a value = D-pad left/right\n"
	"up       = Up\n"
	"down     = Down\n"
	"left     = Left, Q\n"
	"right    = Right, E\n"
	"circle   = Space, Return\n"
	"cross    = Backspace, Escape\n"
	"square   = J, Tab\n"
	"start    = Left Alt\n"
	"select   = Backspace\n\n"
	"[mouse]\n"
	"# menu/story with the mouse: P5D uses the Japanese layout (Circle = confirm/select, Cross = back)\n"
	"left  = circle\n"
	"right = cross\n";
static void p5d_load(void) {
	const char *env = getenv("RECOMP_INPUT_CONFIG");
	const char *path = env && *env ? env : "input.ini";
	FILE *f = fopen(path, "r");
	if (!f) {   /* first run: leave a default file to edit */
		f = fopen(path, "w"); if (f) { fputs(p5d_default_ini, f); fclose(f); } 
		f = fopen(path, "r");
	}
	p5d_defaults();
	if (f) {   /* an input.ini from before the two profiles / the P5D Circle-confirm layout: replace it */
		char whole[4096]; size_t n = fread(whole, 1, sizeof whole - 1, f); whole[n] = 0; fclose(f); f = 0;
		if (!strstr(whole, "[buttons_game]") || !strstr(whole, "layout 5:")) {
			f = fopen(path, "w"); if (f) { fputs(p5d_default_ini, f); fclose(f); f = fopen(path, "r");
				fprintf(stderr, "input: %s upgraded to the two-profile P5D layout (Circle = confirm)\n", path); }
		} else { f = fopen(path, "r"); if (!f) fprintf(stderr, "input: cannot reopen %s\n", path); }
	}
	if (!f) { p5d_recompute(); fprintf(stderr, "input: built-in keyboard map, no %s written\n", path); return; }
	char line[512]; int section = P5D_MENU, n[P5D_PROFILES] = {0, 0, 0, 0, 0}; p5d_mouse_on = 1; p5d_mouse_bits = 0;
	while (fgets(line, sizeof line, f)) {
		char *s = line; while (*s == ' ' || *s == '\t') s++;
		if (*s == '#' || *s == ';' || !*s || *s == '\r' || *s == '\n') continue;
		if (*s == '[') { section = !strncmp(s, "[mouse]", 7) ? -1 : !strncmp(s, "[buttons_game]", 14) ? P5D_GAME : !strncmp(s, "[buttons_config]", 16) ? P5D_CONFIG : P5D_MENU; continue; }
		char *eq = strchr(s, '='); if (!eq) continue;
		*eq = 0; char *name = s, *val = eq + 1;
		char *e = name + strlen(name); while (e > name && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
		while (*val == ' ' || *val == '\t') val++;
		e = val + strlen(val); while (e > val && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
		if (section >= 0) {
			if (!n[section]) n_p5d_keys[section] = 0;   /* the file is authoritative for a section it has */
			n[section] = 1;
			for (char *q = val; q && *q; ) { char *c = strchr(q, ','); if (c) *c = 0; char *a = q; while (*a == ' ') a++; size_t al = strlen(a); while (al && a[al-1] == ' ') a[--al] = 0; if (*a) p5d_add(section, name, a); q = c ? c + 1 : 0; }
		} else {
			uint32_t bit = p5d_bit_of(val);
			if (!strcmp(name, "left")) p5d_mouse_left_btn = bit ? bit : CIRCLE;
			else if (!strcmp(name, "right")) p5d_mouse_right_btn = bit ? bit : CROSS;
			else if (!strcmp(name, "on")) p5d_mouse_on = atoi(val) != 0;
		}
	}
	fclose(f);
	if (!n_p5d_keys[P5D_MENU] && !n_p5d_keys[P5D_GAME]) p5d_defaults();
	p5d_recompute();
	fprintf(stderr, "input: %d menu + %d game + %d config bindings from %s; mouse menu clicks %s; F9 switches profile\n",
	        n_p5d_keys[P5D_MENU], n_p5d_keys[P5D_GAME], n_p5d_keys[P5D_CONFIG], path, p5d_mouse_on ? "on (left = confirm, right = cancel)" : "off");
}
static void p5d_switch_profile(int pr) {
	if (pr >= P5D_PROFILES) pr = P5D_MENU;
	int old_pr = p5d_profile;
	p5d_profile = pr;
	for (int k = 0; k < P5D_PROFILES; k++) for (int i = 0; i < n_p5d_keys[k]; i++) p5d_keys[k][i].held = 0;
	p5d_recompute();
	fprintf(stderr, "input: profile %s\n", p5d_profile == P5D_GAME ? "game (dance)" : p5d_profile == P5D_CONFIG ? "config (settings)" : p5d_profile == P5D_COMMU ? "commu (L/R = Q/E)" : p5d_profile == P5D_SONG ? "song list (Q/E = D-pad)" : "menu/story");
	/* the glyph atlases only differ between the dance map and the menus, and the settings page's ALT keycap:
	 * re-uploading them on every switch (they are force-uploads) could stutter, so only when those gates change */
	if (old_pr == P5D_GAME || pr == P5D_GAME || old_pr == P5D_CONFIG || pr == P5D_CONFIG) {
		{ extern void gfx_keyglyph_refresh(void); gfx_keyglyph_refresh(); }
		{ extern void gfx_hintglyph_refresh(void); gfx_hintglyph_refresh(); }
	}
}

/* ---- PC mouse on the game's menus: P5D's main menu is a fixed vertical list at 960x544 (DANCING!,
 * TUTORIAL, COMMU, COLLECTION, CONFIG, SYSTEM in the left third). A left click on another row is turned
 * into the D-pad presses that move the highlight there plus Cross -- a host-side translation of the
 * pointer into the pad data the game reads (its logic and binary untouched). The list's highlight is a
 * red bar; the row under the pointer and the highlighted row are both read from the guest frame buffer
 * the display just set (display_last_frame). */
extern uint32_t display_last_frame(uint32_t *w, uint32_t *h, uint32_t *pitch);
/* ---- the PC interaction layer's screen model: which ready-made UI (the main menu's left list, the
 * song select list) the frame shows, its row centres and which row the game highlights. Read from the
 * guest frame buffer; the pointer's hover/click and the keyboard's arrows drive the game through the
 * closed loop below -- the game still only sees a D-pad and Circle. */
#include "ui_templates.h"
enum { P5D_UI_NONE, P5D_UI_MAIN, P5D_UI_SONG, P5D_UI_TITLE, P5D_UI_RESULT };
static int p5d_ui = P5D_UI_NONE, p5d_ui_cur = -1, p5d_ui_n;
static float p5d_ui_score; static const char *p5d_ui_tname = "?";   /* the best template this model update (the [UI] log) */
static int p5d_ui_y[8];
static int p5d_menu_sel = -1;
static int64_t p5d_menu_last = -1;
static int p5d_saw_menu, p5d_saw_title;
static uint64_t menu_win_until;   /* submenu/pause interaction window: while now < this, wheel/click act as
                                   * menu input even though the screen model says NONE (the pause menu and the
                                   * submenus are not recognised screens) */
static uint64_t game_started_us;  /* when a song was confirmed: the dance stage uses the [buttons_game] map */
static uint64_t song_confirm_us;   /* when the song list's confirm fired: the next screens are the dance, not a submenu */
static uint64_t p5d_dance_bar_us;  /* last frame the dance stage's song gauge was on screen: the only reliable "we are in the dance stage now" signal */
static uint64_t p5d_story_us;   /* last frame a story/dialogue scene was on screen: their Esc is the pause menu, like the dance's */
static uint64_t p5d_commu_us;
static uint64_t p5d_commu_screen_us;   /* last frame the game's own COMMU layout masks matched (gfx_commu_screen) */
static int p5d_commu_latch;       /* the COMMU pages keep their map while they are up (their logo flickers in transitions) */
static uint64_t p5d_commu_absent_us;
static uint64_t p5d_commu_latch_us;
static int p5d_commu_mode;   /* the COMMU pages are open (from the main-menu row): Q/E are L/R there */
static int p5d_song_mode;    /* the DANCING/song-list flow is open: Q/E are its D-pad */

/* The one scene state. Every Q/E press maps from this enum only (see the per-press table in the event loop):
 * entering a screen sets it (optimistically, from the input intent), leaving a screen sets it back -- no
 * per-screen key flag survives, which is what the user asked for. The picture model only corrects it. */
enum { P5D_SCENE_BOOT = 0, P5D_SCENE_TITLE, P5D_SCENE_MAIN, P5D_SCENE_COMMU, P5D_SCENE_SONG,
       P5D_SCENE_DANCE, P5D_SCENE_PAUSE, P5D_SCENE_RESULT };
static int p5d_scene = P5D_SCENE_BOOT, p5d_scene_prev = P5D_SCENE_MAIN;

/* M1540: the 60 fps presentation is a dance-stage feature: everywhere else the game's own 30 fps cadence is
 * kept, because its title/logo/menu sequences advance by frame count (at 60 they ran 1.5-2x fast). This accessor
 * is the code-level screen state (not a picture guess), so display.c's swap interval follows it. */
int window_p5d_dance_screen(void) { return p5d_scene == P5D_SCENE_DANCE; }
static int p5d_scene_lock;   /* 1 = SONG, 2 = COMMU: set by the input intent, the picture model must not override it */
/* Q/E mode: 0 auto (the flow/latch rules), 1 force L/R shoulders, 2 force D-pad. F3 cycles it and it is persisted in
 * the settings as "qe_mode" - the guaranteed fallback when the automatic screen detection is wrong (reported
 * 2026-11-08). The choice is announced with an overlay toast and logged. */
static int p5d_qe_mode = -1;
static const char *p5d_qe_mode_name(int m) { return m == 1 ? "shoulders (L/R)" : m == 2 ? "D-pad (left/right)" : "auto"; }
static void p5d_qe_mode_load(void) {
	if (p5d_qe_mode >= 0) return;
	char b[24]; settings_str("qe_mode", "auto", b, sizeof b);
	p5d_qe_mode = !strcmp(b, "shoulders") ? 1 : !strcmp(b, "dpad") ? 2 : 0;
	fprintf(stderr, "input: Q/E mode %s\n", p5d_qe_mode_name(p5d_qe_mode));
}
static void p5d_qe_mode_cycle(void) {
	p5d_qe_mode_load();
	p5d_qe_mode = (p5d_qe_mode + 1) % 3;
	settings_set_str("qe_mode", p5d_qe_mode == 1 ? "shoulders" : p5d_qe_mode == 2 ? "dpad" : "auto"); settings_save();
	fprintf(stderr, "[KEY] Q/E mode -> %s\n", p5d_qe_mode_name(p5d_qe_mode));
	{ extern void overlay_toast(const char *msg, double seconds);
	  char m[64]; snprintf(m, sizeof m, "Q/E: %s  (F3 cycles)", p5d_qe_mode_name(p5d_qe_mode)); overlay_toast(m, 2.5); }
}   /* 1 = SONG, 2 = COMMU: set by the input intent, the picture model must not override it */
static uint64_t p5d_scene_set_us;
/* The main-menu module's own page state (static: eboot 0x810FD49E dispatches on obj[+4], 35 cases; the object is
 * *(0x812B414C) -> getter -> *(ptr+8)). Diagnostic [MENUST]; once mapped it joins the scene machine. */
/* The main-menu highlight row, straight from the game's own variable 0x813B017C (it moves by one per D-pad step and
 * by the attract mode's auto-advance). Used when the picture model could not read the row, so a confirm still knows
 * which feature was picked. */
/* the app job list's current entries (static: eboot 0x8116F7CA; head 0x815554C4, node+0xc = job, job+0x14 = name).
 * While a "MusicSelect*" job exists the song list IS on screen - a continuous code-level signal for its Q/E D-pad,
 * independent of the picture model's success at the current render scale. */
/* the mainMenuP5 app process's own page state: static eboot 0x8116F7CA registered it with entry 0x810FD49F and its
 * module object at node+8; the main menu body (0x810FD49E) dispatches 35 sub-screens on object[+4] (main root, the
 * DANCING row page, the COMMU character list, the COMMUNITY story list, CONFIG, ...). That is the game's own sub-screen
 * id - the code-level answer for the screens the sequence page state (all 5 = MAIN MENU) cannot tell apart. */
extern volatile int p5d_mp_state;
extern volatile unsigned p5d_mp_gen;
extern volatile uint32_t p5d_mp_obj;
extern volatile unsigned p5d_mp_calls;
static unsigned p5d_mp_seen_gen;
static int p5d_mp_cur = -1;   /* the main menu's own sub-page state, straight from the game (0x810FD49E's object[+4]) */
static int p5d_menu_page_state(void) {
	uint32_t node = RD32(0x815554C4u);
	for (int k = 0; k < 96 && node && node < 0x88000000u; k++) {
		uint32_t entry = RD32(node);
		if ((entry & ~1u) == 0x810fd49eu) {
			uint32_t obj = RD32(node + 8);
			if (obj && obj < 0x88000000u) { uint32_t st = RD32(obj + 4); if (st <= 0x3fu) return (int)st; }
			return -1;
		}
		node = RD32(node + 0x10);
	}
	return -1;
}
static int p5d_song_process_alive(void) {
	uint32_t node = RD32(0x815554C4u);
	for (int k = 0; k < 64 && node && node < 0x88000000u; k++) {
		uint32_t job = RD32(node + 0xc);
		if (job && job < 0x88000000u) {
			uint32_t np = RD32(job + 0x14);
			if (np && np < 0x88000000u) {
				char nm[32]; int i = 0;
				for (; i < 31; i++) { char c = (char)RD8(np + (uint32_t)i); if (!c) break; nm[i] = c; }
				nm[i] = 0;
				if (strstr(nm, "MusicSelect") || strstr(nm, "music_select")) return 1;
			}
		}
		node = RD32(node + 0x10);
	}
	return 0;
}
static int p5d_banner_y;   /* (defined with the other menu state below) */

/* the main menu's row from its red banner's y (the selection index itself is unreadable at 2K: cur=-1).
 * The rows sit ~48 picture pixels apart, DANCING at ~243. */
static int p5d_banner_row(void) {
	if (p5d_ui != P5D_UI_MAIN || p5d_banner_y < 200 || p5d_banner_y > 540) return -1;
	int r = (p5d_banner_y - 243 + 24) / 48;
	if (r < 0) r = 0; if (r > 5) r = 5;
	return r;
}

static int p5d_menu_row_hint(void) {
	uint32_t v = RD32(0x813B017Cu);
	return v <= 5 ? (int)v : -1;
}

/* The app process manager's current process (static: eboot 0x8116F7CA; the current node pointer lives at
 * 0x815554C4; a node is {name*, id, entry*, ..., obj* at +8}; the object's page state sits at +4 - the main menu
 * dispatches 0x810FD49E on it with 35 cases). Pure memory reads: the CURRENT PROCESS NAME plus its page state. */
static uint64_t p5d_commu_job_us, p5d_config_job_us, p5d_result_job_us, p5d_song_job_us, p5d_tutorial_job_us, p5d_dance_job_us, p5d_main_job_us, p5d_pause_job_us, p5d_story_job_us, p5d_title_job_us, p5d_collection_job_us;   /* (definitions with the other menu state below) */

/* M1522: timestamp every known screen job that is PRESENT in the live job list. This is the game's own code
 * state (the job list at 0x815554C4), no picture model: the screen service uses these to know which page runs. */
static void p5d_proc_mark(uint64_t t) {
	static char seen[64][48]; static int nseen;
	static char prev_names[64][48]; static int np;
	char now_names[64][48]; int nn = 0;
	int dump = getenv("RECOMP_JOBDUMP") != 0;
	uint32_t node = RD32(0x815554C4u);
	for (int k = 0; k < 96 && node && node < 0x88000000u; k++) {
		uint32_t job = RD32(node + 0xc);
		uint32_t np = (job && job < 0x88000000u) ? RD32(job + 0x14) : 0;
		if (np && np < 0x88000000u) {
			char nm[48]; int i = 0;
			for (; i < (int)sizeof nm - 1; i++) { char c = (char)RD8(np + (uint32_t)i); if (!c) break; nm[i] = (c >= 32 && c < 127) ? c : 0; }
			nm[i] = 0;
			if (dump) {   /* discovery: print every job name the live list carries, once */
				int hit = 0;
				for (int q = 0; q < nseen; q++) if (strcmp(seen[q], nm) == 0) { hit = 1; break; }
				if (!hit && nseen < 64) { snprintf(seen[nseen++], sizeof seen[0], "%s", nm); fprintf(stderr, "[JOBALL] %s\n", nm); }
			}
			{   /* discovery: dump the collection viewer/modeler job object on change (looking for the page field) */
				static uint32_t lastj[8]; static int nlast;
				if (getenv("RECOMP_JOBDUMP") && (strstr(nm, "collection character viewer") || strstr(nm, "collection modeler") || strstr(nm, "collection main"))) {
					uint32_t sig = job ^ np;
					int known = 0; for (int q = 0; q < nlast; q++) if (lastj[q] == sig) { known = 1; break; }
					if (!known && nlast < 8) { lastj[nlast++] = sig; }
					else known = 1;
					{ static uint32_t lastfields[8][40]; static int seen2;
					  for (int q = 0; q < nlast; q++) if (lastj[q] == sig && q < 8) {
						uint32_t f[40]; for (int k = 0; k < 40; k++) f[k] = RD32(job + (uint32_t)(k * 4));
						int diff = 0; for (int k = 0; k < 40; k++) if (f[k] != lastfields[q][k]) diff = 1;
						if (diff) { fprintf(stderr, "[JOBF] %s job=%#x np=%#x", nm, job, np); for (int k = 0; k < 40; k++) fprintf(stderr, " %08x", f[k]); fprintf(stderr, "\n"); for (int k = 0; k < 40; k++) lastfields[q][k] = f[k]; seen2++; }
					  }
					}
				}
			}
			if (dump) {   /* discovery: remember this scan's screen-job names for the set diff below */
				if (nn < 64 && (strstr(nm, "collection") || strstr(nm, "preview") || strstr(nm, "viewer") || strstr(nm, "modeler") ||
				                strstr(nm, "main menu") || strstr(nm, "wipe") || strstr(nm, "effect") || strstr(nm, "score") ||
				                strstr(nm, "evt") || strstr(nm, "pause") || strstr(nm, "MusicSelect") || strstr(nm, "Dance"))) {
					snprintf(now_names[nn++], 48, "%s", nm);
				}
			}
			/* M1629 diagnostic: dump the pause/config job objects when their state changes, to find the code-level
			 * pause-page state (the names exist from boot, so presence alone says nothing). RECOMP_PAUSEJOB=1 */
			if (getenv("RECOMP_PAUSEJOB") && (strstr(nm, "pause menu") || strstr(nm, "config menu") || strstr(nm, "ConfigStartEffect"))) {
				static struct { char nm[24]; uint32_t w[24]; } last[8]; static int nlast;
				uint32_t cur[24]; for (int q = 0; q < 24; q++) cur[q] = RD32(job + (uint32_t)(q * 4));
				int slot = -1, changed = 0;
				for (int q = 0; q < nlast; q++) if (strcmp(last[q].nm, nm) == 0) { slot = q; break; }
				if (slot < 0 && nlast < 8) { slot = nlast++; snprintf(last[slot].nm, sizeof last[slot].nm, "%s", nm); memcpy(last[slot].w, cur, sizeof cur); }
				else if (slot >= 0 && memcmp(last[slot].w, cur, sizeof cur) != 0) { changed = 1; memcpy(last[slot].w, cur, sizeof cur); }
				if (changed) {
					fprintf(stderr, "[PAUSEJOB] %s job=%#x f=", nm, job); extern uint64_t display_frames(void);
					fprintf(stderr, "%llu:", (unsigned long long)display_frames());
					for (int q = 0; q < 24; q++) fprintf(stderr, " %08x", cur[q]);
					fprintf(stderr, "\n");
				}
			}
			/* every screen's own job name (discovered with RECOMP_JOBDUMP=1): the code-level screen evidence */
			if (strstr(nm, "collection")) p5d_collection_job_us = t;
			else if (strstr(nm, "pause menu")) p5d_pause_job_us = t;
			else if (strstr(nm, "evtSceneManager") || strstr(nm, "evtSMProcess") || strstr(nm, "evtSpeakerManager")) p5d_story_job_us = t;
			else if (strstr(nm, "title sequence") || strstr(nm, "titleDraw") || strcmp(nm, "title") == 0 || strstr(nm, "title movie")) p5d_title_job_us = t;
			else if (strstr(nm, "commuMain")) p5d_commu_job_us = t;
			else if (strstr(nm, "MusicSelect")) p5d_song_job_us = t;
			else if (strstr(nm, "config menu") || strstr(nm, "ConfigStartEffect")) p5d_config_job_us = t;
			else if (strstr(nm, "TutorialMain")) p5d_tutorial_job_us = t;
			else if (strstr(nm, "esult")) p5d_result_job_us = t;
			else if (strstr(nm, "mainMenuP5") || strstr(nm, "mainmenu load") || strstr(nm, "init mainmenu")) p5d_main_job_us = t;
			else if (strstr(nm, "replay dance") || strstr(nm, "danceVoiceSub") || strstr(nm, "Dancig") || strstr(nm, "Dancing") ||
			         strstr(nm, "DanceProcess") || strstr(nm, "DanceMemberPose") || strcmp(nm, "mns") == 0) p5d_dance_job_us = t;
		}
		node = RD32(node + 0x10);
	}
	if (dump) {   /* set diff: what the screens added/removed since the last scan */
		for (int i = 0; i < nn; i++) { int hit = 0; for (int j = 0; j < np; j++) if (strcmp(now_names[i], prev_names[j]) == 0) { hit = 1; break; } if (!hit) fprintf(stderr, "[JOBSET+] %s\n", now_names[i]); }
		for (int j = 0; j < np; j++) { int hit = 0; for (int i = 0; i < nn; i++) if (strcmp(prev_names[j], now_names[i]) == 0) { hit = 1; break; } if (!hit) fprintf(stderr, "[JOBSET-] %s\n", prev_names[j]); }
		memcpy(prev_names, now_names, sizeof now_names); np = nn;
	}
}
static int p5d_proc_info(char *name, int n, int *page) {
	/* Static analysis of eboot 0x8116F900 / 0x8116F9DC / 0x8116F7CA: the app job list head is 0x815554C4 (nodes
	 * linked by +0x10); node[+0xc] = job object, job[+0x14] = the job's NAME string (FUN_8116F900 strcmp's it).
	 * The list carries every live job (background workers included), so the known UI job names are picked out. */
	static const char *ui_names[] = { "commuMain", "MusicSelectMain", "mainMenuP5", "ConfigStartEffect", "TutorialMain",
	                                  "result", "mns", "scrConsoleStart", "movie play" };
	uint32_t node = RD32(0x815554C4u);
	if (page) *page = -1;
	if (name && n) name[0] = 0;
	if (!node || node >= 0x88000000u) return 0;
	int best = -1, seen = 0;
	for (int k = 0; k < 96 && node && node < 0x88000000u; k++) {
		uint32_t job = RD32(node + 0xc);
		uint32_t np = (job && job < 0x88000000u) ? RD32(job + 0x14) : 0;
		if (np && np < 0x88000000u) {
			char nm[48]; int i = 0;
			for (; i < (int)sizeof nm - 1; i++) { char c = (char)RD8(np + (uint32_t)i); if (!c) break; nm[i] = (c >= 32 && c < 127) ? c : 0; }
			nm[i] = 0;
			seen++;
			for (int u = 0; u < (int)(sizeof ui_names / sizeof ui_names[0]); u++)
				if (strcmp(nm, ui_names[u]) == 0) { best = u; }
			if (name && n > 1) {   /* remember the last UI job name seen in the list */
				int hit = 0;
				for (int u = 0; u < (int)(sizeof ui_names / sizeof ui_names[0]); u++) if (strcmp(nm, ui_names[u]) == 0) hit = 1;
				if (hit) snprintf(name, n, "%s", nm);
			}
		}
		node = RD32(node + 0x10);
	}
	if (name && n > 1 && name[0] == 0) snprintf(name, n, "%d jobs", seen);
	if (page && best >= 0) {
		/* page of the matching node is filled by the caller via the last node walk: keep -1 here (the scene uses names) */
	}
	return 1;
}

extern volatile char p5d_job_name[48];
extern volatile unsigned p5d_job_name_gen;
static unsigned p5d_job_seen_gen;
static uint64_t p5d_job_name_us;
static const char *p5d_job_name_fresh(uint64_t t) {
	unsigned g = __atomic_load_n(&p5d_job_name_gen, __ATOMIC_ACQUIRE);
	if (g != p5d_job_seen_gen) { p5d_job_seen_gen = g; p5d_job_name_us = t; }
	if (!p5d_job_name_us || t - p5d_job_name_us > 3000000) return 0;
	return (const char *)p5d_job_name;
}

/* one-line debug state for the on-screen HUD (metrics.c's OSD, F1 twice / P5D_DEBUG_HUD=1): what the port
 * believes the scene is, the game's own page state, the lock and the last Q/E decision. This makes a screenshot
 * evidence of what the mapping did, instead of guessing from logs. */
static const char *p5d_scene_name(int sc);
static int p5d_app_state_code(void);
static char p5d_qe_last[24] = "-";
int p5d_menu_page_cur(void) { return p5d_mp_cur; }
int p5d_debug_line(char *b, int n) {
	int st = p5d_app_state_code();
	extern int p5d_menu_page_cur(void);
	p5d_qe_mode_load();
	return snprintf(b, n, "SC:%.8s st:%d mp:%d qe:%.8s/%s", p5d_scene_name(p5d_scene), st, p5d_menu_page_cur(), p5d_qe_last, p5d_qe_mode_name(p5d_qe_mode)) > 0;
}
/* the pause menu's rows: the selected row is its one solid red bar, at a fixed pitch inside the black panel.
 * Returns 0..4 (設定/重試/自訂遊玩/放棄/返回) or -1 when not readable. */
static int p5d_pause_row_scan(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch) {
	if (w < 900 || h < 500) return -1;
	long sum = 0; int n = 0;
	for (int y = 150; y < (int)h && y < 390; y++) {
		const uint8_t *row = px + (size_t)y * pitch * 4;
		int r = 0, best = 0;
		for (int x = 240; x < 680; x++) {
			const uint8_t *q = row + (size_t)x * 4;
			if (q[0] > 145 && q[1] < 105 && q[2] < 105) { if (++r > best) best = r; } else r = 0;
		}
		if (best > 120) { sum += y; n++; }
	}
	if (n < 6) return -1;
	int cy = (int)(sum / n);
	int idx = (cy - 186) / 45;
	if (idx < 0) idx = 0; if (idx > 4) idx = 4;
	return idx;
}

static int p5d_pause_row = -1;   /* (legacy) the pause row from the picture: input no longer uses it */
/* M1522: our menu-selection counter, driven ONLY by the steps we send (wheel notches, arrow keys). The menus move
 * on the d-pad steps we synthesize, so counting them reproduces the game's row; the pause exit uses it. No picture. */
static int p5d_nav_row = 0, p5d_nav_rows = 6; static uint64_t p5d_nav_us;
static void p5d_nav_set(int rows, int row) { p5d_nav_rows = rows > 0 ? rows : 6; p5d_nav_row = row; p5d_nav_us = now_us(); }
static void p5d_nav_step(int d) {
	if (!p5d_nav_us || now_us() - p5d_nav_us > 15000000) { p5d_nav_rows = 6; p5d_nav_row = 0; }
	p5d_nav_row += d; if (p5d_nav_row < 0) p5d_nav_row = 0; if (p5d_nav_row >= p5d_nav_rows) p5d_nav_row = p5d_nav_rows - 1;
	p5d_nav_us = now_us();
}

static int p5d_menu_state_code(void) {
	uint32_t p = RD32(0x812B414Cu);
	if (!p || p >= 0x88000000u) return -1;
	uint32_t obj = RD32(p + 8);
	if (!obj || obj >= 0x88000000u) return -1;
	uint32_t st = RD32(obj + 4);
	return st <= 0x3f ? (int)st : -1;
}

static int p5d_app_state_code(void) {
	/* recomp_mem reserves 4 GiB, so these three reads are always safe: node = *(0x813B01A8);
	 * work = node[+8]; slot = work[+4] (see eboot 0x81185616 / 0x811857C2) */
	uint32_t node = RD32(0x813B01A8u);
	if (!node || node >= 0x88000000u) return -1;
	uint32_t work = RD32(node + 8);
	if (!work || work >= 0x88000000u) return -1;
	uint32_t st = RD32(work + 4);
	if (st > 6) return -1;
	{ static int once; if (!once) { once = 1; fprintf(stderr, "[APPST] node=%#x work=%#x slot=%u\n", node, work, st); } }
	return (int)st;
}
static const char *p5d_scene_name(int sc) {
	switch (sc) {
	case P5D_SCENE_TITLE: return "title";
	case P5D_SCENE_MAIN: return "main";
	case P5D_SCENE_COMMU: return "commu";
	case P5D_SCENE_SONG: return "song";
	case P5D_SCENE_DANCE: return "dance";
	case P5D_SCENE_PAUSE: return "pause";
	case P5D_SCENE_RESULT: return "result";
	default: return "boot";
	}
}
static void p5d_scene_set(int sc, const char *why) {
	if (sc < 0 || sc > P5D_SCENE_RESULT) return;
	if (sc != p5d_scene) { extern uint64_t display_frames(void);
		fprintf(stderr, "[SCENE] f=%llu %s -> %s (%s)\n", (unsigned long long)display_frames(), p5d_scene_name(p5d_scene), p5d_scene_name(sc), why); }
	p5d_scene = sc; p5d_scene_set_us = now_us();
}
static void p5d_scene_push(int sc, const char *why) { p5d_scene_prev = p5d_scene; p5d_scene_set(sc, why); }   /* the moment the COMMU row was confirmed (its map applies even if the hint patch is currently hidden) */
static uint64_t p5d_commu_exit_us;
static int p5d_song_latch;          /* the song list is up: Q/E are its D-pad (difficulty); the COMMU pages keep them as L/R */   /* a back press on the COMMU pages: the map is dropped once the main menu is back */      /* last frame the COMMU pages were on screen (their Q/E = L/R shoulders) */
static uint64_t p5d_config_hi_us;   /* the CONFIG page matched strictly (menus): what the settings screen needs */
static int p5d_story_latch;      /* the story map stays selected until the scene has been gone for a moment */
static uint64_t p5d_pause_closed_us;   /* the pause page was just closed: its fading frames must not re-latch the settings map */
static uint64_t p5d_pause_close_us;   /* M1630: an Esc / right-click asked the pause page to close: one Cross closes it at once,
                                       * the old "step to the exit row and confirm" sequence stays only as a fallback */
static int p5d_pause_close_tried;
static uint64_t p5d_pause_us;   /* when it opened (auto-release when the pause picture is gone but the toggle was never seen) */
static int p5d_pause_latch;     /* in the dance stage Esc toggled the pause menu: its page has the CONFIG-like controls */
static uint64_t p5d_pause_pic_us;   /* the pause menu's picture (the black panel with a red row) was on screen: the menus
                                     * and the dance gauge are still drawn behind it, so only the picture tells "still paused" */
static int p5d_config_from_pause;   /* the settings page was entered from the pause menu: its back returns to the pause pages */
static int p5d_pause_back_seq;   /* 1: the pause pages' exit sequence is playing (L+Circle; or 返回 row + Circle) */
static uint64_t p5d_pause_back_us;
static uint32_t p5d_tapq[8];     /* a short queued tap sequence, one every 120 ms (the pause pages' little chords) */
static int p5d_tapq_n, p5d_tapq_i;
static uint64_t p5d_config_ignore_us;   /* right after the settings page's exit chord the picture is still settling:
                                         * its template must not latch the settings page again */
static int p5d_config_latch;            /* (defined with the other pause/settings state below) */

static void p5d_chord(uint32_t bits, int ms);   /* M1631: two buttons pressed together */
static uint64_t p5d_settings_close_us;   /* M1631: a right-click/Esc asked the settings page to go back */
static int p5d_settings_close_step;     /* 1..5: which button of the exit sequence was sent last */
static void p5d_settings_back_request(void);
static void menu_tap(uint32_t bit);
static void p5d_settings_back_request(void) {
	/* M1631: the frame-verified way out of the settings page is exactly TWO inputs: ONE Cross (the page moves its focus
	 * to the OK row - the move takes a moment) and then ONE Circle (confirm). A second Cross sent too early un-focuses
	 * the OK row again (that was the bug in the first version of this fix), so the Circle is scheduled 800 ms later, and
	 * nothing else follows. If the page ignores it, the next right-click repeats it. */
	/* M1633: Cross now and Circle on the NEXT frame (16 ms later): the game applies the back focus and confirms it in two
	 * frames, so the page closes at once - no visible OK-row detour. */
	/* M1633: one Cross (the game's own back for this page) and one Circle 120 ms later: the page's back target is its OK
	 * row, and the game needs a few frames between the back and the confirm - at 120 ms the page animates away at once
	 * (the alternative, a held Cross or a same-frame chord, does nothing). A second pair is only sent if the page is
	 * still on the code-level screen after 0.5 s (a slow animation). */
	menu_tap(CROSS);
	p5d_settings_close_us = now_us(); p5d_settings_close_step = 1;
	menu_win_until = now_us() + 30000000;
	fprintf(stderr, "[PAUSE] settings back: Cross then Circle in 250 ms (direct exit)\n");
}
static void p5d_settings_back_queue(void) {   /* the settings page's exit: L focuses its OK row, the next Circle confirms it */
	p5d_config_latch = 0; p5d_config_ignore_us = now_us() + 1500000;
	/* M1631: the taps must be SEPARATE: the frame-verified sequence in the game's settings page is L (focus moves to the
	 * OK row) and then Circle (confirm) a few frames later. A simultaneous L+Circle hold left the page open with OK
	 * merely highlighted; two Crosses did not leave it either. Back to the sequential pair the page was verified with. */
	p5d_tapq[0] = LTRIG; p5d_tapq[1] = CIRCLE; p5d_tapq_n = 2; p5d_tapq_i = 0;
	p5d_pause_back_seq = 1; p5d_pause_back_us = now_us();
	fprintf(stderr, "[PAUSE] settings back: L (focus OK) then Circle (confirm)\n");
}

static void menu_tap(uint32_t bit);   /* defined below (the tap injector); the pause close below needs it */
/* M1630: Esc / a right-click in the pause page closes it with ONE Cross, at once (the page's own back). If the game
 * ignores it (a confirmation dialog, a page that wants the exit row) the exit sequence is queued 800 ms later. */
static void p5d_pause_close_request(void) {
	p5d_pause_pic_us = now_us(); p5d_pause_close_us = now_us(); p5d_pause_close_tried = 0;
	menu_tap(CROSS); menu_win_until = now_us() + 30000000;
}
static void p5d_pause_exit_queue(void) {
	/* the story scenes' pause menu is a different list (跳過劇情/高速朗讀/設定/結束事件/返回) with a tighter row
	 * pitch, so the dance menu's row scan does not map there: it opens on its first row, and four steps reach 返回 */
	int row = p5d_nav_row;   /* our own input counter, never the picture */
	if (row < 0) row = 0; if (row > 4) row = 4;
	int n = 4 - row;   /* the 返回 row is the last of the five, no wrap in the pause list */
	p5d_tapq_n = 0;
	for (int i = 0; i < n && p5d_tapq_n < 7; i++) p5d_tapq[p5d_tapq_n++] = DOWN;
	p5d_tapq[p5d_tapq_n++] = CIRCLE;
	p5d_tapq_i = 0; p5d_pause_back_seq = 1; p5d_pause_back_us = now_us();
}
static int p5d_dance_latch;      /* the dance map stays selected through the whole stage (its intro story, notice gaps) */
static uint64_t p5d_dance_flow_us;   /* the song confirm started the dance flow; the result/title ends it (a transient
                                      * menu/song-list model frame inside the flow must not clear the stage state: the
                                      * dance gauge only exists after the intro, so the latch alone left gaps) */
static int p5d_dance_flow(uint64_t t) { return p5d_dance_flow_us && t - p5d_dance_flow_us < 300000000ull; }
static void p5d_dance_flow_start(uint64_t t) { p5d_dance_flow_us = t; }
static uint64_t p5d_dance_absent_us;
static uint64_t p5d_story_absent_us;

/* the P5 dialogue box: white text rows on a dark box in the lower part of the frame, no red banner above and no big
 * white panel (the menus' list rows are red bars, the settings page has much more white, a bright story CG too).
 * Sampled, not exact: story scenes differ in artwork, the box is what they share. */
static int p5d_story_box(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch) {
	uint32_t y0 = (uint32_t)(380.0 * h / 544.0), y1 = (uint32_t)(470.0 * h / 544.0);
	uint32_t x0 = (uint32_t)(100.0 * w / 960.0), x1 = (uint32_t)(860.0 * w / 960.0);
	uint32_t ty0 = (uint32_t)(20.0 * h / 544.0), ty1 = (uint32_t)(200.0 * h / 544.0);
	uint32_t tx0 = (uint32_t)(250.0 * w / 960.0), tx1 = (uint32_t)(900.0 * w / 960.0);
	long tot = 0, white = 0, dark = 0, red = 0, ttot = 0, tred = 0;
	for (uint32_t y = y0; y < y1; y += 2) for (uint32_t x = x0; x < x1; x += 2) {
		const uint8_t *p = px + (size_t)y * pitch + (size_t)x * 4;
		int r = p[0], g = p[1], b = p[2];
		tot++;
		if (r > 200 && g > 200 && b > 190) white++;
		else if (r + g + b < 120) dark++;
		if (r > 140 && g < 70 && b < 70) red++;
	}
	for (uint32_t y = ty0; y < ty1; y += 2) for (uint32_t x = tx0; x < tx1; x += 2) {
		const uint8_t *p = px + (size_t)y * pitch + (size_t)x * 4;
		ttot++;
		if (p[0] > 140 && p[1] < 70 && p[2] < 70) tred++;
	}
	if (!tot || !ttot) return 0;
	float fw = (float)white / (float)tot, fd = (float)dark / (float)tot, fr = (float)red / (float)tot, ft = (float)tred / (float)ttot;
	return fw > 0.005f && fw < 0.06f && fd > 0.45f && fr < 0.02f && ft < 0.03f;
}
static uint64_t p5d_song_since_us;    /* first frame the song list has been continuously on screen: only then is the dance stage over */
static uint64_t p5d_config_us;     /* last frame the CONFIG (settings) screen's HELP badge was on screen: that screen has its own key map */
static int p5d_config_latch;       /* the CONFIG key map stays selected for the whole settings session (its confirmation dialogs replace the HELP badge, so the template alone would drop it) */
static uint64_t last_menu_us;     /* last time a modelled menu was on screen (the dance-stage switch fallback) */

static uint64_t last_song_us;
static uint64_t p5d_esc_fallback_us;   /* an Esc that tried the pause menu: if no pause picture appears, it was a menu page and
                                          * the same Esc becomes its back button (the stage and the menus shared one picture at 2K) */
static uint64_t p5d_commu_job_us, p5d_config_job_us, p5d_result_job_us, p5d_song_job_us, p5d_tutorial_job_us, p5d_pause_job_us, p5d_story_job_us, p5d_title_job_us, p5d_collection_job_us;   /* code-level screen evidence (job names) */
static uint64_t p5d_dance_job_us;   /* last frame the dance-stage jobs ran (replay dance / danceVoiceSub / a dancing menu): a
                                      * code-level "the stage is on screen" signal at any resolution (the gauge picture fails at 2K) */
static uint64_t p5d_main_job_us;   /* last frame the game ran its main-menu job */
static int p5d_song_flow_armed;   /* set when the DANCING row is confirmed; only then does a song-list confirm start the dance flow */
static uint64_t p5d_song_ev_us;   /* last frame the song list (or its DANCING-row confirm / MusicSelect job) was positively seen: only then are Q/E the D-pad */     /* last time the song list was recognised (Q/E bridge a detection flicker) */static uint64_t scratch_t0;       /* Space = a scratch swipe on the front touch panel (the dance stage's scratch) */
static int scratch_active;
static const char *p5d_ui_name(void) {
	switch (p5d_ui) { case P5D_UI_MAIN: return "MAIN"; case P5D_UI_SONG: return "SONG"; case P5D_UI_TITLE: return "TITLE"; case P5D_UI_RESULT: return "RESULT"; default: return "NONE"; }
}
/* the screen model is computed from the presented frame on the renderer's thread (gfx_gpu.c's present,
 * right after it syncs the frame into guest memory) and published here, so the window's input thread only
 * reads a finished result -- never a frame half-written by the sync. */
static uint64_t p5d_ui_valid_us;
/* reading the rendered frame back for the screen model costs one full-frame GPU download (w*h*4: 2 MB at 1x, 19 MB
 * at 3x/2K). At 3x that stalled the present thread every frame and the controls felt heavy (reported: "能不能在2k
 * 渲染之下也能正常操控"). So the scan is time-throttled (more at higher render scales) and runs fast only for a short
 * window after any input, where a scene change can happen. RECOMP_UI_SCAN_MS overrides the idle interval (debug). */
static uint64_t p5d_ui_boost_until_us;
static void p5d_ui_boost(void) { p5d_ui_boost_until_us = now_us() + 700000; }
static int p5d_ui_cur_n = -1;
/* the score of the best template at its learned place (rendered UI is pixel-identical between runs; the
 * picture may be render-scaled, so the template's coordinates scale with w/960 and h/544) */
static float ui_template_score(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch, int t, float *fx, float *fy) {
	const UiTemplate *tp = &ui_templates[t];
	float sx = (float)w / 960.f, sy = (float)h / 544.f;
	int x0 = (int)(tp->x * sx), y0 = (int)(tp->y * sy);
	int tw = (int)(tp->w * sx), th = (int)(tp->h * sy);
	if (tw < 4 || th < 4 || x0 < 0 || y0 < 0 || x0 + tw >= (int)w || y0 + th >= (int)h) return 0;
	int hit = 0, hitb = 0, tot = 0;
	for (int j = 0; j < th; j += 2) {
		const uint8_t *row = px + (size_t)(y0 + j) * pitch * 4;
		for (int i = 0; i < tw; i += 2) {
			int ti = (int)((float)i / sx), tj = (int)((float)j / sy);
			if (ti >= tp->w || tj >= tp->h) continue;
			const unsigned char *tpix = tp->px + ((size_t)tj * tp->w + ti) * 3;
			const uint8_t *q = row + (size_t)(x0 + i) * 4;
			if (abs((int)q[0] - tpix[0]) <= 16 && abs((int)q[1] - tpix[1]) <= 16 && abs((int)q[2] - tpix[2]) <= 16) hit++;
			if (abs((int)q[2] - tpix[0]) <= 16 && abs((int)q[1] - tpix[1]) <= 16 && abs((int)q[0] - tpix[2]) <= 16) hitb++;
			tot++;
		}
	}
	*fx = (float)x0 / (float)w; *fy = (float)y0 / (float)h;
	float f = tot ? (float)(hit > hitb ? hit : hitb) / (float)tot : 0;
	return f;
}
static int ui_title_result(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch, float *best, const char **tname) {
	/* template 0 = title plate, 3 = RESULT's MAX COMBO label (the two screens whose UI is a fixed plaque;
	 * the list screens are better served by their row heuristics, which accept any highlighted row) */
	int cand[2] = { 0, 3 }; int bi = -1; float bf = 0;
	for (int i = 0; i < 2; i++) {
		float fx, fy, f = ui_template_score(px, w, h, pitch, cand[i], &fx, &fy);
		if (f > bf) { bf = f; bi = cand[i]; }
	}
	*best = bf; if (tname && bi >= 0) *tname = ui_templates[bi].name;
	float thr = ((p5d_ui == P5D_UI_TITLE && bi == 0) || (p5d_ui == P5D_UI_RESULT && bi == 3)) ? 0.78f : 0.88f;
	if (bi < 0 || bf < thr) return P5D_UI_NONE;
	return bi == 0 ? P5D_UI_TITLE : P5D_UI_RESULT;
}
static int row_scan_main(const uint8_t *px, uint32_t h, uint32_t pitch) {
	static const int ys[6] = { 219, 257, 292, 327, 362, 398 };
	int best = -1, bestred = 0, rows = 0;
	for (int r = 0; r < 6; r++) {
		int red = 0, white = 0;
		for (int y = ys[r] - 10; y <= ys[r] + 10 && y < (int)h; y++) {
			for (int x = 70; x < 142; x++) { const uint8_t *q = px + (size_t)y * pitch * 4 + (size_t)x * 4; if (q[0] > 150 && q[1] < 70 && q[2] < 70) red++; }
			for (int x = 70; x < 250; x += 2) { const uint8_t *q = px + (size_t)y * pitch * 4 + (size_t)x * 4; if (q[0] > 165 && q[1] > 165 && q[2] > 165) white++; }
		}
		if (white > 40) rows |= 1 << r;
		if (red > bestred) { bestred = red; best = r; }
	}
	if (__builtin_popcount((unsigned)rows) < 5 || bestred < 60) return -1;
	for (int r = 0; r < 6; r++) p5d_ui_y[r] = ys[r];
	p5d_ui_n = 6;
	return best;
}
static int row_scan_song(const uint8_t *px, uint32_t h, uint32_t pitch) {
	static const int ys[4] = { 118, 198, 278, 358 };
	int bestr = -1, bestrun = 0;
	for (int r = 0; r < 4; r++) {
		int br = 0;
		for (int y = ys[r] - 12; y <= ys[r] + 12 && y < (int)h; y += 2) {
			int run = 0;
			for (int x = 440; x < 946; x++) { const uint8_t *q = px + (size_t)y * pitch * 4 + (size_t)x * 4; int R = q[0], Gc = q[1], B = q[2]; if (R > 140 && Gc > 140 && B > 140 && abs(R - Gc) < 26 && abs(Gc - B) < 26) run++; else { if (run > br) br = run; run = 0; } }
			if (run > br) br = run;
		}
		if (br > bestrun) { bestrun = br; bestr = r; }
	}
	if (bestrun < 280) return -1;
	for (int r = 0; r < 4; r++) p5d_ui_y[r] = ys[r];
	p5d_ui_n = 4;
	return bestr;
}
/* the RESULT screen's 4-row menu (RETRY / CUSTOM / QUIT / WATCH REPLAY): its selected row is the only
 * solid red bar in the list panel (x 235..679, one row band of 40 px); the tally screen and every other
 * screen have no such bar there, so "no bar" means the menu is not up and the result row model stays -1.
 * Written for the 960x544 picture; the presented frame is scanned at that size (row_scan_main and
 * row_scan_song use the same nominal coordinates). */
static int p5d_result_row_scan(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch) {
	static const int ys[4] = { 187, 227, 267, 307 };
	float sx = (float)w / 960.f, sy = (float)h / 544.f;
	int best = -1; long bestred = 0;
	for (int r = 0; r < 4; r++) {
		long red = 0;
		int y0 = (int)((ys[r] - 14) * sy), y1 = (int)((ys[r] + 14) * sy);
		int x0 = (int)(240 * sx), x1 = (int)(676 * sx);
		if (y0 < 0) y0 = 0; if (y1 > (int)h) y1 = (int)h;
		if (x0 < 0) x0 = 0; if (x1 > (int)w) x1 = (int)w;
		for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
			const uint8_t *q = px + (size_t)y * pitch * 4 + (size_t)x * 4;
			if (q[0] > 150 && q[1] < 60 && q[2] < 60) red++;
		}
		if (red > bestred) { bestred = red; best = r; }
	}
	if (bestred < (long)(160 * sx * sy)) return -1;   /* no highlighted row: the tally screen, not the menu */
	for (int r = 0; r < 4; r++) p5d_ui_y[r] = ys[r];
	p5d_ui_n = 4;
	return best;
}
static void p5d_click_advance(void);   /* defined below, after the tap state */
/* ---- the game's own UI state: the authoritative screen/row source for the PC interaction layer.
 * Found with the integer memory scans (RECOMP_MEMSCAN_INT + ISTEP + scripted presses) and verified against
 * captured frames of each screen:
 *   0x8153f9b4   a 32-bit word in the main module's data. It counts the game's UI transitions, and because
 *                the menus move exactly one step per input its value names the screen and the selected row:
 *                  0,1,2   boot (logos, opening animation)          4  title (PRESS ANY BUTTON)
 *                  5..10   main menu, row 0..5  (row = v - 5)
 *                  7,9,11,13 song select, row 0..3 (row = (v - 7) / 2; the intermediate 8,10,12 are its
 *                            transition animation)
 *                Main menu row 2 (v=7) and song-select row 0 (v=7) share a value, so the value alone cannot
 *                tell those two apart: the difficulty-tab template (ui_templates.h "song") scores 0.65 on the
 *                song select against 0.00 on the main menu and decides. Everything else -- the boot screens,
 *                the story cutscenes, the dance stage -- maps to NONE, exactly as before.
 * The address holds for this executable (main module data); the value is sanity-checked first. */
#define P5D_UI_STATE_ADDR 0x8153f9b4u
static int p5d_song_tpl_score(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch) {
	/* "song" matches the difficulty-tab strip (its score drops when the selected tab changes) and "song2"
	 * matches the bottom hint row, which looks the same for every difficulty: the best of the two keeps the
	 * song select recognised after the user changes the difficulty, which is when the profile switch used
	 * to fall back to the dance map and the keys appeared dead. */
	int best = 0;
	for (int t = 0; t < (int)(sizeof ui_templates / sizeof ui_templates[0]); t++) {
		if (strncmp(ui_templates[t].name, "song", 4)) continue;
		float fx, fy; int s = (int)(ui_template_score(px, w, h, pitch, t, &fx, &fy) * 1000.f);
		if (s > best) best = s;
	}
	return best;
}
static int p5d_guest_ui(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch, int *row) {
	uint32_t v = RD32(P5D_UI_STATE_ADDR);
	if (row) *row = -1;
	if (v > 0x1000) return P5D_UI_NONE;                 /* not this build's layout: fall back to pixels */
	if (v == 4) return P5D_UI_TITLE;
	if (v >= 5 && v <= 14) {
		int song = p5d_song_tpl_score(px, w, h, pitch) >= 600;   /* 0.65 on song select, 0.00 on the menu (0.60 keeps submenus out) */
		if (song) { if (row) { int r = (int)(v - 7); if (r > 0) r /= 2; if (r < 0) r = 0; if (r > 3) r = 3; *row = r; } return P5D_UI_SONG; }
		if (v >= 5 && v <= 10) { if (row) *row = (int)(v - 5); return P5D_UI_MAIN; }
	}
	return P5D_UI_NONE;
}
/* The main menu is drawn on a moving 3D plane: neither the six rows nor the selected red banner stay at
 * fixed y between selection states (DANCING's banner centre is y~260, SYSTEM's y~465). What is stable is
 * that the red banner follows the highlight, so the pointer's y drives a closed loop against the banner's
 * centroid: DOWN while the pointer is below it, UP while above, stop within 18 px. This is the mouse->game
 * link the menu itself does not have (a console game has no pointer) and it reaches every row, including
 * the last one. Six white text rows across the list still confirm the screen. */
#define P5D_PROBE_LO 0x10000000u        /* the UI heap: the menu's selection-index object lives in here */
#define P5D_PROBE_HI 0x13000000u
static uint32_t ui_sel_addr;            /* discovered in this session (the heap layout moves between runs) */
static int ui_direct;                   /* a verified selection-index address is known: write it directly */
static void p5d_probe_arm(void);        /* forward: the screen model starts it when the menu first appears */
static int p5d_banner_y = -1;
static int p5d_main_banner_y(const uint8_t *px, uint32_t h, uint32_t pitch) {
	/* the banner is the one wide red blob in the list column; its height (the slanted quad) separates it
	 * from small red decorations, so no text-row check is needed and the value is available every frame
	 * (the text rows move with the selection too, which made the old row-band check reject valid frames). */
	int n = 0; long sy = 0; int miny = 99999, maxy = -1;
	for (int y = 212; y < (int)h && y < 500; y++)   /* the menu banner lives in 230..500; the bottom
		                                              * 'CrossSave' label (y~515) is red and polluted the centroid */
		for (int x = 65; x < 250; x++) {
			const uint8_t *q = px + (size_t)y * pitch * 4 + (size_t)x * 4;
			if (q[0] > 140 && q[1] < 80 && q[2] < 80) { n++; sy += y; if (y < miny) miny = y; if (y > maxy) maxy = y; }
		}
	if (n < 250) return -1;
	int hgt = maxy - miny;
	if (hgt < 22 || hgt > 160) return -1;
	return (int)(sy / n);
}
/* The dance stage's song gauge: a black-outlined bar across the bottom of the 960x544 picture -- two
 * dark border rows around y=520 and y=530 with a bright (white, or cyan where the song has played)
 * interior between them, more than 400 px wide. No menu or result screen draws that bar, and it is
 * drawn from the first dance frame; that makes it the signal that tells the dance stage apart from a
 * submenu page, which the old "no modelled screen for 2.5 s" heuristic could not. */
static int p5d_dance_bar(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch) {
	if (w < 640 || h < 400) return 0;
	/* M1629: the picture can be render-scaled (2880x1632 at 2K): the layout below is authored in 960x544 */
	int sxp = (int)(w / 960), syp = (int)(h / 544); if (sxp < 1) sxp = 1; if (syp < 1) syp = 1;
	int run[35];
	for (int i = 0; i < 35; i++) {
		const uint8_t *row = px + (size_t)((505 + i) * syp) * pitch * 4;
		int best = 0, r = 0;
		for (int x = 170 * sxp; x < 860 * sxp; x++) {
			const uint8_t *q = row + (size_t)x * 4;
			if (q[0] < 60 && q[1] < 60 && q[2] < 60) { if (++r > best) best = r; } else r = 0;
		}
		run[i] = best;
	}
	for (int i = 0; i < 35 - 8; i++) {
		if (run[i] < 400 * sxp) continue;
		for (int j = i + 6; j < 35 && j < i + 20; j++) {
			if (run[j] < 400 * sxp) continue;
			int bestb = 0;
			for (int k = i + 1; k < j; k++) {
				const uint8_t *row = px + (size_t)((505 + k) * syp) * pitch * 4;
				int r = 0;
				for (int x = 170 * sxp; x < 860 * sxp; x++) {
					const uint8_t *q = row + (size_t)x * 4;
					if (q[0] > 150 || q[1] > 150 || q[2] > 150) { if (++r > bestb) bestb = r; } else r = 0;
				}
			}
			if (bestb >= 250 * sxp) return 1;
			break;
		}
	}
	return 0;
}
/* The pause menu (the dance's and the story scenes'): a solid black panel with one bright red selected row,
 * drawn over the game's picture. Read only while a pause was opened from the key handler (the key handler sets
 * the latch), to tell "still in the pause pages" from "the game went on". */
static int p5d_pause_picture(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch) {
	if (w < 640 || h < 400) return 0;
	/* M1629: render-scale aware (the panel is authored at 960x544; the buffer can be 2880x1632 at 2K) */
	int sxp = (int)(w / 960), syp = (int)(h / 544); if (sxp < 1) sxp = 1; if (syp < 1) syp = 1;
	int black_rows = 0, red_rows = 0;
	for (int y = 120 * syp; y < (int)h && y < 400 * syp; y++) {
		const uint8_t *row = px + (size_t)y * pitch * 4;
		int br = 0, rr = 0;
		for (int x = 180 * sxp; x < 740 * sxp; x++) {
			const uint8_t *q = row + (size_t)x * 4;
			if (q[0] < 48 && q[1] < 48 && q[2] < 48) br++;
			if (q[0] > 145 && q[1] < 105 && q[2] < 105) rr++;
		}
		if (br > 340 * sxp) black_rows++;
		if (rr > 170 * sxp) red_rows++;
	}
	return black_rows >= 50 * syp && red_rows >= 2;
}
static void p5d_ui_scan_pixels(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch) {
	if (!px || w < 640 || h < 400 || pitch < w) return;
	if (p5d_dance_bar(px, w, h, pitch)) p5d_dance_bar_us = now_us();
	if (p5d_pause_latch && p5d_pause_picture(px, w, h, pitch)) p5d_pause_pic_us = now_us();
	if (p5d_pause_latch && now_us() - p5d_pause_pic_us < 300000) { int r = p5d_pause_row_scan(px, w, h, pitch); if (r >= 0) p5d_pause_row = r; }
	/* only on an unmodelled screen after the menu has been seen: the title/movie/logo frames at boot tripped the box
	 * detector and put the dance map on the menus (a profile switch per frame: stutter and hidden keycaps, reported 2026-11-07) */
	if (p5d_ui == P5D_UI_NONE && p5d_saw_menu && p5d_story_box(px, w, h, pitch)) p5d_story_us = now_us();
	{ float fx, fy; float sc = ui_template_score(px, w, h, pitch, 5, &fx, &fy);
	  if (sc >= 0.92f) p5d_config_hi_us = now_us();   /* strict: the menus' settings screen */
	  if (sc >= 0.80f) p5d_config_us = now_us(); }   /* loose: the in-game pause menu's settings page (drawn over the dance) */
	{ float fx, fy; if (ui_template_score(px, w, h, pitch, 6, &fx, &fy) >= 0.75f) p5d_config_us = now_us(); }   /* index 6 = "config2" */
	{ extern int gfx_hintglyph_lr_badges(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch);
	  if (p5d_app_state_code() < 0) {   /* only while the sequence manager is not readable (boot) */
	  static uint64_t commu_scan_us;
	  if (now_us() - commu_scan_us > 200000) {   /* the hint bar changes between scenes, not between frames */
		commu_scan_us = now_us();
		if (gfx_hintglyph_lr_badges(px, w, h, pitch)) p5d_commu_us = now_us();
	{ extern int gfx_commu_screen(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch);
	  if (gfx_commu_screen(px, w, h, pitch)) p5d_commu_screen_us = now_us(); }
	  }
	  }
	}   /* real time fallback: the L/R keycap hints mean the COMMU pages are up */
	if (p5d_saw_menu) {
		/* the song list is recognised by its difficulty-tab template alone: the guest transition counter
		 * 0x8153f9b4 only maps on the first pass (it keeps counting after that), so the state word cannot
		 * be relied on once the user has been through the menus - which is exactly when the song select
		 * (and the wheel there) must still work. This test must run BEFORE the main-menu banner: the song
		 * select's red background also matched the banner detector, so the screen flipped to MAIN there
		 * and the Q/E and tab-strip wheel stopped working. */
		if (p5d_song_tpl_score(px, w, h, pitch) >= 620) {
			p5d_ui = P5D_UI_SONG; p5d_ui_valid_us = now_us(); p5d_ui_n = 4; p5d_ui_cur = -1; p5d_saw_menu = 1;
			menu_win_until = now_us() + 30000000;
			/* (the dance hold is not cleared here: a story frame, the "LET'S DANCE" confirm and the song list
			 * itself all match this template for a moment. The hold clears once the song list has been steady
			 * for a while, in the key logic below -- Esc in a story scene is the pause menu; reported 2026-11-07) */
			return;
		}
		/* the RESULT TALLY stage first (the score view before the 4-row menu slides in): the menu templates score
		 * only ~0.61 there, so it used to read as unmodelled. Two strict masks (the diagonal NOT CLEAR/CLEAR banner
		 * and the HIGH SCORE / SCORE block) recognise it; publish it as RESULT with no selected row, so a wheel
		 * notch sends the game's own UP/DOWN step and a click confirms, exactly like the menu stage below. */
		{ extern int gfx_result_tally(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch);
		  if (gfx_result_tally(px, w, h, pitch)) {
			p5d_ui = P5D_UI_RESULT; p5d_ui_valid_us = now_us(); p5d_ui_score = 1.0f;
			p5d_ui_n = 4; p5d_ui_cur = -1; p5d_menu_sel = -1;
			menu_win_until = now_us() + 30000000;
			song_confirm_us = 0;
			return;
		  } }
		/* the RESULT menu's 4 rows next: its selected row is the only solid red bar in the list panel --
		 * exactly the shape the MAIN banner heuristic below hunts for -- and the MAX COMBO plaque template
		 * scores ~1.00 on the menu (MAIN's own score stays <=0.5), so the template decides the screen before
		 * the banner can claim it. (The tally screen scores ~0.63: below the 0.78 sticky threshold, so it
		 * stays NONE/MAIN until the menu slides in -- the wheel is then routed as a list, which the tally
		 * ignores; once the menu is up the row model below is live.) */
		{	float rbest; const char *rtn = "?";
			int rscr = ui_title_result(px, w, h, pitch, &rbest, &rtn);
			if (rscr == P5D_UI_RESULT && rbest >= 0.80f) {
				int row = p5d_result_row_scan(px, w, h, pitch);
				p5d_ui = P5D_UI_RESULT; p5d_ui_valid_us = now_us(); p5d_ui_score = rbest;
				p5d_ui_n = 4; p5d_ui_cur = row; p5d_menu_sel = row;
				menu_win_until = now_us() + 30000000;
				song_confirm_us = 0;   /* the dance is over: the screens after this are menus again */
				return;
			} }
	}
	if (p5d_saw_title) {
		int by = p5d_main_banner_y(px, h, pitch);
		if (by >= 0) {
			p5d_banner_y = by;
			p5d_ui = P5D_UI_MAIN; p5d_ui_valid_us = now_us(); p5d_ui_n = 6;
			p5d_ui_cur = -1; p5d_menu_sel = -1; p5d_saw_menu = 1;
			menu_win_until = now_us() + 30000000;
			if (p5d_profile == P5D_MENU) song_confirm_us = 0;   /* back at the main menu: a submenu entered from here is not the dance (in the dance map a false banner hit must not drop the hold) */
			return;
		}
	}
	{	int grow = -1; int gs = p5d_guest_ui(px, w, h, pitch, &grow);
		if (gs != P5D_UI_NONE) {
			static int pend_scr = -1, pend_row = -1; static uint64_t pend_since;
			if (gs != pend_scr || grow != pend_row) { pend_scr = gs; pend_row = grow; pend_since = now_us(); }
			uint64_t still = now_us() - pend_since;
			if (still < 200000 && p5d_ui != P5D_UI_NONE && (p5d_ui == P5D_UI_MAIN || p5d_ui == P5D_UI_SONG)) {
				/* keep the settled screen for the animation; the new row is published once it stops moving */
				p5d_ui_valid_us = now_us();
				if (p5d_ui == P5D_UI_MAIN || p5d_ui == P5D_UI_SONG) { p5d_saw_menu = 1; return; }
			}
			p5d_ui = gs; p5d_ui_valid_us = now_us();
			if (gs == P5D_UI_TITLE) { p5d_ui_cur = -1; p5d_ui_n = 0; p5d_menu_sel = -1; p5d_saw_title = 1; return; }
			if (gs == P5D_UI_MAIN) {
				static const int ys6[6] = { 219, 257, 292, 327, 362, 398 };
				p5d_ui_n = 6; for (int r = 0; r < 6; r++) p5d_ui_y[r] = ys6[r];
				if (grow >= 0 && grow < 6) { p5d_ui_cur = grow; p5d_menu_sel = grow; p5d_menu_last = grow; }
				p5d_saw_menu = 1; return;
			}
			if (gs == P5D_UI_SONG) {
				static const int ys4[4] = { 118, 198, 278, 358 };
				p5d_ui_n = 4; for (int r = 0; r < 4; r++) p5d_ui_y[r] = ys4[r];
				if (grow >= 0 && grow < 4) { p5d_ui_cur = grow; p5d_menu_sel = grow; p5d_menu_last = grow; }
				p5d_saw_menu = 1; return;
			}
		}
	}
	float best; int scr = ui_title_result(px, w, h, pitch, &best, &p5d_ui_tname); p5d_ui_score = best;
	if (scr != P5D_UI_NONE) {
		p5d_ui = scr; p5d_ui_valid_us = now_us(); p5d_ui_cur = -1; p5d_ui_n = 0; p5d_menu_sel = -1;
		if (scr == P5D_UI_TITLE) p5d_saw_title = 1;
		return;
	}
	/* no pixel fallback for the list screens: the state word is authoritative for those, and the pixel
	 * heuristics matched boot logos and cutscene frames (a false MAIN while the menu is not up) */
	if (now_us() - p5d_ui_valid_us > 250000) { p5d_ui = P5D_UI_NONE; p5d_ui_cur = -1; p5d_ui_n = 0; p5d_menu_sel = -1; }
}
void window_ui_model_from_frame(const void *pix, uint32_t w, uint32_t h, uint32_t pitch) { p5d_ui_scan_pixels((const uint8_t *)pix, w, h, pitch); }
static int p5d_menu_read(void) { return p5d_ui_cur; }
static int p5d_menu_row_at(int sx, int sy) {
	if (p5d_ui == P5D_UI_NONE) return -1;
	int x0 = p5d_ui == P5D_UI_MAIN ? 20 : p5d_ui == P5D_UI_RESULT ? 230 : 430;
	int x1 = p5d_ui == P5D_UI_MAIN ? 280 : p5d_ui == P5D_UI_RESULT ? 690 : 950;
	if (sx < x0 || sx > x1) return -1;
	int best = -1, bd = p5d_ui == P5D_UI_MAIN ? 26 : p5d_ui == P5D_UI_RESULT ? 20 : 34;
	for (int r = 0; r < p5d_ui_n; r++) { int d = sy - p5d_ui_y[r]; if (d < 0) d = -d; if (d < bd) { bd = d; best = r; } }
	return best;
}
/* closed-loop navigation: one D-pad tap, then re-read the guest frame's highlight; stop as soon as it
 * reaches the clicked row (so a pressed-through repeat or a wrong step count cannot jump past it), and a
 * second click on the highlighted row sends exactly one Cross */
static int menu_target = -1, menu_enter;   /* menu_target: a row; or -1 idle */
static int nav_last_sel = -2, nav_stall;   /* progress watchdog for the closed-loop navigation */
static uint64_t menu_next_us, menu_tap_until; static uint32_t menu_tap_bits;
static void p5d_ui_boost(void);
static void menu_tap(uint32_t bit) { menu_tap_bits = bit; menu_tap_until = now_us() + 45000; p5d_ui_boost(); }
static void p5d_chord(uint32_t bits, int ms) {   /* M1631: buttons pressed together (a chord), held ms milliseconds */
	if (ms < 60) ms = 60;
	fprintf(stderr, "[PAD] chord bits=%#x for %d ms\n", bits, ms);
	menu_tap_bits = bits; menu_tap_until = now_us() + (uint64_t)ms * 1000ull; p5d_ui_boost();
}
static int pending_row = -1, pending_enter; static uint64_t pending_until;
static void p5d_menu_click(int row, int enter) {
	if (enter) { menu_enter = 1; menu_next_us = now_us(); }
	else { menu_enter = 0; menu_target = row; menu_next_us = now_us(); }
}
/* a left click on the list: hold it a moment if this frame's highlight cannot be read (a transition or a
 * frame whose pixels are not synced yet), then run the closed loop from the next readable frame */
static int menu_click_pending(int row) {
	pending_row = row; pending_enter = 0; pending_until = now_us() + 500000;
	return 1;
}
static int ui_hover_y = -1, ui_hover_on, hover_taps, hover_enter_pending;
static void p5d_direct_write(int row) { if (ui_direct && ui_sel_addr && row >= 0 && row <= 5) WR32(ui_sel_addr, (uint32_t)(row + 1)); }
static int hover_burst_left, hover_burst_dir;   /* back-to-back steps for a long pointer jump */
static int hover_retry;
/* selection-index discovery state (its address moves between sessions: see p5d_probe_tick below) */
static uint8_t *probe_snap;
static uint32_t probe_cand[64];
static int probe_n, probe_state, probe_timer, probe_try, probe_armed;
static int probe_cal_y[6], probe_cal_row;   /* the banner centroid measured for each selected row */
static int probe_pass1;                     /* writing DANCING moved the banner to the top row */
static uint32_t probe_g0;                   /* the UI transition counter before our scripted tap */
static uint8_t *probe_snap1, *probe_hit1;   /* the heap after step 1 and its +1 candidates */
static void p5d_hover_tick(void) {
	if (p5d_ui != P5D_UI_MAIN || p5d_banner_y < 0) return;
	if (probe_state > 0) return;   /* discovery in progress: do not fight the scripted tap */
	if (ui_direct) {   /* pointer row from the per-session calibrated banner-y table (fallback: nominal) */
		if (ui_hover_y < 0) return;
		static const int bys0[6] = { 260, 300, 340, 380, 425, 465 };
		const int *bys = probe_cal_row >= 6 ? probe_cal_y : bys0;
		int best = 0, bd = 9999;
		for (int r = 0; r < 6; r++) { int d = ui_hover_y - bys[r]; if (d < 0) d = -d; if (d < bd) { bd = d; best = r; } }
		p5d_direct_write(best);
		return;
	}
	if (!ui_hover_on) return;
	uint64_t now = now_us();
	if (menu_tap_bits && now < menu_tap_until) return;
	/* wait until the banner has stopped moving: a tap starts a list animation and the centroid during it
	 * says nothing about the settled row (acting on it made the hover overshoot back and forth) */
	{ static int lastby = -1; static uint64_t lastchange;
	  if (p5d_banner_y != lastby) { lastby = p5d_banner_y; lastchange = now; return; }
	  if (now - lastchange < 70000) return; }
	int dy = ui_hover_y - p5d_banner_y;
	if (dy < 0) dy = -dy;
	if (dy <= 20) {
		if (hover_enter_pending) { hover_enter_pending = 0; menu_enter = 1; menu_next_us = now; fprintf(stderr, "[UI] hover reached (banner %d, pointer %d) -> confirm\n", p5d_banner_y, ui_hover_y); }
		ui_hover_on = 0; hover_taps = 0;
		return;
	}
	if (now < menu_next_us) return;
	if (++hover_taps > 26) {   /* the animation throttles a few presses: wait a moment and retry; only after
	                            * two full rounds give up (giving up early left the highlight stuck mid-way) */
		if (hover_retry < 2) { hover_retry++; hover_taps = 0; hover_burst_left = 0; menu_next_us = now + 350000; fprintf(stderr, "[UI] hover retry %d (banner %d, pointer %d)\n", hover_retry, p5d_banner_y, ui_hover_y); }
		else { ui_hover_on = 0; hover_enter_pending = 0; hover_retry = 0; fprintf(stderr, "[UI] hover abort (banner %d, pointer %d)\n", p5d_banner_y, ui_hover_y); }
		return; }
	/* a long jump (first row -> last row) must not wait one settle per row: the list moves ~44 px a row,
	 * so the pixel distance gives the number of steps and they are sent back to back (60 ms apart, the
	 * game processes one press edge a frame); the settled-banner check above then corrects the last
	 * step or two. */
	int dy2 = ui_hover_y - p5d_banner_y;
	int dir = dy2 > 0 ? DOWN : UP;
	int want = (dy2 < 0 ? -dy2 : dy2) / 44;
	if (want < 1) want = 1; if (want > 6) want = 6;
	if (hover_burst_left < want && hover_burst_left < 1) { hover_burst_left = want; hover_burst_dir = dir; }
	if (hover_burst_left > 0) {
		menu_tap(hover_burst_dir);
		hover_burst_left--;
		menu_next_us = now + 60000;
		fprintf(stderr, "[UI] hover burst %s %d left (banner %d -> pointer %d)\n", hover_burst_dir == DOWN ? "DOWN" : "UP", hover_burst_left, p5d_banner_y, ui_hover_y);
	} else {
		menu_tap(dir);
		menu_next_us = now + 80000;
		fprintf(stderr, "[UI] hover tap %s (banner %d -> pointer %d, %d)\n", dir == DOWN ? "DOWN" : "UP", p5d_banner_y, ui_hover_y, hover_taps);
	}
}
/* Runtime discovery of the menu's selection-index address. The heap layout moves between sessions (the
 * hardcoded 0x11426e60 worked in one run and did nothing in the next), so it is found once per session:
 * RECOMP_HEAPSCAN validated the method offline - snapshot the UI heap, send one DOWN, keep the isolated
 * small ints that increased by exactly 1, then write DANCING (1) to each candidate for a few frames and
 * keep the one whose red banner jumps to the top row. The confirmed address then makes the hover a single
 * per-frame write: zero latency, no D-pad presses, no list animation. */
static void p5d_probe_arm(void) {
	if (probe_state || ui_direct || probe_armed) return;
	probe_armed = 1;
	probe_snap = (uint8_t *)malloc(P5D_PROBE_HI - P5D_PROBE_LO);
	if (!probe_snap) { probe_state = -1; return; }
	memcpy(probe_snap, G(P5D_PROBE_LO), P5D_PROBE_HI - P5D_PROBE_LO);
	probe_state = 1; probe_timer = 0;
	probe_g0 = RD32(0x8153f9b4);   /* the UI transition counter: it changes when the game processed our tap */
	menu_tap(DOWN);   /* one scripted step: the index increments by exactly 1 */
}
static void p5d_probe_tick(void) {
	if (probe_state == 1) {
		/* wait until the game actually processed the tap (its transition counter moved), not a fixed
		 * frame count: during the title->menu transition the press can be dropped */
		if (RD32(0x8153f9b4) == probe_g0 && ++probe_timer < 30) return;
		probe_timer = 0;
		probe_snap1 = (uint8_t *)malloc(P5D_PROBE_HI - P5D_PROBE_LO);
		probe_hit1 = (uint8_t *)calloc(((P5D_PROBE_HI - P5D_PROBE_LO) / 4), 1);
		if (!probe_snap1 || !probe_hit1) { probe_state = -1; return; }
		{ const uint8_t *cur = G(P5D_PROBE_LO);
		  memcpy(probe_snap1, cur, P5D_PROBE_HI - P5D_PROBE_LO);
		  for (uint32_t o = 0; o + 4 <= P5D_PROBE_HI - P5D_PROBE_LO; o += 4) {
			int32_t a, b; memcpy(&a, probe_snap + o, 4); memcpy(&b, cur + o, 4);
			/* the menu may restore the last selection (often the LAST row), and a DOWN there wraps: the
			 * index may step -5 as well as +1, so accept either direction/wrap in the first step */
			if (a >= 0 && a <= 10 && b >= 0 && b <= 10 && (b == a + 1 || b == a - 1 || b == a + 5 || b == a - 5)) probe_hit1[o / 4] = 1;
		  } }
		free(probe_snap); probe_snap = NULL;
		probe_g0 = RD32(0x8153f9b4);
		menu_tap(DOWN);   /* second scripted step: the index must increment again */
		probe_state = 4;
		return;
	}
	if (probe_state == 4) {
		if (RD32(0x8153f9b4) == probe_g0 && ++probe_timer < 30) return;
		probe_timer = 0; probe_n = 0;
		{ const uint8_t *cur = G(P5D_PROBE_LO);
		  for (uint32_t o = 0; o + 4 <= P5D_PROBE_HI - P5D_PROBE_LO && probe_n < 64; o += 4) {
			if (!probe_hit1[o / 4]) continue;
			int32_t a, b; memcpy(&a, probe_snap1 + o, 4); memcpy(&b, cur + o, 4);
			if (!(a >= 0 && a <= 10 && b >= 0 && b <= 10 && (b == a + 1 || b == a - 1 || b == a + 5 || b == a - 5))) continue;
			int nb = 0;
			for (int d = -4; d <= 4 && !nb; d++) { if (!d) continue; long q = (long)o / 4 + d; if (q >= 0 && q < (long)((P5D_PROBE_HI - P5D_PROBE_LO) / 4) && probe_hit1[q]) nb = 1; }
			if (!nb) probe_cand[probe_n++] = P5D_PROBE_LO + o;
		  } }
		free(probe_snap1); probe_snap1 = NULL;
		free(probe_hit1); probe_hit1 = NULL;
		probe_state = 2; probe_try = 0; probe_pass1 = 0;
		fprintf(stderr, "[UI] direct probe: %d isolated candidates\n", probe_n);
		return;
	}
	if (probe_state == 2) {
		if (probe_try >= probe_n) { probe_state = -1; fprintf(stderr, "[UI] direct probe failed; fallback closed loop\n"); return; }
		/* BOTH extremes must be reached: the menu can already sit at one extreme, so a single matching
		 * read is not evidence (it false-confirmed a candidate in an earlier run). Write DANCING (top),
		 * then CONFIG (bottom); only a real index moves the banner both ways. */
		int ph = probe_timer / 5;
		if (ph >= 2) { probe_timer = 0; probe_try++; probe_pass1 = 0; return; }
		WR32(probe_cand[probe_try], ph == 0 ? 1u : 5u);
		if ((++probe_timer % 5) != 0) return;
		if (ph == 0) probe_pass1 = (p5d_banner_y >= 200 && p5d_banner_y < 275);
		else if (probe_pass1 && p5d_banner_y > 400 && p5d_banner_y < 500) {
			ui_sel_addr = probe_cand[probe_try]; probe_state = 3; probe_timer = 0; probe_cal_row = 0;
			fprintf(stderr, "[UI] direct probe: selection index %#x confirmed, calibrating rows\n", ui_sel_addr);
		}
		return;
	}
	if (probe_state == 3) {
		/* one-time calibration: write each row and record where the banner settles, so the pointer->row
		 * table matches this session's list geometry instead of the nominal y values. */
		if (probe_cal_row >= 6) {
			ui_direct = 1; probe_state = 0;
			fprintf(stderr, "[UI] direct probe: calibrated banner y = %d %d %d %d %d %d\n",
			        probe_cal_y[0], probe_cal_y[1], probe_cal_y[2], probe_cal_y[3], probe_cal_y[4], probe_cal_y[5]);
			return;
		}
		WR32(ui_sel_addr, (uint32_t)(probe_cal_row + 1));
		if (++probe_timer < 6) return;
		probe_timer = 0;
		probe_cal_y[probe_cal_row] = p5d_banner_y;
		probe_cal_row++;
		return;
	}
}
static void p5d_menu_tick(void) {
	uint64_t now = now_us();
	if (pending_row >= 0) {
		int sel = p5d_menu_read();
		if (sel >= 0) {
			int row = pending_row; pending_row = -1;
			if (row == sel) { menu_enter = 1; menu_next_us = now; } else { menu_target = row; menu_next_us = now; }
		} else if (now > pending_until) pending_row = -1;
		return;
	}
	if (menu_tap_bits && now >= menu_tap_until) menu_tap_bits = 0;
	if (menu_enter) {
		if (p5d_ui == P5D_UI_SONG) song_confirm_us = now_us();   /* leaving the song list is the dance, not a submenu */
		if (!menu_tap_bits && now >= menu_next_us) {
				if (p5d_ui == P5D_UI_MAIN && p5d_ui_cur == 0) { p5d_song_latch = 1; p5d_song_mode = 1; p5d_song_flow_armed = 1; p5d_commu_mode = 0; p5d_pause_latch = 0; p5d_config_latch = 0; p5d_scene_set(P5D_SCENE_SONG, "DANCING row confirmed"); p5d_scene_lock = 1; p5d_song_ev_us = now_us(); }
				if (p5d_ui == P5D_UI_SONG) { song_confirm_us = now; p5d_scene_set(P5D_SCENE_DANCE, "song confirmed"); p5d_scene_lock = 0; p5d_song_mode = 0; p5d_config_latch = 0; }
				if (p5d_ui == P5D_UI_MAIN && p5d_ui_cur == 2) { p5d_song_flow_armed = 0; p5d_song_latch = 0; p5d_commu_latch = 1; p5d_commu_latch_us = now_us(); p5d_commu_mode = 1; p5d_song_mode = 0; p5d_scene_set(P5D_SCENE_COMMU, "COMMU row confirmed"); p5d_scene_lock = 2; }
				if (p5d_ui == P5D_UI_MAIN && p5d_ui_cur == 4) { p5d_config_latch = 1; p5d_song_flow_armed = 0; }   /* the CONFIG row: the settings page's Q/E are its D-pad */
				if (p5d_ui == P5D_UI_MAIN && p5d_ui_cur < 0) {   /* the model missed the row: use the game's own highlight */
					int rh = p5d_menu_row_hint();
					if (rh < 0) rh = p5d_banner_row();
					if (rh >= 0) { p5d_song_mode = rh == 0; p5d_commu_mode = rh == 2; p5d_song_latch = rh == 0; }
					if (rh == 0) { p5d_scene_set(P5D_SCENE_SONG, "DANCING row (code hint)"); p5d_scene_lock = 1; }
					else if (rh == 2) { p5d_scene_set(P5D_SCENE_COMMU, "COMMU row (code hint)"); p5d_scene_lock = 2; }
					else if (rh == 4) { p5d_config_latch = 1; p5d_song_flow_armed = 0; }   /* the CONFIG row: the settings page's D-pad */
				}
				if (p5d_ui == P5D_UI_MAIN && p5d_ui_cur >= 0) { p5d_song_mode = p5d_ui_cur == 0; p5d_commu_mode = p5d_ui_cur == 2; }
				else if (p5d_ui == P5D_UI_MAIN && p5d_commu_mode) { p5d_commu_mode = 0; }   /* another row was confirmed: the COMMU flow was left */
				fprintf(stderr, "[MODE] menu confirm cur=%d -> commu=%d song=%d\n", p5d_ui_cur, p5d_commu_mode, p5d_song_mode);
				if (p5d_ui == P5D_UI_MAIN && p5d_ui_cur == 0) p5d_song_latch = 1;   /* the DANCING row: the song list uses Q/E as its D-pad */
				menu_tap(CIRCLE); menu_next_us = now + 160000; menu_enter = 0; }   /* P5D: Circle confirms */
		return;
	}
	if (p5d_ui == P5D_UI_MAIN) { menu_target = -1; return; }   /* the main menu is driven by p5d_hover_tick */
	if (menu_target >= 0 && p5d_ui != P5D_UI_MAIN && p5d_ui != P5D_UI_SONG && p5d_ui != P5D_UI_RESULT) { menu_target = -1; nav_last_sel = -2; nav_stall = 0; }
	if (menu_target < 0 || menu_tap_bits || now < menu_next_us) return;
	int sel = p5d_menu_read();
	if (sel < 0) { static uint64_t bad_since; if (!bad_since) bad_since = now; else if (now - bad_since > 400000) { menu_target = -1; bad_since = 0; } return; }
	if (sel == menu_target) { fprintf(stderr, "[UI] nav reached row %d\n", sel); menu_target = -1; nav_last_sel = -2; nav_stall = 0; return; }
	if (sel != nav_last_sel) { nav_last_sel = sel; nav_stall = 0; } else if (++nav_stall > 6) { fprintf(stderr, "[UI] nav abort (row stuck at %d, target %d)\n", sel, menu_target); menu_target = -1; nav_last_sel = -2; nav_stall = 0; return; }
	menu_tap(sel < menu_target ? DOWN : UP);
	fprintf(stderr, "[UI] nav tap %s from %d to %d\n", sel < menu_target ? "DOWN" : "UP", sel, menu_target);
	menu_next_us = now + 240000;   /* the picture scan (~15 Hz while a menu is up) supplies the settled row */
}
static uint32_t p5d_macro_bits(uint64_t now) { return now < menu_tap_until ? menu_tap_bits : 0; }

int window_in_dance_stage(void);   /* defined below; the wheel guards ask whether the dance stage is up */

/* one wheel notch as the interaction layer sees it -- the same one-step move the physical wheel sends,
 * usable from RECOMP_UI_CMD ('w'/'W') and RECOMP_UI_TEST ("frame:w") so the menus can be verified
 * without a mouse. The RESULT screen moves its 4-row selection; MAIN/SONG move their lists. */
static void p5d_wheel_cmd(int dir) {
	if (!p5d_saw_menu || p5d_ui == P5D_UI_TITLE) return;
	if (p5d_ui == P5D_UI_RESULT) { menu_tap(dir > 0 ? UP : DOWN); menu_win_until = now_us() + 30000000; return; }
	if (p5d_ui == P5D_UI_MAIN || p5d_ui == P5D_UI_SONG || p5d_pause_latch || p5d_config_latch ||
	    (now_us() < menu_win_until && !(p5d_profile == P5D_GAME && !p5d_pause_latch && !p5d_config_latch))) { menu_tap(dir > 0 ? UP : DOWN); menu_win_until = now_us() + 30000000; return; }
	/* an unmodelled submenu page (SYSTEM's DLC / CROSS SAVE, the COLLECTION pages, ...): the page's own steps are
	 * the D-pad's up/down, so the wheel sends those too - never in the dance stage (notes) or a story scene */
	if (p5d_ui == P5D_UI_NONE && p5d_profile == P5D_MENU && !window_in_dance_stage() &&
	    !(p5d_story_us && now_us() - p5d_story_us < 2000000)) { menu_tap(dir > 0 ? UP : DOWN); menu_win_until = now_us() + 30000000; }
}

/* a title/cutscene click: the game is not showing a list, so a left click is the game's "any button"
 * (P5D skips its movies with Cross/Start; the title takes Start). A tap of its own, because the click
 * is never a touch hold: through p5d_mouse_bits it was masked by touch_down and its release cleared
 * the bits in the same event batch, so the game often never saw the click. */
static void p5d_click_advance(void) {
	/* per-screen semantics, not a blanket mapping: at the title Cross is the "back to the opening movie"
	 * button (a click with Cross in the tap sent the user back there), so a title click is START only.
	 * Everywhere else (movies, story scenes, dialogue) the click is the game's advance/skip: START and
	 * Circle, never Cross, so a click can never walk backwards out of the scene. */
	/* TITLE: a single short START press -- a long hold would still be down when the menu takes over and the
	 * menu would count it as a fresh press (and act on it). Movies/dialogue: a longer START|CIRCLE hold. */
	int title = (p5d_ui == P5D_UI_TITLE);
	uint32_t bits = title ? START : (START | CIRCLE);
	menu_tap_bits = bits; menu_tap_until = now_us() + (title ? 45000 : 150000);
	fprintf(stderr, "    click: advance tap %#x (screen %s)\n", bits, p5d_ui_name());
}


/* ---- live state line: what the host's UI layer thinks the game shows, every few presented frames
 * (RECOMP_UI_LOG=0 turns it off). This is the instrumentation the port is debugged through: the game's
 * screen/row, the pointer's row, the navigation state -- no screenshots needed. */
static int ui_last_hover = -1;
static int ui_log(void) {
	static int on = -1; if (on < 0) on = getenv("RECOMP_UI_LOG") ? atoi(getenv("RECOMP_UI_LOG")) != 0 : 1;
	static uint64_t lastf;
	extern uint64_t display_frames(void); uint64_t f = display_frames();
	if (!on || f - lastf < 10) return 0; lastf = f;
	const char *scr = p5d_ui_name();
	fprintf(stderr, "[UI] f=%llu scr=%s cur=%d rows=%d hover=%d target=%d enter=%d pending=%d saw=%d tpl=%s:%.2f by=%d ptr=%d g=%u/%u/%u\n",
	        (unsigned long long)f, scr, p5d_ui_cur, p5d_ui_n, ui_last_hover, menu_target, menu_enter, pending_row, p5d_saw_menu, p5d_ui_tname, p5d_ui_score,
	        p5d_banner_y, ui_hover_on ? ui_hover_y : -1, RD32(0x8153f9b4), RD32(0x81557d80), RD32(0x813b017c));
}
/* ---- RECOMP_UI_TEST="frame:action,...": drive the PC interaction layer itself, as a click/hover would,
 * for headless-style verification of the menu logic (no OS mouse, no screenshots):
 *   h<row>  hover row (0-based)   c  click at the hovered row   b  back (Cross)
 * The [UI] state lines then show what the layer did. */
/* RECOMP_UI_CMD=<file>: a live command channel for the interaction layer. The window loop reads the file
 * every iteration; a non-empty first line is executed and the file is cleared (the orchestrator writes one
 * command, waits for the [UI] log to show the result, then writes the next). Actions, the same vocabulary as
 * RECOMP_UI_TEST: d/u = a macro D-pad tap, e = confirm, b = back, h<row> = hover a row, c = click the hovered
 * row. This is how the submenus and the song list are captured without depending on window focus. */
static void ui_cmd_tick(void) {
	static const char *path; static int inited;
	if (!inited) { inited = 1; path = getenv("RECOMP_UI_CMD"); }
	if (!path) return;
	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[32] = {0};
	if (!fgets(line, sizeof line, f)) { fclose(f); return; }
	fclose(f);
	/* consume the command: truncate the file */
	FILE *w = fopen(path, "wb"); if (w) fclose(w);
	char c = line[0];
	if (c == 'd') { menu_tap(DOWN); fprintf(stderr, "[UI] cmd=d (tap down)\n"); }
	else if (c == 'u') { menu_tap(UP); fprintf(stderr, "[UI] cmd=u (tap up)\n"); }
	else if (c == 'e') { menu_enter = 1; menu_next_us = now_us(); fprintf(stderr, "[UI] cmd=e (confirm)\n"); }
	else if (c == 'b') { menu_tap(CROSS); fprintf(stderr, "[UI] cmd=b (back)\n"); }
	else if (c == 'q') { menu_tap(CIRCLE); fprintf(stderr, "[UI] cmd=q (tap circle)\n"); }
	else if (c == 'p') { menu_tap(START); fprintf(stderr, "[UI] cmd=p (tap start)\n"); }
	else if (c == 'h') { int row = atoi(line + 1); ui_last_hover = row; { int sel = p5d_menu_read(); if (sel >= 0) { p5d_menu_sel = sel; if (row != sel && menu_target < 0 && !menu_enter) { menu_enter = 0; menu_target = row; menu_next_us = now_us(); } } fprintf(stderr, "[UI] cmd=h%d (hover row %d, sel %d)\n", row, row, sel); } }
	else if (c == 'c') { int sel = p5d_menu_read(), row = ui_last_hover; if (sel >= 0 && row >= 0) { if (row == sel) { menu_enter = 1; menu_next_us = now_us(); } else { menu_enter = 0; menu_target = row; menu_next_us = now_us(); } fprintf(stderr, "[UI] cmd=c (click row %d, sel %d)\n", row, sel); } }
	else if (c == 'w') { p5d_wheel_cmd(1); fprintf(stderr, "[UI] cmd=w (wheel up)\n"); }
	else if (c == 'W') { p5d_wheel_cmd(-1); fprintf(stderr, "[UI] cmd=W (wheel down)\n"); }
	else if (c == 's') { extern void gfxcap_request(void); gfxcap_request(); fprintf(stderr, "[UI] cmd=s (capture)\n"); }
	/* direct shoulder/trigger probes: one macro tap, whatever the key map says (the COMMU character-switch test) */
	else if (c == 'L') { menu_tap(LTRIG); fprintf(stderr, "[UI] cmd=L (tap L / shoulder)\n"); }
	else if (c == 'R') { menu_tap(RTRIG); fprintf(stderr, "[UI] cmd=R (tap R / shoulder)\n"); }
	else if (c == '1') { menu_tap(L1); fprintf(stderr, "[UI] cmd=1 (tap L1)\n"); }
	else if (c == '2') { menu_tap(R1); fprintf(stderr, "[UI] cmd=2 (tap R1)\n"); }
	else if (c == '3') { menu_tap(PAD_L2); fprintf(stderr, "[UI] cmd=3 (tap L2)\n"); }
	else if (c == '4') { menu_tap(PAD_R2); fprintf(stderr, "[UI] cmd=4 (tap R2)\n"); }
	else if (c == '<') { menu_tap(LEFT); fprintf(stderr, "[UI] cmd=< (tap LEFT)\n"); }
	else if (c == '>') { menu_tap(RIGHT); fprintf(stderr, "[UI] cmd=> (tap RIGHT)\n"); }
}
static void ui_test_tick(void) {
	static int parsed; static struct { long f; char a[8]; } st[64]; static int n;
	if (!parsed) {
		parsed = 1; const char *e = getenv("RECOMP_UI_TEST");
		for (const char *q = e; q && *q && n < 64; ) { long f; char a[8] = {0}; if (sscanf(q, "%ld:%7s", &f, a) != 2) break; st[n].f = f; snprintf(st[n].a, sizeof st[n].a, "%s", a); n++; q = strchr(q, ','); if (q) q++; }
	}
	extern uint64_t display_frames(void); uint64_t f = display_frames();
	static uint64_t acted_frame;
	if (f == acted_frame) return;   /* one action per presented frame (the window loop runs several times per frame) */
	for (int i = 0; i < n; i++) if ((long)f == st[i].f) { acted_frame = f;
		char c = st[i].a[0];
		if (c == 'h') {
			int row = atoi(st[i].a + 1);
			ui_last_hover = row;
			int sel = p5d_menu_read();
			if (sel >= 0) { p5d_menu_sel = sel; if (row != sel && menu_target < 0 && !menu_enter) { menu_enter = 0; menu_target = row; menu_next_us = now_us(); } }
			fprintf(stderr, "[UI] f=%llu action=hover row=%d sel=%d\n", (unsigned long long)f, row, sel);
		} else if (c == 'c') {
			int sel = p5d_menu_read();
			int row = ui_last_hover;
			if (sel >= 0 && row >= 0) {
				if (row == sel) { menu_enter = 1; menu_next_us = now_us(); }
				else { menu_enter = 0; menu_target = row; menu_next_us = now_us(); }
				fprintf(stderr, "[UI] f=%llu action=click row=%d sel=%d -> %s\n", (unsigned long long)f, row, sel, row == sel ? "enter" : "navigate");
			} else fprintf(stderr, "[UI] f=%llu action=click row=%d sel=%d (no UI)\n", (unsigned long long)f, row, sel);
		} else if (c == 'b') { menu_tap(CROSS); fprintf(stderr, "[UI] f=%llu action=back\n", (unsigned long long)f); }
		else if (c == 'w') { p5d_wheel_cmd(1); fprintf(stderr, "[UI] f=%llu action=wheel-up\n", (unsigned long long)f); }
		else if (c == 'W') { p5d_wheel_cmd(-1); fprintf(stderr, "[UI] f=%llu action=wheel-down\n", (unsigned long long)f); }
		else if (c == 'd') { menu_tap(DOWN); fprintf(stderr, "[UI] f=%llu action=tap-down\n", (unsigned long long)f); }
		else if (c == 'u') { menu_tap(UP); fprintf(stderr, "[UI] f=%llu action=tap-up\n", (unsigned long long)f); }
		else if (c == 'e') { menu_enter = 1; menu_next_us = now_us(); fprintf(stderr, "[UI] f=%llu action=tap-enter\n", (unsigned long long)f); }
	}
}


/* the GPU present asks this (gfx_gpu.c) and the window thread checks it: is the screen model being
 * driven/read right now (then the presented frame must be synced into guest memory every present) */
static int ui_interacting(void) { return menu_target >= 0 || menu_enter || pending_row >= 0; }
int window_ui_frame_wanted(void) {
	static uint64_t n;
	/* every wanted frame costs a GPU->CPU download of the presented picture (a fence wait, ~1-2 ms on the render
	 * thread): keep the rate as low as the screen model can bear (a soft stutter came from this at 2x frames,
	 * reported 2026-11-07) */
	if (ui_interacting()) return 1;                  /* a navigation/hover/click is in flight */
	if (p5d_ui != P5D_UI_NONE) return (++n % 3) == 0;
	if (last_song_us && now_us() - last_song_us < 6000000) return (++n % 3) == 0;   /* the song list just left/flickered: stay fresh (Q/E, profiles) */
	if (p5d_saw_menu) return ++n % 4 == 0;
	return ++n % 12 == 0;
}

static int last_device_kb = 1;   /* which input device drives the on-screen button glyphs (keyboard letters vs pad icons) */
int window_uses_keyboard(void) { return last_device_kb; }
int window_in_dance_stage(void) { return p5d_profile == P5D_GAME; }
int window_in_config_screen(void) { return p5d_profile == P5D_CONFIG; }
extern void gfx_keyglyph_refresh(void);
extern void gfx_hintglyph_refresh(void);
static void device_keyboard(void) { if (!last_device_kb) { last_device_kb = 1; gfx_keyglyph_refresh(); gfx_hintglyph_refresh(); } }
static void device_gamepad(void) { if (last_device_kb) { last_device_kb = 0; gfx_keyglyph_refresh(); gfx_hintglyph_refresh(); } }
static int p5d_key_event(SDL_Keycode k, int dn) {
	int hit = 0, menu_key = 0;
	/* M1524: the COLLECTION garment screen's own keys, by code-level flow depth (not the picture): Enter = START
	 * (watch mode), Ctrl = TRIANGLE (random outfit). Everywhere else these keys keep their normal meaning. */
	{ static int raw_env = -1; if (raw_env < 0) { const char *e = getenv("RECOMP_WATCH_RAW"); raw_env = e && atoi(e) != 0; }
	  if (raw_env) goto p5d_watch_raw_skip; }
	if (p5d_collection_active() && p5d_collection_watching) {
		{ static int wl = -1; if (wl < 0) wl = getenv("RECOMP_WATCH_LOG") ? 1 : 0; if (wl) fprintf(stderr, "[WATCHKEY] %s dn=%d\n", SDL_GetKeyName(k), dn); }
		int idx = -1;
		if (k == SDLK_W) idx = 0;
		else if (k == SDLK_A) idx = 1;
		else if (k == SDLK_S) idx = 2;
		else if (k == SDLK_D) idx = 3;
		else if (k == SDLK_UP) idx = 4;
		else if (k == SDLK_DOWN) idx = 5;
		else if (k == SDLK_LEFT) idx = 6;
		else if (k == SDLK_RIGHT) idx = 7;
		if (idx >= 0) { p5d_watch_keys[idx] = dn ? 1 : 0; return 1; }
		if (dn && (k == SDLK_LALT || k == SDLK_RALT)) { menu_tap(SELECT); fprintf(stderr, "[WATCH] Alt -> SELECT (reset)\n"); return 1; }
	}
	p5d_watch_raw_skip:
	if (!dn && k == SDLK_RETURN && p5d_collection_active()) { extern int p5d_watch_enter_up_clear(void); p5d_watch_enter_up_clear(); }
	if (dn && p5d_collection_active()) {
		int viewer = p5d_collection_depth >= 3;
		if (viewer && k == SDLK_RETURN) {
			/* one press = one toggle: only a fresh Enter (released since the last one) flips the watch mode, so a
			 * held key or a duplicated down can never turn it on and off again */
			if (!p5d_watch_enter_held) {
				p5d_watch_enter_held = 1;
				p5d_collection_watching = !p5d_collection_watching; menu_tap(START);
				fprintf(stderr, "[KEY] collection costume: Enter -> START (watch %s)\n", p5d_collection_watching ? "on" : "off");
			}
			return 1;
		}
		if (viewer && (k == SDLK_LCTRL || k == SDLK_RCTRL)) { menu_tap(TRIANGLE); fprintf(stderr, "[KEY] collection costume: Ctrl -> TRIANGLE (random)\n"); return 1; }
		if (!viewer && (k == SDLK_RETURN || k == SDLK_SPACE) && p5d_collection_depth < 3) { p5d_collection_depth++; p5d_collection_watching = 0; }
		if (k == SDLK_BACKSPACE) p5d_collection_note_back();
	}
	for (int pr = 0; pr < P5D_PROFILES; pr++)
		for (int i = 0; i < n_p5d_keys[pr]; i++) if (p5d_keys[pr][i].kc == k) { p5d_keys[pr][i].held = dn; hit = 1; if (pr == P5D_MENU) menu_key = 1; }
	/* M1627: Q is in the menu map as L and in the dance map as up. While the dance map is active it is a dance key: it
	 * must not count as a menu key, or the "menu key while the dance flow is idle" path below switched the profile back to
	 * the menu map, cleared the held state and made Q dead as the up direction (reported: Q fails in the dance stage; the
	 * session log showed "input: profile menu/story" right at a Q press with dnow=0 latch=0). */
	if (p5d_profile == P5D_GAME && p5d_key_bound(P5D_GAME, k)) menu_key = 0;
	if (hit) p5d_recompute();
	if (dn && p5d_profile != P5D_GAME && !p5d_pause_latch && !p5d_config_latch && p5d_ui == P5D_UI_NONE && p5d_saw_menu &&
	    (p5d_scene == P5D_SCENE_DANCE || p5d_scene == P5D_SCENE_SONG) &&   /* the code-level scene, not the picture */
	    !(p5d_story_us && now_us() - p5d_story_us < 1500000) && !(p5d_commu_screen_us && now_us() - p5d_commu_screen_us < 1500000)) {
		int in_game = 0, in_menu = 0;
		for (int i = 0; i < n_p5d_keys[P5D_GAME]; i++) if (p5d_keys[P5D_GAME][i].kc == k) in_game = 1;
		for (int i = 0; i < n_p5d_keys[P5D_MENU]; i++) if (p5d_keys[P5D_MENU][i].kc == k) in_menu = 1;
		if (in_game && (!in_menu || p5d_scene == P5D_SCENE_DANCE)) {   /* a dance key with the dance stage up: the profile was left behind */
			fprintf(stderr, "[KEY] dance profile self-heal on %s (was profile %d)\n", SDL_GetKeyName(k), p5d_profile);
			p5d_switch_profile(P5D_GAME); p5d_dance_latch = 1; p5d_dance_flow_start(now_us());
			/* p5d_switch_profile clears every held flag: put this press back in the dance map, or the key that healed the
			 * profile would itself be a no-op for its whole hold */
			for (int i = 0; i < n_p5d_keys[P5D_GAME]; i++) if (p5d_keys[P5D_GAME][i].kc == k) p5d_keys[P5D_GAME][i].held = 1;
			p5d_recompute();
		}
	}
	if (dn && !p5d_song_flow_armed && p5d_profile == P5D_MENU && (p5d_scene == P5D_SCENE_MAIN || p5d_ui == P5D_UI_MAIN) && p5d_banner_row() == 0) p5d_song_flow_armed = 1;
	if (dn && p5d_song_flow_armed && p5d_ui != P5D_UI_MAIN && p5d_song_ev_us && now_us() - p5d_song_ev_us < 1500000)
		/* a keyboard confirm in the song list leaves towards the dance (the picture model may still read MAIN/NONE
		 * on the song list: the song flow flag and the SONG scene make the confirm recognisable anyway) */
		for (int i = 0; i < n_p5d_keys[P5D_MENU]; i++)
			if (p5d_keys[P5D_MENU][i].kc == k && p5d_keys[P5D_MENU][i].bit == CIRCLE) { song_confirm_us = now_us(); p5d_dance_flow_start(now_us()); break; }
	if (dn && menu_key) menu_win_until = now_us() + 30000000;   /* a submenu page keeps its key map while it is used */
	if (dn && (k == SDLK_BACKSPACE || k == SDLK_ESCAPE) && p5d_profile == P5D_SONG) { p5d_song_mode = 0; p5d_song_latch = 0; p5d_song_flow_armed = 0; p5d_scene_set(P5D_SCENE_MAIN, "song list back"); }   /* back out of the song list: its Q/E D-pad is over */
	if (dn && (k == SDLK_BACKSPACE || k == SDLK_ESCAPE) && (p5d_scene == P5D_SCENE_MAIN || p5d_scene == P5D_SCENE_COMMU)) { p5d_song_mode = 0; p5d_song_latch = 0; }
	if (dn && p5d_profile == P5D_GAME && menu_key && !p5d_dance_flow(now_us()) && !song_confirm_us && !p5d_story_latch && !(p5d_dance_bar_us && now_us() - p5d_dance_bar_us < 600000))
		p5d_switch_profile(P5D_MENU);   /* a menu key on a submenu page whose hold expired while idle (never while the dance gauge is up, nor in a story scene: there Esc is START) */
	if (dn) {   /* [KEYDBG] TEMP diagnostic (M1509): every key down the game path sees, with the state the dance keys depend on */
		int dnow = p5d_dance_bar_us && now_us() - p5d_dance_bar_us < 600000;
		fprintf(stderr, "[KEYDBG] key=%s hit=%d prof=%d sm=%d dnow=%d latch=%d story=%d ui=%s scene=%d\n",
		        SDL_GetKeyName(k), hit, p5d_profile, p5d_song_mode, dnow, p5d_dance_latch, p5d_story_latch, p5d_ui_name(), p5d_scene);
	}
	if (dn && p5d_profile != P5D_GAME) {
		if (k == SDLK_DOWN) p5d_nav_step(+1);
		else if (k == SDLK_UP) p5d_nav_step(-1);
	}
	if (dn) device_keyboard();
	if (p5d_profile == P5D_GAME && dn) {
		fprintf(stderr, "[KEY] game key %s\n", SDL_GetKeyName(k));
		if (k == SDLK_ESCAPE) menu_win_until = now_us() + 30000000;   /* the pause menu just opened: wheel/click act on it */
		else if (k != SDLK_RETURN) menu_win_until = 0;                /* a dance input means gameplay: no stray menu events */
	} else if (dn && hit && p5d_profile != P5D_GAME) {
		/* RECOMP_KEY_LOG=1: the menu profile's key presses, so a scripted test can prove a key's mapping
		 * (e.g. Tab -> the Square button / the song list's sort) without reading the screen) */
		static int kl = -1; if (kl < 0) kl = getenv("RECOMP_KEY_LOG") ? atoi(getenv("RECOMP_KEY_LOG")) : 0;
		if (kl) fprintf(stderr, "[KEY] menu key %s\n", SDL_GetKeyName(k));
	}
	return hit;
}


/* ---- aspect modes (the overlay's Display page, settings key "aspect"): how the Vita's 960x544
 * picture meets a window of any other shape. fit (today's default): letterbox, the whole picture
 * visible, bars where it doesn't reach. stretch: fills the window, non-uniform scale, no bars, the
 * picture distorted. fill: uniform scale to cover the window with no bars, cropping whatever
 * overhangs on one axis, centred. One function computes both rects (the window.c present loop's
 * blit source/destination AND mouse_to_touch's inverse of it) so a touch always lands under the
 * pixel it looks like it's under, in every mode -- see present_rect's own comment. */
enum { ASPECT_FIT, ASPECT_STRETCH, ASPECT_FILL };
static int aspect_mode = ASPECT_FIT;
static const char *aspect_name(int m) { return m == ASPECT_STRETCH ? "stretch" : m == ASPECT_FILL ? "fill" : "fit"; }
int window_get_aspect(void) { return aspect_mode; }
void window_set_aspect(int mode) {
	if (mode < ASPECT_FIT || mode > ASPECT_FILL) return;
	aspect_mode = mode;
	settings_set_str("aspect", aspect_name(mode)); settings_save();
}
int window_get_fullscreen(void) { return win && (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) != 0; }
void window_set_fullscreen(int on) {
	if (win) SDL_SetWindowFullscreen(win, on != 0);
	settings_set_int("fullscreen", on != 0); settings_save();
}
/* ---- wider aspect ratios (settings "aspect_ratio": native | 21:9 | 32:9 | window): the factor k the game's
 * horizontal view is widened by -- its camera hook (hooks.c, aspect_widen) builds the projection that
 * much wider into the same 960x544 frame, and the window shows that frame k times wider than native.
 * Only for a game whose conf declares such a hook (RECOMP_ASPECT_HOOK=1 in its RUN_ENV): without one the
 * picture would only be stretched. 1 = native (960:544, 1.765:1). */
static volatile uint32_t win_pw, win_ph;   /* the swapchain's size in pixels, as the present loop last saw it */
int window_aspect_supported(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_ASPECT_HOOK"); v = e && *e == '1'; } return v; }
float window_aspect_widen(void) {
	if (!window_aspect_supported()) return 1.0f;
	static const char *env = (const char *)-1; if (env == (const char *)-1) env = getenv("RECOMP_ASPECT");   /* (read once: this runs per draw) */
	char b[16]; if (env) snprintf(b, sizeof b, "%s", env); else settings_str("aspect_ratio", "native", b, sizeof b);   /* RECOMP_ASPECT wins (headless tests) */
	const float native = 960.0f / 544.0f;
	float a = !strcmp(b, "21:9") ? 21.0f / 9.0f : !strcmp(b, "32:9") ? 32.0f / 9.0f : 0;
	if (!strcmp(b, "window") && win_pw && win_ph) a = (float)win_pw / (float)win_ph;
	float k = a > 0 ? a / native : 1.0f;
	return k < 1.02f ? 1.0f : k > 2.5f ? 2.5f : k;   /* narrower than native: native (letterboxed as before) */
}
/* the visible source sub-rect (sx,sy,sw,sh: fit/stretch show it all; fill crops it, centred, to the
 * window's own aspect) and where it lands in the window (dx,dy,dw,dh) -- source and window sizes in
 * any consistent unit (window points for mouse_to_touch's 960x544; actual texture pixels, which may
 * be render-scaled, for the present loop's blit): the aspect ratio is the same either way (960:544),
 * so both callers get the same rects as fractions of their own inputs. */
static void present_rect(float ww, float wh, float sw, float sh,
                          float *sx, float *sy, float *sw_out, float *sh_out,
                          float *dx, float *dy, float *dw, float *dh) {
	switch (aspect_mode) {
	case ASPECT_STRETCH:
		*sx = 0; *sy = 0; *sw_out = sw; *sh_out = sh;
		*dx = 0; *dy = 0; *dw = ww; *dh = wh;
		break;
	case ASPECT_FILL: {
		float wa = ww / wh, sa = (960.0f / 544.0f) * window_aspect_widen();   /* the picture's shape, whatever the frame's pixel ratio (gfx_gpu.c may render it wider) */
		if (wa > sa) { *sw_out = sw; *sh_out = sh * sa / wa; *sx = 0; *sy = (sh - *sh_out) * 0.5f; }
		else { *sh_out = sh; *sw_out = sw * wa / sa; *sy = 0; *sx = (sw - *sw_out) * 0.5f; }
		*dx = 0; *dy = 0; *dw = ww; *dh = wh;
		break;
	}
	default: {   /* ASPECT_FIT: the whole frame, letterboxed, shown window_aspect_widen() times wider than native */
		float ew = sh * (960.0f / 544.0f) * window_aspect_widen();   /* the picture's width at this height: its shape, not the frame's pixel ratio (gfx_gpu.c may render it wider) */
		float s = ww / ew < wh / sh ? ww / ew : wh / sh;
		*sx = 0; *sy = 0; *sw_out = sw; *sh_out = sh;
		*dw = ew * s; *dh = sh * s; *dx = (ww - *dw) * 0.5f; *dy = (wh - *dh) * 0.5f;
	}
	}
}
/* the mouse (window points) as a front-panel position, through present_rect's own picture (fit,
 * stretch or filled and cropped). Outside the visible picture: 0, unless clamping a drag to its
 * edge. present_rect's source rect is in 960x544 "screen" points; the touch panel is exactly double
 * that in both axes (1920x1088), as the hardware's panel is to its screen. */
static int mouse_to_touch(float mx, float my, int clamp, int *tx, int *ty) {
	int w = 0, h = 0; SDL_GetWindowSize(win, &w, &h);
	if (w <= 0 || h <= 0) return 0;
	float sx, sy, sw, sh, dx, dy, dw, dh;
	present_rect((float)w, (float)h, 960.f, 544.f, &sx, &sy, &sw, &sh, &dx, &dy, &dw, &dh);
	float u = (mx - dx) / dw, v = (my - dy) / dh;
	if (!clamp && (u < 0 || u >= 1 || v < 0 || v >= 1)) return 0;
	u = u < 0 ? 0 : u > 0.9999f ? 0.9999f : u; v = v < 0 ? 0 : v > 0.9999f ? 0.9999f : v;
	float px = sx + u * sw, py = sy + v * sh;   /* a point in the 960x544 screen, inside the visible crop */
	*tx = (int)(px * 2.f); *ty = (int)(py * 2.f);
	return 1;
}

/* the inverse, for drawing a touch where it lands: a panel point (front coordinates; the rear panel's
 * are the same space) to window points */
static void touch_to_point(int tx, int ty, float *x, float *y) {
	int w = 0, h = 0; SDL_GetWindowSize(win, &w, &h);
	float sx, sy, sw, sh, dx, dy, dw, dh;
	present_rect((float)w, (float)h, 960.f, 544.f, &sx, &sy, &sw, &sh, &dx, &dy, &dw, &dh);
	*x = dx + (tx * 0.5f - sx) / sw * dw; *y = dy + (ty * 0.5f - sy) / sh * dh;
}

/* ---- the Controls page (read-only): the GAME's controls -- the mappings kb_binds (the defaults; rebindable)/pad_bit (below) and
 * mouse_to_touch (above) actually implement, kept here as one small table so the overlay can show them
 * without reaching into this file's own logic. Keep in sync with those if a binding changes. The host's
 * own hotkeys (F1, F2, F10, F12, Guide, R3, the menu bar) are overlay.cpp's shortcuts table: keep that in
 * sync with the event loop below. */
static const char *const ctrl_rows[][3] = {
	{"D-pad", "Arrow keys", "D-pad"},
	{"Cross / Circle / Square / Triangle", "Space / C / F / E", "Cross / Circle / Square / Triangle (Xbox: A / B / X / Y)"},
	{"Start / Select", "Enter / Tab", "Options / Share or Create (Xbox: Menu / View)"},
	{"L / R", "Right mouse / left mouse (or Q / R)", "L2 / R2 (Xbox: LT / RT)"},
	{"Left / right stick", "WASD / the mouse", "Left / right stick"},
	{"Front touch panel", "Left mouse button (click, drag); hold Alt first while the mouse looks; a swipe prompt: just drag; a two-finger turn: hold and drag down / up; a touchscreen: touch the picture", "Touchpad; or hold L1: a stick + Cross / L2 / R2 (both sticks: two fingers); a two-finger turn: both sticks out, rolled"},
	{"Rear touch panel", "Right mouse button (click, drag); hold Alt first while the mouse looks; a touchscreen: touch with R1 held", "Touchpad with R1 held; or hold R1: a stick + Cross / L2 / R2 (both sticks: two fingers)"},
	{"Motion sensor (tilt, aim)", "--", "Gyro (DS4/DualSense); R3 recentres"},
	{"A PS TV layout's L2 / R2 / L3 / R3 (controller only)", "--", "L1 / R1 / L3 / R3"},
};
/* the pad column with "shoulder_buttons" = bumpers (the bumpers are L and R, the triggers hold the touch cursor) */
static const char *const ctrl_rows_swapped[][2] = {
	{"L / R", "L1 / R1 (Xbox: LB / RB)"},
	{"Front touch panel", "Touchpad; or hold L2: a stick + Cross / L1 / R1 (both sticks: two fingers); a two-finger turn: both sticks out, rolled"},
	{"Rear touch panel", "Touchpad with R2 held; or hold R2: a stick + Cross / L1 / R1 (both sticks: two fingers)"},
	{"A PS TV layout's L2 / R2 / L3 / R3 (controller only)", "L2 / R2 / L3 / R3"},
};
extern int display_pstv_looked(void);
int window_controls_count(void) { return (int)(sizeof ctrl_rows / sizeof ctrl_rows[0]) - !display_pstv_looked(); }   /* (the last row: a PS TV layout's, only in a game that has one) */
void window_controls_row(int i, const char **action, const char **key, const char **pad) {
	if (i < 0 || i >= window_controls_count()) { *action = *key = *pad = ""; return; }
	*action = ctrl_rows[i][0]; *key = ctrl_rows[i][1]; *pad = ctrl_rows[i][2];
	if (shoulders_swapped) for (size_t j = 0; j < sizeof ctrl_rows_swapped / sizeof ctrl_rows_swapped[0]; j++) if (!strcmp(*action, ctrl_rows_swapped[j][0])) *pad = ctrl_rows_swapped[j][1];
}
int window_touch(int *x, int *y) { *x = touch_x; *y = touch_y; return touch_down; }
/* A game hook's word that the game wants a front-panel swipe now (Killzone: a brutal melee / interrogation gesture window):
 * with the mouse captured (keyboard + mouse), the left button then drags a touch instead of firing -- from the panel's
 * centre, moved by the mouse's motion -- so a quick drag lands inside the window. Without it a swipe meant Alt for the
 * cursor, finding the spot, then dragging, and the window was over (found in testing, 2026-10-19). Valid for 200 ms a call. */
static uint64_t gesture_hint_ns;
void window_touch_gesture_hint(void) { __atomic_store_n(&gesture_hint_ns, SDL_GetTicksNS(), __ATOMIC_RELAXED); }
/* (never in a PS TV layout: no touch screen there, and its prompts want the stick itself, which a swipe would have muted) */
extern int display_pstv_active(void);
static int gesture_wanted(void) { uint64_t t = __atomic_load_n(&gesture_hint_ns, __ATOMIC_RELAXED); return t && SDL_GetTicksNS() - t < 200000000ull && !display_pstv_active(); }
static int gesture_drag;   /* the left button is dragging a touch (gesture_wanted when it went down) */
/* ... and with a pad: while the window is open a flick of the right stick is a swipe that way -- a touch from the panel's centre
 * 450 points out over 80 ms (the stick muted for the game meanwhile). Pressing a bumper, moving the cursor and dragging with
 * Cross never fit a 0.8 s interrogation window (found in testing, 2026-10-20). Another flick needs the stick back near the centre. */
static volatile int flick_down, flick_x, flick_y;
static uint64_t gesture_t0;
/* A game hook's word that the game wants a two-finger twist now (Killzone: a valve or an explosive being planted -- two
 * touches anywhere on the front panel, turned by how much the line between them turns). The pad: each stick is a thumb
 * on the wheel -- its finger sits out from the panel's centre the way the stick points, so both sticks pushed apart and rolled
 * the same way turn the line between them (one stick rolled around the other at rest does too); the fingers are down while
 * either stick is out, and the game sees the sticks centred meanwhile. Keyboard + mouse: the left button grips (two fingers
 * either side of the centre) and dragging down turns them clockwise, up anticlockwise; the camera stays. Settled on this
 * design, 2026-10-20 (two cursors with a press held made rolling them around each other too fiddly). Valid for 200 ms a call. */
static uint64_t twist_hint_ns;
void window_touch_twist_hint(void) { __atomic_store_n(&twist_hint_ns, SDL_GetTicksNS(), __ATOMIC_RELAXED); }
static int twist_wanted(void) { uint64_t t = __atomic_load_n(&twist_hint_ns, __ATOMIC_RELAXED); return t && SDL_GetTicksNS() - t < 200000000ull && !display_pstv_active(); }   /* (nor this) */
static volatile int twist_down, twist_x[2], twist_y[2];
static int twist_drag, twist_pad;   /* the left button / the sticks hold the twist's fingers down */
static float twist_ang;             /* the left button's twist: radians, clockwise on the panel */
static void twist_place(float a) {  /* the left button's two fingers, 300 points either side of the centre at angle a */
	const int dx = (int)lroundf(cosf(a) * 300), dy = (int)lroundf(sinf(a) * 300);
	twist_x[0] = 960 - dx; twist_y[0] = 544 - dy; twist_x[1] = 960 + dx; twist_y[1] = 544 + dy;
}
/* every contact on a panel now (port 0 front, 1 rear), each with a key that stays the same while
 * it's held (display.c gives each new key a new report id): 1 the mouse, 2 / 3 the bumper cursors, 4 a flick,
 * 5 / 6 a twist, 10+n touchpad finger n, 20+n touchscreen finger n. Returns how many, at most `max`. */
static int last_touch_key[2];   /* the input behind each panel's latest contact (window_last_touch_key) */
int window_last_touch_key(int port) { return last_touch_key[port & 1]; }
int window_touch_contacts(int port, int *keys, int *xs, int *ys, int max) {
	int n = 0;
	#define ADD(k, x, y) do { if (n < max) { keys[n] = (k); xs[n] = (x); ys[n] = (y); n++; } } while (0)
	if (port == 0 && touch_down) ADD(1, touch_x, touch_y);
	if (port == 1 && mouse_rear_down) ADD(1, mouse_rear_x, mouse_rear_y);
	if (cur_down && cur_mode == port + 1) ADD(2, cur_x[port], cur_y[port]);
	if (cur_down && cur2_on && cur_mode == port + 1) ADD(3, cur2_x[port], cur2_y[port]);   /* (the second cursor: key 3) */
	if (port == 0 && flick_down) ADD(4, flick_x, flick_y);   /* (a right-stick flick's swipe: key 4) */
	if (port == 0 && twist_down) { ADD(5, twist_x[0], twist_y[0]); ADD(6, twist_x[1], twist_y[1]); }   /* (a twist's two fingers: keys 5, 6) */
	for (int i = 0; i < TP_FINGERS; i++) if (tp_down[i] && tp_port[i] == port) ADD(10 + i, tp_x[i], tp_y[i]);
	for (int i = 0; i < ST_FINGERS; i++) if (st_down[i] && st_port[i] == port) ADD(20 + i, st_x[i], st_y[i]);   /* (a touchscreen's fingers: keys 20+) */
	#undef ADD
	if (n && port >= 0 && port < 2) last_touch_key[port] = keys[0];
	return n;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int touchpad_active(void) { for (int i = 0; i < TP_FINGERS; i++) if (tp_down[i]) return 1; return 0; }
/* everything gamepad touch holds, let go: the menu opened, or the pad went away */
static void gamepad_touch_release(void) {
	motion_pause();   /* (and the gyro: the next sample starts a fresh interval) */
	for (int i = 0; i < TP_FINGERS; i++) tp_down[i] = 0;
	cur_down = 0; cur_mode = 0; lb_held = rb_held = 0; ltrig_on = rtrig_on = 0; ptrig[0] = ptrig[1] = 0;
	cur2_on = 0; cur_used_l = cur_used_r = 0; cur_cross = cur_trig[0] = cur_trig[1] = 0; trig_touch[0] = trig_touch[1] = 0;
	twist_pad = 0; twist_down = twist_drag;
	mouse_rear_down = 0;
	for (int i = 0; i < ST_FINGERS; i++) st_down[i] = 0;   /* (and the screen's fingers: the menu takes the screen) */
}
/* A touchscreen (SDL's direct touch devices: a laptop's or a handheld's screen; a trackpad's fingers stay the mouse, unless
 * RECOMP_TOUCH_ANY=1, which takes any touch device's, a Mac trackpad's included, as the screen's: a multi-touch test without a
 * touchscreen). Each finger is a contact of its own on the front panel where it lands on the picture (one on the bars touches
 * nothing), held as it moves (clamped to the picture's edge), the rear panel's instead when RB is held as it lands, as the
 * touchpad's are. Six at once, the front panel's own limit. */
static int touch_any(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_TOUCH_ANY") != 0; return v; }
#define SCRIPT_TOUCH_ID ((SDL_TouchID)0x6e746976)   /* finger_script's device */
/* RECOMP_FINGER_SCRIPT="frame:down|move|up:finger:x,y;..." (x, y 0-1 across the window): those finger events pushed into SDL's
 * queue at those frames, a touchscreen of their own -- screen_finger's test without one (the windowed loop: a window is needed) */
static void finger_script(void) {
	static const char *p = (const char *)1; if (p == (const char *)1) p = getenv("RECOMP_FINGER_SCRIPT");
	extern uint64_t display_frames(void);
	while (p && *p) {
		unsigned long long f; char kind[8]; unsigned id; float x = 0, y = 0;
		const int n = sscanf(p, "%llu:%7[a-z]:%u:%f,%f", &f, kind, &id, &x, &y);
		if (n < 3) { p = 0; break; }
		if (display_frames() < f) break;
		SDL_Event ev; SDL_zero(ev);
		ev.type = !strcmp(kind, "down") ? SDL_EVENT_FINGER_DOWN : !strcmp(kind, "up") ? SDL_EVENT_FINGER_UP : SDL_EVENT_FINGER_MOTION;
		ev.tfinger.touchID = SCRIPT_TOUCH_ID; ev.tfinger.fingerID = (SDL_FingerID)id + 1; ev.tfinger.x = x; ev.tfinger.y = y; ev.tfinger.pressure = 1;
		SDL_PushEvent(&ev);
		fprintf(stderr, "    finger script: frame %llu: %s finger %u at %.3f,%.3f\n", (unsigned long long)display_frames(), kind, id, x, y);
		p = strchr(p, ';'); if (p) p++;
	}
}
static void screen_finger(const SDL_Event *e) {
	if (!touch_any() && e->tfinger.touchID != SCRIPT_TOUCH_ID && SDL_GetTouchDeviceType(e->tfinger.touchID) != SDL_TOUCH_DEVICE_DIRECT) return;
	int w = 0, h = 0; SDL_GetWindowSize(win, &w, &h);
	const float mx = e->tfinger.x * (float)w, my = e->tfinger.y * (float)h;   /* (SDL's finger positions: 0-1 across the window) */
	int k = -1, free_k = -1;
	for (int i = 0; i < ST_FINGERS; i++) { if (st_down[i] && st_id[i] == e->tfinger.fingerID) k = i; else if (!st_down[i] && free_k < 0) free_k = i; }
	if (e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) { if (k >= 0) st_down[k] = 0; return; }
	int tx, ty;
	if (k < 0) {   /* a new finger (a motion of one that landed on the bars is ignored) */
		if (e->type != SDL_EVENT_FINGER_DOWN || free_k < 0 || !mouse_to_touch(mx, my, 0, &tx, &ty)) return;
		k = free_k; st_id[k] = e->tfinger.fingerID; st_port[k] = rb_held;
	} else if (!mouse_to_touch(mx, my, 1, &tx, &ty)) return;
	st_x[k] = tx; st_y[k] = st_port[k] ? clampi(ty, REAR_Y0, REAR_Y1) : ty;
	st_down[k] = 1;
}

int window_active(void) { return active; }

static SDL_GPUDevice *gpu;
void *window_gpu_device(void) { return gpu; }   /* for a future gfx_gpu.c */
void *window_handle(void) { return win; }

/* ---- audio: an SDL audio stream per Vita output port, bound to the default playback device.
 * Output queues the samples and blocks while more than a few buffers wait, so the game is paced
 * by the sound card as on the hardware */
static SDL_AudioStream *streams[8];
static uint32_t afreq[8], achan[8];
static int nstreams;
int audio_open(uint32_t freq, uint32_t channels, uint32_t samples) {
	(void)samples;
	if (nstreams >= 8) return 0;
	if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "    audio: SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError()); return 0; }
	SDL_AudioSpec want = {SDL_AUDIO_S16, (int)channels, (int)freq};
	/* the device's own buffer, in sample frames (before the first stream opens it): small, so what the
	 * game hears as "played" is close to what the speakers play (RECOMP_AUDIO_FRAMES overrides) */
	if (!nstreams) SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, getenv("RECOMP_AUDIO_FRAMES") ? getenv("RECOMP_AUDIO_FRAMES") : "256");
	SDL_AudioSpec devf;   /* the device's own format: a mismatch makes SDL resample the game's 48 kHz buffers (its default converter is linear, which on a 44.1 kHz device is audible as a gritty top) */
	if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &devf, 0))
		fprintf(stderr, "    audio: device format %u Hz, %u ch (%s), the game's port asks %u Hz, %u ch%s\n", devf.freq, devf.channels,
		        devf.format == SDL_AUDIO_S16 ? "s16" : devf.format == SDL_AUDIO_F32 ? "f32" : "other", freq, channels,
		        (devf.freq != freq || (int)devf.channels != (int)channels) ? " -> SDL will resample/convert" : "");
	SDL_AudioStream *s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, 0, 0);
	if (!s) { fprintf(stderr, "    audio: SDL_OpenAudioDeviceStream(%u Hz, %u ch) FAILED: %s\n", freq, channels, SDL_GetError()); return 0; }
	fprintf(stderr, "    audio: SDL stream opened: %u Hz, %u ch (dev index %d)\n", freq, channels, nstreams);
	SDL_ResumeAudioStreamDevice(s);
	streams[nstreams] = s; afreq[nstreams] = freq; achan[nstreams] = channels;
	return ++nstreams;   /* 1-based handle; 0 means "failed to open" to callers */
}
/* ---- automatic A/V sync: the audio held back so that sound and picture of one moment of game time come
 * out together, from measured terms (no per-game or per-frame-rate tuning):
 *   picture: the game's update -> its flip (about one flip interval, display.c) + the flip -> this
 *            window's submit (measured in the present loop) + the compositor and scan-out (1.5 display
 *            refreshes, the one estimated term)
 *   sound:   each port's own queue ahead of the new buffer + the device buffer (measured per buffer)
 * delay = picture - sound + a small bias toward late sound (people notice sound early ~3x sooner than
 * late; RECOMP_AV_BIAS_MS, default 10) + "audio_sync_trim_ms" (-100..100, settings.ini only, per game). Uncharted's cutscene dialogue ran ~47 ms ahead of the mouths with
 * none, and the right fixed value moved with the frame rate (2026-10-09). */
static volatile float av_video_ms = 25.0f;   /* flip -> submit + compositor, averaged (the present loop) */
static float av_audio_ms[8] = {10, 10, 10, 10, 10, 10, 10, 10};
static int av_applied[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
int window_audio_auto_delay_ms(void) { return av_applied[0] < 0 ? 0 : av_applied[0]; }
static int av_auto_delay(int port) {
	extern float display_frame_period_ms(void);
	static int off = -1; if (off < 0) off = getenv("RECOMP_NO_AV_SYNC") != 0;
	if (off) return 0;
	static float bias = -1; if (bias < 0) bias = getenv("RECOMP_AV_BIAS_MS") ? (float)atof(getenv("RECOMP_AV_BIAS_MS")) : 10.0f;
	float trim = (float)settings_int("audio_sync_trim_ms", 0);   /* settings.ini's fine-tune: the constant part no measurement covers (compositor, speakers, a game's pipeline depth) */
	float target = display_frame_period_ms() + av_video_ms - av_audio_ms[port] + bias + trim;
	if (target < 0) target = 0; if (target > 300) target = 300;
	static float last_trim[8] = {-1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f};
	int moved = trim != last_trim[port]; last_trim[port] = trim;   /* the fine-tune moved: apply it at once, whatever the size */
	if (moved || av_applied[port] < 0 || target - (float)av_applied[port] > 5 || (float)av_applied[port] - target > 5) av_applied[port] = (int)(target + 0.5f);   /* else 5 ms hysteresis: each change is a small skip */
	return av_applied[port];
}
void audio_output(int dev, const void *buf, uint32_t bytes) {
	if (dev < 1 || dev > nstreams) return;
	SDL_AudioStream *s = streams[dev - 1];
	/* the overlay is open, or the menu bar's Game > Pause is on: the game is paused (display.c's vblank
	 * wait parks it; see that file's comment), but a buffer already queued before the pause would
	 * otherwise keep playing out -- drop this one and clear what's left so the pause is silent at
	 * once, not a fraction of a second late */
	if (overlay_game_paused()) {
		SDL_ClearAudioStream(s);
		/* and the game's audio thread waits the pause out with the rest of the game. Returned to at once, it ran its
		 * sound system flat out for as long as the menu stayed open -- every voice and stream played on to its end
		 * unheard (Killzone: three times real time over a session with menu visits; after them its streamed ambience
		 * and dialogue had run dry or out of step) */
		int b = sched_block_begin();
		while (overlay_game_paused()) SDL_Delay(5);
		if (b) sched_block_end();
		return;
	}
	/* metrics.c's audio stats: a port that already held less than one buffer's worth when this
	 * one arrives was about to run (or just ran) dry -- the best this path can tell about an
	 * underrun without deeper SDL hooks, counted cumulatively across the run */
	static uint32_t underruns;
	if (SDL_GetAudioStreamQueued(s) < (int)bytes) underruns++;
	/* the master volume (settings "volume" 0..1, the menu's slider; RECOMP_VOLUME wins): the port's
	 * gain, set when it changes */
	static float applied[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
	float vol = getenv("RECOMP_VOLUME") ? (float)atof(getenv("RECOMP_VOLUME")) : (float)settings_float("volume", 1.0);
	if (vol < 0) vol = 0; if (vol > 1) vol = 1;
	if (vol != applied[dev - 1]) { SDL_SetAudioStreamGain(s, vol); applied[dev - 1] = vol; }
	/* the audio delay: av_auto_delay's measured A/V sync (plus settings.ini's "audio_sync_trim_ms" fine-tune),
	 * that much audio held in the port's queue beyond the pacing depth, so everything the game hands over
	 * reaches the speakers that much later while the game's own timing (the buffers it has handed over)
	 * is unchanged. The queue is topped up with silence when it holds less (the delay rose, or the port
	 * ran dry); a lower delay drains through the pacing wait below. (A fixed delay, settings
	 * "audio_delay_ms" / RECOMP_AUDIO_DELAY_MS, was removed 2026-10-15: the dialogue running ahead of the
	 * lips it was set to hide was NGS reporting a stereo voice's samples at half the hardware's count --
	 * ngs.c h_voice_state -- and a fixed number went stale with every fix.) */
	int extra = 0;
	{	int frame = (int)achan[dev - 1] * 2, q = SDL_GetAudioStreamQueued(s);
		static int dev_frames = -1; if (dev_frames < 0) dev_frames = getenv("RECOMP_AUDIO_FRAMES") ? atoi(getenv("RECOMP_AUDIO_FRAMES")) : 256;
		{	/* this port's own latency for the new buffer, our delay excluded (av_auto_delay) */
			float bpms = (float)afreq[dev - 1] / 1000.0f * (float)frame, held = av_applied[dev - 1] > 0 ? (float)av_applied[dev - 1] * bpms : 0;
			float lat = ((float)q - held) / bpms; if (lat < 0) lat = 0;
			lat += (float)dev_frames * 1000.0f / (float)afreq[dev - 1];
			av_audio_ms[dev - 1] += (lat - av_audio_ms[dev - 1]) * 0.05f;
		}
		int dms = av_auto_delay(dev - 1);
		extra = (int)((uint64_t)dms * afreq[dev - 1] / 1000) * frame;
		/* the delay's silence goes in once, on the first buffer: topping the queue up again whenever it dips
		 * below the target puts a silent gap between two buffers of continuous audio, which on a small-buffer
		 * port (P5D's 256-sample one) sits right in the music as a click/hiss at every buffer boundary */
		static int delay_pre[8];
		if (extra && !delay_pre[dev - 1]) {
			static uint8_t zero[8192]; int pad = extra;
			for (; pad > 0;) { int n = pad < (int)sizeof zero ? pad : (int)sizeof zero; SDL_PutAudioStreamData(s, zero, n); pad -= n; }
			delay_pre[dev - 1] = 1;
			if (getenv("RECOMP_AUDIO_STATS")) fprintf(stderr, "    audio: A/V delay %d ms (%d bytes) preloaded once on port %d\n", dms, extra, dev);
		}
	}
	SDL_PutAudioStreamData(s, buf, (int)bytes);
	{	uint32_t ms = 0;
		for (int i = 0; i < nstreams; i++) { int q = SDL_GetAudioStreamQueued(streams[i]); uint32_t bps = achan[i] * 2 * (afreq[i] / 1000); if (bps) ms += (uint32_t)q / bps; }
		metrics_set_audio(ms, (uint32_t)nstreams, underruns);
	}
	/* pacing: the call returns once at most `depth` buffers wait behind the one playing -- the Vita's
	 * output is double-buffered (one pending), and a game times its cutscenes by the buffers it has
	 * handed over. Three (until 2026-10-09) put Uncharted's dialogue port (1024-sample buffers) up to
	 * ~85 ms behind the lips. settings "audio_buffers" 1..4 (default 1; RECOMP_AUDIO_QUEUE wins): more if
	 * a slow machine crackles. The pacing sleep gives the guest thread's run slot up (RECOMP_SCHED). */
	static int depth = -1;
	if (depth < 0) { depth = getenv("RECOMP_AUDIO_QUEUE") ? atoi(getenv("RECOMP_AUDIO_QUEUE")) : settings_int("audio_buffers", 1); if (depth < 1) depth = 1; if (depth > 4) depth = 4; }
	if (SDL_GetAudioStreamQueued(s) > depth * (int)bytes + extra) {
		int b = sched_block_begin();
		while (SDL_GetAudioStreamQueued(s) > depth * (int)bytes + extra) SDL_Delay(1);
		if (b) sched_block_end();
	}
}

/* the GPU backend's frame: blitted (on the game thread) into one of three RGBA8 textures here, the
 * present loop shows the latest; three so the one being written is never the one being shown */
static SDL_GPUTexture *gtex[3]; static uint32_t gtw[3], gth[3]; static int gidx = -1, gshow = -1, gnew;
static uint32_t gtl[3];   /* each one's mip levels (window_present_gpu: more than one when it is over twice the window's size) */
void window_pixel_size(uint32_t *w, uint32_t *h) { *w = win_pw; *h = win_ph; }
static uint64_t gready_us[3];   /* when each was handed over (the game's flip, as the GPU backend presents it): RECOMP_LATENCY_LOG */
static int frames_in_flight = 2;
/* the overlay needs real pixels to blend against, and the GPU-rendered path's frame never otherwise
 * touches the CPU; these two transfer buffers (one each direction -- SDL_GPU's usage hint may pick
 * different memory for them, so one buffer doesn't stand in for both) round-trip it through the
 * composite only while the overlay is actually on. ensure_osd_rt resizes both together. */
static SDL_GPUTransferBuffer *osd_dl, *osd_ul; static uint32_t osd_rtw, osd_rth;
static int ensure_osd_rt(uint32_t w, uint32_t h) {
	if (osd_dl && osd_rtw == w && osd_rth == h) return 1;
	if (osd_dl) SDL_ReleaseGPUTransferBuffer(gpu, osd_dl);
	if (osd_ul) SDL_ReleaseGPUTransferBuffer(gpu, osd_ul);
	SDL_GPUTransferBufferCreateInfo dci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = 4 * w * h };
	SDL_GPUTransferBufferCreateInfo uci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = 4 * w * h };
	osd_dl = SDL_CreateGPUTransferBuffer(gpu, &dci); osd_ul = SDL_CreateGPUTransferBuffer(gpu, &uci);
	osd_rtw = w; osd_rth = h;
	return osd_dl && osd_ul;
}
/* the overlay onto a GPU-rendered frame: download `src`, composite on the CPU, upload into `dst`
 * (1 done, 0 if any step failed: the caller blits the plain frame instead) */
static int osd_round_trip(SDL_GPUTexture *src, SDL_GPUTexture *dst, uint32_t w, uint32_t h) {
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
	if (!cmd) return 0;
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureRegion sr = { .texture = src, .w = w, .h = h, .d = 1 };
	SDL_GPUTextureTransferInfo dt = { .transfer_buffer = osd_dl, .pixels_per_row = w, .rows_per_layer = h };
	SDL_DownloadFromGPUTexture(cp, &sr, &dt);
	SDL_EndGPUCopyPass(cp);
	SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
	if (fence) { SDL_WaitForGPUFences(gpu, true, &fence, 1); SDL_ReleaseGPUFence(gpu, fence); }
	void *dl = SDL_MapGPUTransferBuffer(gpu, osd_dl, false);
	void *ul = dl ? SDL_MapGPUTransferBuffer(gpu, osd_ul, true) : 0;
	if (ul) { memcpy(ul, dl, 4u * w * h); metrics_osd_composite((uint8_t *)ul, w, w, h); p5d_banner_composite((uint8_t *)ul, w, w, h); SDL_UnmapGPUTransferBuffer(gpu, osd_ul); }
	if (dl) SDL_UnmapGPUTransferBuffer(gpu, osd_dl);
	SDL_GPUCommandBuffer *cmd2 = ul ? SDL_AcquireGPUCommandBuffer(gpu) : 0;
	if (!cmd2) return 0;
	SDL_GPUCopyPass *up = SDL_BeginGPUCopyPass(cmd2);
	SDL_GPUTextureTransferInfo si = { .transfer_buffer = osd_ul, .pixels_per_row = w, .rows_per_layer = h };
	SDL_GPUTextureRegion dr = { .texture = dst, .w = w, .h = h, .d = 1 };
	SDL_UploadToGPUTexture(up, &si, &dr, true);
	SDL_EndGPUCopyPass(up);
	SDL_SubmitGPUCommandBuffer(cmd2);
	return 1;
}
static int osd_cpu(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_OSD_CPU") != 0; return v; }   /* RECOMP_OSD_CPU=1: the overlay composited into the frame's pixels on the CPU, as before the ImGui HUD */
/* the screen model (menus/rows) from the texture actually being presented: a small GPU download every
 * few presents on the presenting thread, then the same pixel scan the guest frame buffer path used. The
 * guest frame buffer is not reliable for this in windowed mode (the GPU keeps the frame on the GPU and
 * guest memory holds an older frame), which is why the model reads the presented texture instead. */
static void ui_scan_texture(SDL_GPUTexture *tex, uint32_t w, uint32_t h) {
	if (!gpu || !tex || w < 640 || h < 400) return;
	static SDL_GPUTransferBuffer *xb; static uint32_t xbytes;
	uint32_t need = w * h * 4;
	if (!xb || xbytes < need) {
		if (xb) SDL_ReleaseGPUTransferBuffer(gpu, xb);
		SDL_GPUTransferBufferCreateInfo xi = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = need };
		xb = SDL_CreateGPUTransferBuffer(gpu, &xi); xbytes = need;
		if (!xb) { xbytes = 0; return; }
	}
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
	if (!cmd) return;
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureRegion src = { .texture = tex, .w = w, .h = h, .d = 1 };
	SDL_GPUTextureTransferInfo dst = { .transfer_buffer = xb, .pixels_per_row = w, .rows_per_layer = h };
	SDL_DownloadFromGPUTexture(cp, &src, &dst); SDL_EndGPUCopyPass(cp);
	SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
	if (!f) return;
	SDL_WaitForGPUFences(gpu, true, &f, 1); SDL_ReleaseGPUFence(gpu, f);
	const uint8_t *px = SDL_MapGPUTransferBuffer(gpu, xb, false);
	if (px) { p5d_ui_scan_pixels(px, w, h, w); SDL_UnmapGPUTransferBuffer(gpu, xb); }
}
void window_present_gpu(void *src, uint32_t w, uint32_t h, int raw) {
	/* M1672: which guest frame each window present shows, to tell a 60 Hz window presenting every rendered frame from
	 * one that shows the same texture twice (the 30 Hz content the dance stage shows while metrics report 60 fps).
	 * RECOMP_PRESENT_SEQ=1 prints one line per present with the guest frame counter. */
	{ static int on = -1; if (on < 0) { const char *e = getenv("RECOMP_PRESENT_SEQ"); on = e && atoi(e) != 0; }
	  if (on) { extern uint64_t display_frames(void); static uint64_t last; uint64_t f = display_frames();
	    fprintf(stderr, "[PRES] f=%llu last=%llu %s %ux%u\n", (unsigned long long)f, (unsigned long long)last,
	            f == last ? "SAME" : "new", w, h); last = f; } }
	{ static uint64_t n, next_us; static int ms_override = -1; if (ms_override < 0) { const char *e = getenv("RECOMP_UI_SCAN_MS"); ms_override = e ? atoi(e) : 0; }
	  if (w >= 640 && h >= 400) {
		uint32_t scale = (w + 480u) / 960u; if (scale < 1u) scale = 1u; if (scale > 6u) scale = 6u;
		uint64_t t = now_us();
		int boost = p5d_ui_boost_until_us && t < p5d_ui_boost_until_us;
		if ((boost || p5d_saw_menu || (++n % 6 == 0)) && t >= next_us) {
			uint64_t a = now_us();
			ui_scan_texture((SDL_GPUTexture *)src, w, h);
			uint64_t cost = now_us() - a;
			/* the frame read-back grows with the render scale (2 MB at 1x, 19 MB at 3x); space the scans by what the
			 * last one cost, so at 2K/3x the controls stay responsive (reported: "能不能在2k渲染之下也能正常操控") */
			uint64_t base = boost ? 25000u : (ms_override > 0 ? (uint64_t)ms_override * 1000u : 40000ull * scale);
			uint64_t next = cost * (boost ? 6u : 12u);
			if (next < base) next = base;
			if (next > 500000u) next = 500000u;
			next_us = now_us() + next;
			n = 0;
			static int slog = -1; if (slog < 0) { const char *e = getenv("RECOMP_UI_SCAN_LOG"); slog = e && *e ? 1 : 0; }
			if (slog) fprintf(stderr, "[SCAN] f=%llu scale=%u boost=%d cost=%lluus next=%lluus\n",
				(unsigned long long)display_frames(), scale, boost, (unsigned long long)cost, (unsigned long long)next);
		}
	  } }
	if (!active || !gpu || !w || !h || w > 16384 || h > 16384) return;
	pthread_mutex_lock(&lock);
	int n = (gidx + 1) % 3; if (n == gshow) n = (n + 1) % 3;
	/* a frame over twice the window's size (render scale 4x and up on most displays) carries mip levels,
	 * so the present loop can blit from the level just above the window's size: a linear blit across a
	 * bigger ratio samples only some of the pixels -- the extra resolution came out as shimmer instead of
	 * supersampling */
	uint32_t levels = 1;
	if (win_pw && win_ph) while (levels < 6 && (w >> levels) >= win_pw && (h >> levels) >= win_ph) levels++;
	if (gtex[n] && (gtw[n] != w || gth[n] != h || gtl[n] != levels)) { SDL_ReleaseGPUTexture(gpu, gtex[n]); gtex[n] = 0; }
	if (!gtex[n]) {
		SDL_GPUTextureCreateInfo ci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
			.width = w, .height = h, .layer_count_or_depth = 1, .num_levels = levels, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
		gtex[n] = SDL_CreateGPUTexture(gpu, &ci); gtw[n] = w; gth[n] = h; gtl[n] = levels;
	}
	/* the overlay is off here by default (RECOMP_OSD=0, metrics.c's own comment says why): without
	 * a blending shader of its own in gfx_gpu.c (not written yet), the only way to blend a
	 * translucent overlay onto a frame that's already a GPU texture is to round-trip it through the
	 * CPU -- download `src`, composite, re-upload into gtex[n] below -- with a GPU fence wait to
	 * make the download safe to read. That's a real per-frame stall, only worth paying when someone
	 * actually asked to see the overlay (F1 or RECOMP_OSD=1|2); otherwise this falls through to the
	 * plain same-GPU blit at the bottom, unchanged from before this feature existed. */
	/* ... which is why, with the ImGui overlay up (overlay_draws_hud: every windowed run), the lines are
	 * now drawn by ImGui into the swapchain in the present loop below, beside the touch markers
	 * (overlay_marks), and this round trip isn't taken: the overlay on, the frame no longer costs a GPU
	 * sync. Our test setup had osd=2 on at render scale 2: 8 MB down, 8 MB up and the frame's whole
	 * GPU time waited for, every frame. RECOMP_OSD_CPU=1: the composite below, as before. */
	int osd = gtex[n] && (metrics_osd_level() > 0 || p5d_banner_visible()) && !(overlay_draws_hud() && !osd_cpu()) && ensure_osd_rt(w, h);
	if (osd) {
		/* the round trip waits on the GPU: not under the lock the present loop takes every
		 * iteration. Only this thread (gfx_gpu_present, under the renderer's lock) uses the
		 * transfer buffers, and slot n can't be shown until it's published below. */
		SDL_GPUTexture *dst = gtex[n];
		pthread_mutex_unlock(&lock);
		int done = osd_round_trip(src, dst, w, h);
		pthread_mutex_lock(&lock);
		if (done) { gidx = n; gnew = 1; new_frame = 0; gready_us[n] = now_us(); } else osd = 0;
	}
	if (!osd) {
		SDL_GPUCommandBuffer *cmd = gtex[n] ? SDL_AcquireGPUCommandBuffer(gpu) : 0;
		if (cmd && raw) {
			/* raw: an sRGB frame (Killzone's). A blit would decode it on the way into this plain texture; a copy moves
			 * the bytes as they are, which is what the display scans out */
			SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
			SDL_GPUTextureLocation s = { .texture = src }, d = { .texture = gtex[n] };
			SDL_CopyGPUTextureToTexture(cp, &s, &d, w, h, 1, false);
			SDL_EndGPUCopyPass(cp);
		} else if (cmd) {
			SDL_GPUBlitInfo blit = { .source = { .texture = src, .w = w, .h = h }, .destination = { .texture = gtex[n], .w = w, .h = h }, .load_op = SDL_GPU_LOADOP_DONT_CARE, .filter = SDL_GPU_FILTER_NEAREST };
			SDL_BlitGPUTexture(cmd, &blit);
		}
		if (cmd) {
			if (gtl[n] > 1) SDL_GenerateMipmapsForGPUTexture(cmd, gtex[n]);
			SDL_SubmitGPUCommandBuffer(cmd);
			gidx = n; gnew = 1; new_frame = 0; gready_us[n] = now_us();
		}
	}
	pthread_mutex_unlock(&lock);
}
/* RECOMP_OVERLAY_DUMP=path.ppm (a verification aid, no window needed): the first time this frame
 * passes through here, render the in-game menu once, forced open, over a copy of it, and write the
 * result -- then let the run carry on exactly as before. It calls overlay_render_forced, never
 * overlay_open/overlay_is_open, so nothing else that keys off the overlay being open (display.c's
 * pause gate, this file's own input routing and audio mute above) is ever affected by it; a run with
 * this unset pays one cached getenv and a branch, same as metrics.c's RECOMP_OSD_DUMP. Works with
 * either graphics backend: by the time window_present is called, RECOMP_GFX=gpu has already synced
 * its rendered frame into this same guest memory (gfx_gpu.c's present, for exactly this reason). */
static void overlay_dump_maybe(uint32_t base, uint32_t w, uint32_t h, uint32_t pitch) {
	static int done = -1;
	if (done < 0) { const char *e = getenv("RECOMP_OVERLAY_DUMP"); done = e ? 0 : 1; }
	if (done) return;
	/* vitarecomp test sets RECOMP_SHOT_LAST to the frame it's capturing (TEST_FRAMES) and exits the
	 * process right after writing that shot -- one window_present call later than this one, which as
	 * a result never happens (gfx_soft.c's gfx_soft_present calls shot() then checks that exit
	 * before it ever reaches window_present). Wait for the LAST window_present call the run will
	 * ever make instead: display_frames() reaches RECOMP_SHOT_LAST exactly once, on the call right
	 * before that exit -- as close to the shot frame as this hook can structurally get, and the same
	 * scene. Standalone (no RECOMP_SHOT_LAST), fire on the first frame there is. */
	extern uint64_t display_frames(void);
	static long target = -2; if (target == -2) { const char *e = getenv("RECOMP_SHOT_LAST"); target = e ? atol(e) : -1; }
	if (target >= 0 && (long)display_frames() != target) return;
	done = 1;
	const char *path = getenv("RECOMP_OVERLAY_DUMP");
	if (!gpu) { fprintf(stderr, "RECOMP_OVERLAY_DUMP: no GPU device, skipping\n"); return; }
	if (!overlay_init(gpu, 0, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)) { fprintf(stderr, "RECOMP_OVERLAY_DUMP: overlay_init failed\n"); return; }
	uint8_t *tmp = malloc(4u * w * h);
	if (!tmp) return;
	for (uint32_t y = 0; y < h; y++) memcpy(tmp + 4u * w * y, G(base + 4 * pitch * y), 4u * w);
	SDL_GPUTextureCreateInfo ci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
		.width = w, .height = h, .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
	SDL_GPUTexture *tex = SDL_CreateGPUTexture(gpu, &ci);
	SDL_GPUTransferBufferCreateInfo uci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = 4u * w * h };
	SDL_GPUTransferBuffer *up = SDL_CreateGPUTransferBuffer(gpu, &uci);
	SDL_GPUTransferBufferCreateInfo dci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = 4u * w * h };
	SDL_GPUTransferBuffer *down = SDL_CreateGPUTransferBuffer(gpu, &dci);
	if (!tex || !up || !down) {
		fprintf(stderr, "RECOMP_OVERLAY_DUMP: out of GPU memory\n"); free(tmp);
		if (tex) SDL_ReleaseGPUTexture(gpu, tex);
		if (up) SDL_ReleaseGPUTransferBuffer(gpu, up);
		if (down) SDL_ReleaseGPUTransferBuffer(gpu, down);
		return;
	}
	/* RECOMP_OVERLAY_DUMP_UI: what to show (overlay.h's overlay_render_forced; default the full menu), one
	 * dump per comma-separated entry, each over a fresh copy of the same frame, named f_<entry>.ppm when
	 * there are several */
	char uis[256]; snprintf(uis, sizeof uis, "%s", getenv("RECOMP_OVERLAY_DUMP_UI") ? getenv("RECOMP_OVERLAY_DUMP_UI") : "menu");
	int several = strchr(uis, ',') != 0;
	for (char *ui = strtok(uis, ","); ui; ui = strtok(0, ",")) {
		void *dst = SDL_MapGPUTransferBuffer(gpu, up, true);
		if (dst) { memcpy(dst, tmp, 4u * w * h); SDL_UnmapGPUTransferBuffer(gpu, up); }
		/* three command buffers in order: the frame's upload; the overlay's own (overlay_render_forced
		 * submits one per frame it draws -- the ImGui backend's buffers are per frame; the closed-state
		 * modes draw the HUD, RECOMP_OSD's lines, under the bar in that same frame, as the live path does);
		 * the download. The device runs them in submission order. */
		SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
		SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
		SDL_GPUTextureTransferInfo si = { .transfer_buffer = up, .pixels_per_row = w, .rows_per_layer = h };
		SDL_GPUTextureRegion dr = { .texture = tex, .w = w, .h = h, .d = 1 };
		SDL_UploadToGPUTexture(cp, &si, &dr, true);
		SDL_EndGPUCopyPass(cp);
		SDL_SubmitGPUCommandBuffer(cmd);
		overlay_render_forced(gpu, tex, w, h, ui);   /* draws over the uploaded frame: LOAD, not CLEAR */
		cmd = SDL_AcquireGPUCommandBuffer(gpu);
		SDL_GPUCopyPass *cp2 = SDL_BeginGPUCopyPass(cmd);
		SDL_GPUTextureRegion sr = { .texture = tex, .w = w, .h = h, .d = 1 };
		SDL_GPUTextureTransferInfo dt = { .transfer_buffer = down, .pixels_per_row = w, .rows_per_layer = h };
		SDL_DownloadFromGPUTexture(cp2, &sr, &dt);
		SDL_EndGPUCopyPass(cp2);
		SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
		if (fence) { SDL_WaitForGPUFences(gpu, true, &fence, 1); SDL_ReleaseGPUFence(gpu, fence); }
		char out[600]; const char *dot = strrchr(path, '.');
		if (several) snprintf(out, sizeof out, "%.*s_%s%s", (int)(dot ? dot - path : (long)strlen(path)), path, ui, dot ? dot : "");
		else snprintf(out, sizeof out, "%s", path);
		const uint8_t *px = SDL_MapGPUTransferBuffer(gpu, down, false);
		FILE *f = px ? fopen(out, "wb") : 0;
		if (f) {
			fprintf(f, "P6\n%u %u\n255\n", w, h);
			for (uint32_t i = 0; i < w * h; i++) fwrite(px + 4 * i, 1, 3, f);
			fclose(f);
			fprintf(stderr, "    overlay: wrote the %ux%u dump (%s) to %s\n", w, h, ui, out);
		} else fprintf(stderr, "RECOMP_OVERLAY_DUMP: cannot write %s (%s)\n", out, SDL_GetError());
		if (px) SDL_UnmapGPUTransferBuffer(gpu, down);
	}
	free(tmp);
	SDL_ReleaseGPUTransferBuffer(gpu, down); SDL_ReleaseGPUTransferBuffer(gpu, up); SDL_ReleaseGPUTexture(gpu, tex);
}
void window_present(uint32_t base, uint32_t w, uint32_t h, uint32_t pitch) {
	if (!w || !h || w > 4096 || h > 4096) return;
	overlay_dump_maybe(base, w, h, pitch);
	if (!active) return;
	pthread_mutex_lock(&lock);
	gnew = 0; gshow = -1;   /* a software frame supersedes any GPU one */
	if (!frame || fw != w || fh != h) { free(frame); frame = malloc(4 * w * h); fw = w; fh = h; }
	for (uint32_t y = 0; y < h; y++) memcpy(frame + 4 * w * y, G(base + 4 * pitch * y), 4 * w);
	{ static uint64_t n; if (w >= 640 && h >= 400 && (++n % 6) == 0) p5d_ui_scan_pixels(frame, w, h, w); }
	new_frame = 1;
	pthread_mutex_unlock(&lock);
}

/* M1522: one code-level answer to "which screen owns the input right now". Sources, in order: our pause/settings
 * latches, the dance-stage job stream, the dance latch, the app state (5 MAIN / 6 TITLE), the MusicSelect / COMMU /
 * Config / result job names, the story latch, and the code-level scene flags. No picture model takes part: the input
 * behaviour changes on the frame the game's code state changes. */
enum { P5D_SCR_UNKNOWN, P5D_SCR_TITLE, P5D_SCR_MAIN, P5D_SCR_SONG, P5D_SCR_COMMU, P5D_SCR_CONFIG, P5D_SCR_PAUSE,
       P5D_SCR_DANCE, P5D_SCR_RESULT, P5D_SCR_STORY };
static int p5d_scr_now = P5D_SCR_UNKNOWN;
static int p5d_main_settings;   /* we clicked the main menu CONFIG row (our own row counter): the settings page is up */
static int p5d_input_screen(void) {
	/* CODE LEVEL ONLY (M1523): every screen the game runs has its own job in the live job list (discovered with
	 * RECOMP_JOBDUMP=1). Order: our own pause/settings intents, then the job evidence in screen-priority order,
	 * then the app state; no picture model takes part. */
	uint64_t n = now_us();
	if (p5d_pause_latch && p5d_config_latch) return P5D_SCR_CONFIG;
	if (!p5d_pause_latch && p5d_main_settings) return P5D_SCR_CONFIG;
	if (p5d_pause_latch) return P5D_SCR_PAUSE;
	/* the RESULT page first: the dance stage's own jobs stay alive after the song ends (DanceProcess/mns/
	 * danceVoiceSub are only removed when the result screen lets go of them), so a freshness test on those
	 * kept classifying the result menu as the stage and its wheel/click did nothing (reported by the user). */
	if (p5d_result_job_us && n - p5d_result_job_us < 1200000) return P5D_SCR_RESULT;
	if (p5d_dance_job_us && n - p5d_dance_job_us < 1200000) return P5D_SCR_DANCE;   /* before the preloaded pause job */
	if (p5d_story_job_us && n - p5d_story_job_us < 1200000) return P5D_SCR_STORY;
	if (p5d_song_job_us && n - p5d_song_job_us < 1200000) return P5D_SCR_SONG;
	if (p5d_collection_job_us && n - p5d_collection_job_us < 1200000) return P5D_SCR_MAIN;   /* the COLLECTION pages are menus */
	if (p5d_pause_job_us && n - p5d_pause_job_us < 1200000) return P5D_SCR_PAUSE;
	/* the settings page has its own job; it is checked after the content pages so a stray config job can never hide
	 * a song/commu/dance/story/collection page (this is what makes the main menu's own CONFIG row identifiable
	 * without trusting our row counter) */
	if (p5d_config_job_us && n - p5d_config_job_us < 1200000) return P5D_SCR_CONFIG;
	if (p5d_tutorial_job_us && n - p5d_tutorial_job_us < 1200000) return P5D_SCR_MAIN;   /* TUTORIAL pages are menus */
	if (p5d_commu_job_us && n - p5d_commu_job_us < 1200000) return P5D_SCR_COMMU;
	if (p5d_main_job_us && n - p5d_main_job_us < 1200000) return P5D_SCR_MAIN;   /* before title: the title jobs linger */
	if (p5d_title_job_us && n - p5d_title_job_us < 1200000) return P5D_SCR_TITLE;
	{ int st = p5d_app_state_code();
	  if (st == 3) return P5D_SCR_COMMU;
	  if (st == 4) return P5D_SCR_MAIN;
	  if (st == 5) return P5D_SCR_MAIN;
	  if (st == 6) return P5D_SCR_TITLE; }
	{ const char *jn = p5d_job_name_fresh(n);
	  if (jn) {
		if (strstr(jn, "MusicSelect")) return P5D_SCR_SONG;
		if (strstr(jn, "commu")) return P5D_SCR_COMMU;
		if (strstr(jn, "Config") || strstr(jn, "config")) return P5D_SCR_CONFIG;
		if (strstr(jn, "esult")) return P5D_SCR_RESULT;
		if (strstr(jn, "dance") || strstr(jn, "Danc")) return P5D_SCR_DANCE;
		if (strstr(jn, "main menu")) return P5D_SCR_MAIN;
		if (strstr(jn, "title")) return P5D_SCR_TITLE;
	  } }
	if (p5d_scene == P5D_SCENE_SONG) return P5D_SCR_SONG;
	if (p5d_scene == P5D_SCENE_COMMU) return P5D_SCR_COMMU;
	if (p5d_scene == P5D_SCENE_RESULT) return P5D_SCR_RESULT;
	if (p5d_scene == P5D_SCENE_MAIN) return P5D_SCR_MAIN;
	if (p5d_scene == P5D_SCENE_TITLE) return P5D_SCR_TITLE;
	if (p5d_scene == P5D_SCENE_DANCE) return P5D_SCR_DANCE;
	return P5D_SCR_UNKNOWN;
}
static const char *p5d_input_screen_name(int sc) {
	switch (sc) { case P5D_SCR_TITLE: return "title"; case P5D_SCR_MAIN: return "main menu"; case P5D_SCR_SONG: return "song list";
	case P5D_SCR_COMMU: return "commu"; case P5D_SCR_CONFIG: return "settings"; case P5D_SCR_PAUSE: return "pause menu";
	case P5D_SCR_DANCE: return "dance stage"; case P5D_SCR_RESULT: return "result"; case P5D_SCR_STORY: return "story"; }
	return "unknown";
}
/* M1617: the song-list screen keeps the game's original 30 fps cadence even in the 60 fps mode. Its 3D character is
 * animated by a presented-frame counter (DanceReady/MusicSelect app paths), so at 60 fps the character alone ran 2x
 * real time (reported by the user: the main menu character is correct at 60, the song-select one is fast). display.c
 * asks this in swap_interval: 30 fps on this screen only, everything else stays 60; the dance stage preview (the
 * actual dance the song list starts) is a different screen and keeps 60. */
int window_p5d_song_screen(void) {
	return p5d_scr_now == P5D_SCR_SONG;
}
/* the last stick values the watch mode injected (hooks.c republishes them late in the frame, see
 * recomp_hook_p5d_xt_4f8) */
static uint8_t last_watch_sticks[4] = {128, 128, 128, 128};
static int watch_sticks_valid;
int window_watch_sticks(uint8_t s[4]) {
	if (!watch_sticks_valid) return 0;
	s[0] = last_watch_sticks[0]; s[1] = last_watch_sticks[1];
	s[2] = last_watch_sticks[2]; s[3] = last_watch_sticks[3];
	return 1;
}
void window_input(uint32_t *b, uint8_t s[4]) {
	uint32_t extra = p5d_bits | ((p5d_mouse_on && !touch_down && !cur_mode) ? p5d_mouse_bits : 0) | p5d_macro_bits(now_us()) | ((p5d_mouse_cross_until && now_us() < p5d_mouse_cross_until) ? CROSS : 0);
	*b = (buttons | extra) & ~(L3 | R3 | PAD_L2 | PAD_R2);
	{ static int il2 = -1; if (il2 < 0) il2 = getenv("RECOMP_INPUT_LOG") ? 1 : 0;
	  if (il2) { static uint64_t last2; uint64_t f2 = display_frames(); if (f2 - last2 >= 3) { last2 = f2; fprintf(stderr, "[INPUT2] f=%llu bits=%#x buttons=%#x p5dbits=%#x mouse=%#x macro=%#x latch=%llu\n", (unsigned long long)f2, *b, buttons, p5d_bits, p5d_mouse_bits, p5d_macro_bits(now_us()), (unsigned long long)p5d_mouse_cross_until); } } }
	{ static int il = -1; if (il < 0) il = getenv("RECOMP_INPUT_LOG") ? 1 : 0;
	  if (il) { static uint64_t last; extern uint64_t display_frames(void); uint64_t f = display_frames();
		    if ((*b & (CROSS | CIRCLE)) && f - last >= 6) { last = f; fprintf(stderr, "[INPUT] f=%llu bits=%#x cross=%d circle=%d prof=%d\n", (unsigned long long)f, *b, !!(*b & CROSS), !!(*b & CIRCLE), p5d_profile); } } }
	for (int i = 0; i < 4; i++) s[i] = sticks[i];
	/* the sticks are the touch cursor while a bumper is held -- unless the touchpad is doing the touching
	 * (RB held only to put its fingers on the rear panel: the sticks stay the game's) */
	if (cur_mode && !touchpad_active()) s[0] = s[1] = s[2] = s[3] = 128;
	if (gesture_wanted()) s[2] = s[3] = 128;   /* a gesture window: the right stick flicks a swipe (window_touch_gesture_hint) */
	if (twist_wanted() && !kbm_on) s[0] = s[1] = s[2] = s[3] = 128;   /* a twist: the sticks are its fingers (window_touch_twist_hint) */
	if (tilt_section && (kbm_on || !pad_gyro)) s[0] = 128;   /* a tilt section: left/right is the tilt (A / D, a gyro-less pad's stick), not a step or a steer */
	{ static int raw_env2 = -1; if (raw_env2 < 0) { const char *e = getenv("RECOMP_WATCH_RAW"); raw_env2 = e && atoi(e) != 0; }
	  if (raw_env2) return; }
	if (p5d_collection_active() && p5d_collection_watching) {
		/* WASD = the left stick (move), the arrow keys = the right stick (rotate); 128 is the centre, 0 up/left, 255 down/right.
		 * The key state is polled from SDL directly: the watch screen's key events are not guaranteed to reach our
		 * handler (the game's own handlers consume some), the polled state always describes what is really held. */
		const bool *ks = SDL_GetKeyboardState(NULL);
		int w = ks ? (ks[SDL_SCANCODE_W] ? 1 : 0) : 0, a = ks ? (ks[SDL_SCANCODE_A] ? 1 : 0) : 0;
		int s_ = ks ? (ks[SDL_SCANCODE_S] ? 1 : 0) : 0, d = ks ? (ks[SDL_SCANCODE_D] ? 1 : 0) : 0;
		int up = ks ? (ks[SDL_SCANCODE_UP] ? 1 : 0) : 0, dn = ks ? (ks[SDL_SCANCODE_DOWN] ? 1 : 0) : 0;
		int lf = ks ? (ks[SDL_SCANCODE_LEFT] ? 1 : 0) : 0, rt = ks ? (ks[SDL_SCANCODE_RIGHT] ? 1 : 0) : 0;
		/* the watch mode's left stick: WASD (full deflection, 128 centred, 0 up/left, 255 down/right) */
		s[0] = (uint8_t)(128 + (d - a) * 120);
		s[1] = (uint8_t)(128 + (s_ - w) * 120);
		s[2] = (uint8_t)(128 + (rt - lf) * 100);
		s[3] = (uint8_t)(128 + (dn - up) * 100);
		last_watch_sticks[0] = s[0]; last_watch_sticks[1] = s[1];
		last_watch_sticks[2] = s[2]; last_watch_sticks[3] = s[3]; watch_sticks_valid = 1;
		p5d_watch_keys[0] = w; p5d_watch_keys[1] = a; p5d_watch_keys[2] = s_; p5d_watch_keys[3] = d;
		p5d_watch_keys[4] = up; p5d_watch_keys[5] = dn; p5d_watch_keys[6] = lf; p5d_watch_keys[7] = rt;
		{ static int wl2 = -1; if (wl2 < 0) wl2 = getenv("RECOMP_WATCH_LOG") ? 1 : 0; static uint64_t last; uint64_t t = now_us(); if (wl2 && t - last > 500000) { last = t; fprintf(stderr, "[WATCHSTICK] keys %d%d%d%d %d%d%d%d sticks %u %u %u %u\n", p5d_watch_keys[0], p5d_watch_keys[1], p5d_watch_keys[2], p5d_watch_keys[3], p5d_watch_keys[4], p5d_watch_keys[5], p5d_watch_keys[6], p5d_watch_keys[7], s[0], s[1], s[2], s[3]); } }
	}
	/* a test override: hold the sticks at fixed values whatever the screen is (find out whether this scene reads them at all) */
	{ static int test_read; static char *test_s; if (!test_read) { test_read = 1; test_s = getenv("RECOMP_WATCH_STICK_TEST"); }
	  if (test_s && p5d_collection_active() && p5d_collection_watching) { int v[4] = {128, 128, 128, 128}; sscanf(test_s, "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]); for (int i = 0; i < 4; i++) s[i] = (uint8_t)v[i]; } }
}
/* a PS TV's pad (display.c: the "2" reads of port 1): the handheld's L and R are its L1 and R1, as a PS TV gives them to a
 * game that never asks; the other shoulder pair is its L2 and R2, and the stick clicks are there */
void window_input_pad2(uint32_t *b, uint8_t s[4]) {
	/* the PS-TV read takes its L/R from the held bits -- include the key map's and the tap's bits too, or the
	 * keyboard's L/R (Q/E, the COMMU character switch) never reach a game reading this pad (found 2026-11-07: Q/E
	 * did nothing there while the same keys worked in the handheld reads) */
	const uint32_t v = buttons | p5d_bits | p5d_macro_bits(now_us());
	window_input(b, s);
	*b = (*b & ~(LTRIG | RTRIG)) | (v & LTRIG ? L1 : 0) | (v & RTRIG ? R1 : 0) | (v & PAD_L2 ? LTRIG : 0) | (v & PAD_R2 ? RTRIG : 0) | (v & (L3 | R3));
}

static uint32_t pad_bit(int b) {
	switch (b) {
	case SDL_GAMEPAD_BUTTON_SOUTH: return CROSS; case SDL_GAMEPAD_BUTTON_EAST: return CIRCLE;
	case SDL_GAMEPAD_BUTTON_WEST: return SQUARE; case SDL_GAMEPAD_BUTTON_NORTH: return TRIANGLE;
	case SDL_GAMEPAD_BUTTON_START: return START; case SDL_GAMEPAD_BUTTON_BACK: return SELECT;
	case SDL_GAMEPAD_BUTTON_DPAD_UP: return UP; case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return DOWN;
	case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return LEFT; case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return RIGHT;
	case SDL_GAMEPAD_BUTTON_LEFT_STICK: return L3; case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return R3;
	default: return 0;
	}
}
/* The two shoulder pairs, whichever physical pair each is ("shoulder_buttons": the triggers by default are pad_main).
 * pad_main: the game's L and R; with the touch cursor up (the other pair held) a press is the cursor's touch, like
 * Cross (two cursors: both go down), and a press that touched sends no L/R until it is let go, whatever the other pair
 * does meanwhile. pad_second: held, the touch cursor (left: the front panel, right: the rear; right wins when both are
 * held); a game playing a PS TV has no touch, and there it is the pad's L2 and R2. k: 0 left, 1 right. */
static void pad_main(SDL_Gamepad *pad, int k, int dn) {
	(void)pad;
	int *on = k ? &rtrig_on : &ltrig_on; const uint32_t bit = k ? RTRIG : LTRIG;
	if (dn && !*on) { *on = 1; if (cur_mode) { cur_trig[k] = trig_touch[k] = 1; } else buttons |= bit; }
	else if (!dn && *on) { *on = 0; buttons &= ~bit; cur_trig[k] = trig_touch[k] = 0; }
	cur_down = cur_mode && (cur_cross || cur_trig[0] || cur_trig[1]);
}
static void pad_second(SDL_Gamepad *pad, int k, int dn) {
	if (display_pstv_active()) { const uint32_t bit = k ? PAD_R2 : PAD_L2; if (dn) buttons |= bit; else buttons &= ~bit; return; }
	if (k) rb_held = dn; else lb_held = dn;
	int m = rb_held ? 2 : lb_held ? 1 : 0;
	if (m != cur_mode) { cur_cross = cur_trig[0] = cur_trig[1] = 0; cur2_on = 0; cur_used_l = cur_used_r = 0; }   /* a cursor touch (and the split) ends with its mode */
	if (m && !cur_mode && pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH)) { cur_cross = 1; buttons &= ~CROSS; }   /* Cross already held: it's the touch now, not Cross */
	if (m && !cur_mode) for (int j = 0; j < 2; j++) if (j ? rtrig_on : ltrig_on) { cur_trig[j] = trig_touch[j] = 1; buttons &= ~(j ? RTRIG : LTRIG); }   /* L or R already held: the touch too */
	cur_mode = m;
	cur_down = m && (cur_cross || cur_trig[0] || cur_trig[1]);
}
/* display.c's layout card is offered only with a controller there: SDL's own list too, before the loop has seen its event */
static int pad_connected;   /* (the window loop's gamepad, for other threads) */
int window_gamepad_connected(void) { return __atomic_load_n(&pad_connected, __ATOMIC_RELAXED) || (active && SDL_HasGamepad()); }
/* keyboard or mouse tried to play the game in the PS TV layout, a controller's only: the notice why, once a run */
static int kbm_pstv_said;
static void kbm_used(void) {
	if (kbm_pstv_said || !display_pstv_active()) return;
	kbm_pstv_said = 1;
	overlay_toast_titled("Controller layout", "The PS TV layout is for a controller: keyboard and mouse don't play the game in it. For them, choose touch controls when the game starts (or [F10] > Game > Controller layout).", 10.0);
}
static void pad_resync(SDL_Gamepad *pad) {   /* L and R as held now (the menu closing): no event says so */
	for (int k = 0; k < 2; k++) {
		ptrig[k] = pad && SDL_GetGamepadAxis(pad, k ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000;
		if (pad) pad_main(pad, k, shoulders_swapped ? SDL_GetGamepadButton(pad, k ? SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER : SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) : ptrig[k]);
	}
}

typedef struct { void *(*fn)(void *); void *arg; } Game;
static void *game_main(void *p) { Game *g = p; g->fn(g->arg); return 0; }

/* (re)create the GPU-side texture + transfer buffer that the CPU frame is uploaded into, when its
 * size changes; returns 0 if the GPU texture isn't ready (e.g. device/window not yet claimed) */
static SDL_GPUTexture *src_tex;
static SDL_GPUTransferBuffer *xfer;
static uint32_t stw, sth;
static int ensure_src_texture(uint32_t w, uint32_t h) {
	if (src_tex && stw == w && sth == h) return 1;
	if (src_tex) SDL_ReleaseGPUTexture(gpu, src_tex);
	if (xfer) SDL_ReleaseGPUTransferBuffer(gpu, xfer);
	SDL_GPUTextureCreateInfo tci = {
		.type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = w, .height = h, .layer_count_or_depth = 1, .num_levels = 1,
		.sample_count = SDL_GPU_SAMPLECOUNT_1,
	};
	src_tex = SDL_CreateGPUTexture(gpu, &tci);
	SDL_GPUTransferBufferCreateInfo xci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = 4 * w * h };
	xfer = src_tex ? SDL_CreateGPUTransferBuffer(gpu, &xci) : 0;
	stw = w; sth = h;
	return src_tex && xfer;
}

/* RECOMP_VIRTUAL_PAD=1: a virtual standard gamepad (SDL3's virtual joystick, type GAMEPAD) the window loop
 * opens exactly like a physical pad -- the end-to-end check of the gamepad path (SDL event -> pad_bit ->
 * the game's SceCtrl read) with no hardware: RECOMP_VIRTUAL_PAD_SCRIPT="from-to:button,..." presses
 * SDL_GAMEPAD_BUTTON_* indices (0 south/A, 1 east/B, 2 west/X, 3 north/Y, 6 back, 7 start, 11-14 dpad). */
static SDL_Joystick *vpad;
static const char *vpad_script;
static void vpad_init(void) {
	if (!getenv("RECOMP_VIRTUAL_PAD")) return;
	SDL_VirtualJoystickDesc d; SDL_INIT_INTERFACE(&d);
	d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
	d.naxes = SDL_GAMEPAD_AXIS_COUNT; d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
	d.button_mask = 0xffffffffu; d.axis_mask = 0xffffffffu;
	d.name = "Virtual Xbox Controller"; d.vendor_id = 0x045e; d.product_id = 0x028e;
	SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
	if (id) { vpad = SDL_OpenJoystick(id); fprintf(stderr, "    virtual pad: %s\n", vpad ? "attached" : SDL_GetError()); }
	else fprintf(stderr, "    virtual pad: SDL_AttachVirtualJoystick failed: %s\n", SDL_GetError());
	vpad_script = getenv("RECOMP_VIRTUAL_PAD_SCRIPT");
}
static void vpad_update(void) {
	if (!vpad || !vpad_script) return;
	extern uint64_t display_frames(void); uint64_t f = display_frames();
	/* several ranges may tell the same button's press history: OR them for this frame (a single sequential write
	 * made a second press of the same button impossible, which is exactly what a menu walk needs) */
	static unsigned char have[SDL_GAMEPAD_BUTTON_COUNT];
	unsigned char next[SDL_GAMEPAD_BUTTON_COUNT];
	SDL_memset(next, 0, sizeof next);
	for (const char *q = vpad_script; q && *q; ) {
		unsigned long a = strtoul(q, (char **)&q, 10), z = a, b = 0;
		if (*q == '-') z = strtoul(q + 1, (char **)&q, 10);
		if (*q == ':') b = strtoul(q + 1, (char **)&q, 10);
		if (b < SDL_GAMEPAD_BUTTON_COUNT && f >= a && f <= z) next[b] = 1;
		if (*q == ',') q++; else break;
	}
	for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
		if (next[b] == have[b]) continue;
		have[b] = next[b];
		SDL_SetJoystickVirtualButton(vpad, b, next[b] ? true : false);
	}
}
/* run fn(arg) as the game; with a window, on its own thread while this one shows frames */
void window_run(void *(*fn)(void *), void *arg) {
	const char *hl = getenv("RECOMP_HEADLESS");
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   /* a pad stays live while the window is unfocused */
	if ((hl && *hl && *hl != '0') || !SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
		if (!(hl && *hl && *hl != '0')) fprintf(stderr, "no window (%s): running headless\n", SDL_GetError());
		/* RECOMP_GFX=gpu needs an SDL_GPUDevice even here: gfx_gpu.c never presents to a window
		 * swapchain (window.c's own present loop below does that, for the software backend's
		 * frame, and isn't reached in headless mode) -- it only renders into its own offscreen
		 * colour/depth textures and reads them back for RECOMP_SHOT/window_present, neither of
		 * which needs a window. SDL_GPU itself still needs SDL_INIT_VIDEO initialised. */
		extern void *gfx_gpu_create_device(void);
		if (SDL_Init(SDL_INIT_VIDEO)) gpu = gfx_gpu_create_device();
		fn(arg);
		return;
	}
	int scale = getenv("RECOMP_SCALE") ? atoi(getenv("RECOMP_SCALE")) : 1;
	if (scale < 1) scale = 1;
	{ /* output size: RECOMP_SCALE=n default 960n x 544n; RECOMP_RES=2560x1440 (2K) or RECOMP_FULLSCREEN=1 for
	     * a borderless full-desktop window. The game still renders its original 960x544 picture and the present
	     * blit scales it up (the original code decides the picture; the output resolution is the HLE's). */
	  int ww = 960 * scale, wh = 544 * scale, fs = 0;
	  const char *res = getenv("RECOMP_RES");
	  if (res && *res) { int a = 0, b = 0; if (sscanf(res, "%dx%d", &a, &b) == 2 && a >= 640 && b >= 360) { ww = a; wh = b; } }
	  { const char *f = getenv("RECOMP_FULLSCREEN"); if (f && *f && atoi(f)) fs = 1; }
	  if (fs) { SDL_DisplayID did = SDL_GetPrimaryDisplay(); const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(did); if (dm) { ww = dm->w; wh = dm->h; } }
	  else { /* M1639: a windowed frame larger than the display would hang off the screen (a 4K choice on a 2K display):
	          * clamp it, the same picture downscaled, and say so */
	    SDL_DisplayID did = SDL_GetPrimaryDisplay(); const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(did);
	    if (dm && (ww > dm->w || wh > dm->h)) {
	      int nw = ww, nh = wh;
	      if (ww > dm->w) { nh = (int)((double)wh * dm->w / ww); nw = dm->w; }
	      if (nh > dm->h) { nw = (int)((double)nw * dm->h / nh); nh = dm->h; }
	      fprintf(stderr, "    window: %dx%d is larger than the display %dx%d: using %dx%d\n", ww, wh, dm->w, dm->h, nw, nh);
	      ww = nw; wh = nh;
	    } }
	  const char *name = getenv("RECOMP_GAME_NAME");
	  win = SDL_CreateWindow(name && *name ? name : "P5D 移植版", ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (fs ? SDL_WINDOW_BORDERLESS : 0));
	  fprintf(stderr, "    window: %dx%d%s\n", ww, wh, fs ? " borderless fullscreen" : "");
	}
	if (win) SDL_ShowCursor();   /* a PC game's pointer: visible over the picture (P5D is not mouse-look) */
	{	/* the app icon (assets/brand): the install's, as vitarecomp run names it (an AppImage's moves on every start), else
		 * the one the build saw */
		const char *ip = getenv("RECOMP_ICON");
#ifdef RECOMP_ICON
		if (!ip) ip = RECOMP_ICON;
#endif
		SDL_Surface *icon = ip ? SDL_LoadPNG(ip) : NULL; if (icon) { SDL_SetWindowIcon(win, icon); SDL_DestroySurface(icon); }
	}
	SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	{ extern void *gfx_gpu_create_device(void); gpu = gfx_gpu_create_device(); }
	if (gpu) SDL_ClaimWindowForGPUDevice(gpu, win);
	if (gpu) {
		/* the swapchain's present mode. On Vulkan, mailbox where the display has it: with vsync (fifo) this loop's submit
		 * waits for the swapchain image on the GPU's one queue, and every scene the game submits after it waits out the
		 * refresh too. Killzone's CPU reads of its exposure target waited ~6 ms each: 40 fps windowed against 66 headless
		 * (RTX laptop, 2026-10-26), 47 with mailbox. Mailbox doesn't block, so this loop paces itself (self_paced, below).
		 * Metal blocks the CPU in nextDrawable instead, not the queue: vsync there. RECOMP_PRESENT_MODE=fifo|mailbox|immediate */
		const char *pe = getenv("RECOMP_PRESENT_MODE"), *dv = SDL_GetGPUDeviceDriver(gpu);
		/* (with mailbox, Killzone at 3x and 21:9 lost the device in about half its runs: the FRAGCOLOR copy, a blit since,
		 * gfx_gpu.c fetch_blit) */
		SDL_GPUPresentMode pm = pe ? (!strcmp(pe, "immediate") ? SDL_GPU_PRESENTMODE_IMMEDIATE : !strcmp(pe, "mailbox") ? SDL_GPU_PRESENTMODE_MAILBOX : SDL_GPU_PRESENTMODE_VSYNC)
		                           : dv && !strcmp(dv, "vulkan") ? SDL_GPU_PRESENTMODE_MAILBOX : SDL_GPU_PRESENTMODE_VSYNC;
		if (pm != SDL_GPU_PRESENTMODE_VSYNC && SDL_WindowSupportsGPUPresentMode(gpu, win, pm) && SDL_SetGPUSwapchainParameters(gpu, win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, pm)) self_paced = 1;
		fprintf(stderr, "    window: present mode %s\n", !self_paced ? "vsync" : pm == SDL_GPU_PRESENTMODE_MAILBOX ? "mailbox" : "immediate");
	}
	/* frames the swapchain may have queued for presentation (SDL's default 2): each one is a refresh
	 * between a game's flip and the screen. 1 here -- with the frame picked after the drawable wait
	 * (the present loop) -- is the Vita's single flip; games time their audio against that (Uncharted's
	 * cutscene dialogue ran ~47 ms ahead of the mouths with 2 and the frame picked first, 2026-10-09).
	 * settings "frames_in_flight" 1..3 (RECOMP_FRAMES_IN_FLIGHT wins) */
	if (gpu) {
		frames_in_flight = getenv("RECOMP_FRAMES_IN_FLIGHT") ? atoi(getenv("RECOMP_FRAMES_IN_FLIGHT")) : settings_int("frames_in_flight", 1);
		if (frames_in_flight < 1) frames_in_flight = 1; if (frames_in_flight > 3) frames_in_flight = 3;
		if (!SDL_SetGPUAllowedFramesInFlight(gpu, (Uint32)frames_in_flight)) { fprintf(stderr, "frames in flight %d refused (%s): SDL's 2\n", frames_in_flight, SDL_GetError()); frames_in_flight = 2; }
	}
	if (!gpu) fprintf(stderr, "no GPU device (%s): presenting nothing\n", SDL_GetError());
	/* the overlay's own settings (its Display page), read once here the same way render_scale_init
	 * (gfx_gpu.c) reads "render_scale": the environment doesn't override these two, there being no
	 * scripted test that cares what the window looks like */
	{ char b[16]; settings_str("aspect", "fit", b, sizeof b);
	  aspect_mode = !strcmp(b, "stretch") ? ASPECT_STRETCH : !strcmp(b, "fill") ? ASPECT_FILL : ASPECT_FIT; }   /* window_set_aspect also saves: read it in directly instead, nothing changed yet */
	if (settings_int("fullscreen", 0)) SDL_SetWindowFullscreen(win, true);
	if (gpu) overlay_init(gpu, win, SDL_GetGPUSwapchainTextureFormat(gpu, win));
	SDL_Gamepad *pad = 0;
	vpad_init();
	p5d_load();
	{ int n = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n);
	  fprintf(stderr, "gamepads seen: %d\n", n);
	  for (int i = 0; i < n; i++) { const char *nm = SDL_GetGamepadNameForID(ids[i]); fprintf(stderr, "  gamepad %u: %s\n", (unsigned)ids[i], nm ? nm : "?"); }
	  if (ids) SDL_free(ids); }
	int pad_back = 0, pad_start = 0, pad_combo_latch = 0;
	uint64_t last_overlay_t = 0;
	active = 1;
	Game g = {fn, arg};
	pthread_t t;
	pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
	pthread_create(&t, &at, game_main, &g);
	for (;;) {
		SDL_Event e;
		vpad_update();
		p5d_menu_tick();
		/* (the selection-index discovery experiment is retired: the main menu is wheel-driven now) */
		p5d_hover_tick();
		{ /* the dance stage uses the [buttons_game] map. The modelled menu must be STABLE before either switch:
		     * one false-positive frame in a cutscene used to flip the map back and forth every couple of seconds,
		     * which left the dance keys unmapped half the time. */
		  static uint64_t song_seen_us, game_since_us, mod_change_us, menu_since_us;
		  static int last_mod = -1;
		  uint64_t t = now_us();
		  /* a NONE screen with the menu layer still armed (menu_win_until) is a SUBMENU page (COMMU, CONFIG,
		   * COLLECTION, SYSTEM, the pause menu): the game draws such pages without a modelled screen, and the
		   * auto switch to the dance map used to land on them 2.5 s in -- the hints reverted to the pad icons
		   * and the keyboard keys stopped matching. Only a song confirm (leaving the song list towards the
		   * dance) suppresses the hold; the RESULT/main-menu scanners clear it once the dance is over. */
		  int submenu = p5d_profile == P5D_MENU && p5d_ui == P5D_UI_NONE && p5d_saw_menu && !song_confirm_us && t < menu_win_until;
		  /* the dance key map follows the song gauge itself: it appears with the first dance frame, so
		   * the six keys are live exactly while the dance stage is, and never on a submenu page (the
		   * COMMU pages' Q/E must not turn into the dance keys' Q). */
		  int dance_now = p5d_dance_bar_us && t - p5d_dance_bar_us < 600000;
		  /* the gauge detector also trips on the COMMU pages' orange progress bars: only a stage that was actually
		   * entered (song confirmed) may latch the dance map, or the COMMU pages lost their Q/E L/R shoulders to
		   * the dance keys and the L/R keycaps (reported 2026-11-07) */
		  if (dance_now && (p5d_profile == P5D_GAME || p5d_scene == P5D_SCENE_DANCE) && !p5d_dance_flow(t)) p5d_dance_flow_start(t);   /* the stage began some other way: the gauge is the code-level proof */
		  { static int fw = -1; if (fw < 0) fw = getenv("RECOMP_FORCE_WRONG_PROFILE") ? 1 : 0;
		    if (fw && p5d_dance_flow(t) && t - p5d_dance_flow_us < 600000 && p5d_profile == P5D_GAME) { p5d_switch_profile(P5D_SONG); fprintf(stderr, "[TEST] forced wrong profile (SONG) inside the dance flow\n"); } }
		  int dance_seen = dance_now && (song_confirm_us || p5d_dance_latch || p5d_dance_flow(t));
		  if (dance_seen) { p5d_dance_latch = 1; p5d_dance_absent_us = 0; }   /* (the gauge is drawn behind the pause menu too: only the pause picture decides when the pause closes) */
		  /* only the result screen is positively not paused; MAIN/SONG are the stage's own flickering read at 2K (the pause
	 * menu is drawn over the stage), so they are left to the pause picture's own auto-close (reported 2026-11-08:
	 * pressing Esc on the stage closed the pause latch one frame later, so the wheel/click never reached the menu) */
	if (p5d_pause_latch && p5d_ui == P5D_UI_RESULT) p5d_pause_latch = 0;
		  else if (p5d_dance_latch) {
			if (!p5d_dance_absent_us) p5d_dance_absent_us = t;
			if (t - p5d_dance_absent_us > 8000000) p5d_dance_latch = 0;   /* the gauge gone 8 s: the stage is over (result screens included) */
		  }
		  /* the song list is back to stay (a story frame or the "LET'S DANCE" confirm trips the template for a
		   * moment; a steady song list means the stage ended). Not while the gauge is up: then it is just behind
		   * the dance overlay. */
		  if (p5d_ui == P5D_UI_SONG) { if (!p5d_song_since_us) p5d_song_since_us = t; }
		  else p5d_song_since_us = 0;
		  if (song_confirm_us && p5d_song_since_us && !dance_now && t - p5d_song_since_us > 2500000) { song_confirm_us = 0; p5d_dance_flow_us = 0; }
		  else if (p5d_dance_flow_us && p5d_ui == P5D_UI_SONG && !dance_now && p5d_song_since_us && t - p5d_song_since_us > 2500000) p5d_dance_flow_us = 0;
		  /* the CONFIG map is LATCHED for the whole settings session, not just while the HELP-badge
		   * template is on screen: the 恢復預設 confirmation dialog replaces that screen, and the atlas is
		   * re-patched from the current profile, so an unlocked map flipped the ALT keycap back to the PS
		   * START badge while the dialog was open. The latch clears only when a modelled menu screen (or
		   * the dance gauge) is back AND the CONFIG template is gone -- never while it is still on screen,
		   * so a MAIN false hit on the settings screen cannot clear it either. */
		  int story_now = p5d_story_us && t - p5d_story_us < 900000;
		  if (story_now) { p5d_story_latch = 1; p5d_story_absent_us = 0; }
		  else if (p5d_story_latch) {
			if (!p5d_story_absent_us) p5d_story_absent_us = t;
			if (t - p5d_story_absent_us > 2500000) p5d_story_latch = 0;
		  }
		  int story_hold = p5d_story_latch;
		  if (story_hold) p5d_config_latch = 0;   /* a story scene is not the settings page */
		  static uint64_t cfg_first_us;
		  /* RECOMP_FAKE_MISREAD=1 (a test harness): every 40 frames flip the screen model to SONG/MAIN for 3 frames --
		   * the exact failure mode the launcher build hit inside the dance (a transient misread must not clear the stage) */
		  { static int fm = -1; if (fm < 0) fm = getenv("RECOMP_FAKE_MISREAD") ? 1 : 0;
		    if (fm) { extern uint64_t display_frames(void); uint64_t f = display_frames();
		      if ((f % 40) < 3) { p5d_ui = ((f / 40) % 2) ? P5D_UI_MAIN : P5D_UI_SONG; p5d_ui_valid_us = now_us(); } } }
		  int cfg_seen = p5d_config_us && t - p5d_config_us < 900000;
		  int cfg_seen_hi = p5d_config_hi_us && t - p5d_config_hi_us < 900000;
		  if (cfg_seen) { if (!cfg_first_us) cfg_first_us = t; } else cfg_first_us = 0;
		  /* the settings screen's HELP badge is shared with the other submenu pages, and story frames have tripped
		   * it too: in the menus only the strict match and a short hold may switch the map; inside the dance stage
		   * the loose match plus a 1 s hold is the pause menu's settings page (reported 2026-11-07) */
		  if ((!story_hold || p5d_pause_latch) && t > p5d_config_ignore_us && cfg_seen_hi && cfg_first_us && t - cfg_first_us > 400000 && p5d_profile != P5D_GAME) p5d_config_latch = 1;
		  else if (!p5d_pause_latch && !story_hold && p5d_dance_latch && !dance_now && cfg_seen && cfg_first_us && t - cfg_first_us > 1000000 && (!p5d_pause_closed_us || t - p5d_pause_closed_us > 1500000)) p5d_config_latch = 1;
		  if (p5d_config_latch && p5d_profile == P5D_GAME && !cfg_seen) p5d_config_latch = 0;
		  if (p5d_config_latch && !cfg_seen && (p5d_ui == P5D_UI_MAIN || p5d_ui == P5D_UI_SONG || p5d_ui == P5D_UI_RESULT)) p5d_config_latch = 0;
		  if (p5d_config_latch && dance_now && !p5d_pause_latch) p5d_config_latch = 0;
		  int cfg_now = p5d_config_latch;
		  int mod = (p5d_ui == P5D_UI_MAIN || p5d_ui == P5D_UI_SONG || p5d_ui == P5D_UI_RESULT) || submenu || cfg_now;
		  if (mod != last_mod) { last_mod = mod; mod_change_us = t; if (mod) menu_since_us = t; }
		  if (mod) last_menu_us = t;
		  if (p5d_ui == P5D_UI_SONG) { song_seen_us = t; last_song_us = t; }
		  /* the COMMU logo crop also scores on the song list's red header, so only trust it on an unmodelled screen
		   * and away from the song list (else the song list lost its Q/E D-pad -- regression found 2026-11-07) */
		  int commu_now = p5d_commu_us && t - p5d_commu_us < 900000;
		  (void)commu_now;
		  (void)p5d_commu_absent_us;
		  if (p5d_ui != P5D_UI_NONE && p5d_ui != P5D_UI_MAIN) p5d_commu_latch = 0;   /* a modelled screen: not a COMMU page */
		  /* entering the COMMU pages from the main menu: the row's confirm is the instant signal (the page's logo
		   * template settles a few seconds later, which left the hint icons and the keys on the menu map --
		   * reported 2026-11-07) */
		  { static uint32_t prev_btns; uint32_t bn = buttons;
		    if ((bn & CIRCLE) && !(prev_btns & CIRCLE) && p5d_ui == P5D_UI_MAIN) {
			int rh = p5d_ui_cur >= 0 ? p5d_ui_cur : p5d_menu_row_hint();
			if (rh < 0) rh = p5d_banner_row();
			if (rh >= 0) p5d_song_flow_armed = (rh == 0);   /* only a DANCING-row confirm enters the song flow */
			if (rh == 0) { p5d_song_latch = 1; p5d_song_mode = 1; p5d_commu_mode = 0; p5d_scene_set(P5D_SCENE_SONG, "DANCING row (edge)"); p5d_scene_lock = 1; }
			else if (rh == 2) { p5d_song_flow_armed = 0; p5d_song_latch = 0; p5d_commu_latch = 1; p5d_commu_latch_us = t; p5d_commu_mode = 1; p5d_song_mode = 0; p5d_scene_set(P5D_SCENE_COMMU, "COMMU row (edge)"); p5d_scene_lock = 2; }
			else if (rh == 4) { p5d_config_latch = 1; }   /* the CONFIG row: the settings page's D-pad */
			else if (rh >= 0 && !(p5d_song_mode && p5d_ui_cur < 0)) { p5d_song_mode = 0; p5d_commu_mode = 0; if (rh > 0) p5d_song_flow_armed = 0; }   /* (a stale main-menu row must not clear the song flow: at 2K/3x the model reports MAIN on the song list itself) */
			else if (p5d_commu_mode) { p5d_commu_mode = 0; }
		    }
		    prev_btns = bn; }
		  if (p5d_ui == P5D_UI_SONG || p5d_ui == P5D_UI_RESULT || p5d_ui == P5D_UI_TITLE) { p5d_commu_latch = 0; p5d_commu_exit_us = 0; }
		  /* leaving the COMMU pages: a back (cross) press, then the main menu really back (the COMMU list itself can
		   * look like the main menu, so a bare MAIN match must not drop the map -- reported 2026-11-07) */
		  { static uint32_t prev_cb; uint32_t cb = buttons;
		    if ((cb & CROSS) && !(prev_cb & CROSS) && p5d_profile == P5D_COMMU) p5d_commu_exit_us = t;
		    prev_cb = cb; }
		  if (p5d_commu_exit_us && p5d_ui == P5D_UI_MAIN && t - p5d_commu_exit_us > 600000) { p5d_commu_latch = 0; p5d_commu_exit_us = 0; }
		  if (p5d_ui == P5D_UI_TITLE || p5d_ui == P5D_UI_RESULT) { p5d_song_latch = 0; p5d_song_flow_armed = 0; }   /* (the Q/E modes follow only the menu row confirmations and the back button) */
		  /* the song-list model may set the D-pad mode, but never while the COMMU flow is open (the COMMU lists
		   * look like the song list to that template and would otherwise lose their L/R shoulders) */
		  if (p5d_ui == P5D_UI_SONG && !p5d_commu_mode) p5d_song_mode = 1;
		  if (p5d_profile == P5D_GAME && p5d_ui == P5D_UI_NONE) { p5d_song_latch = 0; p5d_song_mode = 0; }   /* a real dance stage has its own map (the song list's background is not one) */
		  if (p5d_ui == P5D_UI_MAIN && p5d_ui_cur >= 0 && t - p5d_commu_latch_us > 2000000) p5d_commu_latch = 0;   /* back on the main menu */
		  int commu_hold = (p5d_commu_us && t - p5d_commu_us < 3000000) ||   /* the L/R badges were on screen */
		                   (p5d_commu_latch && t - p5d_commu_latch_us > 0);   /* or the COMMU row was confirmed (badges may be hidden by a stale map) */
		  if (p5d_ui == P5D_UI_SONG && !dance_now && !p5d_dance_flow(t) && !(p5d_dance_job_us && t - p5d_dance_job_us < 2000000)) { p5d_song_latch = 1; p5d_dance_latch = 0; p5d_story_latch = 0; p5d_commu_latch = 0; }   /* the song list is modelled: no stage state may leak in -- but a single mis-read dance frame is not the song list (the gauge is up) */
		  int song_now = p5d_song_latch || p5d_ui == P5D_UI_SONG;   /* set by the DANCING row confirm, cleared by the dance stage/result */
		  /* a modelled menu screen is definitive: no stage/pause/settings state from an earlier screen may linger
		   * (waiting for a stable highlight left the dance map active on the menu for up to a second: wrong keys
		   * and a profile switch per frame -- reported 2026-11-07) */
		  if (p5d_ui == P5D_UI_TITLE || p5d_ui == P5D_UI_RESULT) p5d_dance_flow_us = 0;   /* the stage is over: back to the ordinary screens */
		  if ((p5d_ui == P5D_UI_MAIN || p5d_ui == P5D_UI_TITLE || p5d_ui == P5D_UI_RESULT) && !dance_now && !p5d_dance_flow(t)) {
			p5d_dance_latch = 0; p5d_story_latch = 0; p5d_song_latch = 0; song_confirm_us = 0;   /* (the Q/E modes stay: they follow the main-menu row confirmations) */
			if (!(p5d_pause_latch && now_us() - p5d_pause_pic_us < 1200000)) { p5d_pause_latch = 0; p5d_config_latch = 0; p5d_config_from_pause = 0; }
		  } else if (p5d_ui == P5D_UI_SONG && !dance_now) {
			p5d_dance_latch = 0; p5d_story_latch = 0;
			if (!(p5d_pause_latch && now_us() - p5d_pause_pic_us < 1200000)) { p5d_pause_latch = 0; p5d_config_latch = 0; p5d_config_from_pause = 0; }
		  }
		  /* the game's own named app processes decide the scene when one was looked up/created recently: that is
		   * the code-level scene identity (commuMain / MusicSelectMain / mainMenuP5 / result / mns / ...) */
		  { const char *jn = p5d_job_name_fresh(t);
			if (jn && jn[0]) {
				static char last_jn[48];
				if (strcmp(jn, last_jn) != 0) { fprintf(stderr, "[JOBNAME] %s\n", jn); snprintf(last_jn, sizeof last_jn, "%s", jn); }
				/* (the name stream is being calibrated first: log only - forcing scenes from the last job
				 * name is not safe because background jobs are looked up continuously) */
				(void)jn;
			}
		  }
		  /* the game's own job/process NAME stream is the code-level scene identity (verified names: MusicSelectMain /
		   * music_select_notify / commuMain / ConfigStartEffect / main menu p5 / title sequence / mns / result ...).
		   * It fires exactly when a scene's process is looked up or created, so it covers the first frames of a
		   * screen change where the picture model has not caught up yet. */
		  { const char *jn = p5d_job_name_fresh(t);
			if (jn && jn[0]) {
				/* (the song-list job also gets looked up WHILE the dance stage runs: MusicSelect's notify/prefetch jobs
				 * fire during the performance, which flipped the scene SONG 42 times in a 100 s session and made the
				 * profile flicker hint/GUI state every second. The dance stage owns the scene once the game profile is
				 * on and the dance latch is set; the song list is re-identified when it is really back.) */
				if (strstr(jn, "MusicSelect") || strstr(jn, "music_select")) { p5d_song_job_us = t; p5d_song_ev_us = t;
					if (!(p5d_dance_flow(t) || (p5d_profile == P5D_GAME && (p5d_ui == P5D_UI_NONE || p5d_dance_latch)))) { p5d_scene_set(P5D_SCENE_SONG, "job MusicSelect"); p5d_scene_lock = 1; p5d_song_ev_us = t; p5d_song_mode = 1; }
				}
				else if (strstr(jn, "commu")) { p5d_commu_job_us = t; p5d_scene_set(P5D_SCENE_COMMU, "job commuMain"); p5d_scene_lock = 2; }
				else if (strstr(jn, "Config") || strstr(jn, "konfig") || strstr(jn, "Configenu")) { p5d_config_job_us = t; if (!p5d_scene_lock) p5d_scene_set(P5D_SCENE_PAUSE, "job Config"); }
				else if (strstr(jn, "title sequence") || strstr(jn, "movie play") || strstr(jn, "initDataRead")) { if (!p5d_scene_lock && (p5d_scene == P5D_SCENE_BOOT || p5d_scene == P5D_SCENE_TITLE)) p5d_scene_set(P5D_SCENE_TITLE, "job title"); }
				else if (strstr(jn, "result")) { p5d_result_job_us = t; if (!p5d_scene_lock) p5d_scene_set(P5D_SCENE_RESULT, "job result"); }
				else if (strcmp(jn, "mns") == 0) { if (!p5d_scene_lock) p5d_scene_set(P5D_SCENE_DANCE, "job mns"); }
				else if (strstr(jn, "replay dance") || strstr(jn, "danceVoiceSub") || strstr(jn, "Dancig") || strstr(jn, "Dancing")) {
				p5d_dance_job_us = t; p5d_dance_latch = 1; p5d_dance_absent_us = 0;   /* the dance stage's own jobs */
			}
			else if (strstr(jn, "main menu")) { p5d_main_job_us = t; if (!p5d_scene_lock && p5d_scene != P5D_SCENE_SONG && p5d_scene != P5D_SCENE_COMMU) p5d_scene_set(P5D_SCENE_MAIN, "job main menu"); }
			}
		  }
		  if (p5d_ui == P5D_UI_SONG) { p5d_song_ev_us = t;   /* the song list is on screen: its Q/E are the D-pad */
		if (!(p5d_commu_screen_us && t - p5d_commu_screen_us < 1500000)) p5d_song_mode = 1; }
	  /* (no clear here: the COMMU-badge detector also matches the song list's own hint bar) */   /* the COMMU L/R keycaps are up: not the song flow */
	  { static uint32_t k; if ((++k & 3u) == 0 && p5d_song_process_alive()) p5d_song_ev_us = t; }   /* the code-level song-list job */
	  /* M1628: the pause menu leaves on the GAME's side (mouse confirm, its own back row) and at 2K the picture
	   * screen model is useless (ui=NONE for the whole stage), so the pause/config latches used to stay set: the scene
	   * stayed PAUSE, the input profile stayed CONFIG and the dance keys (Q/A/Z/O/K/M) were dead after pausing once.
	   * Two model-independent ways out: the stage's own clock starts advancing again (the pause menu freezes it), or
	   * the pause picture is gone while the stage/result job evidence says the page is over. */
	  if (p5d_pause_latch) {
		extern float p5d_dance_clock_now(void);
		static float clk0; static int have_clk0;
		float ck = p5d_dance_clock_now();
		if (!have_clk0) { clk0 = ck; have_clk0 = 1; }
		int page_visible = now_us() - p5d_pause_pic_us < 200000;
		if (page_visible) clk0 = ck;   /* the pause page is on screen: its clock is frozen, keep the baseline on it */
		/* M1633: a real resume needs the pause page GONE for 0.6 s AND the stage clock running on since the last frame the
		 * page was visible. The pause opening animation advances the clock for a few frames; with the old "clock advanced
		 * 0.10 s" test that cleared the latch by itself 0.2 s after opening (seen in a test run:
		 * "[PAUSE] clock resumed (clk 3.482 -> 3.594)" right after the pause opened). */
		int page_gone = now_us() - p5d_pause_pic_us > 600000;
		int resumed = (page_gone && ck - clk0 > 0.15f) || clk0 - ck > 1.0f;
		/* a quit out of the pause menu (its last row / the result TALLY): only the result job appearing while the
		 * dance jobs have stopped is trusted. MusicSelect prefetch jobs run during the whole stage, so they must not
		 * be used here (they fired this clear 1.2 s into a pause once: the wheel and the pause settings' Q/E died). */
		int quit = (p5d_result_job_us && t - p5d_result_job_us < 1000000) &&
		           (p5d_dance_job_us && t - p5d_dance_job_us > 1500000);
		if (resumed || quit) {
			p5d_pause_close_us = 0;
			fprintf(stderr, "[PAUSE] %s: the game left the pause page (clk %.3f -> %.3f) -- latches cleared\n",
			        resumed ? "clock resumed" : "stage quit", clk0, ck);
			p5d_pause_latch = 0; p5d_config_latch = 0; p5d_config_from_pause = 0; p5d_pause_close_us = 0;
			p5d_pause_closed_us = now_us(); menu_win_until = 0; have_clk0 = 0;
			/* nothing queued for a menu may reach the game after the pause: drop the tap sequences, the macro tap and
			 * any stale held mouse/touch state here (a late Down/Circle tap used to land in the dance stage as notes) */
			p5d_tapq_n = 0; p5d_tapq_i = 0; p5d_pause_back_seq = 0;
			p5d_settings_close_step = 0; p5d_settings_close_us = 0;
			menu_tap_bits = 0; menu_target = -1; menu_enter = 0; pending_row = -1;
			touch_down = 0; p5d_mouse_bits = 0;
		}
	  }
	  /* the scene state: corrected by the evidence every frame, never used to *drive* the picture model */
		  if (p5d_pause_latch) { if (p5d_scene != P5D_SCENE_PAUSE) p5d_scene_set(P5D_SCENE_PAUSE, "pause state"); }
		  else if (!p5d_scene_lock && p5d_ui == P5D_UI_TITLE) p5d_scene_set(P5D_SCENE_TITLE, "title modelled");
		  else if (!p5d_scene_lock && p5d_ui == P5D_UI_RESULT) p5d_scene_set(P5D_SCENE_RESULT, "result modelled");
		  else if (p5d_app_state_code() < 0 && p5d_commu_us && t - p5d_commu_us < 800000) { p5d_scene_set(P5D_SCENE_COMMU, "COMMU keycaps (fallback)"); p5d_scene_lock = 2; }
		  else if (p5d_scene == P5D_SCENE_SONG && p5d_ui != P5D_UI_SONG && t - p5d_main_job_us < 500000 && t - p5d_song_ev_us > 1500000 && !p5d_song_process_alive() && !dance_now && !p5d_dance_flow(t)) {
			/* the song flow is over (no MusicSelect job, no song-list picture, no dance): the main menu / another
			 * page owns the scene. The main-menu entry used to be misread once as SONG, and its scene lock then
			 * kept every menu page in the "song list" state (Q/E stuck as the D-pad) -- reported 2026-11-08 */
			p5d_scene_set(P5D_SCENE_MAIN, "song flow gone"); p5d_scene_lock = 0;
			p5d_song_mode = 0; p5d_song_latch = 0; p5d_song_flow_armed = 0; }
		  else if (!p5d_scene_lock && p5d_ui == P5D_UI_MAIN && p5d_ui_cur >= 0 && t - p5d_scene_set_us > 800000) p5d_scene_set(P5D_SCENE_MAIN, "main modelled");
		  else if (!p5d_scene_lock && p5d_ui == P5D_UI_SONG) { p5d_scene_set(P5D_SCENE_SONG, "song modelled"); p5d_scene_lock = 1; }
		  else if (p5d_profile == P5D_GAME && p5d_ui == P5D_UI_NONE) { p5d_scene_lock = 0; p5d_scene_set(P5D_SCENE_DANCE, "dance stage"); }
		  { static int last_sc = -1; if (last_sc != p5d_scene) { last_sc = p5d_scene;
			if (p5d_scene == P5D_SCENE_MAIN) p5d_nav_set(6, 0);
			else if (p5d_scene == P5D_SCENE_SONG) p5d_nav_set(4, 0);
			else if (p5d_scene == P5D_SCENE_RESULT) p5d_nav_set(4, 0);
			else if (p5d_scene == P5D_SCENE_PAUSE) p5d_nav_set(5, 0);
			else if (p5d_scene == P5D_SCENE_COMMU) p5d_nav_set(5, 0);
		  } }
		  { static int cand = -1, cand_n; int scr = p5d_input_screen();
		{ static uint64_t proc_scan_us; if (t - proc_scan_us > 150000) { proc_scan_us = t; p5d_proc_mark(t); } }
		/* the settings page is identified by the game's own ConfigStartEffect job (works from the main menu and from
		 * both pause menus, story scenes included); the picture template below stays only a fallback */
		if (p5d_pause_latch && p5d_config_job_us && t - p5d_config_job_us < 900000) p5d_config_latch = 1;
		/* (the pause-menu job is not a latch source: it stays alive during the dance stage; the latch comes from
		 * our own Esc/Return open and our own exit paths) */
		if (scr != cand) { cand = scr; cand_n = 1; } else if (cand_n < 3) cand_n++;
		if (cand_n >= 3 && scr != p5d_scr_now) {
			int prev_scr = p5d_scr_now;
			p5d_scr_now = scr;
			{ extern uint64_t display_frames(void);
		fprintf(stderr, "[SCR] f=%llu input screen -> %s\n", (unsigned long long)display_frames(), p5d_input_screen_name(scr)); }
			/* back on the main menu: if we just came out of one of its sub-pages the game keeps that row highlighted,
			 * so the counter must keep it too (only a fresh entry from title/result/dance starts at row 0) */
			int from_sub = (prev_scr == P5D_SCR_COMMU || prev_scr == P5D_SCR_SONG || prev_scr == P5D_SCR_CONFIG || prev_scr == P5D_SCR_RESULT);
			if (scr == P5D_SCR_MAIN) { if (!from_sub) p5d_nav_set(6, 0); else { p5d_nav_rows = 6; p5d_nav_us = now_us(); } }
			else if (scr == P5D_SCR_SONG) p5d_nav_set(4, 0);
			else if (scr == P5D_SCR_RESULT) p5d_nav_set(4, 0);
			else if (scr == P5D_SCR_PAUSE) p5d_nav_set(5, 0);
			else if (scr == P5D_SCR_COMMU) p5d_nav_set(5, 0);
	  } }
	  { extern void p5d_mframe_screen_tick(void); p5d_mframe_screen_tick(); }   /* M1617: frame-base index follows the screen */
	  /* M1527b: the watch screen seems to ignore the injected left stick (the right stick works), so WASD also
	   * repeats the game's own d-pad steps: many Atlus viewer modes accept the d-pad for the same move, and this
	   * fallback needs no new pad plumbing. The repeats run while a key is held (about 14 taps a second). */
	  /* WASD stay a keyboard mapping onto the PS left stick (the values are written in window_input);
	   * no touch-panel simulation here. */
	  /* experiment hook: a continuous front-touch drag while the watch mode is up (RECOMP_WATCH_DRAG_TEST="dx,dy") */
	  { static int dr = -1, dx = 0, dy = 0, curx = 0, cury = 0;
		if (dr < 0) { const char *e = getenv("RECOMP_WATCH_DRAG_TEST"); dr = e ? 1 : 0; if (e) sscanf(e, "%d,%d", &dx, &dy); }
		if (dr && p5d_collection_active() && p5d_collection_watching) {
			if (!curx && !cury) { curx = 960; cury = 544; }
			curx += dx; cury += dy;
			if (curx < 60) curx = 60; if (curx > 1860) curx = 1860;
			if (cury < 60) cury = 60; if (cury > 1028) cury = 1028;
			touch_down = 1; touch_x = curx; touch_y = cury;
		} else if (dr && !(p5d_collection_active() && p5d_collection_watching) && (curx || cury)) { touch_down = 0; curx = cury = 0; } }
	  /* deep-dive diagnostic: dump the collection-character-viewer module state's code pointers while its watch
	   * mode runs (RECOMP_DUMP_VIEWER=1), so the viewer's own update/draw functions can be identified and their
	   * input reads inspected in the recompiled C */
	  { static int dv = -1; if (dv < 0) { const char *e = getenv("RECOMP_DUMP_VIEWER"); dv = e && atoi(e) != 0; }
		if (dv && p5d_collection_active() && p5d_collection_watching) {
			static uint64_t last; uint64_t t = now_us();
			if (t - last > 1000000) { last = t;
				fprintf(stderr, "[VIEWER]");
				for (int i = 0; i < 0x100; i += 4) { uint32_t v = RD32(0x812FB280u + (uint32_t)i); if (v >= 0x81000000u && v < 0x81a00000u) fprintf(stderr, " +%02x=%#x", i, v); }
				fprintf(stderr, "\n"); } } }
	  { int cf = p5d_collection_active();
		if (!cf) { p5d_collection_depth = 0; p5d_collection_watching = 0; }
		else if (p5d_collection_depth == 0) p5d_collection_depth = 1;
		{ extern void gfx_hintglyph_refresh(void); static int last_viewer = -1, last_watch = -1;
		  int vw = window_in_collection_costume(), wt = window_in_collection_watching();
		  if (vw != last_viewer) { last_viewer = vw; gfx_hintglyph_refresh(); fprintf(stderr, "[KEYCAP] collection viewer %s\n", vw ? "on (START->Enter, Triangle->Ctrl)" : "off"); }
		  if (wt != last_watch) { last_watch = wt; gfx_hintglyph_refresh(); fprintf(stderr, "[KEYCAP] watch hints %s\n", wt ? "on (wheel zoom, Alt reset)" : "off"); }
		  if (!wt) p5d_watch_keys_clear(); }
	  }
	  /* the game's own app state decides the big scenes (COMMU/TITLE/MAIN/FREE MODE); the picture evidence below
		   * only refines the sub-screens inside them. Static: eboot 0x81185616 (sequence request) / 0x811857C2. */
		  static uint64_t main_since;
	  { static char last_name[32]; static int last_page = -2; char nm[32]; int pg = -1;
			if (p5d_proc_info(nm, sizeof nm, &pg) && (strcmp(nm, last_name) != 0 || pg != last_page)) {
				fprintf(stderr, "[PROC] process=%s page=%d\n", nm, pg);
				snprintf(last_name, sizeof last_name, "%s", nm); last_page = pg;
			} }
		  { static int last_st = -1; int st = p5d_app_state_code();
			if (st >= 0) {
				if (st != last_st) fprintf(stderr, "[APPST] app state -> %d (%s)\n", st,
					st == 3 ? "COMMU" : st == 6 ? "TITLE" : st == 5 ? "MAIN MENU" : st == 2 ? "FREE MODE" : st == 4 ? "TUTORIAL" : st == 1 ? "initread" : "DEBUG");
				last_st = st;
				{ static unsigned last_calls; static uint64_t last_us; static uint64_t last_us2;
				  unsigned c = __atomic_load_n(&p5d_mp_calls, __ATOMIC_ACQUIRE);
				  int nv;
				  if (c != last_calls) { last_calls = c; last_us = t; nv = __atomic_load_n(&p5d_mp_state, __ATOMIC_ACQUIRE); }
				  else nv = (last_us && t - last_us < 600000) ? __atomic_load_n(&p5d_mp_state, __ATOMIC_ACQUIRE) : -1;
				  (void)last_us2;
				  if (nv != p5d_mp_cur) { p5d_mp_cur = nv; fprintf(stderr, "[MENUPAGE] page -> %d\n", nv); } }
				/* (the title clear is not used: the app-state read flickers to 6 while the song list is up) */
				/* (no auto-clear of p5d_song_mode from the picture model: at 2K/3x the model reports MAIN on the song
				 * list itself, which used to clear the flag and turn that screen's Q/E back into shoulders. The
				 * flow is left by its own input intents - Backspace, the song confirm, the title.) */
				(void)main_since;
				{ static int last_mp = -2; int mp = p5d_menu_page_state(); if (mp != last_mp) { fprintf(stderr, "[MENUPAGE] menu page -> %d\n", mp); last_mp = mp; } }
				{ static int last_ms = -2; int ms = p5d_menu_state_code(); if (ms != last_ms && getenv("RECOMP_JOBDUMP")) { fprintf(stderr, "[MENUST] menu state -> %d\n", ms); last_ms = ms; } }
				{ static uint32_t last_v = 0xffffffffu; uint32_t v = RD32(0x8153f9b4u); if (v != last_v && getenv("RECOMP_JOBDUMP")) { fprintf(stderr, "[GST] 0x8153f9b4 -> %u\n", v); last_v = v; } }
				{ static int last_mps = -2; int mps = (int)__atomic_load_n(&p5d_mp_state, __ATOMIC_ACQUIRE); if (mps != last_mps && getenv("RECOMP_JOBDUMP")) { fprintf(stderr, "[MPST] mp state -> %d\n", mps); last_mps = mps; } }
				/* (the slot's screen mapping is not calibrated yet: log only, the scene machine below still decides;
				 * forcing TITLE/MAIN from it broke the song list in the 2026-11-07 experiment) */
				(void)st;
			}
		  }
		  if (p5d_settings_close_step) {
			int still = (p5d_config_latch || p5d_scr_now == P5D_SCR_CONFIG) && (p5d_pause_latch || p5d_main_settings);
			if (!still) { p5d_settings_close_step = 0; p5d_settings_close_us = 0; }
			else if (p5d_settings_close_step == 1 && now_us() - p5d_settings_close_us > 250000) {
				menu_tap(CIRCLE); p5d_settings_close_step = 0; p5d_settings_close_us = 0;
				fprintf(stderr, "[PAUSE] settings back: Circle\n");
			}

		  }
		  if (p5d_tapq_i < p5d_tapq_n && t - p5d_pause_back_us > 120000) { menu_tap(p5d_tapq[p5d_tapq_i]); p5d_tapq_i++; p5d_pause_back_us = t; }
		  if (p5d_tapq_i >= p5d_tapq_n) { p5d_pause_back_seq = 0; p5d_tapq_n = 0; }
		  int menu_stable = mod && t - mod_change_us > 800000;
		  /* the pause pages close when their picture is gone for a while (the game went on: resumed, retried, gave up):
		   * the dance gauge stays drawn behind the pause, so the picture is the signal */
		  if (p5d_pause_latch && !p5d_config_latch && (!p5d_pause_job_us || now_us() - p5d_pause_job_us > 1500000)) {   /* the pause-menu job is gone: the game resumed/closed it */
			p5d_pause_latch = 0; p5d_pause_closed_us = now_us(); menu_win_until = 0;
			if (p5d_scene == P5D_SCENE_PAUSE) p5d_scene_set(p5d_scene_prev, "pause picture gone");
			if (p5d_esc_fallback_us && now_us() - p5d_esc_fallback_us < 3000000) { menu_tap(CROSS); fprintf(stderr, "[EV] esc fallback -> back on a menu page\n"); }
			p5d_esc_fallback_us = 0;
		  }
		  if (p5d_config_latch && p5d_pause_latch) p5d_config_from_pause = 1;   /* the settings page inside the pause menu */
		  if (!p5d_config_latch) p5d_config_from_pause = 0;
		  /* leaving a flow switches its keys at once: a COMMU page or a stable main menu is never the song flow */
		  if (p5d_scene == P5D_SCENE_COMMU && t - p5d_scene_set_us > 600000) p5d_song_mode = 0;
		  if (p5d_scene == P5D_SCENE_MAIN && p5d_ui == P5D_UI_MAIN && p5d_ui_cur >= 0 && menu_stable) p5d_song_mode = 0;
		  /* the end of the dance flow without a result: back on a really stable main menu for a while (a quit from the
		   * pause menu, the song list's own back) - otherwise the dance map would keep the menus for its 5 minute cap */
		  if (p5d_dance_flow(t) && p5d_ui == P5D_UI_MAIN && p5d_ui_cur >= 0 && !dance_now && menu_stable && t - p5d_dance_flow_us > 3000000) p5d_dance_flow_us = 0;
		  if (p5d_dance_flow(t) && p5d_ui_cur >= 0 && !dance_now && menu_stable && t - p5d_dance_flow_us > 2500000 &&
		      (p5d_ui == P5D_UI_MAIN || p5d_ui == P5D_UI_SONG || p5d_song_mode)) p5d_dance_flow_us = 0;   /* the flow never left the menus: drop it */
		  int menu_gone = !mod && t - mod_change_us > 2500000;
		  if (p5d_pause_latch) {
			if (p5d_profile != P5D_CONFIG) p5d_switch_profile(P5D_CONFIG);   /* the pause menu is open: Q/E change values, Esc = back */
		  } else if (p5d_scr_now == P5D_SCR_CONFIG || p5d_scr_now == P5D_SCR_PAUSE) {
			if (p5d_profile != P5D_CONFIG) p5d_switch_profile(P5D_CONFIG);
		  } else if (p5d_scr_now == P5D_SCR_SONG) {
			if (p5d_profile != P5D_SONG) p5d_switch_profile(P5D_SONG);
		  } else if (p5d_scr_now == P5D_SCR_COMMU) {
			if (p5d_profile != P5D_COMMU) p5d_switch_profile(P5D_COMMU);
		  } else if (p5d_scr_now == P5D_SCR_DANCE || p5d_scr_now == P5D_SCR_STORY) {
			if (p5d_profile != P5D_GAME) { p5d_switch_profile(P5D_GAME); game_since_us = t; }
		  } else if (p5d_scr_now == P5D_SCR_MAIN || p5d_scr_now == P5D_SCR_TITLE) {
			if (p5d_profile != P5D_MENU) p5d_switch_profile(P5D_MENU);
		  } else if (commu_hold) {
			p5d_dance_latch = 0; p5d_story_latch = 0; p5d_song_latch = 0;   /* a page with L/R hints is not a stage */
			if (p5d_profile != P5D_COMMU) p5d_switch_profile(P5D_COMMU);   /* the COMMU pages: Q/E are L/R */
		  } else if (p5d_profile == P5D_COMMU) {
			p5d_switch_profile(P5D_MENU);
		  } else if (dance_now || (p5d_ui != P5D_UI_SONG && (p5d_dance_latch || story_hold))) {
			if (cfg_now && p5d_profile != P5D_GAME) {
				/* the pause menu's settings page: Q/E change a value (the D-pad left/right), Alt restores defaults */
			}
			if (cfg_now) { if (p5d_profile != P5D_CONFIG) p5d_switch_profile(P5D_CONFIG); }
			else { p5d_config_latch = 0; if (p5d_profile != P5D_GAME) { p5d_switch_profile(P5D_GAME); game_since_us = t; } }   /* the dance stage or a story scene: Esc = pause */
		  } else if (p5d_dance_flow(t) && p5d_profile != P5D_CONFIG) {
			/* the dance flow: from the song confirm to the result every screen is the dance map (its intro, notice and
			 * panel screens included). The pause menu (CONFIG) and the pause latch above still win. Not while the
			 * song list is stably on screen, though: a click there (on a row the picture model cannot read) used to
			 * start this flow and switch the map at once, which killed the list's Q/E (reported 2026-11-08). */
			if (!(song_now && p5d_ui_cur >= 0 && menu_stable && !dance_now)) {
				if (p5d_profile != P5D_GAME) { p5d_switch_profile(P5D_GAME); game_since_us = t; }
			}
		  } else if (cfg_now) {
			if (p5d_profile != P5D_CONFIG) p5d_switch_profile(P5D_CONFIG);   /* the CONFIG screen has its own map */
		  } else if (story_hold) {
			if (p5d_profile != P5D_GAME) p5d_switch_profile(P5D_GAME);   /* (the settings page keeps its map) */
		  } else if (song_now && (p5d_profile == P5D_MENU || p5d_profile == P5D_SONG)) {
			if (p5d_profile != P5D_SONG) p5d_switch_profile(P5D_SONG);   /* the song list: Q/E = D-pad (its difficulty) */
		  } else if (p5d_profile == P5D_SONG && !song_now) {
			p5d_switch_profile(P5D_MENU);
		  } else if (commu_hold && (p5d_profile == P5D_MENU || p5d_profile == P5D_COMMU)) {
			if (p5d_profile != P5D_COMMU) p5d_switch_profile(P5D_COMMU);   /* a COMMU page: Q/E are its L/R shoulders */
		  } else if (p5d_profile == P5D_COMMU) {
			p5d_switch_profile(P5D_MENU);   /* left the COMMU pages */
		  } else if (p5d_profile == P5D_CONFIG) {
			p5d_switch_profile(P5D_MENU);   /* left the settings screen */
		  } else if (p5d_profile == P5D_MENU && p5d_saw_menu && dance_seen) {
			/* the dance stage starts with the song confirm: its intro/story scenes come BEFORE the first gauge
			 * frame, and the pause menu (START = Esc in the dance map) must work during them, so the map follows
			 * the whole stage, not only the gauge (a story scene used to fall back to the menu map, where Esc is
			 * "back" and the pause menu never opened -- reported 2026-11-07) */
			p5d_switch_profile(P5D_GAME); game_since_us = t;
		  } else if (p5d_profile == P5D_GAME && !dance_now && !p5d_dance_flow(t)) {
			/* the song list and the main menu are menu screens: switch back as soon as they are stable, or
			 * the dance keys stay active there and the menu keys (arrows, Q/E) appear dead after a song.
			 * Not while song_confirm is set: the dance stage's story scenes and panel screens are NONE screens
			 * (and the tally's red bars can look like the main menu), and their Esc is the pause menu. */
			if (!song_confirm_us && menu_stable && menu_since_us && t - menu_since_us > 800000) p5d_switch_profile(P5D_MENU);
			else if (!song_confirm_us && !story_hold && game_since_us && t - game_since_us > 1200000) p5d_switch_profile(P5D_MENU);   /* a COMMU/character story ended: back to the menus */
		  } }
		if (scratch_active) {
			/* held Space: two horizontal swipes across the front touch panel per press, which is the dance
			 * stage's scratch gesture (the original game reads the touch panels; this is a host-side touch) */
			uint64_t dt = now_us() - scratch_t0;
			if (dt >= 180000) { scratch_active = 0; touch_down = 0; }
			else {
				int seg = (int)(dt / 90000), prog = (int)((dt % 90000) * 1500 / 90000);
				touch_down = 1; touch_x = 210 + ((seg & 1) ? 1500 - prog : prog); touch_y = 760;
			}
		}
		ui_test_tick();
		ui_cmd_tick();
		ui_log();
		finger_script();
		while (SDL_PollEvent(&e)) {
		if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_ESCAPE) fprintf(stderr, "[EV] esc down: pause=%d cfg=%d ui=%s scene=%d profile=%d\n", p5d_pause_latch, p5d_config_latch, p5d_ui_name(), p5d_scene, p5d_profile);
		if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_WHEEL) p5d_ui_boost();
			/* a click or a key while the window is not focused: raise it first, so SDL delivers what follows
			 * (a launcher window left in front kept the keyboard; reported by the user) */
			if ((e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) && win && !(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) SDL_RaiseWindow(win);
			/* _exit, not SDL_Quit()+exit(): the game thread is still running and may be mid-call
			 * into SDL (audio_output, window_present) or guest code; tearing down SDL subsystems
			 * out from under it raced into a SIGSEGV at a null host function pointer. The OS
			 * reclaims everything on process exit; skip the unsafe graceful-shutdown step. */
			/* (a close request asks first: the quit card, overlay.cpp; a second one while it's up quits) */
			if (e.type == SDL_EVENT_QUIT && !overlay_quit_confirm()) { extern void host_summary(const char *why); extern void gfx_gpu_quit(void); host_summary("window closed"); gfx_gpu_quit(); _exit(0); }   /* the import histogram and the stubbed-import list land in the log */
			if (e.type == SDL_EVENT_QUIT) { buttons = 0; touch_down = 0; gamepad_touch_release(); continue; }
			/* fullscreen however it was entered or left -- the window's own button (macOS's green one), the OS's
			 * shortcut, the menu -- is the setting: the next game starts the same way */
			if ((e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || e.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) && settings_int("fullscreen", 0) != (e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN)) {
				settings_set_int("fullscreen", e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN); settings_save();
			}
			/* the overlay itself: always live, open or not, so it can always be reached and always
			 * closed. Opening (or re-opening by the same gamepad combo) clears whatever buttons/touch
			 * were down so the game never sees them as still held on resume. */
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F10) { overlay_toggle(); if (overlay_is_open()) { buttons = 0; touch_down = 0; gamepad_touch_release(); } }
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_ESCAPE) { if (overlay_is_open()) overlay_close(); else overlay_bar_dismiss(); }   /* (closed: Escape hides the menu bar and its menus) */
			if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) { overlay_toggle(); if (overlay_is_open()) { buttons = 0; touch_down = 0; gamepad_touch_release(); } }
			if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) pad_back = 1;
			if (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP && e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) { pad_back = 0; pad_combo_latch = 0; }
			if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_START) pad_start = 1;
			if (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP && e.gbutton.button == SDL_GAMEPAD_BUTTON_START) { pad_start = 0; pad_combo_latch = 0; }
			if (pad_back && pad_start && !pad_combo_latch) { pad_combo_latch = 1; overlay_toggle(); if (overlay_is_open()) { buttons = 0; touch_down = 0; gamepad_touch_release(); } }
			/* exclusive while open: the menu gets every key/mouse/gamepad event instead of the game
			 * (which is paused anyway -- display.c) */
			if (overlay_is_open()) { overlay_process_event(&e); continue; }
			/* the save-data dialog waiting on the player (overlay_dialog_active): it has the keys and the mouse;
			 * the game is polling the dialog, not the pad */
			if (overlay_dialog_active()) { overlay_process_event(&e); continue; }
			/* closed: ImGui still sees the mouse (and the window's focus/enter/leave), so the menu bar can
			 * show on a hover at the top edge; a press or a move it has claimed (the pointer over the bar,
			 * a menu or the pull tab, as of the last frame drawn) never becomes a touch below. A release
			 * always falls through: letting a touch go is never wrong, and a click that began on the
			 * picture then dragged onto the bar ends as the game's (ImGui doesn't claim a drag that began
			 * outside it: its MouseDownOwned). Keys and the pad stay the game's -- the bar is pointer-only. */
			/* M1522: every menu (code state only) maps the PC mouse buttons to the game's: left = confirm, right = back. */
			if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !cur_mode && p5d_scr_now != P5D_SCR_DANCE && e.button.button == SDL_BUTTON_RIGHT) {
				p5d_collection_note_back();
				if (p5d_main_settings) { p5d_main_settings = 0; p5d_config_latch = 0; menu_tap(CROSS); p5d_mouse_cross_until = now_us() + 300000; }
				else if (p5d_config_latch || p5d_scr_now == P5D_SCR_CONFIG) p5d_settings_back_request();   /* M1631: Cross/Cross/Circle */
				else if (p5d_pause_latch) { p5d_pause_close_request(); }   /* M1630: right-click = one Cross, immediate */
				else { p5d_mouse_cross_until = now_us() + 300000; menu_win_until = now_us() + 30000000; }
				fprintf(stderr, "[MOUSE] rmb -> back (screen=%s pause=%d cfg=%d)\n", p5d_input_screen_name(p5d_scr_now), p5d_pause_latch, p5d_config_latch);
				continue;
			}
			if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) && !touch_down && !cur_mode &&
			    (p5d_scr_now == P5D_SCR_MAIN || p5d_scr_now == P5D_SCR_SONG || p5d_scr_now == P5D_SCR_COMMU ||
			     p5d_scr_now == P5D_SCR_RESULT || p5d_scr_now == P5D_SCR_PAUSE || p5d_scr_now == P5D_SCR_CONFIG ||
			     p5d_scr_now == P5D_SCR_TITLE || p5d_scr_now == P5D_SCR_STORY)) {
				int mdn = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
				if (mdn && e.button.button == SDL_BUTTON_LEFT) {
					if (p5d_scr_now == P5D_SCR_MAIN) { p5d_main_settings = (p5d_nav_row == 4); p5d_config_latch = p5d_main_settings; }
					if (p5d_collection_active() && p5d_collection_depth < 3) p5d_collection_depth++;
					menu_tap(CIRCLE); menu_win_until = now_us() + 30000000;
					fprintf(stderr, "[MOUSE] lmb -> confirm (row %d/%d)\n", p5d_nav_row, p5d_nav_rows);
					continue;
				}
				/* (the right button is handled above, outside the menu-screen condition) */
			}
			if (!p5d_pad_allowed() && (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) && p5d_mouse_on) {
				int dn = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
				if (e.button.button == SDL_BUTTON_LEFT && dn && !touch_down && !cur_mode) {
					int tx, ty;
					if (mouse_to_touch(e.button.x, e.button.y, 0, &tx, &ty)) {
						int px = tx / 2, py = ty / 2;
						p5d_menu_sel = p5d_menu_read();
						int row = p5d_menu_row_at(px, py);
						{ static int ml = -1; if (ml < 0) ml = getenv("RECOMP_MOUSE_LOG") ? atoi(getenv("RECOMP_MOUSE_LOG")) != 0 : 1;   /* on while the menu hit-testing is calibrated; RECOMP_MOUSE_LOG=0 turns it off */
						  if (ml) { int ww = 0, wh = 0; SDL_GetWindowSize(win, &ww, &wh);
						    fprintf(stderr, "mouse: window %d,%d (%dx%d) picture %d,%d row=%d selected=%d banner=%d\n", (int)e.button.x, (int)e.button.y, ww, wh, px, py, row, p5d_menu_sel, p5d_banner_y); } }
						if (p5d_ui == P5D_UI_RESULT) {
							/* the result menu's 4 rows: the wheel selects, a left click on the highlighted row
							 * confirms, a click on another row first moves the highlight there (the same closed
							 * loop the song list uses, reading the red highlight bar every presented frame) */
							if (row >= 0) {
								if (p5d_menu_sel >= 0) p5d_menu_click(row, row == p5d_menu_sel);
								else menu_click_pending(row);
							} else { menu_enter = 1; menu_next_us = now_us(); }
							menu_win_until = now_us() + 30000000;
							fprintf(stderr, "mouse: click -> result row %d (sel %d)\n", row, p5d_menu_sel);
							continue;
						}
						if (p5d_ui == P5D_UI_MAIN || p5d_ui == P5D_UI_SONG) {
							/* the menu is wheel-driven: the wheel moves the game's own highlight one item a
							 * notch. A click on another row moves the highlight there; a click on the highlighted
							 * row enters it. Entering a song row is the song confirm and only that starts the dance
							 * flow -- a plain highlight click must not (starting it switched the input profile to
							 * the dance map at once and killed the song list's Q/E, reported 2026-11-08) */
							if (row >= 0 && p5d_menu_sel >= 0 && row != p5d_menu_sel) {
								menu_enter = 0; menu_target = row; menu_next_us = now_us();
								menu_win_until = now_us() + 30000000;
								fprintf(stderr, "mouse: click -> row %d (highlight %d, first click)\n", row, p5d_menu_sel);
								continue;
							}
							if (p5d_ui == P5D_UI_SONG || p5d_song_mode) { game_started_us = now_us(); song_confirm_us = now_us(); }
							/* (the dance map follows the song gauge / the stage's own picture -- see the profile switch;
							 * a click here must not start the dance flow by itself: on the song list at 2K the picture
							 * model often still reads MAIN, so a row click looked like a song confirm and switched the
							 * key map, killing the list's Q/E -- reported 2026-11-08) */
							menu_enter = 1; menu_next_us = now_us();
							menu_win_until = now_us() + 30000000;
							fprintf(stderr, "mouse: click -> confirm (screen %s)\n", p5d_ui_name());
							continue;
						} else if (p5d_saw_menu && now_us() < menu_win_until) {
							/* a pause/submenu screen (the model says NONE there): wheel selects, click confirms */
							menu_enter = 1; menu_next_us = now_us();
							menu_win_until = now_us() + 30000000;
							fprintf(stderr, "mouse: click -> confirm (pause/submenu)\n");
							continue;
						} else if (row >= 0) {
							if (p5d_menu_sel >= 0) { p5d_menu_click(row, row == p5d_menu_sel); }   /* first click highlights, a click on the highlight enters */
							else { menu_click_pending(row); }                                        /* wait for a readable frame, then the same */
							continue;
						}
						/* not a list row (title, logo, opening animation, a story/dance cutscene): the click is
						 * the game's any-button/skip tap -- and never a touch hold, which would mask it */
						p5d_click_advance();
						fprintf(stderr, "mouse: window %d,%d -> advance/skip tap (screen %d)\n", (int)e.button.x, (int)e.button.y, p5d_ui);
						continue;
					}
				}
				if (e.button.button == SDL_BUTTON_LEFT) {
					/* before the main menu (logos/opening animation/title) a click is the game's any-button: P5D's
					 * title takes Cross/Start, not Circle; from the menu on, Circle is confirm */
					uint32_t b = (p5d_ui == P5D_UI_MAIN || p5d_ui == P5D_UI_SONG || p5d_ui == P5D_UI_RESULT || p5d_saw_menu) ? p5d_mouse_left_btn : (CROSS | START);
					if (dn) { p5d_mouse_left_bit = b; p5d_mouse_bits |= b; }
					else { p5d_mouse_bits &= ~(p5d_mouse_left_bit ? p5d_mouse_left_bit : b); p5d_mouse_left_bit = 0; }
				}
				else if (e.button.button == SDL_BUTTON_RIGHT) {
					if (dn && p5d_config_latch) {   /* the settings page exits with L + Circle */
						if (p5d_pause_latch) { p5d_config_from_pause = 1; p5d_pause_pic_us = now_us(); }
						p5d_settings_back_queue();
					}
					else if (dn && p5d_pause_latch) {   /* the pause menu: step to the 返回 row, then Circle */
	
					}
					else if (dn && p5d_pause_latch) { menu_tap(START); menu_win_until = now_us() + 30000000; }   /* the pause menu's back: START closes it */
					else if (dn) {
						p5d_mouse_cross_until = now_us() + 300000; if (p5d_saw_menu) menu_win_until = now_us() + 30000000;   /* back: re-arm the wheel window too */
						fprintf(stderr, "[MOUSE] rmb -> back (ui=%s profile=%d scene=%d pause=%d cfg=%d)\n", p5d_ui_name(), p5d_profile, p5d_scene, p5d_pause_latch, p5d_config_latch);
						if (p5d_ui == P5D_UI_SONG || p5d_song_mode) { p5d_song_mode = 0; p5d_song_latch = 0; p5d_song_flow_armed = 0; p5d_scene_set(P5D_SCENE_MAIN, "song list back"); }   /* left the song flow: no stale D-pad mode */
					}
					/* (the latched back above times itself out) */
				}
			}
			if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_WHEEL ||
			    e.type == SDL_EVENT_WINDOW_MOUSE_ENTER || e.type == SDL_EVENT_WINDOW_MOUSE_LEAVE || e.type == SDL_EVENT_WINDOW_FOCUS_GAINED || e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
				overlay_process_event(&e);
				if (overlay_mouse_captured() && e.type != SDL_EVENT_MOUSE_BUTTON_UP) continue;
			}
			/* a touchscreen's fingers (screen_finger). SDL also makes the first finger the mouse (which == SDL_TOUCH_MOUSEID):
			 * that stops here, after the menu bar has seen it, so a tap never fires, looks, captures the mouse or switches
			 * the input to keyboard + mouse */
			if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_MOTION || e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED) { screen_finger(&e); continue; }
			if ((e.type == SDL_EVENT_MOUSE_MOTION && e.motion.which == SDL_TOUCH_MOUSEID) || ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) && e.button.which == SDL_TOUCH_MOUSEID)) continue;
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F12) { extern void gfxcap_request(void); gfxcap_request(); fprintf(stderr, "capture requested (F12)\n"); }
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F11) { p5d_load(); continue; }   /* input.ini: reload the PC key map */
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F3) { p5d_qe_mode_cycle(); continue; }   /* Q/E mapping: auto / L-R / D-pad (works in the minimal build too) */
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F9) { p5d_switch_profile(p5d_profile == P5D_GAME ? P5D_MENU : P5D_GAME); continue; }
			/* M1635: F5 = the "auto play" cheat (the Vita cheat file's 自动演奏; works in the minimal build too) */
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F5) {
				extern int p5d_autoplay_enabled; p5d_autoplay_enabled = !p5d_autoplay_enabled;
				fprintf(stderr, "[CHEAT] auto play %s\n", p5d_autoplay_enabled ? "on" : "off");
				overlay_toast_titled("Auto play", p5d_autoplay_enabled ? "ON  (F5 toggles)" : "OFF  (F5 toggles)", 1.0);
				p5d_banner_show(p5d_autoplay_enabled, 1.0);   /* the P5-style notification box (port overlay), 1 s */
			}
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F12) {   /* the old F5 marker: a log/frame-history mark for bug reports */
				static int n; n++; extern uint64_t display_frames(void);
				fprintf(stderr, "user mark %d at frame %llu\n", n, (unsigned long long)display_frames());
				{ extern void gfx_gpu_history_save(int mark); gfx_gpu_history_save(n); }
				char b[48]; snprintf(b, sizeof b, "Mark %d at frame %llu", n, (unsigned long long)display_frames()); overlay_toast_titled("Marked in the log", b, 2.0);
			}   /* menu vs dance key map */
#ifndef RECOMP_MINIMAL
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F1) metrics_osd_cycle();
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F6) {   /* a VERIFY build's 60/30 A/B capture (verify.c sets window_f6) */
				if (window_f6) window_f6(); else fprintf(stderr, "F6: the A/B capture needs the verify build (vitarecomp build <title> verify)\n");
			}
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F9) {   /* a VERIFY build's hand-held count window (verify.c sets window_f9) */
				if (window_f9) window_f9(); else fprintf(stderr, "F9: count windows need the verify build (vitarecomp build <title> verify)\n");
			}
if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F8) raw_look_toggle();   /* the experimental raw mouse look (hooks.c) */
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F4) { extern void gfx_gpu_fsr_toggle(void); gfx_gpu_fsr_toggle(); }   /* FSR 3.1 anti-aliasing off and on, live (RECOMP_FSR: gfx_gpu.c) */
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F7) { extern void camera_light_toggle(void); camera_light_toggle(); }   /* the rear camera's lamp (camera.c: Uncharted's hold-it-to-the-light puzzles) */
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F2) { extern void gfx_gpu_render_scale_cycle(void); gfx_gpu_render_scale_cycle(); }   /* the GPU backend's internal resolution
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F2) { extern void gfx_gpu_render_scale_cycle(void); gfx_gpu_render_scale_cycle(); }   /* the GPU backend's internal resolution, 1x -> 2x -> ... -> 6x, from the next frame */
			#endif   /* RECOMP_MINIMAL: no debug keys */
			/* the dance stage's pause menu: Esc (START) toggles it; while it is up the pause page's own map takes over
			 * (the CONFIG map) -- and Esc there is the cross that closes the page */
			/* the dance stage's pause menu: handled here by hand (a START/cross tap), not through the key map, so it
			 * cannot race the profile switch -- Esc used to be lost when it arrived before the map changed
			 * (intermittent, reported 2026-11-07) */
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && p5d_config_latch && e.key.key == SDLK_BACKSPACE) {
				p5d_settings_back_queue();   /* the settings page's exit chord (main menu and pause alike) */
				continue;
			}
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && p5d_pause_latch && e.key.key == SDLK_BACKSPACE) {
				/* only the game's own back is sent, and the pause settings page's exit is its OK prompt: L focuses the
				 * "--- OK 決定 ---" prompt, Circle confirms it (verified: lands back on the pause menu). The state closes
				 * when the picture changes (the auto-close above). */
				if (p5d_config_latch || p5d_scr_now == P5D_SCR_CONFIG) {   /* the settings page's exit: Cross/Cross/Circle */
					p5d_settings_back_request();
				} else {                  /* the pause menu's exit: one Cross closes the page at once */
					p5d_pause_close_request();
				}
				continue;
			}
			/* ESC decided by the CODE-LEVEL screen (M1523): each screen gets its own meaning from the job list */
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_ESCAPE && p5d_saw_menu) {
				int scr = p5d_scr_now;
				if (scr == P5D_SCR_CONFIG) {
					p5d_settings_back_request();
					fprintf(stderr, "[EV] esc -> settings back (Cross/Cross/Circle)\n");
					continue;
				}
				if (scr == P5D_SCR_PAUSE) { p5d_pause_close_request(); fprintf(stderr, "[EV] esc -> pause close (one Cross)\n"); continue; }
				if (scr == P5D_SCR_DANCE || scr == P5D_SCR_STORY) {
					fprintf(stderr, "[PAUSE] esc -> open the pause (screen=%s)\n", p5d_input_screen_name(scr));
					p5d_pause_latch = 1; p5d_pause_us = now_us(); p5d_pause_pic_us = now_us(); p5d_pause_close_us = 0; p5d_pause_close_tried = 0; p5d_scene_push(P5D_SCENE_PAUSE, "ESC pause"); p5d_scene_lock = 0; menu_tap(START);
					menu_win_until = now_us() + 30000000;
					continue;
				}
				if (scr == P5D_SCR_MAIN || scr == P5D_SCR_SONG || scr == P5D_SCR_COMMU || scr == P5D_SCR_RESULT || scr == P5D_SCR_TITLE) {
					fprintf(stderr, "[EV] esc -> back (screen=%s)\n", p5d_input_screen_name(scr));
					p5d_collection_note_back();
					menu_tap(CROSS); menu_win_until = now_us() + 30000000;
					continue;
				}
			}
			/* M1523: the picture-driven ESC fallback is removed -- the code-level screen above covers every screen.
			 * An unknown screen gets the back key, which cannot open anything. */
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_ESCAPE) {
				menu_tap(CROSS); menu_win_until = now_us() + 30000000;
				fprintf(stderr, "[EV] esc -> back (unknown screen fallback)\n");
				continue;
			}
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_RETURN && !p5d_pause_latch && (p5d_dance_latch || p5d_story_latch || p5d_commu_mode)) {
				p5d_pause_latch = 1; p5d_pause_us = now_us(); p5d_pause_pic_us = now_us(); p5d_pause_close_us = 0; p5d_pause_close_tried = 0; menu_tap(START); menu_win_until = now_us() + 30000000;   /* Return is START too in the dance map */
				continue;
			}
			/* Q/E for the two navigation contexts, decided only by the main-menu row that opened them (no pixel model):
			 * the COMMU pages get L/R (character switching), the DANCING/song-list flow gets the D-pad */
			/* Q / E are decided here, not through the key map: the two navigation contexts differ by screen only.
			 *   pause/settings page  -> the D-pad left/right (its own controls: values, difficulty, ...)
			 *   the dance stage     -> the key map (Q = up etc.)
			 *   everything else     -> L/R (the COMMU pages switch characters with the shoulders; the hint row
			 *                          there names Q/E, and the main-menu map is L/R too) */
			/* Q / E priority (no screen pixels are involved):
			 *   1. the pause menu's settings page -> D-pad left/right (values)
			 *   2. the COMMU flow                 -> L/R shoulders (character switching)
			 *   3. the song list                  -> D-pad left/right (difficulty)
			 *   4. the settings page by its strict template (brief fallback for the picture model)
			 *   5. the dance stage                -> the key map (Q = up etc.)
			 *   6. everywhere else                -> L/R shoulders (the menu map is L/R)
			 * This order settles the conflicts: the settings page wins over COMMU (a pause inside a COMMU story has
			 * its own page), the COMMU flow wins over the song-list template (the two lists look alike). */
			/* Q / E priority (no screen pixels are involved; the two lists look alike, so the flow state decides):
			 *   1. the pause menu's settings page -> D-pad left/right (values)
			 *   2. the COMMU flow                 -> L/R shoulders (character switching)
			 *   3. the song list                  -> D-pad left/right (difficulty)
			 *   4. the dance stage (unmodelled)   -> the key map (Q = up etc.)
			 *   5. the settings page's strict template within 900 ms -> D-pad left/right
			 *   6. everything else                -> L/R shoulders (the menu map is L/R)
			 * COMMU outranks the song-list template on purpose: the picture model confuses the two lists. */
			/* Q / E real-time scene priority (the scene flags are latched by their own enter/exit edges):
			 *   1. settings/pause page (pause state or the settings template) -> D-pad left/right
			 *   2. the COMMU pages     (the game's own L/R hint keycaps were just on screen, or the COMMU row opened
			 *      the flow)                                                  -> L/R shoulders
			 *   3. the song list      (its modelled screen / flow flag)      -> D-pad left/right
			 *   4. the dance stage    (unmodelled, the dance profile)        -> the key map (Q = up etc.)
			 *   5. settings template  (brief fallback for the picture model) -> D-pad left/right
			 *   6. everything else                                           -> L/R shoulders
			 * COMMU outranks the song-list template on purpose: the two lists look alike to the picture model. */
			/* Q / E: the scene state alone decides (there is no per-screen key state, and no picture template
			 * takes part in this decision). Leaving a screen changes the scene, so the next press follows it. */
			if ((e.key.key == SDLK_Q || e.key.key == SDLK_E) && (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP)) {
				int dn = e.type == SDL_EVENT_KEY_DOWN;
				int q = e.key.key == SDLK_Q;
				uint64_t qt = now_us();
				/* shoulders are the DEFAULT (the COMMU pages, the main menu, the title all read Q/E as L/R), and the
				 * D-pad is used only while a D-pad screen is positively there: the pause/settings page (latched),
				 * or the song list within 1.5 s of its own evidence (p5d_song_ev_us: modelled frames, the DANCING-row
				 * confirm, the MusicSelect job name). A stale scene flag can therefore never turn a COMMU Q/E into a
				 * D-pad press (reported: the launcher build's COMMU Q/E still failed while the bat's worked). */
				int dpad = 0;
				const char *why = "shoulders";
				p5d_qe_mode_load();
				/* the D-pad is selected by the input-intent FLOWS only - never by the inferred scene (the picture
				 * model's false positives used to leave a scene lock on SONG, which turned the COMMU screens' Q/E
				 * into the D-pad; reported 2026-11-07). p5d_song_mode is set by the DANCING-row confirm and
				 * cleared when the song flow is left (Backspace, the song confirm, the title); pause/settings are
				 * the pause latch and the settings-page latch. Everything else - the COMMU character list and its
				 * COMMUNITY story list included - is L/R shoulders, which is what those screens use. */
				/* M1523: the CODE-LEVEL screen decides; the picture scene only where the code state is unknown */
				if (p5d_qe_mode == 2) { dpad = 1; why = "forced"; }
				else if (p5d_qe_mode == 1) { dpad = 0; why = "forced"; }
				else if (p5d_scr_now == P5D_SCR_CONFIG || p5d_scr_now == P5D_SCR_PAUSE) { dpad = 1; why = "pause/settings"; }
				else if (p5d_scr_now == P5D_SCR_SONG) { dpad = 1; why = "song list (code screen)"; }
				else if (p5d_song_job_us && qt - p5d_song_job_us < 1500000) { dpad = 1; why = "song list (song job)"; }
				else if (p5d_scr_now == P5D_SCR_COMMU || p5d_scr_now == P5D_SCR_STORY) { dpad = 0; why = "commu/story"; }
				else if (p5d_scr_now == P5D_SCR_MAIN || p5d_scr_now == P5D_SCR_TITLE || p5d_scr_now == P5D_SCR_RESULT) { dpad = 0; why = "menu"; }
				else { dpad = 0; why = "shoulders"; }   /* code state unknown: the harmless shoulders */
				if ((p5d_scr_now == P5D_SCR_DANCE || p5d_profile == P5D_GAME) && !p5d_pause_latch && !p5d_config_latch) {
					/* the dance stage: fall through to the key map (Q = up, and the other dance keys) */
				} else if (dpad) {
					snprintf(p5d_qe_last, sizeof p5d_qe_last, "%s", why);
					if (dn && !e.key.repeat) menu_tap(q ? LEFT : RIGHT);
					fprintf(stderr, "[KEY] Q/E -> %s %s (dpad: %s) sm=%d cfg=%d prof=%d ui=%s scr=%s\n", q ? "LEFT" : "RIGHT", dn ? "down" : "up", why, p5d_song_mode, p5d_config_latch, p5d_profile, p5d_ui_name(), p5d_input_screen_name(p5d_scr_now));
					continue;
				} else {
					snprintf(p5d_qe_last, sizeof p5d_qe_last, "shoulder");
					if (dn && !e.key.repeat) menu_tap(q ? LTRIG : RTRIG);
					fprintf(stderr, "[KEY] Q/E -> %s %s (scene=%s, shoulders) sm=%d cfg=%d prof=%d ui=%s\n", q ? "L" : "R", dn ? "down" : "up", p5d_scene_name(p5d_scene), p5d_song_mode, p5d_config_latch, p5d_profile, p5d_ui_name());
					continue;
				}
			}
			/* the dance stage: Space is the scratch gesture (a front-touch swipe), not a button */
			if (p5d_profile == P5D_GAME && e.key.key == SDLK_SPACE) {
				if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) { scratch_active = 1; scratch_t0 = now_us(); fprintf(stderr, "[KEY] scratch (space) in game profile\n"); continue; }
				if (e.type == SDL_EVENT_KEY_UP) { scratch_active = 0; touch_down = 0; continue; }
			}
			if (((e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) || e.type == SDL_EVENT_KEY_UP) && p5d_key_event(e.key.key, e.type == SDL_EVENT_KEY_DOWN)) continue;   /* the PC key map (input.ini) */
			if ((e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) || e.type == SDL_EVENT_KEY_UP) {   /* the bound keys (kb_binds) */
				int i = kb_find(e.key.key), dn = e.type == SDL_EVENT_KEY_DOWN;
				if (i >= 0) {
					kb_binds[i].held = dn;
					if (kb_binds[i].bit && !display_pstv_active()) { if (dn) buttons |= kb_binds[i].bit; else buttons &= ~kb_binds[i].bit; }   /* (the PS TV layout: a controller's only, kbm_used) */
					if (dn) { kbm_used(); kbm_auto = 1; snprintf(kbm_why, sizeof kbm_why, "the key %s", SDL_GetKeyName(e.key.key)); }
				}
			}
			/* PC-only: the pad scheme is banned at launch (code-level, not a setting the game can override) */
			if (!p5d_pad_allowed() && (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
			    e.type == SDL_EVENT_GAMEPAD_BUTTON_UP || e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION)) continue;
			if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad && (pad = SDL_OpenGamepad(e.gdevice.which)) != 0) {
				__atomic_store_n(&pad_connected, 1, __ATOMIC_RELAXED);
				/* a DS4/DualSense's gyroscope and accelerometer stream only once asked for (SDL's HIDAPI driver) */
				int g = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
				int a = SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) && SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
				pad_gyro = g && a;
				fprintf(stderr, "gamepad: %s%s%s\n", SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "?", SDL_GetNumGamepadTouchpads(pad) > 0 ? ", touchpad" : "", pad_gyro ? ", gyro" : "");
			}
			if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && SDL_GetGamepadID(pad) == e.gdevice.which) { SDL_CloseGamepad(pad); pad = 0; __atomic_store_n(&pad_connected, 0, __ATOMIC_RELAXED); pad_gyro = 0; motion_lost(); gamepad_touch_release(); buttons = 0; for (int i = 0; i < 4; i++) sticks[i] = 128; }
			if (e.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE) {
				if (e.gsensor.sensor == SDL_SENSOR_ACCEL) motion_feed_accel(e.gsensor.data, e.gsensor.sensor_timestamp);
				else if (e.gsensor.sensor == SDL_SENSOR_GYRO) motion_feed_gyro(e.gsensor.data, e.gsensor.sensor_timestamp);
			}
			if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) device_gamepad();
			if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) { static int plog = -1; if (plog < 0) plog = getenv("RECOMP_PAD_LOG") ? 1 : 0; if (plog) fprintf(stderr, "    pad: button %d %s (id %u)\n", e.gbutton.button, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? "down" : "up", (unsigned)e.gbutton.which); }
			if (e.type == SDL_EVENT_GAMEPAD_ADDED) { fprintf(stderr, "gamepad added: %s\n", SDL_GetGamepadNameForID(e.gdevice.which) ? SDL_GetGamepadNameForID(e.gdevice.which) : "?"); }
			if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (e.gaxis.value > 16000 || e.gaxis.value < -16000))) {
				kbm_auto = 0; snprintf(kbm_why, sizeof kbm_why, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? "pad button %d" : "pad axis %d", e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? e.gbutton.button : e.gaxis.axis);
			}   /* the pad in hand: auto's choice */
			if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) motion_recenter();   /* R3 (the Vita has no stick clicks): the gyro's heading becomes straight ahead */
			if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
				int dn = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN, b = e.gbutton.button;
				if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER || b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
					const int k = b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
					if (shoulders_swapped) pad_main(pad, k, dn); else pad_second(pad, k, dn);
				} else if (b == SDL_GAMEPAD_BUTTON_SOUTH && (cur_mode || cur_down || cur_cross)) {
					cur_cross = dn && cur_mode;            /* Cross is the touch while a bumper is held */
					cur_down = cur_mode && (cur_cross || cur_trig[0] || cur_trig[1]);
					if (!dn) buttons &= ~CROSS;            /* (a Cross pressed before the bumper, let go after) */
				} else if (dn) buttons |= pad_bit(b);
				else buttons &= ~pad_bit(b);
			}
			if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
				/* a trigger is pressed past ~37%, released under ~24% (no chatter at the edge) */
				const int k = e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, was = ptrig[k];
				if (!was && e.gaxis.value > 12000) ptrig[k] = 1;
				else if (was && e.gaxis.value < 8000) ptrig[k] = 0;
				if (ptrig[k] != was) { if (shoulders_swapped) pad_second(pad, k, ptrig[k]); else pad_main(pad, k, ptrig[k]); }
			}
			if (e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN || e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION || e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP) {
				int f = e.gtouchpad.finger;
				if (e.gtouchpad.touchpad == 0 && f >= 0 && f < TP_FINGERS) {
					if (e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP) tp_down[f] = 0;
					else {
						if (e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN || !tp_down[f]) tp_port[f] = rb_held;   /* the panel is chosen as the finger lands */
						float u = e.gtouchpad.x < 0 ? 0 : e.gtouchpad.x > 1 ? 1 : e.gtouchpad.x, v = e.gtouchpad.y < 0 ? 0 : e.gtouchpad.y > 1 ? 1 : e.gtouchpad.y;
						tp_x[f] = clampi((int)(u * 1920.f), 0, 1919);
						tp_y[f] = tp_port[f] ? clampi(REAR_Y0 + (int)(v * (REAR_Y1 - REAR_Y0 + 1)), REAR_Y0, REAR_Y1) : clampi((int)(v * 1088.f), 0, 1087);
						tp_down[f] = 1;
					}
				}
			}
			/* the mouse moved for real over the picture while the pad had the input (auto mode): back to keyboard +
			 * mouse, so the capture returns at once -- it used to wait for a key, the pointer loose and stopping at
			 * the window's edges after any touch of the pad (Alt still frees it; a nudge under a few points doesn't count) */
			if (!rel_on && e.type == SDL_EVENT_MOUSE_MOTION && !overlay_mouse_captured() && settings_int("mouse_look", 1) &&
			    fabsf(e.motion.xrel) + fabsf(e.motion.yrel) > 3.0f && !(SDL_GetModState() & SDL_KMOD_ALT)) { kbm_auto = 1; snprintf(kbm_why, sizeof kbm_why, "the mouse, not captured"); }
			if (rel_on && (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP)) {   /* captured: the look, and R / L */
				if (e.type == SDL_EVENT_MOUSE_MOTION && now_us() - rel_since_us < 60000) continue;
				if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && gesture_wanted()) {   /* a swipe: see window_touch_gesture_hint */
					gesture_drag = 1; touch_x = 960; touch_y = 544; touch_down = 1; gesture_t0 = now_us();
					fprintf(stderr, "    touch swipe: down (mouse, a gesture window)\n"); continue;
				}
				if (gesture_drag && e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
					fprintf(stderr, "    touch swipe: up after %.0f ms, moved %d,%d\n", (now_us() - gesture_t0) / 1e3, touch_x - 960, touch_y - 544);
					gesture_drag = 0; touch_down = 0; continue;
				}
				if (gesture_drag && e.type == SDL_EVENT_MOUSE_MOTION) {   /* the touch follows the motion, 2.5 panel pixels a mouse unit (a short flick crosses a good part of the panel); the camera stays */
					touch_x = clampi(touch_x + (int)lroundf(e.motion.xrel * 2.5f), 0, 1919); touch_y = clampi(touch_y + (int)lroundf(e.motion.yrel * 2.5f), 0, 1087);
					continue;
				}
				if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && twist_wanted() && !twist_pad) {   /* a twist: see window_touch_twist_hint */
					twist_drag = 1; twist_ang = 0; twist_place(0); twist_down = 1;
					fprintf(stderr, "    touch twist: grip (mouse)\n"); continue;
				}
				if (twist_drag && e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
					fprintf(stderr, "    touch twist: let go (mouse), turned %.0f deg\n", twist_ang * 57.29578f);
					twist_drag = 0; twist_down = 0; continue;
				}
				if (twist_drag && e.type == SDL_EVENT_MOUSE_MOTION) {   /* down turns clockwise: 0.4 degrees a mouse unit x "touch_twist_speed" */
					twist_ang += e.motion.yrel * 0.4f * (float)(M_PI / 180) * (float)settings_float("touch_twist_speed", 1.0);
					twist_place(twist_ang); continue;
				}
				if (e.type == SDL_EVENT_MOUSE_MOTION) {
					int qx = (int)lroundf(e.motion.xrel * 256.0f), qy = (int)lroundf(e.motion.yrel * 256.0f);
					__atomic_fetch_add(&mouse_dx_q8, qx, __ATOMIC_RELAXED); __atomic_fetch_add(&mouse_dy_q8, qy, __ATOMIC_RELAXED);
					look_dx_q8 += qx; look_dy_q8 += qy;
				} else {
					int dn = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
					if (e.button.button == SDL_BUTTON_LEFT) { kbm_lmb = dn; if (dn) buttons |= RTRIG; else buttons &= ~RTRIG; }
					if (e.button.button == SDL_BUTTON_RIGHT) {
					/* in the menus the right button is the back key (the menu branch above already sent Cross); only on the
					 * dance stage is it the L trigger/look. It used to fire LTRIG in the menus too, so a right click on the
					 * COMMU list also switched a character (reported 2026-11-08) */
					kbm_rmb = dn;
					if (p5d_profile == P5D_GAME && !p5d_pause_latch && !p5d_config_latch) { if (dn) buttons |= LTRIG; else buttons &= ~LTRIG; }
					else if (!dn) buttons &= ~LTRIG;
				}
				}
				continue;
			}
			int tx, ty;
			if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && mouse_to_touch(e.button.x, e.button.y, 0, &tx, &ty)) { touch_x = tx; touch_y = ty; touch_down = 1; }   /* (a click on the bars touches nothing) */
			if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) touch_down = 0;
			if (!p5d_pad_allowed() && e.type == SDL_EVENT_MOUSE_MOTION && p5d_mouse_on && !touch_down && !cur_mode && p5d_saw_menu) {
				int tx, ty;
				if (mouse_to_touch(e.motion.x, e.motion.y, 0, &tx, &ty)) {
					int px = tx / 2, py = ty / 2;
					if (p5d_ui == P5D_UI_MAIN || p5d_ui == P5D_UI_RESULT) {
						/* the main menu is wheel-driven (plain motion does not steer the highlight); the
						 * result menu is wheel/click only -- a passing pointer must not move its selection */
					} else {
						int sel = p5d_menu_read();
						int row = p5d_menu_row_at(px, py);
						if (sel >= 0 && row >= 0 && row != sel && !menu_enter) { p5d_menu_sel = sel; menu_enter = 0; menu_target = row; menu_next_us = now_us(); }
					}
				}
			}
			if (e.type == SDL_EVENT_MOUSE_WHEEL && p5d_collection_active() && p5d_collection_watching) {
				static float watch_wheel_acc;
				watch_wheel_acc += e.wheel.y;
				int wacted = 0;
				while (watch_wheel_acc >= 1.f) { menu_tap(LTRIG); watch_wheel_acc -= 1.f; wacted = 1; }
				while (watch_wheel_acc <= -1.f) { menu_tap(RTRIG); watch_wheel_acc += 1.f; wacted = -1; }
				if (wacted) fprintf(stderr, "[WATCH] wheel %s -> zoom\n", wacted > 0 ? "UP (zoom in)" : "DOWN (zoom out)");
				continue;
			}
			if (e.type == SDL_EVENT_MOUSE_WHEEL && !cur_mode &&
			    (p5d_scr_now == P5D_SCR_MAIN || p5d_scr_now == P5D_SCR_SONG || p5d_scr_now == P5D_SCR_COMMU ||
			     p5d_scr_now == P5D_SCR_RESULT || p5d_scr_now == P5D_SCR_PAUSE || p5d_scr_now == P5D_SCR_CONFIG ||
			     p5d_scr_now == P5D_SCR_TITLE || p5d_scr_now == P5D_SCR_STORY)) {
				/* one wheel notch = one of the game's own list steps; the click confirms. (Steering the
				 * highlight continuously was unreliable on the moving 3D list, so the interaction uses
				 * the game's native steps, driven by the PC wheel.) */
				static float wheel_acc;
				if (p5d_ui == P5D_UI_RESULT) {
					/* the result menu's 4 rows: up/down notch = one step of its selection */
					if (e.wheel.y >= 1.f || e.wheel.y <= -1.f) {
						int dir = e.wheel.y > 0 ? 1 : -1;
						menu_tap(dir > 0 ? UP : DOWN);
						menu_win_until = now_us() + 30000000;
						fprintf(stderr, "[UI] wheel %s (result row %d)\n", dir > 0 ? "UP" : "DOWN", p5d_ui_cur);
					}
				} else {
				wheel_acc += e.wheel.y;
				int acted = 0, on_tabs = 0;
				{	/* the difficulty tabs are the top strip (the game's hint: d-pad changes the difficulty) */
					float mx = 0, my = 0; SDL_GetMouseState(&mx, &my);
					int tx, ty; if (mouse_to_touch(mx, my, 0, &tx, &ty) && (p5d_song_mode || p5d_scene == P5D_SCENE_SONG) && ty / 2 < 70) on_tabs = 1;
				}
				while (wheel_acc >= 1.f) { menu_tap(on_tabs ? LEFT : UP); if (!on_tabs) p5d_nav_step(-1); wheel_acc -= 1.f; acted = 1; }
				while (wheel_acc <= -1.f) { menu_tap(on_tabs ? RIGHT : DOWN); if (!on_tabs) p5d_nav_step(+1); wheel_acc += 1.f; acted = -1; }
				if (acted) { menu_win_until = now_us() + 30000000; fprintf(stderr, "[UI] wheel %s (%s, screen %s, row %d/%d)\n", acted > 0 ? "UP" : "DOWN", on_tabs ? "difficulty" : "list", p5d_input_screen_name(p5d_scr_now), p5d_nav_row, p5d_nav_rows); }
				}
			}
			if (e.type == SDL_EVENT_MOUSE_MOTION && touch_down && mouse_to_touch(e.motion.x, e.motion.y, 1, &tx, &ty)) { touch_x = tx; touch_y = ty; }
			/* the right button: the rear panel, at the same place on the picture (the rear panel's
			 * coordinates face the same way as the front's; it's shorter, so y clamps to its rows) */
			if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_RIGHT && mouse_to_touch(e.button.x, e.button.y, 0, &tx, &ty)) { mouse_rear_x = tx; mouse_rear_y = clampi(ty, REAR_Y0, REAR_Y1); mouse_rear_down = 1; }
			if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_RIGHT) mouse_rear_down = 0;
			if (e.type == SDL_EVENT_MOUSE_MOTION && mouse_rear_down && mouse_to_touch(e.motion.x, e.motion.y, 1, &tx, &ty)) { mouse_rear_x = tx; mouse_rear_y = clampi(ty, REAR_Y0, REAR_Y1); }
			if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e.gaxis.axis < 4)
				sticks[e.gaxis.axis] = (uint8_t)((e.gaxis.value + 32768) >> 8);
		}
		{	/* keyboard + mouse, once a loop: which scheme, the capture, WASD as the left stick, the look's stand-in */
			static uint64_t last; uint64_t t = now_us(); double dt = last ? (t - last) / 1e6 : 0; last = t;
			if (dt > 0.1) dt = 0.1;
			static int loaded; if (!loaded) { loaded = 1; kb_reload(); }
			char mb[8]; const char *mode = settings_str("input_mode", "auto", mb, sizeof mb);
			int on = !strcmp(mode, "kbm") ? 1 : !strcmp(mode, "pad") ? 0 : kbm_auto;
			const int pstv = display_pstv_active();   /* the PS TV layout is a controller's only: no keyboard-and-mouse scheme, no capture */
			if (on && pstv) { kbm_used(); on = 0; }
			static int said;
			if (on != kbm_on) { extern uint64_t display_frames(void); fprintf(stderr, "    input: %s (%s%s, frame %llu)\n", on ? "keyboard and mouse" : "the controller", strcmp(mode, "auto") ? "Play with: " : "", strcmp(mode, "auto") ? mode : kbm_why[0] ? kbm_why : "start", (unsigned long long)display_frames()); }
			if (on && !kbm_on && !said) { said = 1; overlay_toast_titled("Keyboard & mouse", "The mouse looks and aims -- hold [Alt] for the cursor\nControls: [F10]", 6.0); }
			kbm_on = on;
			int want = 0;   /* the player's build: the pointer stays visible and free (clicks are the game's) */
#ifndef RECOMP_MINIMAL
			want = (on || (window_raw_look() && !pstv)) && settings_int("mouse_look", 1) && !overlay_is_open() && !overlay_dialog_active() && win && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) && !(SDL_GetModState() & SDL_KMOD_ALT);
#endif
			/* SDL's own state, not just ours: the system can drop the capture behind our back (a focus change as
			 * the window opens, say) and rel_on then claimed a capture that wasn't there -- the pointer showed,
			 * left the window and stopped tracking at its edge. Re-applied whenever the two disagree. */
			if (win && want && rel_on && !SDL_GetWindowRelativeMouseMode(win)) {
				static int said; if (!said++) fprintf(stderr, "mouse: the capture was lost -- re-applied\n");
				SDL_SetWindowRelativeMouseMode(win, true); rel_since_us = now_us();
			}
			/* RECOMP_MOUSE_DIAG=1: once a second, the capture as we, SDL and the system see it -- for a capture
			 * that holds in one game and not another (the pointer stopping at the window's edge) */
			{
				static int diag = -1; if (diag < 0) diag = getenv("RECOMP_MOUSE_DIAG") != 0;
				static uint64_t last;
				if (diag && win && now_us() - last > 1000000) {
					last = now_us();
					float gx, gy, wx, wy; SDL_GetGlobalMouseState(&gx, &gy); SDL_GetMouseState(&wx, &wy);
					int px, py, ww, wh; SDL_GetWindowPosition(win, &px, &py); SDL_GetWindowSize(win, &ww, &wh);
					fprintf(stderr, "mouse: want %d rel_on %d sdl_rel %d kbm %d raw %d focus %d fullscreen %d | global %.0f,%.0f in-window %.0f,%.0f | window %d,%d %dx%d | pending %d,%d\n",
						want, rel_on, SDL_GetWindowRelativeMouseMode(win), on, window_raw_look(), !!(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS),
						!!(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN), gx, gy, wx, wy, px, py, ww, wh,
						__atomic_load_n(&mouse_dx_q8, __ATOMIC_RELAXED) / 256, __atomic_load_n(&mouse_dy_q8, __ATOMIC_RELAXED) / 256);
				}
			}
			if (want != rel_on && win) {
				if (!SDL_SetWindowRelativeMouseMode(win, want) && want) fprintf(stderr, "mouse: capture refused: %s\n", SDL_GetError());
				if (want) rel_since_us = now_us();   /* (the capture's first motion can carry the pointer's jump to the centre: dropped) */
				if (!want) { if (gesture_drag) { gesture_drag = 0; touch_down = 0; } if (twist_drag) { twist_drag = 0; twist_down = twist_pad; } if (kbm_lmb) buttons &= ~RTRIG; if (kbm_rmb) buttons &= ~LTRIG; kbm_lmb = kbm_rmb = 0; __atomic_store_n(&mouse_dx_q8, 0, __ATOMIC_RELAXED); __atomic_store_n(&mouse_dy_q8, 0, __ATOMIC_RELAXED); }
				rel_on = want;
			}
			tilt_section_update();
			if (dt > 0) {   /* the motion sensor's source, and the virtual pad's tilt */
				/* keyboard + mouse, or a pad without a gyro: the virtual pad; a pad with one in hand: its gyro. A change
				 * mid-section glides (motion.c) and the virtual tilt carries on from the angle the game was reading. */
				float roll;
				if (motion_use_virtual(on || !pad_gyro, &roll)) tilt = roll > TILT_MAX ? TILT_MAX : roll < -TILT_MAX ? -TILT_MAX : roll;
				float want = 0;
				if (tilt_section && !overlay_is_open()) {
					if (on) want = (float)(kb_binds[KB_MOVE_RIGHT].held - kb_binds[KB_MOVE_LEFT].held) * TILT_MAX;   /* A / D: eased toward a full tilt while held */
					else if (!pad_gyro) { float sx = (sticks[0] - 127.5f) / 127.5f; if (fabsf(sx) < 0.12f) sx = 0; want = sx * TILT_MAX; }   /* the left stick, analog */
				}
				tilt += (want - tilt) * (float)(1.0 - exp(-dt / (on ? 0.12 : 0.06)));   /* (out of a section: back to level) */
				motion_feed_virtual(tilt, SDL_GetTicksNS());   /* (dropped unless the virtual pad is the source) */
			}
			if (on && !overlay_is_open()) {
				int x = kb_binds[KB_MOVE_RIGHT].held - kb_binds[KB_MOVE_LEFT].held, y = kb_binds[KB_MOVE_DOWN].held - kb_binds[KB_MOVE_UP].held;
				float k = x && y ? 0.7071f : 1.0f;
				sticks[0] = (uint8_t)lroundf(127.5f + x * k * 127.5f); sticks[1] = (uint8_t)lroundf(127.5f + y * k * 127.5f);
				/* the look's stand-in: no raw-look hook has taken the motion lately -- counts per second
				 * as a right-stick deflection (about 900 counts/s full tilt at sensitivity 1), eased over ~8 ms */
				static float vx, vy;
				int raw = t - raw_taken_us < 300000;
				float tx = 0, ty = 0;
				if (rel_on && !raw && dt > 0) {
					float g = window_mouse_sens() / 900.0f / (float)dt;
					tx = look_dx_q8 / 256.0f * g; ty = look_dy_q8 / 256.0f * g * (window_mouse_invert() ? -1.0f : 1.0f);
					tx = tx > 1 ? 1 : tx < -1 ? -1 : tx; ty = ty > 1 ? 1 : ty < -1 ? -1 : ty;
				} else if (rel_on && raw) {
					/* a raw-look hook has the motion: only a nudge just past the game's dead zone on a frame the
					 * mouse moved, so a camera update the game runs only while the stick is deflected still runs
					 * (Uncharted's uc_camera_yaw, hooks.c) -- the turn itself is the hook's. Here, before the
					 * game's pad read: a nudge written after it (an IHOOK past the read) is overwritten unseen. */
					const float n = 0.0f;   /* (off: the hook sets the game's own "stick deflected" flag itself -- hooks.c uc_pad_nudge) */
					tx = look_dx_q8 > 0 ? n : look_dx_q8 < 0 ? -n : 0;
					ty = (look_dy_q8 > 0 ? n : look_dy_q8 < 0 ? -n : 0) * (window_mouse_invert() ? -1.0f : 1.0f);
				}
				look_dx_q8 = look_dy_q8 = 0;
				float a = dt > 0 ? (float)(dt / 0.008) : 1; if (a > 1) a = 1;   /* (eased over ~8 ms: one display frame's jitter, no more) */
				vx += (tx - vx) * a; vy += (ty - vy) * a;
				/* past the game's own radial dead zone: a slow mouse is a small deflection, which the stick
				 * curve would swallow -- start the motion at the dead zone's edge ("mouse_deadzone", the
				 * fraction of full tilt the game ignores; RECOMP_MOUSE_DEADZONE from a game's conf) */
				float ox = vx, oy = vy, m = sqrtf(vx * vx + vy * vy);
				if (m > 0.002f) {
					static float dz = -1; if (dz < 0) { const char *e = getenv("RECOMP_MOUSE_DEADZONE"); dz = (float)settings_float("mouse_deadzone", e ? atof(e) : 0.2); }
					float m2 = dz + m * (1 - dz); if (m2 > 1) m2 = 1;
					ox = vx * m2 / m; oy = vy * m2 / m;
				} else ox = oy = 0;
				sticks[2] = (uint8_t)lroundf(127.5f + ox * 127.5f); sticks[3] = (uint8_t)lroundf(127.5f + oy * 127.5f);
			}
		}
		{	/* a gesture window: a right-stick flick swipes (see flick_down) */
			static int armed = 1; static uint64_t t0; static float fx, fy;
			if (pad && gesture_wanted() && !cur_mode && !touchpad_active() && !overlay_is_open()) {
				float ax = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.f, ay = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.f, mag = sqrtf(ax * ax + ay * ay);
				if (!flick_down && armed && mag > 0.6f) { flick_down = 1; armed = 0; t0 = now_us(); fx = ax / mag; fy = ay / mag; flick_x = 960; flick_y = 544; fprintf(stderr, "    touch swipe: right-stick flick %.2f,%.2f\n", fx, fy); }
				if (mag < 0.3f) armed = 1;
			}
			if (flick_down) {
				double q = (now_us() - t0) / 80000.0;
				if (q >= 1.25) flick_down = 0;   /* (held at the end a moment, then lifted) */
				else { if (q > 1) q = 1; flick_x = clampi(960 + (int)lround(fx * 450 * q), 0, 1919); flick_y = clampi(544 + (int)lround(fy * 450 * q), 0, 1087); }
			}
		}
		{	/* a twist (see window_touch_twist_hint): the sticks are two thumbs on the wheel -- a finger 400 points out from the
			 * centre at full tilt, at the centre inside a 0.15 deadzone (a stick at rest jittering the line would creep the
			 * valve: it takes the turns its way and drops the others); down past 0.35 on either stick, up below 0.2 on both */
			const int w = twist_wanted() && !overlay_is_open();
			if (!w) { twist_drag = 0; twist_pad = 0; }
			else if (pad && !cur_mode && !touchpad_active() && !twist_drag) {
				float v[2][2], mx = 0;
				for (int st = 0; st < 2; st++) {
					float ax = SDL_GetGamepadAxis(pad, st ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX) / 32767.f;
					float ay = SDL_GetGamepadAxis(pad, st ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY) / 32767.f;
					float m = sqrtf(ax * ax + ay * ay); if (m > mx) mx = m;
					if (m < 0.15f) ax = ay = 0; else if (m > 1) { ax /= m; ay /= m; }
					v[st][0] = ax; v[st][1] = ay;
				}
				if (!twist_pad && mx > 0.35f) { twist_pad = 1; fprintf(stderr, "    touch twist: grip (sticks)\n"); }
				else if (twist_pad && mx < 0.2f) { twist_pad = 0; fprintf(stderr, "    touch twist: let go (sticks)\n"); }
				if (twist_pad) for (int i = 0; i < 2; i++) { twist_x[i] = clampi(960 + (int)lroundf(v[i][0] * 400), 0, 1919); twist_y[i] = clampi(544 + (int)lroundf(v[i][1] * 400), 0, 1087); }
			} else twist_pad = 0;
			twist_down = twist_pad || twist_drag;
			static int was, shown;   /* the prompt, the first few valves of a session */
			if (w && !was && shown < 3) { shown++; overlay_toast_titled("Two-finger turn", kbm_on ? "Hold the left button and drag down / up" : "Push both sticks out and roll them", 4.0); }
			if (!w && was) fprintf(stderr, "    touch twist: the game stopped asking for it\n");
			was = w;
		}
		{	/* "Compiling shaders": the game waiting on shader or pipeline builds -- one wait past 300 ms, or over 150 ms of the
			 * last half second in waits (a level's first load: hundreds of ~50 ms ones) -- or a big batch building in the
			 * background for over a second: a badge instead of an unexplained freeze or pop-in */
			extern void gfx_gpu_build_status(double *wait_ms, double *total_ms, int *inflight);
			double wait_ms, total_ms; int inflight; gfx_gpu_build_status(&wait_ms, &total_ms, &inflight);
			static uint64_t busy_since, win_t; static double win_total; const uint64_t t = now_us();
			if (inflight < 16) busy_since = 0; else if (!busy_since) busy_since = t;
			static int burst; if (t - win_t >= 500000) { burst = total_ms - win_total > 150; win_t = t; win_total = total_ms; }
			if (wait_ms > 300 || burst || (busy_since && t - busy_since > 1000000))
				overlay_toast_titled("Compiling shaders", "Building this area's graphics for the first time", 0.6);
		}
		{	/* the bumper cursor: either stick moves it (both add up), full deflection crossing the panel's
			 * width in about 1.3 s times the "touch_cursor_speed" setting; a small deadzone each keeps it still at rest */
			static uint64_t last; uint64_t t = now_us(); double dt = last ? (t - last) / 1e6 : 0; last = t;
			if (dt > 0.1) dt = 0.1;
			if (cur_mode && pad && !overlay_is_open()) {
				int port = cur_mode - 1;
				float vx = 0, vy = 0, sv[2][2] = {{0, 0}, {0, 0}};   /* sv: each stick's own vector, past its deadzone */
				for (int st = 0; st < 2; st++) {   /* left stick, then right */
					float ax = SDL_GetGamepadAxis(pad, st ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX) / 32767.f;
					float ay = SDL_GetGamepadAxis(pad, st ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY) / 32767.f;
					float mag = sqrtf(ax * ax + ay * ay);
					if (mag > 0.12f) { float k = (mag - 0.12f) / (0.88f * mag); sv[st][0] = ax * k; sv[st][1] = ay * k; vx += ax * k; vy += ay * k; if (st) cur_used_r = 1; else cur_used_l = 1; }   /* rescaled past the deadzone */
				}
				if (!cur2_on && cur_used_l && cur_used_r && !touchpad_active()) {   /* both sticks in this hold: the second cursor splits off the first */
					cur2_on = 1; cur2_x[port] = cur_x[port]; cur2_y[port] = cur_y[port];
				}
				if (cur2_on) {   /* two cursors: the left stick steers the first, the right the second, free steering (no straight-line drawing) */
					if (!touchpad_active()) {
						const double v = 1500.0 * settings_float("touch_cursor_speed", 1.0) * dt;
						cur_x[port] = clampi(cur_x[port] + (int)lround(sv[0][0] * v), 0, 1919); cur_y[port] = clampi(cur_y[port] + (int)lround(sv[0][1] * v), port ? REAR_Y0 : 0, port ? REAR_Y1 : 1087);
						cur2_x[port] = clampi(cur2_x[port] + (int)lround(sv[1][0] * v), 0, 1919); cur2_y[port] = clampi(cur2_y[port] + (int)lround(sv[1][1] * v), port ? REAR_Y0 : 0, port ? REAR_Y1 : 1087);
					}
					vx = vy = 0;   /* (the one-cursor path below moves nothing) */
				}
				float vm = sqrtf(vx * vx + vy * vy); if (vm > 1) { vx /= vm; vy /= vm; }   /* both sticks together: no faster than one at full tilt */
				/* drawing (Cross held: the touch is down), the cursor goes in straight lines along
				 * "touch_cursor_draw_directions" (16) directions: it turns when the stick has pointed into another
				 * sector for 40 ms, so a swing through the directions between is a clean corner, and a stick let
				 * back to centre turns at once. A stick steers a cursor in curves -- rounded corners, overshoot,
				 * wobble -- and drawn shapes are judged on shape alone (Uncharted's "$1" matcher: stick-traced
				 * Z's and loops scored 0.48-0.62 where a mouse's scored 0.76, 2026-10-13); straight strokes with
				 * sharp corners are what a finger draws. 0: free steering while drawing too. Aiming (Cross up)
				 * stays free either way. Fractions of a point carry over between frames, so a line at 22.5
				 * degrees stays one at any speed. */
				static int draw_dir = -1, pend_dir = -1; static double pend_t, carry_x, carry_y;
				int ndir = settings_int("touch_cursor_draw_directions", 16);
				if (!cur_down || !(vx || vy)) { draw_dir = pend_dir = -1; }
				if (!cur_down) carry_x = carry_y = 0;
				if ((vx || vy) && !touchpad_active()) {
					double v = 1500.0 * settings_float("touch_cursor_speed", 1.0) * dt, mx = vx, my = vy;
					if (cur_down && ndir >= 4) {
						double step = 2 * M_PI / ndir, mag = sqrt((double)vx * vx + (double)vy * vy);
						int sec = (int)lround(atan2(vy, vx) / step); sec = ((sec % ndir) + ndir) % ndir;
						if (draw_dir < 0) draw_dir = sec;
						if (sec == draw_dir) pend_dir = -1;
						else { if (sec != pend_dir) { pend_dir = sec; pend_t = 0; } pend_t += dt; if (pend_t >= 0.04) { draw_dir = sec; pend_dir = -1; } }
						mx = cos(draw_dir * step) * mag; my = sin(draw_dir * step) * mag;
					}
					double nx = mx * v + carry_x, ny = my * v + carry_y;
					int ix = (int)lround(nx), iy = (int)lround(ny);
					if (cur_down) { carry_x = nx - ix; carry_y = ny - iy; }
					cur_x[port] = clampi(cur_x[port] + ix, 0, 1919);
					cur_y[port] = clampi(cur_y[port] + iy, port ? REAR_Y0 : 0, port ? REAR_Y1 : 1087);
				}
			}
		}
		{	/* the menu just closed (any way: F10, Escape, a pad combo, its Resume button): the sticks moved
			 * while it had the events -- a stick let go with the menu up would otherwise stay deflected */
			static int was_open; int open = overlay_is_open() || overlay_dialog_active();   /* (the save dialog too) */
			if (open && !was_open) { buttons = 0; touch_down = 0; gamepad_touch_release(); for (int i = 0; i < KB_N; i++) kb_binds[i].held = 0; }   /* let go of what was held as it opened */
			if (was_open && !open) {
				kb_reload();   /* (the Controls page may have rebound keys) */
				for (int i = 0; i < 4; i++) sticks[i] = pad ? (uint8_t)((SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)i) + 32768) >> 8) : 128;
				pad_resync(pad);   /* and L and R: one held through the menu sends no new event (the other pair, held through, waits for a new press) */
			}
			was_open = open;
		}
		if (overlay_wants_quit()) { extern void host_summary(const char *why); extern void gfx_gpu_quit(void); host_summary("window closed"); gfx_gpu_quit(); _exit(0); }
		if (overlay_wants_capture()) { extern void gfxcap_request(void); gfxcap_request(); fprintf(stderr, "capture requested (overlay)\n"); }


		if (!gpu) { SDL_Delay(1); continue; }
		if (overlay_game_paused()) { extern void gfx_shader_prep_tick(void); gfx_shader_prep_tick(); }   /* (a paused game doesn't present: the shader warm-up steps on from here, its prep card's too) */
		if (self_paced) {   /* no vsync wait paces this loop: a present once the game has a new frame, else once a refresh (menus, the overlay); spinning presented ~1200 times a second and slowed the game */
			/* the games run on the Vita's fixed 60 Hz grid: pace the present loop to 60, not to a 120/144 Hz panel's
			 * refresh, or the same picture is submitted two-three times per game frame and the motion judders
			 * ("a slight stutter", reported 2026-11-07) */
			const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
			float hz = dm && dm->refresh_rate > 1 ? dm->refresh_rate : 60.0f;
			if (hz > 65.0f) hz = 60.0f;
			const uint64_t period = (uint64_t)(1e6 / hz);
			while (!__atomic_load_n(&gnew, __ATOMIC_ACQUIRE) && !__atomic_load_n(&new_frame, __ATOMIC_ACQUIRE) && now_us() - last_paced_us < period) SDL_DelayPrecise(250000);
			last_paced_us = now_us();
		}
		uint64_t t_blit0 = now_us();
		SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
		if (!cmd) continue;
		uint64_t t_blit1 = now_us();   /* the swapchain wait below is vsync, not a cost metrics.c should count */
		/* this loop's frame two submits back is finished before SDL's acquire below looks at its own
		 * in-flight fence (SDL allows two frames in flight: that fence is this frame's or an older one):
		 * a locked poll of this loop's own fence for it, then the sweep that retires it from SDL's
		 * submitted list -- in SDL 3.4.16 the acquire reads, waits on and releases that fence unlocked,
		 * racing every other thread's submit, and then blocks for vsync, so it can't run under the
		 * host's lock itself; see gpulock.h. The pacing is what it was: the acquire's wait finds the
		 * fence done and blocks only in nextDrawable, for vsync, as before. */
		static SDL_GPUFence *frame_fence[3]; static int frame_fi;
		{ int k = (frame_fi + 3 - frames_in_flight) % 3;   /* the submit SDL's acquire will look at: frames_in_flight back */
		  if (frame_fence[k]) { SDL_WaitForGPUFences(gpu, true, &frame_fence[k], 1); SDL_ReleaseGPUFence(gpu, frame_fence[k]); frame_fence[k] = 0; } }
		/* this loop's SDL_GPU calls under the renderer's lock (gfx_dispatch.c), with Vulkan: SDL's Vulkan backend (3.4.16) isn't
		 * safe against this thread recording and submitting while the renderer creates, records and submits on others. With
		 * GPU transfers and snapshots on (gfx_gpu.c), which make and copy more each frame, Killzone crashed inside SDL in this
		 * thread's submit, or jumped into garbage, in 3 of 4 windowed runs; under the lock, none in 7+ (2026-10-26). Mailbox
		 * doesn't wait in the acquire, so it goes under the lock too; with vsync the acquire waits a refresh, outside it.
		 * RECOMP_WINDOW_LOCK=0|1 picks */
		extern void gfx_lock_enter(void), gfx_lock_leave(void);
		static int wlock = -1; if (wlock < 0) { const char *e = getenv("RECOMP_WINDOW_LOCK"), *dv = SDL_GetGPUDeviceDriver(gpu); wlock = e && *e ? *e != '0' : dv && !strcmp(dv, "vulkan"); }
		if (wlock) gfx_lock_enter();
		gpu_sweep(gpu);
		if (wlock && !self_paced) gfx_lock_leave();   /* (vsync: the acquire waits a refresh, outside the lock) */
		SDL_GPUTexture *swap = 0;
		uint32_t ww = 0, wh = 0;
		int got_swap = SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win, &swap, &ww, &wh);
		uint64_t t_blit2 = now_us();
		if (wlock && !self_paced) gfx_lock_enter();
		if (got_swap && ww && wh) { win_pw = ww; win_ph = wh; }
		/* the frame to show, picked after the drawable wait: whatever the game flipped while this loop
		 * waited for vsync goes out now, not a refresh later */
		pthread_mutex_lock(&lock);
		int have_frame = new_frame;
		uint32_t pw = fw, ph = fh;
		SDL_GPUTransferBuffer *up_xfer = 0;
		if (gnew) { gshow = gidx; gnew = 0; }
		{ /* M1626: did this present iteration actually get a new frame, or the same texture again? The desktop
		   * duplication capture (ddagrab) shows every other displayed frame identical while the guest's draw
		   * signatures differ every frame, so the place to look is here: a loop that presents the same gshow twice.
		   * RECOMP_WINPRES=1 prints once per 60 loop presents. */
		  static int on = -1; static int last = -2, np, nd;
		  if (on < 0) { const char *e = getenv("RECOMP_WINPRES"); on = e && atoi(e) != 0; }
		  if (on) { np++; if (gshow != last) { nd++; last = gshow; }
		    if (np >= 60) { extern uint64_t display_frames(void); fprintf(stderr, "[WIN] f=%llu presents=%d newtex=%d same=%d gshow=%d\n",
		      (unsigned long long)display_frames(), np, nd, np - nd, gshow); np = nd = 0; } } }
		SDL_GPUTexture *gsrc = gshow >= 0 ? gtex[gshow] : 0; uint32_t gw = gshow >= 0 ? gtw[gshow] : 0, gh = gshow >= 0 ? gth[gshow] : 0, gl = gshow >= 0 ? gtl[gshow] : 1;
		if (have_frame && gpu && ensure_src_texture(pw, ph)) {
			void *dst = SDL_MapGPUTransferBuffer(gpu, xfer, true);
			/* the overlay goes on AFTER the game frame, into this upload copy -- never into
			 * `frame` itself (the software path's next guest-memory read overwrites it fresh
			 * anyway) and nowhere near guest memory, so RECOMP_SHOT (read straight from guest
			 * memory, elsewhere) never sees it */
			if (dst) { memcpy(dst, frame, 4 * pw * ph); if (!(overlay_draws_hud() && !osd_cpu())) metrics_osd_composite((uint8_t *)dst, pw, pw, ph); SDL_UnmapGPUTransferBuffer(gpu, xfer); up_xfer = xfer; }   /* (the ImGui HUD draws the overlay below otherwise) */
			new_frame = 0;
		}
		pthread_mutex_unlock(&lock);
		if (up_xfer) {
			SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
			SDL_GPUTextureTransferInfo src = { .transfer_buffer = up_xfer, .pixels_per_row = pw, .rows_per_layer = ph };
			SDL_GPUTextureRegion dst = { .texture = src_tex, .w = pw, .h = ph, .d = 1 };
			SDL_UploadToGPUTexture(cp, &src, &dst, true);
			SDL_EndGPUCopyPass(cp);
		}
		if (got_swap && swap && (gsrc || src_tex)) {
			/* the 960x544-aspect frame inside the (resizable) window, in whichever aspect mode the
			 * overlay's Display page picked (present_rect, above: fit/stretch/fill) */
			uint32_t sw = gsrc ? gw : stw, sh = gsrc ? gh : sth;
			float px, py, pw2, ph2, dx, dy, dw, dh;
			present_rect((float)ww, (float)wh, (float)sw, (float)sh, &px, &py, &pw2, &ph2, &dx, &dy, &dw, &dh);
			/* a frame with mip levels (window_present_gpu): the smallest level still at least the size it is
			 * drawn at, so the last linear step shrinks by under 2x */
			uint32_t lv = 0;
			if (gsrc) while (lv + 1 < gl && ph2 / (float)(2u << lv) >= dh && pw2 / (float)(2u << lv) >= dw) lv++;
			/* FXAA and sharpening (post.c), or else the plain scaled blit */
			extern int post_present(SDL_GPUDevice *, SDL_GPUCommandBuffer *, SDL_GPUTexture *, uint32_t, uint32_t, uint32_t, float, float, float, float, SDL_GPUTexture *, SDL_GPUTextureFormat, float, float, float, float);
			if (!post_present(gpu, cmd, gsrc ? gsrc : src_tex, sw, sh, gsrc ? gl : 1, px, py, pw2, ph2, swap, SDL_GetGPUSwapchainTextureFormat(gpu, win), dx, dy, dw, dh)) {
				SDL_GPUBlitInfo blit = {
					.source = { .texture = gsrc ? gsrc : src_tex, .mip_level = lv, .x = (Uint32)px >> lv, .y = (Uint32)py >> lv, .w = (Uint32)pw2 >> lv, .h = (Uint32)ph2 >> lv },
					.destination = { .texture = swap, .x = (Uint32)dx, .y = (Uint32)dy, .w = (Uint32)dw, .h = (Uint32)dh },
					.load_op = SDL_GPU_LOADOP_CLEAR, .clear_color = { 0, 0, 0, 1 }, .filter = SDL_GPU_FILTER_LINEAR,
				};
				SDL_BlitGPUTexture(cmd, &blit);
			}
			/* the overlay, if open, on top of the frame just blitted -- after it, on the same
			 * swapchain texture, for both the GPU-presented path (gsrc) and the software path
			 * (src_tex): see overlay_frame's own comment on why LOAD, not CLEAR */
			double dt = last_overlay_t ? (double)(t_blit0 - last_overlay_t) / 1e6 : 1.0 / 60.0;
			last_overlay_t = t_blit0;
			/* the free pointer as a touch cursor -- Alt / Option held, or a click under way -- over the picture: the
			 * bumper cursor's ring in its place (the OS arrow hidden, overlay_touch_cursor), blue on the front panel,
			 * orange on the rear (the right button), filled while pressed; over the bars or a menu, the arrow */
			int mt = 0, mtx = 0, mty = 0;
			if (!rel_on && !overlay_is_open() && !overlay_mouse_captured() && ((SDL_GetModState() & SDL_KMOD_ALT) || touch_down || mouse_rear_down)) {
				float px, py; SDL_GetMouseState(&px, &py);
				mt = mouse_to_touch(px, py, 0, &mtx, &mty);
			}
			overlay_touch_cursor(mt);
			overlay_frame(cmd, swap, ww, wh, dt);
			/* the gamepad-touch markers: the bumper cursor, the touchpad's fingers, a right-button rear
			 * touch; the rear panel's rows outlined while rear touch is in play */
			OverlayMark mk[TP_FINGERS + ST_FINGERS + 6]; int nm = 0; float rear[4] = {0, 0, 0, 0};
			if (flick_down) { touch_to_point(flick_x, flick_y, &mk[nm].x, &mk[nm].y); mk[nm].rear = 0; mk[nm].pressed = 1; mk[nm].cursor = 0; nm++; }
			if (twist_down) for (int i = 0; i < 2; i++) { touch_to_point(twist_x[i], twist_y[i], &mk[nm].x, &mk[nm].y); mk[nm].rear = 0; mk[nm].pressed = 1; mk[nm].cursor = 0; nm++; }
			if (cur_mode) { touch_to_point(cur_x[cur_mode - 1], cur_y[cur_mode - 1], &mk[nm].x, &mk[nm].y); mk[nm].rear = cur_mode == 2; mk[nm].pressed = cur_down; mk[nm].cursor = 1; nm++; }
			if (cur_mode && cur2_on) { touch_to_point(cur2_x[cur_mode - 1], cur2_y[cur_mode - 1], &mk[nm].x, &mk[nm].y); mk[nm].rear = cur_mode == 2; mk[nm].pressed = cur_down; mk[nm].cursor = 1; nm++; }
			for (int i = 0; i < TP_FINGERS; i++) if (tp_down[i]) { touch_to_point(tp_x[i], tp_y[i], &mk[nm].x, &mk[nm].y); mk[nm].rear = tp_port[i]; mk[nm].pressed = 1; mk[nm].cursor = 0; nm++; }
			for (int i = 0; i < ST_FINGERS; i++) if (st_down[i] && st_port[i]) { touch_to_point(st_x[i], st_y[i], &mk[nm].x, &mk[nm].y); mk[nm].rear = 1; mk[nm].pressed = 1; mk[nm].cursor = 0; nm++; }   /* (a rear touch from the screen: the finger isn't where it lands) */
			if (mt) { touch_to_point(mtx, mouse_rear_down ? clampi(mty, REAR_Y0, REAR_Y1) : mty, &mk[nm].x, &mk[nm].y); mk[nm].rear = mouse_rear_down; mk[nm].pressed = touch_down || mouse_rear_down; mk[nm].cursor = 1; nm++; }
			else if (mouse_rear_down) { touch_to_point(mouse_rear_x, mouse_rear_y, &mk[nm].x, &mk[nm].y); mk[nm].rear = 1; mk[nm].pressed = 1; mk[nm].cursor = 0; nm++; }
			int rear_in_play = cur_mode == 2 || rb_held || mouse_rear_down;
			for (int i = 0; i < TP_FINGERS; i++) rear_in_play |= tp_down[i] && tp_port[i];
			for (int i = 0; i < ST_FINGERS; i++) rear_in_play |= st_down[i] && st_port[i];
			if (rear_in_play) { float x0, y0, x1, y1; touch_to_point(0, REAR_Y0, &x0, &y0); touch_to_point(1920, REAR_Y1 + 1, &x1, &y1); rear[0] = x0; rear[1] = y0; rear[2] = x1 - x0; rear[3] = y1 - y0; }
			overlay_marks(cmd, swap, ww, wh, dt, mk, nm, rear);
		}
		frame_fence[frame_fi] = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd); frame_fi = (frame_fi + 1) % 3;   /* vsync via the swapchain paces this loop; the fence is for the poll above, frames_in_flight on */
		if (wlock) gfx_lock_leave();
		uint64_t t_blit3 = now_us();
		{	/* each newly shown GPU frame: the flip -> submit time, plus the compositor and scan-out estimate,
			 * into the automatic A/V sync's picture term (av_video_ms); RECOMP_LATENCY_LOG=1 prints the
			 * terms once a second */
			static int on = -1; if (on < 0) on = getenv("RECOMP_LATENCY_LOG") != 0;
			static int last_shown = -1; static uint64_t sum, n, mx, t_rep; static float refresh_ms = 16.7f; static uint64_t t_mode;
			if (t_blit3 - t_mode > 2000000) {   /* the display's refresh, re-read now and then (the window may move) */
				const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
				if (dm && dm->refresh_rate > 1) refresh_ms = 1000.0f / dm->refresh_rate;
				t_mode = t_blit3;
			}
			if (gsrc && gshow >= 0 && gshow != last_shown && gready_us[gshow]) {
				uint64_t d = t_blit3 - gready_us[gshow];
				if (d < 200000) av_video_ms += ((float)d / 1e3f + 1.5f * refresh_ms - av_video_ms) * 0.05f;
				sum += d; n++; if (d > mx) mx = d;
				if (on && t_blit3 - t_rep > 1000000) {
					extern float display_frame_period_ms(void);
					fprintf(stderr, "    latency: flip -> submit avg %.1f ms max %.1f ms over %llu frames (frames in flight %d); A/V: flip interval %.1f + after flip %.1f (refresh %.1f) - audio %.1f/%.1f -> delay %d/%d ms\n",
					        sum / 1e3 / (double)n, mx / 1e3, (unsigned long long)n, frames_in_flight, display_frame_period_ms(), av_video_ms, refresh_ms, av_audio_ms[0], av_audio_ms[1], av_applied[0], av_applied[1]);
					sum = n = mx = 0; t_rep = t_blit3;
				}
			}
			last_shown = gshow;
		}
		metrics_window_frame((t_blit1 - t_blit0) + (t_blit3 - t_blit2));   /* upload + blit/submit; (t_blit2 - t_blit1) is the vsync wait, excluded */

#ifndef RECOMP_MINIMAL
		char title[160];
		if (metrics_title(title, sizeof title)) SDL_SetWindowTitle(win, title);
#endif
	}
}
