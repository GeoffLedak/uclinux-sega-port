/*
 * wm.c -- windows, frames, menus, scroll bars and the event loop
 *
 * Every window is a Win in a tree: the desktop (root) holds the
 * top-level windows, and windows hold controls and, in Program Manager,
 * other framed windows (S_MDI). Children are listed bottom to top.
 *
 * Drawing is by damage: invalidate() records screen rectangles and
 * repaint_now() redraws them, desktop first, then every window bottom
 * to top, each clipped to the damage (the painter's algorithm).
 * Moving and sizing show an outline, like the real thing, so that only
 * one redraw is needed at the end.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "win.h"

/* colour scheme */
unsigned char col_desktop = C_SILVER, col_workspace = C_WHITE,
	col_title = C_NAVY, col_title_text = C_WHITE,
	col_inactive = C_WHITE, col_inactive_text = C_BLACK,
	col_border = C_SILVER, col_menu = C_WHITE, col_hilite = C_NAVY,
	col_window = C_WHITE;

#define CAP_H		10		/* caption (plus a line under it) */
#define MENU_H		10		/* menu bar (plus a line) */
#define SB_W		9		/* scroll bar, with its line */
#define CORNER		14		/* size of the corner sizing areas */
#define ICON_W		64		/* minimized window: icon and title */
#define ICON_H		36

/* hit test codes */
enum {
	HT_NONE, HT_CLIENT, HT_CAPTION, HT_SYSMENU, HT_MIN, HT_MAX, HT_MENU,
	HT_VSCROLL, HT_HSCROLL, HT_ICON, HT_BORDER,
	HT_LEFT, HT_RIGHT, HT_TOP, HT_BOTTOM,
	HT_TOPLEFT, HT_TOPRIGHT, HT_BOTTOMLEFT, HT_BOTTOMRIGHT
};

Win *root, *active;
int quit;
int dbl_time = 30;			/* frames between the clicks of a double click */
int swap_buttons;			/* A = right, B = left */
static Win *modal;			/* the dialog that has to be answered */
static Win *capture;			/* gets the mouse until the button is up */
static int capture_button;
static Win *dead_list[128];
static int depth;			/* nested dialogs */
static int ndead;

static int ptr_x = SCR_W / 2, ptr_y = SCR_H / 2, buttons;
static int pointer_shape = -1, busy_count;

static Win *last_click_win;
static int last_click_ht, last_click_x, last_click_y;
static unsigned long last_click_time;

/* menu being tracked, for drawing the bar */
static Win *menu_owner;
static int menu_cur = -1;

/* ---------------------------------------------------------------- */
/* Geometry                                                          */
/* ---------------------------------------------------------------- */

static int bsize(Win *w)
{
	if (w->style & S_THICK)
		return 3;
	if (w->style & S_DLGFRAME)
		return 4;
	if (w->style & (S_BORDER | S_CAPTION))
		return 1;
	return 0;
}

/* the parts of a window, relative to its top left corner */
typedef struct { Rect cap, menu, client, vsb, hsb; } Layout;

static void layout(Win *w, Layout *l)
{
	int b = bsize(w), W = w->r.r - w->r.l, H = w->r.b - w->r.t, y = b;

	memset(l, 0, sizeof *l);
	if (w->style & S_CAPTION) {
		set_rect(&l->cap, b, b, W - b, b + CAP_H);
		y += CAP_H + 1;
	}
	if (w->menu) {
		set_rect(&l->menu, b, y, W - b, y + MENU_H);
		y += MENU_H + 1;
	}
	set_rect(&l->client, b, y, W - b, H - b);
	if (w->style & S_VSCROLL) {
		l->client.r -= SB_W;
		set_rect(&l->vsb, l->client.r, l->client.t, l->client.r + SB_W, l->client.b);
	}
	if (w->style & S_HSCROLL) {
		l->client.b -= SB_W;
		set_rect(&l->hsb, l->client.l, l->client.b, l->client.r, l->client.b + SB_W);
		if (w->style & S_VSCROLL)
			l->vsb.b -= SB_W;
	}
	if (l->client.r < l->client.l)
		l->client.r = l->client.l;
	if (l->client.b < l->client.t)
		l->client.b = l->client.t;
}

void win_client(Win *w, Rect *c)
{
	Layout l;

	if (!w->parent) {
		set_rect(c, 0, 0, SCR_W, SCR_H);
		return;
	}
	layout(w, &l);
	set_rect(c, 0, 0, l.client.r - l.client.l, l.client.b - l.client.t);
}

/* screen position of w's client (0, 0) */
static void client_origin(Win *w, int *x, int *y)
{
	Layout l;

	if (!w->parent) {
		*x = *y = 0;
		return;
	}
	client_origin(w->parent, x, y);
	layout(w, &l);
	*x += w->r.l + l.client.l;
	*y += w->r.t + l.client.t;
}

void win_screen(Win *w, Rect *r)
{
	int x, y;

	if (!w->parent) {
		set_rect(r, 0, 0, SCR_W, SCR_H);
		return;
	}
	client_origin(w->parent, &x, &y);
	if (w->state == W_MIN)
		set_rect(r, x + w->ix, y + w->iy, x + w->ix + ICON_W, y + w->iy + ICON_H);
	else
		set_rect(r, x + w->r.l, y + w->r.t, x + w->r.r, y + w->r.b);
}

void win_client_screen(Win *w, Rect *r)
{
	int x, y;
	Rect c;

	client_origin(w, &x, &y);
	win_client(w, &c);
	set_rect(r, x, y, x + c.r, y + c.b);
}

Win *win_top(Win *w)
{
	while (w && w->parent && w->parent != root)
		w = w->parent;
	return w;
}

Win *win_frame(Win *w)
{
	while (w && w->parent && (w->style & S_CHILD))
		w = w->parent;
	return w;
}

int is_child_of(Win *w, Win *p)
{
	for (; w; w = w->parent)
		if (w == p)
			return 1;
	return 0;
}

static int visible(Win *w)
{
	for (; w; w = w->parent)
		if (w->style & S_HIDDEN)
			return 0;
	return 1;
}

static int is_active(Win *w)
{
	if (w->style & S_MDI)
		return w->parent->active == w && win_top(w) == active;
	return w == active;
}

/* ---------------------------------------------------------------- */
/* Damage and painting                                               */
/* ---------------------------------------------------------------- */

#define NDAMAGE 10
static Rect damage[NDAMAGE];
static int ndamage;

void invalidate_screen(const Rect *r0)
{
	Rect r = *r0, s;
	int i;

	set_rect(&s, 0, 0, SCR_W, SCR_H);
	if (!rect_and(&r, &s))
		return;
	for (i = 0; i < ndamage; i++) {
		Rect *d = &damage[i];
		if (r.l >= d->l && r.t >= d->t && r.r <= d->r && r.b <= d->b)
			return;
		if (r.l < d->r && r.r > d->l && r.t < d->b && r.b > d->t) {
			/* overlapping: merge */
			if (r.l > d->l) r.l = d->l;
			if (r.t > d->t) r.t = d->t;
			if (r.r < d->r) r.r = d->r;
			if (r.b < d->b) r.b = d->b;
			damage[i] = damage[--ndamage];
			i = -1;			/* the bigger one may overlap others */
		}
	}
	if (ndamage == NDAMAGE) {
		for (i = 1; i < ndamage; i++) {
			if (damage[0].l > damage[i].l) damage[0].l = damage[i].l;
			if (damage[0].t > damage[i].t) damage[0].t = damage[i].t;
			if (damage[0].r < damage[i].r) damage[0].r = damage[i].r;
			if (damage[0].b < damage[i].b) damage[0].b = damage[i].b;
		}
		ndamage = 1;
		invalidate_screen(&r);
		return;
	}
	damage[ndamage++] = r;
}

void invalidate(Win *w)
{
	Rect r;

	win_screen(w, &r);
	invalidate_screen(&r);
}

void invalidate_client(Win *w, int l, int t, int r, int b)
{
	Rect c, d;

	win_client_screen(w, &c);
	set_rect(&d, c.l + l, c.t + t, c.l + r, c.t + b);
	if (rect_and(&d, &c))
		invalidate_screen(&d);
}

static void draw_nc(Win *w);

static void wrap_label(const char *s, int maxw, char out[2][40])
{
	int n = strlen(s), cut, i;

	out[0][0] = out[1][0] = 0;
	if (text_w(s, F_NORMAL) <= maxw || n >= 40) {
		strncpy(out[0], s, 39);
		out[0][39] = 0;
		return;
	}
	/* break at the last space that fits */
	cut = -1;
	for (i = 0; i < n; i++)
		if (s[i] == ' ' && textn_w(s, i, F_NORMAL) <= maxw)
			cut = i;
	if (cut < 0) {
		strcpy(out[0], s);
		return;
	}
	memcpy(out[0], s, cut);
	out[0][cut] = 0;
	strcpy(out[1], s + cut + 1);
}

/* an icon with its title under it, centred in a cell w wide */
void draw_icon_label(int x, int y, int w, const unsigned char *icon,
		     const char *label, int selected, int textc)
{
	char lines[2][40];
	int i, tw;

	if (icon)
		draw_icon(x + (w - 16) / 2, y, icon, 16);
	wrap_label(label, w + 6, lines);
	for (i = 0; i < 2 && lines[i][0]; i++) {
		tw = text_w(lines[i], F_NORMAL);
		if (selected)
			fill(x + (w - tw) / 2 - 1, y + 18 + i * 9, x + (w + tw) / 2 + 1,
			     y + 18 + i * 9 + 9, col_hilite);
		text(x + (w - tw) / 2, y + 18 + i * 9, lines[i],
		     selected ? C_WHITE : textc, F_NORMAL);
	}
}

