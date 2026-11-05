/*
 * lineedit.c -- interactive command line editing for sash
 *
 * Used instead of fgets() when the shell reads commands from a terminal.
 * Puts the tty in raw mode while a line is being typed and talks VT100/ANSI
 * to it, so it works on the Genesis VDP console and on a serial terminal.
 *
 *   Left / Right, Ctrl-B / Ctrl-F       move one character
 *   Ctrl-Left / Ctrl-Right, Alt-B/F     move one word
 *   Home / End, Ctrl-A / Ctrl-E         start / end of line
 *   Backspace, Ctrl-H                   delete the character before the cursor
 *   Delete, Ctrl-D                      delete the character under the cursor
 *   Insert                              toggle insert / overwrite (insert is
 *                                       the default; underline cursor = insert,
 *                                       block cursor = overwrite)
 *   Up / Down, Ctrl-P / Ctrl-N          previous / next command in history
 *   Tab                                 complete a command or file name;
 *                                       press twice to list the choices
 *   Ctrl-K                              delete to end of line
 *   Ctrl-U                              delete to start of line
 *   Ctrl-W, Alt-Backspace               delete the word before the cursor
 *   Alt-D                               delete the word after the cursor
 *   Ctrl-Y                              paste the last deleted text
 *   Ctrl-T                              swap the two characters before the cursor
 *   Ctrl-L                              clear the screen
 *   Ctrl-C                              abandon the line
 *   Enter, Ctrl-J, Ctrl-M               run the line
 */

#include "sash.h"

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#define HIST_MAX	16		/* commands remembered */
#define MAX_CAND	64		/* tab completion candidates */
#define CAND_POOL	1024		/* bytes of candidate names */

#define ESC		0x1B
#undef CTRL
#define CTRL(c)		((c) & 0x1F)

/* special keys returned by read_key() */
#define K_UP		0x100
#define K_DOWN		0x101
#define K_LEFT		0x102
#define K_RIGHT		0x103
#define K_HOME		0x104
#define K_END		0x105
#define K_INSERT	0x106
#define K_DELETE	0x107
#define K_WORDLEFT	0x108
#define K_WORDRIGHT	0x109
#define K_WORDRUBOUT	0x10A
#define K_WORDDELETE	0x10B
#define K_NONE		0x1FF
#define K_EOF		(-1)

static char	*hist[HIST_MAX];
static int	hist_count;
static BOOL	insert_mode = TRUE;
static char	killbuf[CMDLEN];

/* the line being edited */
static char	*line;
static int	size;			/* capacity of line, including the NUL */
static int	len;			/* characters in line */
static int	pos;			/* cursor index into line */
static int	cpos;			/* index the terminal cursor is at */
static const char *prompt;
static int	plen;			/* prompt width on screen */
static int	cols;

/* tab completion */
static char	pool[CAND_POOL];
static int	pool_used;
static char	*cand[MAX_CAND];
static int	ncand;

/*
 * Output, buffered so that a redraw goes to the console in one write.
 */
static char	obuf[256];
static int	olen;

static void
oflush()
{
	int	off = 0;
	int	n;

	while (off < olen) {
		n = write(STDOUT, obuf + off, olen - off);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		off += n;
	}
	olen = 0;
}

static void
oputc(c)
	int	c;
{
	if (olen >= sizeof(obuf))
		oflush();
	obuf[olen++] = c;
}

static void
oputs(s)
	const char	*s;
{
	while (*s)
		oputc(*s++);
}

/* CSI n <c>, for n >= 1 */
static void
ocsi(n, c)
	int	n;
	int	c;
{
	char	digits[6];
	int	i = 0;

	oputc(ESC);
	oputc('[');
	if (n != 1) {
		do {
			digits[i++] = '0' + n % 10;
			n /= 10;
		} while (n);
		while (i)
			oputc(digits[--i]);
	}
	oputc(c);
}

static void
bell()
{
	oputc(7);
	oflush();
}

/* Underline cursor in insert mode, block cursor in overwrite mode. */
static void
show_mode()
{
	oputs(insert_mode ? "\033[3 q" : "\033[1 q");
}

/*
 * Screen position of line index i, relative to the start of the prompt.
 * The terminal cursor is always kept at cpos.  When the text ends exactly
 * at the right margin a newline is output, so the cursor never sits in the
 * terminal's "pending wrap" state.
 */
