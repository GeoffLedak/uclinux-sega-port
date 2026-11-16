/*
 * genesis_term.c -- VT100/ANSI terminal emulator for the Sega Genesis console
 *
 * Output written to the console (tty writes and printk) goes into a ring
 * buffer. Once per frame, from the vblank interrupt, genesis_term_vblank()
 * interprets a frame's worth of it: printable characters, control
 * characters and VT100/ANSI escape sequences. Characters are stored in
 * text_charmap and drawn into tiles by the routines in hw_md.S.
 *
 * Screen: the text is centred inside a margin (GENESIS_SET_MARGIN: 0 - 32
 * pixels in steps of 8, 16 at boot so that a CRT's overscan doesn't hide
 * any of it). With the 8x8 font that gives 40x28 (margin 0), 38x26 (8),
 * 36x24 (16), 34x22 (24) or 32x20 (32); the 6x8 font fits 53, 50, 48, 45
 * or 42 columns in the same rows.
 * Each screen row shows one "block" of tiles (row_block[]); scrolling
 * rotates the blocks and rewrites the name table instead of copying pixels.
 *
 * Supported (a subset of what xterm / the Linux console understand):
 *   BEL BS HT LF VT FF CR, ESC 7/8 (save/restore cursor), ESC D/M/E, ESC c
 *   CSI A B C D E F G H f d `  (cursor movement)
 *   CSI J K (erase), @ P X (insert/delete/erase characters),
 *   L M (insert/delete lines), S T (scroll), r (scroll region),
 *   s u (save/restore cursor), n (status/cursor position report),
 *   c (device attributes), 4h/4l (insert mode), ?7h/l (autowrap),
 *   ?25h/l (show/hide cursor), Ps SP q (cursor shape),
 *   m (attributes: 7 reverse video, 27 not reversed, 0 normal; others
 *   such as bold and colours are accepted and ignored).
 * Bytes 0x80-0xFF are drawn as Code Page 437 characters.
 *
 * Note: the kernel is not linked with libgcc, so this file must not use
 * 32-bit multiply/divide (only shifts and constant multiplies).
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/string.h>
#include <linux/genesis_console.h>

#include <asm/system.h>
#include <asm/segment.h>

/* Boot font and colour (keep TEXT_COLOR in hw_md.S the same as the colour) */
#define TERM_DEFAULT_FONT	8		/* 8 = 8x8 font, 6 = 6x8 font */
#define TERM_DEFAULT_MARGIN	16		/* pixels; the boot layout in hw_md.S */
#define TERM_DEFAULT_COLOR	0x0EEE		/* Genesis CRAM 0x0BGR */

/* Must match hw_md.S */
#define MAX_ROWS	28			/* TEXT_MAX_ROWS */
#define CURSOR_TILES	1121			/* CURSOR_TILES */
#define CELL(b, x)	text_charmap[((b) << 6) - ((b) << 3) + (x)]	/* TEXT_STRIDE = 56 */
#define ATTR(b, x)	text_attrmap[((b) << 6) - ((b) << 3) + (x)]	/* 1 = reverse */

/* hw_md.S */
extern void text_refresh(int block, int from, int to);
extern void text_zero_block(int block);
extern void text_map_row(int row, int block);
extern unsigned char text_charmap[], text_attrmap[];
extern long fontWidth, textCols, textRows;
extern long textFirstRow, textFirstCol, textTileCols;
extern void genesis_load_cursor_tiles(void);

extern struct tty_struct *genesis_tty_table[];

#define VDP_DATA16	(*(volatile unsigned short *)0xC00000)
#define VDP_DATA32	(*(volatile unsigned long *)0xC00000)
#define VDP_CTRL16	(*(volatile unsigned short *)0xC00004)
#define VDP_CTRL32	(*(volatile unsigned long *)0xC00004)
#define PSG_PORT	(*(volatile unsigned char *)0xC00011)

/* ---------------------------------------------------------------- */
/* Output ring buffer                                                */
/* ---------------------------------------------------------------- */

#define OBUF_SIZE	2048			/* power of 2 */
#define OBUF_MASK	(OBUF_SIZE - 1)

