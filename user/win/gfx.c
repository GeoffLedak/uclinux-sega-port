/*
 * gfx.c -- drawing into the frame buffer
 *
 * The frame buffer has the layout of the Genesis video memory: 40x28
 * tiles of 8x8 pixels, each tile 8 longs (one per pixel row), 4 bits per
 * pixel with the leftmost pixel in the top 4 bits. Everything drawn marks
 * its tiles dirty; hw_flush() copies those to the screen.
 *
 * Drawing calls take coordinates relative to org_x, org_y and are
 * clipped to clip (screen coordinates); see gfx_set().
 */
#include "win.h"

u32 fb[SCR_TILES * 8];
u32 dirty[DIRTY_WORDS];
Rect clip = { 0, 0, SCR_W, SCR_H };
int org_x, org_y;

static unsigned short rowbase[SCR_H];	/* fb index of pixel row y, tile column 0 */
static u32 expand[256];			/* 8 pixel bits -> 8 nibble masks */
static int ready;

static void setup(void)
{
	int i, b;

	for (i = 0; i < SCR_H; i++)
		rowbase[i] = (i >> 3) * (SCR_COLS * 8) + (i & 7);
	for (i = 0; i < 256; i++) {
		u32 m = 0;
		for (b = 0; b < 8; b++)
			if (i & (0x80 >> b))
				m |= 0xF0000000u >> (b * 4);
		expand[i] = m;
	}
	ready = 1;
}

void set_rect(Rect *r, int l, int t, int rr, int b)
{
	r->l = l; r->t = t; r->r = rr; r->b = b;
}

int rect_and(Rect *a, const Rect *b)
{
	if (a->l < b->l) a->l = b->l;
	if (a->t < b->t) a->t = b->t;
	if (a->r > b->r) a->r = b->r;
	if (a->b > b->b) a->b = b->b;
	return a->l < a->r && a->t < a->b;
}

int in_rect(const Rect *r, int x, int y)
{
	return x >= r->l && x < r->r && y >= r->t && y < r->b;
}

void gfx_set(int ox, int oy, const Rect *c)
{
	static const Rect screen = { 0, 0, SCR_W, SCR_H };

	if (!ready)
		setup();
	org_x = ox;
	org_y = oy;
	clip = *c;
	rect_and(&clip, &screen);
}

void gfx_screen(void)
{
	Rect r;

	set_rect(&r, 0, 0, SCR_W, SCR_H);
	gfx_set(0, 0, &r);
}

void mark_dirty(int l, int t, int r, int b)
{
	int tx, ty, n, tx0, tx1;

	if (l < 0) l = 0;
	if (t < 0) t = 0;
	if (r > SCR_W) r = SCR_W;
	if (b > SCR_H) b = SCR_H;
	if (l >= r || t >= b)
		return;
	tx0 = l >> 3;
	tx1 = (r - 1) >> 3;
	for (ty = t >> 3; ty <= (b - 1) >> 3; ty++) {
		n = ty * SCR_COLS + tx0;
		for (tx = tx0; tx <= tx1; tx++, n++)
			dirty[n >> 5] |= 1u << (n & 31);
	}
}

/* clip a rectangle given in drawing coordinates; 0 if nothing is left */
static int clip_rect(int *l, int *t, int *r, int *b)
{
	*l += org_x; *r += org_x;
	*t += org_y; *b += org_y;
	if (*l < clip.l) *l = clip.l;
	if (*t < clip.t) *t = clip.t;
	if (*r > clip.r) *r = clip.r;
	if (*b > clip.b) *b = clip.b;
	return *l < *r && *t < *b;
}

/* fill in screen coordinates, already clipped */
static void fill_screen(int l, int t, int r, int b, u32 cv, int xor_pattern)
{
	int y, tx, tx0 = l >> 3, tx1 = (r - 1) >> 3;
	u32 m0 = 0xFFFFFFFFu >> ((l & 7) * 4);
	u32 m1 = 0xFFFFFFFFu << ((7 - ((r - 1) & 7)) * 4);
	u32 *p, m;

	for (y = t; y < b; y++) {
		p = fb + rowbase[y] + (tx0 << 3);
		if (xor_pattern) {
			m = (y & 1) ? 0x0F0F0F0Fu : 0xF0F0F0F0u;
			if (tx0 == tx1) {
				*p ^= m & m0 & m1;
				continue;
			}
			*p ^= m & m0;
			p += 8;
			for (tx = tx0 + 1; tx < tx1; tx++, p += 8)
				*p ^= m;
			*p ^= m & m1;
			continue;
		}
		if (tx0 == tx1) {
			m = m0 & m1;
			*p = (*p & ~m) | (cv & m);
			continue;
		}
		*p = (*p & ~m0) | (cv & m0);
		p += 8;
		for (tx = tx0 + 1; tx < tx1; tx++, p += 8)
			*p = cv;
		*p = (*p & ~m1) | (cv & m1);
	}
	mark_dirty(l, t, r, b);
}

