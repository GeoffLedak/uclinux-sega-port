/*
 * mines.c -- Mines: uncover every square that has no mine under it
 *
 * Left click (pad A) uncovers a square; a number says how many of the
 * eight squares round it have mines. Right click (B) puts a flag on a
 * square you think has a mine. A double click (C) on a number whose mines
 * are all flagged uncovers the rest of the squares round it. The first
 * square uncovered never has a mine: the mines are laid after it.
 * Keys: arrows move, Space or Enter uncovers, F flags, F2 starts again.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "win.h"

#define CELL		12		/* square size */
#define BX		3		/* the board's top left in the window */
#define BY		3
#define STATUS_H	15		/* the line under the board */
#define MAX_COLS	22
#define MAX_ROWS	11
#define MAX_CELLS	(MAX_COLS * MAX_ROWS)

/* each square: the number of mines round it, and */
#define NUM		0x0F
#define MINE		0x10
#define OPEN		0x20
#define FLAG		0x40
#define BOOM		0x80		/* the mine that went off */

static const struct { int cols, rows, mines; } levels[] = {
	{ 9, 9, 10 }, { 16, 11, 28 }, { 22, 11, 45 },
};

typedef struct {
	int level, cols, rows, mines;
	unsigned char sq[MAX_CELLS];
	int laid;			/* the mines are down (after the first square) */
	int over, won;
	int opened, flags;
	int running;			/* the clock */
	time_t start;
	int secs;
	int cur, keys;			/* the keyboard's square, shown once keys are used */
	short todo[MAX_CELLS];		/* (open_from()) */
} Mines;

enum { CM_NEW = 1, CM_SMALL, CM_MEDIUM, CM_LARGE, CM_EXIT, CM_ABOUT };

static MenuItem game_items[] = {
	{ "&New\tF2", CM_NEW, 0 },
	{ 0, 0, 0 },
	{ "&Small: 9 x 9, 10 mines", CM_SMALL, 0 },
	{ "&Medium: 16 x 11, 28 mines", CM_MEDIUM, 0 },
	{ "&Large: 22 x 11, 45 mines", CM_LARGE, 0 },
	{ 0, 0, 0 },
	{ "E&xit", CM_EXIT, 0 },
};
static MenuItem help_items[] = {
	{ "&About Mines...", CM_ABOUT, 0 },
};
static Menu mn_menus[] = {
	{ "&Game", game_items, COUNT(game_items) },
	{ "&Help", help_items, COUNT(help_items) },
};
static MenuBar mn_bar = { mn_menus, COUNT(mn_menus) };

/* the colours of the numbers 1 - 8 */
static const unsigned char num_col[9] = {
	0, C_BLUE, C_GREEN, C_RED, C_PURPLE, C_MAROON, C_TEAL, C_BLACK, C_GRAY
};

/* the squares round i; returns how many */
static int round_sq(Mines *m, int i, int *out)
{
	int x = i % m->cols, y = i / m->cols, dx, dy, n = 0;

	for (dy = -1; dy <= 1; dy++)
		for (dx = -1; dx <= 1; dx++) {
			if ((!dx && !dy) || x + dx < 0 || x + dx >= m->cols ||
			    y + dy < 0 || y + dy >= m->rows)
				continue;
			out[n++] = i + dy * m->cols + dx;
		}
	return n;
}

static void new_game(Mines *m)
{
	m->cols = levels[m->level].cols;
	m->rows = levels[m->level].rows;
	m->mines = levels[m->level].mines;
	memset(m->sq, 0, sizeof m->sq);
	m->laid = m->over = m->won = 0;
	m->opened = m->flags = 0;
	m->running = 0;
	m->secs = 0;
	if (m->cur >= m->cols * m->rows)
		m->cur = 0;
}

/* put the mines down anywhere but on first and round it */
static void lay(Mines *m, int first)
{
	int n = m->cols * m->rows, k, i, r[8], j, c;

	srand(hw_frames());
	for (k = 0; k < m->mines; ) {
		i = (rand() >> 4) % n;
		if (m->sq[i] & MINE)
			continue;
		if (i == first)
			continue;
		c = round_sq(m, first, r);
		for (j = 0; j < c && r[j] != i; j++)
			;
		if (j < c)
			continue;
		m->sq[i] |= MINE;
		k++;
	}
	for (i = 0; i < n; i++) {
		c = round_sq(m, i, r);
		for (j = 0; j < c; j++)
			if (m->sq[r[j]] & MINE)
				m->sq[i]++;
	}
	m->laid = 1;
	m->running = 1;
	m->start = time(0);
}

/* uncover i (no mine), and everything round each 0 it leads to */
static void open_from(Mines *m, int i)
{
	int sp = 0, r[8], c, j;

	m->sq[i] |= OPEN;
	m->opened++;
	m->todo[sp++] = i;
	while (sp) {
		i = m->todo[--sp];
		if (m->sq[i] & NUM)
			continue;
		c = round_sq(m, i, r);
		for (j = 0; j < c; j++)
			if (!(m->sq[r[j]] & (OPEN | FLAG | MINE))) {
				m->sq[r[j]] |= OPEN;
				m->opened++;
				m->todo[sp++] = r[j];
			}
	}
}