static unsigned char obuf[OBUF_SIZE];
static volatile unsigned short ohead;		/* next byte to write */
static volatile unsigned short otail;		/* next byte to draw */

static inline int obuf_used(void)
{
	return (ohead - otail) & OBUF_MASK;
}

static inline int obuf_free(void)
{
	return OBUF_SIZE - 1 - obuf_used();
}

/* printk path: never blocks, drops the character if the buffer is full */
void console_putchar(char c)
{
	unsigned long flags;

	save_flags(flags);
	cli();
	if (obuf_free() > 0) {
		obuf[ohead] = c;
		ohead = (ohead + 1) & OBUF_MASK;
	}
	restore_flags(flags);
}

/* tty path: accepts as many bytes as fit, returns how many */
int genesis_term_write(const unsigned char *buf, int count, int from_user)
{
	unsigned long flags;
	int i, n;

	save_flags(flags);
	cli();
	n = obuf_free();
	if (n > count)
		n = count;
	for (i = 0; i < n; i++) {
		obuf[ohead] = from_user ? get_user(buf + i) : buf[i];
		ohead = (ohead + 1) & OBUF_MASK;
	}
	restore_flags(flags);
	return n;
}

int genesis_term_write_room(void)
{
	return obuf_free();
}

int genesis_term_chars_in_buffer(void)
{
	return obuf_used();
}

/* ---------------------------------------------------------------- */
/* Terminal state                                                    */
/* ---------------------------------------------------------------- */

static int cols = 36, rows = 24;		/* the screen size (see the top) */
static int left_px = 16, top_px = 16;		/* where the text starts on screen */
static int cx, cy;				/* cursor column / row */
static char wrapnext;				/* cursor is past the last column */
static int top, bot = 24 - 1;			/* scroll region (inclusive) */
static char autowrap = 1;
static char insert_mode;			/* CSI 4h */
static char cursor_shown = 1;			/* CSI ?25h */
static char cursor_style;			/* CSI Ps SP q: 0-6 */
static int saved_x, saved_y;
static char attr;				/* for new characters: 1 = reverse */
static char saved_attr;

static unsigned char row_block[MAX_ROWS] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
	14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27
};
static unsigned char dirty_from[MAX_ROWS], dirty_to[MAX_ROWS];	/* per block */

/* escape sequence parser */
#define S_NORMAL	0
#define S_ESC		1
#define S_CSI		2
#define S_SKIP1		3			/* ignore one more byte */
#define NPAR		8
static char state;
static int par[NPAR];
static int npar;
static char priv;				/* '?', '>' ... after CSI */
static char inter;				/* intermediate byte (' ' for SP q) */

/* requests from the console driver, applied at the next vblank */
static volatile char pending_layout = 1;	/* font or margin set */
static volatile int pending_color;
static int cur_font = TERM_DEFAULT_FONT;
static int cur_margin = TERM_DEFAULT_MARGIN;
static int cur_color = TERM_DEFAULT_COLOR;
static int shown_margin = 16;			/* the layout on screen (hw_md.S's) */

static volatile char restore_pending;		/* graphics mode has ended */
static int budget;				/* work allowed this frame */
static int blink;
static int bell_timer;

/* ---------------------------------------------------------------- */
/* Screen helpers                                                    */
/* ---------------------------------------------------------------- */

static void mark(int b, int from, int to)
{
	if (dirty_from[b] >= dirty_to[b]) {
		dirty_from[b] = from;
		dirty_to[b] = to;
	} else {
		if (from < dirty_from[b])
			dirty_from[b] = from;
		if (to > dirty_to[b])
			dirty_to[b] = to;
	}
}

static void flush_dirty(void)
{
	int b;

	for (b = 0; b < MAX_ROWS; b++) {
		if (dirty_from[b] < dirty_to[b]) {
			text_refresh(b, dirty_from[b], dirty_to[b]);
			dirty_from[b] = dirty_to[b] = 0;
		}
	}
}

static void zero_block(int b)
{
	text_zero_block(b);
	dirty_from[b] = dirty_to[b] = 0;
	budget -= 4;
}

