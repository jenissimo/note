/* note_win32.h -- state and types shared across the Win32 backend.
 *
 * The backend outgrew a single translation unit, so it is now several, and
 * they all work on one note_host.  That struct would rather have stayed
 * private to one file, but at this size the privacy was already nominal: the
 * gutter, the tab strip, the palette and the message loop all read and write
 * the same window handles, brushes and cached text.  Making the sharing
 * explicit in a header is honest about what was already true.
 *
 * Only declarations belong here.  Anything with a body lives in one of the
 * win32_*.c files.
 */
#ifndef NOTE_WIN32_H
#define NOTE_WIN32_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <richedit.h>
#include <richole.h>
#include <tom.h>
#include <commdlg.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <dwmapi.h>

#include "../../core/note_core.h"
#include "../../core/note_syntax.h"
#include "../../core/note_theme.h"
#include "../../core/note_palette.h"

/* Themed common controls, so the tab strip looks like the rest of Windows. */
#pragma comment(linker, "/manifestdependency:\"type='win32' "                 \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "             \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "            \
    "language='*'\"")

/* -------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

#define ID_TABS     1000
#define ID_STATUS   1001
#define ID_EDIT0    1100          /* +doc index */

/* The lists the command palette can show.  PAL_MODE_CMDS is the palette
 * proper; the rest are the lists that used to each be a dialog of their own.
 * See kPalModes, where each is a title, a row source and two callbacks. */
enum {
    PAL_MODE_CMDS = 0,
    PAL_MODE_THEME,
    PAL_MODE_FONT,
    PAL_MODE_LINE,
    PAL_MODE_TABS,
    PAL_MODE_COUNT
};

/* Chrome sizes, written for 96 DPI and scaled through px(). */
#define STATUS_H  22
#define TABS_H    26

#define TIMER_VIEW     1          /* repaint gutter / recolour, debounced */
#define TIMER_SESSION  2          /* autosave unsaved buffers            */

#define MAX_SPANS   4096
/* How far back the lexer may look for a safe place to start.  Generous
 * enough that a block comment longer than this is the only thing that can be
 * mis-coloured, and small enough that the cost does not depend on the size of
 * the file. */
#define SAFE_START_WINDOW (32 * 1024)

typedef struct {
    HWND    edit;
    WNDPROC oldproc;
    WCHAR   title[96];
    int     x, w;          /* laid out by tabs_layout() */
} win_doc;

struct note_host {
    HINSTANCE inst;
    HWND      wnd, tabs, status, gutter, finddlg;
    HACCEL    accel;
    HMENU     menubar, ctxmenu;
    HANDLE    heap;

    win_doc   d[NOTE_MAX_DOCS];
    note_app  app;

    FINDREPLACEW fr;
    WCHAR        fr_find[NOTE_FIND_MAX];
    WCHAR        fr_repl[NOTE_FIND_MAX];
    UINT         findmsg;

    LOGFONTW  font;
    int       fontpt;         /* tenths of a point */
    HFONT     uifont;

    PAGESETUPDLGW page;

    note_theme theme;         /* resolved palette, copied out of the core */
    HBRUSH     br_gutter, br_ui;

    /* Non-zero while we are changing a control ourselves, so the EN_CHANGE it
     * sends back does not mark the document dirty. */
    int suppress;
    int quitting;
    int view_pending;         /* gutter/highlight refresh queued */
    int dpi;                  /* of the monitor the window is on */
    int tabs_h, status_h, gutter_w;
    int line_h, char_w;

    /* Cached text of the active document, shared by the gutter (paragraph
     * numbering) and the highlighter (tokenising).  Refreshed on demand. */
    nchar *cache;
    int    cache_len;
    int    cache_doc;
    int    cache_valid;

    /* Where every paragraph starts, built with the cache in one pass.
     *
     * Without it, "which line is this offset on" meant counting breaks from
     * the top of the document, and the gutter and the status bar each asked
     * that on every caret move — thirteen million comparisons per arrow key
     * in a thirteen-megabyte file, which is exactly how it felt.  With it the
     * same question is a binary search. */
    int   *lines;
    int    nlines;

    note_span spans[MAX_SPANS];

    /* Windows draws menus and tabs in its own light palette and offers no way
     * to recolour them, so note owns their pixels: every menu item and every
     * tab is owner-drawn from the active theme. */
    HFONT   menufont;
    HBRUSH  br_menu, br_menusel, br_sep, br_edit;

    /* Tab strip state.  The strip is a window of note's own: the common tab
     * control paints its own frame and shelf from the system theme, and an
     * owner-draw hook cannot reach those pixels, so a dark palette always
     * left pale seams around the tabs. */
    int     tab_hot;        /* index under the pointer, or -1 */
    int     tab_hot_close;  /* pointer is over that tab's close box */
    int     tab_tracking;   /* a TrackMouseEvent is armed */
    int     tab_scroll;     /* px of the strip scrolled off to the left */

