/*
 * genesis_gfx.c -- graphics mode and joypad mouse for full-screen programs
 *
 * See include/linux/genesis_console.h for the interface (the console
 * ioctls GENESIS_GFX_*); the win GUI (user/win) is the user.
 *
 * Video memory in graphics mode:
 *   0x0000 - 0x8BFF  the screen: 1120 tiles, tile t at (t % 40, t / 40)
 *   0x8C00 - 0x8FFF  pointer shapes, 4 tiles each
 *   0xA800           sprite table: sprite 0 is the pointer
 *   0xC000           name table of plane A, and of plane B too, so both
 *                    planes show the screen and colour 0 (transparent)
 *                    shows the backdrop, palette entry 0
 * The text console's tiles (1 - 869) and its name table at 0xE000 are
 * overwritten or left alone; genesis_term_restore() redraws them when
 * graphics mode ends.
 *
 * The program's frame buffer is copied to video memory by DMA, a piece
 * at a time with interrupts off, so that the vblank interrupt (which
 * moves the pointer sprite) can't change the VDP's address in the middle;
 * the vblank interrupt itself sends queued pieces first thing, while the
 * VDP is fastest.
 *
 * Note: no 32-bit multiply or divide here (the kernel has no libgcc).
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/genesis_console.h>

#include <asm/system.h>
#include <asm/segment.h>

#define VDP_DATA16	(*(volatile unsigned short *)0xC00000)
#define VDP_DATA32	(*(volatile unsigned long *)0xC00000)
#define VDP_CTRL16	(*(volatile unsigned short *)0xC00004)
#define VDP_CTRL32	(*(volatile unsigned long *)0xC00004)

#define PTR_TILE	GENESIS_GFX_TILES	/* first pointer tile (1120) */
#define DIRTY_LONGS	((GENESIS_GFX_TILES + 31) / 32)
#define QSIZE		16			/* button events (power of 2) */

extern short get_pad(short pad);		/* hw_md.S */

int genesis_gfx_active;
static struct task_struct *owner;
static struct wait_queue *gfx_wait;
static unsigned long frames;

/* pointer */
static short px, py;
static int shape = -1;
static unsigned char hot_x[GENESIS_GFX_POINTER_SHAPES];
static unsigned char hot_y[GENESIS_GFX_POINTER_SHAPES];
static int hold, frac;				/* d-pad acceleration */
static unsigned short buttons, lastpad;
static volatile char moved;

/* the upload queue (see "Drawing") */
static unsigned long qdirty[DIRTY_LONGS];	/* the tiles still to send */
static const unsigned long *qfb;		/* from this frame buffer */
static int qpos;				/* tiles before qpos are sent */
static volatile char qbusy;
static struct wait_queue *dma_wait;		/* the flush, waiting for it */

/* button events */
static struct genesis_gfx_input queue[QSIZE];
static volatile int qhead, qtail;

static unsigned long vram_write(unsigned long addr)
{
	return ((addr & 0x3FFF) << 16) | 0x40000000 | (addr >> 14);
}

static void queue_event(unsigned short b, unsigned short pad)
{
	struct genesis_gfx_input *e;

	if (((qhead + 1) & (QSIZE - 1)) == qtail)
		return;				/* full: drop it */
	e = &queue[qhead];
	e->x = px;
	e->y = py;
	e->buttons = b;
	e->pad = pad;
	e->time = frames;
	qhead = (qhead + 1) & (QSIZE - 1);
}

static void draw_pointer(void)
{
	unsigned short y = 0, x = 0, tile = 0;

	if (shape >= 0) {
		y = 128 + py - hot_y[shape];
		x = 128 + px - hot_x[shape];
		/* high priority, palette 3, shape's 4 tiles */
		tile = 0x8000 | 0x6000 | (PTR_TILE + (shape << 2));
	}
	VDP_CTRL16 = 0x8F02;			/* INC = 2 */
	VDP_CTRL32 = 0x68000002;		/* write VRAM 0xA800 (sprite 0) */
	VDP_DATA16 = y;				/* 0 = off the top: hidden */
	VDP_DATA16 = 0x0500;			/* 16x16, end of the list */
	VDP_DATA16 = tile;
	VDP_DATA16 = x;
}

/* ---------------------------------------------------------------- */
/* Every frame, from the vblank interrupt (genesis_term_vblank)      */
/* ---------------------------------------------------------------- */

