/*
 * nano -- a small text editor that looks and works like GNU nano.
 *
 *	nano [file]
 *
 * Written from scratch for uClinux on the Sega Genesis; it is not GNU nano,
 * only a look-alike with the everyday features: the title bar, the status
 * bar, the two lines of shortcuts, cutting and pasting whole lines (^K ^U),
 * the mark (^^), searching (^W), replacing (^\), go to line (^_), the
 * cursor position (^C), inserting a file (^R), saving (^O) and help (^G).
 *
 * The screen is drawn with VT100 codes, which the Genesis console
 * understands; reverse video marks the bars and the keys.  Only the parts
 * of the screen that changed are sent, because the console draws a few
 * hundred characters a frame at most.
 *
 * The file is kept in one "gap buffer": the text, with a gap at the place
 * being edited, so typing only has to fill the gap.  A file that doesn't
 * end in a newline gets one, as nano does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#define MAXR	64		/* biggest screen handled */
#define MAXC	128
#define TABSIZE	8
#define BLANK_AFTER 20		/* keystrokes a status message stays for */

/* ---------------------------------------------------------------- */
/* Gap buffer                                                       */

static char *buf;
static long cap, gs, ge;	/* size, gap start, gap end */
#define LEN	(cap - (ge - gs))
#define CH(p)	((unsigned char)buf[(p) < gs ? (p) : (p) + (ge - gs)])

static long cur;		/* cursor position in the text */
static long cur_line = 1;	/* its line number */
static int cur_row;		/* its row in the edit window */
static int want_col;		/* column up/down try to keep */
static long mark = -1;		/* start of the marked region, -1 none */
static int modified;
static char filename[256];

static char *cutbuf;		/* the cutbuffer */
static long cutlen, cutcap;
static int keep_cut;		/* the last key cut too: add to the cutbuffer */

static char last_search[128];
static char last_replace[128];

static void die(const char *msg);

static void move_gap(long p)
{
	if (p < gs) {
		memmove(buf + ge - (gs - p), buf + p, gs - p);
		ge -= gs - p;
		gs = p;
	} else if (p > gs) {
		memmove(buf + gs, buf + ge, p - gs);
		ge += p - gs;
		gs = p;
	}
}

static int make_room(long n)
{
	long ncap, tail;
	char *nb;

	if (ge - gs >= n)
		return 1;
	ncap = cap + n + 4096 + cap / 4;
	if (!(nb = malloc(ncap)))
		return 0;
	tail = cap - ge;
	memcpy(nb, buf, gs);
	memcpy(nb + ncap - tail, buf + ge, tail);
	free(buf);
	buf = nb;
	ge = ncap - tail;
	cap = ncap;
	return 1;
}

static int insert(long p, const char *s, long n)
{
	move_gap(p);
	if (!make_room(n))
		return 0;
	memcpy(buf + gs, s, n);
	gs += n;
	if (mark > p)
		mark += n;
	modified = 1;
	return 1;
}

static void delete(long p, long n)
{
	move_gap(p);
	ge += n;
	if (mark > p)
		mark = (mark - n > p) ? mark - n : p;
	modified = 1;
}

static long count_nl(long a, long b)
{
	long n = 0;

	for (; a < b; a++)
		if (CH(a) == '\n')
			n++;
	return n;
}

static long line_start(long p)
{
	while (p > 0 && CH(p - 1) != '\n')
		p--;
	return p;
}

static long line_end(long p)
{
	long len = LEN;

	while (p < len && CH(p) != '\n')
		p++;
	return p;
}

/* width on screen of character c at column x */
static int char_width(int c, int x)
{
	if (c == '\t')
		return TABSIZE - x % TABSIZE;
	if (c < 0x20 || c == 0x7F)
		return 2;			/* shown as ^X */
	return 1;
}

static int col_of(long p)
{
	long q = line_start(p);
	int x = 0;

	for (; q < p; q++)
		x += char_width(CH(q), x);
	return x;
}

/* position in the line starting at ls nearest to column col (not past it) */
static long pos_at_col(long ls, int col)
{
	long len = LEN;
	int x = 0, w;

	while (ls < len && CH(ls) != '\n') {
		w = char_width(CH(ls), x);
		if (x + w > col)
			break;
		x += w;
		ls++;
	}
	return ls;
}

/* ---------------------------------------------------------------- */
/* Terminal                                                         */

static struct termios saved_tty;
static int rows = 24, cols = 36;
static int edit_rows;			/* rows of text: rows - 4 */

static char obuf[4096];
static int olen;

static void flush_out(void)
{
	int n, done = 0;

	while (done < olen) {
		n = write(1, obuf + done, olen - done);
		if (n <= 0)
			break;
		done += n;
	}
	olen = 0;
}

static void out(const char *s)
{
	while (*s) {
		if (olen == sizeof obuf)
			flush_out();
		obuf[olen++] = *s++;
	}
}

static void outc(int c)
{
	if (olen == sizeof obuf)
		flush_out();
	obuf[olen++] = c;
}

static void goto_rc(int r, int c)
{
	char t[32];

	sprintf(t, "\033[%d;%dH", r + 1, c + 1);
	out(t);
}

static void tty_raw(void)
{
	struct termios t;

	tcgetattr(0, &saved_tty);
	t = saved_tty;
	t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
	t.c_iflag &= ~(IXON | IXOFF | ICRNL | INLCR | IGNCR | ISTRIP);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &t);
}