    /* The one centre line everything in the strip is centred on: the icon, the
     * labels and the close crosses.  Kept here rather than recomputed at each
     * point of drawing, because three copies of the same arithmetic is how
     * they drifted apart in the first place.  Recomputed on WM_SIZE, since a
     * maximised caption is not the same height as a restored one. */
    int     tab_mid;

    /* The strip is drawn in the window caption, the way Notepad and the
     * browsers do it, so it costs no band of its own.  Zero when the frame
     * could not be taken over — see frame_custom() — and then the strip sits
     * below an ordinary caption as it always did. */
    int     title_tabs;

    /* Where DWM puts minimise/maximise/close, in client pixels.  Client and
     * not window pixels on purpose: maximised, the window rect hangs a resize
     * border off every edge of the monitor, so a rectangle measured from it
     * would sit partly off the screen and the buttons would be hard to hit. */
    RECT    sysbtn;

    /* The colour last handed to DWM for the caption, so the attributes are set
     * once per theme rather than once per paint.  cap_set distinguishes "never
     * set" from a theme whose ui_bg really is black. */
    unsigned cap_rgb;
    int      cap_set;

    /* The status bar is repainted on every caret move, so it is a window of
     * our own that paints straight over itself.  A STATIC would erase to the
     * background first and flicker on each keystroke. */
    WCHAR   status_text[160];

    /* The menu bar is a thing you summon, not a thing that sits there.  It is
     * built at startup but only attached to the frame while it is wanted. */
    int     menu_visible;
    int     alt_chord;      /* Alt has been used as a modifier this press */

    /* Which top-level item is lit, or -1.  Windows tells us on the way past —
     * see the UAH handler — and note has to remember, because it repaints the
     * bar itself: menubar_paint() says why. */
    int     menu_hot;

    /* Double-tap Shift, IntelliJ's gesture.  See shift_gesture(). */
    DWORD   shift_last;     /* tick of the last bare Shift release, or 0 */
    int     shift_chord;    /* a key other than Shift intervened         */

    /* The command palette is an overlay of note's own, painted like the tab
     * strip: no caption, no taskbar button, and WS_EX_NOACTIVATE so the frame
     * keeps its active look while the palette is up.  Nothing focuses it, so
     * the message loop hands it the keys. */
    HWND    pal;
    int     pal_open;
    int     pal_sel;             /* selected row, an index into the matches */
    int     pal_top;             /* first visible row                      */

    /* Which list the overlay is showing.  The command list is one mode among
     * several; the others stand in for the dialogs that existed only to show
     * a list.  See the mode table by pal_show().  Each remembers here what
     * its live preview has to put back if the user presses Esc. */
    int     pal_mode;
    int     pal_theme_prev;               /* note_app.theme_index, may be -1 */
    WCHAR   pal_face_prev[LF_FACESIZE];
    int     pal_caret_prev;

    /* What the core last said through ops->set_hint: the palette paints it on
     * the input line.  A mode whose answer can be refused — rename, when the
     * name would land on a file that already exists — has to stay open and
     * say why, and a message box would close it. */
    WCHAR   pal_msg[128];
};

/* The payloads Windows hands to the UAH menu messages.  Undocumented, but
 * stable since Windows 10 and the only way to paint the menu bar: since Win10
 * the bar is drawn through these rather than through WM_DRAWITEM, so an app
 * that owner-draws its items still gets them repainted in the system's light
 * palette unless it answers 0x0091 and 0x0092 as well. */
typedef struct {
    HMENU hmenu;
    HDC   hdc;
    DWORD dwFlags;
} UAHMENU;

typedef union {
    struct { DWORD cx; DWORD cy; } rgsizeBar[2];
    struct { DWORD cx; DWORD cy; } rgsizePopup[4];
} UAHMENUITEMMETRICS;

typedef struct {
    DWORD rgcx[4];
    DWORD fUpdateMaxWidths : 2;
} UAHMENUPOPUPMETRICS;

typedef struct {
    int                 iPosition;
    UAHMENUITEMMETRICS  umim;
    UAHMENUPOPUPMETRICS umpm;
} UAHMENUITEM;

typedef struct {
    DRAWITEMSTRUCT dis;
    UAHMENU        um;
    UAHMENUITEM    umi;
} UAHDRAWMENUITEM;

/* Owner-drawn menu items carry a pointer to one of these as their item data;
 * a NULL item data means a separator. */