static void remap(int r0, int r1)
{
	for (; r0 <= r1; r0++)
		text_map_row(r0, row_block[r0]);
	budget -= 8;
}

/* scroll rows t..b up by n lines (blank lines appear at the bottom) */
static void scroll_up(int t, int b, int n)
{
	int blk;

	if (n > b - t + 1)
		n = b - t + 1;
	if (n <= 0)
		return;
	while (n--) {
		blk = row_block[t];
		memmove(&row_block[t], &row_block[t + 1], b - t);
		row_block[b] = blk;
		zero_block(blk);
	}
	remap(t, b);
}

/* scroll rows t..b down by n lines (blank lines appear at the top) */
static void scroll_down(int t, int b, int n)
{
	int blk;

	if (n > b - t + 1)
		n = b - t + 1;
	if (n <= 0)
		return;
	while (n--) {
		blk = row_block[b];
		memmove(&row_block[t + 1], &row_block[t], b - t);
		row_block[t] = blk;
		zero_block(blk);
	}
	remap(t, b);
}

/* erase columns [from, to) of screen row r */
static void erase(int r, int from, int to)
{
	int b = row_block[r];

	if (to > cols)
		to = cols;
	if (from < 0)
		from = 0;
	if (from >= to)
		return;
	if (from == 0 && to == cols) {
		zero_block(b);
		return;
	}
	memset(&CELL(b, from), 0, to - from);
	memset(&ATTR(b, from), 0, to - from);
	mark(b, from, to);
}

static void clear_screen(void)
{
	int r;

	for (r = 0; r < rows; r++)
		zero_block(row_block[r]);
}

static void linefeed(void)
{
	if (cy == bot)
		scroll_up(top, bot, 1);
	else if (cy < rows - 1)
		cy++;
}

static void reverse_index(void)
{
	if (cy == top)
		scroll_down(top, bot, 1);
	else if (cy > 0)
		cy--;
}

static void put_glyph(unsigned char c)
{
	int b;

	if (wrapnext) {
		wrapnext = 0;
		if (autowrap) {
			cx = 0;
			linefeed();
		}
	}
	b = row_block[cy];
	if (insert_mode && cx < cols - 1) {
		memmove(&CELL(b, cx + 1), &CELL(b, cx), cols - cx - 1);
		memmove(&ATTR(b, cx + 1), &ATTR(b, cx), cols - cx - 1);
		mark(b, cx, cols);
	}
	CELL(b, cx) = c;
	ATTR(b, cx) = attr;
	mark(b, cx, cx + 1);
	if (cx < cols - 1)
		cx++;
	else if (autowrap)
		wrapnext = 1;
	budget--;
}

static void bell(void)
{
	/* PSG tone channel 0, divider 127 = about 880 Hz */
	PSG_PORT = 0x80 | (127 & 0x0F);
	PSG_PORT = (127 >> 4) & 0x3F;
	PSG_PORT = 0x90 | 4;			/* volume (attenuation 4) */
	bell_timer = 6;				/* frames */
}

static void reset_terminal(void)
{
	cx = cy = 0;
	wrapnext = 0;
	top = 0;
	bot = rows - 1;
	autowrap = 1;
	insert_mode = 0;
	cursor_shown = 1;
	cursor_style = 0;
	saved_x = saved_y = 0;
	attr = saved_attr = 0;
	state = S_NORMAL;
	clear_screen();
}

/* the screen size for a font and margin (0 - 32, a multiple of 8) */
static const unsigned char cols_8x8[5] = { 40, 38, 36, 34, 32 };
static const unsigned char cols_6x8[5] = { 53, 50, 48, 45, 42 };

static int layout_cols(int font, int margin)
{
	return (font == 6) ? cols_6x8[margin >> 3] : cols_8x8[margin >> 3];
}

static int layout_rows(int margin)
{
	return 28 - (margin >> 2);		/* (224 - 2 * margin) / 8 */
}

/* plane B's horizontal scroll: the pixels a 6x8 layout leaves over */
static void set_hscroll(void)
{
	VDP_CTRL16 = 0x8F02;			/* INC = 2 */
	VDP_CTRL32 = 0x6C020002;		/* write VRAM 0xAC02 (plane B) */
	VDP_DATA16 = left_px & 7;
}

