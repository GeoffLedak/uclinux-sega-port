/*
 * vgmplay -- play a VGM music log on the Sega Genesis sound chips.
 *
 *	vgmplay file.vgm
 *	vgmplay file.vgz
 *
 * .vgz files are gzip-compressed VGM files, about a quarter of the size;
 * zlib unpacks them as they play.  Their title tag is at the end of the
 * file, which can only be reached by unpacking everything (seconds on the
 * 68000), so for them the file name is shown instead.
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
#include "zlib.h"

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

static void prefetch(void);

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
	if (ticks > 30)		/* we didn't run for 1/2 s: pause, don't rush */
		ticks = 30;
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
			} else if (credit < -2 * TIMER_UNITS) {
				prefetch();		/* over 4 ms to spare */
			}
		}
	}
}

/* ---------------------------------------------------------------- */
/* File                                                             */

/*
 * Raw file access, unpacking .vgz files on the way.  Everything is done in
 * small pieces because the music can't play while we read; zlib's own
 * gzread() reads 16KB at a time.  zlib unpacks all the input it is given
 * before it returns, whatever the room for output, so it is handed ZSTEP
 * bytes at a time: about 128 bytes out, under 1/100 s of work.
 */
#define ZSTEP	32
static int fd;
static int packed;		/* gzip file */
static z_stream z;
static unsigned char zin[512];
static unsigned char *zin_next;	/* not yet handed to zlib */
static int zin_left;
static long zstart;		/* file offset of the compressed data */
static long zpos;		/* unpacked offset of the next byte out */

static int raw_byte(void)
{
	unsigned char c;

	return read(fd, &c, 1) == 1 ? c : -1;
}

/* Open a .vgm or .vgz file; returns 0 or -1 */
static int raw_open(const char *name)
{
	unsigned char h[10];
	int flags, n;

	if ((fd = open(name, O_RDONLY)) < 0)
		return -1;
	if (read(fd, h, 10) != 10 || h[0] != 0x1F || h[1] != 0x8B || h[2] != 8) {
		lseek(fd, 0, SEEK_SET);		/* not gzip: plain file */
		return 0;
	}
	flags = h[3];			/* skip the rest of the gzip header */
	if (flags & 4) {				/* FEXTRA */
		n = raw_byte();
		n |= raw_byte() << 8;
		lseek(fd, n, SEEK_CUR);
	}
	if (flags & 8)					/* FNAME */
		while (raw_byte() > 0)
			;
	if (flags & 16)					/* FCOMMENT */
		while (raw_byte() > 0)
			;
	if (flags & 2)					/* FHCRC */
		lseek(fd, 2, SEEK_CUR);
	zstart = lseek(fd, 0, SEEK_CUR);
	if (inflateInit2(&z, -MAX_WBITS) != Z_OK)	/* raw deflate data */
		return -1;
	packed = 1;
	return 0;
}

static int raw_read(unsigned char *dst, int n)
{
	int e;

	if (!packed)
		return read(fd, dst, n);
	z.next_out = dst;
	z.avail_out = n;
	while (z.avail_out) {
		if (z.avail_in == 0) {
			if (zin_left == 0) {
				zin_left = read(fd, zin, sizeof zin);
				if (zin_left <= 0) {
					zin_left = 0;
					break;
				}
				zin_next = zin;
			}
			z.next_in = zin_next;
			z.avail_in = zin_left > ZSTEP ? ZSTEP : zin_left;
			zin_next += z.avail_in;
			zin_left -= z.avail_in;
		}
		e = inflate(&z, Z_NO_FLUSH);
		if (e != Z_OK)		/* end of data, or damaged */
			break;
	}
	n -= z.avail_out;
	zpos += n;
	return n;
}

static void raw_seek(long off)
{
	unsigned char tmp[256];

	if (!packed) {
		lseek(fd, off, SEEK_SET);
		return;
	}
	if (off < zpos) {		/* backwards: start again from the top */
		inflateReset(&z);
		lseek(fd, zstart, SEEK_SET);
		z.avail_in = 0;
		zin_left = 0;
		zpos = 0;
	}
	while (zpos < off)
		if (raw_read(tmp, off - zpos > 256 ? 256 : off - zpos) <= 0)
			break;
}

/*
 * Read-ahead: unpacked song data waits in a ring buffer.  While the player
 * waits for the next note it unpacks a little more (prefetch()), so the
 * music rarely has to stop for get() to unpack data itself.
 */
#define RING	4096		/* power of 2 */
static unsigned char ring[RING];
static int rtail, rcount;	/* next byte out, bytes waiting */
static long rstart;		/* file offset of the next byte out */
static int at_eof;
static unsigned long read_ticks, read_max;	/* -v: time music waited */

