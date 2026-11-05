/*
 * genesis.c -- sash built-in commands for the Sega Genesis VDP console
 *
 *   font               show the current font
 *   font 6x8 | 8x8     switch font (clears the screen)
 *   color              show the current text colour
 *   color BGR          set the text colour, a Genesis CRAM value in hex
 *                      (0x0BGR, each digit 0-E), e.g. EEE white, 0C0 green
 *
 * Both use ioctls on the console tty, see <linux/genesis_console.h>.
 */

#include "sash.h"

#include <stdlib.h>
#include <sys/ioctl.h>
#include <linux/genesis_console.h>

/* Issue the ioctl on whichever standard fd is the console tty. */
static int
console_ioctl(cmd, arg)
	int		cmd;
	unsigned long	arg;
{
	int	fd;
	int	ret = -1;

	for (fd = 1; fd >= 0; fd--) {
		ret = ioctl(fd, cmd, arg);
		if (ret >= 0)
			return ret;
	}
	return ret;
}

void
do_font(argc, argv)
	char	**argv;
{
	int	width;

	if (argc < 2) {
		width = console_ioctl(GENESIS_GET_FONT, 0);
		if (width < 0) {
			perror("font");
			return;
		}
		printf("%dx8 (%d columns)\n", width, width == 6 ? 48 : 36);
		return;
	}

	if (strcmp(argv[1], "6x8") == 0 || strcmp(argv[1], "6") == 0)
		width = 6;
	else if (strcmp(argv[1], "8x8") == 0 || strcmp(argv[1], "8") == 0)
		width = 8;
	else {
		fprintf(stderr, "usage: font [6x8|8x8]\n");
		return;
	}

	if (console_ioctl(GENESIS_SET_FONT, width) < 0)
		perror("font");
}

void
do_color(argc, argv)
	char	**argv;
{
	long	color;
	char	*end;

	if (argc < 2) {
		color = console_ioctl(GENESIS_GET_COLOR, 0);
		if (color < 0) {
			perror("color");
			return;
		}
		printf("%03lX\n", color);
		return;
	}

	color = strtol(argv[1], &end, 16);
	if (*argv[1] == '\0' || *end != '\0' || color < 0 || color > 0xFFF) {
		fprintf(stderr, "usage: color [BGR]   hex 000-EEE, e.g. EEE white, 0C0 green\n");
		return;
	}

	if (console_ioctl(GENESIS_SET_COLOR, color) < 0)
		perror("color");
}
