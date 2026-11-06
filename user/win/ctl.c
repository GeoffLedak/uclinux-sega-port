/*
 * ctl.c -- controls, message boxes and the common dialogs
 *
 * Controls are child windows (S_CHILD) that tell their parent about
 * clicks and changes with M_COMMAND (a = their id, b = N_ code).
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "win.h"

#define UI_FONT		F_BOLD

static void notify(Win *w, int code)
{
	send(w->parent, M_COMMAND, w->id, code);
}

static int has_focus(Win *w)
{
	return win_focus() == w;
}

/* ---------------------------------------------------------------- */
/* Push buttons                                                      */
/* ---------------------------------------------------------------- */

#define PRESSED(w)	((w)->hpos)		/* (scroll fields are free) */

static int button_proc(Win *w, int msg, int a, int b)
{
	Rect c;
	int W, H, tw, x, y, d, col;

	switch (msg) {
	case M_PAINT:
		win_client(w, &c);
		W = c.r;
		H = c.b;
		d = (w->style & S_DEFAULT) ? 1 : 0;
		/* black border with the corners cut */
		hline(1, W - 1, 0, C_BLACK);
		hline(1, W - 1, H - 1, C_BLACK);
		vline(0, 1, H - 1, C_BLACK);
		vline(W - 1, 1, H - 1, C_BLACK);
		if (d)
			frame(1, 1, W - 1, H - 1, C_BLACK);
		fill(1 + d, 1 + d, W - 1 - d, H - 1 - d, C_SILVER);
		if (PRESSED(w)) {
			hline(1 + d, W - 1 - d, 1 + d, C_GRAY);
			vline(1 + d, 1 + d, H - 1 - d, C_GRAY);
		} else {
			hline(1 + d, W - 2 - d, 1 + d, C_WHITE);
			vline(1 + d, 1 + d, H - 2 - d, C_WHITE);
			hline(1 + d, W - 1 - d, H - 2 - d, C_GRAY);
			vline(W - 2 - d, 1 + d, H - 1 - d, C_GRAY);
			if (W > 30) {
				hline(2 + d, W - 1 - d, H - 3 - d, C_GRAY);
				vline(W - 3 - d, 2 + d, H - 1 - d, C_GRAY);
			}
		}
		tw = text_mn_w(w->text, UI_FONT);
		x = (W - tw) / 2 + (PRESSED(w) ? 1 : 0);
		y = (H - FONT_H) / 2 + (PRESSED(w) ? 1 : 0);
		col = (w->style & S_DISABLED) ? C_GRAY : w->fg;
		text_mn(x, y, w->text, col, UI_FONT);
		if (has_focus(w))
			dotted_frame(x - 2, y - 1, x + tw + 2, y + FONT_H + 1);
		return 1;
	case M_LDOWN:
	case M_LDBL:
		PRESSED(w) = 1;
		invalidate(w);
		return 1;
	case M_MOVE:
		win_client(w, &c);
		if (PRESSED(w) != in_rect(&c, a, b)) {
			PRESSED(w) = !PRESSED(w);
			invalidate(w);
		}
		return 1;
	case M_LUP:
		if (PRESSED(w)) {
			PRESSED(w) = 0;
			invalidate(w);
			notify(w, N_CLICKED);
		}
		return 1;
	case M_CLICK:
		PRESSED(w) = 1;
		invalidate(w);
		repaint_now();
		hw_flush();
		hw_wait(6);
		PRESSED(w) = 0;
		invalidate(w);
		notify(w, N_CLICKED);
		return 1;
	case M_KEY:
		if (a == ' ') {
			send(w, M_CLICK, 0, 0);
			return 1;
		}
		return 0;
	case M_FOCUS:
		invalidate(w);
		return 1;
	}
	return def_proc(w, msg, a, b);
}

Win *ctl_button(Win *p, int x, int y, int wd, int ht, const char *text, int id, int def)
{
	Win *w = win_create(p, x, y, wd, ht, S_CHILD | S_TABSTOP | (def ? S_DEFAULT : 0),
			    text, button_proc, 0);
	w->id = id;
	return w;
}

/* ---------------------------------------------------------------- */
/* Labels and icons                                                  */
/* ---------------------------------------------------------------- */

