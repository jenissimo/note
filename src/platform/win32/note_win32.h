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

/* -------------------------------------------------------------------------
 * Which text surface this build uses.
 *
 * 0 is the RICHEDIT control the editor shipped with.  1 is win32_view.c: a
 * window of note's own over the core's gap buffer, the same one the MS-DOS,
 * Commodore 64 and Game Boy ports edit.  The two cannot coexist in one
 * executable -- they define the same host ops -- so this picks one, and
 * build.bat passes it.
 *
 * The switch exists so the shipped editor keeps building while the view
 * matures, and so the two can be compared side by side.
 * ------------------------------------------------------------------------- */
#ifndef NOTE_OWN_VIEW
#define NOTE_OWN_VIEW 0
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#if !NOTE_OWN_VIEW
#include <richedit.h>
#include <richole.h>
#include <tom.h>
#endif
#include <commdlg.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <dwmapi.h>

#include "../../core/note_core.h"
#include "../../core/note_syntax.h"
#include "../../core/note_theme.h"
#include "../../core/note_reduce.h"
#include "../../core/note_palette.h"
#if NOTE_OWN_VIEW
#include "../../core/note_buffer.h"
#endif

/* Themed common controls, so the tab strip looks like the rest of Windows. */
#pragma comment(linker, "/manifestdependency:\"type='win32' "                 \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "             \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "            \
    "language='*'\"")

/* -------------------------------------------------------------------------
 * Capabilities
 *
 * Everything the desktop compositor and the theme engine offer arrived after
 * Windows 95 and, in the case of the frame attributes, after Windows 7.  A
 * static import of dwmapi or uxtheme would settle the question before note
 * runs a single instruction: the loader resolves imports first, so an older
 * Windows refuses to start the process and no runtime check inside it ever
 * gets a turn.  So the entry points are looked up by name instead.
 *
 * The test is always "is this entry point here", never "which Windows is
 * this".  GetVersionEx has lied since 8.1 to anything without a matching
 * manifest, and the version was only ever a proxy for the real question.
 *
 * A null pointer here is not an error.  Without DWM note wears the system's
 * own title bar; without uxtheme the menus stay light.  Both are the plainer
 * editor, not a broken one.
 * ------------------------------------------------------------------------- */
typedef struct {
    HRESULT (WINAPI *dwm_set_attr)   (HWND, DWORD, LPCVOID, DWORD);
    HRESULT (WINAPI *dwm_get_attr)   (HWND, DWORD, PVOID, DWORD);
    HRESULT (WINAPI *dwm_composition)(BOOL *);
    HRESULT (WINAPI *dwm_extend)     (HWND, const MARGINS *);
    BOOL    (WINAPI *dwm_defproc)    (HWND, UINT, WPARAM, LPARAM, LRESULT *);
    HRESULT (WINAPI *set_window_theme)(HWND, LPCWSTR, LPCWSTR);

    /* Undocumented but long-standing uxtheme ordinals 135 and 136: they let
     * menus, scrollbars and the non-client area follow a dark palette. */
    int     (WINAPI *set_app_mode)   (int);
    void    (WINAPI *flush_themes)   (void);

    /* The four calls the custom caption needs, all present.  frame_custom()
     * would otherwise have to check each one before it could commit. */
    int dwm_frame;

    /* Three that arrived after Windows 95 and carry no text, so the ANSI
     * boundary never had reason to touch them.  A static import of any one is
     * still enough for the loader to refuse the process on 95, before a line
     * of ours runs -- so they are looked up too, and each call site does
     * without when the answer is null.  TrackMouseEvent is 98 and NT 4,
     * SetMenuInfo 98 and 2000, GetMenuBarInfo 2000. */
    BOOL (WINAPI *track_mouse)   (TRACKMOUSEEVENT *);
    BOOL (WINAPI *set_menu_info) (HMENU, LPCMENUINFO);
    BOOL (WINAPI *menu_bar_info) (HWND, LONG, LONG, PMENUBARINFO);
} win_caps;

extern win_caps caps;

void caps_probe(void);

