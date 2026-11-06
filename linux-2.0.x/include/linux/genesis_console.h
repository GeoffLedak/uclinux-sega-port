/*
 * genesis_console.h -- ioctls for the Sega Genesis VDP text console
 *
 * Used by drivers/char/genesis_vdp.c and by the sash "font" and "color"
 * built-in commands (user/sash/genesis.c). Issue them on the console tty,
 * e.g. ioctl(1, GENESIS_SET_FONT, 6).
 *
 * The console is a VT100/ANSI-style terminal of 24 rows and 36 or 48
 * columns; TIOCGWINSZ reports the current size.
 */
#ifndef _LINUX_GENESIS_CONSOLE_H
#define _LINUX_GENESIS_CONSOLE_H

/* arg = 8 for the 8x8 font (36 columns) or 6 for the 6x8 font (48 columns).
 * Switching font clears the screen. */
#define GENESIS_SET_FONT	0x4701
/* returns the current font width (8 or 6) */
#define GENESIS_GET_FONT	0x4702

/* arg = text colour as a Genesis CRAM value 0x0BGR, each of B, G, R 0x0 - 0xE
 * (odd values are rounded down by the hardware), e.g. 0xEEE white, 0x0C0 green */
#define GENESIS_SET_COLOR	0x4703
/* returns the current text colour */
#define GENESIS_GET_COLOR	0x4704

/* Test one 256-byte block of the cartridge RAM in place (see memtest in
 * user/memtest): arg = pointer to struct genesis_memtest, addr filled in.
 * The block's contents are kept, so any block can be tested, even one
 * the kernel or a program is using. */
#define GENESIS_MEMTEST		0x4705

struct genesis_memtest {
	unsigned long addr;	/* in: 256-byte aligned, 0x200000 - 0x3FFF00 */
	unsigned long result;	/* out: 0 ok, 1 data error, 2 address line
				 * error, 3 byte write error */
	unsigned long bad_addr;	/* out, on an error: where, */
	unsigned long wrote;	/* what should have been read */
	unsigned long read;	/* and what was */
};

#define GENESIS_RAM_START	0x200000	/* mapper slots 4-7 */
#define GENESIS_RAM_END		0x400000

/*
 * Graphics mode (used by the win GUI, user/win).
 *
 * GENESIS_GFX_ENTER hands the screen to the calling process: 320x224
 * pixels in 16 colours, made of 40x28 tiles of 8x8 pixels, numbered
 * left to right, top to bottom. The process draws into its own frame
 * buffer, laid out exactly like the tiles in video memory (tile t is the
 * 8 longs fb[t*8] - fb[t*8+7], one long per pixel row, 4 bits per pixel,
 * leftmost pixel in the top bits) and copies changed tiles to the screen
 * with GENESIS_GFX_FLUSH. Colour 0 is the backdrop.
 *
 * The mouse pointer is a sprite moved by the kernel every frame with the
 * d-pad of the joypad in port 1: A is the left button, B the right
 * button, and C a quick double click of the left button. The pointer's
 * colours are palette 3 (CRAM entries 48-63; 0 is transparent).
 *
 * The console's text is kept but not shown; output to it waits until
 * graphics mode ends (GENESIS_GFX_LEAVE, or when the process exits).
 */
#define GENESIS_GFX_COLS	40
#define GENESIS_GFX_ROWS	28
#define GENESIS_GFX_TILES	(GENESIS_GFX_COLS * GENESIS_GFX_ROWS)
#define GENESIS_GFX_WIDTH	320
#define GENESIS_GFX_HEIGHT	224

#define GENESIS_GFX_ENTER	0x4710	/* -EBUSY if another process has it */
#define GENESIS_GFX_LEAVE	0x4711

/* copy the tiles whose bits are set in dirty[] (tile t = bit t & 31 of
 * dirty[t >> 5]) from fb to the screen, and clear those bits */
#define GENESIS_GFX_FLUSH	0x4712
struct genesis_gfx_flush {
	const unsigned long *fb;	/* GENESIS_GFX_TILES * 8 longs */
	unsigned long *dirty;		/* (GENESIS_GFX_TILES + 31) / 32 longs */
};

/* set count CRAM entries from first (0 - 63), Genesis colours 0x0BGR */
#define GENESIS_GFX_PALETTE	0x4713
struct genesis_gfx_palette {
	int first, count;
	const unsigned short *colors;
};

/* With tiles: define pointer shape 0 - 7, a 16x16 sprite (4 tiles in
 * sprite order: top left, bottom left, top right, bottom right) whose
 * hot spot is (hot_x, hot_y). Without tiles: show shape `shape`, or
 * hide the pointer if it is -1. */
#define GENESIS_GFX_POINTER	0x4714
struct genesis_gfx_pointer {
	int shape;
	int hot_x, hot_y;
	const unsigned long *tiles;	/* 32 longs, or 0 */
};
#define GENESIS_GFX_POINTER_SHAPES	8

/* Read the next button event (returns 1): where the pointer was and the
 * buttons held after the change. With no event waiting, returns 0 and
 * the pointer and buttons as they are now. */
#define GENESIS_GFX_INPUT	0x4715
struct genesis_gfx_input {
	short x, y;
	unsigned short buttons;		/* GFX_LEFT | GFX_RIGHT */
	unsigned short pad;		/* raw joypad: 0 0 0 1 M X Y Z S A C B R L D U */
	unsigned long time;		/* frame number */
};
#define GENESIS_GFX_LEFT	1
#define GENESIS_GFX_RIGHT	2

/* Sleep until a button event, pointer movement or keyboard input, or
 * until arg frames have passed (arg = 1: the next frame) */
#define GENESIS_GFX_WAIT	0x4716

/* move the pointer: arg = (x << 16) | y */
#define GENESIS_GFX_WARP	0x4717

#ifdef __KERNEL__
struct tty_struct;
struct task_struct;

/* arch/m68knommu/platform/68000/68Katy/genesis_term.c */
void console_putchar(char c);			/* printk path, drops if full */
int  genesis_term_write(const unsigned char *buf, int count, int from_user);
int  genesis_term_write_room(void);
int  genesis_term_chars_in_buffer(void);
void genesis_term_set_font(int width);		/* applied at the next vblank */
int  genesis_term_get_font(void);
void genesis_term_set_color(int color);
int  genesis_term_get_color(void);
int  genesis_term_cols(void);
int  genesis_term_rows(void);

void genesis_term_restore(void);		/* redraw everything after graphics mode */

/* arch/m68knommu/platform/68000/68Katy/genesis_gfx.c */
extern int genesis_gfx_active;
int  genesis_gfx_ioctl(struct tty_struct *tty, unsigned int cmd, unsigned long arg);
void genesis_gfx_vblank(void);
void genesis_gfx_exit(struct task_struct *p);	/* from do_exit */

/* arch/m68knommu/platform/68000/68Katy/genesis_keyboard.c */
void genesis_tty_input(const char *s, int len);	/* feed bytes to the tty as input */
#endif

#endif /* _LINUX_GENESIS_CONSOLE_H */
