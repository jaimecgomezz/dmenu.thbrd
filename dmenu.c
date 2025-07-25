/* See LICENSE file for copyright and license details. */
#include <ctype.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif
#include <X11/Xft/Xft.h>

/* Patch incompatibility overrides */

#include "drw.h"
#include "util.h"

/* macros */
#define INTERSECT(x,y,w,h,r)  (MAX(0, MIN((x)+(w),(r).x_org+(r).width)  - MAX((x),(r).x_org)) \
                             * MAX(0, MIN((y)+(h),(r).y_org+(r).height) - MAX((y),(r).y_org)))
#define TEXTW(X)              (drw_fontset_getwidth(drw, (X)) + lrpad)

/* enums */
enum {
	SchemeNorm,
	SchemeSel,
	SchemeOut,
	SchemeBorder,
	SchemeNormHighlight,
	SchemeSelHighlight,
	SchemeCursor,
	SchemeCaret,
	SchemeLast,
}; /* color schemes */

struct item {
	char *text;
	char *stext;
	struct item *left, *right;
	int out;
	double distance;
};

static char text[BUFSIZ] = "";
static char pipeout[8] = " | dmenu";
static char *embed;
static int bh, mw, mh;
static int inputw = 0, promptw;
static int passwd = 0;
static int lrpad; /* sum of left and right padding */
static int reject_no_match = 0;
static size_t cursor;
static struct item *items = NULL;
static struct item *matches, *matchend;
static struct item *prev, *curr, *next, *sel;
static int mon = -1, screen;
static unsigned int sortmatches = 1;
static unsigned int preselected = 0;

static Atom clip, utf8;
static Display *dpy;
static Window root, parentwin, win;
static XIC xic;

static Drw *drw;
static Clr *scheme[SchemeLast];

#include "patch/include.h"

#include "config.h"

static unsigned int
textw_clamp(const char *str, unsigned int n)
{
	unsigned int w = drw_fontset_getwidth_clamp(drw, str, n) + lrpad;
	return MIN(w, n);
}

static void appenditem(struct item *item, struct item **list, struct item **last);
static void calcoffsets(void);
static void cleanup(void);
static char * cistrstr(const char *s, const char *sub);
static int drawitem(struct item *item, int x, int y, int w);
static void drawmenu(void);
static void grabfocus(void);
static void grabkeyboard(void);
static void match(void);
static void insert(const char *str, ssize_t n);
static size_t nextrune(int inc);
static void movewordedge(int dir);
static void keypress(XKeyEvent *ev);
static void paste(void);
static void readstdin(void);
static void run(void);
static void setup(void);
static void usage(void);

static int (*fstrncmp)(const char *, const char *, size_t) = strncasecmp;
static char *(*fstrstr)(const char *, const char *) = cistrstr;

#include "patch/include.c"

static void
appenditem(struct item *item, struct item **list, struct item **last)
{
	if (*last)
		(*last)->right = item;
	else
		*list = item;

	item->left = *last;
	item->right = NULL;
	*last = item;
}

static void
calcoffsets(void)
{
	int i, n, rpad = 0;

	if (lines > 0) {
		n = lines * bh;
	} else {
		rpad = TEXTW(numbers);
		n = mw - (promptw + inputw + TEXTW(symbol_1) + TEXTW(symbol_2) + rpad);
	}
	/* calculate which items will begin the next page and previous page */
	for (i = 0, next = curr; next; next = next->right)
		if ((i += (lines > 0) ? bh : textw_clamp(next->text, n)) > n)
			break;
	for (i = 0, prev = curr; prev && prev->left; prev = prev->left)
		if ((i += (lines > 0) ? bh : textw_clamp(prev->left->text, n)) > n)
			break;
}

static void
cleanup(void)
{
	size_t i;

	XUngrabKeyboard(dpy, CurrentTime);
	for (i = 0; i < SchemeLast; i++)
		free(scheme[i]);
	for (i = 0; items && items[i].text; ++i) {
		free(items[i].text);
	}
	free(items);
	drw_free(drw);
	XSync(dpy, False);
	XCloseDisplay(dpy);
}

