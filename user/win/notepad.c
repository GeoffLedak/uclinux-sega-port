/*
 * notepad.c -- Notepad, a plain text editor (and the Read Me viewer)
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include "win.h"

#define MAX_TEXT	32000
#define NP_LINE_H	9
#define NSIG		(SCR_H / NP_LINE_H + 2)
#define TABW		8

typedef struct {
	char *t;			/* the text */
	int len, cap;
	int *ls, nls, lscap;		/* line starts */
	int cur, anchor;		/* caret, other end of the selection */
	int top, left;			/* first line and column shown */
	int want;			/* column to keep on up/down */
	char path[256];
	char modified, caret, dragging, readonly;
	char find[64];
	int ver;			/* counts changes to the text */
	int maxw, maxw_ver;		/* the longest line, at text version maxw_ver */
	/* what each row of the window shows (the caret aside), as a
	 * checksum, so a change redraws only the rows that look different;
	 * and what they were worked out from */
	u32 sig[NSIG];
	int sig_ok, sig_top, sig_left, sig_ver, sig_cur, sig_anchor;
	int sig_cl, sig_cc;		/* the caret: line, column (-1: none) */
} Note;

static char *clipboard;
static int clip_len;

void clip_set(const char *s, int n)
{
	free(clipboard);
	clipboard = malloc(n + 1);
	clip_len = clipboard ? n : 0;
	if (clipboard) {
		memcpy(clipboard, s, n);
		clipboard[n] = 0;
	}
}

const char *clip_get(int *n)
{
	*n = clip_len;
	return clipboard;
}

enum {
	NM_NEW = 1, NM_OPEN, NM_SAVE, NM_SAVEAS, NM_EXIT, NM_CUT, NM_COPY,
	NM_PASTE, NM_DELETE, NM_SELALL, NM_TIME, NM_FIND, NM_FINDNEXT, NM_ABOUT
};

static MenuItem file_items[] = {
	{ "&New", NM_NEW, 0 },
	{ "&Open...", NM_OPEN, 0 },
	{ "&Save", NM_SAVE, 0 },
	{ "Save &As...", NM_SAVEAS, 0 },
	{ 0, 0, 0 },
	{ "E&xit", NM_EXIT, 0 },
};
static MenuItem edit_items[] = {
	{ "Cu&t\tCtrl+X", NM_CUT, 0 },
	{ "&Copy\tCtrl+C", NM_COPY, 0 },
	{ "&Paste\tCtrl+V", NM_PASTE, 0 },
	{ "De&lete\tDel", NM_DELETE, 0 },
	{ 0, 0, 0 },
	{ "Select &All", NM_SELALL, 0 },
	{ "Time/&Date\tF5", NM_TIME, 0 },
};
static MenuItem search_items[] = {
	{ "&Find...", NM_FIND, 0 },
	{ "Find &Next\tF3", NM_FINDNEXT, 0 },
};
static MenuItem help_items[] = {
	{ "&About Notepad...", NM_ABOUT, 0 },
};
static Menu np_menus[] = {
	{ "&File", file_items, COUNT(file_items) },
	{ "&Edit", edit_items, COUNT(edit_items) },
	{ "&Search", search_items, COUNT(search_items) },
	{ "&Help", help_items, COUNT(help_items) },
};
static MenuBar np_bar = { np_menus, COUNT(np_menus) };

/* ---------------- the text ---------------- */

static void relines(Note *n)
{
	int i;

	n->nls = 0;
	for (i = 0; ; i++) {
		if (n->nls == n->lscap) {
			int cap = n->lscap ? n->lscap * 2 : 64;
			int *p = realloc(n->ls, cap * sizeof *p);
			if (!p)
				return;
			n->ls = p;
			n->lscap = cap;
		}
		if (i == 0 || n->t[i - 1] == '\n')
			n->ls[n->nls++] = i;
		if (i >= n->len)
			break;
	}
}

