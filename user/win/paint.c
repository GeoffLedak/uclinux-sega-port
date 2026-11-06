/*
 * paint.c -- Paint: a small 16-colour picture editor (saves .BMP files)
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "win.h"

#define CW	176			/* picture size */
#define CH	104
#define CX	26			/* where it is in the window */
#define CY	2
#define PAL_Y	(CY + CH + 4)

enum { T_PENCIL, T_BRUSH, T_LINE, T_RECT, T_FRECT, T_FILL, T_ERASER, T_PICKER, NTOOLS };

typedef struct {
	unsigned char *pix;		/* CW x CH, one colour per byte */
	int tool, fg, bg;
	int drawing, colour;		/* a button is down; its colour */
	int x0, y0, x1, y1;		/* shape being dragged, or the last point */
	int shape;			/* a line or rectangle is being dragged */
	char path[256];
	int modified;
} Paint;

static const unsigned char *tool_icons[NTOOLS] = {
	small_pencil, small_brush, small_line, small_rect, small_frect,
	small_fill, small_eraser, small_picker
};

enum { CM_NEW = 1, CM_OPEN, CM_SAVE, CM_SAVEAS, CM_EXIT, CM_CLEAR, CM_ABOUT };

static MenuItem file_items[] = {
	{ "&New", CM_NEW, 0 },
	{ "&Open...", CM_OPEN, 0 },
	{ "&Save", CM_SAVE, 0 },
	{ "Save &As...", CM_SAVEAS, 0 },
	{ 0, 0, 0 },
	{ "E&xit", CM_EXIT, 0 },
};
static MenuItem help_items[] = {
	{ "&About Paint...", CM_ABOUT, 0 },
};
static Menu paint_menus[] = {
	{ "&File", file_items, COUNT(file_items) },
	{ "&Help", help_items, COUNT(help_items) },
};
static MenuBar paint_bar = { paint_menus, COUNT(paint_menus) };

/* ---------------- drawing into the picture ---------------- */

static void put(Paint *p, int x, int y, int c)
{
	if (x >= 0 && y >= 0 && x < CW && y < CH)
		p->pix[y * CW + x] = c;
}

static void dot(Paint *p, int x, int y, int c)
{
	int i, j, r = (p->tool == T_BRUSH) ? 1 : (p->tool == T_ERASER) ? 2 : 0;

	for (j = -r; j <= r; j++)
		for (i = -r; i <= r; i++)
			put(p, x + i, y + j, c);
}

static void pline(Paint *p, int x0, int y0, int x1, int y1, int c, int dots)
{
	int dx = abs(x1 - x0), dy = abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
	int err = dx - dy, e2;

	for (;;) {
		if (dots)
			dot(p, x0, y0, c);
		else
			put(p, x0, y0, c);
		if (x0 == x1 && y0 == y1)
			break;
		e2 = 2 * err;
		if (e2 > -dy) { err -= dy; x0 += sx; }
		if (e2 < dx) { err += dx; y0 += sy; }
	}
}

static void prect(Paint *p, int x0, int y0, int x1, int y1, int c, int filled)
{
	int x, y, t;

	if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
	if (y0 > y1) { t = y0; y0 = y1; y1 = t; }
	for (y = y0; y <= y1; y++)
		for (x = x0; x <= x1; x++)
			if (filled || y == y0 || y == y1 || x == x0 || x == x1)
				put(p, x, y, c);
}

static void flood(Paint *p, int x, int y, int c)
{
	int old = p->pix[y * CW + x], l, r, i, n = 0;
	short *stack;

	if (old == c)
		return;
	stack = malloc(4096 * sizeof *stack);
	if (!stack)
		return;
	stack[n++] = x;
	stack[n++] = y;
	while (n) {
		y = stack[--n];
		x = stack[--n];
		if (p->pix[y * CW + x] != old)
			continue;
		for (l = x; l > 0 && p->pix[y * CW + l - 1] == old; l--)
			;
		for (r = x; r < CW - 1 && p->pix[y * CW + r + 1] == old; r++)
			;
		for (i = l; i <= r; i++) {
			p->pix[y * CW + i] = c;
			if (n < 4092) {
				if (y > 0 && p->pix[(y - 1) * CW + i] == old &&
				    (i == l || p->pix[(y - 1) * CW + i - 1] != old)) {
					stack[n++] = i;
					stack[n++] = y - 1;
				}
				if (y < CH - 1 && p->pix[(y + 1) * CW + i] == old &&
				    (i == l || p->pix[(y + 1) * CW + i - 1] != old)) {
					stack[n++] = i;
					stack[n++] = y + 1;
				}
			}
		}
	}
	free(stack);
}

