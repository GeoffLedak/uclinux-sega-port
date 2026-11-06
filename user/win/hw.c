/*
 * hw.c -- the Genesis side: graphics mode, joypad mouse and keyboard
 * (the console ioctls in linux/genesis_console.h)
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/times.h>
#include <linux/genesis_console.h>
#include "win.h"

#define PSG	(*(volatile unsigned char *)0xC00011)

static int con = -1;
static struct termios saved;

/* the 16 colours in Genesis format 0BGR */
static const unsigned short palette[16] = {
	0x000, 0x008, 0x080, 0x088, 0x800, 0x808, 0x880, 0xCCC,
	0x888, 0x00E, 0x0E0, 0x0EE, 0xE00, 0xE0E, 0xEE0, 0xEEE,
};
/* pointer palette (palette 3): transparent, black, white */
static const unsigned short pointer_palette[3] = { 0x000, 0x000, 0xEEE };

int hw_init(void)
{
	struct termios t;
	int fd, i;

	for (fd = 0; fd < 3; fd++)
		if (ioctl(fd, GENESIS_GFX_ENTER, 0) == 0) {
			con = fd;
			break;
		}
	if (con < 0)
		return -1;

	tcgetattr(con, &saved);
	t = saved;
	t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
	t.c_iflag &= ~(IXON | ICRNL | INLCR);
	t.c_cc[VMIN] = 0;			/* reads don't wait */
	t.c_cc[VTIME] = 0;
	tcsetattr(con, TCSANOW, &t);

	hw_palette(0, 16, palette);
	hw_palette(48, 3, pointer_palette);
	for (i = 0; i < PTR_COUNT; i++)
		hw_pointer_define(i, pointer_hot[2 * i], pointer_hot[2 * i + 1],
				  pointer_tiles[i]);
	return 0;
}

void hw_exit(void)
{
	if (con < 0)
		return;
	PSG = 0x9F;
	ioctl(con, GENESIS_GFX_LEAVE, 0);
	tcsetattr(con, TCSANOW, &saved);
	con = -1;
}

/* what the video memory holds (graphics mode starts with it all 0) */
static u32 shadow[SCR_TILES * 8];
void fb_sync(const u32 *fb, u32 *shadow, u32 *dirty, int nwords);	/* gfx68k.S */

void hw_flush(void)
{
	struct genesis_gfx_flush f;
	int i;

	/* drop the tiles that were redrawn the same as before */
	fb_sync(fb, shadow, dirty, DIRTY_WORDS);
	for (i = 0; i < DIRTY_WORDS; i++)
		if (dirty[i])
			break;
	if (i == DIRTY_WORDS)
		return;
	f.fb = (const unsigned long *)fb;
	f.dirty = (unsigned long *)dirty;
	ioctl(con, GENESIS_GFX_FLUSH, &f);
}

void hw_palette(int first, int n, const unsigned short *c)
{
	struct genesis_gfx_palette p;

	p.first = first;
	p.count = n;
	p.colors = c;
	ioctl(con, GENESIS_GFX_PALETTE, &p);
}

void hw_pointer_define(int shape, int hx, int hy, const u32 *tiles)
{
	struct genesis_gfx_pointer p;

	p.shape = shape;
	p.hot_x = hx;
	p.hot_y = hy;
	p.tiles = (const unsigned long *)tiles;
	ioctl(con, GENESIS_GFX_POINTER, &p);
}

void hw_pointer(int shape)
{
	struct genesis_gfx_pointer p;

	memset(&p, 0, sizeof p);
	p.shape = shape;
	ioctl(con, GENESIS_GFX_POINTER, &p);
}

int hw_input(struct hw_input *e)
{
	struct genesis_gfx_input in;
	int r = ioctl(con, GENESIS_GFX_INPUT, &in);

	e->x = in.x;
	e->y = in.y;
	e->buttons = in.buttons;
	e->pad = in.pad;
	e->time = in.time;
	return r == 1;
}

void hw_wait(int frames)
{
	ioctl(con, GENESIS_GFX_WAIT, frames);
}

int hw_keys(char *buf, int n)
{
	int r = read(con, buf, n);

	return r < 0 ? 0 : r;
}

void hw_warp(int x, int y)
{
	ioctl(con, GENESIS_GFX_WARP, (x << 16) | y);
}

void hw_beep(void)
{
	/* PSG tone channel 0, about 800 Hz, for a tenth of a second */
	PSG = 0x80 | (140 & 0x0F);
	PSG = (140 >> 4) & 0x3F;
	PSG = 0x90 | 3;
	hw_wait(6);
	PSG = 0x9F;
}

unsigned long hw_frames(void)
{
	struct tms t;

	return (unsigned long)times(&t);
}
