/*
 * vgmplay -- play a VGM music log on the Sega Genesis sound chips.
 *
 *	vgmplay file.vgm
 *
 * VGM files record every write a game made to its sound chips, with waits
 * in 1/44100 s units between them.  This program replays the YM2612 (FM)
 * and SN76489 (PSG) writes on the real chips; writes for other chips and
 * sampled sound (YM2612 DAC data blocks) are skipped.  The song repeats
 * from its loop point until q or Ctrl-C is pressed.
 *
 * There is no MMU, so the chips are written directly.  The YM2612 sits on
 * the Z80 bus; the kernel holds the Z80 off the bus at boot, and this
 * program requests the bus again to be sure.
 *
 * Timing uses the YM2612's own Timer A: it overflows every TIMER_TICKS
 * ticks of the chip's sample clock (about 2 ms), and each overflow lets
 * that much music play.  The timer has only an "overflowed" flag, so
 * overflows are lost while the kernel's vblank interrupt keeps us away for
 * longer than one period.  The system tick (60 a second, from times())
 * never loses time but is far too coarse on its own, so it is used to
 * catch the music up whenever the timer falls more than a period behind.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/times.h>

#define YM_ADDR0	(*(volatile unsigned char *)0xA04000)	/* also status */
#define YM_DATA0	(*(volatile unsigned char *)0xA04001)
#define YM_ADDR1	(*(volatile unsigned char *)0xA04002)
#define YM_DATA1	(*(volatile unsigned char *)0xA04003)
#define PSG		(*(volatile unsigned char *)0xC00011)
#define Z80_BUSREQ	(*(volatile unsigned short *)0xA11100)
#define Z80_RESET	(*(volatile unsigned short *)0xA11200)
#define VERSION_REG	(*(volatile unsigned char *)0xA10001)

#define VGM_RATE	44100L
#define TIMER_TICKS	106	/* Timer A period: 106 / 53267 Hz = about 2 ms */
#define TIMER_UNITS	(TIMER_TICKS * VGM_RATE)

/*
 * Time is counted in units of 1/(VGM_RATE * fm_rate) s, so a song sample
 * is fm_rate units and a Timer A period is TIMER_UNITS.  A vblank lasts
 * VGM_RATE * fm_rate / vblank rate units, which works out to
 * VGM_RATE * master clocks per frame / (7 * 144), and 44100 / 1008 is
 * exactly 175 / 4.  The kernel ticks once per vblank on NTSC; on PAL it
 * ticks 6 times every 5 vblanks, so a tick is 5/6 of a frame.
 */
#define TICK_UNITS_NTSC	(896040L * 175 / 4)	/* 262 lines x 3420 clocks */
#define TICK_UNITS_PAL	(1070460L * 175 * 5 / 24) /* 313 x 3420, x 5/6 */

static long fm_rate;		/* YM2612 sample clock: master clock / 144 */
static long tick_units;		/* one system tick */
static long credit;		/* time owed to the song */
static long behind;		/* system-tick time minus Timer A time */
static long granted;		/* Timer A time since the last system tick */
static clock_t last_tick;
static struct tms tms;
static unsigned char reg27;	/* channel 3 mode bits the song set in reg 0x27 */
static unsigned long overflows;

static struct termios saved_tty;
static int have_tty;
static int quit;

/* ---------------------------------------------------------------- */
/* Sound chips                                                      */

static void ym_write(int port, int reg, int val)
{
	while (YM_ADDR0 & 0x80)		/* busy */
		;
	if (port) {
		YM_ADDR1 = reg;
		YM_DATA1 = val;
	} else {
		YM_ADDR0 = reg;
		YM_DATA0 = val;
	}
}

/* Timer A: load, enable its flag and clear it, keeping the song's mode bits */
static void timer_restart(void)
{
	ym_write(0, 0x27, reg27 | 0x15);
}

static void silence(void)
{
	int port, r;

	ym_write(0, 0x27, 0x00);		/* timers off, ch3 normal */
	ym_write(0, 0x2B, 0x00);		/* DAC off */
	for (r = 0; r < 7; r++)
		if (r != 3)
			ym_write(0, 0x28, r);	/* key off */
	for (port = 0; port < 2; port++)
		for (r = 0; r < 16; r++) {
			ym_write(port, 0x40 + r, 0x7F);	/* total level: silent */
			ym_write(port, 0x80 + r, 0xFF);	/* fastest release */
		}
	PSG = 0x9F; PSG = 0xBF; PSG = 0xDF; PSG = 0xFF;
}

static void restore(void)
{
	silence();
	if (have_tty)
		tcsetattr(0, TCSANOW, &saved_tty);
}