/* switch to a font and margin: lay the text out afresh, clear the screen */
static void apply_layout(int font, int margin)
{
	int r, width;

	fontWidth = font;
	cols = layout_cols(font, margin);
	rows = layout_rows(margin);
	width = (font == 6) ? (cols << 2) + (cols << 1) : cols << 3;
	left_px = (320 - width) >> 1;
	top_px = margin;
	textCols = cols;
	textRows = rows;
	textFirstRow = top_px >> 3;
	textFirstCol = left_px >> 3;
	textTileCols = (width + 7) >> 3;
	shown_margin = margin;

	/* plane B: blank, then the text rows */
	VDP_CTRL16 = 0x8F02;
	VDP_CTRL32 = 0x60000003;		/* write VRAM 0xE000 */
	for (r = 0; r < 64 * 32 / 2; r++)
		VDP_DATA32 = 0;
	set_hscroll();
	for (r = 0; r < MAX_ROWS; r++)
		row_block[r] = r;
	for (r = 0; r < rows; r++)
		text_map_row(r, r);
	/* blocks off the screen are blanked too: one could still hold
	 * characters past the new last column */
	for (r = rows; r < MAX_ROWS; r++)
		zero_block(r);
	reset_terminal();
}

/* send a reply (cursor position report etc.) as if it was typed */
static void respond(const char *s)
{
	genesis_tty_input(s, strlen(s));
}

/* ---------------------------------------------------------------- */
/* Escape sequences                                                  */
/* ---------------------------------------------------------------- */

/* parameter i, or def if it is missing or 0 */
static int P(int i, int def)
{
	return (i < npar && par[i]) ? par[i] : def;
}

static void clamp_cursor(void)
{
	if (cx < 0)
		cx = 0;
	if (cx > cols - 1)
		cx = cols - 1;
	if (cy < 0)
		cy = 0;
	if (cy > rows - 1)
		cy = rows - 1;
}

