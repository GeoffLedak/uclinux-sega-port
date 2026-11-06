/*
 * win -- a graphical shell for the Sega Genesis, in the style of the
 * early 1990s. Started with "win" from the shell; Exit (or
 * closing Program Manager) returns to the shell.
 *
 * The mouse pointer is moved with the d-pad of the joypad in port 1:
 * A is the left button, B the right button, C a double click.
 * The keyboard in port 2 works as usual (Alt+F4, Alt+Tab, F10 ...).
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "win.h"

#ifdef HOST
void host_open_script(const char *name);
#endif
void bench(void);
void check(void);
void bench_report(void);

static void reap(int sig)
{
	while (waitpid(-1, 0, WNOHANG) > 0)
		;
	signal(SIGCHLD, reap);
}

int main(int argc, char **argv)
{
	int do_bench = argc > 1 && !strcmp(argv[1], "-b");
	int do_check = argc > 1 && !strcmp(argv[1], "-c");

#ifdef HOST
	if (argc > 1 && !do_bench && !do_check)
		host_open_script(argv[1]);
#endif
	if (hw_init()) {
		fprintf(stderr, "win: this needs the Genesis console (and no other win running)\n");
		return 1;
	}
	signal(SIGCHLD, reap);
	signal(SIGPIPE, SIG_IGN);
	wm_init();
	progman_start();
	if (do_bench)
		bench();
	else if (do_check)
		check();
	else
		run();
	/* close the windows, so music players and the like are stopped */
	while (root->child)
		win_destroy(root->child);
	hw_exit();
	if (do_bench || do_check)
		bench_report();
	return 0;
}
