/*
 * genesis_term.c -- VT100/ANSI terminal emulator for the Sega Genesis console
 *
 * Output written to the console (tty writes and printk) goes into a ring
 * buffer. Once per frame, from the vblank interrupt, genesis_term_vblank()
 * interprets a frame's worth of it: printable characters, control
 * characters and VT100/ANSI escape sequences. Characters are stored in
 * text_charmap and drawn into tiles by the routines in hw_md.S.
 *
 * Screen: 24 rows x 36 columns (8x8 font) or 48 columns (6x8 font).
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
 *   m (attributes: accepted and ignored - there are no colours yet).
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
#define TERM_DEFAULT_COLOR	0x0EEE		/* Genesis CRAM 0x0BGR */

/* Must match hw_md.S */
#define ROWS		24			/* TEXT_ROWS */
#define CURSOR_TILES	865			/* CURSOR_TILES */
#define CELL(b, x)	text_charmap[((b) << 5) + ((b) << 4) + (x)]	/* TEXT_COLS = 48 */

/* hw_md.S */
extern void text_refresh(int block, int from, int to);
extern void text_zero_block(int block);
extern void text_map_row(int row, int block);
extern unsigned char text_charmap[];
extern long fontWidth, textCols;

extern struct tty_struct *genesis_tty_table[];

#define VDP_DATA16	(*(volatile unsigned short *)0xC00000)
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

static int cols = 36;				/* 36 or 48 */
static int cx, cy;				/* cursor column / row */
static char wrapnext;				/* cursor is past the last column */
static int top, bot = ROWS - 1;			/* scroll region (inclusive) */
static char autowrap = 1;
static char insert_mode;			/* CSI 4h */
static char cursor_shown = 1;			/* CSI ?25h */
static char cursor_style;			/* CSI Ps SP q: 0-6 */
static int saved_x, saved_y;

static unsigned char row_block[ROWS] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
	12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23
};
static unsigned char dirty_from[ROWS], dirty_to[ROWS];	/* per block */

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
static volatile int pending_font = TERM_DEFAULT_FONT;
static volatile int pending_color;
static int cur_font = TERM_DEFAULT_FONT;
static int cur_color = TERM_DEFAULT_COLOR;

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

	for (b = 0; b < ROWS; b++) {
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
	mark(b, from, to);
}

static void clear_screen(void)
{
	int r;

	for (r = 0; r < ROWS; r++)
		zero_block(row_block[r]);
}

static void linefeed(void)
{
	if (cy == bot)
		scroll_up(top, bot, 1);
	else if (cy < ROWS - 1)
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
		mark(b, cx, cols);
	}
	CELL(b, cx) = c;
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
	bot = ROWS - 1;
	autowrap = 1;
	insert_mode = 0;
	cursor_shown = 1;
	cursor_style = 0;
	saved_x = saved_y = 0;
	state = S_NORMAL;
	clear_screen();
}

static void apply_font(int width)
{
	int r;

	fontWidth = width;
	cols = (width == 6) ? 48 : 36;
	textCols = cols;
	for (r = 0; r < ROWS; r++) {
		row_block[r] = r;
		text_map_row(r, r);
	}
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
	if (cy > ROWS - 1)
		cy = ROWS - 1;
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
		lim = (cy <= bot) ? bot : ROWS - 1;
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
			for (i = cy + 1; i < ROWS; i++)
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
		mark(b, cx, cols);
		break;
	case 'P':				/* delete characters */
		n = P(0, 1);
		if (n > cols - cx)
			n = cols - cx;
		b = row_block[cy];
		memmove(&CELL(b, cx), &CELL(b, cx + n), cols - cx - n);
		memset(&CELL(b, cols - n), 0, n);
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
		n = P(1, ROWS) - 1;
		if (i >= 0 && i < n && n < ROWS) {
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
		break;
	case 'u':				/* restore cursor */
		cx = saved_x;
		cy = saved_y;
		break;
	default:				/* m (attributes), g, ... ignored */
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
			break;
		case '8':			/* restore cursor */
			cx = saved_x;
			cy = saved_y;
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
	if (shape != 4 && cols == 48)
		tile++;				/* 6 pixel wide version */

	if (!cursor_shown || (!steady && blink >= 16))
		tile = 0;			/* blank */

	x = 128 + 16 + ((cols == 48) ? (cx << 2) + (cx << 1) : (cx << 3));
	y = 128 + 16 + (cy << 3);

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

void genesis_term_vblank(void)
{
	struct tty_struct *tty;
	int did_output = 0;
	unsigned char c;

	if (pending_color) {
		VDP_CTRL16 = 0x8F02;
		VDP_CTRL32 = 0xC0020000;	/* write CRAM entry 1 (text colour) */
		VDP_DATA16 = pending_color & 0x0EEE;
		pending_color = 0;
	}
	if (pending_font) {
		if (pending_font != fontWidth)
			apply_font(pending_font);
		pending_font = 0;
	}

	/* draw up to a frame's worth of output (6x8 is slower to draw) */
	budget = (cols == 48) ? 64 : 160;
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
	pending_font = width;
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

int genesis_term_cols(void)
{
	return (cur_font == 6) ? 48 : 36;
}

int genesis_term_rows(void)
{
	return ROWS;
}