static void paint_win(Win *w, const Rect *area)
{
	Rect a, c;
	Win *ch;
	int ox, oy;

	if (w->style & S_HIDDEN)
		return;
	win_screen(w, &a);
	if (!rect_and(&a, area))
		return;
	if (w->state == W_MIN) {
		Rect r;
		win_screen(w, &r);
		gfx_set(r.l, r.t, &a);
		draw_icon_label(0, 0, ICON_W, w->icon ? w->icon : icon_progman,
				w->text, is_active(w),
				w->parent == root ? C_BLACK : C_BLACK);
		return;
	}
	if (w->parent) {
		Rect r;
		win_screen(w, &r);
		gfx_set(r.l, r.t, &a);
		draw_nc(w);
	}
	win_client_screen(w, &c);
	client_origin(w, &ox, &oy);
	if (!rect_and(&c, &a))
		return;
	gfx_set(ox, oy, &c);
	if (w->bg >= 0)
		fill(0, 0, SCR_W, SCR_H, w->bg);
	w->proc(w, M_PAINT, 0, 0);
	for (ch = w->child; ch; ch = ch->next)
		paint_win(ch, &c);
}

void repaint_now(void)
{
	Rect d;

	while (ndamage) {
		d = damage[--ndamage];
		paint_win(root, &d);
	}
	gfx_screen();
}

/* ---------------------------------------------------------------- */
/* Drawing the frame                                                 */
/* ---------------------------------------------------------------- */

static void triangle(int cx, int y, int rows, int up, int c)
{
	int i;

	for (i = 0; i < rows; i++) {
		int half = up ? i : rows - 1 - i;
		hline(cx - half, cx + half + 1, y + i, c);
	}
}

/* a caption button: what = HT_SYSMENU, HT_MIN or HT_MAX */
static void caption_button(Win *w, int x, int y, int what, int pressed)
{
	int d = pressed ? 1 : 0;

	fill(x, y, x + CAP_H, y + CAP_H, C_SILVER);
	if (what == HT_SYSMENU) {
		if (w->style & S_MDI) {
			fill(x + 2, y + 4, x + 7, y + 6, C_WHITE);
			hline(x + 3, x + 8, y + 6, C_GRAY);
			vline(x + 7, y + 5, y + 6, C_GRAY);
		} else {
			fill(x + 1, y + 4, x + 8, y + 6, C_WHITE);
			hline(x + 2, x + 9, y + 6, C_GRAY);
			vline(x + 8, y + 5, y + 6, C_GRAY);
		}
		return;
	}
	bevel(x, y, x + CAP_H, y + CAP_H, !pressed);
	if (what == HT_MIN)
		triangle(x + 5 + d, y + 4 + d, 3, 0, C_BLACK);
	else if (w->state == W_MAX) {
		triangle(x + 5 + d, y + 1 + d, 3, 1, C_BLACK);
		triangle(x + 5 + d, y + 5 + d, 3, 0, C_BLACK);
	} else
		triangle(x + 5 + d, y + 3 + d, 3, 1, C_BLACK);
}

static void draw_scrollbar(Win *w, const Rect *r, int vert)
{
	int pos = vert ? w->vpos : w->hpos, max = vert ? w->vmax : w->hmax;
	int len, t;

	if (vert) {
		vline(r->l, r->t, r->b, C_BLACK);
		fill(r->l + 1, r->t, r->r, r->b, C_SILVER);
		/* arrows */
		bevel(r->l + 1, r->t, r->r, r->t + 8, 1);
		triangle(r->l + 4, r->t + 2, 3, 1, max > 0 ? C_BLACK : C_GRAY);
		hline(r->l + 1, r->r, r->t + 8, C_BLACK);
		bevel(r->l + 1, r->b - 8, r->r, r->b, 1);
		triangle(r->l + 4, r->b - 6, 3, 0, max > 0 ? C_BLACK : C_GRAY);
		hline(r->l + 1, r->r, r->b - 9, C_BLACK);
		len = r->b - r->t - 18;
		if (max > 0 && len >= 8) {
			t = r->t + 9 + (int)((long)(len - 8) * pos / max);
			frame(r->l + 1, t - 1, r->r, t + 9, C_BLACK);
			fill(r->l + 1, t, r->r, t + 8, C_SILVER);
			bevel(r->l + 1, t, r->r, t + 8, 1);
		}
	} else {
		hline(r->l, r->r, r->t, C_BLACK);
		fill(r->l, r->t + 1, r->r, r->b, C_SILVER);
		bevel(r->l, r->t + 1, r->l + 8, r->b, 1);
		/* left arrow */
		{
			int i;
			int c = max > 0 ? C_BLACK : C_GRAY;
			for (i = 0; i < 3; i++)
				vline(r->l + 3 + i, r->t + 5 - i, r->t + 6 + i, c);
		}
		vline(r->l + 8, r->t + 1, r->b, C_BLACK);
		bevel(r->r - 8, r->t + 1, r->r, r->b, 1);
		{
			int i;
			int c = max > 0 ? C_BLACK : C_GRAY;
			for (i = 0; i < 3; i++)
				vline(r->r - 3 - i, r->t + 5 - i, r->t + 6 + i, c);
		}
		vline(r->r - 9, r->t + 1, r->b, C_BLACK);
		len = r->r - r->l - 18;
		if (max > 0 && len >= 8) {
			t = r->l + 9 + (int)((long)(len - 8) * pos / max);
			frame(t - 1, r->t + 1, t + 9, r->b, C_BLACK);
			fill(t, r->t + 1, t + 8, r->b, C_SILVER);
			bevel(t, r->t + 1, t + 8, r->b, 1);
		}
	}
}

static void menu_item_rect(Win *w, int i, Rect *r)	/* window coordinates */
{
	Layout l;
	int k, x;

	layout(w, &l);
	x = l.menu.l + 1;
	for (k = 0; k < i; k++)
		x += text_mn_w(w->menu->menus[k].title, F_BOLD) + 8;
	set_rect(r, x, l.menu.t, x + text_mn_w(w->menu->menus[i].title, F_BOLD) + 8, l.menu.b);
}

static int caption_pressed = HT_NONE;	/* button held down */
static Win *caption_pressed_win;

static void draw_nc(Win *w)
{
	int W = w->r.r - w->r.l, H = w->r.b - w->r.t, b = bsize(w);
	int act = is_active(w), i, x0, x1, tw;
	Layout l;

	layout(w, &l);
	if (b) {
		frame(0, 0, W, H, C_BLACK);
		if (w->style & (S_THICK | S_DLGFRAME)) {
			int band = (w->style & S_THICK) ? col_border
				   : act ? col_title : col_inactive;
			for (i = 1; i < b - 1; i++)
				frame(i, i, W - i, H - i, band);
			frame(b - 1, b - 1, W - b + 1, H - b + 1, C_BLACK);
		}
		if (w->style & S_THICK) {
			/* the marks between the corner and the side sizing areas */
			int c = CORNER;
			vline(c, 0, b, C_BLACK);
			vline(W - 1 - c, 0, b, C_BLACK);
			vline(c, H - b, H, C_BLACK);
			vline(W - 1 - c, H - b, H, C_BLACK);
			hline(0, b, c, C_BLACK);
			hline(0, b, H - 1 - c, C_BLACK);
			hline(W - b, W, c, C_BLACK);
			hline(W - b, W, H - 1 - c, C_BLACK);
		}
	}
	if (w->style & S_CAPTION) {
		x0 = l.cap.l;
		x1 = l.cap.r;
		if (w->style & S_SYSMENU) {
			caption_button(w, x0, l.cap.t, HT_SYSMENU, 0);
			vline(x0 + CAP_H, l.cap.t, l.cap.b, C_BLACK);
			x0 += CAP_H + 1;
		}
		if (w->style & S_MAXBOX) {
			caption_button(w, x1 - CAP_H, l.cap.t, HT_MAX,
				       caption_pressed_win == w && caption_pressed == HT_MAX);
			vline(x1 - CAP_H - 1, l.cap.t, l.cap.b, C_BLACK);
			x1 -= CAP_H + 1;
		}
		if (w->style & S_MINBOX) {
			caption_button(w, x1 - CAP_H, l.cap.t, HT_MIN,
				       caption_pressed_win == w && caption_pressed == HT_MIN);
			vline(x1 - CAP_H - 1, l.cap.t, l.cap.b, C_BLACK);
			x1 -= CAP_H + 1;
		}
		fill(x0, l.cap.t, x1, l.cap.b, act ? col_title : col_inactive);
		tw = text_w(w->text, F_BOLD);
		{
			Rect save = clip, c;
			int ox = org_x, oy = org_y;
			set_rect(&c, org_x + x0, org_y + l.cap.t, org_x + x1, org_y + l.cap.b);
			rect_and(&c, &save);
			gfx_set(ox, oy, &c);
			text(tw <= x1 - x0 ? (x0 + x1 - tw) / 2 : x0 + 2, l.cap.t + 1,
			     w->text, act ? col_title_text : col_inactive_text, F_BOLD);
			gfx_set(ox, oy, &save);
		}
		hline(l.cap.l, l.cap.r, l.cap.b, C_BLACK);
	}
	if (w->menu) {
		fill(l.menu.l, l.menu.t, l.menu.r, l.menu.b, col_menu);
		for (i = 0; i < w->menu->n; i++) {
			Rect r;
			int hi = (menu_owner == w && menu_cur == i);
			menu_item_rect(w, i, &r);
			if (hi)
				fill(r.l, r.t, r.r, r.b, col_hilite);
			text_mn(r.l + 4, r.t + 1, w->menu->menus[i].title,
				hi ? C_WHITE : C_BLACK, F_BOLD);
		}
		hline(l.menu.l, l.menu.r, l.menu.b, C_BLACK);
	}
	if (w->style & S_VSCROLL)
		draw_scrollbar(w, &l.vsb, 1);
	if (w->style & S_HSCROLL)
		draw_scrollbar(w, &l.hsb, 0);
	if ((w->style & S_VSCROLL) && (w->style & S_HSCROLL))
		fill(l.vsb.l, l.vsb.b, l.vsb.r, l.hsb.b, C_SILVER);
}