static void do_csi(unsigned char f)
{
	int n, i, b, lim;
	char tmp[24];

	wrapnext = 0;

	if (priv == '?') {
		if (f == 'h' || f == 'l') {
			for (i = 0; i < npar; i++) {
				if (par[i] == 7)
					autowrap = (f == 'h');
				else if (par[i] == 25)
					cursor_shown = (f == 'h');
			}
		}
		return;
	}
	if (priv)
		return;
	if (inter == ' ') {
		if (f == 'q')
			cursor_style = (npar > 0 && par[0] <= 6) ? par[0] : 0;
		return;
	}
	if (inter)
		return;

	switch (f) {
	case 'A':				/* cursor up */
	case 'F':				/* cursor up, column 1 */
		lim = (cy >= top) ? top : 0;
		cy -= P(0, 1);
		if (cy < lim)
			cy = lim;
		if (f == 'F')
			cx = 0;
		break;
	case 'B':				/* cursor down */
	case 'e':
	case 'E':				/* cursor down, column 1 */
		lim = (cy <= bot) ? bot : rows - 1;
		cy += P(0, 1);
		if (cy > lim)
			cy = lim;
		if (f == 'E')
			cx = 0;
		break;
	case 'C':				/* cursor right */
	case 'a':
		cx += P(0, 1);
		break;
	case 'D':				/* cursor left */
		cx -= P(0, 1);
		break;
	case 'G':				/* cursor to column */
	case '`':
		cx = P(0, 1) - 1;
		break;
	case 'd':				/* cursor to row */
		cy = P(0, 1) - 1;
		break;
	case 'H':				/* cursor to row;column */
	case 'f':
		cy = P(0, 1) - 1;
		cx = P(1, 1) - 1;
		break;
	case 'J':				/* erase in display */
		n = npar ? par[0] : 0;
		if (n == 0) {
			erase(cy, cx, cols);
			for (i = cy + 1; i < rows; i++)
				erase(i, 0, cols);
		} else if (n == 1) {
			for (i = 0; i < cy; i++)
				erase(i, 0, cols);
			erase(cy, 0, cx + 1);
		} else {
			clear_screen();
		}
		break;
	case 'K':				/* erase in line */
		n = npar ? par[0] : 0;
		if (n == 0)
			erase(cy, cx, cols);
		else if (n == 1)
			erase(cy, 0, cx + 1);
		else
			erase(cy, 0, cols);
		break;
	case 'L':				/* insert lines */
		if (cy >= top && cy <= bot) {
			scroll_down(cy, bot, P(0, 1));
			cx = 0;
		}
		break;
	case 'M':				/* delete lines */
		if (cy >= top && cy <= bot) {
			scroll_up(cy, bot, P(0, 1));
			cx = 0;
		}
		break;
	case '@':				/* insert blank characters */
		n = P(0, 1);
		if (n > cols - cx)
			n = cols - cx;
		b = row_block[cy];
		memmove(&CELL(b, cx + n), &CELL(b, cx), cols - cx - n);
		memset(&CELL(b, cx), 0, n);
		memmove(&ATTR(b, cx + n), &ATTR(b, cx), cols - cx - n);
		memset(&ATTR(b, cx), 0, n);
		mark(b, cx, cols);
		break;
	case 'P':				/* delete characters */
		n = P(0, 1);
		if (n > cols - cx)
			n = cols - cx;
		b = row_block[cy];
		memmove(&CELL(b, cx), &CELL(b, cx + n), cols - cx - n);
		memset(&CELL(b, cols - n), 0, n);
		memmove(&ATTR(b, cx), &ATTR(b, cx + n), cols - cx - n);
		memset(&ATTR(b, cols - n), 0, n);
		mark(b, cx, cols);
		break;
	case 'X':				/* erase characters */
		erase(cy, cx, cx + P(0, 1));
		break;
	case 'S':				/* scroll up */
		scroll_up(top, bot, P(0, 1));
		break;
	case 'T':				/* scroll down */
		scroll_down(top, bot, P(0, 1));
		break;
	case 'r':				/* set scroll region */
		i = P(0, 1) - 1;
		n = P(1, rows) - 1;
		if (i >= 0 && i < n && n < rows) {
			top = i;
			bot = n;
		}
		cx = cy = 0;
		break;
	case 'h':				/* set mode */
	case 'l':				/* reset mode */
		for (i = 0; i < npar; i++)
			if (par[i] == 4)
				insert_mode = (f == 'h');
		break;
	case 'n':				/* device status report */
		if (P(0, 0) == 5) {
			respond("\033[0n");
		} else if (P(0, 0) == 6) {
			sprintf(tmp, "\033[%d;%dR", cy + 1, cx + 1);
			respond(tmp);
		}
		break;
	case 'c':				/* device attributes: VT102 */
		if (P(0, 0) == 0)
			respond("\033[?6c");
		break;
	case 's':				/* save cursor */
		saved_x = cx;
		saved_y = cy;
		saved_attr = attr;
		break;
	case 'u':				/* restore cursor */
		cx = saved_x;
		cy = saved_y;
		attr = saved_attr;
		break;
	case 'm':				/* character attributes */
		if (npar == 0)
			attr = 0;
		for (i = 0; i < npar; i++) {
			if (par[i] == 0)
				attr = 0;
			else if (par[i] == 7)
				attr = 1;
			else if (par[i] == 27)
				attr = 0;
		}
		break;
	default:				/* g, ... ignored */
		break;
	}
	clamp_cursor();
}

