/*
 * beep -- play a continuous tone on the Sega Genesis sound hardware
 * until q is pressed.
 *
 * The tone comes from the PSG (SN76489), which is reached directly at
 * 0xC00011 (there is no MMU).  Tone channel 1 is used because the kernel's
 * terminal bell uses channel 0 and silences it after every beep.
 */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>

#define PSG		(*(volatile unsigned char *)0xC00011)

#define CHANNEL		1	/* PSG tone channel 0-2 */
#define DIVIDER		254	/* 3579545 Hz / (32 * 254) = about 440 Hz */
#define ATTENUATION	2	/* 0 = loudest, 15 = silent */

static struct termios saved;
static int have_saved;

static void tone_on(void)
{
	PSG = 0x80 | (CHANNEL << 5) | (DIVIDER & 0x0F);	/* tone, low 4 bits */
	PSG = (DIVIDER >> 4) & 0x3F;			/* tone, high 6 bits */
	PSG = 0x90 | (CHANNEL << 5) | ATTENUATION;	/* volume */
}

static void tone_off(void)
{
	PSG = 0x90 | (CHANNEL << 5) | 0x0F;		/* silent */
}

static void restore(void)
{
	tone_off();
	if (have_saved)
		tcsetattr(0, TCSANOW, &saved);
}

/* Ctrl-C, kill, hangup: stop the tone and quit, just like q */
static void on_signal(int sig)
{
	restore();
	_exit(0);
}

int main(void)
{
	struct termios raw;
	char c;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	signal(SIGQUIT, on_signal);

	/* Read single key presses, without echo; Ctrl-C still works */
	if (tcgetattr(0, &saved) == 0) {
		have_saved = 1;
		raw = saved;
		raw.c_lflag &= ~(ICANON | ECHO);
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0;
		tcsetattr(0, TCSANOW, &raw);
	}

	printf("BEEEEEEEP\n\nPress q to quit.\n");
	fflush(stdout);

	tone_on();
	while (read(0, &c, 1) == 1)
		if (c == 'q' || c == 'Q')
			break;

	restore();
	return 0;
}