void genesis_gfx_vblank(void)
{
	int pad, dir, step = 0, x, y;
	unsigned short b;

	frames++;
	pad = get_pad(0);
	if ((pad & 0xF000) == 0xF000)
		pad = 0;			/* no joypad */
	pad &= 0x1FFF;

	/* d-pad: one pixel straight away (for small adjustments), then
	 * after a moment a steady speed that builds up to 4 pixels a frame */
	dir = pad & 0x0F;
	if (dir) {
		if (hold < 200)
			hold++;
		if (hold == 1)
			step = 1;
		else if (hold > 10) {
			x = 4 + ((hold - 10) >> 1);	/* quarter pixels */
			if (x > 16)
				x = 16;
			frac += x;
			step = frac >> 2;
			frac &= 3;
		}
		x = px;
		y = py;
		if (dir & 0x08) x += step;	/* right */
		if (dir & 0x04) x -= step;	/* left */
		if (dir & 0x02) y += step;	/* down */
		if (dir & 0x01) y -= step;	/* up */
		if (x < 0) x = 0;
		if (x > GENESIS_GFX_WIDTH - 1) x = GENESIS_GFX_WIDTH - 1;
		if (y < 0) y = 0;
		if (y > GENESIS_GFX_HEIGHT - 1) y = GENESIS_GFX_HEIGHT - 1;
		if (x != px || y != py) {
			px = x;
			py = y;
			moved = 1;
		}
	} else {
		hold = 0;
		frac = 0;
	}

	/* A = left button, B = right button */
	b = ((pad & 0x40) ? GENESIS_GFX_LEFT : 0) |
	    ((pad & 0x10) ? GENESIS_GFX_RIGHT : 0);
	if (b != buttons) {
		buttons = b;
		queue_event(b, pad);
	}
	/* C = double click */
	if ((pad & 0x20) && !(lastpad & 0x20) && !(buttons & GENESIS_GFX_LEFT)) {
		queue_event(buttons | GENESIS_GFX_LEFT, pad);
		queue_event(buttons, pad);
		queue_event(buttons | GENESIS_GFX_LEFT, pad);
		queue_event(buttons, pad);
	}
	/* Start: an event, so the program sees it in pad */
	if ((pad & 0x80) && !(lastpad & 0x80))
		queue_event(buttons, pad);
	lastpad = pad;

	draw_pointer();
	wake_up_interruptible(&gfx_wait);
}

/* ---------------------------------------------------------------- */
/* Entering and leaving                                              */
/* ---------------------------------------------------------------- */

static int gfx_enter(void)
{
	unsigned long flags;
	int i, r, c, t;

	if (genesis_gfx_active)
		return (owner == current) ? 0 : -EBUSY;

	save_flags(flags);
	cli();
	VDP_CTRL16 = 0x8134;			/* display off (faster access) */
	VDP_CTRL16 = 0x8F02;			/* INC = 2 */

	/* clear the screen tiles and the pointer tiles */
	VDP_CTRL32 = vram_write(0);
	for (i = (PTR_TILE + 4 * GENESIS_GFX_POINTER_SHAPES) * 8; i > 0; i--)
		VDP_DATA32 = 0;

	/* name table: the 40x28 tiles in order, palette 0 */
	VDP_CTRL32 = vram_write(0xC000);
	t = 0;
	for (r = 0; r < 32; r++)
		for (c = 0; c < 64; c++)
			VDP_DATA16 = (r < GENESIS_GFX_ROWS && c < GENESIS_GFX_COLS) ? t++ : 0;
	VDP_CTRL16 = 0x8406;			/* plane B name table = 0xC000 too */

	/* all colours black until the program sets its palette */
	VDP_CTRL32 = 0xC0000000;
	for (i = 0; i < 64; i++)
		VDP_DATA16 = 0;

	px = GENESIS_GFX_WIDTH / 2;
	py = GENESIS_GFX_HEIGHT / 2;
	shape = -1;
	hold = frac = 0;
	buttons = lastpad = 0;
	qhead = qtail = 0;
	moved = 0;
	draw_pointer();

	owner = current;
	genesis_gfx_active = 1;
	VDP_CTRL16 = 0x8174;			/* display on, vblank interrupt on */
	restore_flags(flags);
	return 0;
}

static void gfx_leave(void)
{
	unsigned long flags;

	save_flags(flags);
	cli();
	if (genesis_gfx_active) {
		genesis_gfx_active = 0;
		owner = 0;
		qbusy = 0;
		memset(qdirty, 0, sizeof qdirty);
		wake_up_interruptible(&dma_wait);
		genesis_term_restore();		/* redrawn at the next vblank */
	}
	restore_flags(flags);
	wake_up_interruptible(&gfx_wait);
}

void genesis_gfx_exit(struct task_struct *p)
{
	if (genesis_gfx_active && owner == p)
		gfx_leave();
}

/* ---------------------------------------------------------------- */
/* Drawing                                                           */
/* ---------------------------------------------------------------- */