/* -------------------------------------------------------------------------
 * The Unicode boundary
 *
 * note is Unicode all the way down: nchar is a UTF-16 unit, every literal is
 * a wide literal, and the core has no other character type.  Windows 95 has
 * almost no other character type either -- its W entry points are stubs that
 * set ERROR_CALL_NOT_IMPLEMENTED and fail -- so somewhere the two have to
 * meet, and the only place they can is the call itself.
 *
 * nchar could not be the thing that moves.  It is a type: it is settled when
 * the compiler runs, and it decides every literal, every buffer and every
 * signature above it.  A call can move.  So each Windows call that carries
 * text goes through one of the wrappers below, and each wrapper either hands
 * the wide text to the W function unchanged, or converts it to the active
 * ANSI codepage and calls the A function, converting whatever comes back the
 * other way.
 *
 * Which of the two happens is settled once, at startup, by asking a W entry
 * point whether it is real -- see wide_probe().  It is never settled by
 * asking which Windows this is: GetVersionEx has lied since 8.1 to anything
 * without a matching manifest, and the version was only ever standing in for
 * the question these wrappers actually need answered.
 *
 * On any Windows whose W entry points work -- which is every Windows anyone
 * is running -- win_wide stays 1 and each wrapper is the same call, with the
 * same arguments, that stood at the call site before.  The conversion is a
 * road only Windows 95 ever drives down.
 *
 * A few W calls are deliberately not wrapped.  Windows 95 implements sixteen
 * of them for real rather than as stubs, and the Platform SDK names all
 * sixteen in one place -- "Other Existing Unicode Support", the page that says
 * which functions the Microsoft Layer for Unicode does not have to stand in
 * for.  Four of them matter here: TextOut, ExtTextOut, GetTextExtentPoint32
 * and GetCharWidth.  So a run of text is drawn and measured in UTF-16 even on
 * Windows 95, and note's rendering never goes through the codepage at all.
 *
 * The neighbours of those four are not on the list and are not exceptions.
 * DrawText is user32, where the only native W entry points are MessageBox and
 * MessageBoxEx; GetTextExtentExPoint is a stub although GetTextExtentPoint32
 * beside it is not; and the font is created and measured through
 * CreateFontIndirectA and GetTextMetricsA even when what is drawn with it is
 * wide.  Mixing an A font with W drawing is not a compromise -- it is the
 * shape the native set was left in.
 *
 * What degrades to the codepage, then, is the text Windows keeps rather than
 * draws: file names, window titles, menu labels, a command handed to the
 * shell.  Those are the calls with a wrapper.
 * ------------------------------------------------------------------------- */

extern int win_wide;
void wide_probe(void);

/* An ANSI codepage spends at most two bytes on a UTF-16 unit, and every
 * conversion buffer here is sized from a count of those. */
#define ACP_MAX (NOTE_PATH_MAX * 2 + 2)

/* Wide to the codepage, in two kinds, because losing a character means two
 * different things.  A label or a title that will not fit should still be
 * readable with a visible mark where the character was; a path must not be,
 * because a path with a '?' in it names some other file or no file at all,
 * and a save that quietly writes to the wrong name is worse than one that
 * refuses.  acp_text substitutes, acp_path returns null. */
char  *acp_text(const nchar *s, char *dst, int cap);
char  *acp_path(const nchar *s, char *dst, int cap);
nchar *wide_of(const char *s, nchar *dst, int cap);

/* Directory walking, as the two callers actually use it: a name and its
 * attributes.  WIN32_FIND_DATA is a different shape in each half of the API
 * and note reads two fields of it, so the wrapper hands those over rather
 * than pretending to convert the rest. */
typedef struct {
    HANDLE handle;
    DWORD  attrs;
    nchar  name[NOTE_PATH_MAX];
} os_find;

/* The face names one enumeration pass found.  Returning zero stops the walk,
 * the way the GDI callback it stands in for does. */
typedef int (*os_font_fn)(const nchar *face, unsigned pitch_family);

