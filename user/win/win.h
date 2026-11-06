/*
 * win.h -- a small windowing system in the style of the early 1990s
 *
 * gfx.c    drawing into the frame buffer (320x224, 16 colours)
 * hw.c     the Genesis graphics mode ioctls (host.c: a test harness)
 * wm.c     windows, frames, menus, scroll bars, the event loop
 * ctl.c    controls (buttons, edit fields, list boxes), message boxes,
 *          the file dialog
 * the rest: Program Manager and the accessories
 */
#ifndef WIN_H
#define WIN_H

#include "art.h"

typedef unsigned int u32;		/* 32 bits on the 68000 and the host */

#define SCR_W		320
#define SCR_H		224
#define SCR_COLS	40
#define SCR_ROWS	28
#define SCR_TILES	(SCR_COLS * SCR_ROWS)
#define DIRTY_WORDS	((SCR_TILES + 31) / 32)

/* the palette (the standard 16 PC colours) */
enum {
	C_BLACK, C_MAROON, C_GREEN, C_OLIVE, C_NAVY, C_PURPLE, C_TEAL, C_SILVER,
	C_GRAY, C_RED, C_LIME, C_YELLOW, C_BLUE, C_FUCHSIA, C_AQUA, C_WHITE
};

/* colour scheme (Control Panel can change these) */
extern unsigned char col_desktop, col_workspace, col_title, col_title_text,
	col_inactive, col_inactive_text, col_border, col_menu, col_hilite,
	col_window;

typedef struct { short l, t, r, b; } Rect;	/* right and bottom exclusive */

/* ---------------- gfx.c ---------------- */
/* the 68000 multiplies and divides 16-bit numbers in one instruction;
 * with int, gcc calls a slow library routine. For values known to fit
 * (0 - 65535 for UDIV16, -32768 - 32767 for MUL16): */
#define UDIV16(a, b)	((int)((unsigned short)(a) / (unsigned short)(b)))
#define MUL16(a, b)	((int)(short)(a) * (short)(b))

#define FB_LONGS	(SCR_TILES * 8 + 8)	/* (8 to spare: copies read one tile on) */
extern u32 fb[FB_LONGS];
extern u32 dirty[DIRTY_WORDS];
extern Rect clip;			/* screen coordinates */
extern int org_x, org_y;		/* screen position of drawing (0, 0) */

#define FONT_H		8		/* glyph height */
#define LINE_H		10		/* text line spacing */
#define FIXED_W		6		/* width of the fixed font */

void gfx_set(int ox, int oy, const Rect *c);	/* origin and clip */
void gfx_screen(void);				/* whole screen, origin 0, 0 */
void mark_dirty(int l, int t, int r, int b);	/* screen coordinates */
void copy_rect(int l, int t, int r, int b, int dx, int dy);	/* screen, may overlap */
void fill(int l, int t, int r, int b, int c);
void hline(int x0, int x1, int y, int c);	/* x1 exclusive */
void vline(int x, int y0, int y1, int c);	/* y1 exclusive */
void frame(int l, int t, int r, int b, int c);
void pixel(int x, int y, int c);
int  get_pixel(int x, int y);			/* screen coordinates */
void line(int x0, int y0, int x1, int y1, int c);
void bevel(int l, int t, int r, int b, int up);	/* white/gray 3D edge */
void xor_frame(int l, int t, int r, int b, int thick);	/* screen, dithered */
void xor_fill(int l, int t, int r, int b);	/* invert, client coordinates */
void draw_icon(int x, int y, const unsigned char *img, int size);
void dotted_frame(int l, int t, int r, int b);	/* focus rectangle */

/* fonts: 0 = proportional, 1 = bold, 2 = fixed 6x8 */
#define F_NORMAL	0
#define F_BOLD		1
#define F_FIXED		2
int  text(int x, int y, const char *s, int c, int font);
int  textn(int x, int y, const char *s, int n, int c, int font);
int  text_w(const char *s, int font);
int  textn_w(const char *s, int n, int font);
int  char_w(int ch, int font);
/* draws "&File" as File with the F underlined; returns the width */
int  text_mn(int x, int y, const char *s, int c, int font);
int  text_mn_w(const char *s, int font);
int  mnemonic(const char *s);			/* the letter after '&', lower case */

int  rect_and(Rect *a, const Rect *b);		/* a = a & b, 0 if empty */
int  in_rect(const Rect *r, int x, int y);
/* (inline: it is called very often) */
static __inline__ void set_rect(Rect *r, int l, int t, int rr, int b)
{
	r->l = l; r->t = t; r->r = rr; r->b = b;
}

/* ---------------- hw.c / host.c ---------------- */
struct hw_input {
	int x, y, buttons, pad;
	unsigned long time;
};
#define B_LEFT		1
#define B_RIGHT		2