static char *
cistrstr(const char *s, const char *sub)
{
	size_t len;

	for (len = strlen(sub); *s; s++)
		if (!strncasecmp(s, sub, len))
			return (char *)s;
	return NULL;
}

static int
drawitem(struct item *item, int x, int y, int w)
{
	int r;
	char *text = item->stext;

	if (item == sel)
		drw_setscheme(drw, scheme[SchemeSel]);
	else if (item->out)
		drw_setscheme(drw, scheme[SchemeOut]);
	else
		drw_setscheme(drw, scheme[SchemeNorm]);

	r = drw_text(drw
		, x
		, y
		, w
		, bh
		, lrpad / 2
		, text
		, 0
		);
	drawhighlights(item, x, y, w);
	return r;
}

static void
drawmenu(void)
{
	unsigned int curpos;
	struct item *item;
	int x = 0, y = 0, w, rpad = 0, itw = 0, stw = 0;
	int fh = drw->fonts->h;
	char *censort;

	drw_setscheme(drw, scheme[SchemeNorm]);
	drw_rect(drw, 0, 0, mw, mh, 1, 1);

	if (prompt && *prompt) {
		x = drw_text(drw, x, 0, promptw, bh, lrpad / 2, prompt, 0
		);
	}
	/* draw input field */
	w = (lines > 0 || !matches) ? mw - x : inputw;

	drw_setscheme(drw, scheme[SchemeNorm]);
	if (passwd) {
		censort = ecalloc(1, sizeof(text));
		memset(censort, '.', strlen(text));
		drw_text(drw, x, 0, w, bh, lrpad / 2, censort, 0
		);
		drw_text(drw, x, 0, w, bh, lrpad / 2, censort, 0
		);
		free(censort);
	} else {
		drw_text(drw, x, 0, w, bh, lrpad / 2, text, 0
		);
	}

	curpos = TEXTW(text) - TEXTW(&text[cursor]);
	curpos += lrpad / 2 - 1;

	if (using_vi_mode && text[0] != '\0') {
		drw_setscheme(drw, scheme[SchemeCursor]);
		char vi_char[] = {text[cursor], '\0'};
		drw_text(drw, x + curpos, 0, TEXTW(vi_char) - lrpad, bh, 0, vi_char, 0);
	} else if (using_vi_mode) {
		drw_setscheme(drw, scheme[SchemeNorm]);
		drw_rect(drw, x + curpos, 2, lrpad / 2, bh - 4, 1, 0);
	} else
	if (curpos < w) {
		drw_setscheme(drw, scheme[SchemeCaret]);
		drw_rect(drw, x + curpos, 2 + (bh-fh)/2, 2, fh - 4, 1, 0);
	}

	recalculatenumbers();
	rpad = TEXTW(numbers);
	rpad += border_width;
	if (lines > 0) {
		/* draw vertical list */
		for (item = curr; item != next; item = item->right)
			drawitem(item, 0, y += bh, mw);
	} else if (matches) {
		/* draw horizontal list */
		x += inputw;
		w = TEXTW(symbol_1);
		if (curr->left) {
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_text(drw, x, 0, w, bh, lrpad / 2, symbol_1, 0
			);
		}
		x += w;
		for (item = curr; item != next; item = item->right) {
			stw = TEXTW(symbol_2);
			itw = textw_clamp(item->stext, mw - x - stw - rpad);
			x = drawitem(item, x, 0, itw);
		}
		if (next) {
			w = TEXTW(symbol_2);
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_text(drw, mw - w - rpad, 0, w, bh, lrpad / 2
				, symbol_2
				, 0
			);
		}
	}
	drw_setscheme(drw, scheme[SchemeNorm]);
	drw_text(drw, mw - rpad, 0, TEXTW(numbers), bh, lrpad / 2, numbers, 0);
	drw_map(drw, win, 0, 0, mw, mh);
}

static void
grabfocus(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000  };
	Window focuswin;
	int i, revertwin;

	for (i = 0; i < 100; ++i) {
		XGetInputFocus(dpy, &focuswin, &revertwin);
		if (focuswin == win)
			return;
		XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
		nanosleep(&ts, NULL);
	}
	die("cannot grab focus");
}