/* kernel32 */
HMODULE  os_module(const nchar *name);
HMODULE  os_library(const nchar *name);
void     os_command_line(nchar *dst, int cap);
DWORD    os_module_file_name(HMODULE mod, nchar *dst, int cap);
DWORD    os_env(const nchar *name, nchar *dst, int cap);
DWORD    os_current_dir(nchar *dst, int cap);
void     os_full_path(const nchar *path, nchar *dst, int cap);
HANDLE   os_create_file(const nchar *path, DWORD access, DWORD share,
                        DWORD disp);
int      os_delete_file(const nchar *path);
DWORD    os_file_attrs(const nchar *path);
int      os_move_file(const nchar *from, const nchar *to);
int      os_create_dir(const nchar *path);
int      os_find_open(const nchar *pattern, os_find *f);
int      os_find_step(os_find *f);
void     os_find_close(os_find *f);
int      os_time_date(const SYSTEMTIME *st, nchar *dst, int cap);
HRSRC    os_find_resource(HINSTANCE inst, int id, int type);
int      os_run(nchar *line, const nchar *dir);
int      os_reg_dword(HKEY root, const nchar *sub, const nchar *value,
                      DWORD *out);

/* user32 */
ATOM     os_register_class(WNDCLASSEXW *wc);
HWND     os_create_window(DWORD ex, const nchar *cls, const nchar *title,
                          DWORD style, int x, int y, int w, int h,
                          HWND parent, HMENU menu, HINSTANCE inst, void *param);