static void tty_restore(void)
{
	out("\033[0m\033[r\033[2J\033[H");
	flush_out();
	tcsetattr(0, TCSANOW, &saved_tty);
}

static void on_signal(int sig)
{
	tty_restore();
	_exit(1);
}

static void die(const char *msg)
{
	tty_restore();
	fprintf(stderr, "nano: %s\n", msg);
	exit(1);
}

static int key_waiting(void)
{
	long n = 0;

	return ioctl(0, FIONREAD, &n) == 0 && n > 0;
}

static int read_byte(void)
{
	unsigned char c;

	for (;;) {
		int n = read(0, &c, 1);
		if (n == 1)
			return c;
		if (n < 0 && errno == EINTR)
			continue;
		die("lost the keyboard");
	}
}

/* Keys: plain bytes, and these for the special keys */
#define K_UP	0x101
#define K_DOWN	0x102
#define K_LEFT	0x103
#define K_RIGHT	0x104
#define K_HOME	0x105
#define K_END	0x106
#define K_PGUP	0x107
#define K_PGDN	0x108
#define K_DEL	0x109
#define K_INS	0x10A
#define K_CLEFT	0x10B		/* Ctrl+Left */
#define K_CRIGHT 0x10C
#define K_CUP	0x10D
#define K_CDOWN	0x10E
#define K_NONE	0x1FF		/* an unknown sequence */
#define META	0x200		/* Alt+key or Esc, key */

static int get_key(void)
{
	int c = read_byte(), n, mod;

	if (c != 0x1B)
		return c;
	/* The keyboard sends a whole sequence at once, so Esc with nothing
	 * after it is Esc typed on its own: a Meta prefix for the next key */
	c = read_byte();
	if ((c != '[' && c != 'O') || !key_waiting()) {
		if (c == 0x1B)
			return K_NONE;
		return META | (isupper(c) ? tolower(c) : c);
	}
	n = 0;
	mod = 0;
	for (;;) {
		c = read_byte();
		if (c >= '0' && c <= '9') {
			n = n * 10 + c - '0';
		} else if (c == ';') {
			mod = n;
			n = 0;
		} else {
			break;
		}
	}
	if (mod)		/* ESC [ 1 ; 5 A: n is the modifier */
		mod = n;
	switch (c) {
	case 'A': return mod == 5 ? K_CUP : K_UP;
	case 'B': return mod == 5 ? K_CDOWN : K_DOWN;
	case 'C': return mod == 5 ? K_CRIGHT : K_RIGHT;
	case 'D': return mod == 5 ? K_CLEFT : K_LEFT;
	case 'H': return K_HOME;
	case 'F': return K_END;
	case '~':
		switch (n) {
		case 1: case 7: return K_HOME;
		case 2: return K_INS;
		case 3: return K_DEL;
		case 4: case 8: return K_END;
		case 5: return K_PGUP;
		case 6: return K_PGDN;
		}
	}
	return K_NONE;
}

/* ---------------------------------------------------------------- */
/* Screen                                                           */

static unsigned char want_ch[MAXR][MAXC], want_at[MAXR][MAXC];
static unsigned char have_ch[MAXR][MAXC], have_at[MAXR][MAXC];
static int full_redraw = 1;
static long drawn_top = -1;		/* top line when last drawn */
static int out_attr;			/* attribute the terminal is using */

struct shortcut {
	const char *key, *label;
};

static const struct shortcut main_keys[] = {
	{ "^G", "Help" }, { "^X", "Exit" },
	{ "^O", "Write Out" }, { "^R", "Read File" },
	{ "^W", "Where Is" }, { "^\\", "Replace" },
	{ "^K", "Cut" }, { "^U", "Paste" },
	{ "^C", "Location" }, { "^_", "Go To Line" },
	{ "M-6", "Copy" }, { "^^", "Set Mark" },
	{ 0, 0 }
};
static const struct shortcut cancel_keys[] = {
	{ "^C", "Cancel" }, { 0, 0 }
};
static const struct shortcut yesno_keys[] = {
	{ " Y", "Yes" }, { " N", "No" }, { "^C", "Cancel" }, { 0, 0 }
};
static const struct shortcut yesnoall_keys[] = {
	{ " Y", "Yes" }, { " N", "No" }, { " A", "All" }, { "^C", "Cancel" },
	{ 0, 0 }
};
static const struct shortcut help_keys[] = {
	{ "^Y", "Prev Page" }, { "^X", "Exit" },
	{ "^V", "Next Page" }, { 0, 0 }
};

/* What the bottom of the screen shows */
static const struct shortcut *keys = main_keys;
static char status_msg[MAXC];
static int status_left;			/* keystrokes until it is blanked */
static const char *prompt_msg;		/* a question, or 0 */
static char *prompt_text;		/* the answer being typed */
static int prompt_pos;