/* ---------------- files (16-colour BMP) ---------------- */

static const unsigned char bmp_pal[16][3] = {	/* R G B */
	{ 0, 0, 0 }, { 128, 0, 0 }, { 0, 128, 0 }, { 128, 128, 0 },
	{ 0, 0, 128 }, { 128, 0, 128 }, { 0, 128, 128 }, { 192, 192, 192 },
	{ 128, 128, 128 }, { 255, 0, 0 }, { 0, 255, 0 }, { 255, 255, 0 },
	{ 0, 0, 255 }, { 255, 0, 255 }, { 0, 255, 255 }, { 255, 255, 255 },
};

static void le32(unsigned char *b, unsigned long v)
{
	b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
}

static unsigned long rd32(const unsigned char *b)
{
	return b[0] | (b[1] << 8) | ((unsigned long)b[2] << 16) | ((unsigned long)b[3] << 24);
}

static int save_bmp(Paint *p, const char *path)
{
	unsigned char h[54 + 64], row[CW / 2];
	int fd, x, y, i;

	memset(h, 0, sizeof h);
	h[0] = 'B'; h[1] = 'M';
	le32(h + 2, sizeof h + CW / 2 * CH);
	le32(h + 10, sizeof h);
	le32(h + 14, 40);
	le32(h + 18, CW);
	le32(h + 22, CH);
	h[26] = 1;
	h[28] = 4;
	le32(h + 34, CW / 2 * CH);
	le32(h + 46, 16);
	for (i = 0; i < 16; i++) {
		h[54 + i * 4] = bmp_pal[i][2];
		h[55 + i * 4] = bmp_pal[i][1];
		h[56 + i * 4] = bmp_pal[i][0];
	}
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return 0;
	if (write(fd, h, sizeof h) != sizeof h) {
		close(fd);
		return 0;
	}
	for (y = CH - 1; y >= 0; y--) {
		for (x = 0; x < CW; x += 2)
			row[x / 2] = (p->pix[y * CW + x] << 4) | p->pix[y * CW + x + 1];
		if (write(fd, row, sizeof row) != sizeof row) {
			close(fd);
			return 0;
		}
	}
	close(fd);
	return 1;
}

static int load_bmp(Paint *p, const char *path)
{
	unsigned char h[54], row[256];
	int fd, w, ht, x, y, stride, bottom_up;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	if (read(fd, h, 54) != 54 || h[0] != 'B' || h[1] != 'M' || h[28] != 4) {
		close(fd);
		return -1;
	}
	w = rd32(h + 18);
	ht = (long)rd32(h + 22);
	bottom_up = ht > 0;
	if (!bottom_up)
		ht = -ht;
	stride = ((w + 1) / 2 + 3) & ~3;
	if (w <= 0 || stride > (int)sizeof row) {
		close(fd);
		return -1;
	}
	lseek(fd, rd32(h + 10), SEEK_SET);
	memset(p->pix, 15, CW * CH);
	for (y = 0; y < ht; y++) {
		int py = bottom_up ? ht - 1 - y : y;
		if (read(fd, row, stride) != stride)
			break;
		if (py >= CH)
			continue;
		for (x = 0; x < w && x < CW; x++)
			p->pix[py * CW + x] = (x & 1) ? row[x / 2] & 15 : row[x / 2] >> 4;
	}
	close(fd);
	return 1;
}

static void set_title(Win *w, Paint *p)
{
	char buf[60];
	const char *b = strrchr(p->path, '/');

	sprintf(buf, "Paint - %.28s", p->path[0] ? (b ? b + 1 : p->path) : "(Untitled)");
	win_set_text(w, buf);
}

static int do_save(Win *w, Paint *p, int as)
{
	char path[256];

	strcpy(path, p->path[0] ? p->path : "/tmp/picture.bmp");
	if ((as || !p->path[0]) && !file_dialog(w, "Save As", path, 1))
		return 0;
	if (!save_bmp(p, path)) {
		msgbox(w, "Paint", "Cannot write this file.\n(Try a name in /tmp.)", MB_OK | MB_EXCLAIM);
		return 0;
	}
	strcpy(p->path, path);
	p->modified = 0;
	set_title(w, p);
	return 1;
}

static int ask_save(Win *w, Paint *p)
{
	int r;

	if (!p->modified)
		return 1;
	r = msgbox(w, "Paint", "Save the changes to the picture?", MB_YESNOCANCEL | MB_EXCLAIM);
	if (r == ID_CANCEL)
		return 0;
	if (r == ID_YES)
		return do_save(w, p, 0);
	return 1;
}