static int label_proc(Win *w, int msg, int a, int b)
{
	Rect c;
	const char *s, *e, *sp;
	int y = 0;

	if (msg == M_DESTROY) {
		free(w->data);
		return 1;
	}
	if (msg != M_PAINT)
		return def_proc(w, msg, a, b);
	win_client(w, &c);
	/* word wrap at the width, and at newlines */
	for (s = w->data ? (const char *)w->data : w->text; *s; ) {
		e = s;
		sp = 0;
		while (*e && *e != '\n') {
			if (*e == ' ')
				sp = e;
			if (textn_w(s, e - s + 1, UI_FONT) > c.r && sp)
				break;
			e++;
		}
		if (*e && *e != '\n' && sp)
			e = sp;
		textn(0, y, s, e - s, C_BLACK, UI_FONT);
		y += LINE_H;
		s = (*e == '\n' || *e == ' ') ? e + 1 : e;
	}
	return 1;
}

Win *ctl_label(Win *p, int x, int y, int wd, int ht, const char *text)
{
	Win *w = win_create(p, x, y, wd, ht, S_CHILD, text, label_proc, 0);

	if (strlen(text) >= sizeof w->text) {
		w->data = malloc(strlen(text) + 1);
		if (w->data)
			strcpy(w->data, text);
	}
	return w;
}

static int icon_proc(Win *w, int msg, int a, int b)
{
	if (msg == M_PAINT) {
		draw_icon(0, 0, w->icon, 16);
		return 1;
	}
	return def_proc(w, msg, a, b);
}

Win *ctl_icon(Win *p, int x, int y, const unsigned char *icon)
{
	Win *w = win_create(p, x, y, 16, 16, S_CHILD, 0, icon_proc, 0);

	w->icon = icon;
	return w;
}

/* ---------------------------------------------------------------- */
/* Single-line edit fields                                           */
/* ---------------------------------------------------------------- */

typedef struct {
	char buf[256];
	int len, cur, scroll;
	char caret, all;		/* caret shown; all the text selected */
} Edit;

static void edit_fix_scroll(Win *w, Edit *e)
{
	Rect c;
	int x;

	win_client(w, &c);
	if (e->cur < e->scroll)
		e->scroll = e->cur;
	for (;;) {
		x = textn_w(e->buf + e->scroll, e->cur - e->scroll, UI_FONT);
		if (x <= c.r - 5 || e->scroll >= e->cur)
			break;
		e->scroll++;
	}
}