/* Help screen */
static int help_mode, help_top;
static const char *const help_text[] = {
	"Main nano help text",
	"",
	"This is a small nano look-alike",
	"for the Sega Genesis.  The text",
	"is shown in the middle, the",
	"shortcuts at the bottom.  ^ is",
	"the Ctrl key, M- is Alt (or Esc",
	"and then the key).",
	"",
	"^G       Show this help text",
	"^X       Close nano",
	"^O       Write (save) the file",
	"^R       Insert another file",
	"^W       Search forward",
	"M-W      Repeat the last search",
	"^\\       Replace text",
	"^K       Cut the line, or the",
	"         marked text",
	"^U       Paste the cutbuffer",
	"M-6      Copy the line, or the",
	"         marked text",
	"^^ M-A   Set or unset the mark",
	"^C       Show the cursor position",
	"^_       Go to line (and column)",
	"^L       Redraw the screen",
	"",
	"^A Home  Start of the line",
	"^E End   End of the line",
	"^Y PgUp  Previous page",
	"^V PgDn  Next page",
	"M-\\      First line",
	"M-/      Last line",
	"^P ^N    Line up / down",
	"^B ^F    Character left / right",
	"Ctrl+Left/Right  Word left/right",
	"^D Del   Delete the character",
	"         under the cursor",
	"^H Bksp  Delete the character",
	"         left of the cursor",
	"^I Tab   Insert a tab",
	"",
	"Searches ignore upper/lower case.",
	0
};

static void put_text(int r, int c, const char *s, int attr, int max)
{
	for (; *s && c < max; s++, c++) {
		want_ch[r][c] = *s;
		want_at[r][c] = attr;
	}
}

static void get_size(void)
{
	struct winsize ws;
	int r = rows, c = cols;

	if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
		r = ws.ws_row;
		c = ws.ws_col;
	}
	if (r > MAXR)
		r = MAXR;
	if (c > MAXC)
		c = MAXC;
	if (r < 6)
		r = 6;
	if (c < 16)
		c = 16;
	if (r != rows || c != cols)
		full_redraw = 1;
	rows = r;
	cols = c;
	edit_rows = rows - 4;
}

/* first column shown of a line whose cursor is at column x (as nano does) */
static int page_start(int x)
{
	if (x < cols - 1)
		return 0;
	return x - 7 - (x - 7) % (cols - 8);
}

static void draw_title(void)
{
	const char *name = filename[0] ? filename : "New Buffer";
	int n, at;

	if (help_mode)
		name = "Help";
	memset(want_at[0], 1, cols);
	put_text(0, 0, "  nano", 1, cols);
	n = strlen(name);
	at = (cols - n) / 2;
	if (at < 8)
		at = 8;
	put_text(0, at, name, 1, cols);
	if (modified && !help_mode && at + n + 10 <= cols)
		put_text(0, cols - 9, "Modified", 1, cols);
}

static void draw_line(int r, long p, int is_cur, int *cur_x)
{
	long len = LEN, a = -1, b = -1;
	int x = 0, start = 0, c, w, i, sx;

	if (mark >= 0) {
		a = mark < cur ? mark : cur;
		b = mark < cur ? cur : mark;
	}
	if (is_cur) {
		start = page_start(col_of(cur));
		*cur_x = col_of(cur) - start;
	}
	while (p <= len) {
		if (p == len || (c = CH(p)) == '\n')
			break;
		w = char_width(c, x);
		for (i = 0; i < w; i++) {
			sx = x + i - start;
			if (sx >= 0 && sx < cols) {
				int ch = ' ';
				if (c == '\t')
					ch = ' ';
				else if (c < 0x20 || c == 0x7F)
					ch = i == 0 ? '^' : (c == 0x7F ? '?' : c + 0x40);
				else
					ch = c;
				want_ch[r][sx] = ch;
				want_at[r][sx] = (p >= a && p < b);
			}
		}
		x += w;
		p++;
	}
	if (start > 0) {
		want_ch[r][0] = '$';
		want_at[r][0] = 0;
	}
	if (x - start > cols) {
		want_ch[r][cols - 1] = '$';
		want_at[r][cols - 1] = 0;
	}
}

static int item_width(const struct shortcut *s)
{
	return strlen(s->key) + 1 + strlen(s->label);
}

static void draw_bottom(int *cx, int *cy)
{
	int r = rows - 3, i, n, w, per, x, colw[MAXC / 4];
	char t[MAXC + 1];

	if (prompt_msg) {
		int plen = strlen(prompt_msg), off = 0;

		memset(want_at[r], 1, cols);
		put_text(r, 0, prompt_msg, 1, cols);
		/* scroll the answer if it doesn't fit */
		if (plen + prompt_pos >= cols)
			off = plen + prompt_pos - cols + 1;
		if (off < (int)strlen(prompt_text))
			put_text(r, plen > cols ? cols : plen, prompt_text + off, 1, cols);
		*cy = r;
		*cx = plen + prompt_pos - off;
		if (*cx >= cols)
			*cx = cols - 1;
	} else if (status_msg[0]) {
		n = strlen(status_msg);
		if (n > cols)
			n = cols;
		x = (cols - n) / 2;
		memcpy(t, status_msg, n);
		t[n] = 0;
		put_text(r, x, t, 1, cols);
	}

	/* shortcuts: two rows, filled column by column, as many columns as
	 * fit; each column is as wide as its longest shortcut, plus a share
	 * of the room left over */
	for (n = 0; keys[n].key; n++)
		;
	x = 0;
	for (per = 0; per * 2 < n; per++) {
		int a = item_width(&keys[per * 2]);
		int b = per * 2 + 1 < n ? item_width(&keys[per * 2 + 1]) : 0;
		int cw = a > b ? a : b;

		if (x + cw > cols)
			break;
		colw[per] = cw + 1;
		x += cw + 1;
	}
	if (per == 0) {
		per = 1;
		colw[0] = cols;
	} else if (per * 2 < n || x <= cols + 1) {
		w = (cols + 1 - x) / per;	/* share out the spare room */
		for (i = 0; i < per; i++)
			colw[i] += w;
	}
	for (i = 0, x = 0; i < n && i / 2 < per; i++) {
		int rr = rows - 2 + (i & 1), k;

		if (i && !(i & 1))
			x += colw[i / 2 - 1];
		k = strlen(keys[i].key);
		put_text(rr, x, keys[i].key, 1, cols);
		put_text(rr, x + k + 1, keys[i].label, 0, cols);
	}
}