int  hw_init(void);			/* 0 = ok */
void hw_exit(void);
void hw_flush(void);			/* copy the dirty tiles */
void hw_palette(int first, int n, const unsigned short *c);
void hw_pointer_define(int shape, int hx, int hy, const u32 *tiles);
void hw_pointer(int shape);		/* -1 = hidden */
int  hw_input(struct hw_input *e);	/* 1 = a button event */
void hw_wait(int frames);
int  hw_keys(char *buf, int n);		/* non-blocking keyboard read */
void hw_warp(int x, int y);
void hw_beep(void);
unsigned long hw_frames(void);		/* frames since start */

/* ---------------- wm.c ---------------- */
typedef struct Win Win;
typedef int (*Proc)(Win *w, int msg, int a, int b);

/* messages (a, b): mouse messages have client coordinates */
enum {
	M_CREATE = 1, M_DESTROY, M_PAINT, M_LDOWN, M_LUP, M_LDBL, M_RDOWN,
	M_RUP, M_MOVE, M_KEY, M_COMMAND, M_TIMER, M_SIZE, M_CLOSE, M_FOCUS,
	M_ACTIVATE, M_VSCROLL, M_HSCROLL, M_CURSOR, M_INITMENU, M_CLICK
};

/* keys (M_KEY a): ASCII, or these */
#define K_BS		8
#define K_TAB		9
#define K_ENTER		13
#define K_ESC		27
#define K_UP		0x101
#define K_DOWN		0x102
#define K_RIGHT		0x103
#define K_LEFT		0x104
#define K_HOME		0x105
#define K_INS		0x106
#define K_DEL		0x107
#define K_END		0x108
#define K_PGUP		0x109
#define K_PGDN		0x10A
#define K_F1		0x111		/* K_F1 + n - 1 = Fn */
#define K_START		0x130		/* the pad's Start button */
#define K_CTRL		0x2000		/* with arrows */
#define K_ALT		0x4000
#define CTRL(c)		((c) & 0x1F)

/* scroll bar codes (M_VSCROLL / M_HSCROLL a; b = position for SB_THUMB) */
enum { SB_LINEUP, SB_LINEDOWN, SB_PAGEUP, SB_PAGEDOWN, SB_THUMB, SB_TOP, SB_BOTTOM };

/* styles */
#define S_CAPTION	0x0001
#define S_SYSMENU	0x0002
#define S_MINBOX	0x0004
#define S_MAXBOX	0x0008
#define S_THICK		0x0010		/* sizing border */
#define S_DLGFRAME	0x0020		/* dialog border */
#define S_BORDER	0x0040		/* thin black border */
#define S_VSCROLL	0x0080
#define S_HSCROLL	0x0100
#define S_CHILD		0x0200		/* a control */
#define S_HIDDEN	0x0400
#define S_TABSTOP	0x0800
#define S_DISABLED	0x1000
#define S_MDI		0x2000		/* a framed window inside another's client area */
#define S_DEFAULT	0x4000		/* the default push button */
#define S_DIALOG	0x8000		/* modal dialog: Tab, Enter, Esc */
#define S_OVERLAPPED	(S_CAPTION | S_SYSMENU | S_MINBOX | S_MAXBOX | S_THICK)

/* window states */
#define W_NORMAL	0
#define W_MIN		1
#define W_MAX		2

/* menus: an item with text 0 is a separator; "&Open...\tCtrl+O" */
#define MF_GRAY		1
#define MF_CHECK	2
typedef struct { const char *text; short id; unsigned char flags; } MenuItem;
typedef struct { const char *title; MenuItem *items; int n; } Menu;
typedef struct { Menu *menus; int n; } MenuBar;
#define COUNT(a)	((int)(sizeof(a) / sizeof((a)[0])))

/* system menu commands (M_COMMAND a) */
#define SC_RESTORE	0x7F01
#define SC_MOVE		0x7F02
#define SC_SIZE		0x7F03
#define SC_MINIMIZE	0x7F04
#define SC_MAXIMIZE	0x7F05
#define SC_CLOSE	0x7F06
#define SC_SWITCH	0x7F07
#define SC_NEXT		0x7F08

/* control notifications (M_COMMAND b) and dialog results */
#define N_CLICKED	0
#define N_SELCHANGE	1
#define N_DBLCLK	2
#define N_CHANGE	3
#define ID_OK		1
#define ID_CANCEL	2
#define ID_YES		6
#define ID_NO		7

struct Win {
	Win *parent, *child, *next;	/* child: bottom-most; next: the one above */
	Rect r;				/* outer rectangle, parent's client coordinates */
	Rect nr;			/* the normal rectangle while maximized */
	short ix, iy;			/* icon position (minimized), -1 = unplaced */
	unsigned short style;
	unsigned char state;
	signed char bg;			/* client background, -1 = the proc paints it */
	signed char fg;			/* buttons: label colour */
	short id;
	char text[40];
	const unsigned char *icon;
	MenuBar *menu;
	Proc proc;
	void *data;
	Win *focus;			/* frames: the control with the keyboard */
	Win *active;			/* MDI parents: the active child */
	Win *owner;			/* dialogs: the window it belongs to */
	short timer, timer_left;	/* M_TIMER every timer frames */
	short vpos, vmax, vpage;	/* scroll bars: position 0 - max */
	short hpos, hmax, hpage;
	int result;			/* dialogs */
	char done;
	char dead;
	char was_max;			/* minimized from maximized */
};

