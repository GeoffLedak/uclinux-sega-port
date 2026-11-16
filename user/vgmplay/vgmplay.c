/*
 * vgmplay -- play a VGM music log on the Sega Genesis sound chips.
 *
 *	vgmplay [-v] file.vgm
 *	vgmplay [-v] file.vgz
 *
 * .vgz files are gzip-compressed VGM files, about a quarter of the size;
 * zlib unpacks them as they play.  Their title tag is at the end of the
 * file, which can only be reached by unpacking everything (seconds on the
 * 68000).  So for them the gzip comment is shown instead: mkvgz.py puts
 * the title, game and composer there.  Without one, the file name is shown.
 *
 * VGM files record every write a game made to its sound chips, with waits
 * in 1/44100 s units between them.  This program replays the YM2612 (FM)
 * and SN76489 (PSG) writes on the real chips; writes for other chips and
 * sampled sound (YM2612 DAC data blocks) are skipped.  The song repeats
 * from its loop point until q or Ctrl-C is pressed.
 *
 * The Z80 plays the music (player.z80), keeping time with the YM2612's
 * timers, so the music doesn't slow down or stutter when the 68000 is
 * busy with other programs.  This program unpacks the file, turns it into
 * simpler commands for the Z80 and keeps a ring buffer in the Z80's RAM
 * topped up: about 7 KB, which is a few seconds of music.  In between it
 * sleeps.  The kernel copies to the Z80's RAM and starts and stops the Z80
 * (the GENESIS_Z80_* console ioctls); when this program ends, however it
 * ends, the kernel stops the Z80 and silences the chips.
 *
 * -v prints how long the song took and how often the Z80 ran out of music.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <linux/genesis_console.h>
#include "zlib.h"
#include "z80prog.h"		/* the Z80 program, from player.z80 */

#define VERSION_REG	(*(volatile unsigned char *)0xA10001)

#define VGM_RATE	44100L
#define TIMER_A		128	/* player.z80's Timer A period, in YM2612 samples */

static struct tms tms;
static struct termios saved_tty;
static int have_tty;
static int quit;

/* ---------------------------------------------------------------- */
/* File                                                             */

/*
 * Raw file access, unpacking .vgz files on the way.  The Z80 has a few
 * seconds of music in hand, so this can take its time; what matters is
 * how much 68000 time it takes in all, so files are read in big pieces
 * (each read() costs about as much as copying a few hundred bytes).
 * zlib is handed ZSTEP bytes at a time.
 */
#define ZSTEP	512
static int fd;
static int packed;		/* gzip file */
static z_stream z;
static unsigned char zin[4096];
static unsigned char *zin_next;	/* not yet handed to zlib */
static int zin_left;
static long zstart;		/* file offset of the compressed data */
static long zpos;		/* unpacked offset of the next byte out */

static char comment[256];	/* gzip comment: title lines from mkvgz.py */

static int raw_byte(void)
{
	unsigned char c;

	return read(fd, &c, 1) == 1 ? c : -1;
}

