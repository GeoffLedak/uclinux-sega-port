/*
 * filemgr.c -- File Manager, running programs, and the music player
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include "win.h"

/* ---------------------------------------------------------------- */
/* Running Linux programs                                            */
/* ---------------------------------------------------------------- */

/* Start a program in the background, with its input and output on
 * /dev/null (the console is busy showing us). Returns its pid. */
int run_program(const char *cmd)
{
	char buf[256], *argv[16], path[280], *p;
	int argc = 0, pid, fd;
	struct stat st;

	strncpy(buf, cmd, sizeof buf - 1);
	buf[sizeof buf - 1] = 0;
	for (p = strtok(buf, " \t"); p && argc < 15; p = strtok(0, " \t"))
		argv[argc++] = p;
	argv[argc] = 0;
	if (!argc)
		return -1;
	if (strchr(argv[0], '/'))
		strcpy(path, argv[0]);
	else
		sprintf(path, "/bin/%s", argv[0]);
	if (stat(path, &st) < 0 || !(st.st_mode & 0111))
		return -1;
	pid = vfork();
	if (pid == 0) {
		fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
			if (fd > 2)
				close(fd);
		}
		execv(path, argv);
		_exit(127);
	}
	return pid;
}

/* ---------------- music ---------------- */

typedef struct {
	int pid;
	char name[40];
} Music;

static int music_proc(Win *w, int msg, int a, int b)
{
	Music *m = w->data;

	switch (msg) {
	case M_PAINT:
		draw_icon(8, 8, icon_music, 16);
		text(30, 8, m->pid > 0 ? "Playing:" : "Finished:", C_BLACK, F_BOLD);
		text(30, 18, m->name, C_BLACK, F_NORMAL);
		return 1;
	case M_TIMER:
		if (m->pid > 0 && kill(m->pid, 0) < 0) {
			m->pid = 0;
			win_set_text(w, "Music");
			win_show(ctl_find(w, 1), 0);
			invalidate(w);
		}
		return 1;
	case M_COMMAND:
		if (a == 1) {
			send(w, M_CLOSE, 0, 0);
			return 1;
		}
		break;
	case M_DESTROY:
		if (m->pid > 0)
			kill(m->pid, SIGTERM);
		free(m);
		return 1;
	}
	return def_proc(w, msg, a, b);
}

static void play_music(const char *path)
{
	char cmd[300];
	const char *base = strrchr(path, '/');
	Music *m = calloc(1, sizeof *m);
	Win *w;

	if (!m)
		return;
	sprintf(cmd, "vgmplay %.280s", path);
	m->pid = run_program(cmd);
	if (m->pid < 0) {
		free(m);
		msgbox(active, "Music", "Cannot start vgmplay.", MB_OK | MB_STOP);
		return;
	}
	strncpy(m->name, base ? base + 1 : path, sizeof m->name - 1);
	w = win_create(root, 90, 150, 150, 64, S_CAPTION | S_SYSMENU | S_MINBOX | S_BORDER,
		       "Music - Playing", music_proc, m);
	w->icon = icon_music;
	w->bg = C_SILVER;
	ctl_button(w, 52, 32, 44, 14, "Stop", 1, 1);
	set_timer(w, 30);
}

static int ends_with(const char *s, const char *e)
{
	int ls = strlen(s), le = strlen(e), i;

	if (ls < le)
		return 0;
	for (i = 0; i < le; i++) {
		int a = s[ls - le + i], b = e[i];
		if (a >= 'A' && a <= 'Z') a += 32;
		if (a != b)
			return 0;
	}
	return 1;
}

void open_file(const char *path)
{
	struct stat st;

	if (stat(path, &st) < 0) {
		msgbox(active, "File Manager", "Cannot find this file.", MB_OK | MB_EXCLAIM);
		return;
	}
	if (S_ISDIR(st.st_mode))
		app_filemgr(path);
	else if (ends_with(path, ".vgm") || ends_with(path, ".vgz"))
		play_music(path);
	else if (ends_with(path, ".bmp"))
		msgbox(active, "File Manager", "Open pictures from Paint\n(File, Open).", MB_OK | MB_INFO);
	else if (st.st_mode & 0111) {
		if (msgbox(active, "Run", "Programs run in the background, without\n"
			   "a screen: their output is thrown away.\n\nRun it anyway?",
			   MB_YESNO | MB_QUESTION) == ID_YES && run_program(path) < 0)
			msgbox(active, "Run", "Cannot run this program.", MB_OK | MB_STOP);
	} else
		app_notepad(path);
}

