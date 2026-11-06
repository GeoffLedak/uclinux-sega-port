/*
 * progman.c -- Program Manager: program groups holding program icons
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "win.h"

#define CELL_W		48
#define CELL_H		36

typedef struct {
	const char *title;
	const unsigned char *icon;
	void (*start)(void);
} Item;

typedef struct {
	const Item *items;
	int n, sel;
} Group;

static void start_notepad(void) { app_notepad(0); }
static void start_filemgr(void) { app_filemgr("/"); }

static const Item main_items[] = {
	{ "File Manager", icon_filemgr, start_filemgr },
	{ "Control Panel", icon_control, app_control },
	{ "Read Me", icon_readme, app_readme },
};
static const Item acc_items[] = {
	{ "Notepad", icon_notepad, start_notepad },
	{ "Calculator", icon_calc, app_calc },
	{ "Clock", icon_clock, app_clock },
	{ "Paint", icon_paint, app_paint },
};
static const Item game_items[] = {
	{ "Puzzle", icon_puzzle, app_puzzle },
};

static Group groups[3] = {
	{ main_items, COUNT(main_items), 0 },
	{ acc_items, COUNT(acc_items), 0 },
	{ game_items, COUNT(game_items), 0 },
};
static const char *group_names[3] = { "Main", "Accessories", "Games" };
static Win *group_win[3];
static Win *pm;
static int minimize_on_use, auto_arrange = 1;

enum {
	CM_OPEN = 1, CM_RUN, CM_EXIT, CM_AUTOARRANGE, CM_MINONUSE,
	CM_CASCADE, CM_TILE, CM_ARRANGE, CM_GROUP0, CM_GROUP1, CM_GROUP2, CM_ABOUT
};

static MenuItem file_items[] = {
	{ "&Open\tEnter", CM_OPEN, 0 },
	{ 0, 0, 0 },
	{ "&Run...", CM_RUN, 0 },
	{ 0, 0, 0 },
	{ "E&xit...", CM_EXIT, 0 },
};
static MenuItem opt_items[] = {
	{ "&Auto Arrange", CM_AUTOARRANGE, 0 },
	{ "&Minimize on Use", CM_MINONUSE, 0 },
};
static MenuItem win_items[] = {
	{ "&Cascade", CM_CASCADE, 0 },
	{ "&Tile", CM_TILE, 0 },
	{ "&Arrange Icons", CM_ARRANGE, 0 },
	{ 0, 0, 0 },
	{ "&1 Main", CM_GROUP0, 0 },
	{ "&2 Accessories", CM_GROUP1, 0 },
	{ "&3 Games", CM_GROUP2, 0 },
};
static MenuItem help_items[] = {
	{ "&About Program Manager...", CM_ABOUT, 0 },
};
static Menu pm_menus[] = {
	{ "&File", file_items, COUNT(file_items) },
	{ "&Options", opt_items, COUNT(opt_items) },
	{ "&Window", win_items, COUNT(win_items) },
	{ "&Help", help_items, COUNT(help_items) },
};
static MenuBar pm_bar = { pm_menus, COUNT(pm_menus) };

static int columns(Win *w)
{
	Rect c;
	int n;

	win_client(w, &c);
	n = c.r / CELL_W;
	return n < 1 ? 1 : n;
}

static void launch(Group *g)
{
	if (g->sel < 0 || g->sel >= g->n)
		return;
	busy(1);
	repaint_now();
	hw_flush();
	g->items[g->sel].start();
	busy(0);
	if (minimize_on_use)
		win_set_state(pm, W_MIN);
}

static int group_proc(Win *w, int msg, int a, int b)
{
	Group *g = w->data;
	int i, cols, x, y;

	switch (msg) {
	case M_PAINT:
		cols = columns(w);
		for (i = 0; i < g->n; i++) {
			x = (i % cols) * CELL_W;
			y = (i / cols) * CELL_H + 3;
			draw_icon_label(x, y, CELL_W, g->items[i].icon, g->items[i].title,
					i == g->sel && w->parent->active == w && win_top(w) == active,
					C_BLACK);
		}
		return 1;
	case M_LDOWN:
	case M_LDBL:
		cols = columns(w);
		i = (b - 3) / CELL_H * cols + a / CELL_W;
		if (a >= cols * CELL_W || b < 3 || i >= g->n)
			return 1;
		if (i != g->sel) {
			g->sel = i;
			invalidate(w);
		}
		if (msg == M_LDBL)
			launch(g);
		return 1;
	case M_KEY:
		cols = columns(w);
		i = g->sel;
		switch (a) {
		case K_LEFT: i--; break;
		case K_RIGHT: i++; break;
		case K_UP: i -= cols; break;
		case K_DOWN: i += cols; break;
		case K_HOME: i = 0; break;
		case K_END: i = g->n - 1; break;
		case K_ENTER: launch(g); return 1;
		default: return 0;
		}
		if (i >= 0 && i < g->n && i != g->sel) {
			g->sel = i;
			invalidate(w);
		}
		return 1;
	case M_CLOSE:
		/* closing a group makes it an icon */
		win_set_state(w, W_MIN);
		return 1;
	case M_ACTIVATE:
		invalidate(w);
		return 1;
	}
	return def_proc(w, msg, a, b);
}

