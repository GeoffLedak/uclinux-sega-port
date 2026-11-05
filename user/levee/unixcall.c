/*
 * LEVEE, or Captain Video;  A vi clone
 *
 * Copyright (c) 1982-1997 David L Parsons
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by David L Parsons (orc@pell.chi.il.us).  My name may not be used
 * to endorse or promote products derived from this software without
 * specific prior written permission.  THIS SOFTWARE IS PROVIDED
 * AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING,
 * WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF MERCHANTIBILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.
 */
/*
 * Unix interface for levee
 */
#include "levee.h"
#include "extern.h"
#include <termios.h>
#ifdef GCC
#include <ioctls.h>
#endif
#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>

int
min(a,b)
int a, b;
{
    return (a>b) ? b : a;
}

int
max(a,b)
int a, b;
{
    return (a<b) ? b : a;
}

strput(s)
register char *s;
{
    if (s)
	write(1, s, strlen(s));
}


#ifndef GCC
char *
basename(s)
char *s;
{
    char *rindex();
    register char *p;

    if (p=strrchr(s,'/'))
	return 1+p;
    return s;
}
#endif


static int ioset = 0;
static struct termios old;

initcon()
{
    struct termios new;

    if (!ioset) {
	tcgetattr(0, &old);
        /*ioctl(0, TCGETS, &old);*/	/* get editing keys */

        erasechar = old.c_cc[VERASE];
        eraseline = old.c_cc[VKILL];

        new = old;

	new.c_iflag &= ~(IXON|IXOFF|IXANY|ICRNL|INLCR);
	new.c_lflag &= ~(ICANON|ISIG|ECHO);
	new.c_oflag = 0;

	tcsetattr(0, TCSANOW, &new);
        /*ioctl(0, TCSETS, &new);*/
        ioset=1;
    }
}

fixcon()
{
    if (ioset) {
	 tcsetattr(0, TCSANOW, &old);
         /*ioctl(0, TCSETS, &old);*/
/*   More or less blind attempt to fix console corruption.
     T. Huld 1998-05-19
         ioctl(0, TCSETA, &old);
*/
         ioset = 0;
    }
}

/*
 * Screen size from the terminal; 24x79 if it can't tell us.
 */
screensz(lines, cols)
int *lines, *cols;
{
    struct winsize ws;

    *lines = 24;
    *cols = 79;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 1 && ws.ws_col > 1) {
	*lines = ws.ws_row;
	*cols = ws.ws_col;
    }
}

/*
 * Read a key.  The arrow keys' escape sequences (ESC [ A etc.) become
 * ^K ^J ^L ^H, which move up, down, right and left in command mode.
 * Other escape sequences (Home, Delete, function keys...) are swallowed,
 * so they can't be taken as commands.  An ESC with nothing straight after
 * it is the Escape key.
 */
static int pushback = -1;

static int
rawkey()
{
    unsigned char c;
    int n;

    if (pushback >= 0) {
	c = pushback;
	pushback = -1;
	return c;
    }
    for (;;) {
	n = read(0, &c, 1);
	if (n == 1)
	    return c;
	if (n < 0 && errno == EINTR)
	    continue;
	return ESC;			/* end of input: act like Escape */
    }
}

getKey()
{
    int c, n;

    for (;;) {
	c = rawkey();
	if (c != ESC)
	    return c;
	n = 0;
	if (ioctl(0, FIONREAD, &n) < 0 || n <= 0)
	    return ESC;			/* the Escape key */
	c = rawkey();
	if (c != '[' && c != 'O') {	/* not a sequence: ESC, then c */
	    pushback = c;
	    return ESC;
	}
	do				/* parameters, up to the final byte */
	    c = rawkey();
	while (c < 0x40 || c > 0x7E);
	switch (c) {
	case 'A': return 'K' & 0x1F;	/* up */
	case 'B': return 'J' & 0x1F;	/* down */
	case 'C': return 'L' & 0x1F;	/* right */
	case 'D': return 'H' & 0x1F;	/* left */
	}
	/* anything else: ignore it and read another key */
    }
}