/* Open a .vgm or .vgz file; returns 0 or -1 */
static int raw_open(const char *name)
{
	unsigned char h[10];
	int flags, n, c;

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
	if (flags & 16) {				/* FCOMMENT */
		n = 0;
		while ((c = raw_byte()) > 0)
			if (n < (int)sizeof comment - 1)
				comment[n++] = c;
		comment[n] = 0;
	}
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

/*
 * Read-ahead: unpacked song data waits in a ring buffer, filled 4 KB at a
 * time when it runs out.
 */
#define RING	4096		/* power of 2 */
static unsigned char ring[RING];
static int rtail, rcount;	/* next byte out, bytes waiting */
static long rstart;		/* file offset of the next byte out */
static int at_eof;
static unsigned long read_ticks, read_max;	/* -v: time music waited */

/* (a .vgz is unpacked up to off into the ring, which the caller empties:
 * zlib is only quick when it has 258 bytes or more to unpack into) */
static void raw_seek(long off)
{
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
		if (raw_read(ring, off - zpos > RING ? RING : off - zpos) <= 0)
			break;
}

/*
 * A .vgz file can only be unpacked from the start, so jumping back to the
 * loop point would mean unpacking everything before it again, all at once.
 * Instead the first LOOP_KEEP bytes after the loop point are kept the first
 * time through.  At the loop they go back into the ring, and when they run
 * out the unpacking is redone from the start, throwing away what was got
 * before (fill(); unpacked into the empty ring, in big pieces, as zlib is
 * slow with small ones: this once took over a second).
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
		if (rcount == 0)
			rtail = head = 0;	/* (the whole ring is free) */
		if (n > RING - head)
			n = RING - head;
		if (n > max)
			n = max;
		if (n > at - zpos)
			n = at - zpos;
		if (raw_read(ring + head, n) <= 0)	/* (not kept) */
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

/*
 * The ring, for the conversion loop in main(), which takes bytes from it
 * through locals of its own (see there): used bytes of the run it had are
 * gone; returns the next run of bytes in a row, *run of them (0 at the
 * end), reading more if the ring is empty.
 */
static void ring_used(int used)
{
	rtail = (rtail + used) & (RING - 1);
	rcount -= used;
	rstart += used;
}

static unsigned char *next_run(int used, int *run)
{
	ring_used(used);
	if (rcount == 0) {
		clock_t t0 = times(&tms), t;

		while (rcount == 0 && !at_eof)
			fill(RING);
		t = times(&tms) - t0;
		read_ticks += t;
		if (t > read_max)
			read_max = t;
	}
	*run = rcount < RING - rtail ? rcount : RING - rtail;
	return ring + rtail;
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
/* The Z80                                                          */

#define Z80_RING_SIZE	(Z80_RING_END - Z80_RING)

static int con = -1;		/* the console: the Z80 ioctls go to it */
static int wr = Z80_RING;	/* where the next byte goes in the Z80's ring */
static unsigned char z80_image[GENESIS_Z80_RAM];	/* the program, and */
static int started;		/* the first ring full of music, until it starts */
static unsigned long start;	/* when it started (times()) */
static unsigned char out[512];	/* commands not yet in the ring */
static int nout;

static int z80_io(int cmd, int addr, void *data, int len)
{
	struct genesis_z80_io io;

	io.addr = addr;
	io.len = len;
	io.data = data;
	return ioctl(con, cmd, &io);
}

/* a 2 byte value the Z80 writes: read until it is the same twice, as
 * the Z80 may have been stopped half way through writing it */
static int z80_word(int addr)
{
	unsigned char a[2], b[2];

	z80_io(GENESIS_Z80_READ, addr, a, 2);
	for (;;) {
		z80_io(GENESIS_Z80_READ, addr, b, 2);
		if (a[0] == b[0] && a[1] == b[1])
			return a[0] | (a[1] << 8);
		a[0] = b[0];
		a[1] = b[1];
	}
}

static int z80_byte(int addr)
{
	unsigned char c;

	z80_io(GENESIS_Z80_READ, addr, &c, 1);
	return c;
}

/* bytes in the Z80's ring that it hasn't played yet */
static int z80_used(void)
{
	int n = wr - z80_word(Z80_RD);

	return n < 0 ? n + Z80_RING_SIZE : n;
}

static void check_key(void)
{
	long n = 0;
	char c;

	if (ioctl(0, FIONREAD, &n) == 0 && n > 0 && read(0, &c, 1) == 1)
		if (c == 'q' || c == 'Q')
			quit = 1;
}

/* sleep for 1/10 s, or until a key */
static void nap(void)
{
	fd_set fds;
	struct timeval tv;

	FD_ZERO(&fds);
	if (have_tty)
		FD_SET(0, &fds);
	tv.tv_sec = 0;
	tv.tv_usec = 100000;
	if (select(have_tty ? 1 : 0, &fds, 0, 0, &tv) > 0)
		check_key();
}

static void z80_start(void)
{
	struct genesis_z80_io io;
	unsigned long tick;
	int fd;

	/* the console: stdin, or (from win, with /dev/null) opened */
	for (fd = 0; fd < 3; fd++)
		if (ioctl(fd, GENESIS_GET_FONT, 0) >= 0) {
			con = fd;
			break;
		}
	if (con < 0)
		con = open("/dev/ttyS0", O_RDWR | O_NOCTTY);

	/* samples of the song per Timer A period, x 256: the YM2612's
	 * sample clock is 7670453 Hz / 144 on NTSC, 7600489 / 144 on PAL */
	tick = TIMER_A * VGM_RATE * 256 / ((VERSION_REG & 0x40) ? 52781 : 53267);
	memcpy(z80_image, z80_prog, sizeof z80_prog);
	z80_image[Z80_TICK] = tick;
	z80_image[Z80_TICK + 1] = tick >> 8;
	z80_image[Z80_WR] = wr;		/* the music so far */
	z80_image[Z80_WR + 1] = wr >> 8;

	io.addr = 0;
	io.len = wr;
	io.data = z80_image;
	if (con < 0 || ioctl(con, GENESIS_Z80_START, &io) < 0) {
		perror("vgmplay: the Z80");
		exit(1);
	}
	started = 1;
	start = times(&tms);
}

/* put out[] in the ring, waiting for room */
static void send_out(void)
{
	unsigned char p[2];
	int i = 0, n, k, room;

	while (i < nout && !quit) {
		n = nout - i;
		if (!started) {
			if (wr + n < Z80_RING_END) {	/* (1 byte stays free) */
				memcpy(z80_image + wr, out + i, n);
				wr += n;
				i += n;
				continue;
			}
			z80_start();
		}
		room = Z80_RING_SIZE - 1 - z80_used();
		if (room < n && room < 1024) {
			nap();			/* (not for every little bit) */
			continue;
		}
		if (n > room)
			n = room;
		k = Z80_RING_END - wr;		/* to the end of the ring... */
		if (k > n)
			k = n;
		z80_io(GENESIS_Z80_WRITE, wr, out + i, k);
		if (n > k)			/* ...and on from its start */
			z80_io(GENESIS_Z80_WRITE, Z80_RING, out + i + k, n - k);
		wr += n;
		if (wr >= Z80_RING_END)
			wr -= Z80_RING_SIZE;
		p[0] = wr;			/* then tell the Z80 */
		p[1] = wr >> 8;
		z80_io(GENESIS_Z80_WRITE, Z80_WR, p, 2);
		i += n;
	}
	nout = 0;
}

/* out[] is filled up to op: send it, and start again */
static unsigned char *flush_out(unsigned char *op)
{
	nout = op - out;
	send_out();
	return out;
}

/* a wait of n samples (the song's waits come in many small ones, so they
 * are added up first), in pieces of up to 32767 */
static unsigned char *put_wait(unsigned char *op, unsigned long n)
{
	unsigned long k;

	while (n) {
		if (op > out + sizeof out - 3)
			op = flush_out(op);
		k = n > 32767 ? 32767 : n;
		n -= k;
		*op++ = 3;
		*op++ = k;
		*op++ = k >> 8;
	}
	return op;
}

static void restore(void)
{
	if (con >= 0)
		ioctl(con, GENESIS_Z80_STOP, 0);
	if (have_tty)
		tcsetattr(0, TCSANOW, &saved_tty);
}

static void on_signal(int sig)
{
	restore();
	_exit(0);
}

/* ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
	unsigned char h[0x40];
	unsigned long version, data_off, loop_off, gd3_off;
	struct termios raw;
	struct tms tm;
	unsigned long played = 0, opened, size, wait = 0;
	int verbose = 0, c, a, b, under;
	/* the conversion runs for every byte of the song, so it reaches the
	 * ring and out[] through these (in a program like this each use of
	 * a global costs a look-up): run bytes from rp, used of them used;
	 * commands go at op */
	unsigned char *rp = ring, *op = out;
	unsigned char *const oend = out + sizeof out - 3;	/* (a command fits) */
	int run = 0, used = 0;

#define GET()	(used < run ? rp[used++] : \
		 (rp = next_run(used, &run), used = 0, run ? rp[used++] : -1))
#define DROP()	(ring_used(used), run = used = 0)	/* before a seek */
#define ROOM()	do { \
			if (wait) { op = put_wait(op, wait); wait = 0; } \
			if (op > oend) op = flush_out(op); \
		} while (0)