static void flush_screen(int cx, int cy)
{
	int r, c, f, l, nb;

	for (r = 0; r < rows; r++) {
		for (f = 0; f < cols; f++)
			if (want_ch[r][f] != have_ch[r][f] ||
			    want_at[r][f] != have_at[r][f])
				break;
		if (f == cols)
			continue;
		for (l = cols - 1; l > f; l--)
			if (want_ch[r][l] != have_ch[r][l] ||
			    want_at[r][l] != have_at[r][l])
				break;
		for (nb = cols - 1; nb >= 0; nb--)
			if (want_ch[r][nb] != ' ' || want_at[r][nb])
				break;
		goto_rc(r, f);
		for (c = f; c <= l && c <= nb; c++) {
			if (want_at[r][c] != out_attr) {
				out_attr = want_at[r][c];
				out(out_attr ? "\033[7m" : "\033[0m");
			}
			outc(want_ch[r][c]);
		}
		if (l > nb) {		/* the rest of the row is blank: erase it */
			if (out_attr) {
				out_attr = 0;
				out("\033[0m");
			}
			out("\033[K");
		}
		memcpy(have_ch[r], want_ch[r], cols);
		memcpy(have_at[r], want_at[r], cols);
	}
	if (out_attr) {
		out_attr = 0;
		out("\033[0m");
	}
	goto_rc(cy, cx);
	flush_out();
}

/* The edit window moved by d lines since the last refresh: let the
 * terminal scroll it, which is much quicker than drawing it again. */
static void scroll_window(long d)
{
	char t[40];
	int r, n = d < 0 ? -d : d;

	sprintf(t, "\033[2;%dr\033[%d%c\033[r", edit_rows + 1, n, d > 0 ? 'S' : 'T');
	out(t);
	if (d > 0) {
		for (r = 1; r <= edit_rows - n; r++) {
			memcpy(have_ch[r], have_ch[r + n], cols);
			memcpy(have_at[r], have_at[r + n], cols);
		}
		for (; r <= edit_rows; r++) {
			memset(have_ch[r], ' ', cols);
			memset(have_at[r], 0, cols);
		}
	} else {
		for (r = edit_rows; r >= 1 + n; r--) {
			memcpy(have_ch[r], have_ch[r - n], cols);
			memcpy(have_at[r], have_at[r - n], cols);
		}
		for (; r >= 1; r--) {
			memset(have_ch[r], ' ', cols);
			memset(have_at[r], 0, cols);
		}
	}
}

static void refresh(void)
{
	long p, top_line;
	int r, i, cx = 0, cy = 0;

	get_size();
	if (cur_row >= edit_rows)
		cur_row = edit_rows - 1;
	if (cur_row > cur_line - 1)
		cur_row = cur_line - 1;
	if (cur_row < 0)
		cur_row = 0;

	for (r = 0; r < rows; r++) {
		memset(want_ch[r], ' ', cols);
		memset(want_at[r], 0, cols);
	}
	if (full_redraw) {
		out("\033[0m\033[r\033[2J");
		out_attr = 0;
		for (r = 0; r < rows; r++) {
			memset(have_ch[r], ' ', cols);
			memset(have_at[r], 0, cols);
		}
		drawn_top = -1;
		full_redraw = 0;
	}
	draw_title();

	if (help_mode) {
		for (r = 0; r < edit_rows && help_text[help_top + r]; r++)
			put_text(r + 1, 0, help_text[help_top + r], 0, cols);
		cx = 0;
		cy = rows - 3;
	} else {
		/* the line at the top of the window */
		p = line_start(cur);
		for (i = 0; i < cur_row; i++)
			p = line_start(p - 1);
		top_line = cur_line - cur_row;
		if (drawn_top > 0 && top_line != drawn_top &&
		    top_line - drawn_top < edit_rows && drawn_top - top_line < edit_rows)
			scroll_window(top_line - drawn_top);
		drawn_top = top_line;

		for (r = 0; r < edit_rows; r++) {
			int x = 0;

			draw_line(r + 1, p, r == cur_row, &x);
			if (r == cur_row) {
				cx = x;
				cy = r + 1;
			}
			p = line_end(p);
			if (p >= LEN)
				break;
			p++;
		}
	}
	draw_bottom(&cx, &cy);
	flush_screen(cx, cy);
}

static void message(const char *fmt, const char *arg)
{
	char t[300];

	sprintf(t, fmt, arg);
	t[MAXC - 1] = 0;
	strcpy(status_msg, t);
	status_left = BLANK_AFTER;
}

/* ---------------------------------------------------------------- */
/* Prompts                                                          */