static int nc_hit(Win *w, int x, int y)		/* window coordinates */
{
	int W = w->r.r - w->r.l, H = w->r.b - w->r.t, b = bsize(w);
	Layout l;

	if (x < 0 || y < 0 || x >= W || y >= H)
		return HT_NONE;
	if ((w->style & S_THICK) && w->state != W_MAX &&
	    (x < b || y < b || x >= W - b || y >= H - b)) {
		int left = x < CORNER, right = x >= W - CORNER;
		int top = y < CORNER, bottom = y >= H - CORNER;
		if (top && left) return HT_TOPLEFT;
		if (top && right) return HT_TOPRIGHT;
		if (bottom && left) return HT_BOTTOMLEFT;
		if (bottom && right) return HT_BOTTOMRIGHT;
		if (x < b) return HT_LEFT;
		if (x >= W - b) return HT_RIGHT;
		if (y < b) return HT_TOP;
		return HT_BOTTOM;
	}
	layout(w, &l);
	if (in_rect(&l.client, x, y))
		return HT_CLIENT;
	if (in_rect(&l.cap, x, y)) {
		int x1 = l.cap.r;
		if ((w->style & S_SYSMENU) && x < l.cap.l + CAP_H + 1)
			return HT_SYSMENU;
		if (w->style & S_MAXBOX) {
			if (x >= x1 - CAP_H)
				return HT_MAX;
			x1 -= CAP_H + 1;
		}
		if ((w->style & S_MINBOX) && x >= x1 - CAP_H && x < x1)
			return HT_MIN;
		return HT_CAPTION;
	}
	if (w->menu && in_rect(&l.menu, x, y))
		return HT_MENU;
	if ((w->style & S_VSCROLL) && in_rect(&l.vsb, x, y))
		return HT_VSCROLL;
	if ((w->style & S_HSCROLL) && in_rect(&l.hsb, x, y))
		return HT_HSCROLL;
	return HT_BORDER;
}

/* the window under (x, y) on the screen, and what part of it */
static Win *find_hit(Win *w, int x, int y, int *ht)
{
	Win *c, *found = 0;
	Rect r;

	for (c = w->child; c; c = c->next) {
		if (c->style & S_HIDDEN)
			continue;
		win_screen(c, &r);
		if (in_rect(&r, x, y))
			found = c;
	}
	if (!found) {
		*ht = HT_CLIENT;
		return w;
	}
	if (found->state == W_MIN) {
		*ht = HT_ICON;
		return found;
	}
	win_screen(found, &r);
	*ht = nc_hit(found, x - r.l, y - r.t);
	if (*ht != HT_CLIENT)
		return found;
	return find_hit(found, x, y, ht);
}

/* ---------------------------------------------------------------- */
/* Creating and managing windows                                     */
/* ---------------------------------------------------------------- */

int send(Win *w, int msg, int a, int b)
{
	if (!w || w->dead)
		return 0;
	return w->proc(w, msg, a, b);
}

static void unlink_win(Win *w)
{
	Win **pp;

	for (pp = &w->parent->child; *pp; pp = &(*pp)->next)
		if (*pp == w) {
			*pp = w->next;
			break;
		}
	w->next = 0;
}

static void link_top(Win *w)
{
	Win **pp;

	for (pp = &w->parent->child; *pp; pp = &(*pp)->next)
		;
	*pp = w;
	w->next = 0;
}

Win *win_create(Win *parent, int x, int y, int wd, int ht, int style,
		const char *title, Proc proc, void *data)
{
	Win *w = calloc(1, sizeof *w);

	if (!w)
		return 0;
	w->parent = parent;
	set_rect(&w->r, x, y, x + wd, y + ht);
	w->nr = w->r;
	w->ix = w->iy = -1;
	w->style = style;
	w->bg = (style & S_CHILD) ? -1 : col_window;
	if (title) {
		strncpy(w->text, title, sizeof w->text - 1);
	}
	w->proc = proc ? proc : def_proc;
	w->data = data;
	if (parent)
		link_top(w);
	send(w, M_CREATE, 0, 0);
	if (parent && !(style & S_HIDDEN)) {
		if (parent == root)
			win_activate(w);
		else if (style & S_MDI) {
			Win *old = parent->active;
			parent->active = w;
			if (old)
				invalidate(old);
		}
		invalidate(w);
	}
	return w;
}

static Win *next_to_activate(Win *except)
{
	Win *c, *best = 0;

	for (c = root->child; c; c = c->next)
		if (c != except && !c->dead && !(c->style & S_HIDDEN))
			best = c;		/* the top-most */
	/* prefer a window that isn't an icon */
	for (c = root->child; c; c = c->next)
		if (c != except && !c->dead && !(c->style & S_HIDDEN) && c->state != W_MIN)
			best = c;
	return best;
}

void win_destroy(Win *w)
{
	Win *p = w->parent, *c;

	if (w->dead)
		return;
	while ((c = w->child) != 0) {
		win_destroy(c);
		if (w->child == c)		/* (already unlinked) */
			w->child = c->next;
	}
	send(w, M_DESTROY, 0, 0);
	if (!(w->style & S_HIDDEN))
		invalidate(w);
	w->dead = 1;
	if (p)
		unlink_win(w);
	if (capture && is_child_of(capture, w))
		capture = 0;
	if (p && p->active == w) {
		Win *n = 0;
		for (c = p->child; c; c = c->next)
			if ((c->style & S_MDI) && !(c->style & S_HIDDEN))
				n = c;
		p->active = n;
		if (n)
			invalidate(n);
	}
	for (c = p; c; c = c->parent)
		if (c->focus && is_child_of(c->focus, w))
			c->focus = 0;
	if (w->owner) {
		/* nothing: dialog_run reactivates the owner */
	}
	if (active == w) {
		active = 0;
		c = next_to_activate(w);
		if (c)
			win_activate(c);
	}
	if (ndead < COUNT(dead_list))
		dead_list[ndead++] = w;
}

static void free_dead(void)
{
	while (ndead)
		free(dead_list[--ndead]);
}

void win_show(Win *w, int show)
{
	if (show)
		w->style &= ~S_HIDDEN;
	invalidate(w);
	if (!show)
		w->style |= S_HIDDEN;
}

void win_set_text(Win *w, const char *s)
{
	strncpy(w->text, s, sizeof w->text - 1);
	w->text[sizeof w->text - 1] = 0;
	invalidate(w);
}

static void fit_max(Win *w)
{
	Rect c;
	int b = bsize(w);

	win_client(w->parent, &c);
	set_rect(&w->r, -b, -b, c.r + b, c.b + b);
}

void win_move(Win *w, int x, int y, int wd, int ht)
{
	int resized = (w->r.r - w->r.l != wd) || (w->r.b - w->r.t != ht);
	Rect c;
	Win *ch;

	invalidate(w);
	set_rect(&w->r, x, y, x + wd, y + ht);
	invalidate(w);
	if (resized) {
		for (ch = w->child; ch; ch = ch->next)
			if (ch->state == W_MAX)
				fit_max(ch);
		win_client(w, &c);
		send(w, M_SIZE, c.r, c.b);
	}
}

static void place_icon(Win *w)
{
	Rect c;
	int x, y, used;
	Win *o;

	win_client(w->parent, &c);
	for (y = c.b - ICON_H; y >= 0; y -= ICON_H)
		for (x = 2; x + ICON_W <= c.r; x += ICON_W) {
			used = 0;
			for (o = w->parent->child; o; o = o->next)
				if (o != w && o->state == W_MIN &&
				    o->ix < x + ICON_W && o->ix + ICON_W > x &&
				    o->iy < y + ICON_H && o->iy + ICON_H > y)
					used = 1;
			if (!used) {
				w->ix = x;
				w->iy = y;
				return;
			}
		}
	w->ix = 2;
	w->iy = c.b - ICON_H;
}

void win_set_state(Win *w, int st)
{
	Rect c;
	int old = w->state;

	if (old == st)
		return;
	invalidate(w);
	if (old == W_NORMAL)
		w->nr = w->r;
	if (st == W_MIN)
		w->was_max = (old == W_MAX);
	w->state = st;
	if (st == W_MAX)
		fit_max(w);
	else if (st == W_NORMAL)
		w->r = w->nr;
	else if (w->ix < 0)
		place_icon(w);
	invalidate(w);
	if (st != W_MIN) {
		win_client(w, &c);
		send(w, M_SIZE, c.r, c.b);
	}
	if (st == W_MIN && w->parent == root && w == active) {
		Win *n = next_to_activate(w);
		if (n && n->state != W_MIN)
			win_activate(n);
	}
	if (st == W_MIN && (w->style & S_MDI) && w->parent->active == w) {
		Win *n = 0, *o;
		for (o = w->parent->child; o; o = o->next)
			if ((o->style & S_MDI) && o->state != W_MIN && o != w)
				n = o;
		if (n) {
			w->parent->active = n;
			invalidate(n);
		}
	}
	(void)c;
}