static void
move_cursor(to)
	int	to;
{
	int	r0 = (plen + cpos) / cols;
	int	c0 = (plen + cpos) % cols;
	int	r1 = (plen + to) / cols;
	int	c1 = (plen + to) % cols;

	if (r1 < r0)
		ocsi(r0 - r1, 'A');
	else if (r1 > r0)
		ocsi(r1 - r0, 'B');
	if (c1 < c0) {
		if (c1 == 0)
			oputc('\r');
		else
			ocsi(c0 - c1, 'D');
	} else if (c1 > c0)
		ocsi(c1 - c0, 'C');
	cpos = to;
}

/* Redraw the line from index 'from' onwards and put the cursor at pos. */
static void
redraw_from(from)
	int	from;
{
	int	i;

	move_cursor(from);
	oputs("\033[J");
	for (i = from; i < len; i++)
		oputc(line[i]);
	if (len > from) {
		cpos = len;
		if ((plen + len) % cols == 0)
			oputc('\n');
	}
	move_cursor(pos);
	oflush();
}

/* Draw the prompt and the line on a fresh screen line. */
static void
draw_all()
{
	oputs(prompt);
	if (plen > 0 && plen % cols == 0)
		oputc('\n');
	cpos = 0;
	redraw_from(0);
}

/* Width of the prompt on screen, skipping escape sequences. */
static int
prompt_width(p)
	const char	*p;
{
	int	w = 0;

	while (*p) {
		if (*p == ESC) {
			p++;
			if (*p == '[') {
				p++;
				while (*p && (*p < 0x40 || *p > 0x7E))
					p++;
			}
			if (*p)
				p++;
			continue;
		}
		if ((unsigned char) *p >= 0x20 && *p != 0x7F)
			w++;
		p++;
	}
	return w;
}

/*
 * Editing primitives.
 */
static void
insert_text(s, n)
	const char	*s;
	int		n;
{
	if (n > size - 1 - len)
		n = size - 1 - len;
	if (n <= 0) {
		bell();
		return;
	}
	memmove(line + pos + n, line + pos, len - pos);
	memcpy(line + pos, s, n);
	len += n;
	pos += n;
	redraw_from(pos - n);
}

static void
type_char(c)
	int	c;
{
	char	ch = c;

	if (!insert_mode && pos < len) {
		line[pos++] = ch;
		redraw_from(pos - 1);
		return;
	}
	if (pos == len && len < size - 1) {
		/* appending: no need to redraw anything */
		line[len++] = ch;
		pos = len;
		oputc(ch);
		cpos = len;
		if ((plen + len) % cols == 0)
			oputc('\n');
		oflush();
		return;
	}
	insert_text(&ch, 1);
}

/* Remove line[from..to), optionally saving it for Ctrl-Y. */
static void
delete_range(from, to, save)
	int	from;
	int	to;
	BOOL	save;
{
	if (from >= to)
		return;
	if (save) {
		memcpy(killbuf, line + from, to - from);
		killbuf[to - from] = '\0';
	}
	memmove(line + from, line + to, len - to);
	len -= to - from;
	pos = from;
	redraw_from(from);
}

static void
set_line(s)
	const char	*s;
{
	len = strlen(s);
	if (len > size - 1)
		len = size - 1;
	memcpy(line, s, len);
	pos = len;
	redraw_from(0);
}

static BOOL
is_word(c)
	int	c;
{
	return isalnum((unsigned char) c) || c == '_';
}

static int
word_left(i)
	int	i;
{
	while (i > 0 && !is_word(line[i - 1]))
		i--;
	while (i > 0 && is_word(line[i - 1]))
		i--;
	return i;
}

static int
word_right(i)
	int	i;
{
	while (i < len && !is_word(line[i]))
		i++;
	while (i < len && is_word(line[i]))
		i++;
	return i;
}

/*
 * History.
 */
static void
hist_add(s)
	const char	*s;
{
	char	*copy;
	int	i;

	while (*s == ' ')
		s++;
	if (*s == '\0')
		return;
	if (hist_count && strcmp(hist[hist_count - 1], s) == 0)
		return;
	copy = malloc(strlen(s) + 1);
	if (copy == NULL)
		return;
	strcpy(copy, s);
	if (hist_count == HIST_MAX) {
		free(hist[0]);
		for (i = 1; i < HIST_MAX; i++)
			hist[i - 1] = hist[i];
		hist_count--;
	}
	hist[hist_count++] = copy;
}