static void
grabkeyboard(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000  };
	int i;

	if (embed)
		return;
	/* try to grab keyboard, we may have to wait for another process to ungrab */
	for (i = 0; i < 1000; i++) {
		if (XGrabKeyboard(dpy, DefaultRootWindow(dpy), True, GrabModeAsync,
		                  GrabModeAsync, CurrentTime) == GrabSuccess) {
			return;
		}
		nanosleep(&ts, NULL);
	}
	die("cannot grab keyboard");
}

static void
match(void)
{
	if (dynamic && *dynamic)
		refreshoptions();

	if (fuzzy) {
		fuzzymatch();
		return;
	}
	static char **tokv = NULL;
	static int tokn = 0;

	char buf[sizeof text], *s;
	int i, tokc = 0;
	size_t len, textsize;
	struct item *item, *lprefix, *lsubstr, *prefixend, *substrend;

	strcpy(buf, text);
	/* separate input text into tokens to be matched individually */
	for (s = strtok(buf, " "); s; tokv[tokc - 1] = s, s = strtok(NULL, " "))
		if (++tokc > tokn && !(tokv = realloc(tokv, ++tokn * sizeof *tokv)))
			die("cannot realloc %zu bytes:", tokn * sizeof *tokv);
	len = tokc ? strlen(tokv[0]) : 0;

	matches = lprefix = lsubstr = matchend = prefixend = substrend = NULL;
	textsize = strlen(text) + 1;
	for (item = items; item && item->text; item++)
	{
		for (i = 0; i < tokc; i++)
			if (!fstrstr(item->text, tokv[i]))
				break;
		if (i != tokc && !(dynamic && *dynamic)) /* not all tokens match */
			continue;
		/* exact matches go first, then prefixes, then substrings */
		if (!sortmatches)
 			appenditem(item, &matches, &matchend);
 		else
		if (!tokc || !fstrncmp(text, item->text, textsize))
			appenditem(item, &matches, &matchend);
		else if (!fstrncmp(tokv[0], item->text, len))
			appenditem(item, &lprefix, &prefixend);
		else
			appenditem(item, &lsubstr, &substrend);
	}
	if (lprefix) {
		if (matches) {
			matchend->right = lprefix;
			lprefix->left = matchend;
		} else
			matches = lprefix;
		matchend = prefixend;
	}
	if (lsubstr)
	{
		if (matches) {
			matchend->right = lsubstr;
			lsubstr->left = matchend;
		} else
			matches = lsubstr;
		matchend = substrend;
	}
	curr = sel = matches;

	if (instant && matches && matches==matchend && !lsubstr) {
		puts(matches->text);
		cleanup();
		exit(0);
	}

	calcoffsets();
}

static void
insert(const char *str, ssize_t n)
{
	if (strlen(text) + n > sizeof text - 1)
		return;

	static char last[BUFSIZ] = "";
	if (reject_no_match) {
		/* store last text value in case we need to revert it */
		memcpy(last, text, BUFSIZ);
	}

	/* move existing text out of the way, insert new text, and update cursor */
	memmove(&text[cursor + n], &text[cursor], sizeof text - cursor - MAX(n, 0));
	if (n > 0)
		memcpy(&text[cursor], str, n);
	cursor += n;
	match();

	if (!matches && reject_no_match) {
		/* revert to last text value if theres no match */
		memcpy(text, last, BUFSIZ);
		cursor -= n;
		match();
	}
}

static size_t
nextrune(int inc)
{
	ssize_t n;

	/* return location of next utf8 rune in the given direction (+1 or -1) */
	for (n = cursor + inc; n + inc >= 0 && (text[n] & 0xc0) == 0x80; n += inc)
		;
	return n;
}

static void
movewordedge(int dir)
{
	if (dir < 0) { /* move cursor to the start of the word*/
		while (cursor > 0 && strchr(worddelimiters, text[nextrune(-1)]))
			cursor = nextrune(-1);
		while (cursor > 0 && !strchr(worddelimiters, text[nextrune(-1)]))
			cursor = nextrune(-1);
	} else { /* move cursor to the end of the word */
		while (text[cursor] && strchr(worddelimiters, text[cursor]))
			cursor = nextrune(+1);
		while (text[cursor] && !strchr(worddelimiters, text[cursor]))
			cursor = nextrune(+1);
	}
}

