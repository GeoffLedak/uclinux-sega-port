/*
 * genesis.c -- sash built-in commands for the Sega Genesis VDP console
 *
 *   font               show the current font
 *   font 6x8 | 8x8     switch font (clears the screen)
 *   margin             show the blank margin round the text, in pixels
 *   margin 0|8|16|24|32  set it (clears the screen): 16 at boot keeps the
 *                      text clear of a CRT's overscan, 0 fills the screen
 *   color              show the current text colour
 *   color BGR          set the text colour, a Genesis CRAM value in hex
 *                      (0x0BGR, each digit 0-E), e.g. EEE white, 0C0 green
 *
 * They use ioctls on the console tty, see <linux/genesis_console.h>.
 */

#include "sash.h"

#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <linux/genesis_console.h>

/* Print the screen size as it will be once a font or margin change shows. */
static void
print_size()
{
	struct winsize	ws;
	int		fd;

	for (fd = 1; fd >= 0; fd--)
		if (ioctl(fd, TIOCGWINSZ, &ws) == 0) {
			printf(" (%d columns, %d rows)", ws.ws_col, ws.ws_row);
			return;
		}
}

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
		printf("%dx8", width);
		print_size();
		printf("\n");
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

void
do_margin(argc, argv)
	char	**argv;
{
	int	margin;
	char	*end;

	if (argc < 2) {
		margin = console_ioctl(GENESIS_GET_MARGIN, 0);
		if (margin < 0) {
			perror("margin");
			return;
		}
		printf("%d pixels", margin);
		print_size();
		printf("\n");
		return;
	}

	margin = strtol(argv[1], &end, 10);
	if (*argv[1] == '\0' || *end != '\0' || margin < 0 || margin > 32
	    || (margin & 7)) {
		fprintf(stderr, "usage: margin [0|8|16|24|32]\n"
				"  pixels of border (16 for a CRT)\n");
		return;
	}

	if (console_ioctl(GENESIS_SET_MARGIN, margin) < 0)
		perror("margin");
}