/* Ask for some text; returns 1 when Enter is pressed, 0 for ^C */
static int ask(const char *msg, char *answer, int max)
{
	int k, n;

	prompt_msg = msg;
	prompt_text = answer;
	prompt_pos = strlen(answer);
	keys = cancel_keys;
	status_msg[0] = 0;
	for (;;) {
		refresh();
		k = get_key();
		n = strlen(answer);
		if (k == '\r' || k == '\n') {
			prompt_msg = 0;
			keys = main_keys;
			return 1;
		}
		if (k == 3 || k == 7) {			/* ^C, ^G */
			prompt_msg = 0;
			keys = main_keys;
			message("[ Cancelled ]", 0);
			return 0;
		}
		if ((k == 0x7F || k == 8) && prompt_pos > 0) {
			memmove(answer + prompt_pos - 1, answer + prompt_pos,
				n - prompt_pos + 1);
			prompt_pos--;
		} else if ((k == K_DEL || k == 4) && prompt_pos < n) {
			memmove(answer + prompt_pos, answer + prompt_pos + 1,
				n - prompt_pos);
		} else if ((k == K_LEFT || k == 2) && prompt_pos > 0) {
			prompt_pos--;
		} else if ((k == K_RIGHT || k == 6) && prompt_pos < n) {
			prompt_pos++;
		} else if (k == K_HOME || k == 1) {
			prompt_pos = 0;
		} else if (k == K_END || k == 5) {
			prompt_pos = n;
		} else if (k == 0x15 || k == 0x0B) {	/* ^U ^K: clear */
			answer[0] = 0;
			prompt_pos = 0;
		} else if (k >= 0x20 && k < 0x100 && k != 0x7F && n < max - 1) {
			memmove(answer + prompt_pos + 1, answer + prompt_pos,
				n - prompt_pos + 1);
			answer[prompt_pos++] = k;
		}
	}
}

/* Ask a yes/no question: returns 'y', 'n', 'a' (if allowed) or 0 for ^C */
static int ask_yes_no(const char *msg, int all)
{
	char none[1];
	int k;

	none[0] = 0;
	prompt_msg = msg;
	prompt_text = none;
	prompt_pos = 0;
	keys = all ? yesnoall_keys : yesno_keys;
	status_msg[0] = 0;
	for (;;) {
		refresh();
		k = get_key();
		if (k < 0x100 && isupper(k))
			k = tolower(k);
		if (k == 'y' || k == 'n' || (all && k == 'a') || k == 3) {
			prompt_msg = 0;
			keys = main_keys;
			if (k == 3) {
				message("[ Cancelled ]", 0);
				return 0;
			}
			return k;
		}
	}
}

/* ---------------------------------------------------------------- */
/* Files                                                            */

/* Read a file into the buffer at the cursor; returns lines read or -1 */
static long read_file(const char *name, int *is_new, int add_nl)
{
	struct stat st;
	char tmp[1024];
	long at = cur, lines = 0, n, total = 0;
	int fd, last = '\n';

	*is_new = 0;
	memset(&st, 0, sizeof st);
	if (stat(name, &st) == 0 && S_ISDIR(st.st_mode)) {
		message("[ \"%s\" is a directory ]", name);
		return -1;
	}
	if ((fd = open(name, O_RDONLY)) < 0) {
		if (errno == ENOENT) {
			*is_new = 1;
			return 0;
		}
		sprintf(tmp, "[ Error reading %%s: %s ]", strerror(errno));
		message(tmp, name);
		return -1;
	}
	if (st.st_size > 0 && !make_room(st.st_size + 1)) {
		close(fd);
		message("[ Not enough memory for %s ]", name);
		return -1;
	}
	while ((n = read(fd, tmp, sizeof tmp)) > 0) {
		if (!insert(at, tmp, n)) {
			message("[ Not enough memory for all of %s ]", name);
			break;
		}
		lines += count_nl(at, at + n);
		last = tmp[n - 1];
		at += n;
		total += n;
	}
	close(fd);
	if (add_nl && total > 0 && last != '\n') {	/* nano adds the final newline */
		insert(at, "\n", 1);
		lines++;
	}
	return lines;
}