static int line_of(Note *n, int off)
{
	int lo = 0, hi = n->nls - 1, mid;

	while (lo < hi) {
		mid = (lo + hi + 1) / 2;
		if (n->ls[mid] <= off)
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

static int line_end(Note *n, int l)
{
	int e = (l + 1 < n->nls) ? n->ls[l + 1] - 1 : n->len;
	return e;
}

static int col_of(Note *n, int off)
{
	int l = line_of(n, off), i, col = 0;

	for (i = n->ls[l]; i < off; i++)
		col = (n->t[i] == '\t') ? (col / TABW + 1) * TABW : col + 1;
	return col;
}

static int off_at(Note *n, int l, int col)
{
	int i, c = 0, e;

	if (l < 0) l = 0;
	if (l >= n->nls) l = n->nls - 1;
	e = line_end(n, l);
	for (i = n->ls[l]; i < e; i++) {
		int nc = (n->t[i] == '\t') ? (c / TABW + 1) * TABW : c + 1;
		if (nc > col)
			break;
		c = nc;
	}
	return i;
}

static int sel_range(Note *n, int *a, int *b)
{
	*a = n->cur < n->anchor ? n->cur : n->anchor;
	*b = n->cur < n->anchor ? n->anchor : n->cur;
	return *a != *b;
}

static void replace(Note *n, int a, int b, const char *s, int len)
{
	if (n->len - (b - a) + len > MAX_TEXT) {
		beep();
		return;
	}
	if (n->len - (b - a) + len + 1 > n->cap) {
		int cap = n->len - (b - a) + len + 1024;
		char *p = realloc(n->t, cap);
		if (!p) {
			beep();
			return;
		}
		n->t = p;
		n->cap = cap;
	}
	memmove(n->t + a + len, n->t + b, n->len - b);
	memcpy(n->t + a, s, len);
	n->len += len - (b - a);
	n->cur = n->anchor = a + len;
	n->modified = 1;
	n->ver++;
	relines(n);
}

static void set_title(Win *w, Note *n)
{
	char buf[60];
	const char *base = strrchr(n->path, '/');

	sprintf(buf, "Notepad - %.28s", n->path[0] ? (base ? base + 1 : n->path) : "(Untitled)");
	win_set_text(w, buf);
}

/* ---------------- view ---------------- */

static void view_size(Win *w, int *rows, int *cols)
{
	Rect c;

	win_client(w, &c);
	*rows = c.b > 0 ? UDIV16(c.b, NP_LINE_H) : 0;
	*cols = c.r > 2 ? UDIV16(c.r - 2, FIXED_W) : 0;
	if (*rows < 1) *rows = 1;
	if (*cols < 1) *cols = 1;
}

static void update_scroll(Win *w, Note *n)
{
	int rows, cols, i;

	view_size(w, &rows, &cols);
	if (n->maxw_ver != n->ver || !n->maxw) {
		n->maxw = 0;
		for (i = 0; i < n->nls; i++) {
			int l = line_end(n, i) - n->ls[i];
			if (l > n->maxw)
				n->maxw = l;
		}
		n->maxw_ver = n->ver;
	}
	set_scroll(w, 1, n->top, n->nls - rows, rows);
	set_scroll(w, 0, n->left, n->maxw + 1 - cols, cols);
}

#define VIS_COLS	(SCR_W / FIXED_W + 2)

/* columns that can be seen */
static int vis_cols(Win *w)
{
	Rect c;
	int vis;

	win_client(w, &c);
	vis = (c.r > 1 ? UDIV16(c.r - 1, FIXED_W) : 0) + 2;
	return vis > VIS_COLS ? VIS_COLS : vis;
}

/* line l as shown: vis characters (tabs as spaces) in buf, and in fl
 * 0 past the end, 1 text, 2 selected */
static void line_cells(Note *n, int l, int vis, char *buf, char *fl)
{
	int a, b, sel = sel_range(n, &a, &b), e = line_end(n, l), col = 0, i, k;

	memset(buf, ' ', vis);
	memset(fl, 0, vis);
	for (i = n->ls[l]; i <= e && i <= n->len && col < n->left + vis; i++) {
		int ch = (i < e) ? (unsigned char)n->t[i] : ' ';
		int nc = (ch == '\t') ? (col / TABW + 1) * TABW : col + 1;
		int insel = sel && i >= a && i < b && i < e + (e < n->len);
		for (k = col; k < nc; k++)
			if (k >= n->left && k < n->left + vis) {
				if (ch != '\t')
					buf[k - n->left] = ch;
				fl[k - n->left] = insel ? 2 : 1;
			}
		col = nc;
		if (i == e)
			break;
	}
}

/* the column of the caret in the window, if it is drawn; else -1 */
static int caret_col(Win *w, Note *n, int *line)
{
	int a, b, rows, cols, i;

	if (!n->caret || win_focus() != w || sel_range(n, &a, &b))
		return -1;
	view_size(w, &rows, &cols);
	*line = line_of(n, n->cur);
	i = col_of(n, n->cur) - n->left;
	if (*line >= n->top && *line < n->top + rows && i >= 0 && i <= cols)
		return i;
	return -1;
}

/*
 * A checksum of everything that decides how row l looks: the bytes of
 * the line as far as they can be seen (each is at least one column),
 * whether it ends there, the part of it that is selected and the first
 * column shown. Laying the line out would be slower. (The caret is
 * kept apart: it blinks, and moves without changing anything else.)
 */
#define MIX(h, v)	(h = ((h << 5) + h) ^ (u32)(v))

static u32 row_sig(Note *n, int l, int vis, int sel, int a, int b)
{
	u32 h = 0x811c9dc5;
	int s, e, end, i, sb;
	const unsigned char *p;

	if (l >= n->nls)
		return 0;
	s = n->ls[l];
	e = line_end(n, l);
	end = (e - s > n->left + vis) ? s + n->left + vis : e;
	p = (const unsigned char *)n->t + s;
	for (i = end - s; i > 0; i--)
		MIX(h, *p++);
	MIX(h, end - s);
	MIX(h, n->left);
	sb = e + (e < n->len);			/* the newline can be selected too */
	if (b < sb)
		sb = b;
	if (sel && sb > a && sb > s) {
		MIX(h, (a > s ? a : s) - s);
		MIX(h, sb - s);
	}
	return h ? h : 1;
}

static int sig_rows(Win *w)
{
	Rect c;
	int vrows;

	win_client(w, &c);
	vrows = c.b > 0 ? UDIV16(c.b + NP_LINE_H - 1, NP_LINE_H) : 0;
	return vrows > NSIG ? NSIG : vrows;
}

static void note_state(Note *n, int cl, int cc)
{
	n->sig_top = n->top;
	n->sig_left = n->left;
	n->sig_ver = n->ver;
	n->sig_cur = n->cur;
	n->sig_anchor = n->anchor;
	n->sig_cl = cl;
	n->sig_cc = cc;
	n->sig_ok = 1;
}

/* what the rows show now */
static void take_sigs(Win *w, Note *n)
{
	int i, vrows = sig_rows(w), vis = vis_cols(w), cl = -1, cc = caret_col(w, n, &cl);
	int a, b, sel = sel_range(n, &a, &b);

	for (i = 0; i < vrows; i++)
		n->sig[i] = row_sig(n, n->top + i, vis, sel, a, b);
	note_state(n, cl, cc);
}

/* redraw the caret's column of pixels at line l, column c */
static void inval_caret(Win *w, Note *n, const Rect *cs, int l, int c)
{
	Rect r;
	int x;

	if (c < 0 || l < n->top || l - n->top >= sig_rows(w))
		return;
	x = cs->l + c * FIXED_W + (c == 0);
	set_rect(&r, x, cs->t + (l - n->top) * NP_LINE_H, x + 1, cs->t + (l - n->top + 1) * NP_LINE_H);
	if (rect_and(&r, cs))
		invalidate_screen(&r);
}

/* redraw what changed since the last time */
static void refresh(Win *w, Note *n)
{
	Rect cs, r;
	int vrows, vis, i, d, cl = -1, cc, a, b, sel;
	u32 old[NSIG], sg;

	win_client_screen(w, &cs);
	vrows = sig_rows(w);
	vis = vis_cols(w);
	cc = caret_col(w, n, &cl);
	sel = sel_range(n, &a, &b);
	d = n->top - n->sig_top;
	if (!n->sig_ok || d >= vrows || d <= -vrows) {
		invalidate_screen(&cs);
		take_sigs(w, n);
		return;
	}
	if (!d && n->ver == n->sig_ver && n->left == n->sig_left &&
	    ((n->cur == n->sig_cur && n->anchor == n->sig_anchor) ||
	     (n->cur == n->anchor && n->sig_cur == n->sig_anchor))) {
		/* the same text and selection, so only the caret can differ */
		if (cl != n->sig_cl || cc != n->sig_cc) {
			inval_caret(w, n, &cs, n->sig_cl, n->sig_cc);
			inval_caret(w, n, &cs, cl, cc);
		}
		note_state(n, cl, cc);
		return;
	}
	if (d) {
		/* move the rows that are still in view */
		scroll_client(w, 0, -d * NP_LINE_H);
		memcpy(old, n->sig, sizeof old);
		for (i = 0; i < vrows; i++)
			n->sig[i] = (i + d >= 0 && i + d < vrows) ? old[i + d] : 0xFFFFFFFF;
		n->sig_top = n->top;
	}
	for (i = 0; i < vrows; i++) {
		sg = row_sig(n, n->top + i, vis, sel, a, b);
		if (sg != n->sig[i]) {
			n->sig[i] = sg;
			set_rect(&r, cs.l, cs.t + i * NP_LINE_H, cs.r, cs.t + (i + 1) * NP_LINE_H);
			if (r.b > cs.b)
				r.b = cs.b;
			invalidate_screen(&r);
		}
	}
	if (cl != n->sig_cl || cc != n->sig_cc) {
		/* (its old place has moved with the rows, if they moved) */
		inval_caret(w, n, &cs, n->sig_cl, n->sig_cc);
		inval_caret(w, n, &cs, cl, cc);
	}
	note_state(n, cl, cc);
}

static void show_caret(Win *w, Note *n)
{
	int rows, cols, l = line_of(n, n->cur), c = col_of(n, n->cur);

	view_size(w, &rows, &cols);
	if (l < n->top) n->top = l;
	if (l >= n->top + rows) n->top = l - rows + 1;
	if (c < n->left) n->left = c > 4 ? c - 4 : 0;
	if (c >= n->left + cols) n->left = c - cols + 4;
	update_scroll(w, n);
	refresh(w, n);
}

/* draw one run of a line: cols columns of buf from column x0 (window x) */
static void paint_run(const char *buf, int n, int x, int y, int sel)
{
	if (sel)
		fill(x < 1 ? 1 : x, y, x + n * FIXED_W, y + NP_LINE_H, col_hilite);
	textn(x, y + 1, buf, n, sel ? C_WHITE : C_BLACK, F_FIXED);
}

static int note_paint(Win *w, Note *n)
{
	Rect c;
	int l, y, i, k, start, vis;
	char buf[VIS_COLS], fl[VIS_COLS];

	win_client(w, &c);
	vis = vis_cols(w);
	for (l = n->top, y = 0; l < n->nls && y < c.b; l++, y += NP_LINE_H) {
		/* skip lines outside the clip */
		if (org_y + y + NP_LINE_H <= clip.t || org_y + y >= clip.b)
			continue;
		line_cells(n, l, vis, buf, fl);
		/* draw it in runs of the same selection */
		for (start = 0; start < vis && fl[start]; start = k) {
			for (k = start; k < vis && fl[k] == fl[start]; k++)
				;
			paint_run(buf + start, k - start, 1 + start * FIXED_W, y, fl[start] == 2);
		}
	}
	i = caret_col(w, n, &l);
	if (i >= 0)
		vline(1 + i * FIXED_W - 1 + (i == 0), (l - n->top) * NP_LINE_H,
		      (l - n->top + 1) * NP_LINE_H, C_BLACK);
	/* all of it drawn: now it's known what every row shows */
	if (clip.l <= org_x && clip.t <= org_y && clip.r >= org_x + c.r && clip.b >= org_y + c.b)
		take_sigs(w, n);
	return 1;
}

static int offset_at_point(Note *n, int x, int y)
{
	int l = n->top + (y < 0 ? -1 : UDIV16(y, NP_LINE_H));
	int cx = x + FIXED_W / 2 - 1;
	int col = n->left + (cx < 0 ? -UDIV16(-cx, FIXED_W) : UDIV16(cx, FIXED_W));

	if (col < 0)
		col = 0;
	if (l >= n->nls)
		return n->len;
	if (l < 0)
		return 0;
	return off_at(n, l, col);
}

/* ---------------- files ---------------- */

static int ask_save(Win *w, Note *n);

static int load(Win *w, Note *n, const char *path)
{
	int fd = open(path, O_RDONLY), r, total = 0, i, j;
	char *buf;

	if (fd < 0) {
		msgbox(w, "Notepad", "Cannot open this file.", MB_OK | MB_EXCLAIM);
		return 0;
	}
	buf = malloc(MAX_TEXT + 1);
	if (!buf) {
		close(fd);
		msgbox(w, "Notepad", "Not enough memory.", MB_OK | MB_STOP);
		return 0;
	}
	while ((r = read(fd, buf + total, MAX_TEXT + 1 - total)) > 0)
		total += r;
	close(fd);
	if (total > MAX_TEXT) {
		free(buf);
		msgbox(w, "Notepad", "This file is too large for Notepad.", MB_OK | MB_EXCLAIM);
		return 0;
	}
	/* drop carriage returns and other control characters */
	for (i = j = 0; i < total; i++) {
		unsigned char ch = buf[i];
		if (ch == '\r' || (ch < 32 && ch != '\n' && ch != '\t'))
			continue;
		buf[j++] = ch;
	}
	free(n->t);
	n->t = buf;
	n->cap = MAX_TEXT + 1;
	n->len = j;
	n->cur = n->anchor = n->top = n->left = 0;
	n->modified = 0;
	n->ver++;
	strncpy(n->path, path, sizeof n->path - 1);
	relines(n);
	set_title(w, n);
	update_scroll(w, n);
	n->sig_ok = 0;
	refresh(w, n);
	return 1;
}

static int save(Win *w, Note *n, int as)
{
	char path[256];
	int fd;

	if (as || !n->path[0]) {
		strcpy(path, n->path[0] ? n->path : "/tmp/untitled.txt");
		if (!file_dialog(w, "Save As", path, 1))
			return 0;
	} else
		strcpy(path, n->path);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || write(fd, n->t, n->len) != n->len) {
		if (fd >= 0)
			close(fd);
		msgbox(w, "Notepad", "Cannot write this file.\n(The ROM file system is read-only:\ntry /tmp.)",
		       MB_OK | MB_EXCLAIM);
		return 0;
	}
	close(fd);
	strcpy(n->path, path);
	n->modified = 0;
	n->readonly = 0;
	set_title(w, n);
	return 1;
}

/* 1 = go ahead, 0 = cancelled */
static int ask_save(Win *w, Note *n)
{
	char msg[120];
	const char *base = strrchr(n->path, '/');
	int r;

	if (!n->modified)
		return 1;
	sprintf(msg, "The text in %.40s has changed.\n\nDo you want to save the changes?",
		n->path[0] ? (base ? base + 1 : n->path) : "(Untitled)");
	r = msgbox(w, "Notepad", msg, MB_YESNOCANCEL | MB_EXCLAIM);
	if (r == ID_CANCEL)
		return 0;
	if (r == ID_YES)
		return save(w, n, 0);
	return 1;
}

/* ---------------- editing ---------------- */

static void copy_sel(Note *n)
{
	int a, b;

	if (sel_range(n, &a, &b))
		clip_set(n->t + a, b - a);
}

static void find_next(Win *w, Note *n)
{
	int i, k, fl = strlen(n->find), a, b;
	char msg[100];

	if (!fl)
		return;
	sel_range(n, &a, &b);
	for (i = b; i + fl <= n->len; i++) {
		for (k = 0; k < fl; k++) {
			int x = (unsigned char)n->t[i + k], y = (unsigned char)n->find[k];
			if (x >= 'A' && x <= 'Z') x += 32;
			if (y >= 'A' && y <= 'Z') y += 32;
			if (x != y)
				break;
		}
		if (k == fl) {
			n->anchor = i;
			n->cur = i + fl;
			show_caret(w, n);
			return;
		}
	}
	sprintf(msg, "Cannot find \"%.40s\"", n->find);
	msgbox(w, "Notepad", msg, MB_OK | MB_INFO);
}

static void insert_time(Win *w, Note *n)
{
	time_t t = time(0);
	struct tm *tm = localtime(&t);
	char buf[40];
	int a, b;

	sprintf(buf, "%d:%02d %s %d/%d/%04d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12,
		tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM",
		tm->tm_mon + 1, tm->tm_mday, tm->tm_year + 1900);
	sel_range(n, &a, &b);
	replace(n, a, b, buf, strlen(buf));
}

static int note_key(Win *w, Note *n, int k)
{
	int a, b, l, rows, cols, has = sel_range(n, &a, &b);
	char ch;

	view_size(w, &rows, &cols);
	if (n->readonly && ((k >= 32 && k < 127) || k == K_ENTER || k == K_TAB ||
			    k == K_BS || k == K_DEL || k == CTRL('x') || k == CTRL('v'))) {
		beep();
		return 1;
	}
	switch (k) {
	case K_LEFT:
		n->cur = has ? a : (n->cur > 0 ? n->cur - 1 : 0);
		n->anchor = n->cur;
		n->want = -1;
		break;
	case K_RIGHT:
		n->cur = has ? b : (n->cur < n->len ? n->cur + 1 : n->len);
		n->anchor = n->cur;
		n->want = -1;
		break;
	case K_UP: case K_DOWN: case K_PGUP: case K_PGDN:
		if (n->want < 0)
			n->want = col_of(n, n->cur);
		l = line_of(n, n->cur);
		l += (k == K_UP) ? -1 : (k == K_DOWN) ? 1 : (k == K_PGUP) ? -(rows - 1) : rows - 1;
		if (k == K_PGUP) { n->top -= rows - 1; if (n->top < 0) n->top = 0; }
		if (k == K_PGDN) { n->top += rows - 1; if (n->top > n->nls - rows) n->top = n->nls - rows; if (n->top < 0) n->top = 0; }
		if (l < 0) l = 0;
		if (l >= n->nls) l = n->nls - 1;
		n->cur = n->anchor = off_at(n, l, n->want);
		break;
	case K_HOME:
		n->cur = n->anchor = n->ls[line_of(n, n->cur)];
		n->want = -1;
		break;
	case K_END:
		n->cur = n->anchor = line_end(n, line_of(n, n->cur));
		n->want = -1;
		break;
	case K_CTRL | K_HOME: case K_CTRL | K_UP:
		n->cur = n->anchor = 0;
		break;
	case K_CTRL | K_END: case K_CTRL | K_DOWN:
		n->cur = n->anchor = n->len;
		break;
	case K_BS:
		if (has)
			replace(n, a, b, "", 0);
		else if (n->cur > 0)
			replace(n, n->cur - 1, n->cur, "", 0);
		n->want = -1;
		break;
	case K_DEL:
		if (has)
			replace(n, a, b, "", 0);
		else if (n->cur < n->len)
			replace(n, n->cur, n->cur + 1, "", 0);
		n->want = -1;
		break;
	case CTRL('x'):
		copy_sel(n);
		if (has)
			replace(n, a, b, "", 0);
		break;
	case CTRL('c'):
		copy_sel(n);
		return 1;
	case CTRL('v'):
		if (clipboard)
			replace(n, a, b, clipboard, clip_len);
		break;
	case K_F1 + 4:
		insert_time(w, n);
		break;
	case K_F1 + 2:
		find_next(w, n);
		return 1;
	default:
		if (k == K_ENTER)
			ch = '\n';
		else if (k == K_TAB)
			ch = '\t';
		else if (k >= 32 && k < 256)
			ch = k;
		else
			return 0;
		replace(n, a, b, &ch, 1);
		n->want = -1;
		break;
	}
	n->caret = 1;
	w->timer_left = w->timer;
	show_caret(w, n);
	return 1;
}

static int note_proc(Win *w, int msg, int a, int b)
{
	Note *n = w->data;
	int s, e;
	char path[256];

	switch (msg) {
	case M_PAINT:
		return note_paint(w, n);
	case M_TIMER:
		n->caret = !n->caret;
		refresh(w, n);
		return 1;
	case M_ACTIVATE:
		n->caret = 1;
		set_timer(w, a ? 30 : 0);
		refresh(w, n);
		return 1;
	case M_SIZE:
		n->sig_ok = 0;
		update_scroll(w, n);
		return 1;
	case M_CURSOR:
		return PTR_IBEAM;
	case M_LDOWN:
	case M_LDBL:
		n->cur = offset_at_point(n, a, b);
		if (msg == M_LDBL) {
			/* select the word */
			for (s = n->cur; s > 0 && n->t[s - 1] > ' '; s--)
				;
			for (e = n->cur; e < n->len && n->t[e] > ' '; e++)
				;
			n->anchor = s;
			n->cur = e;
		} else
			n->anchor = n->cur;
		n->dragging = 1;
		n->want = -1;
		refresh(w, n);
		return 1;
	case M_MOVE:
		if (n->dragging) {
			int old = n->cur;
			n->cur = offset_at_point(n, a, b);
			if (n->cur != old)
				show_caret(w, n);
		}
		return 1;
	case M_LUP:
		n->dragging = 0;
		return 1;
	case M_VSCROLL:
	case M_HSCROLL:
		{
			int rows, cols, *p = (msg == M_VSCROLL) ? &n->top : &n->left;
			int page;
			view_size(w, &rows, &cols);
			page = (msg == M_VSCROLL) ? rows : cols;
			switch (a) {
			case SB_LINEUP: (*p)--; break;
			case SB_LINEDOWN: (*p)++; break;
			case SB_PAGEUP: *p -= page; break;
			case SB_PAGEDOWN: *p += page; break;
			case SB_THUMB: *p = b; break;
			}
			if (*p > ((msg == M_VSCROLL) ? w->vmax : w->hmax))
				*p = (msg == M_VSCROLL) ? w->vmax : w->hmax;
			if (*p < 0)
				*p = 0;
			update_scroll(w, n);
			refresh(w, n);
		}
		return 1;
	case M_KEY:
		return note_key(w, n, a);
	case M_INITMENU:
		s = sel_range(n, &a, &b) ? 0 : MF_GRAY;
		edit_items[0].flags = edit_items[3].flags = n->readonly ? MF_GRAY : s;
		edit_items[1].flags = s;
		edit_items[2].flags = (clipboard && !n->readonly) ? 0 : MF_GRAY;
		edit_items[6].flags = n->readonly ? MF_GRAY : 0;
		search_items[1].flags = n->find[0] ? 0 : MF_GRAY;
		return 1;
	case M_COMMAND:
		switch (a) {
		case NM_NEW:
			if (ask_save(w, n)) {
				replace(n, 0, n->len, "", 0);
				n->path[0] = 0;
				n->modified = n->readonly = 0;
				n->top = n->left = 0;
				set_title(w, n);
				show_caret(w, n);
			}
			return 1;
		case NM_OPEN:
			if (!ask_save(w, n))
				return 1;
			strcpy(path, n->path[0] ? n->path : "/etc/");
			if (file_dialog(w, "Open", path, 0))
				load(w, n, path);
			n->readonly = 0;
			return 1;
		case NM_SAVE: save(w, n, 0); return 1;
		case NM_SAVEAS: save(w, n, 1); return 1;
		case NM_EXIT: send(w, M_CLOSE, 0, 0); return 1;
		case NM_CUT: note_key(w, n, CTRL('x')); return 1;
		case NM_COPY: note_key(w, n, CTRL('c')); return 1;
		case NM_PASTE: note_key(w, n, CTRL('v')); return 1;
		case NM_DELETE: note_key(w, n, K_DEL); return 1;
		case NM_SELALL:
			n->anchor = 0;
			n->cur = n->len;
			refresh(w, n);
			return 1;
		case NM_TIME: note_key(w, n, K_F1 + 4); return 1;
		case NM_FIND:
			if (input_box(w, "Find", "Find What:", n->find, sizeof n->find))
				find_next(w, n);
			return 1;
		case NM_FINDNEXT: find_next(w, n); return 1;
		case NM_ABOUT:
			msgbox(w, "About Notepad", "Notepad\nA plain text editor.", MB_OK | MB_INFO);
			return 1;
		}
		break;
	case M_CLOSE:
		if (ask_save(w, n))
			win_destroy(w);
		return 1;
	case M_DESTROY:
		free(n->t);
		free(n->ls);
		free(n);
		return 1;
	}
	return def_proc(w, msg, a, b);
}

static int next_x = 20, next_y = 16;

static Win *new_notepad(void)
{
	Note *n = calloc(1, sizeof *n);
	Win *w;

	if (!n)
		return 0;
	n->want = -1;
	n->caret = 1;
	relines(n);
	w = win_create(root, next_x, next_y, 250, 160, S_OVERLAPPED | S_VSCROLL | S_HSCROLL,
		       "Notepad - (Untitled)", note_proc, n);
	next_x = (next_x + 12) % 60;
	next_y = (next_y + 12) % 48;
	w->menu = &np_bar;
	w->icon = icon_notepad;
	update_scroll(w, n);
	set_timer(w, 30);
	return w;
}

void app_notepad(const char *file)
{
	Win *w = new_notepad();

	if (w && file)
		load(w, w->data, file);
}

void app_notepad_text(const char *title, const char *text)
{
	Win *w = new_notepad();
	Note *n;
	char buf[60];

	if (!w)
		return;
	n = w->data;
	replace(n, 0, 0, text, strlen(text));
	n->cur = n->anchor = 0;
	n->modified = 0;
	n->readonly = 1;
	sprintf(buf, "Notepad - %.28s", title);
	win_set_text(w, buf);
	win_move(w, 10, 8, 300, 200);
	update_scroll(w, n);
}
