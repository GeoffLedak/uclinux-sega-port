/*
 * control.c -- Control Panel: colours, the mouse, the date and time
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "win.h"

/* ---------------------------------------------------------------- */
/* Colour schemes                                                    */
/* ---------------------------------------------------------------- */

typedef struct {
	const char *name;
	unsigned char desktop, workspace, title, title_text, inactive,
		inactive_text, border, menu, hilite, window;
} Scheme;

static const Scheme schemes[] = {
	{ "Default",      C_SILVER, C_WHITE,  C_NAVY,   C_WHITE, C_WHITE,  C_BLACK, C_SILVER, C_WHITE,  C_NAVY,   C_WHITE },
	{ "Deep Water",   C_TEAL,   C_WHITE,  C_BLUE,   C_WHITE, C_SILVER, C_NAVY,  C_AQUA,   C_WHITE,  C_BLUE,   C_WHITE },
	{ "Moss",         C_GREEN,  C_WHITE,  C_GREEN,  C_WHITE, C_SILVER, C_BLACK, C_SILVER, C_WHITE,  C_GREEN,  C_WHITE },
	{ "Brick",        C_MAROON, C_SILVER, C_MAROON, C_WHITE, C_GRAY,   C_SILVER, C_SILVER, C_WHITE, C_MAROON, C_WHITE },
	{ "Grape Soda",   C_PURPLE, C_WHITE,  C_PURPLE, C_WHITE, C_SILVER, C_BLACK, C_FUCHSIA, C_WHITE, C_PURPLE, C_WHITE },
	{ "Sandstone",    C_OLIVE,  C_WHITE,  C_OLIVE,  C_WHITE, C_SILVER, C_BLACK, C_YELLOW, C_WHITE,  C_OLIVE,  C_WHITE },
	{ "Slate",        C_GRAY,   C_SILVER, C_BLACK,  C_WHITE, C_GRAY,   C_SILVER, C_SILVER, C_SILVER, C_BLACK, C_WHITE },
	{ "Night Shift",  C_BLACK,  C_GRAY,   C_TEAL,   C_WHITE, C_GRAY,   C_BLACK, C_GRAY,   C_SILVER, C_TEAL,   C_SILVER },
	{ "Bold Contrast", C_BLACK, C_WHITE,  C_BLUE,   C_YELLOW, C_WHITE, C_BLACK, C_YELLOW, C_WHITE,  C_BLUE,   C_WHITE },
};
static int scheme_now;

static void walk_bg(Win *w, int from_ws, int to_ws, int from_win, int to_win)
{
	for (; w; w = w->next) {
		if (w->bg == from_ws && w->menu && w->parent == root)
			w->bg = to_ws;		/* frames whose client is a workspace */
		else if (w->bg == from_win)
			w->bg = to_win;
		walk_bg(w->child, from_ws, to_ws, from_win, to_win);
	}
}

static void apply_scheme(const Scheme *s)
{
	walk_bg(root->child, col_workspace, s->workspace, col_window, s->window);
	col_desktop = s->desktop;
	col_workspace = s->workspace;
	col_title = s->title;
	col_title_text = s->title_text;
	col_inactive = s->inactive;
	col_inactive_text = s->inactive_text;
	col_border = s->border;
	col_menu = s->menu;
	col_hilite = s->hilite;
	col_window = s->window;
	invalidate(root);
}

#define ID_SCHEMES	100

typedef struct {
	Win *list;
	int sel;
} ColorDlg;

/* a small picture of two windows in a scheme */
static void preview(const Scheme *s, int x, int y)
{
	frame(x, y, x + 120, y + 70, C_BLACK);
	fill(x + 1, y + 1, x + 119, y + 69, s->desktop);
	/* the inactive window */
	frame(x + 6, y + 5, x + 76, y + 40, C_BLACK);
	fill(x + 7, y + 6, x + 75, y + 39, s->border);
	fill(x + 9, y + 8, x + 73, y + 16, s->inactive);
	text(x + 12, y + 8, "Inactive", s->inactive_text, F_BOLD);
	fill(x + 9, y + 16, x + 73, y + 37, s->workspace);
	/* the active window */
	frame(x + 30, y + 22, x + 114, y + 65, C_BLACK);
	fill(x + 31, y + 23, x + 113, y + 64, s->border);
	fill(x + 33, y + 25, x + 111, y + 33, s->title);
	text(x + 36, y + 25, "Active", s->title_text, F_BOLD);
	fill(x + 33, y + 33, x + 111, y + 42, s->menu);
	text(x + 35, y + 34, "File", C_BLACK, F_NORMAL);
	fill(x + 55, y + 33, x + 78, y + 42, s->hilite);
	text(x + 57, y + 34, "Edit", C_WHITE, F_NORMAL);
	fill(x + 33, y + 42, x + 111, y + 62, s->window);
	text(x + 36, y + 46, "Window text", C_BLACK, F_NORMAL);
}