void win_activate(Win *w)
{
	Win *old;

	if (!w || w == root)
		return;
	if (w->style & S_MDI) {
		Win *p = w->parent;
		if (p->active != w) {
			Win *o = p->active;
			p->active = w;
			unlink_win(w);
			link_top(w);
			if (o)
				invalidate(o);
			invalidate(w);
		}
	}
	w = win_top(w);
	if (w == active) {
		/* make sure it is on top anyway */
		if (w->next) {
			unlink_win(w);
			link_top(w);
			invalidate(w);
		}
		return;
	}
	old = active;
	active = w;
	unlink_win(w);
	link_top(w);
	if (old) {
		invalidate(old);
		send(old, M_ACTIVATE, 0, 0);
	}
	invalidate(w);
	send(w, M_ACTIVATE, 1, 0);
}

void win_set_focus(Win *w)
{
	Win *f = win_frame(w), *old;

	if (!f)
		return;
	if (w == f)
		w = 0;
	old = f->focus;
	if (old == w)
		return;
	f->focus = w;
	if (old)
		send(old, M_FOCUS, 0, 0);
	if (w)
		send(w, M_FOCUS, 1, 0);
}

Win *win_focus(void)
{
	Win *f = active;

	if (!f)
		return root;
	while (f->active && f->active->state != W_MIN && !(f->active->style & S_HIDDEN))
		f = f->active;
	return f->focus ? f->focus : f;
}

void set_timer(Win *w, int frames)
{
	w->timer = w->timer_left = frames;
}

void set_scroll(Win *w, int vert, int pos, int max, int page)
{
	short *p = vert ? &w->vpos : &w->hpos;

	if (max < 0)
		max = 0;
	if (pos > max)
		pos = max;
	if (pos < 0)
		pos = 0;
	if (p[0] == pos && p[1] == max && p[2] == page)
		return;
	p[0] = pos;
	p[1] = max;
	p[2] = page;
	{
		Layout l;
		Rect s, r;
		layout(w, &l);
		win_screen(w, &s);
		r = vert ? l.vsb : l.hsb;
		r.l += s.l; r.r += s.l; r.t += s.t; r.b += s.t;
		invalidate_screen(&r);
	}
}

void set_pointer(int shape)
{
	if (busy_count)
		shape = PTR_WAIT;
	if (shape != pointer_shape) {
		pointer_shape = shape;
		hw_pointer(shape);
	}
}

void busy(int on)
{
	busy_count += on ? 1 : -1;
	if (busy_count < 0)
		busy_count = 0;
	pointer_shape = -2;
	set_pointer(PTR_ARROW);
}

void beep(void)
{
	hw_beep();
}

void center_window(Win *w, Win *over)
{
	Rect o;
	int wd = w->r.r - w->r.l, ht = w->r.b - w->r.t, x, y;

	if (over && over != root && over->state != W_MIN)
		win_screen(over, &o);
	else
		set_rect(&o, 0, 0, SCR_W, SCR_H);
	x = (o.l + o.r - wd) / 2;
	y = (o.t + o.b - ht) / 2;
	if (x + wd > SCR_W) x = SCR_W - wd;
	if (y + ht > SCR_H) y = SCR_H - ht;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	win_move(w, x, y, wd, ht);
}

/* ---------------------------------------------------------------- */
/* Input                                                             */
/* ---------------------------------------------------------------- */

enum { E_NONE, E_DOWN, E_UP, E_MOVE, E_KEY };
typedef struct { int type, x, y, button, key; unsigned long time; } Event;

#define KEYQ 64
static int keyq[KEYQ], kq_head, kq_tail;
static struct hw_input pend;
static int have_pend;

static void push_key(int k)
{
	int n = (kq_head + 1) & (KEYQ - 1);

	if (n != kq_tail) {
		keyq[kq_head] = k;
		kq_head = n;
	}
}

static void parse_keys(const unsigned char *b, int n)
{
	int i = 0, c, alt, p1, p2, k;

	while (i < n) {
		c = b[i++];
		if (c != 27) {
			if (c == 127)
				c = K_BS;
			else if (c == '\n')
				c = K_ENTER;
			push_key(c);
			continue;
		}
		alt = 0;
		if (i >= n) {
			push_key(K_ESC);
			continue;
		}
		if (b[i] == 27) {
			alt = K_ALT;
			i++;
			if (i >= n) {
				push_key(K_ALT | K_ESC);
				continue;
			}
		}
		if (b[i] == '[') {
			i++;
			p1 = p2 = 0;
			while (i < n && b[i] >= '0' && b[i] <= '9')
				p1 = p1 * 10 + b[i++] - '0';
			if (i < n && b[i] == ';') {
				i++;
				while (i < n && b[i] >= '0' && b[i] <= '9')
					p2 = p2 * 10 + b[i++] - '0';
			}
			if (i >= n)
				break;
			c = b[i++];
			k = 0;
			switch (c) {
			case 'A': k = K_UP; break;
			case 'B': k = K_DOWN; break;
			case 'C': k = K_RIGHT; break;
			case 'D': k = K_LEFT; break;
			case 'H': k = K_HOME; break;
			case 'F': k = K_END; break;
			case '~':
				switch (p1) {
				case 1: k = K_HOME; break;
				case 2: k = K_INS; break;
				case 3: k = K_DEL; break;
				case 4: k = K_END; break;
				case 5: k = K_PGUP; break;
				case 6: k = K_PGDN; break;
				case 15: k = K_F1 + 4; break;
				case 17: k = K_F1 + 5; break;
				case 18: k = K_F1 + 6; break;
				case 19: k = K_F1 + 7; break;
				case 20: k = K_F1 + 8; break;
				case 21: k = K_F1 + 9; break;
				case 23: k = K_F1 + 10; break;
				case 24: k = K_F1 + 11; break;
				}
				break;
			}
			if (k) {
				if (p2 == 5)
					k |= K_CTRL;
				push_key(k | alt);
			}
			continue;
		}
		if (b[i] == 'O' && i + 1 < n) {
			c = b[i + 1];
			if (c >= 'P' && c <= 'S') {
				i += 2;
				push_key((K_F1 + c - 'P') | alt);
				continue;
			}
		}
		/* Alt+key */
		c = b[i++];
		if (c >= 'A' && c <= 'Z')
			c += 32;
		if (c == 127)
			c = K_BS;
		push_key(K_ALT | c);
	}
}

static int last_pad;

static int poll_event(Event *e)
{
	struct hw_input in;
	unsigned char kb[32];
	int n, changed, bit;

	if (kq_tail != kq_head) {
		e->type = E_KEY;
		e->key = keyq[kq_tail];
		kq_tail = (kq_tail + 1) & (KEYQ - 1);
		return 1;
	}
	if (have_pend || hw_input(&in) == 1) {
		if (have_pend)
			in = pend;
		else if (swap_buttons)
			in.buttons = ((in.buttons & B_LEFT) ? B_RIGHT : 0) |
				     ((in.buttons & B_RIGHT) ? B_LEFT : 0);
		have_pend = 0;
		/* Start on the pad: the Task List */
		if ((in.pad & 0x80) && !(last_pad & 0x80))
			push_key(K_START);
		last_pad = in.pad;
		ptr_x = in.x;
		ptr_y = in.y;
		changed = in.buttons ^ buttons;
		if (changed) {
			bit = (changed & B_LEFT) ? B_LEFT : B_RIGHT;
			e->type = (in.buttons & bit) ? E_DOWN : E_UP;
			e->button = bit;
			e->x = in.x;
			e->y = in.y;
			e->time = in.time;
			buttons ^= bit;
			if (buttons != in.buttons) {
				pend = in;
				have_pend = 1;
			}
			return 1;
		}
		return poll_event(e);
	}
	if (in.x != ptr_x || in.y != ptr_y) {
		ptr_x = in.x;
		ptr_y = in.y;
		e->type = E_MOVE;
		e->x = ptr_x;
		e->y = ptr_y;
		return 1;
	}
	/* (every byte makes at most one key: read what fits in the queue) */
	n = KEYQ - 1 - ((kq_head - kq_tail) & (KEYQ - 1));
	n = hw_keys((char *)kb, n < (int)sizeof kb ? n : (int)sizeof kb);
	if (n > 0) {
		parse_keys(kb, n);
		if (kq_tail != kq_head)
			return poll_event(e);
	}
	return 0;
}

static unsigned long last_tick;

static void walk_timers(Win *w, int el)
{
	Win *c, *next;

	if (w->timer > 0 && !w->dead) {
		w->timer_left -= el;
		if (w->timer_left <= 0) {
			w->timer_left = w->timer;
			send(w, M_TIMER, 0, 0);
		}
	}
	for (c = w->child; c; c = next) {
		next = c->next;
		walk_timers(c, el);
	}
}

static int next_timer(Win *w, int best)
{
	Win *c;

	if (w->timer > 0 && w->timer_left < best)
		best = w->timer_left;
	for (c = w->child; c; c = c->next)
		best = next_timer(c, best);
	return best;
}

static void idle(void)
{
	unsigned long now = hw_frames();
	int el = (int)(now - last_tick);

	if (el > 0) {
		last_tick = now;
		walk_timers(root, el);
	}
	repaint_now();
	if (!depth)
		free_dead();
	hw_flush();
}

/* wait for an event; with timeout >= 0, give up after that many frames */
static int get_event(Event *e, int do_idle, int timeout)
{
	unsigned long start = hw_frames();
	int t;

	for (;;) {
		if (poll_event(e))
			return 1;
		if (do_idle) {
			idle();
			if (poll_event(e))
				return 1;
		} else
			hw_flush();
		if (timeout >= 0 && (long)(hw_frames() - start) >= timeout)
			return 0;
		t = do_idle ? next_timer(root, 60) : 60;
		if (timeout >= 0 && t > timeout)
			t = timeout;
		if (t < 1)
			t = 1;
		hw_wait(t);
	}
}

/* ---------------------------------------------------------------- */
/* Moving and sizing (outline)                                       */
/* ---------------------------------------------------------------- */