#define WAIT(n)	(played += (n), wait += (n))

	if (argc > 1 && !strcmp(argv[1], "-v")) {
		verbose = 1;
		argc--, argv++;
	}
	if (argc != 2) {
		fprintf(stderr, "usage: vgmplay [-v] file.vgm|file.vgz\n"
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

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	signal(SIGQUIT, on_signal);
	if (isatty(0) && tcgetattr(0, &saved_tty) == 0) {
		have_tty = 1;
		raw = saved_tty;
		raw.c_lflag &= ~(ICANON | ECHO);
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0;
		tcsetattr(0, TCSANOW, &raw);
	}

	if (gd3_off && !packed) {
		show_tag(gd3_off);
	} else if (comment[0]) {
		char *p;

		for (p = comment; *p; p++)	/* plain ASCII only */
			putchar((*p & 0x80) || (*p < ' ' && *p != '\n') ? '?' : *p);
		putchar('\n');
	} else {
		const char *base = strrchr(argv[1], '/');
		printf("Playing %s\n", base ? base + 1 : argv[1]);
	}
	printf("\nPress q to quit.\n");
	fflush(stdout);

	seek_to(data_off);
	loop_at = loop_off;
	while (fill(RING))		/* read ahead before the music starts */
		;

	/* the Z80 starts once the first ring full is ready (send_out) */
	while (!quit) {
		c = GET();
		switch (c) {
		case 0x4F:			/* Game Gear PSG stereo */
			GET();
			break;
		case 0x50:			/* PSG write */
			ROOM();
			*op++ = 2;
			*op++ = GET();
			break;
		case 0x52:			/* YM2612 port 0 write */
		case 0x53:			/* YM2612 port 1 write */
			a = GET();
			b = GET();
			if (c == 0x52 && a == 0x27) {
				ROOM();		/* the timers are the Z80's */
				*op++ = 4;
				*op++ = b;
			} else if (!(c == 0x52 && (a >= 0x24 && a <= 0x26))) {
				ROOM();
				*op++ = c - 0x52;
				*op++ = a;
				*op++ = b;
			}
			break;
		case 0x61:			/* wait n samples */
			a = GET();
			b = GET();
			WAIT(a | (b << 8));
			break;
		case 0x62:			/* wait 1/60 s */
			WAIT(735);
			break;
		case 0x63:			/* wait 1/50 s */
			WAIT(882);
			break;
		case 0x66:			/* end of song */
			if (!loop_off)
				goto done;
			DROP();
			seek_to(loop_off);
			break;
		case -1:			/* end of file or read error */
			goto done;
		case 0x67:			/* data block: 0x66 type size data */
			GET();
			GET();
			size = GET();
			size |= GET() << 8;
			size |= (unsigned long)GET() << 16;
			size |= (unsigned long)GET() << 24;
			DROP();
			skip(size);
			break;
		case 0xE0:			/* PCM seek */
			GET(); GET(); GET(); GET();
			break;
		default:
			if (c >= 0x70 && c <= 0x7F) {		/* wait 1-16 */
				WAIT((c & 15) + 1);
			} else if (c >= 0x80 && c <= 0x8F) {	/* DAC + wait */
				WAIT(c & 15);
			} else if (c >= 0x30 && c <= 0x3F) {
				GET();
			} else if ((c >= 0x40 && c <= 0x4E) ||
				   (c >= 0x51 && c <= 0x5F) ||
				   (c >= 0xA0 && c <= 0xBF)) {
				GET(); GET();
			} else if (c >= 0xC0 && c <= 0xDF) {
				GET(); GET(); GET();
			} else if (c >= 0xE1) {
				GET(); GET(); GET(); GET();
			} else if (c >= 0x90 && c <= 0x95) {	/* DAC streams */
				static const char len[] = { 4, 4, 5, 10, 1, 4 };
				for (a = len[c - 0x90]; a; a--)
					GET();
			}
			break;
		}
	}
done:
	/* let the Z80 play to the end: it runs out after the last wait */
	if (wait)
		op = put_wait(op, wait);
	flush_out(op);
	if (!started)			/* (a short song) */
		z80_start();
	under = z80_byte(Z80_UNDER);
	while (!quit && (z80_used() > 0 || z80_byte(Z80_UNDER) == under))
		nap();
	under = z80_byte(Z80_UNDER);
	restore();
	if (verbose) {
		printf("%lu samples (%lu.%02lu s) in %lu ticks of 1/60 s, "
		       "%lu ticks to start\n",
		       played, played / VGM_RATE,
		       played % VGM_RATE * 100 / VGM_RATE,
		       (unsigned long)times(&tm) - start, start - opened);
		/* (the last time is the end) */
		printf("the Z80 ran out of music %d times\n", under > 0 ? under - 1 : 0);
		printf("waited for data: %lu ticks in all, longest %lu\n",
		       read_ticks, read_max);
	}
	return 0;
}