static int edit_proc(Win *w, int msg, int a, int b)
{
	Edit *e = w->data;
	Rect c;
	int x, i;

	switch (msg) {
	case M_CREATE:
		w->data = e = calloc(1, sizeof(Edit));
		return 1;
	case M_DESTROY:
		free(e);
		return 1;
	case M_PAINT:
		win_client(w, &c);
		fill(0, 0, c.r, c.b, C_WHITE);
		x = 2;
		if (e->all && has_focus(w) && e->len) {
			int tw = textn_w(e->buf + e->scroll, e->len - e->scroll, UI_FONT);
			fill(x - 1, 1, x + tw + 1, c.b - 1, col_hilite);
			textn(x, (c.b - FONT_H) / 2, e->buf + e->scroll, e->len - e->scroll,
			      C_WHITE, UI_FONT);
		} else
			textn(x, (c.b - FONT_H) / 2, e->buf + e->scroll, e->len - e->scroll,
			      C_BLACK, UI_FONT);
		if (has_focus(w) && e->caret && !e->all) {
			x += textn_w(e->buf + e->scroll, e->cur - e->scroll, UI_FONT);
			vline(x - 1, 1, c.b - 1, C_BLACK);
		}
		return 1;
	case M_FOCUS:
		e->caret = 1;
		e->all = a ? 1 : 0;
		set_timer(w, a ? 30 : 0);
		invalidate(w);
		return 1;
	case M_TIMER:
		e->caret = !e->caret;
		invalidate(w);
		return 1;
	case M_LDOWN:
	case M_LDBL:
		e->all = (msg == M_LDBL);
		for (i = e->scroll; i < e->len; i++)
			if (2 + textn_w(e->buf + e->scroll, i - e->scroll + 1, UI_FONT)
			    - char_w((unsigned char)e->buf[i], UI_FONT) / 2 > a)
				break;
		e->cur = i;
		e->caret = 1;
		invalidate(w);
		return 1;
	case M_CURSOR:
		return PTR_IBEAM;
	case M_KEY:
		if (e->all && ((a >= 32 && a < 127) || a == K_BS || a == K_DEL)) {
			e->len = e->cur = e->scroll = 0;
			e->buf[0] = 0;
			e->all = 0;
			if (a == K_BS || a == K_DEL) {
				invalidate(w);
				notify(w, N_CHANGE);
				return 1;
			}
		}
		e->all = 0;
		if (a >= 32 && a < 127) {
			if (e->len >= (int)sizeof e->buf - 1) {
				beep();
				return 1;
			}
			memmove(e->buf + e->cur + 1, e->buf + e->cur, e->len - e->cur + 1);
			e->buf[e->cur++] = a;
			e->len++;
		} else if (a == K_BS) {
			if (e->cur > 0) {
				memmove(e->buf + e->cur - 1, e->buf + e->cur, e->len - e->cur + 1);
				e->cur--;
				e->len--;
			}
		} else if (a == K_DEL) {
			if (e->cur < e->len) {
				memmove(e->buf + e->cur, e->buf + e->cur + 1, e->len - e->cur);
				e->len--;
			}
		} else if (a == K_LEFT) {
			if (e->cur > 0) e->cur--;
		} else if (a == K_RIGHT) {
			if (e->cur < e->len) e->cur++;
		} else if (a == K_HOME) {
			e->cur = 0;
		} else if (a == K_END) {
			e->cur = e->len;
		} else
			return 0;
		e->caret = 1;
		w->timer_left = w->timer;
		edit_fix_scroll(w, e);
		invalidate(w);
		if (a >= 32 || a == K_BS || a == K_DEL)
			notify(w, N_CHANGE);
		return 1;
	}
	return def_proc(w, msg, a, b);
}

Win *ctl_edit(Win *p, int x, int y, int wd, int ht, const char *text, int id)
{
	Win *w = win_create(p, x, y, wd, ht, S_CHILD | S_TABSTOP | S_BORDER, 0, edit_proc, 0);

	w->id = id;
	edit_set(w, text);
	return w;
}

const char *edit_text(Win *w)
{
	return ((Edit *)w->data)->buf;
}

void edit_set(Win *w, const char *s)
{
	Edit *e = w->data;

	strncpy(e->buf, s ? s : "", sizeof e->buf - 1);
	e->buf[sizeof e->buf - 1] = 0;
	e->len = e->cur = strlen(e->buf);
	e->scroll = 0;
	edit_fix_scroll(w, e);
	invalidate(w);
}

/* ---------------------------------------------------------------- */
/* List boxes                                                        */
/* ---------------------------------------------------------------- */

typedef struct {
	char **items;
	const unsigned char **icons;
	int n, cap, sel, top;
} List;

#define ITEM_H 10

static int list_rows(Win *w)
{
	Rect c;

	win_client(w, &c);
	return c.b / ITEM_H;
}

static void list_update_scroll(Win *w)
{
	List *l = w->data;
	int rows = list_rows(w);

	if (l->top > l->n - rows)
		l->top = l->n - rows;
	if (l->top < 0)
		l->top = 0;
	set_scroll(w, 1, l->top, l->n - rows, rows);
}

static void list_show_sel(Win *w)
{
	List *l = w->data;
	int rows = list_rows(w);

	if (l->sel < 0)
		return;
	if (l->sel < l->top)
		l->top = l->sel;
	if (l->sel >= l->top + rows)
		l->top = l->sel - rows + 1;
	list_update_scroll(w);
}

/* redraw item i */
static void list_inval_item(Win *w, int i)
{
	List *l = w->data;
	Rect c, r;

	if (i < l->top)
		return;
	win_client_screen(w, &c);
	set_rect(&r, c.l, c.t + (i - l->top) * ITEM_H, c.r, c.t + (i - l->top + 1) * ITEM_H);
	if (rect_and(&r, &c))
		invalidate_screen(&r);
}