static int color_proc(Win *w, int msg, int a, int b)
{
	ColorDlg *d = w->data;

	switch (msg) {
	case M_PAINT:
		text(8, 4, "Color Schemes:", C_BLACK, F_BOLD);
		preview(&schemes[d->sel], 118, 16);
		return 1;
	case M_COMMAND:
		if (a == ID_SCHEMES && b == N_SELCHANGE) {
			d->sel = list_sel(d->list);
			invalidate_client(w, 118, 16, 240, 88);
			return 1;
		}
		if (a == ID_SCHEMES && b == N_DBLCLK)
			a = ID_OK;
		if (a == ID_OK) {
			scheme_now = d->sel;
			apply_scheme(&schemes[d->sel]);
			dialog_end(w, ID_OK);
			return 1;
		}
		break;
	}
	return def_proc(w, msg, a, b);
}

static void color_dialog(Win *owner)
{
	ColorDlg d;
	Win *dlg;
	int i;

	d.sel = scheme_now;
	dlg = dialog_create(owner, 246, 110, "Color", color_proc, &d);
	d.list = ctl_list(dlg, 8, 16, 102, 72, ID_SCHEMES);
	for (i = 0; i < COUNT(schemes); i++)
		list_add(d.list, schemes[i].name, 0);
	list_set_sel(d.list, d.sel);
	ctl_button(dlg, 118, 92, 44, 14, "OK", ID_OK, 1);
	ctl_button(dlg, 170, 92, 44, 14, "Cancel", ID_CANCEL, 0);
	win_set_focus(d.list);
	dialog_run(dlg);
}

/* ---------------------------------------------------------------- */
/* Mouse                                                             */
/* ---------------------------------------------------------------- */

#define ID_SLOW		100
#define ID_MEDIUM	101
#define ID_FAST		102
#define ID_SWAP		103

static const int speeds[3] = { 45, 30, 18 };	/* frames */

typedef struct {
	int lit;			/* the test box */
	int old_dbl;
} MouseDlg;

static int mouse_proc(Win *w, int msg, int a, int b)
{
	MouseDlg *d = w->data;
	int i;

	switch (msg) {
	case M_PAINT:
		text(8, 4, "Double Click Speed:", C_BLACK, F_BOLD);
		frame(130, 16, 176, 46, C_BLACK);
		fill(131, 17, 175, 45, d->lit ? C_NAVY : C_WHITE);
		text(140, 26, "TEST", d->lit ? C_WHITE : C_BLACK, F_BOLD);
		text(8, 76, "(The C button double clicks too.)", C_GRAY, F_NORMAL);
		return 1;
	case M_LDBL:
		/* double clicking the test box, at the speed being chosen */
		if (a >= 130 && a < 176 && b >= 16 && b < 46) {
			d->lit = !d->lit;
			invalidate_client(w, 130, 16, 176, 46);
		}
		return 1;
	case M_COMMAND:
		if (a >= ID_SLOW && a <= ID_FAST) {
			dbl_time = speeds[a - ID_SLOW];
			return 1;
		}
		if (a == ID_CANCEL)
			dbl_time = d->old_dbl;
		if (a == ID_OK) {
			swap_buttons = check_get(ctl_find(w, ID_SWAP));
			for (i = 0; i < 3; i++)
				if (check_get(ctl_find(w, ID_SLOW + i)))
					dbl_time = speeds[i];
		}
		break;
	}
	return def_proc(w, msg, a, b);
}

static void mouse_dialog(Win *owner)
{
	MouseDlg d;
	Win *dlg;
	int i, best = 1;

	d.lit = 0;
	d.old_dbl = dbl_time;
	for (i = 0; i < 3; i++)
		if (abs(speeds[i] - dbl_time) < abs(speeds[best] - dbl_time))
			best = i;
	dlg = dialog_create(owner, 184, 110, "Mouse", mouse_proc, &d);
	ctl_check(dlg, 12, 16, 100, 11, "&Slow", ID_SLOW, 1);
	ctl_check(dlg, 12, 28, 100, 11, "&Medium", ID_MEDIUM, 1);
	ctl_check(dlg, 12, 40, 100, 11, "&Fast", ID_FAST, 1);
	check_set(ctl_find(dlg, ID_SLOW + best), 1);
	ctl_check(dlg, 8, 58, 170, 11, "S&wap A and B buttons", ID_SWAP, 0);
	check_set(ctl_find(dlg, ID_SWAP), swap_buttons);
	ctl_button(dlg, 40, 92, 44, 14, "OK", ID_OK, 1);
	ctl_button(dlg, 92, 92, 44, 14, "Cancel", ID_CANCEL, 0);
	win_set_focus(ctl_find(dlg, ID_SLOW + best));
	dialog_run(dlg);
}

