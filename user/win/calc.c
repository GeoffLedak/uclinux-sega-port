/*
 * calc.c -- Calculator (standard)
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "win.h"

typedef struct {
	double acc, mem;
	int op;			/* pending operator, 0 = none */
	int entering;		/* typing a number into ent */
	int error;
	char ent[16];		/* what has been typed: "12", "-3.5" */
	char buf[24];		/* the display */
	Win *display;
} Calc;

enum {
	K0 = 100, KDOT = 110, KADD, KSUB, KMUL, KDIV, KEQ, KSQRT, KPCT, KINV,
	KNEG, KBACK, KCE, KC, KMC, KMR, KMS, KMP
};
enum { CM_COPY = 1, CM_PASTE, CM_ABOUT };

static MenuItem edit_items[] = {
	{ "&Copy\tCtrl+C", CM_COPY, 0 },
	{ "&Paste\tCtrl+V", CM_PASTE, 0 },
};
static MenuItem help_items[] = {
	{ "&About Calculator...", CM_ABOUT, 0 },
};
static Menu calc_menus[] = {
	{ "&Edit", edit_items, COUNT(edit_items) },
	{ "&Help", help_items, COUNT(help_items) },
};
static MenuBar calc_bar = { calc_menus, COUNT(calc_menus) };

/* ---------------- numbers ---------------- */

static double my_sqrt(double x)
{
	double r = x > 1 ? x / 2 : 1;
	int i;

	for (i = 0; i < 60; i++)
		r = (r + x / r) / 2;
	return r;
}

