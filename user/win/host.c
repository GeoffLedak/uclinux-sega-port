/*
 * host.c -- run win on a PC for testing, driven by a script
 *
 *	cc -o win-host -DHOST *.c (see Makefile.host); ./win-host script
 *
 * Script lines (one action each, carried out whenever win waits):
 *	move X Y	put the pointer at X, Y
 *	down / up	left button;  rdown / rup: right button
 *	click [X Y]	(move and) press and release the left button
 *	dclick [X Y]	double click
 *	drag X Y	press, move to X, Y in steps, release
 *	key TEXT	type TEXT (\e = Esc, \r = Enter, \t = Tab, \xNN)
 *	wait N		let N frames pass
 *	shot FILE	save the screen as a PNG (2x size, with the pointer)
 *	quit		stop
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "win.h"

static FILE *script;
static unsigned long frames = 1000;
static int px = SCR_W / 2, py = SCR_H / 2, pb, shape = -1;
static unsigned short pal[64];
static u32 ptiles[8][32];
static int phx[8], phy[8];

static struct hw_input queue[64];
static int qh, qt, moved;
static char keys[256];
static int nkeys;
static int waiting;			/* frames left of a wait */

static void push(void)
{
	struct hw_input *e = &queue[qh];

	e->x = px;
	e->y = py;
	e->buttons = pb;
	e->pad = 0;
	e->time = frames;
	qh = (qh + 1) & 63;
}

/* ---------------- PNG writing (stored deflate) ---------------- */

static unsigned long crc_table[256];