static void finish(Win *w, Mines *m)
{
	int i, n = m->cols * m->rows;
	char buf[60];

	m->over = 1;
	m->running = 0;
	if (m->won) {
		for (i = 0; i < n; i++)		/* flag the rest */
			if (m->sq[i] & MINE)
				m->sq[i] |= FLAG;
		m->flags = m->mines;
	}
	invalidate(w);
	if (m->won) {
		repaint_now();
		sprintf(buf, "You cleared the field in %d seconds!", m->secs);
		msgbox(w, "Mines", buf, MB_OK | MB_INFO);
	} else
		beep();
}

static void uncover(Win *w, Mines *m, int i)
{
	if (m->over || (m->sq[i] & (OPEN | FLAG)))
		return;
	if (!m->laid)
		lay(m, i);
	if (m->sq[i] & MINE) {
		m->sq[i] |= OPEN | BOOM;
		m->secs = time(0) - m->start;
		finish(w, m);
		return;
	}
	open_from(m, i);
	invalidate(w);
	if (m->opened == m->cols * m->rows - m->mines) {
		m->won = 1;
		m->secs = time(0) - m->start;
		finish(w, m);
	}
}

/* a number with all its mines flagged: uncover the other squares round it */
static void clear_round(Win *w, Mines *m, int i)
{
	int r[8], c, j, f = 0;

	if (m->over || !(m->sq[i] & OPEN) || !(m->sq[i] & NUM))
		return;
	c = round_sq(m, i, r);
	for (j = 0; j < c; j++)
		if (m->sq[r[j]] & FLAG)
			f++;
	if (f != (m->sq[i] & NUM)) {
		beep();
		return;
	}
	for (j = 0; j < c && !m->over; j++)
		uncover(w, m, r[j]);
}

static void flag(Win *w, Mines *m, int i)
{
	if (m->over || (m->sq[i] & OPEN))
		return;
	m->sq[i] ^= FLAG;
	m->flags += (m->sq[i] & FLAG) ? 1 : -1;
	invalidate(w);
}

/* the square at client x, y, or -1 */
static int sq_at(Mines *m, int x, int y)
{
	if (x < BX || y < BY)
		return -1;
	x = (x - BX) / CELL;
	y = (y - BY) / CELL;
	if (x >= m->cols || y >= m->rows)
		return -1;
	return y * m->cols + x;
}

static void draw_sq(Mines *m, int i)
{
	int x = BX + (i % m->cols) * CELL, y = BY + (i / m->cols) * CELL;
	int s = m->sq[i];
	char buf[2];

	/* at the end, show where the mines were */
	if (m->over && !m->won && (s & MINE) && !(s & FLAG))
		s |= OPEN;
	if (!(s & OPEN)) {
		fill(x, y, x + CELL, y + CELL, C_SILVER);
		bevel(x, y, x + CELL, y + CELL, 1);
		if (s & FLAG) {
			draw_icon(x + 2, y + 2, small_flag, 8);
			if (m->over && !(s & MINE)) {	/* a wrong flag */
				line(x + 2, y + 2, x + 9, y + 9, C_RED);
				line(x + 9, y + 2, x + 2, y + 9, C_RED);
			}
		}
	} else {
		fill(x, y, x + CELL, y + CELL, (s & BOOM) ? C_RED : C_WHITE);
		hline(x, x + CELL, y + CELL - 1, C_SILVER);
		vline(x + CELL - 1, y, y + CELL, C_SILVER);
		if (s & MINE)
			draw_icon(x + 2, y + 2, small_mine, 8);
		else if (s & NUM) {
			buf[0] = '0' + (s & NUM);
			buf[1] = 0;
			text(x + (CELL - 1 - text_w(buf, F_BOLD)) / 2 + 1, y + 2,
			     buf, num_col[s & NUM], F_BOLD);
		}
	}
	if (m->keys && i == m->cur && !m->over)
		dotted_frame(x + 1, y + 1, x + CELL - 1, y + CELL - 1);
}

static void draw_status(Win *w, Mines *m)
{
	Rect c;
	char buf[30];
	int y = BY + m->rows * CELL + 4;

	win_client(w, &c);
	if (m->over)
		strcpy(buf, m->won ? "Cleared!" : "Boom!");
	else
		sprintf(buf, "Mines: %d", m->mines - m->flags);
	text(BX, y, buf, (m->over && !m->won) ? C_RED : C_BLACK, F_BOLD);
	sprintf(buf, "Time: %d", m->secs > 999 ? 999 : m->secs);
	text(c.r - BX - text_w(buf, F_BOLD), y, buf, C_BLACK, F_BOLD);
}

/* the window's outer size for level lv */
static void outer_size(Win *w, int lv, int *ow, int *oh)
{
	Rect c;

	win_client(w, &c);
	*ow = (w->r.r - w->r.l) - c.r + 2 * BX + levels[lv].cols * CELL;
	*oh = (w->r.b - w->r.t) - c.b + BY + levels[lv].rows * CELL + STATUS_H;
}

