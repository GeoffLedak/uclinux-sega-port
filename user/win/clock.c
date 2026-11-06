/*
 * clock.c -- Clock: analog or digital
 *
 * (The Genesis has no battery clock: set the time in Control Panel.)
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "win.h"

typedef struct {
	int digital, seconds, date;
	int last;			/* the second shown */
} Clock;

enum { CM_ANALOG = 1, CM_DIGITAL, CM_SECONDS, CM_DATE, CM_ABOUT };

static MenuItem set_items[] = {
	{ "&Analog", CM_ANALOG, 0 },
	{ "&Digital", CM_DIGITAL, 0 },
	{ 0, 0, 0 },
	{ "&Seconds", CM_SECONDS, 0 },
	{ "D&ate", CM_DATE, 0 },
	{ 0, 0, 0 },
	{ "A&bout Clock...", CM_ABOUT, 0 },
};
static Menu clock_menus[] = {
	{ "&Settings", set_items, COUNT(set_items) },
};
static MenuBar clock_bar = { clock_menus, COUNT(clock_menus) };

/* sin(6 * i degrees) * 1000, i = 0 - 15 */
static const short sin_q[16] = {
	0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995, 1000
};

static int isin(int i)			/* i in sixtieths of a turn */
{
	i %= 60;
	if (i < 0) i += 60;
	if (i <= 15) return sin_q[i];
	if (i <= 30) return sin_q[30 - i];
	if (i <= 45) return -sin_q[i - 30];
	return -sin_q[60 - i];
}

static int icos(int i)
{
	return isin(i + 15);
}

/* a hand: a kite shape from the centre */
static void hand(int cx, int cy, int pos, int len, int width, int c)
{
	int x = cx + isin(pos) * len / 1000, y = cy - icos(pos) * len / 1000;
	int sx = cx + isin(pos + 15) * width / 1000, sy = cy - icos(pos + 15) * width / 1000;
	int tx = cx - isin(pos + 15) * width / 1000, ty = cy + icos(pos + 15) * width / 1000;
	int bx = cx - isin(pos) * width / 1000, by = cy + icos(pos) * width / 1000;
	int k;

	/* filled by drawing lines from the tip to points along the base */
	for (k = 0; k <= 8; k++) {
		int px = sx + (tx - sx) * k / 8, py = sy + (ty - sy) * k / 8;
		line(x, y, px, py, c);
		line(bx, by, px, py, c);
	}
}

/* seven-segment digits for the digital clock */
static void seg_digit(int x, int y, int w, int h, int d, int c)
{
	static const unsigned char segs[10] = {
		0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
	};
	int s = segs[d], t = w / 5 + 1, m = y + h / 2;

	if (s & 0x01) fill(x + t, y, x + w - t, y + t, c);		/* top */
	if (s & 0x02) fill(x + w - t, y + t, x + w, m, c);		/* top right */
	if (s & 0x04) fill(x + w - t, m, x + w, y + h - t, c);		/* bottom right */
	if (s & 0x08) fill(x + t, y + h - t, x + w - t, y + h, c);	/* bottom */
	if (s & 0x10) fill(x, m, x + t, y + h - t, c);			/* bottom left */
	if (s & 0x20) fill(x, y + t, x + t, m, c);			/* top left */
	if (s & 0x40) fill(x + t, m - t / 2, x + w - t, m - t / 2 + t, c);	/* middle */
}