/* ---------------------------------------------------------------- */
/* Date and time                                                     */
/* ---------------------------------------------------------------- */

#define ID_DATE		100
#define ID_TIME		101

static int date_proc(Win *w, int msg, int a, int b)
{
	struct tm tm;
	struct timeval tv;
	int mo, dd, yy, hh, mi, ss = 0;
	time_t t;

	switch (msg) {
	case M_PAINT:
		text(8, 6, "Date (month/day/year):", C_BLACK, F_BOLD);
		text(8, 34, "Time (24 hour):", C_BLACK, F_BOLD);
		return 1;
	case M_COMMAND:
		if (a != ID_OK)
			break;
		if (sscanf(edit_text(ctl_find(w, ID_DATE)), "%d/%d/%d", &mo, &dd, &yy) != 3 ||
		    mo < 1 || mo > 12 || dd < 1 || dd > 31) {
			msgbox(w, "Date & Time", "Type the date as month/day/year,\nfor example 6/24/1994.",
			       MB_OK | MB_EXCLAIM);
			return 1;
		}
		if (sscanf(edit_text(ctl_find(w, ID_TIME)), "%d:%d:%d", &hh, &mi, &ss) < 2 ||
		    hh < 0 || hh > 23 || mi < 0 || mi > 59 || ss < 0 || ss > 59) {
			msgbox(w, "Date & Time", "Type the time as hours:minutes,\nfor example 13:45.",
			       MB_OK | MB_EXCLAIM);
			return 1;
		}
		if (yy < 70)
			yy += 2000;
		else if (yy < 100)
			yy += 1900;
		memset(&tm, 0, sizeof tm);
		tm.tm_year = yy - 1900;
		tm.tm_mon = mo - 1;
		tm.tm_mday = dd;
		tm.tm_hour = hh;
		tm.tm_min = mi;
		tm.tm_sec = ss;
		tm.tm_isdst = -1;
		t = mktime(&tm);
		tv.tv_sec = t;
		tv.tv_usec = 0;
		if (t == (time_t)-1 || settimeofday(&tv, 0) < 0) {
			msgbox(w, "Date & Time", "Cannot set the clock.", MB_OK | MB_STOP);
			return 1;
		}
		invalidate(root);	/* clocks */
		dialog_end(w, ID_OK);
		return 1;
	}
	return def_proc(w, msg, a, b);
}

