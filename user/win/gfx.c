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
#include <stdlib.h>
#include <string.h>
#include "win.h"

u32 fb[FB_LONGS];
u32 dirty[DIRTY_WORDS];
Rect clip = { 0, 0, SCR_W, SCR_H };
int org_x, org_y;

static unsigned short rowbase[SCR_H];	/* fb index of pixel row y, tile column 0 */
static u32 expand[256];			/* 8 pixel bits -> 8 nibble masks */
static u32 lomask[9];			/* pixels i - 7 of a long (lomask[8] = 0) */
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
	for (i = 0; i < 8; i++)
		lomask[i] = 0xFFFFFFFFu >> (i * 4);
	lomask[8] = 0;
	ready = 1;
}

/* ---------------------------------------------------------------- */
/* The inner loops: assembly on the Genesis (gfx68k.S), C on the PC   */
/* ---------------------------------------------------------------- */

void fb_fill(u32 *p, int nrows, int ntiles, u32 cv);
void fb_mask(u32 *p, int nrows, u32 m, u32 cv);
void fb_strip(u32 *p, const unsigned char *b, int n, u32 cv, const u32 *expand);
void fb_blit_mv(u32 *p, const u32 *mv, int nrows, int sh, u32 colmask, int stride);
void fb_copy(u32 *dst, const u32 *src, int n, u32 m0, u32 m1, int sstride);
void fb_gather(u32 *buf, const u32 *src, int n, int sh);
void fb_shift(u32 *dst, const u32 *src, int n, int sh, u32 m0, u32 m1, u32 first);

#ifdef HOST
static u32 ror(u32 v, int sh)
{
	return sh ? (v >> sh) | (v << (32 - sh)) : v;
}

void fb_fill(u32 *p, int nrows, int ntiles, u32 cv)
{
	int i;

	for (; ntiles > 0; ntiles--, p += 8)
		for (i = 0; i < nrows; i++)
			p[i] = cv;
}

void fb_mask(u32 *p, int nrows, u32 m, u32 cv)
{
	for (; nrows > 0; nrows--, p++)
		*p = (*p & ~m) | (cv & m);
}

void fb_strip(u32 *p, const unsigned char *b, int n, u32 cv, const u32 *ex)
{
	u32 m;

	for (; n > 0; n--, p += 8)
		if (*b++) {
			m = ex[b[-1]];
			*p ^= (*p ^ cv) & m;
		}
}

void fb_blit_mv(u32 *p, const u32 *mv, int nrows, int sh, u32 colmask, int stride)
{
	u32 lo = 0xFFFFFFFFu >> sh, m, m1, v;

	for (; nrows > 0; nrows--, p++, mv += stride) {
		m = mv[0] & colmask;
		if (!m)
			continue;
		m = ror(m, sh);
		v = ror(mv[1], sh);
		m1 = m & ~lo;
		m &= lo;
		p[0] ^= (p[0] ^ v) & m;
		if (m1)
			p[8] ^= (p[8] ^ v) & m1;
	}
}

void fb_copy(u32 *dst, const u32 *src, int n, u32 m0, u32 m1, int sstride)
{
	int k;

	for (k = 0; k < n; k++, dst += 8, src += sstride / 4) {
		u32 m = (k == 0) ? m0 : (k == n - 1) ? m1 : 0xFFFFFFFFu;
		*dst ^= (*dst ^ *src) & m;
	}
}

void fb_gather(u32 *buf, const u32 *src, int n, int sh)
{
	for (; n > 0; n--, src += 8)
		*buf++ = (src[0] << sh) | (src[8] >> (32 - sh));
}

void fb_shift(u32 *dst, const u32 *src, int n, int sh, u32 m0, u32 m1, u32 a)
{
	int k;
	u32 b, v, m;

	for (k = 0; k < n; k++, dst += 8, src += 8) {
		b = *src;
		v = (a << sh) | (b >> (32 - sh));
		a = b;
		m = (k == 0) ? m0 : (k == n - 1) ? m1 : 0xFFFFFFFFu;
		*dst ^= (*dst ^ v) & m;
	}
}
#endif

/* ---------------------------------------------------------------- */

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

/* set cnt bits of dirty[] from bit n */
static void set_bits(int n, int cnt)
{
	u32 *w = dirty + (n >> 5);
	int b = n & 31, k;

	while (cnt > 0) {
		k = 32 - b;
		if (k > cnt)
			k = cnt;
		*w++ |= (k == 32) ? 0xFFFFFFFFu : ((1u << k) - 1) << b;
		cnt -= k;
		b = 0;
	}
}