static unsigned long crc(unsigned long c, const unsigned char *b, int n)
{
	int i, k;

	if (!crc_table[1])
		for (i = 0; i < 256; i++) {
			unsigned long v = i;
			for (k = 0; k < 8; k++)
				v = (v & 1) ? 0xEDB88320UL ^ (v >> 1) : v >> 1;
			crc_table[i] = v;
		}
	c ^= 0xFFFFFFFFUL;
	while (n--)
		c = crc_table[(c ^ *b++) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFUL;
}

static void put32(unsigned char *p, unsigned long v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void chunk(FILE *f, const char *type, const unsigned char *data, unsigned long n)
{
	unsigned char b[8];
	unsigned long c;

	put32(b, n);
	fwrite(b, 1, 4, f);
	fwrite(type, 1, 4, f);
	fwrite(data, 1, n, f);
	c = crc(0, (const unsigned char *)type, 4);
	c = crc(c, data, n);
	put32(b, c);
	fwrite(b, 1, 4, f);
}

static int level(int v)			/* Genesis 3-bit level -> 8 bits */
{
	static const int l[8] = { 0, 52, 87, 116, 144, 172, 206, 255 };
	return l[(v >> 1) & 7];
}

static void shot(const char *name)
{
	int W = SCR_W * 2, H = SCR_H * 2, x, y, c, row = 1 + W * 3;
	unsigned char *raw = malloc(row * H), *z, hdr[13];
	unsigned long n = row * H, zn, a = 1, b = 0, i, pos;
	FILE *f;

	for (y = 0; y < SCR_H; y++)
		for (x = 0; x < SCR_W; x++) {
			int sx = x - px + (shape >= 0 ? phx[shape] : 0);
			int sy = y - py + (shape >= 0 ? phy[shape] : 0);
			c = get_pixel(x, y);
			c = pal[c];
			if (shape >= 0 && sx >= 0 && sx < 16 && sy >= 0 && sy < 16) {
				int t = (sx >> 3) * 2 + (sy >> 3);
				int v = (ptiles[shape][t * 8 + (sy & 7)] >> ((7 - (sx & 7)) * 4)) & 15;
				if (v)
					c = pal[48 + v];
			}
			{
				int k, dy;
				for (dy = 0; dy < 2; dy++) {
					unsigned char *p = raw + (y * 2 + dy) * row;
					p[0] = 0;
					for (k = 0; k < 2; k++) {
						p[1 + (x * 2 + k) * 3] = level(c & 0xF);
						p[2 + (x * 2 + k) * 3] = level((c >> 4) & 0xF);
						p[3 + (x * 2 + k) * 3] = level((c >> 8) & 0xF);
					}
				}
			}
		}
	/* zlib stream with stored blocks */
	z = malloc(n + n / 65535 * 5 + 64);
	z[0] = 0x78; z[1] = 0x01;
	zn = 2;
	for (pos = 0; pos < n; ) {
		unsigned long len = n - pos > 65535 ? 65535 : n - pos;
		z[zn++] = (pos + len == n);
		z[zn++] = len & 0xFF; z[zn++] = len >> 8;
		z[zn++] = ~len & 0xFF; z[zn++] = (~len >> 8) & 0xFF;
		memcpy(z + zn, raw + pos, len);
		zn += len;
		pos += len;
	}
	for (i = 0; i < n; i++) {
		a = (a + raw[i]) % 65521;
		b = (b + a) % 65521;
	}
	put32(z + zn, (b << 16) | a);
	zn += 4;
	f = fopen(name, "wb");
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
	put32(hdr, W); put32(hdr + 4, H);
	hdr[8] = 8; hdr[9] = 2; hdr[10] = 0; hdr[11] = 0; hdr[12] = 0;
	chunk(f, "IHDR", hdr, 13);
	chunk(f, "IDAT", z, zn);
	chunk(f, "IEND", (const unsigned char *)"", 0);
	fclose(f);
	free(raw);
	free(z);
}

/* ---------------- the script ---------------- */

static void unescape(const char *s)
{
	while (*s && nkeys < (int)sizeof keys) {
		if (*s == '\\' && s[1]) {
			s++;
			switch (*s) {
			case 'e': keys[nkeys++] = 27; break;
			case 'r': keys[nkeys++] = 13; break;
			case 't': keys[nkeys++] = 9; break;
			case 'b': keys[nkeys++] = 127; break;
			case 'x': keys[nkeys++] = strtol(s + 1, 0, 16); s += 2; break;
			default: keys[nkeys++] = *s;
			}
			s++;
		} else
			keys[nkeys++] = *s++;
	}
}

static int drag_steps, drag_x, drag_y;

/* carry out the next script line; 0 at the end */
static int step(void)
{
	char line[256], cmd[32], arg[200];
	int a, b, n;

	if (drag_steps) {
		px += (drag_x - px) / drag_steps;
		py += (drag_y - py) / drag_steps;
		moved = 1;
		if (--drag_steps == 0) {
			pb &= ~B_LEFT;
			push();
		}
		return 1;
	}
	if (!script || !fgets(line, sizeof line, script)) {
		quit = 1;
		return 0;
	}
	line[strcspn(line, "\n")] = 0;
	cmd[0] = arg[0] = 0;
	n = sscanf(line, "%31s %d %d", cmd, &a, &b);
	if (!strcmp(cmd, "move") && n == 3) {
		px = a; py = b; moved = 1;
	} else if (!strcmp(cmd, "down")) {
		pb |= B_LEFT; push();
	} else if (!strcmp(cmd, "up")) {
		pb &= ~B_LEFT; push();
	} else if (!strcmp(cmd, "rdown")) {
		pb |= B_RIGHT; push();
	} else if (!strcmp(cmd, "rup")) {
		pb &= ~B_RIGHT; push();
	} else if (!strcmp(cmd, "click") || !strcmp(cmd, "dclick")) {
		if (n == 3) { px = a; py = b; moved = 1; }
		pb |= B_LEFT; push(); pb &= ~B_LEFT; push();
		if (cmd[0] == 'd') { pb |= B_LEFT; push(); pb &= ~B_LEFT; push(); }
	} else if (!strcmp(cmd, "rclick")) {
		if (n == 3) { px = a; py = b; moved = 1; }
		pb |= B_RIGHT; push(); pb &= ~B_RIGHT; push();
	} else if (!strcmp(cmd, "drag") && n == 3) {
		pb |= B_LEFT; push();
		drag_steps = 8; drag_x = a; drag_y = b;
	} else if (!strcmp(cmd, "key")) {
		unescape(line + 4);
	} else if (!strcmp(cmd, "wait") && n >= 2) {
		waiting = a;
	} else if (!strcmp(cmd, "shot")) {
		sscanf(line, "%*s %199s", arg);
		repaint_now();
		shot(arg);
	} else if (!strcmp(cmd, "quit")) {
		quit = 1;
		return 0;
	}
	return 1;
}

/* ---------------- hw interface ---------------- */

int hw_init(void)
{
	static const unsigned short palette[16] = {
		0x000, 0x008, 0x080, 0x088, 0x800, 0x808, 0x880, 0xCCC,
		0x888, 0x00E, 0x0E0, 0x0EE, 0xE00, 0xE0E, 0xEE0, 0xEEE,
	};
	int i;

	memcpy(pal, palette, sizeof palette);
	pal[49] = 0x000;
	pal[50] = 0xEEE;
	for (i = 0; i < PTR_COUNT; i++)
		hw_pointer_define(i, pointer_hot[2 * i], pointer_hot[2 * i + 1],
				  pointer_tiles[i]);
	return 0;
}

void hw_exit(void)
{
}

void hw_flush(void)
{
	memset(dirty, 0, sizeof dirty);
}

void hw_palette(int first, int n, const unsigned short *c)
{
	memcpy(pal + first, c, n * 2);
}

void hw_pointer_define(int s, int hx, int hy, const u32 *tiles)
{
	memcpy(ptiles[s], tiles, sizeof ptiles[s]);
	phx[s] = hx;
	phy[s] = hy;
}

void hw_pointer(int s)
{
	shape = s;
}

int hw_input(struct hw_input *e)
{
	if (qt != qh) {
		*e = queue[qt];
		qt = (qt + 1) & 63;
		return 1;
	}
	e->x = px;
	e->y = py;
	e->buttons = pb;
	e->pad = 0;
	e->time = frames;
	moved = 0;
	return 0;
}

void hw_wait(int n)
{
	if (waiting > 0) {
		if (n > waiting)
			n = waiting;
		frames += n;
		waiting -= n;
		return;
	}
	frames++;
	if (!step())
		exit(0);		/* the end of the script */
}

int hw_keys(char *buf, int n)
{
	int k = nkeys < n ? nkeys : n;

	memcpy(buf, keys, k);
	memmove(keys, keys + k, nkeys - k);
	nkeys -= k;
	return k;
}

void hw_warp(int x, int y)
{
	px = x;
	py = y;
}

void hw_beep(void)
{
	fprintf(stderr, "(beep)\n");
}

unsigned long hw_frames(void)
{
	return frames;
}

void host_open_script(const char *name)
{
	script = fopen(name, "r");
	if (!script) {
		perror(name);
		exit(1);
	}
}