static void
keypress(XKeyEvent *ev)
{
	char buf[64];
	int len;
	KeySym ksym = NoSymbol;
	Status status;

	len = XmbLookupString(xic, ev, buf, sizeof buf, &ksym, &status);
	switch (status) {
	default: /* XLookupNone, XBufferOverflow */
		return;
	case XLookupChars: /* composed string from input method */
		goto insert;
	case XLookupKeySym:
	case XLookupBoth: /* a KeySym and a string are returned: use keysym */
		break;
	}

	if (using_vi_mode) {
		vi_keypress(ksym, ev);
		return;
	}

	if (vi_mode &&
			   (ksym == global_esc.ksym &&
				(ev->state & global_esc.state) == global_esc.state)) {
		using_vi_mode = 1;
		if (cursor)
			cursor = nextrune(-1);
		goto draw;
	}

	if (ev->state & ControlMask) {
		switch(ksym) {
		case XK_a: ksym = XK_Home;      break;
		case XK_b: ksym = XK_Left;      break;
		case XK_c: ksym = XK_Escape;    break;
		case XK_d: ksym = XK_Delete;    break;
		case XK_e: ksym = XK_End;       break;
		case XK_f: ksym = XK_Right;     break;
		case XK_g: ksym = XK_Escape;    break;
		case XK_h: ksym = XK_BackSpace; break;
		case XK_i: ksym = XK_Tab;       break;
		case XK_j: /* fallthrough */
		case XK_J: /* fallthrough */
		case XK_m: /* fallthrough */
		case XK_M: ksym = XK_Return; ev->state &= ~ControlMask; break;
		case XK_n: ksym = XK_Down;      break;
		case XK_p: ksym = XK_Up;        break;

		case XK_k: /* delete right */
			text[cursor] = '\0';
			match();
			break;
		case XK_u: /* delete left */
			insert(NULL, 0 - cursor);
			break;
		case XK_w: /* delete word */
			while (cursor > 0 && strchr(worddelimiters, text[nextrune(-1)]))
				insert(NULL, nextrune(-1) - cursor);
			while (cursor > 0 && !strchr(worddelimiters, text[nextrune(-1)]))
				insert(NULL, nextrune(-1) - cursor);
			break;
		case XK_v:
		case XK_V:
			XConvertSelection(dpy, (ev->state & ShiftMask) ? clip : XA_PRIMARY,
			                  utf8, utf8, win, CurrentTime);
			return;
		case XK_y: /* paste selection */
		case XK_Y:
			XConvertSelection(dpy, (ev->state & ShiftMask) ? clip : XA_PRIMARY,
			                  utf8, utf8, win, CurrentTime);
			return;
		case XK_Left:
		case XK_KP_Left:
			movewordedge(-1);
			goto draw;
		case XK_Right:
		case XK_KP_Right:
			movewordedge(+1);
			goto draw;
		case XK_Return:
		case XK_KP_Enter:
			if (restrict_return)
				break;
			break;
		case XK_bracketleft:
			cleanup();
			exit(1);
		default:
			return;
		}
	} else if (ev->state & Mod1Mask) {
		switch(ksym) {
		case XK_b:
			movewordedge(-1);
			goto draw;
		case XK_f:
			movewordedge(+1);
			goto draw;
		case XK_g: ksym = XK_Home;  break;
		case XK_G: ksym = XK_End;   break;
		case XK_h: ksym = XK_Up;    break;
		case XK_j: ksym = XK_Next;  break;
		case XK_k: ksym = XK_Prior; break;
		case XK_l: ksym = XK_Down;  break;
		default:
			return;
		}
	}

	switch(ksym) {
	default:
insert:
		if (!iscntrl((unsigned char)*buf))
			insert(buf, len);
		break;
	case XK_Delete:
	case XK_KP_Delete:
		if (text[cursor] == '\0')
			return;
		cursor = nextrune(+1);
		/* fallthrough */
	case XK_BackSpace:
		if (cursor == 0)
			return;
		insert(NULL, nextrune(-1) - cursor);
		break;
	case XK_End:
	case XK_KP_End:
		if (text[cursor] != '\0') {
			cursor = strlen(text);
			break;
		}
		if (next) {
			/* jump to end of list and position items in reverse */
			curr = matchend;
			calcoffsets();
			curr = prev;
			calcoffsets();
			while (next && (curr = curr->right))
				calcoffsets();
		}
		sel = matchend;
		break;
	case XK_Escape:
		cleanup();
		exit(1);
	case XK_Home:
	case XK_KP_Home:
		if (sel == matches) {
			cursor = 0;
			break;
		}
		sel = curr = matches;
		calcoffsets();
		break;
	case XK_Left:
	case XK_KP_Left:
		if (cursor > 0 && (!sel || !sel->left || lines > 0)) {
			cursor = nextrune(-1);
			break;
		}
		if (lines > 0)
			return;
		/* fallthrough */
	case XK_Up:
	case XK_KP_Up:
		if (sel && sel->left && (sel = sel->left)->right == curr) {
			curr = prev;
			calcoffsets();
		}
		break;
	case XK_Next:
	case XK_KP_Next:
		if (!next)
			return;
		sel = curr = next;
		calcoffsets();
		break;
	case XK_Prior:
	case XK_KP_Prior:
		if (!prev)
			return;
		sel = curr = prev;
		calcoffsets();
		break;
	case XK_Return:
	case XK_KP_Enter:
		if (restrict_return && (!sel || ev->state & (ShiftMask | ControlMask)))
			break;
		if (sel && !(ev->state & ShiftMask)){
			cleanup();
			exit(0);
		}
		if (!(ev->state & ControlMask)) {
			if (sel->text[0] == startpipe[0]) {
				strncpy(sel->text + strlen(sel->text),pipeout,8);
				puts(sel->text+1);
			}
			puts(sel->text);
		} else {
			if (text[0] == startpipe[0]) {
				strncpy(text + strlen(text),pipeout,8);
				puts(text+1);
			}
			puts(text);
		}
		if (sel)
			sel->out = 1;
		break;
	case XK_Right:
	case XK_KP_Right:
		if (text[cursor] != '\0') {
			cursor = nextrune(+1);
			break;
		}
		if (lines > 0)
			return;
		/* fallthrough */
	case XK_Down:
	case XK_KP_Down:
		if (sel && sel->right && (sel = sel->right) == next) {
			curr = next;
			calcoffsets();
		}
		break;
	case XK_Tab:
		if (!sel)
			return;
		cursor = strnlen(sel->text, sizeof text - 1);
		memcpy(text, sel->text, cursor);
		text[cursor] = '\0';
		match();
		break;
	}

draw:
	if (incremental) {
		puts(text);
		fflush(stdout);
	}
	if (using_vi_mode && text[cursor] == '\0')
		--cursor;

	drawmenu();
}

