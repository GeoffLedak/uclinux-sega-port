/*
 * bench.c -- "win -b": time the drawing on the real machine
 *
 * Each test repeats one operation for about a second and reports the
 * time per operation. The results are printed after leaving graphics
 * mode. Frames are counted with the 60 Hz clock tick.
 */
#include <stdio.h>
#include <string.h>
#include "win.h"

#ifdef HOST
#include <time.h>
static unsigned long ticks(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 60 + ts.tv_nsec / 16666667;
}
#else
#define ticks() hw_frames()
#endif

static char report[4096];
static int rlen;

static Win *bench_win;
static int bench_flip;

typedef void (*Op)(void);

/* run op for at least a second; microseconds per call */
static long time_op(Op op)
{
	unsigned long start, t;
	long n = 0;

	/* start on a tick, so the count is not off by most of a frame */
	start = ticks();
	while (ticks() == start)
		;
	start = ticks();
	do {
		op();
		n++;
		t = ticks() - start;
	} while (t < 60);
	return (long)(t * 16683 / n);
}

static void all_dirty(void)
{
	memset(dirty, 0xFF, sizeof dirty);
	dirty[DIRTY_WORDS - 1] &= (1u << (SCR_TILES & 31)) - 1;
}

/* 10000 x 18 cycles = 180000 cycles: 23.5 ms on a 7.67 MHz 68000 */
static void op_cpu(void)
{
#ifndef HOST
	__asm__ __volatile__("movel #10000,%%d0\n1:\tsubql #1,%%d0\n\tbne 1b" : : : "d0");
#endif
}

static void op_render(void)
{
	invalidate(root);
	repaint_now();
	memset(dirty, 0, sizeof dirty);
}

static void op_fill(void);

static void op_upload(void)
{
	op_fill();				/* every tile changes */
	hw_flush();
}

static void op_same(void)
{
	all_dirty();				/* every tile redrawn the same */
	hw_flush();
}

static void op_fill(void)
{
	gfx_screen();
	fill(0, 0, SCR_W, SCR_H, (bench_flip++ & 1) ? C_SILVER : C_WHITE);
}

static void op_text(void)
{
	gfx_screen();
	text(4, 100, "The quick brown fox jumps over the dog", C_BLACK, F_NORMAL);
}

static void op_icon(void)
{
	gfx_screen();
	draw_icon(100, 100, icon_notepad, 16);
}

static void op_move(void)
{
	Rect r;

	win_screen(bench_win, &r);
	bench_flip = !bench_flip;
	win_move(bench_win, r.l + (bench_flip ? 8 : -8), r.t + (bench_flip ? 4 : -4),
		 r.r - r.l, r.b - r.t);
	repaint_now();
	hw_flush();
}

static void op_scroll(void)
{
	send(bench_win, M_VSCROLL, (bench_flip++ % 16) < 8 ? SB_LINEDOWN : SB_LINEUP, 0);
	repaint_now();
	hw_flush();
}

static void op_scroll_noup(void)
{
	send(bench_win, M_VSCROLL, (bench_flip++ % 16) < 8 ? SB_LINEDOWN : SB_LINEUP, 0);
	repaint_now();
	memset(dirty, 0, sizeof dirty);		/* (the screen is left behind) */
}

static void op_caret(void)
{
	bench_flip = !bench_flip;
	send(bench_win, M_KEY, bench_flip ? K_RIGHT : K_LEFT, 0);
	repaint_now();
	hw_flush();
}

static void op_blink(void)
{
	send(bench_win, M_TIMER, 0, 0);
	repaint_now();
	hw_flush();
}

static void op_area(void)
{
	Rect r;

	set_rect(&r, 60, 40, 160, 120);
	invalidate_screen(&r);
	repaint_now();
	hw_flush();
}

static void op_copy(void)
{
	bench_flip = !bench_flip;
	if (bench_flip)
		copy_rect(10, 10, 266, 190, 3, 2);
	else
		copy_rect(13, 12, 269, 192, -3, -2);
}

static void op_copy8(void)
{
	bench_flip = !bench_flip;
	if (bench_flip)
		copy_rect(10, 10, 266, 190, 8, 4);
	else
		copy_rect(18, 14, 274, 194, -8, -4);
}

static void result(const char *name, long us)
{
	rlen += sprintf(report + rlen, "%-22s %6ld.%ld ms\n", name, us / 1000, (us % 1000) / 100);
}