static void set_title(Win *w, Clock *c, struct tm *tm)
{
	char buf[40];

	if (w->state == W_MIN)
		sprintf(buf, "%d:%02d", tm->tm_hour, tm->tm_min);
	else if (c->date)
		sprintf(buf, "Clock - %d/%d/%02d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year % 100);
	else
		strcpy(buf, "Clock");
	if (strcmp(buf, w->text))
		win_set_text(w, buf);
}

static int clock_proc(Win *w, int msg, int a, int b)
{
	Clock *c = w->data;
	time_t t;
	struct tm *tm;
	Rect r;
	int cx, cy, rad, i;

	switch (msg) {
	case M_PAINT:
		t = time(0);
		tm = localtime(&t);
		win_client(w, &r);
		if (c->digital) {
			int n = c->seconds ? 6 : 4, dw, dh, x, k;
			int digs[6];
			digs[0] = tm->tm_hour / 10; digs[1] = tm->tm_hour % 10;
			digs[2] = tm->tm_min / 10; digs[3] = tm->tm_min % 10;
			digs[4] = tm->tm_sec / 10; digs[5] = tm->tm_sec % 10;
			dw = (r.r - 8) / (n + (n / 2 - 1) / 2 + 1);
			if (dw > 40) dw = 40;
			dh = dw * 2;
			if (dh > r.b - 8) { dh = r.b - 8; dw = dh / 2; }
			x = (r.r - (n * (dw + dw / 4) + (n / 2 - 1) * dw / 2)) / 2;
			for (k = 0; k < n; k++) {
				seg_digit(x, (r.b - dh) / 2, dw, dh, digs[k], C_TEAL);
				x += dw + dw / 4;
				if (k == 1 || k == 3) {
					if (k == 3 && n == 4)
						break;
					fill(x, r.b / 2 - dh / 4, x + dw / 5 + 1, r.b / 2 - dh / 4 + dw / 5 + 1, C_TEAL);
					fill(x, r.b / 2 + dh / 4 - dw / 5, x + dw / 5 + 1, r.b / 2 + dh / 4 + 1, C_TEAL);
					x += dw / 2;
				}
			}
			return 1;
		}
		cx = r.r / 2;
		cy = r.b / 2;
		rad = (r.r < r.b ? r.r : r.b) / 2 - 4;
		if (rad < 8)
			return 1;
		for (i = 0; i < 60; i++) {
			int x = cx + isin(i) * rad / 1000, y = cy - icos(i) * rad / 1000;
			if (i % 5 == 0)
				fill(x - 1, y - 1, x + 2, y + 2, C_TEAL);
			else if (rad > 30)
				pixel(x, y, C_BLACK);
		}
		hand(cx, cy, (tm->tm_hour % 12) * 5 + tm->tm_min / 12, rad * 55 / 100, rad / 12 + 1, C_TEAL);
		hand(cx, cy, tm->tm_min, rad * 85 / 100, rad / 14 + 1, C_TEAL);
		if (c->seconds)
			line(cx, cy, cx + isin(tm->tm_sec) * rad * 9 / 10000,
			     cy - icos(tm->tm_sec) * rad * 9 / 10000, C_BLACK);
		return 1;
	case M_TIMER:
		t = time(0);
		tm = localtime(&t);
		set_title(w, c, tm);
		if (w->state != W_MIN && (c->seconds || tm->tm_sec == 0 || tm->tm_min != c->last)) {
			c->last = tm->tm_min;
			invalidate_client(w, 0, 0, SCR_W, SCR_H);
		}
		return 1;
	case M_SIZE:
		t = time(0);
		set_title(w, c, localtime(&t));
		invalidate(w);
		return 1;
	case M_INITMENU:
		set_items[0].flags = c->digital ? 0 : MF_CHECK;
		set_items[1].flags = c->digital ? MF_CHECK : 0;
		set_items[3].flags = c->seconds ? MF_CHECK : 0;
		set_items[4].flags = c->date ? MF_CHECK : 0;
		return 1;
	case M_COMMAND:
		switch (a) {
		case CM_ANALOG: c->digital = 0; break;
		case CM_DIGITAL: c->digital = 1; break;
		case CM_SECONDS: c->seconds = !c->seconds; break;
		case CM_DATE: c->date = !c->date; break;
		case CM_ABOUT:
			msgbox(w, "About Clock", "Clock\n\nThe Genesis has no battery clock:\n"
			       "set the date and time in Control Panel.", MB_OK | MB_INFO);
			return 1;
		default:
			return def_proc(w, msg, a, b);
		}
		t = time(0);
		set_title(w, c, localtime(&t));
		invalidate(w);
		return 1;
	case M_DESTROY:
		free(c);
		return 1;
	}
	return def_proc(w, msg, a, b);
}

void app_clock(void)
{
	Clock *c = calloc(1, sizeof *c);
	Win *w;

	if (!c)
		return;
	c->seconds = 1;
	w = win_create(root, 196, 28, 112, 124, S_OVERLAPPED, "Clock", clock_proc, c);
	w->menu = &clock_bar;
	w->icon = icon_clock;
	set_timer(w, 60);
}