/*
 * Uploading by DMA: the VDP reads the program's frame buffer itself.
 * While the picture is being drawn the VDP has little time for that
 * (18 bytes a line, with the CPU stopped meanwhile), but in the vertical
 * blank it takes about 8 KB. So the tiles to send are queued, and the
 * vblank interrupt sends what it can first thing (before reading the
 * keyboard); the flush ioctl sleeps until the queue is empty, leaving
 * the CPU to other programs (music). A small upload is sent at once.
 */
#define DMA_WORDS	512		/* a piece: 32 tiles */
#define VBLANK_WORDS	8192		/* per vblank (it may run on past it) */
#define NOW_WORDS	1024		/* this much is sent at once */


static volatile unsigned short dma_cmd;	/* see below */
static const unsigned char nbits[16] = { 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4 };

/* send n tiles from src to tile t onwards, interrupts off */
static void dma(int t, int n, const unsigned long *src)
{
	unsigned long addr = (unsigned long)src, vaddr = (unsigned long)t << 5;
	unsigned long words = (unsigned long)n << 4, k, room, cmd;

	while (words > 0) {
		/* a piece can't cross a 128 KB boundary of the source:
		 * the VDP's source counter wraps there */
		k = words;
		room = (0x20000 - (addr & 0x1FFFF)) >> 1;
		if (k > room)
			k = room;
		cmd = vram_write(vaddr) | 0x80;		/* CD5: DMA */
		VDP_CTRL16 = 0x8F02;
		VDP_CTRL16 = 0x9300 | (k & 0xFF);
		VDP_CTRL16 = 0x9400 | ((k >> 8) & 0xFF);
		VDP_CTRL16 = 0x9500 | ((addr >> 1) & 0xFF);
		VDP_CTRL16 = 0x9600 | ((addr >> 9) & 0xFF);
		VDP_CTRL16 = 0x9700 | ((addr >> 17) & 0x7F);
		VDP_CTRL16 = cmd >> 16;
		/* the word that starts the DMA is written from RAM: some
		 * consoles go wrong if the CPU fetches it from ROM */
		dma_cmd = cmd;
		VDP_CTRL16 = dma_cmd;
		addr += k << 1;
		vaddr += k << 1;
		words -= k;
	}
}

/* send the next run of queued tiles, at most max words; interrupts off.
 * Returns the words sent, 0 when the queue is empty. */
static int dma_next(int max)
{
	int t = qpos, n = 0, limit = max >> 4;

	while (t < GENESIS_GFX_TILES) {
		if (!(t & 31) && !qdirty[t >> 5]) {
			t += 32;
			continue;
		}
		if (qdirty[t >> 5] & (1UL << (t & 31)))
			break;
		t++;
	}
	if (t >= GENESIS_GFX_TILES) {
		qpos = GENESIS_GFX_TILES;
		qbusy = 0;
		return 0;
	}
	while (t + n < GENESIS_GFX_TILES && n < limit &&
	       (qdirty[(t + n) >> 5] & (1UL << ((t + n) & 31)))) {
		qdirty[(t + n) >> 5] &= ~(1UL << ((t + n) & 31));
		n++;
	}
	qpos = t + n;
	dma(t, n, qfb + (t << 3));
	return n << 4;
}

/* from the vblank interrupt, before anything else */
void genesis_gfx_vblank_dma(void)
{
	int budget = VBLANK_WORDS, w;

	if (!genesis_gfx_active || !qbusy)
		return;
	while (budget > 0 && (w = dma_next(budget < DMA_WORDS ? budget : DMA_WORDS)) > 0)
		budget -= w;
	if (!qbusy)
		wake_up_interruptible(&dma_wait);
}

static int gfx_flush(unsigned long arg)
{
	struct genesis_gfx_flush f;
	unsigned long flags, bits;
	int err, w, n = 0;

	err = verify_area(VERIFY_READ, (void *)arg, sizeof f);
	if (err)
		return err;
	memcpy_fromfs(&f, (void *)arg, sizeof f);
	err = verify_area(VERIFY_READ, f.fb, GENESIS_GFX_TILES * 32);
	if (!err)
		err = verify_area(VERIFY_WRITE, f.dirty, DIRTY_LONGS * 4);
	if (err)
		return err;

	/* no MMU: the program's memory can be read directly */
	save_flags(flags);
	cli();
	for (w = 0; w < DIRTY_LONGS; w++) {
		qdirty[w] |= f.dirty[w];
		f.dirty[w] = 0;
	}
	qdirty[DIRTY_LONGS - 1] &= (1UL << (GENESIS_GFX_TILES & 31)) - 1;
	for (w = 0; w < DIRTY_LONGS; w++) {
		bits = qdirty[w];
		for (; bits; bits >>= 4)
			n += nbits[bits & 15];
	}
	qfb = f.fb;
	qpos = 0;
	qbusy = n > 0;
	/* it must all be sent before returning: the program draws on
	 * afterwards. A big upload waits for the vertical blanks, unless
	 * a signal comes: then the rest is sent straight away */
	if (n > NOW_WORDS / 16)
		while (qbusy && !(current->signal & ~current->blocked))
			interruptible_sleep_on(&dma_wait);
	restore_flags(flags);
	do {
		save_flags(flags);
		cli();
		w = dma_next(DMA_WORDS);
		restore_flags(flags);
	} while (w);
	return 0;
}