void fill(int l, int t, int r, int b, int c)
{
	if (clip_rect(&l, &t, &r, &b))
		fill_screen(l, t, r, b, (u32)c * 0x11111111u, 0);
}

void xor_fill(int l, int t, int r, int b)
{
	int y, x;
	u32 *p;

	if (!clip_rect(&l, &t, &r, &b))
		return;
	for (y = t; y < b; y++)
		for (x = l; x < r; x++) {
			p = fb + rowbase[y] + ((x >> 3) << 3);
			*p ^= 0xF0000000u >> ((x & 7) * 4);
		}
	mark_dirty(l, t, r, b);
}

void hline(int x0, int x1, int y, int c)
{
	fill(x0, y, x1, y + 1, c);
}

void vline(int x, int y0, int y1, int c)
{
	fill(x, y0, x + 1, y1, c);
}

void frame(int l, int t, int r, int b, int c)
{
	hline(l, r, t, c);
	hline(l, r, b - 1, c);
	vline(l, t + 1, b - 1, c);
	vline(r - 1, t + 1, b - 1, c);
}

void pixel(int x, int y, int c)
{
	u32 *p;
	int sh;

	x += org_x;
	y += org_y;
	if (x < clip.l || x >= clip.r || y < clip.t || y >= clip.b)
		return;
	p = fb + rowbase[y] + ((x >> 3) << 3);
	sh = (7 - (x & 7)) * 4;
	*p = (*p & ~(0xFu << sh)) | ((u32)c << sh);
	sh = (y >> 3) * SCR_COLS + (x >> 3);	/* tile number */
	dirty[sh >> 5] |= 1u << (sh & 31);
}

int get_pixel(int x, int y)
{
	if (x < 0 || x >= SCR_W || y < 0 || y >= SCR_H)
		return 0;
	return (fb[rowbase[y] + ((x >> 3) << 3)] >> ((7 - (x & 7)) * 4)) & 15;
}

void line(int x0, int y0, int x1, int y1, int c)
{
	int dx = x1 - x0, dy = y1 - y0, sx = 1, sy = 1, err, e2;

	if (dx < 0) { dx = -dx; sx = -1; }
	if (dy < 0) { dy = -dy; sy = -1; }
	err = dx - dy;
	for (;;) {
		pixel(x0, y0, c);
		if (x0 == x1 && y0 == y1)
			break;
		e2 = 2 * err;
		if (e2 > -dy) { err -= dy; x0 += sx; }
		if (e2 < dx) { err += dx; y0 += sy; }
	}
}

void bevel(int l, int t, int r, int b, int up)
{
	int hi = up ? C_WHITE : C_GRAY, lo = up ? C_GRAY : C_WHITE;

	hline(l, r - 1, t, hi);
	vline(l, t + 1, b - 1, hi);
	hline(l, r, b - 1, lo);
	vline(r - 1, t, b - 1, lo);
}

static void xspan(int l, int t, int r, int b)
{
	if (l < 0)
		l = 0;
	if (t < 0)
		t = 0;
	if (r > SCR_W)
		r = SCR_W;
	if (b > SCR_H)
		b = SCR_H;
	if (l < r && t < b)
		fill_screen(l, t, r, b, 0, 1);
}

/* a dithered outline in screen coordinates (drawing it again removes it) */
void xor_frame(int l, int t, int r, int b, int thick)
{
	if (r - l <= 2 * thick || b - t <= 2 * thick) {
		xspan(l, t, r, b);
		return;
	}
	xspan(l, t, r, t + thick);
	xspan(l, b - thick, r, b);
	xspan(l, t + thick, l + thick, b - thick);
	xspan(r - thick, t + thick, r, b - thick);
}