static void
paste(void)
{
	char *p, *q;
	int di;
	unsigned long dl;
	Atom da;

	/* we have been given the current selection, now insert it into input */
	if (XGetWindowProperty(dpy, win, utf8, 0, (sizeof text / 4) + 1, False,
	                   utf8, &da, &di, &dl, &dl, (unsigned char **)&p)
	    == Success && p) {
		insert(p, (q = strchr(p, '\n')) ? q - p : (ssize_t)strlen(p));
		XFree(p);
	}
	drawmenu();
}

static void
readstdin(void)
{
	char *line = NULL;
	char *buf, *p;

	size_t i, linesiz, itemsiz = 0;
	ssize_t len;

	if (passwd) {
		inputw = lines = 0;
		return;
	}

	/* read each line from stdin and add it to the item list */
	for (i = 0; (len = getline(&line, &linesiz, stdin)) != -1; i++) {
		if (i + 1 >= itemsiz) {
			itemsiz += 256;
			if (!(items = realloc(items, itemsiz * sizeof(*items))))
				die("cannot realloc %zu bytes:", itemsiz * sizeof(*items));
		}
		if (line[len - 1] == '\n')
			line[len - 1] = '\0';

		if (!(items[i].text = strdup(line)))
			die("strdup:");
		if (!(buf = strdup(line)))
			die("cannot strdup %u bytes:", strlen(line) + 1);
		if ((p = strchr(buf, '\t')))
			*p = '\0';
		items[i].stext = buf;
		items[i].out = 0;

	}
	free(line);
	if (items)
		items[i].text = NULL;
	lines = MIN(lines, i);
}