void
do_history(argc, argv)
	int	argc;
	char	**argv;
{
	int	i;

	for (i = 0; i < hist_count; i++)
		printf("%3d  %s\n", i + 1, hist[i]);
}

void
do_clear(argc, argv)
	int	argc;
	char	**argv;
{
	fputs("\033[H\033[2J", stdout);
	fflush(stdout);
}

/*
 * Tab completion.
 */
static void
add_cand(name, nlen, slash)
	const char	*name;
	int		nlen;
	BOOL		slash;
{
	int	i;
	int	need = nlen + slash + 1;

	for (i = 0; i < ncand; i++)
		if (strncmp(cand[i], name, nlen) == 0
		    && cand[i][nlen] == (slash ? '/' : '\0'))
			return;
	if (ncand >= MAX_CAND || pool_used + need > CAND_POOL)
		return;
	cand[ncand] = pool + pool_used;
	memcpy(cand[ncand], name, nlen);
	if (slash)
		cand[ncand][nlen] = '/';
	cand[ncand][nlen + slash] = '\0';
	pool_used += need;
	ncand++;
}

/* Add the entries of dir that start with prefix. */
static void
scan_dir(dir, prefix, plen_, mark_dirs)
	const char	*dir;
	const char	*prefix;
	int		plen_;
	BOOL		mark_dirs;
{
	DIR		*d;
	struct dirent	*de;
	struct stat	st;
	char		path[PATHLEN];
	BOOL		isdir;

	d = opendir(dir);
	if (d == NULL)
		return;
	while ((de = readdir(d)) != NULL) {
		if (strncmp(de->d_name, prefix, plen_) != 0)
			continue;
		if (de->d_name[0] == '.' && prefix[0] != '.')
			continue;
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		isdir = FALSE;
		if (mark_dirs && strlen(dir) + strlen(de->d_name) + 2 < PATHLEN) {
			strcpy(path, dir);
			strcat(path, "/");
			strcat(path, de->d_name);
			isdir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
		}
		add_cand(de->d_name, strlen(de->d_name), isdir);
	}
	closedir(d);
}

static int
cand_compare(a, b)
	const void	*a;
	const void	*b;
{
	return strcmp(*(char **) a, *(char **) b);
}

/* Print the candidates in columns below the line, then redraw it. */
static void
list_cands()
{
	int	i, w, maxw = 0, per;

	qsort(cand, ncand, sizeof(char *), cand_compare);
	for (i = 0; i < ncand; i++) {
		w = strlen(cand[i]);
		if (w > maxw)
			maxw = w;
	}
	per = cols / (maxw + 2);
	if (per < 1)
		per = 1;

	move_cursor(len);
	if ((plen + len) % cols != 0)
		oputc('\n');
	for (i = 0; i < ncand; i++) {
		oputs(cand[i]);
		if ((i % per) == per - 1 || i == ncand - 1)
			oputc('\n');
		else
			for (w = strlen(cand[i]); w < maxw + 2; w++)
				oputc(' ');
	}
	draw_all();
}

static void
complete(again)
	BOOL	again;
{
	char	word[PATHLEN];
	char	dir[PATHLEN];
	char	*base, *slash, *path, *end;
	int	ws, wlen, blen, common, i;
	BOOL	command_word = TRUE;
	const char *name;

	ws = pos;
	while (ws > 0 && line[ws - 1] != ' ')
		ws--;
	for (i = 0; i < ws; i++)
		if (line[i] != ' ')
			command_word = FALSE;
	wlen = pos - ws;
	if (wlen >= PATHLEN - 1) {
		bell();
		return;
	}
	memcpy(word, line + ws, wlen);
	word[wlen] = '\0';

	ncand = 0;
	pool_used = 0;
	slash = strrchr(word, '/');

	if (command_word && slash == NULL) {
		/* built-in commands and programs on the PATH */
		base = word;
		blen = wlen;
		for (i = 0; (name = sash_command_name(i)) != NULL; i++)
			if (strncmp(name, word, wlen) == 0)
				add_cand(name, strlen(name), FALSE);
		path = getenv("PATH");
		while (path && *path) {
			end = strchr(path, ':');
			i = end ? end - path : strlen(path);
			if (i > 0 && i < PATHLEN) {
				memcpy(dir, path, i);
				dir[i] = '\0';
				scan_dir(dir, word, wlen, FALSE);
			}
			path = end ? end + 1 : NULL;
		}
	} else {
		/* file names */
		if (slash) {
			base = slash + 1;
			if (slash == word)
				strcpy(dir, "/");
			else {
				memcpy(dir, word, slash - word);
				dir[slash - word] = '\0';
			}
		} else {
			base = word;
			strcpy(dir, ".");
		}
		blen = strlen(base);
		scan_dir(dir, base, blen, TRUE);
	}

	if (ncand == 0) {
		bell();
		return;
	}

	/* longest common prefix of the candidates */
	common = strlen(cand[0]);
	for (i = 1; i < ncand; i++) {
		int	j = 0;

		while (j < common && cand[i][j] == cand[0][j])
			j++;
		common = j;
	}

	if (ncand == 1) {
		char	*c = cand[0];
		int	n = strlen(c);

		if (n > blen)
			insert_text(c + blen, n - blen);
		if (c[n - 1] != '/' && (pos == len || line[pos] != ' '))
			insert_text(" ", 1);
	} else if (common > blen) {
		insert_text(cand[0] + blen, common - blen);
	} else if (again) {
		list_cands();
	} else {
		bell();
	}
}