/* after top and sel changed from otop and osel: move what is still in
 * view, redraw what's new and the selection */
static void list_changed(Win *w, int otop, int osel)
{
	List *l = w->data;
	int d = l->top - otop, rows = list_rows(w) + 1;

	if (d >= rows || d <= -rows) {
		invalidate(w);
		return;
	}
	if (d)
		scroll_client(w, 0, -d * ITEM_H);
	if (osel != l->sel) {
		list_inval_item(w, osel);
		list_inval_item(w, l->sel);
	}
}

static int list_proc(Win *w, int msg, int a, int b)
{
	List *l = w->data;
	Rect c;
	int i, y, rows, old, otop;

	switch (msg) {
	case M_CREATE:
		w->data = l = calloc(1, sizeof(List));
		l->sel = -1;
		return 1;
	case M_DESTROY:
		list_clear(w);
		free(l->items);
		free(l->icons);
		free(l);
		return 1;
	case M_PAINT:
		win_client(w, &c);
		fill(0, 0, c.r, c.b, C_WHITE);
		for (i = l->top, y = 0; i < l->n && y < c.b; i++, y += ITEM_H) {
			int x = 2, col = C_BLACK;
			if (i == l->sel) {
				fill(0, y, c.r, y + ITEM_H, col_hilite);
				col = C_WHITE;
			}
			if (l->icons[i]) {
				draw_icon(x, y + 1, l->icons[i], 8);
				x += 11;
			}
			text(x, y + 1, l->items[i], col, UI_FONT);
			if (i == l->sel && has_focus(w))
				dotted_frame(0, y, c.r, y + ITEM_H);
		}
		return 1;
	case M_LDOWN:
	case M_LDBL:
	case M_MOVE:
		if (msg == M_MOVE && b >= 0 && b < list_rows(w) * ITEM_H)
			;
		else if (msg == M_MOVE)
			return 1;
		i = l->top + b / ITEM_H;
		if (b < 0 || i >= l->n)
			return 1;
		old = l->sel;
		l->sel = i;
		if (old != i) {
			list_changed(w, l->top, old);
			notify(w, N_SELCHANGE);
		}
		if (msg == M_LDBL)
			notify(w, N_DBLCLK);
		return 1;
	case M_KEY:
		rows = list_rows(w);
		old = l->sel;
		otop = l->top;
		switch (a) {
		case K_UP: l->sel--; break;
		case K_DOWN: l->sel++; break;
		case K_PGUP: l->sel -= rows - 1; break;
		case K_PGDN: l->sel += rows - 1; break;
		case K_HOME: l->sel = 0; break;
		case K_END: l->sel = l->n - 1; break;
		default:
			if (a > ' ' && a < 127) {
				/* the next item starting with that letter */
				int k, ch = a | 32;
				for (k = 1; k <= l->n; k++) {
					i = (old + k) % l->n;
					if (i >= 0 && (l->items[i][0] | 32) == ch) {
						l->sel = i;
						break;
					}
				}
				if (k > l->n)
					return 1;
				break;
			}
			return 0;
		}
		if (l->sel >= l->n) l->sel = l->n - 1;
		if (l->sel < 0) l->sel = l->n ? 0 : -1;
		list_show_sel(w);
		list_changed(w, otop, old);
		if (l->sel != old)
			notify(w, N_SELCHANGE);
		return 1;
	case M_VSCROLL:
		rows = list_rows(w);
		otop = l->top;
		switch (a) {
		case SB_LINEUP: l->top--; break;
		case SB_LINEDOWN: l->top++; break;
		case SB_PAGEUP: l->top -= rows; break;
		case SB_PAGEDOWN: l->top += rows; break;
		case SB_THUMB: l->top = b; break;
		}
		list_update_scroll(w);
		list_changed(w, otop, l->sel);
		return 1;
	case M_SIZE:
		list_update_scroll(w);
		return 1;
	case M_FOCUS:
		list_inval_item(w, l->sel);	/* the dotted frame */
		return 1;
	}
	return def_proc(w, msg, a, b);
}