void mark_dirty(int l, int t, int r, int b)
{
	int ty, n, tx0;

	if (l < 0) l = 0;
	if (t < 0) t = 0;
	if (r > SCR_W) r = SCR_W;
	if (b > SCR_H) b = SCR_H;
	if (l >= r || t >= b)
		return;
	tx0 = l >> 3;
	n = ((r - 1) >> 3) - tx0 + 1;
	for (ty = t >> 3; ty <= (b - 1) >> 3; ty++)
		set_bits((ty << 5) + (ty << 3) + tx0, n);	/* ty * 40 */
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

/* fill in screen coordinates, already clipped: one band of tile rows at a time */
static void fill_screen(int l, int t, int r, int b, u32 cv)
{
	int y, n, tx0 = l >> 3, tx1 = (r - 1) >> 3, mid0, mid1;
	u32 m0 = lomask[l & 7], m1 = ~lomask[((r - 1) & 7) + 1], *p;

	mid0 = (l & 7) ? tx0 + 1 : tx0;		/* whole tiles: mid0 to mid1 - 1 */
	mid1 = (r & 7) ? tx1 : tx1 + 1;
	for (y = t; y < b; y += n) {
		n = 8 - (y & 7);
		if (n > b - y)
			n = b - y;
		p = fb + rowbase[y] + (tx0 << 3);
		if (tx0 == tx1) {
			fb_mask(p, n, m0 & m1, cv);
			continue;
		}
		if (l & 7) {
			fb_mask(p, n, m0, cv);
			p += 8;
		}
		if (mid1 > mid0) {
			fb_fill(p, n, mid1 - mid0, cv);
			p += (mid1 - mid0) << 3;
		}
		if (r & 7)
			fb_mask(p, n, m1, cv);
	}
	mark_dirty(l, t, r, b);
}

/* a dithered fill (for outlines), in screen coordinates, clipped */
static void xor_screen(int l, int t, int r, int b)
{
	int y, tx, tx0 = l >> 3, tx1 = (r - 1) >> 3;
	u32 m0 = lomask[l & 7], m1 = ~lomask[((r - 1) & 7) + 1];
	u32 *p, m;

	for (y = t; y < b; y++) {
		p = fb + rowbase[y] + (tx0 << 3);
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
	}
	mark_dirty(l, t, r, b);
}

void fill(int l, int t, int r, int b, int c)
{
	if (clip_rect(&l, &t, &r, &b))
		fill_screen(l, t, r, b, (u32)c * 0x11111111u);
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
	u32 *p, m;
	int n;

	x += org_x;
	y += org_y;
	if (x < clip.l || x >= clip.r || y < clip.t || y >= clip.b)
		return;
	p = fb + rowbase[y] + ((x >> 3) << 3);
	m = lomask[x & 7] & ~lomask[(x & 7) + 1];
	*p = (*p & ~m) | ((u32)c * 0x11111111u & m);
	n = ((y >> 3) << 5) + ((y >> 3) << 3) + (x >> 3);	/* tile number */
	dirty[n >> 5] |= 1u << (n & 31);
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

	if (dy == 0) {				/* the common cases: straight lines */
		hline(x0 < x1 ? x0 : x1, (x0 < x1 ? x1 : x0) + 1, y0, c);
		return;
	}
	if (dx == 0) {
		vline(x0, y0 < y1 ? y0 : y1, (y0 < y1 ? y1 : y0) + 1, c);
		return;
	}
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
		xor_screen(l, t, r, b);
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

/* ---------------- copying ---------------- */

/*
 * Copy the pixels of screen rectangle (l, t, r, b) to (l + dx, t + dy);
 * both inside the screen. The rectangles may overlap.
 */
void copy_rect(int l, int t, int r, int b, int dx, int dy)
{
	static u32 row[SCR_COLS + 2];
	int y, y1, step, dl = l + dx, dr = r + dx, tx0, n, s, sh;
	u32 m0, m1;
	const u32 *src;

	if (l >= r || t >= b || (dx == 0 && dy == 0))
		return;
	tx0 = dl >> 3;
	n = ((dr - 1) >> 3) - tx0 + 1;
	m0 = lomask[dl & 7];
	m1 = ~lomask[((dr - 1) & 7) + 1];
	if (n == 1)
		m0 &= m1;
	s = (tx0 << 3) - dx;			/* the source pixel of the first long */
	sh = (s & 7) * 4;
	/* going down: start at the bottom, so nothing is read after it's written */
	if (dy > 0) {
		y = b - 1; y1 = t - 1; step = -1;
	} else {
		y = t; y1 = b; step = 1;
	}
	for (; y != y1; y += step) {
		u32 *dst = fb + rowbase[y + dy] + (tx0 << 3);
		src = fb + rowbase[y] + (s & ~7);	/* (may be -8: s < 0) */
		/* straight across, unless it's to the right in the same row
		 * (where it would read what it has just written) */
		if (!sh && (dx <= 0 || dy))
			fb_copy(dst, src, n, m0, m1, 32);
		else if (dx < 0 || dy)
			fb_shift(dst, src + 8, n, sh, m0, m1, s < 0 ? 0 : src[0]);
		else {
			/* through a row buffer, lined up with the destination */
			if (!sh) {
				int k;
				for (k = 0; k < n; k++)
					row[k] = src[k << 3];
			} else if (s < 0) {
				row[0] = src[8] >> (32 - sh);		/* nothing left of 0 */
				if (n > 1)
					fb_gather(row + 1, src + 8, n - 1, sh);
			} else
				fb_gather(row, src, n, sh);
			fb_copy(dst, row, n, m0, m1, 4);
		}
	}
	mark_dirty(dl, t + dy, dr, b + dy);
}

/* ---------------- icons ---------------- */

/*
 * Icons (one byte per pixel, 0xFF = transparent) are turned into
 * mask/value longs, 8 pixels each, the first time they are drawn.
 */
#define ICON_CACHE	64

static struct {
	const unsigned char *img;
	u32 *mv;
} icache[ICON_CACHE];

static u32 *icon_mv(const unsigned char *img, int size)
{
	int i, j, k, slot, groups = size >> 3;
	u32 *mv, m, v;

	for (slot = 0; slot < ICON_CACHE && icache[slot].img; slot++)
		if (icache[slot].img == img)
			return icache[slot].mv;
	if (slot == ICON_CACHE)
		return 0;			/* full: draw it the slow way */
	mv = malloc(size * groups * 2 * sizeof(u32));
	if (!mv)
		return 0;
	for (j = 0; j < size; j++)
		for (k = 0; k < groups; k++) {
			m = v = 0;
			for (i = 0; i < 8; i++) {
				unsigned char c = img[j * size + k * 8 + i];
				if (c != 0xFF) {
					m |= 0xF0000000u >> (i * 4);
					v |= (u32)c << (28 - i * 4);
				}
			}
			mv[(j * groups + k) * 2] = m;
			mv[(j * groups + k) * 2 + 1] = v;
		}
	icache[slot].img = img;
	icache[slot].mv = mv;
	return mv;
}

static void draw_icon_slow(int x, int y, const unsigned char *img, int size)
{
	int i, j;
	const unsigned char *p = img;

	for (j = 0; j < size; j++)
		for (i = 0; i < size; i++, p++)
			if (*p != 0xFF)
				pixel(x + i, y + j, *p);
}

void draw_icon(int x, int y, const unsigned char *img, int size)
{
	int sx = x + org_x, sy = y + org_y, groups = size >> 3, g, gx, j, n, i;
	unsigned cm;
	const u32 *mv;

	if (sx >= clip.r || sx + size <= clip.l || sy >= clip.b || sy + size <= clip.t)
		return;
	mv = (sx >= 0 && (size & 7) == 0) ? icon_mv(img, size) : 0;
	if (!mv) {
		draw_icon_slow(x, y, img, size);
		return;
	}
	for (g = 0; g < groups; g++) {
		gx = sx + g * 8;
		/* the columns inside the clip */
		cm = 0;
		for (i = 0; i < 8; i++)
			if (gx + i >= clip.l && gx + i < clip.r)
				cm |= 0x80 >> i;
		if (!cm)
			continue;
		for (j = 0; j < size; j += n) {
			int y0 = sy + j, skip = 0;
			n = 8 - (y0 & 7);
			if (n > size - j)
				n = size - j;
			/* clip the band's rows */
			if (y0 + n <= clip.t || y0 >= clip.b)
				continue;
			if (y0 < clip.t)
				skip = clip.t - y0;
			i = n - skip;
			if (y0 + n > clip.b)
				i -= y0 + n - clip.b;
			fb_blit_mv(fb + rowbase[y0 + skip] + ((gx >> 3) << 3),
				   mv + (MUL16(j + skip, groups) + g) * 2, i, (gx & 7) * 4,
				   expand[cm], groups * 2);
		}
	}
	mark_dirty(sx, sy, sx + size + 8, sy + size);
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
static void put_char_slow(int x, int y, int ch, u32 cv, int font)
{
	int w, row, yy, sh, i;
	unsigned int bits, cmask = 0;
	const unsigned char *g = glyph(ch, font, &w);
	u32 *p, m, m0, m1;

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

/*
 * A string is drawn by first putting its glyphs side by side in a strip
 * of 1-bit pixels, 8 rows, whose bytes line up with the tiles (byte k of
 * a row is tile column k from ox); then each byte is turned into a mask
 * of 8 pixels and written to its long in one go (fb_strip).
 */
#define STRIP_BYTES	(SCR_COLS + 4)
static unsigned char strip[FONT_H][STRIP_BYTES];	/* all 0 between calls */

/* or one row of a glyph into the strip: bits >> off into sp[0], what
 * spills over into sp[1] (as a 16-bit number, the two bytes) */
#define SROW(r)	do {							\
		if ((b = g[r]) != 0) {					\
			if (bold)					\
				b |= b >> 1;				\
			b <<= sh;					\
			sp[(r) * STRIP_BYTES] |= b >> 8;		\
			sp[(r) * STRIP_BYTES + 1] |= b;			\
		}							\
	} while (0)

int textn(int x, int y, const char *s, int n, int c, int font)
{
	u32 cv = (u32)c * 0x11111111u;
	int w, ch, ox, off, k, r, nb = 0, used, last, y0, y1, bold = (font == F_BOLD), sh;
	int fixed = (font == F_FIXED);
	const unsigned char *g, *base = fixed ? font_fixed : font_bits;
	unsigned char *sp;
	unsigned short b;

	x += org_x;
	y += org_y;
	if (y >= clip.b || y + FONT_H <= clip.t || x >= clip.r || clip.l >= clip.r) {
		while (n-- > 0 && *s)
			x += char_w((unsigned char)*s++, font);
		return x - org_x;
	}
	ox = (x > clip.l ? x : clip.l) & ~7;
	last = (clip.r - 1 - ox) >> 3;		/* the last byte that can show */
	for (; n > 0 && *s; n--) {
		ch = (unsigned char)*s++;
		if (ch < 32 || ch > 126)
			ch = (ch == 0xA0 || ch == '\t') ? ' ' : '?';
		w = fixed ? FIXED_W : font_width[ch - 32] + bold;
		if (ch == ' ' || x + w <= clip.l || x >= clip.r) {
			x += w;
			continue;
		}
		if (x < ox) {				/* sticks out to the left */
			put_char_slow(x, y, ch, cv, font);
			x += w;
			continue;
		}
		off = x - ox;
		k = off >> 3;
		sh = 8 - (off & 7);
		g = base + ((ch - 32) << 3);
		sp = &strip[0][k];
		SROW(0); SROW(1); SROW(2); SROW(3);
		SROW(4); SROW(5); SROW(6); SROW(7);
		if (k + 2 > nb)
			nb = k + 2;
		x += w;
	}
	while (n-- > 0 && *s)
		x += char_w((unsigned char)*s++, font);
	used = nb;
	if (nb > last + 1)
		nb = last + 1;
	if (nb <= 0)
		return x - org_x;
	/* clip the first and last bytes */
	y0 = y < clip.t ? clip.t : y;
	y1 = y + FONT_H > clip.b ? clip.b : y + FONT_H;
	for (r = y0 - y; r < y1 - y; r++) {
		if (clip.l > ox)
			strip[r][0] &= 0xFF >> (clip.l - ox);
		if (nb == last + 1)
			strip[r][last] &= (0xFF << (7 - ((clip.r - 1 - ox) & 7))) & 0xFF;
		fb_strip(fb + rowbase[y + r] + ((ox >> 3) << 3), strip[r], nb, cv, expand);
	}
	for (r = 0; r < FONT_H; r++)		/* leave it all 0 */
		memset(strip[r], 0, used);
	mark_dirty(ox, y0, ox + nb * 8, y1);
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
	char buf[80];
	int n = 0, ul = -1, w;

	for (; *s && *s != '\t' && n < (int)sizeof buf - 1; s++) {
		if (*s == '&' && s[1] && s[1] != '&') {
			s++;
			ul = n;
		}
		buf[n++] = *s;
	}
	buf[n] = 0;
	w = textn(x, y, buf, n, c, font);
	if (ul >= 0) {
		int ux = x + textn_w(buf, ul, font);
		hline(ux, ux + char_w((unsigned char)buf[ul], font) - 1, y + FONT_H, c);
	}
	return w - x;
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