/* up to 12 significant digits, like a pocket calculator */
static void fmt(double v, char *out)
{
	char d[24];
	int neg = v < 0, n = 0, e = 0, i, digits, frac;
	unsigned long long x;

	if (v != v || v > 1e300 || v < -1e300) {
		strcpy(out, "Error");
		return;
	}
	if (neg)
		v = -v;
	if (v != 0 && (v >= 1e12 || v < 1e-10)) {
		/* scientific: m.mmmmmmmmme+ee */
		while (v >= 10) { v /= 10; e++; }
		while (v < 1) { v *= 10; e--; }
		x = (unsigned long long)(v * 1e9 + 0.5);
		if (x >= 10000000000ULL) { x /= 10; e++; }
		for (i = 0; i < 10; i++) { d[9 - i] = '0' + (int)(x % 10); x /= 10; }
		i = 9;
		while (i > 1 && d[i] == '0')
			i--;
		n = sprintf(out, "%s%c.", neg ? "-" : "", d[0]);
		memcpy(out + n, d + 1, i);
		n += i;
		if (out[n - 1] == '.')
			n--;
		sprintf(out + n, "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e);
		return;
	}
	/* digits before the point */
	{
		double t = v;
		digits = 1;
		while (t >= 10) { t /= 10; digits++; }
	}
	frac = 12 - digits;
	{
		double scale = 1;
		for (i = 0; i < frac; i++)
			scale *= 10;
		x = (unsigned long long)(v * scale + 0.5);
	}
	n = 0;
	do {
		d[n++] = '0' + (int)(x % 10);
		x /= 10;
	} while (x || n <= frac);
	/* d holds the digits backwards; the point goes after `frac` of them */
	i = 0;
	if (neg)
		out[i++] = '-';
	while (n > frac)
		out[i++] = d[--n];
	out[i++] = '.';
	while (n > 0)
		out[i++] = d[--n];
	out[i] = 0;
	/* trim the zeros of the fraction (keep the point, as Calculator does) */
	while (i > 0 && out[i - 1] == '0')
		out[--i] = 0;
	if (!strcmp(out, "-0.") )
		strcpy(out, "0.");
}

/* "-12.5e+3" -> number (no floating point in printf/scanf here) */
static double parse(const char *s)
{
	double v = 0, f = 1;
	int neg = 0, e = 0, eneg = 0;

	if (*s == '-') { neg = 1; s++; }
	while (*s >= '0' && *s <= '9')
		v = v * 10 + (*s++ - '0');
	if (*s == '.')
		for (s++; *s >= '0' && *s <= '9'; s++) {
			f /= 10;
			v += (*s - '0') * f;
		}
	if (*s == 'e') {
		s++;
		if (*s == '-') { eneg = 1; s++; } else if (*s == '+') s++;
		while (*s >= '0' && *s <= '9')
			e = e * 10 + (*s++ - '0');
		while (e-- > 0)
			v = eneg ? v / 10 : v * 10;
	}
	return neg ? -v : v;
}

static double value(Calc *c)
{
	return parse(c->buf);
}

/* the display while typing: "12" shows as "12." */
static void show_entry(Calc *c)
{
	strcpy(c->buf, c->ent[0] && strcmp(c->ent, "-") ? c->ent : "0");
	if (!strchr(c->buf, '.'))
		strcat(c->buf, ".");
	invalidate(c->display);
}

static void show(Calc *c, double v)
{
	fmt(v, c->buf);
	if (!strcmp(c->buf, "Error"))
		c->error = 1;
	invalidate(c->display);
}

static double apply(Calc *c, double a, int op, double b)
{
	switch (op) {
	case KADD: return a + b;
	case KSUB: return a - b;
	case KMUL: return a * b;
	case KDIV:
		if (b == 0) {
			c->error = 1;
			return 0;
		}
		return a / b;
	}
	return b;
}

static void press(Calc *c, int k)
{
	double v;
	int len = strlen(c->buf);

	if (c->error && k != KC && k != KCE) {
		beep();
		return;
	}
	if (k >= K0 && k <= K0 + 9) {
		if (!c->entering) {
			c->ent[0] = 0;
			c->entering = 1;
		}
		len = strlen(c->ent);
		if (len >= 13) {
			beep();
			return;
		}
		if (!strcmp(c->ent, "0") || !strcmp(c->ent, "-0"))
			c->ent[--len] = 0;
		c->ent[len] = '0' + k - K0;
		c->ent[len + 1] = 0;
		show_entry(c);
		return;
	}
	switch (k) {
	case KDOT:
		if (!c->entering) {
			c->ent[0] = 0;
			c->entering = 1;
		}
		if (!strchr(c->ent, '.')) {
			if (!c->ent[0] || !strcmp(c->ent, "-"))
				strcat(c->ent, "0");
			strcat(c->ent, ".");
		}
		show_entry(c);
		return;
	case KBACK:
		if (c->entering) {
			len = strlen(c->ent);
			if (len)
				c->ent[len - 1] = 0;
			show_entry(c);
		}
		return;
	case KCE:
		c->error = 0;
		c->entering = 0;
		show(c, 0);
		return;
	case KC:
		c->error = 0;
		c->entering = 0;
		c->acc = 0;
		c->op = 0;
		show(c, 0);
		return;
	case KADD: case KSUB: case KMUL: case KDIV: case KEQ:
		v = value(c);
		if (c->op && (c->entering || k == KEQ))
			v = apply(c, c->acc, c->op, v);
		c->acc = v;
		c->op = (k == KEQ) ? 0 : k;
		c->entering = 0;
		if (c->error)
			strcpy(c->buf, "Error"), invalidate(c->display);
		else
			show(c, v);
		return;
	case KSQRT:
		v = value(c);
		if (v < 0) {
			c->error = 1;
			strcpy(c->buf, "Error");
			invalidate(c->display);
		} else
			show(c, my_sqrt(v));
		c->entering = 0;
		return;
	case KPCT:
		show(c, c->acc * value(c) / 100);
		c->entering = 0;
		return;
	case KINV:
		v = value(c);
		if (v == 0) {
			c->error = 1;
			strcpy(c->buf, "Error");
			invalidate(c->display);
		} else
			show(c, 1 / v);
		c->entering = 0;
		return;
	case KNEG:
		if (c->entering) {
			if (c->ent[0] == '-')
				memmove(c->ent, c->ent + 1, strlen(c->ent));
			else {
				memmove(c->ent + 1, c->ent, strlen(c->ent) + 1);
				c->ent[0] = '-';
			}
			show_entry(c);
		} else
			show(c, -value(c));
		return;
	case KMC: c->mem = 0; break;
	case KMR: show(c, c->mem); c->entering = 0; break;
	case KMS: c->mem = value(c); c->entering = 0; break;
	case KMP: c->mem += value(c); c->entering = 0; break;
	}
	invalidate(c->display->parent);
}

/* ---------------- the window ---------------- */

static int display_proc(Win *w, int msg, int a, int b)
{
	Calc *c = w->parent->data;
	Rect r;

	if (msg == M_PAINT) {
		win_client(w, &r);
		fill(0, 0, r.r, r.b, C_WHITE);
		text(r.r - 3 - text_w(c->buf, F_BOLD), (r.b - FONT_H) / 2, c->buf, C_BLACK, F_BOLD);
		return 1;
	}
	return def_proc(w, msg, a, b);
}

static int calc_proc(Win *w, int msg, int a, int b)
{
	Calc *c = w->data;
	static const char keys[] = "0123456789.+-*/=";
	const char *p;
	int n;

	switch (msg) {
	case M_PAINT:
		/* the memory indicator */
		frame(6, 25, 26, 39, C_BLACK);
		bevel(7, 26, 25, 38, 0);
		if (c->mem != 0)
			text(13, 28, "M", C_BLACK, F_BOLD);
		return 1;
	case M_COMMAND:
		if (a >= K0 && a <= KMP && b == N_CLICKED) {
			press(c, a);
			win_set_focus(w);
			return 1;
		}
		switch (a) {
		case CM_COPY:
			clip_set(c->buf, strlen(c->buf));
			return 1;
		case CM_PASTE:
			{
				const char *s = clip_get(&n);
				for (; s && n-- > 0; s++)
					send(w, M_KEY, (unsigned char)*s, 0);
			}
			return 1;
		case CM_ABOUT:
			msgbox(w, "About Calculator", "Calculator\nStandard arithmetic, with memory.",
			       MB_OK | MB_INFO);
			return 1;
		}
		break;
	case M_KEY:
		if (a == CTRL('c')) { send(w, M_COMMAND, CM_COPY, 0); return 1; }
		if (a == CTRL('v')) { send(w, M_COMMAND, CM_PASTE, 0); return 1; }
		if (a == K_ENTER) a = '=';
		if (a == ',') a = '.';
		if (a && (p = strchr(keys, a)) != 0) {
			n = p - keys;
			press(c, n <= 10 ? K0 + n : KADD + n - 11);
			return 1;
		}
		switch (a) {
		case K_ESC: press(c, KC); return 1;
		case K_DEL: press(c, KCE); return 1;
		case K_BS: press(c, KBACK); return 1;
		case '%': press(c, KPCT); return 1;
		case '@': press(c, KSQRT); return 1;
		case 'r': case 'R': press(c, KINV); return 1;
		case K_F1 + 8: press(c, KNEG); return 1;
		}
		return 0;
	case M_DESTROY:
		free(c);
		return 1;
	}
	return def_proc(w, msg, a, b);
}

void app_calc(void)
{
	static const struct { const char *t; int id, col; } grid[4][6] = {
		{ { "MC", KMC, C_RED }, { "7", K0 + 7, C_BLUE }, { "8", K0 + 8, C_BLUE },
		  { "9", K0 + 9, C_BLUE }, { "/", KDIV, C_RED }, { "sqrt", KSQRT, C_BLUE } },
		{ { "MR", KMR, C_RED }, { "4", K0 + 4, C_BLUE }, { "5", K0 + 5, C_BLUE },
		  { "6", K0 + 6, C_BLUE }, { "*", KMUL, C_RED }, { "%", KPCT, C_BLUE } },
		{ { "MS", KMS, C_RED }, { "1", K0 + 1, C_BLUE }, { "2", K0 + 2, C_BLUE },
		  { "3", K0 + 3, C_BLUE }, { "-", KSUB, C_RED }, { "1/x", KINV, C_BLUE } },
		{ { "M+", KMP, C_RED }, { "0", K0, C_BLUE }, { "+/-", KNEG, C_BLUE },
		  { ".", KDOT, C_BLUE }, { "+", KADD, C_RED }, { "=", KEQ, C_RED } },
	};
	Calc *c = calloc(1, sizeof *c);
	Win *w, *b;
	int r, k, x;

	if (!c)
		return;
	strcpy(c->buf, "0.");
	w = win_create(root, 150, 40, 158, 142, S_CAPTION | S_SYSMENU | S_MINBOX | S_BORDER,
		       "Calculator", calc_proc, c);
	w->menu = &calc_bar;
	w->icon = icon_calc;
	w->bg = C_SILVER;
	c->display = win_create(w, 6, 6, 144, 14, S_CHILD | S_BORDER, 0, display_proc, 0);
	b = ctl_button(w, 32, 25, 38, 14, "Back", KBACK, 0); b->fg = C_RED;
	b = ctl_button(w, 72, 25, 38, 14, "CE", KCE, 0); b->fg = C_RED;
	b = ctl_button(w, 112, 25, 38, 14, "C", KC, 0); b->fg = C_RED;
	for (r = 0; r < 4; r++)
		for (k = 0; k < 6; k++) {
			x = 6 + k * 24 + (k > 0 ? 4 : 0);
			b = ctl_button(w, x, 44 + r * 18, 22, 15, grid[r][k].t, grid[r][k].id, 0);
			b->fg = grid[r][k].col;
			b->style &= ~S_TABSTOP;
		}
}