LRESULT  os_defproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT  os_send(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
BOOL     os_post(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
BOOL     os_get_message(MSG *msg);
LRESULT  os_dispatch(MSG *msg);
BOOL     os_is_dialog_message(HWND dlg, MSG *msg);
int      os_translate_accel(HWND wnd, HACCEL accel, MSG *msg);
HACCEL   os_accel_table(ACCEL *a, int n);
WNDPROC  os_set_wndproc(HWND wnd, WNDPROC proc);
/* A subclass calls the procedure it displaced through this, never through
 * CallWindowProc directly: the two have to be the same half of the API that
 * os_set_wndproc installed. */
LRESULT  os_call_wndproc(WNDPROC proc, HWND wnd, UINT msg, WPARAM wp,
                         LPARAM lp);
void     os_set_window_text(HWND wnd, const nchar *s);
int      os_message_box(HWND owner, const nchar *text, const nchar *title,
                        UINT flags);
UINT     os_register_message(const nchar *name);
HWND     os_find_window(const nchar *cls);
LRESULT  os_send_timeout(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT ms);
HICON    os_icon(HINSTANCE inst, int id);
/* The stock cursors, by the number behind the IDC_ macro: the macro casts to
 * LPCWSTR and half of os_cursor wants LPCSTR. */
#define OS_CURSOR_ARROW 32512
#define OS_CURSOR_IBEAM 32513
HCURSOR  os_cursor(int id);
HCURSOR  os_arrow_cursor(void);
UINT     os_wheel_lines(void);
HICON    os_class_icon(HWND wnd, int which);
BOOL     os_append_menu(HMENU m, UINT flags, UINT_PTR id, const void *data);
/* Every menu item note owns is owner-drawn, so the only things ever read back
 * out of one are the pointer behind it and, for the bar, whether menu mode has
 * walked to it.  `state` may be null. */
ULONG_PTR os_menu_item_data(HMENU m, UINT pos, UINT *state);
int      os_draw_text(HDC dc, const nchar *s, int len, RECT *rc, UINT fmt);
/* A WM_CHAR's wParam, which is a codepage byte on an ANSI window and a UTF-16
 * unit on a wide one. */
unsigned os_wm_char(unsigned wp);
HFONT    os_menu_font(void);

/* gdi32 */
HFONT    os_font(const LOGFONTW *lf);
void     os_text_metrics(HDC dc, TEXTMETRICW *tm);
void     os_enum_fonts(HDC dc, os_font_fn fn);
int      os_start_doc(HDC dc, const nchar *name);

/* comdlg32 */
int      os_choose_file(OPENFILENAMEW *ofn, int save);
int      os_choose_font(CHOOSEFONTW *cf);
HWND     os_find_dlg(FINDREPLACEW *fr, int replace);
/* The find dialog is modeless: what it sends back is the structure it was
 * given, in whichever half of the API put it up.  This is the lParam of that
 * message, seen from note's side. */
FINDREPLACEW *os_find_msg(void *lp);
int      os_page_setup(PAGESETUPDLGW *ps);
int      os_print_dlg(PRINTDLGW *pd);

/* shell32 */
UINT     os_drag_query(HDROP drop, UINT i, nchar *dst, int cap);

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
    PAL_MODE_OPEN,
    PAL_MODE_COUNT
};

/* The three caption buttons, where note draws them itself. */
enum { CAPBTN_NONE = 0, CAPBTN_MIN, CAPBTN_MAX, CAPBTN_CLOSE };

/* Chrome sizes, written for 96 DPI and scaled through px(). */
#define STATUS_H  22
#define TABS_H    26

/* WM_COPYDATA's dwData when one note hands a file to another.  Any other
 * value is somebody else's message and is left to the default handling. */
#define NOTE_HANDOFF  0x6E6F7465UL   /* 'note' */

#define TIMER_VIEW     1          /* repaint gutter / recolour, debounced */
#define TIMER_SESSION  2          /* autosave unsaved buffers            */
#define TIMER_HL       3          /* colour the next chunk past the view  */

#define MAX_SPANS   4096

/* How much text one pass of the highlighter tokenises at a time.  The span
 * array is fixed, so the slice has to be small enough that a dense slice
 * cannot fill it: at roughly a token every three characters, 8 K of source is
 * well inside 4096 spans. */
#define HL_SLICE   8192

/* And how much a single idle pass will colour before giving the message loop
 * its turn back.  The viewport is never chunked -- it is always finished in
 * one pass, because half a coloured screen is worse than none -- so this only
 * bounds the work done ahead of and behind it. */
#define HL_CHUNK  24576

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

    /* The stretch of this document that is known to be coloured, as one
     * character range.  RichEdit keeps formatting once it is set, so the
     * highlighter's job is not "colour the screen" but "extend this range
     * until it covers the screen and a screenful either side" -- which is
     * what makes a held Page Down cost one screen per press instead of
     * recolouring the same viewport on every frame.  Cleared whenever the
     * text, the theme or the language changes; per document, because each
     * has a control of its own holding its own formatting. */
    int     hl_from, hl_to;
    int     hl_valid;
    /* The lowest offset an edit has touched since the last colouring pass,
     * or -1.  An edit cannot change how anything above its own paragraph
     * lexes, so the range is trimmed back to that paragraph rather than
     * thrown away -- which is the difference between typing costing one
     * paragraph and typing costing a screenful of EM_SETCHARFORMAT.  It is
     * resolved to a paragraph in hl_settle(), because turning an offset into
     * a paragraph needs the line index, and the line index is rebuilt after
     * the edit, not during it. */
    int     hl_dirty_at;

    /* The characters currently carrying the caret-line background, so that
     * moving off the line knows what to put back.  RichEdit has no notion of
     * a highlighted line, so the line is highlighted by giving its own
     * characters a background -- see curline_update(). */
    int     cur_from, cur_to;
    int     cur_valid;

#if NOTE_OWN_VIEW
    /* The document itself.  With the view, the backend owns the text: this is
     * the same gap buffer the MS-DOS, Commodore 64 and Game Boy ports edit,
     * over arrays allocated here because Windows has a heap and they do not.
     * See view_room() for how the text array is sized and grown. */
    note_buffer buf;
    nchar      *text;
    int         text_cap;
    note_edit  *undo;
    nchar      *utext;
    int        *lidx;

    /* Where the view starts, as a document line and how many of that line's
     * wrapped rows are above the window.  A line and a row rather than a row
     * number on its own: a row number means nothing until the wrap width is
     * known, so it would have to be recomputed on every resize and every edit,
     * where this pair survives both.  `sub` is always 0 with wrap off. */
    int  top;
    int  sub;
    int  xoff;       /* pixels of every line scrolled off to the left      */
    int  goal;       /* the column Up and Down are trying to keep, or -1   */
    int  widest;     /* the widest line seen, in columns: the scroll range */
    int  modified;

    /* The caret sits at the end of a wrapped row rather than at the start of
     * the next one.  The two are the same offset, so without this End on a
     * wrapped row would put the caret at the start of the row below and look
     * as though it had done nothing at all. */
    int  rowend;

    /* A selection being dragged out with the mouse, and what the click that
     * started it selected -- a character, a word or a line -- so that dragging
     * after a double or triple click keeps extending by the same unit. */
    int   drag;                 /* 0 none, 1 char, 2 word, 3 line */
    int   drag_from, drag_to;   /* the word or line the drag started on */
    DWORD click_at;             /* tick of the last button down */
    int   click_x, click_y, click_n;
#endif
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
    /* The wash behind the line the caret is on.  A theme does not name it, so
     * it is mixed from the colours a theme does name and every one of the
     * shipped palettes gets it. */
    HBRUSH     br_curline;

    /* Non-zero while we are changing a control ourselves, so the EN_CHANGE it
     * sends back does not mark the document dirty. */
    int suppress;
    /* An indenting Tab has taken the key; the character it was translated
     * into is still coming and has to go the same way. */
    int eat_tab;
    int quitting;
    /* Set once the editor is up and able to take a file from another note.
     * Until then a hand-off is refused rather than run against a half-built
     * application -- startup sends messages of its own, and a message sent
     * from outside is dispatched inside every one of them. */
    int ready;
    int view_pending;         /* gutter/highlight refresh queued */
    int dpi;                  /* of the monitor the window is on */
    int tabs_h, status_h, gutter_w;
    int line_h, char_w;

    /* Cached text of the active document, shared by the gutter (paragraph
     * numbering) and the highlighter (tokenising).  Refreshed on demand. */
    nchar *cache;
    int    cache_len;
    int    cache_cap;         /* allocated, not used: the buffer is kept */
    int    cache_doc;
    int    cache_valid;

    /* Where every paragraph starts, built with the cache in one pass.
     *
     * Without it, "which line is this offset on" meant counting breaks from
     * the top of the document, and the gutter and the status bar each asked
     * that on every caret move вЂ” thirteen million comparisons per arrow key
     * in a thirteen-megabyte file, which is exactly how it felt.  With it the
     * same question is a binary search. */
    int   *lines;
    int    nlines;
    int    lines_cap;         /* kept between refreshes, like the cache */

    note_span spans[MAX_SPANS];

#if NOTE_OWN_VIEW
    /* h->font at the current zoom, which is the only font the view draws in.
     * Rebuilt whenever the face, the size, the zoom or the DPI changes; the
     * metrics it measures are h->line_h and h->char_w. */
    HFONT   viewfont;

    /* The flat run of characters the lexer is handed, copied out of the gap
     * buffer a screenful at a time.  Kept between paints and only grown. */
    nchar  *hl;
    int     hl_cap;

    /* A view paints a whole client area into this and blits it once, rather
     * than letting the screen show a background and then text drawn on top of
     * it.  One of them for the window and not one per document: only the
     * active document ever paints, and thirty-two of these would be more
     * memory than the documents. */
    HDC     back;
    HBITMAP backbm, backold;
    int     back_w, back_h;

#endif

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
     * browsers do it, so it costs no band of its own.  It says that this
     * backend is painting the caption, not which of the two ways it is doing
     * it: see frame_custom().  Zero only if the frame was never taken over at
     * all, and then the strip sits below an ordinary caption. */
    int     title_tabs;

    /* Which of the two ways.  With DWM the frame is extended into the client
     * area and the compositor goes on drawing the shadow, the rounded corners
     * and the three system buttons behind our pixels.  Without it вЂ” Windows 95
     * through XP, and any later Windows with composition off вЂ” the window is
     * still an ordinary overlapped one, but the whole top of the non-client
     * area is taken into the client by WM_NCCALCSIZE and everything DWM would
     * have drawn there is note's: the border, and the three buttons. */
    int     frame_dwm;

    /* Where minimise/maximise/close are, in client pixels.  Client and not
     * window pixels on purpose: maximised, the window rect hangs a resize
     * border off every edge of the monitor, so a rectangle measured from it
     * would sit partly off the screen and the buttons would be hard to hit. */
    RECT    sysbtn;

    /* Which of the three the pointer is over and which one a press is being
     * held on, as CAPBTN_*.  Only the frame without DWM has these: with DWM
     * the compositor lights its own buttons and DwmDefWindowProc tracks them. */
    int     cap_hot;
    int     cap_press;

    /* The scroll bar a press is being held on, while it is held: which view,
     * which of its two bars, which part of it as SBP_*, and -- for a thumb --
     * how far into the thumb it was taken hold of.  One set for all of them,
     * because the mouse is captured for the duration and there can only be
     * one. */
    HWND    sb_wnd;
    int     sb_horz, sb_part, sb_grab;

    /* The colour last handed to DWM for the caption, so the attributes are set
     * once per theme rather than once per paint.  cap_set distinguishes "never
     * set" from a theme whose ui_bg really is black. */
    note_color cap_rgb;
    int      cap_set;

    /* The status bar is repainted on every caret move, so it is a window of
     * our own that paints straight over itself.  A STATIC would erase to the
     * background first and flicker on each keystroke. */
    WCHAR   status_text[160];

    /* The menu bar is a thing you summon, not a thing that sits there.  It is
     * built at startup but only attached to the frame while it is wanted. */
    int     menu_visible;
    int     alt_chord;      /* Alt has been used as a modifier this press */

    /* Which top-level item is lit, or -1.  Windows tells us on the way past вЂ”
     * see the UAH handler вЂ” and note has to remember, because it repaints the
     * bar itself: menubar_paint() says why. */
    int     menu_hot;

    /* Double-tap Shift, IntelliJ's gesture.  See shift_gesture(). */
    DWORD   shift_last;     /* tick of the last bare Shift release, or 0 */
    int     shift_chord;    /* a key other than Shift intervened         */

    /* The command palette is an overlay of note's own, painted like the tab
     * strip: no caption, no taskbar button, and WS_EX_NOACTIVATE so the frame
     * keeps its active look while the palette is up.  Nothing focuses it, so
     * the message loop hands it the keys. */
    /* The key sheet: the same kind of overlay as the palette, and up at the
     * same times, but it answers nothing -- so it is a window of its own
     * rather than a mode, and Esc is all it listens for. */
    HWND    help;
    int     help_open;
    int     help_top;            /* first visible row, when it will not fit */

    HWND    pal;
    int     pal_open;
    int     pal_sel;             /* selected row, an index into the matches */
    int     pal_top;             /* first visible row                      */

    /* Which list the overlay is showing.  The command list is one mode among
     * several; the others stand in for the dialogs that existed only to show
     * a list.  See the mode table by pal_show().  Each remembers here what
     * its live preview has to put back if the user presses Esc. */
    int     pal_mode;

    /* Tab in the open-a-path mode walks the names the typed stem matched.
     * Completing to one of them would narrow the list to that one name, so
     * the stem is kept here and the list stays the stem's until something is
     * typed again.  pal_cycle is -1 when nothing is being cycled. */
    WCHAR   pal_stem[NOTE_PATH_MAX];
    int     pal_cycle;
    int     pal_theme_prev;               /* note_app.theme_index, may be -1 */
    WCHAR   pal_face_prev[LF_FACESIZE];
    int     pal_caret_prev;

    /* What the core last said through ops->set_hint: the palette paints it on
     * the input line.  A mode whose answer can be refused вЂ” rename, when the
     * name would land on a file that already exists вЂ” has to stay open and
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
COLORREF cr(note_color rgb);
int      px(int v);
void     read_dpi(note_host *h);
HWND     active_edit(void);

#if !NOTE_OWN_VIEW
/* Suspends RichEdit's undo around programmatic formatting; see win32_main.c. */
ITextDocument *tom_open(HWND edit);
#endif

/* The three questions the rest of the backend asks the text surface, whichever
 * one this build has.  Everything else it needs goes through the host ops. */
void edit_show_caret(note_host *h);
/* Where a document offset is, as the status bar says it: both 1-based, and the
 * line is a paragraph rather than a wrapped row. */
void edit_status_pos(note_host *h, int pos, int *line, int *col);
/* Copies [from,to) of the active document out, NUL-terminated. */
int  edit_text_range(note_host *h, int from, int to, nchar *dst, int cap);

void relayout(note_host *h);
void update_status(note_host *h);
void measure_font(note_host *h);
void on_find_msg(note_host *h, FINDREPLACEW *fr);

/* ---- published by the split ---- */
int para_at(note_host *h, int off);
void pal_show(note_host *h);
void pal_open_path(note_host *h);
/* Forgets what is coloured, so the next pass starts again.  doc < 0 is every
 * document, which is what a change of theme or of language means. */
void hl_invalidate(note_host *h, int doc);
void hl_touch(note_host *h, int doc, int off);
void hl_step(note_host *h);
void curline_update(note_host *h);
void curline_tail(note_host *h, HWND e, const RECT *clip);
void help_show(note_host *h);
void h_set_hint_text(note_host *h, const nchar *text);
void help_close(note_host *h);
int  help_key(note_host *h, int vk);
void frame_custom(note_host *h);
void tabs_layout(note_host *h);
#if !NOTE_OWN_VIEW
LRESULT CALLBACK EditProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
#endif
LRESULT CALLBACK GutterProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK HelpProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK PaletteProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK StatusProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK TabsProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
void apply_font_to(HWND e, note_host *h);
COLORREF chrome_snap(COLORREF c);
/* The same, but a colour asked for as a neutral comes back a neutral: on a
 * small palette the nearest entry to a light grey can be a pale green. */
COLORREF chrome_snap_grey(COLORREF c);
/* Whether this display has a palette to snap onto at all.  Every correction
 * that exists for a small palette is gated on this, so a true-colour display
 * gets the theme exactly as its author wrote it. */
int chrome_palettised(void);
COLORREF blend_rgb(note_color a, note_color b);
COLORREF mix_rgb(note_color a, note_color b, int num, int den);
void build_accels(note_host *h);
void build_menus(note_host *h);
void caption_buttons_calc(note_host *h);
int caption_buttons_w(note_host *h);
/* Which button is at this client point, or CAPBTN_NONE вЂ” including "none,
 * because DWM owns the buttons in this frame". */
int caption_btn_at(note_host *h, int x, int y);
void caption_colours(note_host *h);
int caption_height(void);
/* The rows of that band the caption itself occupies -- all of them with a
 * compositor, one short of them without, where the last is the border under
 * the caption. */
int caption_band(void);
/* How far down the window a compositor paints its own caption, and so how far
 * the frame is extended for it.  Below it the band is ours and opaque. */
int caption_dwm_top(void);
/* How far below the top of the window the client area starts, once the caption
 * has been taken into it. */
int frame_client_top(note_host *h);
/* One rectangle of flat colour, without a brush -- see win32_menu.c. */
void fill_px(HDC dc, int x, int y, int w, int h, COLORREF c);
/* The material every raised control in this chrome is made of: the face, the
 * three-dimensional bevel around it, and the colour a glyph on it is drawn in.
 * Shared so that a caption button and a scroll bar arrow cannot drift apart. */
note_color chrome_face(void);
COLORREF chrome_ink(void);
/* A colour part of the way from one theme colour towards another, walked
 * further along that line until the display it will be painted on stops
 * confusing it with `apart`.  See win32_menu.c. */
COLORREF chrome_apart(note_color from, note_color to, int num, int den,
                      COLORREF apart);
void chrome_button_paint(HDC dc, const RECT *box, int pushed);
int caption_plain(note_host *h);
/* The resize border, once the caption itself has been taken into the client
 * area and there is nobody left to draw it. */
void frame_border_paint(note_host *h, HWND wnd);
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
#if NOTE_OWN_VIEW
/* Puts WS_VSCROLL/WS_HSCROLL on the views, or takes them off, according to
 * whether the system's own scroll bars still suit the theme.  See sb_own(). */
void view_sb_sync(note_host *h);
#endif
void refresh_cache(note_host *h);
void service_view(note_host *h);
void set_app_dark(int dark);
void sync_menu(note_host *h);
int tabs_hit(note_host *h, int x, int y, int *on_close);
int tabs_left(note_host *h);
int tabs_right(note_host *h);

#endif /* NOTE_WIN32_H */