Win *ctl_list(Win *p, int x, int y, int wd, int ht, int id)
{
	Win *w = win_create(p, x, y, wd, ht, S_CHILD | S_TABSTOP | S_BORDER | S_VSCROLL,
			    0, list_proc, 0);
	w->id = id;
	return w;
}

void list_clear(Win *w)
{
	List *l = w->data;
	int i;

	for (i = 0; i < l->n; i++)
		free(l->items[i]);
	l->n = 0;
	l->sel = -1;
	l->top = 0;
	list_update_scroll(w);
	invalidate(w);
}

void list_add(Win *w, const char *s, const unsigned char *icon)
{
	List *l = w->data;

	if (l->n == l->cap) {
		int cap = l->cap ? l->cap * 2 : 16;
		char **ni = realloc(l->items, cap * sizeof *ni);
		const unsigned char **nc = realloc(l->icons, cap * sizeof *nc);
		if (ni) l->items = ni;
		if (nc) l->icons = nc;
		if (!ni || !nc)
			return;
		l->cap = cap;
	}
	l->items[l->n] = malloc(strlen(s) + 1);
	if (!l->items[l->n])
		return;
	strcpy(l->items[l->n], s);
	l->icons[l->n] = icon;
	l->n++;
	list_update_scroll(w);
	invalidate(w);
}

int list_sel(Win *w) { return ((List *)w->data)->sel; }
int list_count(Win *w) { return ((List *)w->data)->n; }

const char *list_item(Win *w, int i)
{
	List *l = w->data;

	return (i >= 0 && i < l->n) ? l->items[i] : "";
}

void list_set_sel(Win *w, int i)
{
	List *l = w->data;
	int otop = l->top, osel = l->sel;

	l->sel = i;
	list_show_sel(w);
	list_changed(w, otop, osel);
}

/* ---------------------------------------------------------------- */
/* Check boxes and option buttons                                    */
/* ---------------------------------------------------------------- */

#define CHECKED(w)	((w)->vpos)
#define RADIO(w)	((w)->vmax)

static int check_proc(Win *w, int msg, int a, int b)
{
	Rect c;
	int y;

	switch (msg) {
	case M_PAINT:
		win_client(w, &c);
		y = (c.b - 9) / 2;
		if (RADIO(w)) {
			/* a small circle */
			hline(3, 6, y, C_BLACK);
			hline(3, 6, y + 8, C_BLACK);
			vline(0, y + 3, y + 6, C_BLACK);
			vline(8, y + 3, y + 6, C_BLACK);
			pixel(1, y + 2, C_BLACK); pixel(2, y + 1, C_BLACK);
			pixel(6, y + 1, C_BLACK); pixel(7, y + 2, C_BLACK);
			pixel(1, y + 6, C_BLACK); pixel(2, y + 7, C_BLACK);
			pixel(6, y + 7, C_BLACK); pixel(7, y + 6, C_BLACK);
			fill(2, y + 2, 7, y + 7, C_WHITE);
			fill(3, y + 1, 6, y + 8, C_WHITE);
			fill(1, y + 3, 8, y + 6, C_WHITE);
			if (CHECKED(w)) {
				fill(3, y + 3, 6, y + 6, C_BLACK);
				hline(4, 5, y + 2, C_BLACK);
				hline(4, 5, y + 6, C_BLACK);
				vline(2, y + 4, y + 5, C_BLACK);
				vline(6, y + 4, y + 5, C_BLACK);
			}
		} else {
			frame(0, y, 9, y + 9, C_BLACK);
			fill(1, y + 1, 8, y + 8, C_WHITE);
			if (CHECKED(w)) {
				line(1, y + 1, 7, y + 7, C_BLACK);
				line(7, y + 1, 1, y + 7, C_BLACK);
			}
		}
		text_mn(12, (c.b - FONT_H) / 2, w->text, C_BLACK, UI_FONT);
		if (has_focus(w))
			dotted_frame(10, (c.b - FONT_H) / 2 - 1,
				     14 + text_mn_w(w->text, UI_FONT), (c.b + FONT_H) / 2 + 1);
		return 1;
	case M_LDOWN:
	case M_LDBL:
	case M_CLICK:
		if (RADIO(w)) {
			/* option buttons in a window turn each other off */
			Win *o;
			for (o = w->parent->child; o; o = o->next)
				if (o != w && o->proc == check_proc && RADIO(o) && CHECKED(o)) {
					CHECKED(o) = 0;
					invalidate(o);
				}
			CHECKED(w) = 1;
		} else
			CHECKED(w) = !CHECKED(w);
		invalidate(w);
		notify(w, N_CLICKED);
		return msg != M_CLICK || a == 0;
	case M_KEY:
		if (a == ' ') {
			send(w, M_CLICK, 0, 0);
			return 1;
		}
		return 0;
	case M_FOCUS:
		invalidate(w);
		return 1;
	}
	return def_proc(w, msg, a, b);
}