/* ---------------------------------------------------------------- */
/* File Manager                                                      */
/* ---------------------------------------------------------------- */

#define ID_LIST		100

typedef struct {
	char dir[256];
	Win *list;
	int files, dirs;
	long bytes;
} FileMgr;

enum { CM_OPEN = 1, CM_RUN, CM_DELETE, CM_MKDIR, CM_EXIT, CM_REFRESH, CM_ABOUT };

static MenuItem file_items[] = {
	{ "&Open\tEnter", CM_OPEN, 0 },
	{ "&Run...", CM_RUN, 0 },
	{ 0, 0, 0 },
	{ "&Delete...\tDel", CM_DELETE, 0 },
	{ "Create Di&rectory...", CM_MKDIR, 0 },
	{ 0, 0, 0 },
	{ "E&xit", CM_EXIT, 0 },
};
static MenuItem view_items[] = {
	{ "&Refresh\tF5", CM_REFRESH, 0 },
};
static MenuItem help_items[] = {
	{ "&About File Manager...", CM_ABOUT, 0 },
};
static Menu fm_menus[] = {
	{ "&File", file_items, COUNT(file_items) },
	{ "&View", view_items, COUNT(view_items) },
	{ "&Help", help_items, COUNT(help_items) },
};
static MenuBar fm_bar = { fm_menus, COUNT(fm_menus) };

static int cmp_entry(const void *a, const void *b)
{
	return strcmp(*(char * const *)a, *(char * const *)b);
}

static void join(char *out, const char *dir, const char *name)
{
	if (!strcmp(dir, "/"))
		sprintf(out, "/%.250s", name);
	else
		sprintf(out, "%.200s/%.50s", dir, name);
}

static void fm_fill(Win *w, FileMgr *f)
{
	DIR *d;
	struct dirent *de;
	struct stat st;
	char path[300], *names[300];
	int n = 0, i;

	busy(1);
	list_clear(f->list);
	f->files = f->dirs = 0;
	f->bytes = 0;
	if (strcmp(f->dir, "/"))
		list_add(f->list, "..", small_up);
	d = opendir(f->dir);
	if (d) {
		while ((de = readdir(d)) != 0 && n < 300) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
				continue;
			join(path, f->dir, de->d_name);
			if (stat(path, &st) < 0)
				continue;
			names[n] = malloc(strlen(de->d_name) + 2);
			if (!names[n])
				break;
			/* d = directory, p = program, f = file: sorts directories first */
			names[n][0] = S_ISDIR(st.st_mode) ? 'd' : 'f';
			if (!S_ISDIR(st.st_mode) && (st.st_mode & 0111))
				names[n][0] = 'p';
			strcpy(names[n] + 1, de->d_name);
			if (S_ISDIR(st.st_mode))
				f->dirs++;
			else {
				f->files++;
				f->bytes += st.st_size;
			}
			n++;
		}
		closedir(d);
	}
	qsort(names, n, sizeof names[0], cmp_entry);
	for (i = 0; i < n; i++) {
		list_add(f->list, names[i] + 1, names[i][0] == 'd' ? small_folder :
			 names[i][0] == 'p' ? small_program : small_file);
		free(names[i]);
	}
	list_set_sel(f->list, 0);
	sprintf(path, "File Manager - %.24s", f->dir);
	win_set_text(w, path);
	invalidate(w);
	busy(0);
}

static void fm_layout(Win *w, FileMgr *f)
{
	Rect c;

	win_client(w, &c);
	win_move(f->list, -1, -1, c.r + 2, c.b - 10 + 1);
}

static void fm_open(Win *w, FileMgr *f)
{
	const char *name = list_item(f->list, list_sel(f->list));
	char path[300], *p;
	struct stat st;

	if (!name[0])
		return;
	if (!strcmp(name, "..")) {
		p = strrchr(f->dir, '/');
		if (p == f->dir)
			p[1] = 0;
		else if (p)
			*p = 0;
		fm_fill(w, f);
		return;
	}
	join(path, f->dir, name);
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
		strncpy(f->dir, path, sizeof f->dir - 1);
		fm_fill(w, f);
		return;
	}
	open_file(path);
}

