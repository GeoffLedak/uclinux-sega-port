/*
 * genesis_console.h -- ioctls for the Sega Genesis VDP text console
 *
 * Used by drivers/char/genesis_vdp.c and by the sash "font" and "color"
 * built-in commands (user/sash/genesis.c). Issue them on the console tty,
 * e.g. ioctl(1, GENESIS_SET_FONT, 6).
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

#endif /* _LINUX_GENESIS_CONSOLE_H */