Win *ctl_check(Win *p, int x, int y, int wd, int ht, const char *text, int id, int radio)
{
	Win *w = win_create(p, x, y, wd, ht, S_CHILD | S_TABSTOP, text, check_proc, 0);

	w->id = id;
	RADIO(w) = radio;
	return w;
}

int check_get(Win *w) { return CHECKED(w); }

void check_set(Win *w, int on)
{
	CHECKED(w) = on;
	invalidate(w);
}

Win *ctl_find(Win *dlg, int id)
{
	Win *c;

	for (c = dlg->child; c; c = c->next)
		if (c->id == id)
			return c;
	return 0;
}

/* ---------------------------------------------------------------- */
/* Dialogs                                                           */
/* ---------------------------------------------------------------- */

/* w x h is the client size */
Win *dialog_create(Win *owner, int wd, int ht, const char *title, Proc proc, void *data)
{
	Win *d;

	wd += 2 * 4;
	ht += 2 * 4 + 11;
	d = win_create(root, 0, 0, wd, ht,
		       S_CAPTION | S_SYSMENU | S_DLGFRAME | S_DIALOG | S_HIDDEN,
		       title, proc, data);
	d->owner = owner;
	center_window(d, owner ? win_top(owner) : 0);
	win_show(d, 1);
	return d;
}

/* focus the first control that takes it */
static int msgbox_proc(Win *w, int msg, int a, int b)
{
	if (msg == M_COMMAND && (a == ID_OK || a == ID_CANCEL || a == ID_YES || a == ID_NO)) {
		if (a == ID_CANCEL && !ctl_find(w, ID_CANCEL)) {
			/* Esc or Close without a Cancel button */
			if (ctl_find(w, ID_OK))
				a = ID_OK;
			else
				return 1;
		}
		dialog_end(w, a);
		return 1;
	}
	return def_proc(w, msg, a, b);
}

int msgbox(Win *owner, const char *title, const char *text, int flags)
{
	static const char *labels[] = { "OK", "Cancel", "&Yes", "&No" };
	static const int ids[] = { ID_OK, ID_CANCEL, ID_YES, ID_NO };
	int set[4], n = 0, i, tw = 0, lines = 1, w, h, x, bw = 44, tx;
	const unsigned char *icon = 0;
	const char *s;
	Win *d, *first = 0;

	switch (flags & 0x0F) {
	case MB_OK: set[n++] = 0; break;
	case MB_OKCANCEL: set[n++] = 0; set[n++] = 1; break;
	case MB_YESNO: set[n++] = 2; set[n++] = 3; break;
	case MB_YESNOCANCEL: set[n++] = 2; set[n++] = 3; set[n++] = 1; break;
	}
	switch (flags & 0xF0) {
	case MB_STOP: icon = icon_mb_stop; break;
	case MB_QUESTION: icon = icon_mb_question; break;
	case MB_EXCLAIM: icon = icon_mb_excl; break;
	case MB_INFO: icon = icon_mb_info; break;
	}
	/* size of the text (lines wider than 200 pixels wrap) */
	{
		const char *ls = text;
		for (s = text; ; s++)
			if (!*s || *s == '\n') {
				int lw = textn_w(ls, s - ls, F_BOLD);
				if (lw > 200) {
					lines += lw / 200;
					lw = 200;
				}
				if (lw > tw)
					tw = lw;
				if (!*s)
					break;
				lines++;
				ls = s + 1;
			}
	}
	tx = icon ? 32 : 12;
	w = tx + tw + 12;
	if (w < n * (bw + 8) + 16)
		w = n * (bw + 8) + 16;
	h = 10 + (lines * LINE_H > 20 ? lines * LINE_H : 20) + 10 + 14 + 8;
	d = dialog_create(owner, w, h, title, msgbox_proc, 0);
	if (icon)
		ctl_icon(d, 10, 10, icon);
	ctl_label(d, tx, 10 + (lines == 1 && icon ? 4 : 0), w - tx - 8, lines * LINE_H, text);
	x = (w - (n * bw + (n - 1) * 8)) / 2;
	for (i = 0; i < n; i++) {
		Win *b = ctl_button(d, x, h - 22, bw, 14, labels[set[i]], ids[set[i]], i == 0);
		if (!first)
			first = b;
		x += bw + 8;
	}
	win_set_focus(first);
	if ((flags & 0xF0) == MB_STOP || (flags & 0xF0) == MB_EXCLAIM)
		beep();
	return dialog_run(d);
}