void bench(void)
{
	repaint_now();
	hw_flush();
	bench_win = active;			/* Program Manager */

	result("cpu (ideal 23.5)", time_op(op_cpu));
	result("full screen draw", time_op(op_render));
	result("fill screen", time_op(op_fill));
	result("fill + upload", time_op(op_upload));
	result("flush, unchanged", time_op(op_same));
	result("text, 38 chars", time_op(op_text));
	result("icon 16x16", time_op(op_icon));
	result("copy 256x180", time_op(op_copy));
	result("copy 256x180 by 8", time_op(op_copy8));
	invalidate(root);
	repaint_now();
	hw_flush();
	result("move window", time_op(op_move));
	wm_copy = 0;
	result("  (redrawing it)", time_op(op_move));
	wm_copy = 1;
	result("redraw 100x80", time_op(op_area));
	app_readme();
	bench_win = active;
	repaint_now();
	hw_flush();
	result("scroll Notepad line", time_op(op_scroll));
	result("  (without upload)", time_op(op_scroll_noup));
	invalidate(root);
	repaint_now();
	hw_flush();
	wm_copy = 0;
	result("  (redrawing it)", time_op(op_scroll));
	wm_copy = 1;
	result("Notepad caret move", time_op(op_caret));
	result("Notepad caret blink", time_op(op_blink));
}

/* "win -c": draw some fixed scenes and print a checksum of the screen
 * after each, to compare the Genesis (assembly) with the PC (C) */
static u32 fb_sum(void)
{
	u32 h = 0;
	int i;

	for (i = 0; i < SCR_TILES * 8; i++)
		h = ((h << 1) | (h >> 31)) ^ fb[i];
	return h;
}

static int nchecks;

static void check_step(const char *name)
{
	repaint_now();
	hw_flush();
	/* three to a line, so they all fit on the screen (by number:
	 * the names are in the calls below) */
	nchecks++;
	rlen += sprintf(report + rlen, "%2d %08lx%s", nchecks, (unsigned long)fb_sum(),
			(nchecks % 3) ? " " : "\n");
}

void check(void)
{
	Rect r;
	Win *pm = active, *w;
	const char *s;

	check_step("start");
	app_readme();
	check_step("readme");
	send(active, M_VSCROLL, SB_PAGEDOWN, 0);
	check_step("scroll");
	app_control();
	check_step("control");
	app_paint();
	check_step("paint");
	app_calc();
	check_step("calc");
	app_filemgr("/no such dir");
	check_step("filemgr");
	win_screen(pm, &r);
	win_move(pm, -37, 13, r.r - r.l, r.b - r.t);
	win_activate(pm);
	check_step("move left");
	win_move(pm, 41, 101, r.r - r.l, r.b - r.t);
	check_step("move down");
	/* moves that copy the pixels: must look the same as drawing it all */
	win_move(pm, 49, 105, r.r - r.l, r.b - r.t);
	check_step("copy 8,4");
	win_move(pm, 46, 98, r.r - r.l, r.b - r.t);
	check_step("copy -3,-7");
	win_move(pm, 51, 100, r.r - r.l, r.b - r.t);
	check_step("copy 5,2");
	win_move(pm, -10, 100, r.r - r.l, r.b - r.t);
	check_step("left edge");
	win_move(pm, -5, 97, r.r - r.l, r.b - r.t);
	check_step("copy 5,-3");
	win_move(pm, 290, 150, r.r - r.l, r.b - r.t);
	check_step("right edge");
	win_move(pm, 293, 152, r.r - r.l, r.b - r.t);
	check_step("copy 3,2");
	win_move(pm, 51, 100, r.r - r.l, r.b - r.t);
	check_step("back");
	invalidate(root);
	check_step("redrawn");

	/* Notepad redraws only the rows that change */
	app_readme();
	w = active;
	check_step("np readme");
	send(w, M_VSCROLL, SB_LINEDOWN, 0);
	send(w, M_VSCROLL, SB_LINEDOWN, 0);
	check_step("np ln down");
	send(w, M_VSCROLL, SB_LINEUP, 0);
	check_step("np ln up");
	send(w, M_LDOWN, 20, 12);
	send(w, M_MOVE, 70, 40);
	send(w, M_LUP, 70, 40);
	check_step("np select");
	send(w, M_KEY, K_DOWN, 0);
	check_step("np down");
	send(w, M_TIMER, 0, 0);
	check_step("np blink");
	send(w, M_KEY, K_PGDN, 0);
	check_step("np pg down");
	send(w, M_HSCROLL, SB_LINEDOWN, 0);
	check_step("np right");
	app_notepad(0);
	w = active;
	for (s = "Hello\tthere\nsecond line\nthird"; *s; s++)
		send(w, M_KEY, *s == '\n' ? K_ENTER : *s == '\t' ? K_TAB : *s, 0);
	check_step("np typed");
	send(w, M_KEY, K_BS, 0);
	send(w, M_KEY, K_BS, 0);
	check_step("np bksp");
	send(w, M_LDOWN, 3, 3);
	check_step("np click");
	send(w, M_KEY, K_ENTER, 0);
	check_step("np enter");
	send(w, M_KEY, K_BS, 0);
	check_step("np join");
	invalidate(root);
	check_step("np redrawn");		/* must be the same */
	if (nchecks % 3)
		rlen += sprintf(report + rlen, "\n");
}

void bench_report(void)
{
	fputs(report, stdout);
}