static int min_width(Win *w)
{
	int m = 2 * bsize(w) + 4;

	if (w->style & S_SYSMENU) m += CAP_H + 1;
	if (w->style & S_MINBOX) m += CAP_H + 1;
	if (w->style & S_MAXBOX) m += CAP_H + 1;
	return m + 20;
}

static int min_height(Win *w)
{
	return 2 * bsize(w) + ((w->style & S_CAPTION) ? CAP_H + 1 : 0) +
	       (w->menu ? MENU_H + 1 : 0) + 4;
}

static int size_pointer(int ht)
{
	switch (ht) {
	case HT_LEFT: case HT_RIGHT: return PTR_SIZE_WE;
	case HT_TOP: case HT_BOTTOM: return PTR_SIZE_NS;
	case HT_TOPLEFT: case HT_BOTTOMRIGHT: return PTR_SIZE_NWSE;
	case HT_TOPRIGHT: case HT_BOTTOMLEFT: return PTR_SIZE_NESW;
	}
	return PTR_ARROW;
}

/* Drag an outline. ht: HT_CAPTION / HT_ICON to move, a border to size.
 * keyboard: started from the system menu (arrows, Enter, Esc).
 * Returns 1 if the window was moved or sized. */
static int track(Win *w, int ht, int keyboard)
{
	Rect start, r, bounds, pr;
	int sx = ptr_x, sy = ptr_y, drawn = 0, done = 0, ok = 1, thick;
	int mw = min_width(w), mh = min_height(w), moved = 0;
	Event e;

	repaint_now();
	win_screen(w, &start);
	r = start;
	if (w->parent)
		win_client_screen(w->parent, &pr);
	else
		set_rect(&pr, 0, 0, SCR_W, SCR_H);
	bounds = pr;
	thick = (ht == HT_ICON) ? 1 : 3;
	if (keyboard) {
		/* put the pointer in the middle of the caption / bottom right */
		if (ht == HT_CAPTION)
			sx = (r.l + r.r) / 2, sy = r.t + 6;
		else
			sx = r.r - 2, sy = r.b - 2;
		hw_warp(sx, sy);
		ptr_x = sx;
		ptr_y = sy;
		xor_frame(r.l, r.t, r.r, r.b, thick);
		drawn = 1;
		set_pointer(ht == HT_CAPTION ? PTR_ARROW : PTR_SIZE_NWSE);
	} else if (ht != HT_CAPTION && ht != HT_ICON) {
		xor_frame(r.l, r.t, r.r, r.b, thick);
		drawn = 1;
	}
	hw_flush();
	while (!done) {
		int x, y;
		if (!get_event(&e, 0, -1))
			continue;
		x = ptr_x;
		y = ptr_y;
		if (e.type == E_UP && e.button == B_LEFT && !keyboard)
			done = 1;
		else if (e.type == E_DOWN && keyboard)
			done = 1;
		else if (e.type == E_KEY) {
			int k = e.key & ~K_CTRL;
			if (k == K_ESC) {
				ok = 0;
				done = 1;
			} else if (k == K_ENTER)
				done = 1;
			else if (keyboard && (k == K_UP || k == K_DOWN || k == K_LEFT || k == K_RIGHT)) {
				x += (k == K_RIGHT) ? 4 : (k == K_LEFT) ? -4 : 0;
				y += (k == K_DOWN) ? 4 : (k == K_UP) ? -4 : 0;
				if (x < 0) x = 0;
				if (y < 0) y = 0;
				if (x >= SCR_W) x = SCR_W - 1;
				if (y >= SCR_H) y = SCR_H - 1;
				hw_warp(x, y);
				ptr_x = x;
				ptr_y = y;
			} else
				continue;
		} else if (e.type != E_MOVE)
			continue;
		if (done)
			break;
		{
			Rect n = start;
			int dx = x - sx, dy = y - sy;
			if (ht == HT_CAPTION || ht == HT_ICON) {
				/* keep a bit of the caption reachable */
				if (n.t + dy < bounds.t) dy = bounds.t - n.t;
				if (n.t + dy > bounds.b - 8) dy = bounds.b - 8 - n.t;
				if (n.l + dx > bounds.r - 16) dx = bounds.r - 16 - n.l;
				if (n.r + dx < bounds.l + 16) dx = bounds.l + 16 - n.r;
				n.l += dx; n.r += dx; n.t += dy; n.b += dy;
			} else {
				if (keyboard)
					ht = HT_BOTTOMRIGHT;
				if (ht == HT_LEFT || ht == HT_TOPLEFT || ht == HT_BOTTOMLEFT) {
					n.l += dx;
					if (n.r - n.l < mw) n.l = n.r - mw;
				}
				if (ht == HT_RIGHT || ht == HT_TOPRIGHT || ht == HT_BOTTOMRIGHT) {
					n.r += dx;
					if (n.r - n.l < mw) n.r = n.l + mw;
				}
				if (ht == HT_TOP || ht == HT_TOPLEFT || ht == HT_TOPRIGHT) {
					n.t += dy;
					if (n.b - n.t < mh) n.t = n.b - mh;
				}
				if (ht == HT_BOTTOM || ht == HT_BOTTOMLEFT || ht == HT_BOTTOMRIGHT) {
					n.b += dy;
					if (n.b - n.t < mh) n.b = n.t + mh;
				}
			}
			if (!drawn && (abs(x - sx) > 1 || abs(y - sy) > 1)) {
				xor_frame(n.l, n.t, n.r, n.b, thick);
				drawn = 1;
				r = n;
			} else if (drawn && memcmp(&n, &r, sizeof n)) {
				xor_frame(r.l, r.t, r.r, r.b, thick);
				xor_frame(n.l, n.t, n.r, n.b, thick);
				r = n;
			}
			hw_flush();
		}
	}
	if (drawn)
		xor_frame(r.l, r.t, r.r, r.b, thick);
	if (ok && drawn && memcmp(&r, &start, sizeof r)) {
		if (ht == HT_ICON) {
			w->ix += r.l - start.l;
			w->iy += r.t - start.t;
			invalidate_screen(&start);
			invalidate(w);
		} else
			win_move(w, r.l - pr.l, r.t - pr.t, r.r - r.l, r.b - r.t);
		moved = 1;
	}
	set_pointer(PTR_ARROW);
	return moved;
}

/* a caption button held down: returns 1 if released over it */
static int track_caption_button(Win *w, int ht)
{
	Rect s;
	Event e;
	int in = 1, now;

	win_screen(w, &s);
	caption_pressed_win = w;
	caption_pressed = ht;
	invalidate(w);
	repaint_now();
	for (;;) {
		if (!get_event(&e, 0, -1))
			continue;
		if (e.type == E_UP && e.button == B_LEFT)
			break;
		if (e.type == E_KEY && e.key == K_ESC) {
			in = 0;
			break;
		}
		now = nc_hit(w, ptr_x - s.l, ptr_y - s.t) == ht;
		if (now != in) {
			in = now;
			caption_pressed = in ? ht : HT_NONE;
			invalidate(w);
			repaint_now();
		}
	}
	caption_pressed_win = 0;
	caption_pressed = HT_NONE;
	invalidate(w);
	return in;
}

/* ---------------------------------------------------------------- */
/* Scroll bars                                                       */
/* ---------------------------------------------------------------- */

static void track_scroll(Win *w, int vert)
{
	Layout l;
	Rect s, r;
	int msg = vert ? M_VSCROLL : M_HSCROLL, part, pos, len, t, delay = 20;
	int grab = 0, p0;
	Event e;

	win_screen(w, &s);
	layout(w, &l);
	r = vert ? l.vsb : l.hsb;
	r.l += s.l; r.r += s.l; r.t += s.t; r.b += s.t;
	pos = vert ? ptr_y - r.t : ptr_x - r.l;
	len = (vert ? r.b - r.t : r.r - r.l);
	if ((vert ? w->vmax : w->hmax) <= 0)
		return;
	t = 9 + (int)((long)(len - 18 - 8) * (vert ? w->vpos : w->hpos) /
		      (vert ? w->vmax : w->hmax));
	if (pos < 8)
		part = SB_LINEUP;
	else if (pos >= len - 8)
		part = SB_LINEDOWN;
	else if (pos < t)
		part = SB_PAGEUP;
	else if (pos >= t + 8)
		part = SB_PAGEDOWN;
	else {
		part = SB_THUMB;
		grab = pos - t;
	}
	p0 = vert ? ptr_y : ptr_x;
	if (part != SB_THUMB)
		send(w, msg, part, 0);
	repaint_now();
	hw_flush();
	for (;;) {
		if (get_event(&e, 0, part == SB_THUMB ? -1 : delay)) {
			if (e.type == E_UP && e.button == B_LEFT)
				break;
			if (part == SB_THUMB && e.type == E_MOVE) {
				int max = vert ? w->vmax : w->hmax, span = len - 18 - 8, np;
				int p = (vert ? ptr_y - r.t : ptr_x - r.l) - grab - 9;
				if (span <= 0)
					continue;
				if (p < 0) p = 0;
				if (p > span) p = span;
				np = (int)(((long)p * max + span / 2) / span);
				send(w, msg, SB_THUMB, np);
				repaint_now();
				hw_flush();
			}
			continue;
		}
		/* held down: repeat */
		delay = 3;
		if (part == SB_PAGEUP || part == SB_PAGEDOWN) {
			/* stop when the thumb reaches the pointer */
			int max = vert ? w->vmax : w->hmax;
			int cur = vert ? w->vpos : w->hpos;
			int tt = 9 + (int)((long)(len - 18 - 8) * cur / (max ? max : 1));
			int pp = (vert ? ptr_y - r.t : ptr_x - r.l);
			if ((part == SB_PAGEUP && pp >= tt) || (part == SB_PAGEDOWN && pp < tt + 8))
				continue;
		}
		send(w, msg, part, 0);
		repaint_now();
		hw_flush();
	}
	(void)p0;
}