static void term_byte(unsigned char c)
{
	/* control characters work in any state */
	if (c < 0x20 || c == 0x7F) {
		switch (c) {
		case 0x07:			/* BEL */
			if (state == S_NORMAL)
				bell();
			break;
		case 0x08:			/* BS */
			wrapnext = 0;
			if (cx > 0)
				cx--;
			break;
		case 0x09:			/* HT */
			wrapnext = 0;
			cx = (cx + 8) & ~7;
			if (cx > cols - 1)
				cx = cols - 1;
			break;
		case 0x0A:			/* LF, VT, FF */
		case 0x0B:
		case 0x0C:
			wrapnext = 0;
			linefeed();
			break;
		case 0x0D:			/* CR */
			wrapnext = 0;
			cx = 0;
			break;
		case 0x18:			/* CAN, SUB: abort sequence */
		case 0x1A:
			state = S_NORMAL;
			break;
		case 0x1B:			/* ESC */
			state = S_ESC;
			break;
		}
		return;
	}

	switch (state) {
	case S_NORMAL:
		put_glyph(c);
		break;

	case S_ESC:
		state = S_NORMAL;
		switch (c) {
		case '[':
			state = S_CSI;
			npar = 0;
			memset(par, 0, sizeof(par));
			priv = 0;
			inter = 0;
			break;
		case '7':			/* save cursor */
			saved_x = cx;
			saved_y = cy;
			saved_attr = attr;
			break;
		case '8':			/* restore cursor */
			cx = saved_x;
			cy = saved_y;
			attr = saved_attr;
			wrapnext = 0;
			clamp_cursor();
			break;
		case 'D':			/* index */
			wrapnext = 0;
			linefeed();
			break;
		case 'M':			/* reverse index */
			wrapnext = 0;
			reverse_index();
			break;
		case 'E':			/* next line */
			wrapnext = 0;
			cx = 0;
			linefeed();
			break;
		case 'c':			/* full reset */
			reset_terminal();
			break;
		case '(': case ')': case '*': case '+':
		case '#': case '%':		/* character sets etc: ignore */
			state = S_SKIP1;
			break;
		default:			/* ESC = / ESC > / ESC H ...: ignore */
			break;
		}
		break;

	case S_CSI:
		if (c >= '0' && c <= '9') {
			if (npar == 0)
				npar = 1;
			if (par[npar - 1] < 1000)
				par[npar - 1] = par[npar - 1] * 10 + (c - '0');
		} else if (c == ';') {
			if (npar == 0)
				npar = 1;
			if (npar < NPAR)
				npar++;
		} else if (c >= 0x3C && c <= 0x3F) {	/* < = > ? */
			priv = c;
		} else if (c >= 0x20 && c <= 0x2F) {
			inter = c;
		} else {
			state = S_NORMAL;
			if (c >= 0x40 && c <= 0x7E)
				do_csi(c);
		}
		break;

	case S_SKIP1:
		state = S_NORMAL;
		break;
	}
}

/* ---------------------------------------------------------------- */
/* Cursor sprite                                                     */
/* ---------------------------------------------------------------- */

static void draw_cursor(void)
{
	int x, y, tile, shape, steady;

	blink = (blink + 1) & 31;

	/* cursor_style: 0/1 blinking block, 2 block, 3 blinking underline,
	 * 4 underline, 5 blinking bar, 6 bar */
	shape = (cursor_style <= 2) ? 0 : (cursor_style <= 4) ? 2 : 4;
	steady = cursor_style && !(cursor_style & 1);
	tile = CURSOR_TILES + shape;
	if (shape != 4 && fontWidth == 6)
		tile++;				/* 6 pixel wide version */

	if (!cursor_shown || (!steady && blink >= 16))
		tile = 0;			/* blank */
	else if (ATTR(row_block[cy], cx))
		tile |= 0x2000;			/* palette 1, colour 1 = black: shows on reverse video */

	x = 128 + left_px + ((fontWidth == 6) ? (cx << 2) + (cx << 1) : (cx << 3));
	y = 128 + top_px + (cy << 3);

	VDP_CTRL16 = 0x8F02;			/* INC = 2 */
	VDP_CTRL32 = 0x68000002;		/* write VRAM 0xA800 (sprite table) */
	VDP_DATA16 = y;
	VDP_DATA16 = 0;				/* 8x8 sprite, end of list */
	VDP_DATA16 = tile;
	VDP_DATA16 = x;
}

/* ---------------------------------------------------------------- */
/* Called every frame from the vblank interrupt (hw_md.S)            */
/* ---------------------------------------------------------------- */