/* ---------------------------------------------------------------- */
/* Input box                                                         */
/* ---------------------------------------------------------------- */

typedef struct {
	char *buf;
	int max;
	Win *edit;
} InputDlg;

static int input_proc(Win *w, int msg, int a, int b)
{
	InputDlg *in = w->data;

	if (msg == M_COMMAND && a == ID_OK) {
		strncpy(in->buf, edit_text(in->edit), in->max - 1);
		in->buf[in->max - 1] = 0;
		dialog_end(w, ID_OK);
		return 1;
	}
	return def_proc(w, msg, a, b);
}

int input_box(Win *owner, const char *title, const char *prompt, char *buf, int max)
{
	InputDlg in;
	Win *d;

	in.buf = buf;
	in.max = max;
	d = dialog_create(owner, 200, 62, title, input_proc, &in);
	ctl_label(d, 8, 6, 184, 10, prompt);
	in.edit = ctl_edit(d, 8, 18, 184, 13, buf, 100);
	ctl_button(d, 52, 40, 44, 14, "OK", ID_OK, 1);
	ctl_button(d, 104, 40, 44, 14, "Cancel", ID_CANCEL, 0);
	win_set_focus(in.edit);
	return dialog_run(d) == ID_OK;
}

/* ---------------------------------------------------------------- */
/* Open / Save As                                                    */
/* ---------------------------------------------------------------- */

typedef struct {
	char dir[256];
	char *path;
	int save;
	Win *name, *files, *dirs, *dirlabel;
} FileDlg;

#define ID_NAME		100
#define ID_FILES	101
#define ID_DIRS		102

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char * const *)a, *(char * const *)b);
}

static void join(char *out, const char *dir, const char *name)
{
	if (name[0] == '/')
		strcpy(out, name);
	else if (!strcmp(dir, "/"))
		sprintf(out, "/%s", name);
	else
		sprintf(out, "%s/%s", dir, name);
}

static void fd_fill(FileDlg *f)
{
	DIR *d;
	struct dirent *de;
	struct stat st;
	char path[300], *names[200];
	int dirflag[200], n = 0, i;

	list_clear(f->files);
	list_clear(f->dirs);
	if (strcmp(f->dir, "/"))
		list_add(f->dirs, "..", small_up);
	d = opendir(f->dir);
	if (d) {
		while ((de = readdir(d)) != 0 && n < 200) {
			if (de->d_name[0] == '.')
				continue;
			join(path, f->dir, de->d_name);
			if (stat(path, &st) < 0)
				continue;
			names[n] = malloc(strlen(de->d_name) + 1);
			if (!names[n])
				break;
			strcpy(names[n], de->d_name);
			dirflag[n] = S_ISDIR(st.st_mode);
			n++;
		}
		closedir(d);
	}
	/* sort, keeping the dir flags with the names */
	for (i = 0; i < n; i++) {
		char *t = malloc(strlen(names[i]) + 2);
		if (!t) { n = i; break; }
		t[0] = dirflag[i] ? 'd' : 'f';
		strcpy(t + 1, names[i]);
		free(names[i]);
		names[i] = t;
	}
	qsort(names, n, sizeof names[0], cmp_str);
	for (i = 0; i < n; i++) {
		if (names[i][0] == 'd')
			list_add(f->dirs, names[i] + 1, small_folder);
		else
			list_add(f->files, names[i] + 1, small_file);
		free(names[i]);
	}
	win_set_text(f->dirlabel, f->dir);
}