/* ---------------------------------------------------------------- */
/* Menus                                                             */
/* ---------------------------------------------------------------- */

static MenuItem sys_items[] = {
	{ "&Restore", SC_RESTORE, 0 },
	{ "&Move", SC_MOVE, 0 },
	{ "&Size", SC_SIZE, 0 },
	{ "Mi&nimize", SC_MINIMIZE, 0 },
	{ "Ma&ximize", SC_MAXIMIZE, 0 },
	{ 0, 0, 0 },
	{ "&Close\tAlt+F4", SC_CLOSE, 0 },
	{ 0, 0, 0 },
	{ "S&witch To...\tCtrl+Esc", SC_SWITCH, 0 },
};
static MenuItem mdi_sys_items[] = {
	{ "&Restore", SC_RESTORE, 0 },
	{ "&Move", SC_MOVE, 0 },
	{ "&Size", SC_SIZE, 0 },
	{ "Mi&nimize", SC_MINIMIZE, 0 },
	{ "Ma&ximize", SC_MAXIMIZE, 0 },
	{ 0, 0, 0 },
	{ "&Close\tCtrl+F4", SC_CLOSE, 0 },
	{ 0, 0, 0 },
	{ "Nex&t\tCtrl+F6", SC_NEXT, 0 },
};
static MenuItem dlg_sys_items[] = {
	{ "&Move", SC_MOVE, 0 },
	{ "&Close", SC_CLOSE, 0 },
};
static Menu sys_menu;
static Rect sys_box;			/* clicking here again quickly: */
static int sys_dbl_cmd;			/* this command (close / restore) */
static unsigned long sys_time;

static Menu *system_menu(Win *w)
{
	int i, n;
	MenuItem *it;

	if (w->style & S_DIALOG) {
		sys_menu.items = dlg_sys_items;
		sys_menu.n = COUNT(dlg_sys_items);
		return &sys_menu;
	}
	if (w->style & S_MDI) {
		it = mdi_sys_items;
		n = COUNT(mdi_sys_items);
	} else {
		it = sys_items;
		n = COUNT(sys_items);
	}
	for (i = 0; i < n; i++) {
		it[i].flags = 0;
		switch (it[i].id) {
		case SC_RESTORE: if (w->state == W_NORMAL) it[i].flags = MF_GRAY; break;
		case SC_MOVE: if (w->state == W_MAX) it[i].flags = MF_GRAY; break;
		case SC_SIZE: if (w->state != W_NORMAL || !(w->style & S_THICK)) it[i].flags = MF_GRAY; break;
		case SC_MINIMIZE: if (w->state == W_MIN || !(w->style & S_MINBOX)) it[i].flags = MF_GRAY; break;
		case SC_MAXIMIZE: if (w->state == W_MAX || !(w->style & S_MAXBOX)) it[i].flags = MF_GRAY; break;
		}
	}
	sys_menu.items = it;
	sys_menu.n = n;
	return &sys_menu;
}

#define ITEM_H		10
#define SEP_H		5

typedef struct {
	Menu *m;
	Rect r;			/* screen */
	int sel;
} Popup;

static int popup_item_y(Popup *p, int i)
{
	int k, y = p->r.t + 2;

	for (k = 0; k < i; k++)
		y += p->m->items[k].text ? ITEM_H : SEP_H;
	return y;
}

static void popup_layout(Popup *p, int x, int y)
{
	int i, w = 40, h = 4, tw, sw;
	const char *tab;

	for (i = 0; i < p->m->n; i++) {
		MenuItem *it = &p->m->items[i];
		if (!it->text) {
			h += SEP_H;
			continue;
		}
		h += ITEM_H;
		tw = text_mn_w(it->text, F_BOLD);
		tab = strchr(it->text, '\t');
		sw = tab ? text_w(tab + 1, F_BOLD) + 12 : 0;
		if (tw + sw + 20 > w)
			w = tw + sw + 20;
	}
	if (x + w > SCR_W) x = SCR_W - w;
	if (y + h > SCR_H) y = SCR_H - h;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	set_rect(&p->r, x, y, x + w, y + h);
}

static void popup_draw(Popup *p)
{
	int i, y, c;
	const char *tab;

	gfx_screen();
	fill(p->r.l, p->r.t, p->r.r, p->r.b, col_menu);
	frame(p->r.l, p->r.t, p->r.r, p->r.b, C_BLACK);
	for (i = 0; i < p->m->n; i++) {
		MenuItem *it = &p->m->items[i];
		y = popup_item_y(p, i);
		if (!it->text) {
			hline(p->r.l + 1, p->r.r - 1, y + 2, C_BLACK);
			continue;
		}
		c = (it->flags & MF_GRAY) ? C_GRAY : C_BLACK;
		if (i == p->sel) {
			fill(p->r.l + 1, y, p->r.r - 1, y + ITEM_H, col_hilite);
			if (!(it->flags & MF_GRAY))
				c = C_WHITE;
		}
		if (it->flags & MF_CHECK) {
			/* a check mark */
			int x = p->r.l + 3;
			line(x, y + 4, x + 2, y + 6, c);
			line(x + 2, y + 6, x + 6, y + 2, c);
			line(x, y + 5, x + 2, y + 7, c);
			line(x + 2, y + 7, x + 6, y + 3, c);
		}
		text_mn(p->r.l + 11, y + 1, it->text, c, F_BOLD);
		tab = strchr(it->text, '\t');
		if (tab)
			text(p->r.r - 5 - text_w(tab + 1, F_BOLD), y + 1, tab + 1, c, F_BOLD);
	}
}

static int popup_hit(Popup *p, int x, int y)
{
	int i, iy;

	if (!in_rect(&p->r, x, y))
		return -2;
	for (i = 0; i < p->m->n; i++) {
		iy = popup_item_y(p, i);
		if (y >= iy && y < iy + (p->m->items[i].text ? ITEM_H : SEP_H))
			return p->m->items[i].text ? i : -1;
	}
	return -1;
}

static void popup_close(Popup *p)
{
	if (p->m) {
		invalidate_screen(&p->r);
		p->m = 0;
	}
}

static int bar_hit(Win *w, int x, int y)
{
	Rect s, r;
	int i;

	if (!w->menu)
		return -1;
	win_screen(w, &s);
	for (i = 0; i < w->menu->n; i++) {
		menu_item_rect(w, i, &r);
		if (in_rect(&r, x - s.l, y - s.t))
			return i;
	}
	return -1;
}

/*
 * Track a menu. owner gets M_INITMENU and the command.
 * With a menu bar: start = the menu to open. Without (single != 0):
 * single pops up at (x, y). by_key: opened from the keyboard.
 * Returns the chosen id, or 0.
 */
static int track_menu(Win *owner, int start, Menu *single, int x, int y,
		      int by_key, int held)
{
	Popup p;
	Event e;
	int cur = start, result = 0, done = 0, i, h, dragged = 0, k;
	Rect s;

	memset(&p, 0, sizeof p);
	win_screen(owner, &s);
	menu_owner = single ? 0 : owner;
	for (;;) {
		/* open menu number cur */
		if (!p.m) {
			Menu *m;
			int px = x, py = y;
			if (single)
				m = single;
			else {
				Rect r;
				m = &owner->menu->menus[cur];
				menu_cur = cur;
				menu_item_rect(owner, cur, &r);
				px = s.l + r.l;
				py = s.t + r.b + 1;
				invalidate(owner);
				send(owner, M_INITMENU, cur, 0);
			}
			repaint_now();
			p.m = m;
			p.sel = by_key ? 0 : -1;
			while (by_key && p.sel < m->n && !m->items[p.sel].text)
				p.sel++;
			popup_layout(&p, px, py);
			popup_draw(&p);
			hw_flush();
		}
		if (done)
			break;
		if (!get_event(&e, 0, -1))
			continue;
		switch (e.type) {
		case E_MOVE:
			i = single ? -1 : bar_hit(owner, ptr_x, ptr_y);
			if (i >= 0 && i != cur) {
				popup_close(&p);
				cur = i;
				break;
			}
			h = popup_hit(&p, ptr_x, ptr_y);
			if (h == -2 && !held)
				break;
			if (h < -1)
				h = -1;
			if (h != p.sel) {
				p.sel = h;
				popup_draw(&p);
			}
			if (held)
				dragged = 1;
			break;
		case E_DOWN:
			held = 1;
			dragged = 0;
			i = single ? -1 : bar_hit(owner, ptr_x, ptr_y);
			if (i >= 0) {
				if (i != cur) {
					popup_close(&p);
					cur = i;
				}
				break;
			}
			h = popup_hit(&p, ptr_x, ptr_y);
			if (h == -2) {
				if (single && sys_dbl_cmd && in_rect(&sys_box, ptr_x, ptr_y) &&
				    e.time - sys_time < dbl_time)
					result = sys_dbl_cmd;
				done = 1;	/* clicked outside: close */
				break;
			}
			p.sel = h;
			popup_draw(&p);
			break;
		case E_UP:
			held = 0;
			h = popup_hit(&p, ptr_x, ptr_y);
			if (h >= 0) {
				if (!(p.m->items[h].flags & MF_GRAY)) {
					result = p.m->items[h].id;
					done = 1;
				}
			} else if (h == -2 && dragged) {
				i = single ? -1 : bar_hit(owner, ptr_x, ptr_y);
				if (i < 0)
					done = 1;
			}
			break;
		case E_KEY:
			k = e.key;
			if (k == K_ESC || k == (K_ALT | K_ESC) || k == (K_F1 + 9)) {
				done = 1;
			} else if ((k == K_LEFT || k == K_RIGHT) && !single) {
				popup_close(&p);
				cur = (cur + (k == K_RIGHT ? 1 : owner->menu->n - 1)) % owner->menu->n;
				by_key = 1;
			} else if (k == K_UP || k == K_DOWN) {
				int n = p.m->n, d = (k == K_DOWN) ? 1 : n - 1, j;
				i = p.sel < 0 ? (k == K_DOWN ? n - 1 : 0) : p.sel;
				for (j = 0; j < n; j++) {
					i = (i + d) % n;
					if (p.m->items[i].text)
						break;
				}
				p.sel = i;
				popup_draw(&p);
			} else if (k == K_ENTER) {
				if (p.sel >= 0 && !(p.m->items[p.sel].flags & MF_GRAY)) {
					result = p.m->items[p.sel].id;
					done = 1;
				}
			} else {
				int c = k & ~K_ALT;
				if (c >= 'A' && c <= 'Z')
					c += 32;
				for (i = 0; i < p.m->n; i++)
					if (p.m->items[i].text && mnemonic(p.m->items[i].text) == c &&
					    !(p.m->items[i].flags & MF_GRAY)) {
						result = p.m->items[i].id;
						done = 1;
						break;
					}
			}
			break;
		}
		hw_flush();
		if (done) {
			popup_close(&p);
			break;
		}
	}
	popup_close(&p);
	menu_owner = 0;
	menu_cur = -1;
	invalidate(owner);
	repaint_now();
	hw_flush();
	return result;
}