/*
 * Keyboard input.
 */
static int
read_byte()
{
	unsigned char	c;
	int		n;

	for (;;) {
		n = read(STDIN, &c, 1);
		if (n == 1)
			return c;
		if (n < 0 && errno == EINTR)
			continue;
		return K_EOF;
	}
}

static int
pending_input()
{
	int	n = 0;

	if (ioctl(STDIN, FIONREAD, &n) < 0)
		return 1;	/* can't tell: assume a sequence follows */
	return n;
}

/* Decode the rest of an escape sequence after ESC. */
static int
read_escape()
{
	int	c, final, p1 = 0, p2 = 0, np = 0;

	if (pending_input() == 0)
		return K_NONE;			/* lone ESC */
	c = read_byte();
	if (c == K_EOF)
		return K_EOF;

	if (c == '[' || c == 'O') {
		for (;;) {
			final = read_byte();
			if (final == K_EOF)
				return K_EOF;
			if (final >= '0' && final <= '9') {
				if (np == 0)
					np = 1;
				if (np == 1)
					p1 = p1 * 10 + final - '0';
				else
					p2 = p2 * 10 + final - '0';
			} else if (final == ';') {
				np = 2;
			} else if (final >= 0x40 && final <= 0x7E)
				break;
		}
		switch (final) {
		case 'A':	return K_UP;
		case 'B':	return K_DOWN;
		case 'C':	return (p2 == 5 || p2 == 3) ? K_WORDRIGHT : K_RIGHT;
		case 'D':	return (p2 == 5 || p2 == 3) ? K_WORDLEFT : K_LEFT;
		case 'H':	return K_HOME;
		case 'F':	return K_END;
		case '~':
			switch (p1) {
			case 1: case 7:	return K_HOME;
			case 4: case 8:	return K_END;
			case 2:		return K_INSERT;
			case 3:		return K_DELETE;
			}
		}
		return K_NONE;
	}

	/* Alt+key arrives as ESC key */
	switch (c) {
	case 'b': case 'B':	return K_WORDLEFT;
	case 'f': case 'F':	return K_WORDRIGHT;
	case 'd': case 'D':	return K_WORDDELETE;
	case 0x7F: case 0x08:	return K_WORDRUBOUT;
	}
	return K_NONE;
}

static int
read_key()
{
	int	c = read_byte();

	if (c == ESC)
		return read_escape();
	return c;
}

/*
 * Read a line from the terminal into buf, with editing.  The prompt is
 * printed first.  Returns buf, or NULL at end of file.  The newline is
 * not stored.  If the terminal can't be put in raw mode this falls back
 * to fgets().
 */