static void fd_chdir(FileDlg *f, const char *name)
{
	char *p;

	if (!strcmp(name, "..")) {
		p = strrchr(f->dir, '/');
		if (p == f->dir)
			p[1] = 0;
		else if (p)
			*p = 0;
	} else {
		char path[256];
		join(path, f->dir, name);
		strncpy(f->dir, path, sizeof f->dir - 1);
	}
	fd_fill(f);
}

static int fd_ok(Win *w, FileDlg *f)
{
	const char *name = edit_text(f->name);
	char path[256];
	struct stat st;

	if (!name[0])
		return 0;
	join(path, f->dir, name);
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
		strcpy(f->dir, path);
		edit_set(f->name, "");
		fd_fill(f);
		return 0;
	}
	if (!f->save && stat(path, &st) < 0) {
		char msg[300];
		sprintf(msg, "%.200s\nFile not found.\nCheck the file name and try again.", path);
		msgbox(w, "Open", msg, MB_OK | MB_EXCLAIM);
		return 0;
	}
	if (f->save && stat(path, &st) == 0) {
		char msg[300];
		sprintf(msg, "%.200s already exists.\nReplace it?", path);
		if (msgbox(w, "Save As", msg, MB_YESNO | MB_EXCLAIM) != ID_YES)
			return 0;
	}
	strcpy(f->path, path);
	return 1;
}

static int filedlg_proc(Win *w, int msg, int a, int b)
{
	FileDlg *f = w->data;

	if (msg == M_COMMAND) {
		switch (a) {
		case ID_FILES:
			if (b == N_SELCHANGE || b == N_DBLCLK)
				edit_set(f->name, list_item(f->files, list_sel(f->files)));
			if (b == N_DBLCLK && fd_ok(w, f))
				dialog_end(w, ID_OK);
			return 1;
		case ID_DIRS:
			if (b == N_DBLCLK)
				fd_chdir(f, list_item(f->dirs, list_sel(f->dirs)));
			return 1;
		case ID_OK:
			if (win_focus() == f->dirs && list_sel(f->dirs) >= 0) {
				fd_chdir(f, list_item(f->dirs, list_sel(f->dirs)));
				return 1;
			}
			if (fd_ok(w, f))
				dialog_end(w, ID_OK);
			return 1;
		}
	}
	return def_proc(w, msg, a, b);
}

int file_dialog(Win *owner, const char *title, char *path, int save)
{
	FileDlg f;
	Win *d;
	char *slash;

	memset(&f, 0, sizeof f);
	f.path = path;
	f.save = save;
	if (path[0] == '/') {
		strncpy(f.dir, path, sizeof f.dir - 1);
		slash = strrchr(f.dir, '/');
		if (slash == f.dir)
			slash[1] = 0;
		else
			*slash = 0;
	} else if (!getcwd(f.dir, sizeof f.dir))
		strcpy(f.dir, "/");
	d = dialog_create(owner, 236, 132, title, filedlg_proc, &f);
	ctl_label(d, 6, 4, 100, 10, "File &Name:");
	f.name = ctl_edit(d, 6, 15, 100, 13, path[0] == '/' ? strrchr(path, '/') + 1 : path, ID_NAME);
	f.files = ctl_list(d, 6, 31, 100, 96, ID_FILES);
	ctl_label(d, 114, 4, 70, 10, "&Directories:");
	f.dirlabel = ctl_label(d, 114, 17, 70, 10, "");
	f.dirs = ctl_list(d, 114, 31, 70, 96, ID_DIRS);
	ctl_button(d, 190, 15, 42, 14, "OK", ID_OK, 1);
	ctl_button(d, 190, 33, 42, 14, "Cancel", ID_CANCEL, 0);
	fd_fill(&f);
	win_set_focus(f.name);
	return dialog_run(d) == ID_OK;
}