static int fits(Win *w, int lv)
{
	int ow, oh;

	outer_size(w, lv, &ow, &oh);
	return ow <= SCR_W && oh <= SCR_H;
}

/* size the window for the board, keeping it on the screen */
static void fit_window(Win *w, Mines *m)
{
	int ow, oh, x = w->r.l, y = w->r.t;

	outer_size(w, m->level, &ow, &oh);
	if (x + ow > SCR_W)
		x = SCR_W - ow;
	if (y + oh > SCR_H)
		y = SCR_H - oh;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	win_move(w, x, y, ow, oh);
}

static int mines_proc(Win *w, int msg, int a, int b)
{
	Mines *m = w->data;
	int i, n, x, y, t;

	switch (msg) {
	case M_PAINT:
		n = m->cols * m->rows;
		bevel(BX - 1, BY - 1, BX + m->cols * CELL + 1, BY + m->rows * CELL + 1, 0);
		for (i = 0; i < n; i++)
			draw_sq(m, i);
		draw_status(w, m);
		return 1;
	case M_LDOWN:
	case M_LDBL:
	case M_RDOWN:
		i = sq_at(m, a, b);
		if (i < 0)
			return 1;
		if (m->keys) {			/* the mouse again: hide the key square */
			m->keys = 0;
			invalidate(w);
		}
		m->cur = i;
		if (msg == M_RDOWN)
			flag(w, m, i);
		else if (m->sq[i] & OPEN)
			clear_round(w, m, i);
		else
			uncover(w, m, i);
		return 1;
	case M_KEY:
		x = m->cur % m->cols;
		y = m->cur / m->cols;
		switch (a) {
		case K_UP:	if (y > 0) y--; break;
		case K_DOWN:	if (y < m->rows - 1) y++; break;
		case K_LEFT:	if (x > 0) x--; break;
		case K_RIGHT:	if (x < m->cols - 1) x++; break;
		case ' ':
		case K_ENTER:
			m->keys = 1;
			if (m->sq[m->cur] & OPEN)
				clear_round(w, m, m->cur);
			else
				uncover(w, m, m->cur);
			invalidate(w);
			return 1;
		case 'f':
		case 'F':
			m->keys = 1;
			flag(w, m, m->cur);
			return 1;
		case K_F1 + 1:
			send(w, M_COMMAND, CM_NEW, 0);
			return 1;
		default:
			return 0;
		}
		m->cur = y * m->cols + x;
		m->keys = 1;
		invalidate(w);
		return 1;
	case M_TIMER:
		if (m->running) {
			t = time(0) - m->start;
			if (t != m->secs) {
				m->secs = t;
				invalidate_client(w, 0, BY + m->rows * CELL + 2,
						  2 * BX + m->cols * CELL,
						  BY + m->rows * CELL + STATUS_H);
			}
		}
		return 1;
	case M_INITMENU:
		for (i = 0; i < 3; i++)
			game_items[2 + i].flags = (m->level == i ? MF_CHECK : 0) |
						  (fits(w, i) ? 0 : MF_GRAY);
		break;
	case M_COMMAND:
		switch (a) {
		case CM_NEW:
			new_game(m);
			invalidate(w);
			return 1;
		case CM_SMALL:
		case CM_MEDIUM:
		case CM_LARGE:
			if (!fits(w, a - CM_SMALL)) {
				msgbox(w, "Mines", "That board doesn't fit on\nthe screen.",
				       MB_OK | MB_EXCLAIM);
				return 1;
			}
			m->level = a - CM_SMALL;
			new_game(m);
			fit_window(w, m);
			invalidate(w);
			return 1;
		case CM_EXIT:
			send(w, M_CLOSE, 0, 0);
			return 1;
		case CM_ABOUT:
			msgbox(w, "About Mines", "Mines\n\n"
			       "Uncover every square without a mine.\n"
			       "A number tells how many mines touch\n"
			       "its square.\n\n"
			       "Click (A): uncover a square.\n"
			       "Right click (B): flag a mine.\n"
			       "Double click (C) on a number whose\n"
			       "mines are flagged: uncover the rest\n"
			       "round it.\n"
			       "Keys: arrows, Space, F to flag.",
			       MB_OK | MB_INFO);
			return 1;
		}
		break;
	case M_DESTROY:
		free(m);
		return 1;
	}
	return def_proc(w, msg, a, b);
}

void app_mines(void)
{
	Mines *m = calloc(1, sizeof *m);
	Win *w;

	if (!m)
		return;
	new_game(m);
	w = win_create(root, 110, 24, 120, 150, S_CAPTION | S_SYSMENU | S_MINBOX | S_BORDER,
		       "Mines", mines_proc, m);
	w->menu = &mn_bar;
	w->icon = icon_mines;
	w->bg = C_SILVER;
	fit_window(w, m);
	set_timer(w, 30);
}