extern Win *root;			/* the desktop */
extern Win *active;			/* the active top-level window */
extern int quit;			/* end the session */

Win *win_create(Win *parent, int x, int y, int w, int h, int style,
		const char *title, Proc proc, void *data);
void win_destroy(Win *w);
int  def_proc(Win *w, int msg, int a, int b);
int  send(Win *w, int msg, int a, int b);
void win_show(Win *w, int show);
void win_move(Win *w, int x, int y, int wd, int ht);
void win_activate(Win *w);
void win_set_state(Win *w, int state);
void win_set_text(Win *w, const char *s);
void win_set_focus(Win *w);
Win *win_focus(void);			/* the window keys go to */
Win *win_top(Win *w);			/* its top-level window */
Win *win_frame(Win *w);			/* the nearest framed (captioned) window */
void win_client(Win *w, Rect *c);	/* client area, own coordinates (0, 0, w, h) */
void win_screen(Win *w, Rect *r);	/* outer rectangle on the screen */
void win_client_screen(Win *w, Rect *r);
void invalidate(Win *w);		/* whole window */
void invalidate_client(Win *w, int l, int t, int r, int b);
void invalidate_screen(const Rect *r);
void set_timer(Win *w, int frames);	/* 0 = off */
void set_scroll(Win *w, int vert, int pos, int max, int page);
void scroll_client(Win *w, int dx, int dy);
extern int wm_copy;	/* move what it shows; the rest is redrawn */
void set_pointer(int shape);
void center_window(Win *w, Win *over);
void cascade(Win *parent);
void tile(Win *parent);
void arrange_icons(Win *parent);
int  is_child_of(Win *w, Win *parent);
void beep(void);

void wm_init(void);
void run(void);				/* the event loop until quit */
void draw_icon_label(int x, int y, int w, const unsigned char *icon,
		     const char *label, int selected, int textc);
int  dialog_run(Win *dlg);		/* modal; returns dlg->result */
void dialog_end(Win *dlg, int result);
void pump(int idle);			/* handle the events waiting */
void repaint_now(void);
void busy(int on);			/* the hourglass */
void task_list(void);
int  popup_menu(Win *owner, Menu *m, int x, int y);	/* screen coords; returns id or 0 */

/* ---------------- ctl.c ---------------- */
Win *ctl_button(Win *p, int x, int y, int w, int h, const char *text, int id, int def);
Win *ctl_label(Win *p, int x, int y, int w, int h, const char *text);
Win *ctl_edit(Win *p, int x, int y, int w, int h, const char *text, int id);
Win *ctl_list(Win *p, int x, int y, int w, int h, int id);
Win *ctl_check(Win *p, int x, int y, int w, int h, const char *text, int id, int radio);
Win *ctl_icon(Win *p, int x, int y, const unsigned char *icon);
Win *ctl_find(Win *dlg, int id);
const char *edit_text(Win *e);
void edit_set(Win *e, const char *s);
void list_clear(Win *l);
void list_add(Win *l, const char *s, const unsigned char *small_icon);
int  list_sel(Win *l);
void list_set_sel(Win *l, int i);
const char *list_item(Win *l, int i);
int  list_count(Win *l);
int  check_get(Win *c);
void check_set(Win *c, int on);
Win *dialog_create(Win *owner, int w, int h, const char *title, Proc proc, void *data);

#define MB_OK		0
#define MB_OKCANCEL	1
#define MB_YESNO	2
#define MB_YESNOCANCEL	3
#define MB_STOP		0x10
#define MB_QUESTION	0x20
#define MB_EXCLAIM	0x30
#define MB_INFO		0x40
int  msgbox(Win *owner, const char *title, const char *text, int flags);
/* returns 1 and fills path (256 bytes) if a file was chosen */
int  file_dialog(Win *owner, const char *title, char *path, int save);
int  input_box(Win *owner, const char *title, const char *prompt, char *buf, int max);

/* ---------------- the applications ---------------- */
void progman_start(void);
void app_notepad(const char *file);
void app_calc(void);
void app_clock(void);
void app_filemgr(const char *dir);
void app_paint(void);
void app_puzzle(void);
void app_control(void);
void app_readme(void);
int  run_program(const char *path);	/* a Linux program in the background */
void open_file(const char *path);	/* by its kind: text, music, program */
void app_notepad_text(const char *title, const char *text);
void clip_set(const char *s, int n);	/* the clipboard */
const char *clip_get(int *n);
extern int dbl_time, swap_buttons;	/* mouse settings */

#endif