typedef struct {
    const nchar *label;
    int          onbar;      /* a top-level item, which has no check column */
    int          checkcol;   /* its menu has something checkable in it      */
} menu_item_data;


/* ---- defined in win32_main.c -------------------------------------------- */

extern struct note_host g;
/* Menu item data, the ops table and the one note_host, all defined in the
 * module named beside them. */
extern menu_item_data g_mid[192];      /* win32_main.c  */
extern int            g_nmid;
extern const note_host_ops kOps;       /* win32_host.c  */


/* Scaling and colour conversion, used by everything that draws. */
COLORREF cr(unsigned rgb);
int      px(int v);
void     read_dpi(note_host *h);
HWND     active_edit(void);

/* Suspends RichEdit's undo around programmatic formatting; see win32_main.c. */
ITextDocument *tom_open(HWND edit);

void relayout(note_host *h);
void update_status(note_host *h);
void measure_font(note_host *h);
void on_find_msg(note_host *h, FINDREPLACEW *fr);

/* ---- published by the split ---- */
int para_at(note_host *h, int off);
void pal_show(note_host *h);
void frame_custom(note_host *h);
void tabs_layout(note_host *h);
LRESULT CALLBACK EditProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK GutterProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK PaletteProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK StatusProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK TabsProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
void apply_font_to(HWND e, note_host *h);
COLORREF blend_rgb(unsigned a, unsigned b);
COLORREF mix_rgb(unsigned a, unsigned b, int num, int den);
void build_accels(note_host *h);
void build_menus(note_host *h);
void caption_buttons_calc(note_host *h);
int caption_buttons_w(note_host *h);
void caption_colours(note_host *h);
int caption_height(void);
int caption_plain(note_host *h);
int  menubar_band(note_host *h, RECT *band);
void menubar_item_draw(note_host *h, HDC dc, const RECT *rc,
                       const menu_item_data *d, int hot);
int  menubar_paint(note_host *h, HDC dc);
void draw_menu_item(note_host *h, DRAWITEMSTRUCT *dis);
int edit_len(HWND e);
int frame_edge(void);
void gutter_width(note_host *h);
void *h_alloc(note_host *h, unsigned long bytes);
int h_ask_save(note_host *h, const nchar *name);
int h_can_undo(note_host *h);
void h_dlg_find(note_host *h, int replace);
void h_dlg_font(note_host *h);
int h_dlg_open(note_host *h, nchar *path, int cap);
void h_dlg_pagesetup(note_host *h);
void h_dlg_print(note_host *h);
int h_dlg_save(note_host *h, nchar *path, int cap);
void h_edit_op(note_host *h, int cmd);
int h_find_text(note_host *h, const nchar *needle, unsigned flags);
void h_free(note_host *h, void *p);
void h_goto_line(note_host *h, int line);
void h_message(note_host *h, const nchar *text, const nchar *title);
void h_rehighlight(note_host *h);
void h_sel_get(note_host *h, int *from, int *to);
void h_sel_replace(note_host *h, const nchar *s);
void h_sel_set(note_host *h, int from, int to);
void h_set_linenums(note_host *h, int on);
void h_set_modified(note_host *h, int doc, int modified);
void h_set_status(note_host *h, const nchar *s);
void h_set_theme(note_host *h, int theme);
void h_set_title(note_host *h, const nchar *s);
void h_set_wrap(note_host *h, int wrap);
void h_set_zoom(note_host *h, int percent);
void h_show_status(note_host *h, int visible);
int h_system_dark(note_host *h);
int h_tab_create(note_host *h, int doc);
void h_tab_destroy(note_host *h, int doc);
void h_tab_select(note_host *h, int doc);
void h_tab_title(note_host *h, int doc, const nchar *title);
int h_text_get(note_host *h, int doc, nchar *buf, int cap);
int h_text_len(note_host *h, int doc);
void h_text_set(note_host *h, int doc, const nchar *s);
void measure_menu_item(note_host *h, MEASUREITEMSTRUCT *mis);
void menu_set_brush(HMENU m, HBRUSH br);
void menubar_show(note_host *h, int show);
menu_item_data *mid_new(const nchar *label, int onbar, int checkcol);
void pal_char(note_host *h, unsigned ch);
void pal_close(note_host *h);
int pal_key(note_host *h, int vk);
void pal_layout(note_host *h);
void pal_open_mode(note_host *h, int mode);
int para_number(const nchar *t, int len, int upto);
void queue_view(note_host *h);
void refresh_cache(note_host *h);
void service_view(note_host *h);
void set_app_dark(int dark);
void sync_menu(note_host *h);
int tabs_hit(note_host *h, int x, int y, int *on_close);
int tabs_left(note_host *h);

#endif /* NOTE_WIN32_H */