static void show_group(int i)
{
	Win *w = group_win[i];

	if (w->state == W_MIN)
		win_set_state(w, w->was_max ? W_MAX : W_NORMAL);
	win_activate(w);
}

static void about(void)
{
	msgbox(pm, "About Program Manager",
	       "Program Manager\n"
	       "A graphical shell for SEGA Linux.\n\n"
	       "Move the pointer with the d-pad.\n"
	       "A = click, B = right click,\n"
	       "C = double click.", MB_OK | MB_INFO);
}

static int pm_proc(Win *w, int msg, int a, int b)
{
	char buf[128];
	Win *g;

	switch (msg) {
	case M_INITMENU:
		opt_items[0].flags = auto_arrange ? MF_CHECK : 0;
		opt_items[1].flags = minimize_on_use ? MF_CHECK : 0;
		for (a = 0; a < 3; a++)
			win_items[4 + a].flags = (w->active == group_win[a]) ? MF_CHECK : 0;
		return 1;
	case M_COMMAND:
		switch (a) {
		case CM_OPEN:
			g = w->active;
			if (g && g->state != W_MIN)
				launch(g->data);
			else if (g)
				show_group(g == group_win[0] ? 0 : g == group_win[1] ? 1 : 2);
			return 1;
		case CM_RUN:
			buf[0] = 0;
			if (input_box(w, "Run", "Command Line:", buf, sizeof buf) && buf[0])
				if (run_program(buf) < 0)
					msgbox(w, "Program Manager", "Cannot run the program.",
					       MB_OK | MB_STOP);
			return 1;
		case CM_EXIT:
			send(w, M_CLOSE, 0, 0);
			return 1;
		case CM_AUTOARRANGE:
			auto_arrange = !auto_arrange;
			return 1;
		case CM_MINONUSE:
			minimize_on_use = !minimize_on_use;
			return 1;
		case CM_CASCADE: cascade(w); return 1;
		case CM_TILE: tile(w); return 1;
		case CM_ARRANGE: arrange_icons(w); return 1;
		case CM_GROUP0: case CM_GROUP1: case CM_GROUP2:
			show_group(a - CM_GROUP0);
			return 1;
		case CM_ABOUT:
			about();
			return 1;
		}
		break;
	case M_CLOSE:
		if (msgbox(w, "Exit", "This will end your session.",
			   MB_OKCANCEL | MB_EXCLAIM) == ID_OK)
			quit = 1;
		return 1;
	case M_SIZE:
		if (auto_arrange)
			arrange_icons(w);
		return 1;
	case M_KEY:
		if (a == (K_CTRL | (K_F1 + 5)) || a == (K_F1 + 5)) {	/* Ctrl+F6: next group */
			if (w->active)
				send(w->active, M_COMMAND, SC_NEXT, 0);
			return 1;
		}
		return 0;
	}
	return def_proc(w, msg, a, b);
}

void progman_start(void)
{
	int i;

	pm = win_create(root, 12, 6, 296, 176, S_OVERLAPPED, "Program Manager", pm_proc, 0);
	pm->menu = &pm_bar;
	pm->icon = icon_progman;
	pm->bg = col_workspace;
	for (i = 2; i >= 0; i--) {
		group_win[i] = win_create(pm, 4 + i * 12, 4 + i * 12, 4 * CELL_W + 12, CELL_H + 22,
					  S_OVERLAPPED | S_MDI, group_names[i], group_proc, &groups[i]);
		group_win[i]->icon = icon_group;
	}
	win_set_state(group_win[1], W_MIN);
	win_set_state(group_win[2], W_MIN);
	win_activate(group_win[0]);
	win_activate(pm);
}