int popup_menu(Win *owner, Menu *m, int x, int y)
{
	return track_menu(owner, 0, m, x, y, 0, buttons != 0);
}

static void open_system_menu(Win *w, int by_key)
{
	Rect s;
	int id;

	win_screen(w, &s);
	sys_dbl_cmd = 0;
	if (!by_key) {
		sys_time = last_click_time;
		if (w->state == W_MIN) {
			sys_box = s;
			sys_dbl_cmd = SC_RESTORE;
		} else {
			set_rect(&sys_box, s.l + bsize(w), s.t + bsize(w),
				 s.l + bsize(w) + CAP_H, s.t + bsize(w) + CAP_H);
			sys_dbl_cmd = SC_CLOSE;
		}
	}
	if (w->state == W_MIN)
		id = track_menu(w, 0, system_menu(w), s.l, s.t > 100 ? s.t - 95 : s.b, by_key, 0);
	else
		id = track_menu(w, 0, system_menu(w), s.l + bsize(w), s.t + bsize(w) + CAP_H + 1,
				by_key, buttons != 0);
	sys_dbl_cmd = 0;
	if (id)
		send(w, M_COMMAND, id, 0);
}

static void open_menu(Win *w, int i, int by_key)
{
	int id = track_menu(w, i, 0, 0, 0, by_key, buttons != 0);

	if (id)
		send(w, M_COMMAND, id, 0);
}

/* ---------------------------------------------------------------- */
/* Dispatching                                                       */
/* ---------------------------------------------------------------- */


static void client_point(Win *w, int x, int y, int *cx, int *cy)
{
	int ox, oy;

	client_origin(w, &ox, &oy);
	*cx = x - ox;
	*cy = y - oy;
}

static Win *mdi_child_of(Win *w)
{
	for (; w; w = w->parent)
		if (w->style & S_MDI)
			return w;
	return 0;
}

static void mouse_down(Event *e)
{
	int ht, cx, cy, dbl;
	Win *w = find_hit(root, e->x, e->y, &ht), *m;

	if (modal && !is_child_of(w, modal)) {
		beep();
		win_activate(modal);
		return;
	}
	if (w->style & S_DISABLED)
		return;
	if (e->button == B_RIGHT) {
		if (ht == HT_CLIENT) {
			client_point(w, e->x, e->y, &cx, &cy);
			capture = w;
			capture_button = B_RIGHT;
			send(w, M_RDOWN, cx, cy);
		}
		return;
	}

	dbl = (w == last_click_win && ht == last_click_ht &&
	       e->time - last_click_time < dbl_time &&
	       abs(e->x - last_click_x) < 6 && abs(e->y - last_click_y) < 6);
	last_click_win = dbl ? 0 : w;
	last_click_ht = ht;
	last_click_time = e->time;
	last_click_x = e->x;
	last_click_y = e->y;

	if (w != root)
		win_activate(w);
	m = mdi_child_of(w);
	if (m)
		win_activate(m);

	switch (ht) {
	case HT_CAPTION:
		if (dbl && (w->style & S_MAXBOX))
			win_set_state(w, w->state == W_MAX ? W_NORMAL : W_MAX);
		else if (w->state != W_MAX)
			track(w, HT_CAPTION, 0);
		return;
	case HT_SYSMENU:
		if (dbl) {
			send(w, M_COMMAND, SC_CLOSE, 0);
			return;
		}
		open_system_menu(w, 0);
		return;
	case HT_MIN:
		if (track_caption_button(w, HT_MIN))
			send(w, M_COMMAND, SC_MINIMIZE, 0);
		return;
	case HT_MAX:
		if (track_caption_button(w, HT_MAX))
			send(w, M_COMMAND, w->state == W_MAX ? SC_RESTORE : SC_MAXIMIZE, 0);
		return;
	case HT_MENU:
		{
			int i = bar_hit(w, e->x, e->y);
			if (i >= 0)
				open_menu(w, i, 0);
		}
		return;
	case HT_VSCROLL:
	case HT_HSCROLL:
		track_scroll(w, ht == HT_VSCROLL);
		return;
	case HT_ICON:
		if (dbl) {
			send(w, M_COMMAND, SC_RESTORE, 0);
			return;
		}
		if (!track(w, HT_ICON, 0))
			open_system_menu(w, 0);
		return;
	case HT_CLIENT:
		if (w == root) {
			if (dbl)
				task_list();
			return;
		}
		if (w->style & S_TABSTOP)
			win_set_focus(w);
		client_point(w, e->x, e->y, &cx, &cy);
		capture = w;
		capture_button = B_LEFT;
		send(w, dbl ? M_LDBL : M_LDOWN, cx, cy);
		return;
	case HT_BORDER:
		return;
	default:
		if (ht >= HT_LEFT && w->state == W_NORMAL)
			track(w, ht, 0);
		return;
	}
}

static void mouse_up(Event *e)
{
	int cx, cy;
	Win *w = capture;

	if (!w || e->button != capture_button)
		return;
	capture = 0;
	client_point(w, e->x, e->y, &cx, &cy);
	send(w, e->button == B_LEFT ? M_LUP : M_RUP, cx, cy);
}

static void mouse_move(void)
{
	int ht, cx, cy, shape;
	Win *w;

	if (capture) {
		client_point(capture, ptr_x, ptr_y, &cx, &cy);
		send(capture, M_MOVE, cx, cy);
		return;
	}
	w = find_hit(root, ptr_x, ptr_y, &ht);
	shape = PTR_ARROW;
	if (modal && !is_child_of(w, modal))
		;
	else if (ht >= HT_LEFT && w->state == W_NORMAL)
		shape = size_pointer(ht);
	else if (ht == HT_CLIENT && w != root) {
		client_point(w, ptr_x, ptr_y, &cx, &cy);
		shape = send(w, M_CURSOR, cx, cy);
		if (shape <= 0)
			shape = PTR_ARROW;
	}
	set_pointer(shape);
}

static void switch_next(void)
{
	Win *c, *bottom = 0;

	if (modal)
		return;
	for (c = root->child; c; c = c->next)
		if (!(c->style & S_HIDDEN) && c != active) {
			bottom = c;
			break;
		}
	if (bottom) {
		if (bottom->state == W_MIN)
			win_set_state(bottom, bottom->was_max ? W_MAX : W_NORMAL);
		win_activate(bottom);
	}
}

static void key(int k)
{
	Win *f, *top = active;

	if (k == (K_ALT | K_TAB)) {
		switch_next();
		return;
	}
	if (k == K_START) {
		if (!modal)
			task_list();
		return;
	}
	if (top && k == (K_ALT | (K_F1 + 3))) {
		send(top, M_COMMAND, SC_CLOSE, 0);
		return;
	}
	if (top && k == (K_ALT | ' ')) {
		open_system_menu(top, 1);
		return;
	}
	if (top && k == (K_ALT | '-') && top->active) {
		open_system_menu(top->active, 1);
		return;
	}
	if (top && top->menu && !modal && (k == (K_F1 + 9) || ((k & K_ALT) && (k & 0xFF) > ' '))) {
		int i, c = k & 0xFF;
		if (k == K_F1 + 9) {
			open_menu(top, 0, 1);
			return;
		}
		for (i = 0; i < top->menu->n; i++)
			if (mnemonic(top->menu->menus[i].title) == c) {
				open_menu(top, i, 1);
				return;
			}
	}
	for (f = win_focus(); f; f = f->parent)
		if (send(f, M_KEY, k, 0))
			return;
}

static void dispatch(Event *e)
{
	switch (e->type) {
	case E_MOVE: mouse_move(); break;
	case E_DOWN: mouse_down(e); break;
	case E_UP: mouse_up(e); break;
	case E_KEY: key(e->key); break;
	}
}

void pump(int do_idle)
{
	Event e;

	if (get_event(&e, do_idle, -1))
		dispatch(&e);
}

void run(void)
{
	while (!quit)
		pump(1);
}