static int gfx_palette(unsigned long arg)
{
	struct genesis_gfx_palette p;
	unsigned long flags;
	int err, i;

	err = verify_area(VERIFY_READ, (void *)arg, sizeof p);
	if (err)
		return err;
	memcpy_fromfs(&p, (void *)arg, sizeof p);
	if (p.first < 0 || p.count < 0 || p.first + p.count > 64)
		return -EINVAL;
	err = verify_area(VERIFY_READ, p.colors, p.count * 2);
	if (err)
		return err;
	save_flags(flags);
	cli();
	VDP_CTRL16 = 0x8F02;
	VDP_CTRL32 = 0xC0000000 | ((unsigned long)(p.first << 1) << 16);
	for (i = 0; i < p.count; i++)
		VDP_DATA16 = p.colors[i] & 0x0EEE;
	restore_flags(flags);
	return 0;
}

static int gfx_pointer(unsigned long arg)
{
	struct genesis_gfx_pointer p;
	unsigned long flags;
	int err;

	err = verify_area(VERIFY_READ, (void *)arg, sizeof p);
	if (err)
		return err;
	memcpy_fromfs(&p, (void *)arg, sizeof p);
	if (p.shape < -1 || p.shape >= GENESIS_GFX_POINTER_SHAPES)
		return -EINVAL;
	if (p.tiles) {
		if (p.shape < 0 || p.hot_x < 0 || p.hot_x > 15 ||
		    p.hot_y < 0 || p.hot_y > 15)
			return -EINVAL;
		err = verify_area(VERIFY_READ, p.tiles, 4 * 32);
		if (err)
			return err;
		hot_x[p.shape] = p.hot_x;
		hot_y[p.shape] = p.hot_y;
		save_flags(flags);
		cli();
		dma(PTR_TILE + (p.shape << 2), 4, p.tiles);
		restore_flags(flags);
	} else
		shape = p.shape;		/* drawn at the next frame */
	return 0;
}

static int gfx_input(unsigned long arg)
{
	struct genesis_gfx_input e;
	unsigned long flags;
	int err, got = 0;

	err = verify_area(VERIFY_WRITE, (void *)arg, sizeof e);
	if (err)
		return err;
	save_flags(flags);
	cli();
	if (qtail != qhead) {
		e = queue[qtail];
		qtail = (qtail + 1) & (QSIZE - 1);
		got = 1;
	} else {
		e.x = px;
		e.y = py;
		e.buttons = buttons;
		e.pad = lastpad;
		e.time = frames;
	}
	restore_flags(flags);
	memcpy_tofs((void *)arg, &e, sizeof e);
	return got;
}

static int gfx_wait_frames(struct tty_struct *tty, unsigned long n)
{
	unsigned long flags, until = frames + n;

	save_flags(flags);
	cli();
	while (genesis_gfx_active && (long)(frames - until) < 0 &&
	       qhead == qtail && !moved && !(tty && tty->read_cnt) &&
	       !(current->signal & ~current->blocked))
		interruptible_sleep_on(&gfx_wait);
	moved = 0;
	restore_flags(flags);
	return 0;
}

int genesis_gfx_ioctl(struct tty_struct *tty, unsigned int cmd, unsigned long arg)
{
	if (cmd == GENESIS_GFX_ENTER)
		return gfx_enter();
	if (!genesis_gfx_active || owner != current)
		return -EPERM;

	switch (cmd) {
	case GENESIS_GFX_LEAVE:
		gfx_leave();
		return 0;
	case GENESIS_GFX_FLUSH:
		return gfx_flush(arg);
	case GENESIS_GFX_PALETTE:
		return gfx_palette(arg);
	case GENESIS_GFX_POINTER:
		return gfx_pointer(arg);
	case GENESIS_GFX_INPUT:
		return gfx_input(arg);
	case GENESIS_GFX_WAIT:
		return gfx_wait_frames(tty, arg);
	case GENESIS_GFX_WARP:
		{
			int x = (short)(arg >> 16), y = (short)arg;

			if (x < 0 || x >= GENESIS_GFX_WIDTH ||
			    y < 0 || y >= GENESIS_GFX_HEIGHT)
				return -EINVAL;
			px = x;
			py = y;
			return 0;
		}
	}
	return -ENOIOCTLCMD;
}