/* ---------------- the window ---------------- */

static void canvas_dirty(Win *w, int x0, int y0, int x1, int y1)
{
	int t;

	if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
	if (y0 > y1) { t = y0; y0 = y1; y1 = t; }
	invalidate_client(w, CX + x0 - 3, CY + y0 - 3, CX + x1 + 4, CY + y1 + 4);
}

static void paint_canvas(Paint *p)
{
	int y, x, s, c;

	for (y = 0; y < CH; y++) {
		if (org_y + CY + y < clip.t || org_y + CY + y >= clip.b)
			continue;
		for (x = 0; x < CW; x = s) {
			c = p->pix[y * CW + x];
			for (s = x + 1; s < CW && p->pix[y * CW + s] == c; s++)
				;
			hline(CX + x, CX + s, CY + y, c);
		}
	}
	if (p->shape) {
		/* the line or rectangle being dragged, drawn over the picture */
		int x0 = p->x0, y0 = p->y0, x1 = p->x1, y1 = p->y1, t;
		if (p->tool == T_LINE)
			line(CX + x0, CY + y0, CX + x1, CY + y1, p->colour);
		else {
			if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
			if (y0 > y1) { t = y0; y0 = y1; y1 = t; }
			if (p->tool == T_FRECT)
				fill(CX + x0, CY + y0, CX + x1 + 1, CY + y1 + 1, p->colour);
			else
				frame(CX + x0, CY + y0, CX + x1 + 1, CY + y1 + 1, p->colour);
		}
	}
}