char *
lineedit(pr, buf, bufsize)
	const char	*pr;
	char		*buf;
	int		bufsize;
{
	struct termios	orig, raw;
	struct winsize	ws;
	int		c, hist_pos, last = 0, i;
	char		saved[CMDLEN];
	BOOL		done = FALSE, eof = FALSE, nl_done = FALSE;

	fflush(stdout);
	if (tcgetattr(STDIN, &orig) < 0) {
		fputs(pr, stdout);
		fflush(stdout);
		return fgets(buf, bufsize, stdin);
	}
	raw = orig;
	raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
	raw.c_iflag &= ~(ICRNL | INLCR | IGNCR | IXON);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(STDIN, TCSANOW, &raw);

	cols = 80;
	if (ioctl(STDOUT, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		cols = ws.ws_col;

	line = buf;
	size = bufsize < CMDLEN ? bufsize : CMDLEN;
	len = pos = cpos = 0;
	prompt = pr;
	plen = prompt_width(pr);
	hist_pos = hist_count;
	saved[0] = '\0';

	show_mode();
	draw_all();

	while (!done) {
		c = read_key();
		switch (c) {
		case K_EOF:
			eof = TRUE;
			done = TRUE;
			break;

		case '\r':
		case '\n':
			done = TRUE;
			break;

		case CTRL('C'):
			move_cursor(len);
			oputs("^C\n");
			len = pos = 0;
			done = nl_done = TRUE;
			break;

		case CTRL('D'):
			if (len == 0) {
				move_cursor(len);
				oputs("\nUse \"exit\" to leave the shell.\n");
				draw_all();
				break;
			}
			/* fall through */
		case K_DELETE:
			if (pos < len)
				delete_range(pos, pos + 1, FALSE);
			break;

		case 0x7F:
		case CTRL('H'):
			if (pos > 0)
				delete_range(pos - 1, pos, FALSE);
			break;

		case K_LEFT:
		case CTRL('B'):
			if (pos > 0) {
				pos--;
				move_cursor(pos);
				oflush();
			}
			break;

		case K_RIGHT:
		case CTRL('F'):
			if (pos < len) {
				pos++;
				move_cursor(pos);
				oflush();
			}
			break;

		case K_HOME:
		case CTRL('A'):
			pos = 0;
			move_cursor(pos);
			oflush();
			break;

		case K_END:
		case CTRL('E'):
			pos = len;
			move_cursor(pos);
			oflush();
			break;

		case K_WORDLEFT:
			pos = word_left(pos);
			move_cursor(pos);
			oflush();
			break;

		case K_WORDRIGHT:
			pos = word_right(pos);
			move_cursor(pos);
			oflush();
			break;

		case K_INSERT:
			insert_mode = !insert_mode;
			show_mode();
			oflush();
			break;

		case K_UP:
		case CTRL('P'):
			if (hist_pos == 0) {
				bell();
				break;
			}
			if (hist_pos == hist_count) {
				memcpy(saved, line, len);
				saved[len] = '\0';
			}
			hist_pos--;
			set_line(hist[hist_pos]);
			break;

		case K_DOWN:
		case CTRL('N'):
			if (hist_pos >= hist_count) {
				bell();
				break;
			}
			hist_pos++;
			set_line(hist_pos == hist_count ? saved : hist[hist_pos]);
			break;

		case CTRL('K'):
			delete_range(pos, len, TRUE);
			break;

		case CTRL('U'):
			delete_range(0, pos, TRUE);
			break;

		case CTRL('W'):
			i = pos;
			while (i > 0 && line[i - 1] == ' ')
				i--;
			while (i > 0 && line[i - 1] != ' ')
				i--;
			delete_range(i, pos, TRUE);
			break;

		case K_WORDRUBOUT:
			delete_range(word_left(pos), pos, TRUE);
			break;

		case K_WORDDELETE:
			delete_range(pos, word_right(pos), TRUE);
			break;

		case CTRL('Y'):
			if (killbuf[0])
				insert_text(killbuf, strlen(killbuf));
			break;

		case CTRL('T'):
			if (pos > 0 && len > 1) {
				char	t;

				if (pos == len)
					pos--;
				t = line[pos - 1];
				line[pos - 1] = line[pos];
				line[pos] = t;
				pos++;
				redraw_from(pos - 2);
			}
			break;

		case CTRL('L'):
			oputs("\033[H\033[2J");
			draw_all();
			break;

		case '\t':
			complete(last == '\t');
			break;

		default:
			if ((c >= 0x20 && c < 0x7F) || (c >= 0x80 && c <= 0xFF))
				type_char(c);
			break;
		}
		if (c != K_NONE)
			last = c;
	}

	if (!nl_done) {
		move_cursor(len);
		if ((plen + len) % cols != 0 || plen + len == 0)
			oputc('\n');
	}
	oputs("\033[0 q");			/* default cursor for programs */
	oflush();
	tcsetattr(STDIN, TCSANOW, &orig);

	line[len] = '\0';
	if (eof && len == 0)
		return NULL;
	hist_add(line);
	return buf;
}