/* returns 1 if written */
static int write_file(const char *name)
{
	int fd, ok;
	long lines = count_nl(0, LEN), len = LEN;
	char t[300];

	if ((fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
		sprintf(t, "[ Error writing %%s: %s ]", strerror(errno));
		message(t, name);
		return 0;
	}
	move_gap(len);			/* all the text in one piece */
	ok = write(fd, buf, len) == len;
	if (ok && len > 0 && buf[len - 1] != '\n') {
		ok = write(fd, "\n", 1) == 1;
		lines++;
	}
	if (close(fd) < 0)
		ok = 0;
	if (!ok) {
		sprintf(t, "[ Error writing %%s: %s ]", strerror(errno));
		message(t, name);
		return 0;
	}
	sprintf(t, "[ Wrote %ld line%s ]", lines, lines == 1 ? "" : "s");
	message("%s", t);
	modified = 0;
	return 1;
}

/* ^O, and saving on exit; returns 1 if saved */
static int write_out(void)
{
	char name[256];
	struct stat st;

	strcpy(name, filename);
	if (!ask("File Name to Write: ", name, sizeof name) || !name[0]) {
		if (!name[0])
			message("[ Cancelled ]", 0);
		return 0;
	}
	if (strcmp(name, filename) && stat(name, &st) == 0) {
		int a = ask_yes_no("File exists, OVERWRITE ? ", 0);
		if (a != 'y') {
			if (a == 'n')
				message("[ Cancelled ]", 0);
			return 0;
		}
	}
	if (!write_file(name))
		return 0;
	strcpy(filename, name);
	return 1;
}

/* ---------------------------------------------------------------- */
/* Moving around                                                    */

/* the cursor moved from line old_line: keep it on screen */
static void track(long old_line, int center)
{
	long r = cur_row + (cur_line - old_line);

	if (r >= 0 && r < edit_rows)
		cur_row = r;
	else if (center)
		cur_row = edit_rows / 2;
	else
		cur_row = r < 0 ? 0 : edit_rows - 1;
}

static void go_to(long p, int center)
{
	long old = cur_line;

	if (p > cur)
		cur_line += count_nl(cur, p);
	else
		cur_line -= count_nl(p, cur);
	cur = p;
	track(old, center);
	want_col = col_of(cur);
}

static void go_line(long n, int center)
{
	long p = 0, l = 1, len = LEN;

	while (l < n && p < len) {
		if (CH(p) == '\n')
			l++;
		p++;
	}
	go_to(line_start(p), center);
}

static void up(void)
{
	long ls = line_start(cur);

	if (ls == 0)
		return;
	cur = pos_at_col(line_start(ls - 1), want_col);
	cur_line--;
	track(cur_line + 1, 0);
}

static void down(void)
{
	long e = line_end(cur);

	if (e >= LEN)
		return;
	cur = pos_at_col(e + 1, want_col);
	cur_line++;
	track(cur_line - 1, 0);
}

static void page(int dir)
{
	int n = edit_rows - 2, row = cur_row, want = want_col;

	if (n < 1)
		n = 1;
	while (n--) {
		if (dir < 0)
			up();
		else
			down();
	}
	cur_row = row;		/* the text moves, the cursor stays put */
	want_col = want;
	cur = pos_at_col(line_start(cur), want_col);
}

static int is_word(int c)
{
	return isalnum(c) || c == '_' || c >= 0x80;
}

static void word_right(void)
{
	long len = LEN, p = cur;

	while (p < len && is_word(CH(p)))
		p++;
	while (p < len && !is_word(CH(p)))
		p++;
	go_to(p, 0);
}

static void word_left(void)
{
	long p = cur;

	while (p > 0 && !is_word(CH(p - 1)))
		p--;
	while (p > 0 && is_word(CH(p - 1)))
		p--;
	go_to(p, 0);
}

/* ---------------------------------------------------------------- */
/* Editing                                                          */

static void type_char(int c)
{
	char ch = c;

	if (!insert(cur, &ch, 1)) {
		message("[ Out of memory ]", 0);
		return;
	}
	cur++;
	if (c == '\n') {
		cur_line++;
		track(cur_line - 1, 0);
	}
	want_col = col_of(cur);
}

static void backspace(void)
{
	if (cur == 0)
		return;
	if (CH(cur - 1) == '\n') {
		cur_line--;
		track(cur_line + 1, 0);
	}
	delete(cur - 1, 1);
	cur--;
	want_col = col_of(cur);
}

static void delete_char(void)
{
	if (cur < LEN)
		delete(cur, 1);
}

/* put [a, b) in the cutbuffer, after what is there if keep */
static int to_cutbuf(long a, long b, int keep)
{
	long n = b - a, i;

	if (!keep)
		cutlen = 0;
	if (cutlen + n > cutcap) {
		long ncap = cutlen + n + 1024;
		char *nb = malloc(ncap);

		if (!nb) {
			message("[ Out of memory ]", 0);
			return 0;
		}
		if (cutbuf) {
			memcpy(nb, cutbuf, cutlen);
			free(cutbuf);
		}
		cutbuf = nb;
		cutcap = ncap;
	}
	for (i = 0; i < n; i++)
		cutbuf[cutlen++] = CH(a + i);
	return 1;
}

/* the marked region, or the cursor's line (with its newline) */
static void region(long *a, long *b)
{
	long e;

	if (mark >= 0) {
		*a = mark < cur ? mark : cur;
		*b = mark < cur ? cur : mark;
	} else {
		*a = line_start(cur);
		e = line_end(cur);
		*b = e < LEN ? e + 1 : e;
	}
}

static void cut(void)
{
	long a, b, old = cur_line;
	int was_marked = mark >= 0;

	region(&a, &b);
	if (a == b)
		return;
	if (!to_cutbuf(a, b, keep_cut && !was_marked))
		return;
	if (cur > a)
		cur_line -= count_nl(a, cur);
	mark = -1;
	delete(a, b - a);
	cur = a;
	track(old, 0);
	want_col = col_of(cur);
}

static void copy(void)
{
	long a, b;
	int was_marked = mark >= 0;

	region(&a, &b);
	if (a == b)
		return;
	if (!to_cutbuf(a, b, keep_cut && !was_marked))
		return;
	if (was_marked) {
		mark = -1;
		message("[ Copied ]", 0);
	} else {
		go_to(b, 0);	/* nano moves on to the next line */
	}
}

static void paste(void)
{
	long old = cur_line;

	if (!cutlen)
		return;
	if (!insert(cur, cutbuf, cutlen)) {
		message("[ Out of memory ]", 0);
		return;
	}
	cur_line += count_nl(cur, cur + cutlen);
	cur += cutlen;
	track(old, 1);
	want_col = col_of(cur);
}

static void insert_file(void)
{
	char name[256];
	long lines, old = cur_line;
	int is_new;
	char t[64];

	name[0] = 0;
	if (!ask("File to insert: ", name, sizeof name) || !name[0])
		return;
	lines = read_file(name, &is_new, 0);
	if (lines < 0)
		return;
	if (is_new) {
		message("[ \"%s\" not found ]", name);
		return;
	}
	sprintf(t, "[ Read %ld line%s ]", lines, lines == 1 ? "" : "s");
	message("%s", t);
	track(old, 1);
}

/* ---------------------------------------------------------------- */
/* Searching                                                        */

static int match_at(long p, const char *s, int n)
{
	int i;

	if (p + n > LEN)
		return 0;
	for (i = 0; i < n; i++)
		if (tolower(CH(p + i)) != tolower((unsigned char)s[i]))
			return 0;
	return 1;
}

/* next match after (or at, if at_too) position from; wraps; -1 if none */
static long find(const char *s, long from, int at_too, int *wrapped)
{
	long len = LEN, p;
	int n = strlen(s);

	*wrapped = 0;
	if (!n)
		return -1;
	for (p = at_too ? from : from + 1; p + n <= len; p++)
		if (match_at(p, s, n))
			return p;
	*wrapped = 1;
	for (p = 0; p <= from && p + n <= len; p++)
		if (match_at(p, s, n))
			return p;
	return -1;
}

static void do_search(const char *s)
{
	int wrapped;
	long was = cur, p = find(s, cur, 0, &wrapped);

	if (p < 0) {
		message("[ \"%s\" not found ]", s);
		return;
	}
	go_to(p, 1);
	if (p == was)
		message("[ This is the only occurrence ]", 0);
	else if (wrapped)
		message("[ Search Wrapped ]", 0);
}

static void make_prompt(char *t, const char *what, const char *prev)
{
	if (prev[0])
		sprintf(t, "%s [%.20s]: ", what, prev);
	else
		sprintf(t, "%s: ", what);
}

static void where_is(void)
{
	char t[80], s[128];

	make_prompt(t, "Search", last_search);
	s[0] = 0;
	if (!ask(t, s, sizeof s))
		return;
	if (s[0])
		strcpy(last_search, s);
	if (!last_search[0]) {
		message("[ Cancelled ]", 0);
		return;
	}
	do_search(last_search);
}

static void search_again(void)
{
	if (!last_search[0])
		message("[ No current search pattern ]", 0);
	else
		do_search(last_search);
}

static void replace(void)
{
	char t[80], s[128], r[128];
	long p, start = cur, count = 0;
	int wrapped, all = 0, a, sn, rn, passed = 0;

	make_prompt(t, "Search (to replace)", last_search);
	s[0] = 0;
	if (!ask(t, s, sizeof s))
		return;
	if (s[0])
		strcpy(last_search, s);
	if (!last_search[0]) {
		message("[ Cancelled ]", 0);
		return;
	}
	strcpy(s, last_search);
	if (find(s, cur, 1, &wrapped) < 0) {
		message("[ \"%s\" not found ]", s);
		return;
	}
	strcpy(r, last_replace);
	if (!ask("Replace with: ", r, sizeof r))
		return;
	strcpy(last_replace, r);
	mark = -1;
	sn = strlen(s);
	rn = strlen(r);

	p = cur;
	for (;;) {
		p = find(s, p, 1, &wrapped);
		if (p < 0)
			break;
		if (wrapped)
			passed = 1;
		if (passed && p >= start)	/* back where we began */
			break;
		go_to(p, 1);
		if (!all) {
			mark = p + sn;		/* show the match marked */
			a = ask_yes_no("Replace this instance?", 1);
			mark = -1;
			if (!a)
				break;
			if (a == 'a')
				all = 1;
			if (a == 'n') {
				p++;
				if (p > LEN)
					break;
				continue;
			}
		}
		delete(p, sn);
		insert(p, r, rn);
		count++;
		if (passed)
			start += rn - sn;
		p += rn;
		if (p > LEN)
			break;
	}
	sprintf(t, "[ Replaced %ld occurrence%s ]", count, count == 1 ? "" : "s");
	message("%s", t);
}

static void go_to_line(void)
{
	char s[32];
	long l = 0, c = 0;

	s[0] = 0;
	if (!ask("Enter line number, column number: ", s, sizeof s) || !s[0])
		return;
	if (sscanf(s, "%ld,%ld", &l, &c) < 1 && sscanf(s, "%ld %ld", &l, &c) < 1) {
		message("[ Invalid line or column number ]", 0);
		return;
	}
	if (l < 1)
		l = 1;
	go_line(l, 1);
	if (c > 1) {
		cur = pos_at_col(cur, c - 1);
		want_col = col_of(cur);
	}
	cur_row = edit_rows / 2;
}

static void location(void)
{
	long lines = count_nl(0, LEN) + 1, len = LEN;
	long le = line_end(cur);
	int col = col_of(cur) + 1, cols_in = col_of(le) + 1;
	char t[160];

	/* the longest form that fits */
	sprintf(t, "[ line %ld/%ld (%ld%%), col %d/%d (%d%%), char %ld/%ld (%ld%%) ]",
		cur_line, lines, cur_line * 100 / lines,
		col, cols_in, col * 100 / cols_in,
		cur + 1, len + 1, (cur + 1) * 100 / (len + 1));
	if ((int)strlen(t) > cols)
		sprintf(t, "[ line %ld/%ld, col %d/%d, char %ld/%ld ]",
			cur_line, lines, col, cols_in, cur + 1, len + 1);
	if ((int)strlen(t) > cols)
		sprintf(t, "[ line %ld/%ld, col %d/%d ]",
			cur_line, lines, col, cols_in);
	message("%s", t);
}

/* ---------------------------------------------------------------- */

static void help(void)
{
	int n, k;

	for (n = 0; help_text[n]; n++)
		;
	help_mode = 1;
	help_top = 0;
	keys = help_keys;
	status_msg[0] = 0;
	full_redraw = 1;
	for (;;) {
		refresh();
		k = get_key();
		if (k == 0x18 || k == 3 || k == 7 || k == 'q' || k == 'Q')
			break;
		if (k == K_DOWN || k == 0x0E)
			help_top++;
		else if (k == K_UP || k == 0x10)
			help_top--;
		else if (k == K_PGDN || k == 0x16 || k == ' ')
			help_top += edit_rows - 2;
		else if (k == K_PGUP || k == 0x19)
			help_top -= edit_rows - 2;
		if (help_top > n - edit_rows)
			help_top = n - edit_rows;
		if (help_top < 0)
			help_top = 0;
	}
	help_mode = 0;
	keys = main_keys;
	full_redraw = 1;
}

/* ^X: returns 1 to quit */
static int exit_nano(void)
{
	int a;

	if (!modified)
		return 1;
	a = ask_yes_no("Save modified buffer? ", 0);
	if (a == 'n')
		return 1;
	if (a == 'y')
		return write_out();
	return 0;
}

int main(int argc, char **argv)
{
	long lines;
	int is_new, k, was_cut;
	char t[64];

	if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
		fprintf(stderr, "usage: nano [file]\n");
		return 1;
	}
	cap = 8192;
	if (!(buf = malloc(cap))) {
		fprintf(stderr, "nano: out of memory\n");
		return 1;
	}
	gs = 0;
	ge = cap;

	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	tty_raw();
	get_size();

	if (argc == 2) {
		strncpy(filename, argv[1], sizeof filename - 1);
		lines = read_file(filename, &is_new, 1);
		if (is_new) {
			message("[ New File ]", 0);
		} else if (lines >= 0) {
			sprintf(t, "[ Read %ld line%s ]", lines, lines == 1 ? "" : "s");
			message("%s", t);
		}
	}
	modified = 0;
	cur = 0;
	cur_line = 1;
	cur_row = 0;

	for (;;) {
		if (!key_waiting())
			refresh();
		k = get_key();
		if (status_msg[0] && --status_left <= 0)
			status_msg[0] = 0;
		was_cut = 0;

		switch (k) {
		case 0x07:			/* ^G */
			help();
			break;
		case 0x18:			/* ^X */
			if (exit_nano()) {
				tty_restore();
				return 0;
			}
			break;
		case 0x0F:			/* ^O */
			write_out();
			break;
		case 0x12:			/* ^R */
		case K_INS:
			insert_file();
			break;
		case 0x17:			/* ^W */
			where_is();
			break;
		case META | 'w':
			search_again();
			break;
		case 0x1C:			/* ^\ */
		case META | 'r':
			replace();
			break;
		case 0x0B:			/* ^K */
			cut();
			was_cut = 1;
			break;
		case META | '6':
		case META | '^':
			copy();
			was_cut = 1;
			break;
		case 0x15:			/* ^U */
			paste();
			break;
		case 0x1E:			/* ^^ (Ctrl+6) */
		case META | 'a':
			if (mark >= 0) {
				mark = -1;
				message("[ Mark Unset ]", 0);
			} else {
				mark = cur;
				message("[ Mark Set ]", 0);
			}
			break;
		case 0x03:			/* ^C */
			location();
			break;
		case 0x1F:			/* ^_ */
		case META | 'g':
			go_to_line();
			break;
		case 0x0C:			/* ^L */
			full_redraw = 1;
			break;
		case K_UP: case K_CUP: case 0x10:	/* ^P */
			up();
			break;
		case K_DOWN: case K_CDOWN: case 0x0E:	/* ^N */
			down();
			break;
		case K_LEFT: case 0x02:		/* ^B */
			if (cur > 0)
				go_to(cur - 1, 0);
			break;
		case K_RIGHT: case 0x06:	/* ^F */
			if (cur < LEN)
				go_to(cur + 1, 0);
			break;
		case K_CLEFT:
			word_left();
			break;
		case K_CRIGHT:
			word_right();
			break;
		case K_HOME: case 0x01:		/* ^A */
			go_to(line_start(cur), 0);
			break;
		case K_END: case 0x05:		/* ^E */
			go_to(line_end(cur), 0);
			break;
		case K_PGUP: case 0x19:		/* ^Y */
			page(-1);
			break;
		case K_PGDN: case 0x16:		/* ^V */
			page(1);
			break;
		case META | '\\': case META | '|':
			go_to(0, 1);
			break;
		case META | '/': case META | '?':
			go_to(line_start(LEN), 1);
			break;
		case '\r': case '\n':
			type_char('\n');
			break;
		case 0x7F: case 0x08:		/* Backspace, ^H */
			backspace();
			break;
		case K_DEL: case 0x04:		/* Delete, ^D */
			delete_char();
			break;
		case 0x1A:			/* ^Z */
			message("[ Suspension is not enabled ]", 0);
			break;
		case 0x14:			/* ^T */
			message("[ Not available in this nano ]", 0);
			break;
		default:
			if (k == '\t' || (k >= 0x20 && k < 0x100))
				type_char(k);
			else
				message("[ Unknown Command ]", 0);
			break;
		}
		keep_cut = was_cut;
	}
}
