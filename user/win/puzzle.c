/*
 * puzzle.c -- Puzzle: slide the numbered tiles into order
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "win.h"

#define N	4
#define TILE	24

typedef struct {
	unsigned char t[N * N];		/* 0 = the gap */
	int moves, solved;
} Puzzle;

enum { CM_NEW = 1, CM_EXIT, CM_ABOUT };

static MenuItem game_items[] = {
	{ "&New\tF2", CM_NEW, 0 },
	{ 0, 0, 0 },
	{ "E&xit", CM_EXIT, 0 },
};
static MenuItem help_items[] = {
	{ "&About Puzzle...", CM_ABOUT, 0 },
};
static Menu pz_menus[] = {
	{ "&Game", game_items, COUNT(game_items) },
	{ "&Help", help_items, COUNT(help_items) },
};
static MenuBar pz_bar = { pz_menus, COUNT(pz_menus) };

static int gap(Puzzle *p)
{
	int i;

	for (i = 0; i < N * N; i++)
		if (!p->t[i])
			return i;
	return 0;
}

/* slide the tiles between the gap and tile i (same row or column) */
static int slide(Puzzle *p, int i)
{
	int g = gap(p), step;

	if (i == g)
		return 0;
	if (i / N == g / N)
		step = (i > g) ? 1 : -1;
	else if (i % N == g % N)
		step = (i > g) ? N : -N;
	else
		return 0;
	while (g != i) {
		p->t[g] = p->t[g + step];
		g += step;
	}
	p->t[i] = 0;
	return 1;
}

static int is_solved(Puzzle *p)
{
	int i;

	for (i = 0; i < N * N - 1; i++)
		if (p->t[i] != i + 1)
			return 0;
	return 1;
}

static void shuffle(Puzzle *p)
{
	int i, g, k;

	srand(hw_frames());
	for (i = 0; i < N * N; i++)
		p->t[i] = (i + 1) % (N * N);
	/* random moves from the solved position, so it can always be solved */
	for (k = 0; k < 300; k++) {
		g = gap(p);
		i = (rand() >> 4) % (N * N);
		slide(p, i);
		(void)g;
	}
	if (is_solved(p))
		slide(p, N * N - 2);
	p->moves = 0;
	p->solved = 0;
}

static int puzzle_proc(Win *w, int msg, int a, int b)
{
	Puzzle *p = w->data;
	int i, x, y;
	char buf[60];

	switch (msg) {
	case M_PAINT:
		for (i = 0; i < N * N; i++) {
			x = 4 + (i % N) * TILE;
			y = 4 + (i / N) * TILE;
			if (!p->t[i]) {
				fill(x, y, x + TILE, y + TILE, C_GRAY);
				continue;
			}
			frame(x, y, x + TILE, y + TILE, C_BLACK);
			fill(x + 1, y + 1, x + TILE - 1, y + TILE - 1, C_SILVER);
			bevel(x + 1, y + 1, x + TILE - 1, y + TILE - 1, 1);
			bevel(x + 2, y + 2, x + TILE - 2, y + TILE - 2, 1);
			sprintf(buf, "%d", p->t[i]);
			text(x + (TILE - text_w(buf, F_BOLD)) / 2, y + (TILE - FONT_H) / 2,
			     buf, p->solved ? C_GREEN : C_NAVY, F_BOLD);
		}
		frame(3, 3, 5 + N * TILE, 5 + N * TILE, C_BLACK);
		sprintf(buf, "Moves: %d", p->moves);
		text(4, 10 + N * TILE, buf, C_BLACK, F_BOLD);
		return 1;
	case M_LDOWN:
	case M_LDBL:
		if (p->solved || a < 4 || b < 4)
			return 1;
		x = (a - 4) / TILE;
		y = (b - 4) / TILE;
		if (x >= N || y >= N)
			return 1;
		if (slide(p, y * N + x)) {
			p->moves++;
			invalidate(w);
			if (is_solved(p)) {
				p->solved = 1;
				repaint_now();
				sprintf(buf, "Solved in %d moves!", p->moves);
				msgbox(w, "Puzzle", buf, MB_OK | MB_INFO);
			}
		} else
			beep();
		return 1;
	case M_KEY:
		/* arrows move a tile into the gap */
		i = gap(p);
		switch (a) {
		case K_UP: x = i + N; break;
		case K_DOWN: x = i - N; break;
		case K_LEFT: x = (i % N < N - 1) ? i + 1 : -1; break;
		case K_RIGHT: x = (i % N > 0) ? i - 1 : -1; break;
		case K_F1 + 1: send(w, M_COMMAND, CM_NEW, 0); return 1;
		default: return 0;
		}
		if (x >= 0 && x < N * N && !p->solved)
			send(w, M_LDOWN, 4 + (x % N) * TILE + 1, 4 + (x / N) * TILE + 1);
		return 1;
	case M_COMMAND:
		switch (a) {
		case CM_NEW:
			shuffle(p);
			invalidate(w);
			return 1;
		case CM_EXIT:
			send(w, M_CLOSE, 0, 0);
			return 1;
		case CM_ABOUT:
			msgbox(w, "About Puzzle", "Puzzle\n\nClick a tile next to the gap\n"
			       "(or in line with it) to slide it.\nPut the numbers in order.",
			       MB_OK | MB_INFO);
			return 1;
		}
		break;
	case M_DESTROY:
		free(p);
		return 1;
	}
	return def_proc(w, msg, a, b);
}

void app_puzzle(void)
{
	Puzzle *p = calloc(1, sizeof *p);
	Win *w;

	if (!p)
		return;
	shuffle(p);
	w = win_create(root, 100, 30, N * TILE + 16, N * TILE + 46,
		       S_CAPTION | S_SYSMENU | S_MINBOX | S_BORDER, "Puzzle", puzzle_proc, p);
	w->menu = &pz_bar;
	w->icon = icon_puzzle;
	w->bg = C_SILVER;
}