/* Put back everything graphics mode (genesis_gfx.c) changed */
static void restore_screen(void)
{
	int i, b;

	VDP_CTRL16 = 0x8F02;			/* INC = 2 */
	VDP_CTRL16 = 0x8407;			/* plane B name table back at 0xE000 */
	VDP_CTRL32 = 0x40000003;		/* VRAM 0xC000: plane A blank again */
	for (i = 0; i < 1024; i++)
		VDP_DATA32 = 0;
	VDP_CTRL32 = 0x40000000;		/* VRAM 0: tile 0 blank again */
	for (i = 0; i < 8; i++)
		VDP_DATA32 = 0;
	VDP_CTRL32 = 0xC0000000;		/* the palettes as at boot */
	for (i = 0; i < 64; i++)
		VDP_DATA16 = 0;
	VDP_CTRL32 = 0xC0020000;		/* entry 1: text colour */
	VDP_DATA16 = cur_color;
	VDP_CTRL32 = 0xC0420000;		/* entry 33: red */
	VDP_DATA16 = 0x000A;
	genesis_load_cursor_tiles();
	set_hscroll();				/* graphics mode zeroes it */
	for (b = 0; b < rows; b++)		/* and all the text */
		mark(row_block[b], 0, cols);
}

/* called by genesis_gfx.c when graphics mode ends */
void genesis_term_restore(void)
{
	restore_pending = 1;
}

void genesis_term_vblank(void)
{
	struct tty_struct *tty;
	int did_output = 0;
	unsigned char c;
	static char palette1_set;

	if (genesis_gfx_active) {
		/* the screen belongs to a graphics program: just move the
		 * pointer, and keep the output until the program is done */
		genesis_gfx_vblank();
		if (bell_timer && --bell_timer == 0)
			PSG_PORT = 0x9F;
		return;
	}
	if (restore_pending) {
		restore_pending = 0;
		restore_screen();
	}

	if (!palette1_set) {
		/* palette 1 colour 1 = black: the cursor over reverse video */
		VDP_CTRL16 = 0x8F02;
		VDP_CTRL32 = 0xC0220000;	/* write CRAM entry 17 */
		VDP_DATA16 = 0x0000;
		palette1_set = 1;
	}
	if (pending_color) {
		VDP_CTRL16 = 0x8F02;
		VDP_CTRL32 = 0xC0020000;	/* write CRAM entry 1 (text colour) */
		VDP_DATA16 = pending_color & 0x0EEE;
		pending_color = 0;
	}
	if (pending_layout) {
		pending_layout = 0;
		if (cur_font != fontWidth || cur_margin != shown_margin)
			apply_layout(cur_font, cur_margin);
	}

	/* draw up to a frame's worth of output (6x8 is slower to draw) */
	budget = (fontWidth == 6) ? 64 : 160;
	while (budget > 0 && otail != ohead) {
		c = obuf[otail];
		otail = (otail + 1) & OBUF_MASK;
		term_byte(c);
		did_output = 1;
	}
	flush_dirty();

	if (did_output) {
		blink = 0;			/* keep the cursor visible while output appears */
		tty = genesis_tty_table[0];
		if (tty) {
			wake_up_interruptible(&tty->write_wait);
			if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
			    tty->ldisc.write_wakeup)
				(tty->ldisc.write_wakeup)(tty);
		}
	}

	if (bell_timer && --bell_timer == 0)
		PSG_PORT = 0x9F;		/* channel 0 silent */

	draw_cursor();
}

/* ---------------------------------------------------------------- */
/* Settings (called from the console driver's ioctls)                */
/* ---------------------------------------------------------------- */

void genesis_term_set_font(int width)
{
	cur_font = width;
	pending_layout = 1;
}

int genesis_term_get_font(void)
{
	return cur_font;
}

void genesis_term_set_color(int color)
{
	cur_color = color & 0x0EEE;
	pending_color = 0x10000 | cur_color;
}

int genesis_term_get_color(void)
{
	return cur_color;
}

/* margin: 0 - 32, a multiple of 8 (checked by the caller) */
void genesis_term_set_margin(int margin)
{
	cur_margin = margin;
	pending_layout = 1;
}

int genesis_term_get_margin(void)
{
	return cur_margin;
}

/* the size as of the last setting (it shows from the next vblank) */
int genesis_term_cols(void)
{
	return layout_cols(cur_font, cur_margin);
}

int genesis_term_rows(void)
{
	return layout_rows(cur_margin);
}