void dotted_frame(int l, int t, int r, int b)
{
	int x, y;

	for (x = l; x < r; x += 2) {
		pixel(x, t, C_BLACK);
		pixel(x + ((b - 1 - t) & 1), b - 1, C_BLACK);
	}
	for (y = t + 2; y < b - 1; y += 2) {
		pixel(l, y, C_BLACK);
		pixel(r - 1, y + ((r - 1 - l) & 1), C_BLACK);
	}
}

void draw_icon(int x, int y, const unsigned char *img, int size)
{
	int i, j;
	const unsigned char *p = img;

	for (j = 0; j < size; j++)
		for (i = 0; i < size; i++, p++)
			if (*p != 0xFF)
				pixel(x + i, y + j, *p);
}

/* ---------------- text ---------------- */

static const unsigned char *glyph(int ch, int font, int *w)
{
	if (ch < 32 || ch > 126)
		ch = (ch == 0xA0 || ch == '\t') ? ' ' : '?';
	if (font == F_FIXED) {
		*w = FIXED_W;
		return font_fixed + (ch - 32) * 8;
	}
	*w = font_width[ch - 32] + (font == F_BOLD);
	return font_bits + (ch - 32) * 8;
}

int char_w(int ch, int font)
{
	int w;

	glyph(ch & 0xFF, font, &w);
	return w;
}

/* draw one character at screen position (x, y), already offset */
static void put_char(int x, int y, int ch, u32 cv, int font)
{
	int w, row, yy, sh, i;
	unsigned int bits, cmask = 0;
	const unsigned char *g = glyph(ch, font, &w);
	u32 *p, m, m0, m1;

	if (x >= clip.r || x + 8 <= clip.l || y >= clip.b || y + FONT_H <= clip.t)
		return;
	for (i = 0; i < 8; i++)
		if (x + i >= clip.l && x + i < clip.r)
			cmask |= 0x80 >> i;
	if (x < 0) {
		cmask = (cmask << -x) & 0xFF;	/* columns left of 0 are clipped */
		sh = -x;
		x = 0;
	} else
		sh = 0;
	for (row = 0; row < FONT_H; row++) {
		yy = y + row;
		if (yy < clip.t || yy >= clip.b)
			continue;
		bits = g[row];
		if (font == F_BOLD)
			bits |= bits >> 1;
		bits = ((bits << sh) & 0xFF) & cmask;
		if (!bits)
			continue;
		m = expand[bits];
		p = fb + rowbase[yy] + ((x >> 3) << 3);
		i = (x & 7) * 4;
		m0 = m >> i;
		*p = (*p & ~m0) | (cv & m0);
		if (i) {
			m1 = m << (32 - i);
			if (m1)
				p[8] = (p[8] & ~m1) | (cv & m1);
		}
	}
	mark_dirty(x, y, x + 9, y + FONT_H);
}

int textn(int x, int y, const char *s, int n, int c, int font)
{
	u32 cv = (u32)c * 0x11111111u;
	int w, ch;

	x += org_x;
	y += org_y;
	while (n-- > 0 && *s) {
		ch = (unsigned char)*s++;
		glyph(ch, font, &w);
		if (ch != ' ')
			put_char(x, y, ch, cv, font);
		x += w;
	}
	return x - org_x;
}

int text(int x, int y, const char *s, int c, int font)
{
	return textn(x, y, s, 0x7FFF, c, font);
}

int textn_w(const char *s, int n, int font)
{
	int w = 0;

	while (n-- > 0 && *s)
		w += char_w((unsigned char)*s++, font);
	return w;
}

int text_w(const char *s, int font)
{
	return textn_w(s, 0x7FFF, font);
}

int text_mn(int x, int y, const char *s, int c, int font)
{
	int x0 = x, w;

	for (; *s && *s != '\t'; s++) {
		if (*s == '&' && s[1] && s[1] != '&') {
			s++;
			w = char_w((unsigned char)*s, font);
			hline(x, x + w - 1, y + FONT_H, c);
		}
		x = textn(x, y, s, 1, c, font);
	}
	return x - x0;
}

int text_mn_w(const char *s, int font)
{
	int w = 0;

	for (; *s && *s != '\t'; s++) {
		if (*s == '&' && s[1] && s[1] != '&')
			s++;
		w += char_w((unsigned char)*s, font);
	}
	return w;
}

int mnemonic(const char *s)
{
	for (; *s && *s != '\t'; s++)
		if (*s == '&' && s[1] && s[1] != '&') {
			int c = (unsigned char)s[1];
			return (c >= 'A' && c <= 'Z') ? c + 32 : c;
		}
	return 0;
}