int dialog_run(Win *dlg)
{
	Win *old_modal = modal, *owner = dlg->owner;
	int result;

	modal = dlg;
	depth++;
	win_activate(dlg);
	while (!dlg->done && !quit && !dlg->dead)
		pump(1);
	depth--;
	modal = old_modal;
	result = dlg->result;
	win_destroy(dlg);
	if (owner && !owner->dead)
		win_activate(win_top(owner));
	return result;
}

void dialog_end(Win *dlg, int result)
{
	dlg->result = result;
	dlg->done = 1;
}

/* ---------------------------------------------------------------- */
/* Default behaviour                                                 */
/* ---------------------------------------------------------------- */

static Win *next_tab(Win *dlg, Win *from, int back)
{
	Win *c, *list[40];
	int n = 0, cur = -1;

	for (c = dlg->child; c && n < 40; c = c->next)
		if ((c->style & S_TABSTOP) && !(c->style & (S_HIDDEN | S_DISABLED))) {
			if (c == from)
				cur = n;
			list[n++] = c;
		}
	if (!n)
		return 0;
	if (cur < 0)
		return list[0];
	return list[(cur + (back ? n - 1 : 1)) % n];
}

static Win *default_button(Win *dlg)
{
	Win *c;

	for (c = dlg->child; c; c = c->next)
		if ((c->style & S_DEFAULT) && !(c->style & (S_HIDDEN | S_DISABLED)))
			return c;
	return 0;
}

int def_proc(Win *w, int msg, int a, int b)
{
	switch (msg) {
	case M_CLOSE:
		if (w->style & S_DIALOG)
			send(w, M_COMMAND, ID_CANCEL, 0);
		else
			win_destroy(w);
		return 1;
	case M_COMMAND:
		switch (a) {
		case SC_RESTORE:
			win_set_state(w, (w->state == W_MIN && w->was_max) ? W_MAX : W_NORMAL);
			win_activate(w);
			return 1;
		case SC_MINIMIZE: win_set_state(w, W_MIN); return 1;
		case SC_MAXIMIZE: win_set_state(w, W_MAX); win_activate(w); return 1;
		case SC_CLOSE: send(w, M_CLOSE, 0, 0); return 1;
		case SC_MOVE: track(w, w->state == W_MIN ? HT_ICON : HT_CAPTION, 1); return 1;
		case SC_SIZE: track(w, HT_BOTTOMRIGHT, 1); return 1;
		case SC_SWITCH: task_list(); return 1;
		case SC_NEXT:
			if (w->parent && w->parent->child && w->parent->child != w) {
				Win *c = w->parent->child;
				while (c && (!(c->style & S_MDI) || c == w))
					c = c->next;
				if (c)
					win_activate(c);
			}
			return 1;
		}
		if ((w->style & S_DIALOG) && (a == ID_OK || a == ID_CANCEL)) {
			dialog_end(w, a);
			return 1;
		}
		return 0;
	case M_KEY:
		if (w->style & S_DIALOG) {
			Win *f = win_focus(), *d;
			if (a == K_TAB || a == (K_ALT | K_TAB)) {
				Win *n = next_tab(w, f, 0);
				if (n)
					win_set_focus(n);
				return 1;
			}
			if (a == K_ENTER) {
				d = (f && f != w && f->proc != def_proc && (f->style & S_TABSTOP) &&
				     send(f, M_CLICK, 1, 0)) ? 0 : default_button(w);
				if (d)
					send(d, M_CLICK, 0, 0);
				else if (!f || f == w)
					send(w, M_COMMAND, ID_OK, 0);
				return 1;
			}
			if (a == K_ESC) {
				send(w, M_COMMAND, ID_CANCEL, 0);
				return 1;
			}
		}
		if ((w->style & S_MDI) && a == (K_CTRL | (K_F1 + 3))) {
			send(w, M_COMMAND, SC_CLOSE, 0);
			return 1;
		}
		return 0;
	case M_CURSOR:
		return PTR_ARROW;
	}
	return 0;
}

static int root_proc(Win *w, int msg, int a, int b)
{
	if (msg == M_PAINT) {
		fill(0, 0, SCR_W, SCR_H, col_desktop);
		return 1;
	}
	return 0;
}

/* ---------------------------------------------------------------- */
/* Arranging windows                                                 */
/* ---------------------------------------------------------------- */

static int arrangeable(Win *c)
{
	return !(c->style & (S_HIDDEN | S_CHILD | S_DIALOG)) && c->state != W_MIN &&
	       ((c->style & S_THICK) || c->parent != root);
}

void cascade(Win *p)
{
	Win *c;
	Rect cl;
	int n = 0, x = 0, y = 0, w, h;

	win_client(p, &cl);
	for (c = p->child; c; c = c->next)
		if (arrangeable(c))
			n++;
	w = cl.r * 3 / 4;
	h = cl.b * 3 / 4;
	if (w < 120) w = cl.r;
	if (h < 80) h = cl.b;
	for (c = p->child; c; c = c->next) {
		if (!arrangeable(c))
			continue;
		if (c->state == W_MAX)
			win_set_state(c, W_NORMAL);
		win_move(c, x, y, w, h);
		x += CAP_H + 2;
		y += CAP_H + 2;
		if (x + w > cl.r || y + h > cl.b)
			x = y = 0;
	}
}

void tile(Win *p)
{
	Win *c;
	Rect cl;
	int n = 0, i = 0, cols, rows, w, h, bottom = 0;

	win_client(p, &cl);
	for (c = p->child; c; c = c->next) {
		if (arrangeable(c))
			n++;
		else if (c->state == W_MIN && !(c->style & S_HIDDEN))
			bottom = ICON_H;
	}
	if (!n)
		return;
	cols = 1;
	while (cols * cols < n)
		cols++;
	rows = (n + cols - 1) / cols;
	w = cl.r / cols;
	h = (cl.b - bottom) / rows;
	for (c = p->child; c; c = c->next) {
		if (!arrangeable(c))
			continue;
		if (c->state == W_MAX)
			win_set_state(c, W_NORMAL);
		win_move(c, (i % cols) * w, (i / cols) * h, w, h);
		i++;
	}
}

void arrange_icons(Win *p)
{
	Win *c;

	for (c = p->child; c; c = c->next)
		if (c->state == W_MIN) {
			invalidate(c);
			c->ix = -1;
			c->iy = -10000;		/* out of the way while placing */
		}
	for (c = p->child; c; c = c->next)
		if (c->state == W_MIN) {
			place_icon(c);
			invalidate(c);
		}
}

/* ---------------------------------------------------------------- */
/* Start up                                                          */
/* ---------------------------------------------------------------- */

void wm_init(void)
{
	root = calloc(1, sizeof *root);
	set_rect(&root->r, 0, 0, SCR_W, SCR_H);
	root->bg = -1;
	root->proc = root_proc;
	strcpy(root->text, "Desktop");
	invalidate(root);
	last_tick = hw_frames();
	set_pointer(PTR_ARROW);
}

/* ---------------------------------------------------------------- */
/* Task List (Ctrl+Esc, or double click on the desktop)              */
/* ---------------------------------------------------------------- */

#define TL_LIST		100
#define TL_SWITCH	101
#define TL_END		102
#define TL_CASCADE	103
#define TL_TILE		104
#define TL_ARRANGE	105

static int tasklist_proc(Win *w, int msg, int a, int b)
{
	if (msg == M_COMMAND) {
		Win *list = ctl_find(w, TL_LIST);
		switch (a) {
		case TL_LIST:
			if (b == N_DBLCLK)
				dialog_end(w, TL_SWITCH);
			return 1;
		case TL_SWITCH: case TL_END:
			if (list_sel(list) >= 0)
				dialog_end(w, a);
			return 1;
		case ID_OK:
			dialog_end(w, TL_SWITCH);
			return 1;
		case TL_CASCADE: case TL_TILE: case TL_ARRANGE:
			dialog_end(w, a);
			return 1;
		}
	}
	return def_proc(w, msg, a, b);
}

void task_list(void)
{
	Win *tasks[32], *c, *d, *list, *t;
	int n = 0, i, r, sel;

	if (modal)
		return;
	for (c = root->child; c; c = c->next)
		if (!(c->style & (S_HIDDEN | S_DIALOG)) && n < 32)
			tasks[n++] = c;
	d = dialog_create(0, 272, 104, "Task List", tasklist_proc, 0);
	list = ctl_list(d, 6, 6, 260, 52, TL_LIST);
	for (i = n - 1; i >= 0; i--)		/* the top window first */
		list_add(list, tasks[i]->text, 0);
	list_set_sel(list, 0);
	ctl_button(d, 6, 64, 84, 14, "&Switch To", TL_SWITCH, 1);
	ctl_button(d, 94, 64, 84, 14, "&End Task", TL_END, 0);
	ctl_button(d, 182, 64, 84, 14, "Cancel", ID_CANCEL, 0);
	ctl_button(d, 6, 84, 84, 14, "C&ascade", TL_CASCADE, 0);
	ctl_button(d, 94, 84, 84, 14, "&Tile", TL_TILE, 0);
	ctl_button(d, 182, 84, 84, 14, "Arrange &Icons", TL_ARRANGE, 0);
	win_set_focus(list);
	r = dialog_run(d);
	sel = n - 1 - list_sel(list);	/* (the list is freed later) */
	t = (sel >= 0 && sel < n) ? tasks[sel] : 0;
	switch (r) {
	case TL_SWITCH:
		if (t && !t->dead) {
			if (t->state == W_MIN)
				win_set_state(t, t->was_max ? W_MAX : W_NORMAL);
			win_activate(t);
		}
		break;
	case TL_END:
		if (t && !t->dead)
			send(t, M_CLOSE, 0, 0);
		break;
	case TL_CASCADE: cascade(root); break;
	case TL_TILE: tile(root); break;
	case TL_ARRANGE: arrange_icons(root); break;
	}
}