static void
run(void)
{
	XEvent ev;
	int i;
	while (!XNextEvent(dpy, &ev)) {
		if (preselected) {
			for (i = 0; i < preselected; i++) {
				if (sel && sel->right && (sel = sel->right) == next) {
					curr = next;
					calcoffsets();
				}
			}
			drawmenu();
			preselected = 0;
		}
		if (XFilterEvent(&ev, win))
			continue;
		switch(ev.type) {
		case DestroyNotify:
			if (ev.xdestroywindow.window != win)
				break;
			cleanup();
			exit(1);
		case Expose:
			if (ev.xexpose.count == 0)
				drw_map(drw, win, 0, 0, mw, mh);
			break;
		case FocusIn:
			/* regrab focus from parent window */
			if (ev.xfocus.window != win)
				grabfocus();
			break;
		case KeyPress:
			keypress(&ev.xkey);
			break;
		case SelectionNotify:
			if (ev.xselection.property == utf8)
				paste();
			break;
		case VisibilityNotify:
			if (ev.xvisibility.state != VisibilityUnobscured)
				XRaiseWindow(dpy, win);
			break;
		}
	}
}

static void
setup(void)
{
	int x, y, i, j;
	unsigned int du;
	XSetWindowAttributes swa;
	XIM xim;
	Window w, dw, *dws;
	XWindowAttributes wa;
	XClassHint ch = {"dmenu", "dmenu"};
#ifdef XINERAMA
	XineramaScreenInfo *info;
	Window pw;
	int a, di, n, area = 0;
#endif
	/* init appearance */
	for (j = 0; j < SchemeLast; j++)
		scheme[j] = drw_scm_create(drw, (const char**)colors[j], 2);

	clip = XInternAtom(dpy, "CLIPBOARD",   False);
	utf8 = XInternAtom(dpy, "UTF8_STRING", False);

	/* calculate menu geometry */
	bh = drw->fonts->h + 2;
	bh = MAX(bh,lineheight);	/* make a menu line AT LEAST 'lineheight' tall */
	lines = MAX(lines, 0);
	mh = (lines + 1) * bh;
	promptw = (prompt && *prompt) ? TEXTW(prompt) - lrpad / 4 : 0;
#ifdef XINERAMA
	i = 0;
	if (parentwin == root && (info = XineramaQueryScreens(dpy, &n))) {
		XGetInputFocus(dpy, &w, &di);
		if (mon >= 0 && mon < n)
			i = mon;
		else if (w != root && w != PointerRoot && w != None) {
			/* find top-level window containing current input focus */
			do {
				if (XQueryTree(dpy, (pw = w), &dw, &w, &dws, &du) && dws)
					XFree(dws);
			} while (w != root && w != pw);
			/* find xinerama screen with which the window intersects most */
			if (XGetWindowAttributes(dpy, pw, &wa))
				for (j = 0; j < n; j++)
					if ((a = INTERSECT(wa.x, wa.y, wa.width, wa.height, info[j])) > area) {
						area = a;
						i = j;
					}
		}
		/* no focused window is on screen, so use pointer location instead */
		if (mon < 0 && !area && XQueryPointer(dpy, root, &dw, &dw, &x, &y, &di, &di, &du))
			for (i = 0; i < n; i++)
				if (INTERSECT(x, y, 1, 1, info[i]) != 0)
					break;

		if (center) {
			mw = MIN(MAX(max_textw() + promptw, min_width), info[i].width);
			x = info[i].x_org + ((info[i].width  - mw) / 2);
			y = info[i].y_org + ((info[i].height - mh) / 2);
		} else {
			x = info[i].x_org;
			y = info[i].y_org + (topbar ? 0 : info[i].height - mh);
			mw = info[i].width;
		}
		XFree(info);
	} else
#endif
	{
		if (!XGetWindowAttributes(dpy, parentwin, &wa))
			die("could not get embedding window attributes: 0x%lx",
			    parentwin);
		if (center) {
			mw = MIN(MAX(max_textw() + promptw, min_width), wa.width);
			x = (wa.width  - mw) / 2;
			y = (wa.height - mh) / 2;
		} else {
			x = 0;
			y = topbar ? 0 : wa.height - mh;
			mw = wa.width;
		}
	}
	inputw = mw / 3; /* input width: ~33.33% of monitor width */
	match();

	/* create menu window */
	swa.override_redirect = True;
	swa.background_pixel = scheme[SchemeNorm][ColBg].pixel;
	swa.event_mask = ExposureMask | KeyPressMask | VisibilityChangeMask
	;
	win = XCreateWindow(
		dpy, root,
		x, y - (topbar ? 0 : border_width * 2), mw - border_width * 2, mh, border_width,
		CopyFromParent, CopyFromParent, CopyFromParent,
		CWOverrideRedirect | CWBackPixel | CWEventMask, &swa
	);
	if (border_width)
		XSetWindowBorder(dpy, win, scheme[SchemeBorder][ColBg].pixel);
	XSetClassHint(dpy, win, &ch);

	/* input methods */
	if ((xim = XOpenIM(dpy, NULL, NULL, NULL)) == NULL)
		die("XOpenIM failed: could not open input device");

	xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
	                XNClientWindow, win, XNFocusWindow, win, NULL);

	XMapRaised(dpy, win);
	if (embed) {
		XReparentWindow(dpy, win, parentwin, x, y);
		XSelectInput(dpy, parentwin, FocusChangeMask | SubstructureNotifyMask);
		if (XQueryTree(dpy, parentwin, &dw, &w, &dws, &du) && dws) {
			for (i = 0; i < du && dws[i] != win; ++i)
				XSelectInput(dpy, dws[i], FocusChangeMask);
			XFree(dws);
		}
		grabfocus();
	}
	drw_resize(drw, mw, mh);
	drawmenu();
}