/*
 * A .vgz file can only be unpacked from the start, so jumping back to the
 * loop point would mean unpacking everything before it again, all at once.
 * Instead the first LOOP_KEEP bytes after the loop point are kept the first
 * time through.  At the loop they go back into the ring, and the unpacking
 * is redone in the background (fill() throws away what it has already got).
 */
#define LOOP_KEEP	RING
static unsigned char loop_keep[LOOP_KEEP];
static long loop_at;		/* loop point, 0 if none */
static int loop_len;		/* bytes kept */

/* Add up to max bytes to the ring; returns how many */
static int fill(int max)
{
	int head = (rtail + rcount) & (RING - 1);
	int n = RING - rcount;
	long at = rstart + rcount;	/* file offset of ring[head] */

	if (packed && zpos < at) {	/* catching up after a loop */
		unsigned char tmp[256];

		n = at - zpos > max ? max : at - zpos;
		if (raw_read(tmp, n > 256 ? 256 : n) <= 0)
			at_eof = 1;
		return 0;
	}
	if (n > RING - head)
		n = RING - head;
	if (n > max)
		n = max;
	if (n <= 0 || at_eof)
		return 0;
	n = raw_read(ring + head, n);
	if (n <= 0) {
		at_eof = 1;
		return 0;
	}
	rcount += n;
	if (packed && loop_at && at + n > loop_at + loop_len && loop_len < LOOP_KEEP &&
	    at <= loop_at + loop_len) {		/* keep the loop's start */
		int from = loop_at + loop_len - at;
		int k = n - from;

		if (k > LOOP_KEEP - loop_len)
			k = LOOP_KEEP - loop_len;
		memcpy(loop_keep + loop_len, ring + head + from, k);
		loop_len += k;
	}
	return n;
}

static void prefetch(void)
{
	fill(64);		/* about 3 ms of unpacking on the 68000 */
}

static int get(void)
{
	int c;

	if (rcount == 0) {
		clock_t t0 = times(&tms), t;

		while (rcount == 0 && !at_eof)
			fill(256);
		t = times(&tms) - t0;
		read_ticks += t;
		if (t > read_max)
			read_max = t;
		if (rcount == 0)
			return -1;
	}
	c = ring[rtail];
	rtail = (rtail + 1) & (RING - 1);
	rcount--;
	rstart++;
	return c;
}

static void seek_to(long off)
{
	if (off >= rstart && off <= rstart + rcount) {	/* already read */
		rtail = (rtail + (off - rstart)) & (RING - 1);
		rcount -= off - rstart;
	} else if (packed && off == loop_at && loop_len) {
		memcpy(ring, loop_keep, loop_len);	/* see loop_keep */
		rtail = 0;
		rcount = loop_len;
		at_eof = 0;
		inflateReset(&z);
		lseek(fd, zstart, SEEK_SET);
		z.avail_in = 0;
		zin_left = 0;
		zpos = 0;
	} else {
		raw_seek(off);
		rcount = 0;
		at_eof = 0;
	}
	rstart = off;
}

static void skip(long n)
{
	seek_to(rstart + n);
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
	if (raw_read(hdr, 12) != 12 || memcmp(hdr, "Gd3 ", 4))
		return;
	len = le32(hdr + 8);
	if (len > 4096 || !(tag = malloc(len)))
		return;
	if (raw_read(tag, len) == (int)len) {
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
	unsigned long played = 0, start, opened;
	int verbose = 0, c, a, b;

	if (argc > 1 && !strcmp(argv[1], "-v")) {
		verbose = 1;
		argc--, argv++;
	}
	if (argc != 2) {
		fprintf(stderr, "usage: vgmplay file.vgm|file.vgz\n"
				"try:   vgmplay /usr/music/spacewalk.vgz\n");
		return 1;
	}
	opened = times(&tm);
	if (raw_open(argv[1]) < 0) {
		perror(argv[1]);
		return 1;
	}
	if (raw_read(h, sizeof h) != sizeof h || memcmp(h, "Vgm ", 4)) {
		fprintf(stderr, "%s: not a VGM file\n", argv[1]);
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

	if (gd3_off && !packed) {
		show_tag(gd3_off);
	} else {
		const char *base = strrchr(argv[1], '/');
		printf("Playing %s\n", base ? base + 1 : argv[1]);
	}
	printf("\nPress q to quit.\n");
	fflush(stdout);

	seek_to(data_off);
	loop_at = loop_off;
	while (fill(256))		/* read ahead before the music starts */
		;

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
		printf("%lu samples (%lu.%02lu s) in %lu ticks of 1/60 s, "
		       "%lu ticks to start\n",
		       played, played / VGM_RATE,
		       played % VGM_RATE * 100 / VGM_RATE,
		       (unsigned long)times(&tm) - start, start - opened);
	if (verbose)
		printf("waited for data: %lu ticks in all, longest %lu\n",
		       read_ticks, read_max);
	return 0;
}