static void on_signal(int sig)
{
	restore();
	_exit(0);
}

/* ---------------------------------------------------------------- */
/* Time                                                             */

static void check_key(void)
{
	long n = 0;
	char c;

	if (ioctl(0, FIONREAD, &n) == 0 && n > 0 && read(0, &c, 1) == 1)
		if (c == 'q' || c == 'Q')
			quit = 1;
}

/* Compare Timer A with the system tick; make up time the timer lost */
static void sync_clock(void)
{
	clock_t now = times(&tms);
	unsigned long ticks = now - last_tick;

	if (ticks == 0)
		return;
	last_tick = now;
	if (ticks > 4)		/* we didn't run for a while: pause, don't rush */
		ticks = 4;
	behind += ticks * tick_units - granted;
	granted = 0;
	if (behind > TIMER_UNITS) {
		credit += behind - TIMER_UNITS;
		behind = TIMER_UNITS;
	} else if (behind < -4 * TIMER_UNITS) {
		behind = -4 * TIMER_UNITS;
	}
}

/* Let n samples of song time pass */
static void wait_samples(unsigned long n)
{
	while (n && !quit) {
		unsigned long m = n > 16384 ? 16384 : n;

		n -= m;
		credit -= (long)m * fm_rate;
		while (credit < 0 && !quit) {
			if (YM_ADDR0 & 0x01) {		/* Timer A overflowed */
				timer_restart();
				credit += TIMER_UNITS;
				granted += TIMER_UNITS;
				sync_clock();
				if ((++overflows & 15) == 0)	/* every ~32 ms */
					check_key();
			}
		}
	}
}

/* ---------------------------------------------------------------- */
/* File                                                             */

static int fd;
static unsigned char buf[4096];
static int bpos, blen;
static long bstart;		/* file offset of buf[0] */

static int get(void)
{
	if (bpos >= blen) {
		bstart += blen;
		blen = read(fd, buf, sizeof buf);
		bpos = 0;
		if (blen <= 0) {
			blen = 0;
			return -1;
		}
	}
	return buf[bpos++];
}

static void seek_to(long off)
{
	lseek(fd, off, SEEK_SET);
	bstart = off;
	bpos = blen = 0;
}

static void skip(long n)
{
	seek_to(bstart + bpos + n);
}

static unsigned long le32(unsigned char *p)
{
	return p[0] | (p[1] << 8) | ((unsigned long)p[2] << 16) |
		((unsigned long)p[3] << 24);
}

/* Print one GD3 string (UTF-16LE), keeping plain ASCII; returns bytes used */
static int gd3_string(unsigned char *p, int len, int print)
{
	int i;

	for (i = 0; i + 1 < len; i += 2) {
		int c = p[i] | (p[i + 1] << 8);
		if (c == 0)
			return i + 2;
		if (print)
			putchar(c < 0x80 ? c : '?');
	}
	return len;
}

/* Show title, game and composer from the GD3 tag, if there is one */
static void show_tag(long off)
{
	unsigned char *tag, *p, hdr[12];
	unsigned long len;
	int i, n;

	seek_to(off);
	if (read(fd, hdr, 12) != 12 || memcmp(hdr, "Gd3 ", 4))
		return;
	len = le32(hdr + 8);
	if (len > 4096 || !(tag = malloc(len)))
		return;
	if (read(fd, tag, len) == (int)len) {
		/* strings: title, title (jp), game, game (jp), system,
		 * system (jp), author, author (jp), date, ripper, notes */
		p = tag;
		for (i = 0; i < 11 && p < tag + len; i++) {
			int show = (i == 0 || i == 2 || i == 6);
			if (show && *p == 0 && p[1] == 0)
				show = 0;
			n = gd3_string(p, tag + len - p, show);
			if (show)
				putchar('\n');
			p += n;
		}
	}
	free(tag);
}

/* ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
	unsigned char h[0x40];
	unsigned long version, data_off, loop_off, gd3_off;
	struct termios raw;
	struct tms tm;
	unsigned long played = 0, start;
	int verbose = 0, c, a, b;

	if (argc > 1 && !strcmp(argv[1], "-v")) {
		verbose = 1;
		argc--, argv++;
	}
	if (argc != 2) {
		fprintf(stderr, "usage: vgmplay file.vgm\n"
				"try:   vgmplay /usr/music/spacewalk.vgm\n");
		return 1;
	}
	if ((fd = open(argv[1], O_RDONLY)) < 0) {
		perror(argv[1]);
		return 1;
	}
	if (read(fd, h, sizeof h) != sizeof h || memcmp(h, "Vgm ", 4)) {
		fprintf(stderr, "%s: not a VGM file (compressed .vgz files "
				"must be gunzipped first)\n", argv[1]);
		return 1;
	}
	version = le32(h + 0x08);
	data_off = (version >= 0x150 && le32(h + 0x34)) ? 0x34 + le32(h + 0x34)
							 : 0x40;
	loop_off = le32(h + 0x1C) ? 0x1C + le32(h + 0x1C) : 0;
	gd3_off = le32(h + 0x14) ? 0x14 + le32(h + 0x14) : 0;

	/* YM2612 sample clock: 7670453 Hz / 144 on NTSC, 7600489 / 144 on PAL */
	if (VERSION_REG & 0x40) {
		fm_rate = 52781;
		tick_units = TICK_UNITS_PAL;
	} else {
		fm_rate = 53267;
		tick_units = TICK_UNITS_NTSC;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	signal(SIGQUIT, on_signal);
	if (tcgetattr(0, &saved_tty) == 0) {
		have_tty = 1;
		raw = saved_tty;
		raw.c_lflag &= ~(ICANON | ECHO);
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0;
		tcsetattr(0, TCSANOW, &raw);
	}

	if (gd3_off)
		show_tag(gd3_off);
	printf("\nPress q to quit.\n");
	fflush(stdout);

	/* Get the Z80 off the bus so the 68000 can reach the YM2612 */
	Z80_BUSREQ = 0x100;
	Z80_RESET = 0x100;
	while (Z80_BUSREQ & 0x100)
		;

	silence();
	ym_write(0, 0x24, (1024 - TIMER_TICKS) >> 2);
	ym_write(0, 0x25, (1024 - TIMER_TICKS) & 3);
	timer_restart();
	start = last_tick = times(&tm);

	seek_to(data_off);
	while (!quit) {
		c = get();
		switch (c) {
		case 0x4F:			/* Game Gear PSG stereo */
			get();
			break;
		case 0x50:			/* PSG write */
			PSG = get();
			break;
		case 0x52:			/* YM2612 port 0 write */
		case 0x53:			/* YM2612 port 1 write */
			a = get();
			b = get();
			if (c == 0x52 && a == 0x27) {
				/* keep our timer running; take the mode bits */
				reg27 = b & 0xC0;
				timer_restart();
			} else if (!(c == 0x52 && (a >= 0x24 && a <= 0x26)))
				ym_write(c - 0x52, a, b);
			break;
		case 0x61:			/* wait n samples */
			a = get();
			b = get();
			played += a | (b << 8);
			wait_samples(a | (b << 8));
			break;
		case 0x62:			/* wait 1/60 s */
			played += 735;
			wait_samples(735);
			break;
		case 0x63:			/* wait 1/50 s */
			played += 882;
			wait_samples(882);
			break;
		case 0x66:			/* end of song */
			if (!loop_off)
				goto done;
			seek_to(loop_off);
			break;
		case -1:			/* end of file or read error */
			goto done;
		case 0x67:			/* data block: 0x66 type size data */
			get();
			get();
			a = get(); b = get();
			{
				unsigned long size = a | (b << 8);
				a = get(); b = get();
				size |= ((unsigned long)a << 16) |
					((unsigned long)b << 24);
				skip(size);
			}
			break;
		case 0xE0:			/* PCM seek */
			get(); get(); get(); get();
			break;
		default:
			if (c >= 0x70 && c <= 0x7F) {		/* wait 1-16 */
				played += (c & 15) + 1;
				wait_samples((c & 15) + 1);
			} else if (c >= 0x80 && c <= 0x8F) {	/* DAC + wait */
				played += c & 15;
				wait_samples(c & 15);
			} else if (c >= 0x30 && c <= 0x3F) {
				get();
			} else if ((c >= 0x40 && c <= 0x4E) ||
				   (c >= 0x51 && c <= 0x5F) ||
				   (c >= 0xA0 && c <= 0xBF)) {
				get(); get();
			} else if (c >= 0xC0 && c <= 0xDF) {
				get(); get(); get();
			} else if (c >= 0xE1) {
				get(); get(); get(); get();
			} else if (c >= 0x90 && c <= 0x95) {	/* DAC streams */
				static const char len[] = { 4, 4, 5, 10, 1, 4 };
				for (a = len[c - 0x90]; a; a--)
					get();
			}
			break;
		}
	}
done:
	restore();
	if (verbose)
		printf("%lu samples (%lu.%02lu s) in %lu ticks of 1/60 s\n",
		       played, played / VGM_RATE,
		       played % VGM_RATE * 100 / VGM_RATE,
		       (unsigned long)times(&tm) - start);
	return 0;
}