static void
usage(void)
{
	die("usage: dmenu [-bv"
		"c"
		"f"
		"r"
		"s"
		"n"
		"F"
		"P"
		"S"
		"R" // (changed from r to R due to conflict with INCREMENTAL_PATCH)
		"1"
		"] "
		"[-vi] "
		"[-l lines] [-p prompt] [-fn font] [-m monitor]"
		"\n             [-nb color] [-nf color] [-sb color] [-sf color] [-w windowid]"
		"\n            "
		" [-dy command]"
		" [-bw width]"
		"\n            "
		" [-it text]"
		" [-h height]"
		" [-ps index]"
		"\n             [-nhb color] [-nhf color] [-shb color] [-shf color]" // highlight colors
		"\n");
}

int
main(int argc, char *argv[])
{
	XWindowAttributes wa;
	int i;
	int fast = 0;

	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fputs("warning: no locale support\n", stderr);
	if (!(dpy = XOpenDisplay(NULL)))
		die("cannot open display");

	/* These need to be checked before we init the visuals and read X resources. */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) { /* prints version information */
			puts("dmenu-"VERSION);
			exit(0);
		} else if (!strcmp(argv[i], "-w")) {
			argv[i][0] = '\0';
			embed = strdup(argv[++i]);
		} else {
			continue;
		}
		argv[i][0] = '\0'; // mark as used
	}

	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	if (!embed || !(parentwin = strtol(embed, NULL, 0)))
		parentwin = root;
	if (!XGetWindowAttributes(dpy, parentwin, &wa))
		die("could not get embedding window attributes: 0x%lx",
		    parentwin);

	drw = drw_create(dpy, screen, root, wa.width, wa.height);
	readxresources();

	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '\0')
			continue;

		/* these options take no arguments */
		if (!strcmp(argv[i], "-v")) {      /* prints version information */
			puts("dmenu-"VERSION);
			exit(0);
		} else if (!strcmp(argv[i], "-b")) { /* appears at the bottom of the screen */
			topbar = 0;
		} else if (!strcmp(argv[i], "-c")) { /* toggles centering of dmenu window on screen */
			center = !center;
		} else if (!strcmp(argv[i], "-f")) { /* grabs keyboard before reading stdin */
			fast = 1;
		} else if (!strcmp(argv[i], "-r")) { /* incremental */
			incremental = !incremental;
		} else if (!strcmp(argv[i], "-s")) { /* case-sensitive item matching */
			fstrncmp = strncmp;
			fstrstr = strstr;
		} else if (!strcmp(argv[i], "-vi")) {
			vi_mode = 1;
			using_vi_mode = start_mode;
			global_esc.ksym = XK_Escape;
			global_esc.state = 0;
		} else if (!strcmp(argv[i], "-n")) { /* instant select only match */
			instant = !instant;
		} else if (!strcmp(argv[i], "-F")) { /* disable/enable fuzzy matching, depends on default */
			fuzzy = !fuzzy;
		} else if (!strcmp(argv[i], "-P")) { /* is the input a password */
			passwd = 1;
		} else if (!strcmp(argv[i], "-R")) { /* reject input which results in no match */
			reject_no_match = 1;
		} else if (!strcmp(argv[i], "-S")) { /* do not sort matches */
			sortmatches = 0;
		} else if (!strcmp(argv[i], "-1")) {
			restrict_return = 1;
		} else if (i + 1 == argc)
			usage();
		/* these options take one argument */
		else if (!strcmp(argv[i], "-l"))   /* number of lines in vertical list */
			lines = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-m"))
			mon = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-p"))   /* adds prompt to left of input field */
			prompt = argv[++i];
		else if (!strcmp(argv[i], "-fn"))  /* font or font set */
			fonts[0] = argv[++i];
		else if(!strcmp(argv[i], "-h")) { /* minimum height of one menu line */
			lineheight = atoi(argv[++i]);
			lineheight = MAX(lineheight, min_lineheight); /* reasonable default in case of value too small/negative */
		}
		else if (!strcmp(argv[i], "-nb"))  /* normal background color */
			colors[SchemeNorm][ColBg] = argv[++i];
		else if (!strcmp(argv[i], "-nf"))  /* normal foreground color */
			colors[SchemeNorm][ColFg] = argv[++i];
		else if (!strcmp(argv[i], "-sb"))  /* selected background color */
			colors[SchemeSel][ColBg] = argv[++i];
		else if (!strcmp(argv[i], "-sf"))  /* selected foreground color */
			colors[SchemeSel][ColFg] = argv[++i];
		else if (!strcmp(argv[i], "-nhb")) /* normal hi background color */
			colors[SchemeNormHighlight][ColBg] = argv[++i];
		else if (!strcmp(argv[i], "-nhf")) /* normal hi foreground color */
			colors[SchemeNormHighlight][ColFg] = argv[++i];
		else if (!strcmp(argv[i], "-shb")) /* selected hi background color */
			colors[SchemeSelHighlight][ColBg] = argv[++i];
		else if (!strcmp(argv[i], "-shf")) /* selected hi foreground color */
			colors[SchemeSelHighlight][ColFg] = argv[++i];
		else if (!strcmp(argv[i], "-ps"))   /* preselected item */
			preselected = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-dy"))  /* dynamic command to run */
			dynamic = argv[++i];
		else if (!strcmp(argv[i], "-bw"))  /* border width around dmenu */
			border_width = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-it")) {   /* adds initial text */
			const char * text = argv[++i];
			insert(text, strlen(text));
		}
		else {
			usage();
		}
	}

	if (!drw_fontset_create(drw, (const char**)fonts, LENGTH(fonts)))
		die("no fonts could be loaded.");

	lrpad = drw->fonts->h;

	if (lineheight == -1)
		lineheight = drw->fonts->h * 2.5;

#ifdef __OpenBSD__
	if (pledge("stdio rpath", NULL) == -1)
		die("pledge");
#endif

	if (fast && !isatty(0)) {
		grabkeyboard();
		if (!(dynamic && *dynamic))
			readstdin();
	} else {
		if (!(dynamic && *dynamic))
			readstdin();
		grabkeyboard();
	}
	setup();
	run();

	return 1; /* unreachable */
}