static int paint_proc(Win *w, int msg, int a, int b)
{
	Paint *p = w->data;
	int i, x, y, right = (msg == M_RDOWN || msg == M_RUP);
	char path[256];

	switch (msg) {
	case M_PAINT:
		/* tools */
		for (i = 0; i < NTOOLS; i++) {
			x = 2 + (i % 2) * 11;
			y = CY + (i / 2) * 11;
			frame(x, y, x + 11, y + 11, C_BLACK);
			fill(x + 1, y + 1, x + 10, y + 10, i == p->tool ? C_WHITE : C_SILVER);
			bevel(x + 1, y + 1, x + 10, y + 10, i != p->tool);
			draw_icon(x + 2 + (i == p->tool), y + 2 + (i == p->tool), tool_icons[i], 8);
		}
		/* the picture */
		frame(CX - 1, CY - 1, CX + CW + 1, CY + CH + 1, C_BLACK);
		paint_canvas(p);
		/* colours: current foreground on background, then the palette */
		frame(2, PAL_Y, 22, PAL_Y + 16, C_BLACK);
		fill(3, PAL_Y + 1, 21, PAL_Y + 15, p->bg);
		frame(6, PAL_Y + 4, 16, PAL_Y + 12, C_BLACK);
		fill(7, PAL_Y + 5, 15, PAL_Y + 11, p->fg);
		for (i = 0; i < 16; i++) {
			x = CX + (i % 8) * 11;
			y = PAL_Y + (i / 8) * 8;
			frame(x, y, x + 11, y + 8, C_BLACK);
			fill(x + 1, y + 1, x + 10, y + 7, i);
		}
		text(CX + 92, PAL_Y + 4, "A: colour  B: back", C_BLACK, F_NORMAL);
		return 1;
	case M_CURSOR:
		return (a >= CX && a < CX + CW && b >= CY && b < CY + CH) ? PTR_CROSS : PTR_ARROW;
	case M_LDOWN:
	case M_LDBL:
	case M_RDOWN:
		/* a tool? */
		if (a < CX - 2 && b < CY + 44) {
			i = ((b - CY) / 11) * 2 + (a - 2) / 11;
			if (i >= 0 && i < NTOOLS && !right) {
				p->tool = i;
				invalidate_client(w, 0, 0, CX - 1, CY + 46);
			}
			return 1;
		}
		/* a colour? */
		if (b >= PAL_Y && b < PAL_Y + 16 && a >= CX && a < CX + 88) {
			i = (b - PAL_Y) / 8 * 8 + (a - CX) / 11;
			if (right)
				p->bg = i;
			else
				p->fg = i;
			invalidate_client(w, 0, PAL_Y, CX, PAL_Y + 16);
			return 1;
		}
		x = a - CX;
		y = b - CY;
		if (x < 0 || y < 0 || x >= CW || y >= CH)
			return 1;
		p->colour = right ? p->bg : p->fg;
		if (p->tool == T_ERASER)
			p->colour = p->bg;
		switch (p->tool) {
		case T_PICKER:
			if (right)
				p->bg = p->pix[y * CW + x];
			else
				p->fg = p->pix[y * CW + x];
			invalidate_client(w, 0, PAL_Y, CX, PAL_Y + 16);
			return 1;
		case T_FILL:
			flood(p, x, y, p->colour);
			p->modified = 1;
			invalidate(w);
			return 1;
		case T_LINE: case T_RECT: case T_FRECT:
			p->shape = 1;
			p->x0 = p->x1 = x;
			p->y0 = p->y1 = y;
			p->drawing = 1;
			canvas_dirty(w, x, y, x, y);
			return 1;
		default:
			dot(p, x, y, p->colour);
			p->x1 = x;
			p->y1 = y;
			p->drawing = 1;
			p->modified = 1;
			canvas_dirty(w, x, y, x, y);
			return 1;
		}
	case M_MOVE:
		if (!p->drawing)
			return 1;
		x = a - CX;
		y = b - CY;
		if (x < 0) x = 0;
		if (y < 0) y = 0;
		if (x >= CW) x = CW - 1;
		if (y >= CH) y = CH - 1;
		if (p->shape) {
			int ox0 = p->x0 < p->x1 ? p->x0 : p->x1, ox1 = p->x0 < p->x1 ? p->x1 : p->x0;
			int oy0 = p->y0 < p->y1 ? p->y0 : p->y1, oy1 = p->y0 < p->y1 ? p->y1 : p->y0;
			canvas_dirty(w, ox0, oy0, ox1, oy1);
			p->x1 = x;
			p->y1 = y;
			canvas_dirty(w, p->x0, p->y0, x, y);
		} else {
			pline(p, p->x1, p->y1, x, y, p->colour, 1);
			canvas_dirty(w, p->x1, p->y1, x, y);
			p->x1 = x;
			p->y1 = y;
		}
		return 1;
	case M_LUP:
	case M_RUP:
		if (p->drawing && p->shape) {
			if (p->tool == T_LINE)
				pline(p, p->x0, p->y0, p->x1, p->y1, p->colour, 0);
			else
				prect(p, p->x0, p->y0, p->x1, p->y1, p->colour, p->tool == T_FRECT);
			canvas_dirty(w, p->x0, p->y0, p->x1, p->y1);
			p->modified = 1;
		}
		p->drawing = p->shape = 0;
		return 1;
	case M_COMMAND:
		switch (a) {
		case CM_NEW:
			if (ask_save(w, p)) {
				memset(p->pix, 15, CW * CH);
				p->path[0] = 0;
				p->modified = 0;
				set_title(w, p);
				invalidate(w);
			}
			return 1;
		case CM_OPEN:
			if (!ask_save(w, p))
				return 1;
			strcpy(path, p->path[0] ? p->path : "/tmp/");
			if (file_dialog(w, "Open", path, 0)) {
				i = load_bmp(p, path);
				if (i <= 0)
					msgbox(w, "Paint", i < 0 ? "This is not a 16-colour BMP picture."
					       : "Cannot open this file.", MB_OK | MB_EXCLAIM);
				else {
					strcpy(p->path, path);
					p->modified = 0;
					set_title(w, p);
				}
				invalidate(w);
			}
			return 1;
		case CM_SAVE: do_save(w, p, 0); return 1;
		case CM_SAVEAS: do_save(w, p, 1); return 1;
		case CM_EXIT: send(w, M_CLOSE, 0, 0); return 1;
		case CM_ABOUT:
			msgbox(w, "About Paint", "Paint\n\nA: draw in the colour,\nB: draw in the background colour.\n"
			       "Pictures are saved as 16-colour BMP files.", MB_OK | MB_INFO);
			return 1;
		}
		break;
	case M_CLOSE:
		if (ask_save(w, p))
			win_destroy(w);
		return 1;
	case M_DESTROY:
		free(p->pix);
		free(p);
		return 1;
	}
	return def_proc(w, msg, a, b);
}

void app_paint(void)
{
	Paint *p = calloc(1, sizeof *p);
	Win *w;

	if (!p)
		return;
	p->pix = malloc(CW * CH);
	if (!p->pix) {
		free(p);
		msgbox(active, "Paint", "Not enough memory.", MB_OK | MB_STOP);
		return;
	}
	memset(p->pix, 15, CW * CH);
	p->fg = C_BLACK;
	p->bg = C_WHITE;
	w = win_create(root, 40, 20, CX + CW + 4 + 2, PAL_Y + 18 + 2 + 22, S_CAPTION | S_SYSMENU |
		       S_MINBOX | S_BORDER, "Paint - (Untitled)", paint_proc, p);
	w->menu = &paint_bar;
	w->icon = icon_paint;
	w->bg = C_SILVER;
}