static int fm_proc(Win *w, int msg, int a, int b)
{
	FileMgr *f = w->data;
	char buf[300], path[300];
	const char *name;
	Rect c;

	switch (msg) {
	case M_PAINT:
		/* the status bar */
		win_client(w, &c);
		fill(0, c.b - 10, c.r, c.b, C_SILVER);
		hline(0, c.r, c.b - 10, C_BLACK);
		sprintf(buf, "%d dir(s), %d file(s), %ld bytes", f->dirs, f->files, f->bytes);
		text(3, c.b - 9, buf, C_BLACK, F_NORMAL);
		return 1;
	case M_SIZE:
		fm_layout(w, f);
		return 1;
	case M_ACTIVATE:
		if (a)
			win_set_focus(f->list);
		return 1;
	case M_KEY:
		if (a == K_ENTER) { fm_open(w, f); return 1; }
		if (a == K_DEL) { send(w, M_COMMAND, CM_DELETE, 0); return 1; }
		if (a == K_F1 + 4) { fm_fill(w, f); return 1; }
		if (a == K_BS) {
			list_set_sel(f->list, 0);
			if (!strcmp(list_item(f->list, 0), ".."))
				fm_open(w, f);
			return 1;
		}
		return 0;
	case M_COMMAND:
		switch (a) {
		case ID_LIST:
			if (b == N_DBLCLK)
				fm_open(w, f);
			return 1;
		case CM_OPEN:
			fm_open(w, f);
			return 1;
		case CM_RUN:
			buf[0] = 0;
			if (input_box(w, "Run", "Command Line:", buf, 200) && buf[0] &&
			    run_program(buf) < 0)
				msgbox(w, "Run", "Cannot run the program.", MB_OK | MB_STOP);
			return 1;
		case CM_DELETE:
			name = list_item(f->list, list_sel(f->list));
			if (!name[0] || !strcmp(name, ".."))
				return 1;
			join(path, f->dir, name);
			sprintf(buf, "Delete %.200s?", path);
			if (msgbox(w, "Confirm Delete", buf, MB_YESNO | MB_QUESTION) == ID_YES) {
				struct stat st;
				int r = (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? rmdir(path) : unlink(path);
				if (r < 0)
					msgbox(w, "File Manager", "Cannot delete it.\n(The ROM file system is read-only.)",
					       MB_OK | MB_EXCLAIM);
				fm_fill(w, f);
			}
			return 1;
		case CM_MKDIR:
			buf[0] = 0;
			if (input_box(w, "Create Directory", "Name:", buf, 60) && buf[0]) {
				join(path, f->dir, buf);
				if (mkdir(path, 0755) < 0)
					msgbox(w, "File Manager", "Cannot create it.\n(The ROM file system is read-only:\ntry /tmp.)",
					       MB_OK | MB_EXCLAIM);
				fm_fill(w, f);
			}
			return 1;
		case CM_REFRESH:
			fm_fill(w, f);
			return 1;
		case CM_EXIT:
			send(w, M_CLOSE, 0, 0);
			return 1;
		case CM_ABOUT:
			msgbox(w, "About File Manager", "File Manager\n\nDouble click a directory to open it,\n"
			       "a text file to edit it, or music\n(.vgm, .vgz) to play it.", MB_OK | MB_INFO);
			return 1;
		}
		break;
	case M_DESTROY:
		free(f);
		return 1;
	}
	return def_proc(w, msg, a, b);
}

void app_filemgr(const char *dir)
{
	FileMgr *f = calloc(1, sizeof *f);
	static int nx = 30, ny = 20;
	Win *w;

	if (!f)
		return;
	strncpy(f->dir, dir, sizeof f->dir - 1);
	w = win_create(root, nx, ny, 190, 150, S_OVERLAPPED, "File Manager", fm_proc, f);
	nx = (nx + 14) % 90;
	ny = (ny + 14) % 50;
	w->menu = &fm_bar;
	w->icon = icon_filemgr;
	f->list = ctl_list(w, 0, 0, 10, 10, ID_LIST);
	fm_layout(w, f);
	fm_fill(w, f);
	win_set_focus(f->list);
}