static void date_dialog(Win *owner)
{
	char buf[40];
	time_t t = time(0);
	struct tm *tm = localtime(&t);
	Win *dlg, *e;

	dlg = dialog_create(owner, 150, 82, "Date & Time", date_proc, 0);
	sprintf(buf, "%d/%d/%d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year + 1900);
	e = ctl_edit(dlg, 8, 16, 100, 13, buf, ID_DATE);
	sprintf(buf, "%d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
	ctl_edit(dlg, 8, 44, 100, 13, buf, ID_TIME);
	ctl_button(dlg, 22, 64, 44, 14, "OK", ID_OK, 1);
	ctl_button(dlg, 74, 64, 44, 14, "Cancel", ID_CANCEL, 0);
	win_set_focus(e);
	dialog_run(dlg);
}

/* ---------------------------------------------------------------- */
/* The Control Panel window                                          */
/* ---------------------------------------------------------------- */

#define CELL_W	52
#define CELL_H	36

typedef struct {
	const char *title;
	const unsigned char *icon;
	void (*open)(Win *owner);
	const char *help;
} Applet;

static const Applet applets[] = {
	{ "Color", icon_cp_color, color_dialog, "Changes the screen colors" },
	{ "Mouse", icon_cp_mouse, mouse_dialog, "Double click speed, buttons" },
	{ "Date/Time", icon_cp_date, date_dialog, "Sets the date and time" },
};

typedef struct {
	int sel;
} Panel;

enum { CM_EXIT = 1, CM_ABOUT, CM_APPLET = 10 };

static MenuItem settings_items[] = {
	{ "&Color...", CM_APPLET + 0, 0 },
	{ "&Mouse...", CM_APPLET + 1, 0 },
	{ "&Date/Time...", CM_APPLET + 2, 0 },
	{ 0, 0, 0 },
	{ "E&xit", CM_EXIT, 0 },
};
static MenuItem help_items[] = {
	{ "&About Control Panel...", CM_ABOUT, 0 },
};
static Menu cp_menus[] = {
	{ "&Settings", settings_items, COUNT(settings_items) },
	{ "&Help", help_items, COUNT(help_items) },
};
static MenuBar cp_bar = { cp_menus, COUNT(cp_menus) };

static int panel_proc(Win *w, int msg, int a, int b)
{
	Panel *p = w->data;
	Rect c;
	int i;

	switch (msg) {
	case M_PAINT:
		win_client(w, &c);
		for (i = 0; i < COUNT(applets); i++)
			draw_icon_label(i * CELL_W, 3, CELL_W, applets[i].icon, applets[i].title,
					i == p->sel && active == w, C_BLACK);
		hline(0, c.r, c.b - 11, C_BLACK);
		fill(0, c.b - 10, c.r, c.b, C_SILVER);
		text(3, c.b - 9, applets[p->sel].help, C_BLACK, F_NORMAL);
		return 1;
	case M_LDOWN:
	case M_LDBL:
		i = a / CELL_W;
		if (b < 3 || b >= 3 + CELL_H || i >= COUNT(applets))
			return 1;
		p->sel = i;
		invalidate(w);
		if (msg == M_LDBL) {
			repaint_now();
			applets[i].open(w);
		}
		return 1;
	case M_KEY:
		if (a == K_LEFT && p->sel > 0) p->sel--;
		else if (a == K_RIGHT && p->sel < COUNT(applets) - 1) p->sel++;
		else if (a == K_ENTER) applets[p->sel].open(w);
		else return 0;
		invalidate(w);
		return 1;
	case M_ACTIVATE:
		invalidate(w);
		return 1;
	case M_COMMAND:
		if (a >= CM_APPLET && a < CM_APPLET + COUNT(applets)) {
			applets[a - CM_APPLET].open(w);
			return 1;
		}
		if (a == CM_EXIT) {
			send(w, M_CLOSE, 0, 0);
			return 1;
		}
		if (a == CM_ABOUT) {
			msgbox(w, "About Control Panel", "Control Panel\n\nSettings last until you\nleave win.",
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

void app_control(void)
{
	Win *w;
	Panel *p;

	for (w = root->child; w; w = w->next)
		if (w->proc == panel_proc && !w->dead) {
			if (w->state == W_MIN)
				win_set_state(w, W_NORMAL);
			win_activate(w);
			return;
		}
	p = calloc(1, sizeof *p);
	if (!p)
		return;
	w = win_create(root, 70, 50, COUNT(applets) * CELL_W + 10, CELL_H + 46,
		       S_CAPTION | S_SYSMENU | S_MINBOX | S_BORDER, "Control Panel", panel_proc, p);
	w->menu = &cp_bar;
	w->icon = icon_control;
}

/* ---------------------------------------------------------------- */
/* Read Me                                                           */
/* ---------------------------------------------------------------- */

static const char readme[] =
	"WIN: A GRAPHICAL SHELL FOR SEGA LINUX\n"
	"=====================================\n"
	"\n"
	"THE CONTROLLER (port 1)\n"
	"  D-pad   move the pointer (hold to speed up)\n"
	"  A       left click (hold it to drag)\n"
	"  B       right click\n"
	"  C       double click\n"
	"  Start   the Task List (so does double\n"
	"          clicking the desktop)\n"
	"\n"
	"THE KEYBOARD (port 2)\n"
	"  Alt+Tab     switch to the next window\n"
	"  Alt+F4      close the window\n"
	"  Alt+Space   the window's Control menu\n"
	"  Alt+letter  open a menu (F10: the first)\n"
	"  Ctrl+F4     close a group or document\n"
	"  Tab, Enter and Esc work in dialog boxes.\n"
	"\n"
	"PROGRAMS\n"
	"  File Manager  look through the files.\n"
	"                Double click music (.vgm,\n"
	"                .vgz) to play it.\n"
	"  Notepad       write text files.\n"
	"  Paint         draw 16 color pictures.\n"
	"  Calculator, Clock and Puzzle.\n"
	"\n"
	"FILES\n"
	"  Most of the disk is in ROM and cannot be\n"
	"  changed: save your work in /tmp. It is a\n"
	"  RAM disk, so it is lost when the power is\n"
	"  switched off.\n"
	"\n"
	"THE CLOCK\n"
	"  The Genesis has no battery clock, so it\n"
	"  starts at 1970 each time. Set it in\n"
	"  Control Panel, Date/Time.\n"
	"\n"
	"LEAVING\n"
	"  File, Exit in Program Manager goes back\n"
	"  to the shell prompt.\n";

void app_readme(void)
{
	app_notepad_text("Read Me", readme);
}
