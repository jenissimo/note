/* win32_view.c -- the text view note draws itself
 *
 * Part of note's Win32 backend, and the alternative to win32_edit.c: this
 * file is compiled instead of the RICHEDIT plumbing when NOTE_OWN_VIEW is on.
 * See note_win32.h for the switch.
 *
 * The editor's other three backends -- MS-DOS, the Commodore 64, the Game Boy
 * -- edit note_buffer and draw it themselves.  Windows was the odd one out: it
 * handed its text to a control and then spent a thousand lines asking that
 * control questions through EM_* messages, keeping a second copy of the
 * document and a second line index to answer the ones the control could not.
 * Here the text is the buffer, a colour is a SetTextColor before a TextOut,
 * the caret line is a FillRect, and undo is the buffer's own.
 *
 * What that buys, besides the deletions: RichEdit 2.0 messages are not on
 * Windows 95, and a window class, a font and TextOutW are.
 */

#include "note_win32.h"

#if NOTE_OWN_VIEW

/* -------------------------------------------------------------------------
 * Storage
 *
 * note_buffer never allocates -- that is what lets it run where there is no
 * heap -- so the arrays are this file's problem.  Windows has a heap, and the
 * project's stated goal is that a ten-megabyte document works, which on this
 * platform is twenty megabytes of nchar.  So the text array grows rather than
 * being sized once: it starts small enough that thirty-two empty tabs cost
 * nothing and doubles under whatever is put in it, and an insertion that
 * cannot be given room fails instead of being quietly truncated.
 *
 * The undo rings do not grow.  They are rings on purpose: when one wraps, the
 * oldest history is forgotten, which is the right thing to lose and the reason
 * a ring was chosen over a stack in the first place.  A quarter of a megabyte
 * of undo text is some thousands of keystrokes, or one large paste that the
 * ring correctly refuses to remember.
 *
 * The line index is checkpoints rather than one entry per line (see
 * NOTE_LINE_CHECKPOINTS), so its size sets how finely a large document is
 * indexed, never how large a document may be: 4096 ints is 2048 checkpoints,
 * which covers a million lines at the buffer's widest spacing.
 * ------------------------------------------------------------------------- */

#define VIEW_TEXT_MIN  (16 * 1024)     /* nchars a new document starts with  */
#define VIEW_UNDO_RECS  2048
#define VIEW_UNDO_TEXT (128 * 1024)
#define VIEW_LINE_INTS  4096

/* Tab stops.  Four, matching the tab an indenting Tab key puts in and the
 * spaces a Shift+Tab takes back out. */
#define VIEW_TAB 4

/* The widest run of columns one row of the screen is ever built for.  A
 * window wider than this in characters -- some seven thousand pixels at a
 * readable size -- draws its first thousand columns and stops. */
#define VIEW_COLS 1024

/* How much of a viewport is handed to the lexer.  Only reached by a line long
 * enough to fill the screen on its own, and then the colouring stops rather
 * than the painting. */
#define VIEW_LEX_MAX (96 * 1024)

/* Scrolling a wheel notch, when Windows has no opinion. */
#define VIEW_WHEEL_ROWS 3

#define TIMER_DRAG 7          /* autoscroll while a selection is dragged out */
#define TIMER_SB   8          /* a scroll bar arrow or page held down        */

static const WCHAR kViewClass[] = L"noteView";

/* -------------------------------------------------------------------------
 * Reaching the state
 * ------------------------------------------------------------------------- */

static win_doc *doc_of(HWND wnd)
{
    int i;
    for (i = 0; i < NOTE_MAX_DOCS; i++)
        if (g.d[i].edit == wnd) return &g.d[i];
    return 0;
}

static int index_of(HWND wnd)
{
    int i;
    for (i = 0; i < NOTE_MAX_DOCS; i++)
        if (g.d[i].edit == wnd) return i;
    return -1;
}

static win_doc *active_doc(note_host *h)
{
    if (h->app.active < 0 || h->app.active >= NOTE_MAX_DOCS) return 0;
    return h->d[h->app.active].edit ? &h->d[h->app.active] : 0;
}

/* -------------------------------------------------------------------------
 * The text array
 * ------------------------------------------------------------------------- */

/* Makes room for `want` characters, moving the buffer onto a larger array if
 * it has to.  Only the run after the gap moves, and it moves to the end of the
 * new array because that is where the free space belongs.
 *
 * Every slot past the gap therefore changes, which the line index is kept in
 * -- so the index is marked for rebuilding rather than repaired.  A grow
 * happens once per doubling of a document, and a rebuild is one pass over it.
 */
static int view_room(note_host *h, win_doc *d, int want)
{
    note_buffer *b = &d->buf;
    nchar *grown;
    int cap = d->text_cap, tail, i;

    if (cap >= want) return 1;
    if (want < 0) return 0;                 /* an int that has wrapped */
    if (cap < VIEW_TEXT_MIN) cap = VIEW_TEXT_MIN;
    while (cap < want) {
        if (cap > 0x3FFFFFFF) return 0;
        cap *= 2;
    }

    grown = (nchar *)h_alloc(h, (unsigned long)cap * sizeof(nchar));
    if (!grown) return 0;

    for (i = 0; i < b->gap; i++) grown[i] = b->buf[i];
    tail = b->cap - b->gapend;
    for (i = 0; i < tail; i++) grown[cap - tail + i] = b->buf[b->gapend + i];

    if (d->text) h_free(h, d->text);
    d->text     = grown;
    d->text_cap = cap;

    b->buf    = grown;
    b->cap    = cap;
    b->gapend = cap - tail;

    b->lines_dirty = 1;
#if !NOTE_LINE_CHECKPOINTS
    b->valid = 0;
#endif
    return 1;
}

/* -------------------------------------------------------------------------
 * The font
 *
 * One font, monospaced, at the zoom the core last asked for.  The metrics it
 * measures are h->line_h and h->char_w, which the gutter reads too, so both
 * sides of the window agree about where a row is without measuring twice.
 *
 * Monospaced is a requirement here, not an observation.  Everything below
 * measures the screen in *columns* -- a caret position, a click, a wrap point,
 * a run of one colour -- and a column is char_w pixels wide because every
 * character is.  A proportional face breaks that at the first letter: the view
 * would draw the text at its real widths and place the caret on the grid, and
 * the two would drift further apart across the line.
 *
 * The alternative is to measure every character and cache the widths, which is
 * a different view: no column grid, a per-character x table per row, a hit test
 * that binary-searches it, and a wrap point that has to be measured in pixels.
 * That is worth doing for a word processor and not for a code editor, where a
 * proportional font is a misfeature anyway -- alignment, block selection and
 * the ruler all assume a grid.
 *
 * So a proportional face is refused rather than mishandled.  The refusal
 * rewrites h->font, not just the font this file draws with, so the font dialog
 * and the saved config agree with what is on screen; and it is said out loud
 * once per face, because a silent substitution is indistinguishable from the
 * dialog not working.
 * ------------------------------------------------------------------------- */

static int  font_dpi, font_zoom, font_h, font_w, font_it;
static WCHAR font_face[LF_FACESIZE];
/* The last face turned away, so a rejection is reported once and not on every
 * zoom step and DPI change that rebuilds the font afterwards. */

static int face_same(const WCHAR *a, const WCHAR *b)
{
    int i;
    for (i = 0; i < LF_FACESIZE; i++) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

/* Fixed pitch, as the metrics report it: TMPF_FIXED_PITCH is set on a face
 * whose characters have *varying* widths, which is the one place in Windows
 * where a flag means the opposite of its name.
 *
 * The flag is a bit in a byte and means the same thing in either half of the
 * API, so the answer survives metrics that arrived from GetTextMetricsA --
 * which is where they come from on a Windows whose GetTextMetricsW is a stub. */
static int face_fixed(HDC dc, HFONT font, TEXTMETRICW *tm)
{
    HFONT old = (HFONT)SelectObject(dc, font);
    os_text_metrics(dc, tm);
    SelectObject(dc, old);
    return (tm->tmPitchAndFamily & TMPF_FIXED_PITCH) == 0;
}

/* The face to fall back to.  Consolas is on every Windows since Vista and
 * Courier New on every Windows there has ever been, so between them one is
 * always there; a machine with neither still lands on whatever the font
 * mapper gives for FIXED_PITCH, which is by definition monospaced. */
static void face_fallback(LOGFONTW *lf, int second)
{
    static const WCHAR consolas[]    = L"Consolas";
    static const WCHAR courier_new[] = L"Courier New";
    const WCHAR *want = second ? courier_new : consolas;
    int i;

    for (i = 0; i < LF_FACESIZE - 1 && want[i]; i++) lf->lfFaceName[i] = want[i];
    lf->lfFaceName[i] = 0;
    lf->lfPitchAndFamily = (BYTE)(FIXED_PITCH | FF_MODERN);
}

static void view_font_sync(note_host *h, int force)
{
    LOGFONTW    lf;
    TEXTMETRICW tm;
    HDC   dc;
    HFONT font;
    int   pt = h->fontpt > 0 ? h->fontpt : 110;
    int   zoom = h->app.zoom > 0 ? h->app.zoom : 100;
    int   dpi  = h->dpi > 0 ? h->dpi : 96;
    int   try_;

    if (!force && h->viewfont &&
        font_dpi == dpi && font_zoom == zoom &&
        font_h == h->font.lfHeight && font_w == h->font.lfWeight &&
        font_it == h->font.lfItalic && face_same(font_face, h->font.lfFaceName))
        return;

    memset(&tm, 0, sizeof(tm));
    lf = h->font;
    /* The point size the font dialog reports, in tenths, taken to pixels on
     * this monitor and then scaled by the zoom.  Not lfHeight: that was
     * measured at whatever DPI the window was on when it was set. */
    lf.lfHeight = -MulDiv(pt * dpi, zoom, 72 * 10 * 100);
    if (lf.lfHeight > -2) lf.lfHeight = -2;
    lf.lfWidth  = 0;

    dc = GetDC(h->wnd);
    for (try_ = 0; try_ < 3; try_++) {
        font = os_font(&lf);
        if (font && face_fixed(dc, font, &tm)) break;
        if (font) DeleteObject(font);
        font = 0;
        /* Silently.  The font dialog offers fixed-pitch faces only, so a
         * proportional one can no longer be chosen -- what reaches here is a
         * config naming a face this machine does not have, which nobody in
         * this session asked for and nobody can act on.  A box explaining an
         * already-corrected problem, in front of a window that has not
         * appeared yet, is the genre of dialog this editor exists against. */
        face_fallback(&lf, try_);
        /* The substitute is the document's font from here on, so the dialog,
         * the palette's font list and the saved config all name what is
         * actually on screen. */
        n_copy((nchar *)h->font.lfFaceName, (const nchar *)lf.lfFaceName,
               LF_FACESIZE);
        h->font.lfPitchAndFamily = lf.lfPitchAndFamily;
    }
    /* Three faces refused in a row means the font mapper is giving something
     * unexpected for FIXED_PITCH; the last one is used anyway, because a view
     * with no font at all draws nothing.  Measured here rather than trusting
     * the metrics of whichever try failed last. */
    if (!font) {
        font = os_font(&lf);
        if (font) face_fixed(dc, font, &tm);
    }
    ReleaseDC(h->wnd, dc);

    if (h->viewfont) DeleteObject(h->viewfont);
    h->viewfont = font;

    h->line_h = tm.tmHeight;
    h->char_w = tm.tmAveCharWidth;
    if (h->line_h <= 0) h->line_h = 16;
    if (h->char_w <= 0) h->char_w = 8;

    font_dpi  = dpi;
    font_zoom = zoom;
    font_h    = h->font.lfHeight;
    font_w    = h->font.lfWeight;
    font_it   = h->font.lfItalic;
    n_copy((nchar *)font_face, (const nchar *)h->font.lfFaceName, LF_FACESIZE);
}

/* -------------------------------------------------------------------------
 * Geometry
 *
 * A column is a cell of the monospaced grid, which is not a character: a tab
 * is as many columns as it takes to reach the next stop.  Everything the view
 * measures on screen is in columns, and everything the buffer knows is in
 * document offsets.
 *
 * A *row* is one line of the screen.  With wrap off a row is a document line;
 * with wrap on a line is cut into as many rows as it takes, and a view_row is
 * the run one of them covers -- in offsets and in columns both, because the
 * columns are what the tab stops and the caret are measured in and they go on
 * counting from the start of the *line*, not of the row.  Every question the
 * view asks about the screen is asked of a view_row, so wrap on and wrap off
 * are one code path with the width set to zero for "no limit".
 *
 * What is deliberately absent is a table.  A row number for a wrapped document
 * cannot be had without measuring every line, so nothing here ever asks for
 * one: the view keeps a line and a row within it (win_doc's `top` and `sub`),
 * and every screen question is answered by walking rows from there.  That walk
 * is bounded by the height of the window plus the line it starts in, which is
 * what a paint already costs -- so wrap costs a ten-megabyte document no
 * memory and no pass over the text, and there is no cached row count that an
 * edit, a resize or a change of font could leave stale.
 *
 * The price is that the vertical scroll bar counts lines rather than rows.
 * See view_scrollbars().
 * ------------------------------------------------------------------------- */

typedef struct {
    int off, col;    /* where the row starts: document offset, and column */
    int end, ecol;   /* and just past its last character                  */
    int last;        /* nothing of the line is left after it              */
} view_row;

/* A row, and the line it is a row of.  Walking the screen means walking one of
 * these, which is why the line's start and length are carried along: they are
 * two buffer queries per line and every row of the line reuses them. */
typedef struct {
    int      line, lstart, llen;
    view_row r;
} view_walk;

/* -------------------------------------------------------------------------
 * The scroll bars
 *
 * On the classic frame the view draws them itself, inside its own client area,
 * rather than leaving them to Windows in the non-client area a WS_VSCROLL gives
 * it.  With a compositor it keeps the system's, and that split is deliberate.
 *
 * The reason to draw them at all is Windows 95.  Everything else about the
 * window there comes from the theme -- the caption, the tabs, the gutter, the
 * menus, the status bar -- and a system scroll bar is painted from GetSysColor
 * whatever the rest of the window is doing.  On a dark theme that is a bright
 * strip down the right-hand edge of a black page: the one part of the window
 * that belongs to another program.  Nothing on that Windows will theme it for
 * us, so the view does it.
 *
 * The reason not to, on Windows 11, is that there the system's scroll bar is
 * the one the user is expecting -- it follows the dark mode, it is the width
 * every other application's is, and it behaves the way the rest of the desktop
 * does.  Drawing our own there would be replacing something correct.  It was
 * tried the other way first, for one geometry instead of two; the answer was
 * that two geometries are worth it, and this is that answer.
 *
 * So the difference lives in one place.  view_client() is the text area: the
 * whole client rect where the system's bars are outside it, and the client rect
 * less the strips where they are ours.  Every measurement of what is on screen
 * goes through it, and so does the hit test, so neither can quietly assume the
 * wrong one.  Everything below sb_ is inert on the composited path.
 *
 * A strip inside the client rather than a themed WS_VSCROLL: WM_NCPAINT over a
 * non-client scroll bar means intercepting a paint the system also does for
 * itself, in a region it computes, on messages that arrive in an order that has
 * never been documented.
 * ------------------------------------------------------------------------- */

/* Which part of a bar a point is over.  In track order, so a comparison
 * against SBP_THUMB says which side of the thumb a page click was on. */
enum { SBP_NONE = 0, SBP_LINEUP, SBP_PAGEUP, SBP_THUMB, SBP_PAGEDOWN,
       SBP_LINEDOWN };

/* The system's own scroll bar metrics, so a desktop set up for larger ones
 * gets larger ones here too. */
static int sb_w(void)
{
    int v = GetSystemMetrics(SM_CXVSCROLL);
    return v > 0 ? v : px(16);
}

static int sb_h(void)
{
    int v = GetSystemMetrics(SM_CYHSCROLL);
    return v > 0 ? v : px(16);
}

/* Whether the bars are the view's to draw.  Everything else in this section
 * answers to this: where they are not, the window carries WS_VSCROLL and
 * WS_HSCROLL and Windows draws, hit-tests and scrolls them as it always did.
 *
 * This is a question about colour, not about which Windows this is, and the
 * distinction matters because "no compositor" is not "Windows 95".  It is also
 * Windows 2000, XP in classic mode, XP with visual styles, Vista and 7 with
 * Aero switched off or under Basic, and any remote-desktop or safe-mode session
 * on any of them.  On every one of those the system's scroll bar is the one the
 * user is expecting, and shipping ours there would be the same regression as
 * shipping it on Windows 11 -- on machines nobody here can test with, so nobody
 * would find it.
 *
 * What actually went wrong on the guest was not the age of the Windows.  It was
 * that the theme was black and the system's bar was grey: a scroll bar is drawn
 * to sit beside COLOR_WINDOW, and ours was sitting beside a page that was
 * nothing like it.  The same Windows 95 on a light theme looked right, and that
 * is the test asked here -- is our page still the colour the system drew that
 * bar to sit against?  Where it is, the system's bar is used, on every Windows,
 * which is the safe answer for all the ones between the two we can test on.
 *
 * A composited desktop is never ours, and for the same reason rather than as an
 * exception to it: there the system's non-client controls follow the light or
 * dark mode note has already asked for -- see set_app_dark() -- so they are not
 * pasted on to begin with, and a user who has that desktop expects the scroll
 * bar every other window on it has. */
static int sb_own(note_host *h)
{
    COLORREF sys = GetSysColor(COLOR_WINDOW);
    long dr, dg, db;

    if (h->frame_dwm) return 0;

    dr = (long)GetRValue(sys) - (long)((h->theme.bg >> 16) & 0xFF);
    dg = (long)GetGValue(sys) - (long)((h->theme.bg >>  8) & 0xFF);
    db = (long)GetBValue(sys) - (long)( h->theme.bg        & 0xFF);

    /* The same weighting chrome_snap() matches colours with, against a quarter
     * of the range: far enough apart that a grey bar beside our page reads as
     * another program's window, and near enough that a cream or an off-white
     * page keeps the bar the rest of the desktop has. */
    return (3 * dr * dr + 6 * dg * dg + db * db) / 10 > 64L * 64L;
}

static void view_scrollbars(note_host *h, HWND wnd, win_doc *d);

/* The styles follow the answer, because the answer can change while note is
 * running: the theme is a menu item.  Called wherever the theme is applied. */
void view_sb_sync(note_host *h)
{
    LONG want = sb_own(h) ? 0 : (LONG)(WS_VSCROLL | WS_HSCROLL);
    int  i;

    for (i = 0; i < NOTE_MAX_DOCS; i++) {
        HWND e = h->d[i].edit;
        LONG st;

        if (!e) continue;

        /* The same call the tab strip gets, for the same reason and now that
         * this window has scroll bars of the system's to theme.  Asking the app
         * for dark mode is not enough on its own: a control's own non-client
         * scroll bar keeps the light colours until its window is named as one
         * of the dark ones, which is how ours came out a white strip down the
         * side of a black page.  Behind the probe, because SetWindowTheme is
         * uxtheme and the name is Windows 10's; where either is missing this
         * does nothing and the bar is whatever the system draws. */
        if (caps.set_window_theme)
            caps.set_window_theme(e, h->theme.dark ? L"DarkMode_Explorer"
                                                   : L"Explorer", NULL);

        /* The A form on purpose: the style is not text, and this window class
         * was registered through RegisterClassExA where the boundary said so. */
        st = GetWindowLongA(e, GWL_STYLE);
        if ((st & (LONG)(WS_VSCROLL | WS_HSCROLL)) == want) continue;

        st = (st & ~(LONG)(WS_VSCROLL | WS_HSCROLL)) | want;
        SetWindowLongA(e, GWL_STYLE, st);
        /* The non-client area has changed shape, so the frame has to be
         * recalculated rather than merely repainted. */
        SetWindowPos(e, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
        view_scrollbars(h, e, &h->d[i]);
        InvalidateRect(e, NULL, FALSE);
    }
}

/* The horizontal bar is only there when there is something to scroll to: with
 * wrap on every line already fits. */
static int sb_has_horz(note_host *h)
{
    return sb_own(h) && !h->app.wrap;
}

/* The text area: the client rect, less the bars where they are drawn inside it.
 * Every measurement of what is on screen goes through here, and so does the hit
 * test -- the system's bars are already outside the client rect, so on that
 * path this is GetClientRect and nothing more. */
static void view_client(note_host *h, HWND wnd, RECT *rc)
{
    GetClientRect(wnd, rc);
    if (!sb_own(h)) return;

    rc->right -= sb_w();
    if (sb_has_horz(h)) rc->bottom -= sb_h();
    if (rc->right  < 0) rc->right  = 0;
    if (rc->bottom < 0) rc->bottom = 0;
}

static void sb_vrect(note_host *h, HWND wnd, RECT *r)
{
    RECT cl;
    GetClientRect(wnd, &cl);
    r->left   = cl.right - sb_w();
    r->right  = cl.right;
    r->top    = 0;
    r->bottom = cl.bottom - (sb_has_horz(h) ? sb_h() : 0);
    if (r->left < 0)         r->left   = 0;
    if (r->bottom < r->top)  r->bottom = r->top;
}

static void sb_hrect(note_host *h, HWND wnd, RECT *r)
{
    RECT cl;
    GetClientRect(wnd, &cl);
    r->left   = 0;
    r->right  = cl.right - sb_w();
    r->top    = cl.bottom - sb_h();
    r->bottom = cl.bottom;
    if (r->top < 0)         r->top   = 0;
    if (r->right < r->left) r->right = r->left;
}

static int view_rows(note_host *h, HWND wnd)
{
    RECT rc;
    view_client(h, wnd, &rc);
    return h->line_h > 0 ? (rc.bottom + h->line_h - 1) / h->line_h : 1;
}

/* The width a row is wrapped to, in columns, or 0 when wrap is off.
 *
 * Derived from the client area every time it is wanted rather than stored, so
 * a resize invalidates nothing: there is no second copy of the width to fall
 * out of step with the window.  One column is held back so the caret at the
 * end of a full row is still inside the window rather than under its edge. */
static int wrap_cols(note_host *h, HWND wnd)
{
    RECT rc;
    int  cols;

    if (!h->app.wrap) return 0;
    view_client(h, wnd, &rc);
    cols = (h->char_w > 0) ? rc.right / h->char_w - 1 : 0;
    /* A window dragged down to nothing still has to lay text out somewhere,
     * and a width of zero would put no character on any row. */
    if (cols < 4) cols = 4;
    return cols;
}

/* Where the row that starts at `r->off`/`r->col` ends.
 *
 * The break goes after the last space that still fits, so a word is not cut in
 * half.  A word wider than the whole row has nowhere to break and is cut where
 * the row runs out, which is the only thing to do with a line of base64.  At
 * least one character always goes on the row, or a window narrower than a tab
 * stop would lay out rows for ever without advancing. */
static void row_fill(note_buffer *b, int lstart, int llen, int wcols,
                     view_row *r)
{
    int end   = lstart + llen;
    int limit = (wcols > 0) ? r->col + wcols : 0x7FFFFFFF;
    int i = r->off, col = r->col, brk = -1, bcol = 0;

    while (i < end) {
        int rl, k = 0;
        const nchar *q = note_buffer_span(b, i, &rl);
        if (rl <= 0) break;
        if (rl > end - i) rl = end - i;
        while (k < rl) {
            nchar c = q[k];
            int   w = (c == (nchar)'\t') ? VIEW_TAB - (col % VIEW_TAB) : 1;
            if (col + w > limit && i + k > r->off) {
                if (brk > r->off) { r->end = brk;     r->ecol = bcol; }
                else              { r->end = i + k;   r->ecol = col;  }
                r->last = 0;
                return;
            }
            col += w;
            k++;
            /* The space stays on the row it ended, which is what keeps the
             * next row starting with a word rather than with a gap. */
            if (c == (nchar)' ' || c == (nchar)'\t') { brk = i + k; bcol = col; }
        }
        i += rl;
    }
    r->end  = end;
    r->ecol = col;
    r->last = 1;
}

/* The row `sub` rows down from the start of a line. */
static void row_of_sub(note_buffer *b, int lstart, int llen, int wcols,
                       int sub, view_row *r)
{
    r->off = lstart;
    r->col = 0;
    row_fill(b, lstart, llen, wcols, r);
    while (sub-- > 0 && !r->last) {
        r->off = r->end;
        r->col = r->ecol;
        row_fill(b, lstart, llen, wcols, r);
    }
}

/* How many rows a line takes.  Never less than one: an empty line is a row.
 *
 * The answer is one for every line when nothing is wrapped, and answering it
 * without looking at the text is what keeps scrolling over a document of very
 * long lines free: the callers below ask this per line they step over. */
static int row_count(note_buffer *b, int lstart, int llen, int wcols)
{
    view_row r;
    int n = 1;

    if (wcols <= 0) return 1;

    r.off = lstart;
    r.col = 0;
    row_fill(b, lstart, llen, wcols, &r);
    while (!r.last) {
        r.off = r.end;
        r.col = r.ecol;
        row_fill(b, lstart, llen, wcols, &r);
        n++;
    }
    return n;
}

/* The row `pos` is on, and which of the line's rows that is.
 *
 * A wrap point is one offset and two places on the screen -- the end of one
 * row and the start of the next -- and which one is meant depends on how the
 * caret got there.  `atend` is win_doc's rowend: normally the offset belongs
 * to the row that starts there, and after End it belongs to the row that ends
 * there. */
static int row_of_pos(note_buffer *b, int lstart, int llen, int wcols,
                      int pos, int atend, view_row *r)
{
    int n = 0;

    r->off = lstart;
    r->col = 0;
    row_fill(b, lstart, llen, wcols, r);
    while (!r->last && (atend ? pos > r->end : pos >= r->end)) {
        r->off = r->end;
        r->col = r->ecol;
        row_fill(b, lstart, llen, wcols, r);
        n++;
    }
    return n;
}

/* The column `pos` sits in, counted from the start of its line so that tab
 * stops stay where they are on a row that begins mid-line. */
static int col_in_row(note_buffer *b, const view_row *r, int pos)
{
    int col = r->col, i = r->off;

    while (i < pos && i < r->end) {
        nchar c = note_buffer_at(b, i);
        col += (c == (nchar)'\t') ? VIEW_TAB - (col % VIEW_TAB) : 1;
        i++;
    }
    return col;
}

/* And back: the offset whose cell covers `col`, clamped to the row.  A click
 * in the middle of a tab lands on the tab. */
static int pos_in_row(note_buffer *b, const view_row *r, int col)
{
    int c = r->col, i = r->off;

    while (i < r->end) {
        nchar ch = note_buffer_at(b, i);
        int   w  = (ch == (nchar)'\t') ? VIEW_TAB - (c % VIEW_TAB) : 1;
        /* Past the middle of a cell is the next one, so clicking the right
         * half of a character puts the caret after it. */
        if (col < c + (w + 1) / 2) return i;
        c += w;
        i++;
    }
    return r->end;
}

/* Walking the screen ------------------------------------------------------- */

static void walk_at(note_buffer *b, view_walk *w, int line, int sub, int wcols)
{
    w->line   = line;
    w->lstart = note_buffer_line_start(b, line);
    w->llen   = note_buffer_line_len(b, line);
    row_of_sub(b, w->lstart, w->llen, wcols, sub, &w->r);
}

/* The next row down, or 0 at the end of the document. */
static int walk_next(note_buffer *b, view_walk *w, int wcols, int nlines)
{
    if (!w->r.last) {
        w->r.off = w->r.end;
        w->r.col = w->r.ecol;
        row_fill(b, w->lstart, w->llen, wcols, &w->r);
        return 1;
    }
    if (w->line + 1 >= nlines) return 0;
    walk_at(b, w, w->line + 1, 0, wcols);
    return 1;
}

/* `n` rows back from (line, sub), stopping at the top of the document.
 *
 * Within a line this is arithmetic; crossing into the line above costs one
 * pass over it to find out how many rows it has.  Either way the work is
 * bounded by `n`, and every caller's `n` is at most a screenful. */
static void rows_back(note_buffer *b, int *line, int *sub, int n, int wcols)
{
    while (n > 0) {
        if (*sub > 0) {
            int step = (*sub < n) ? *sub : n;
            *sub -= step;
            n    -= step;
            continue;
        }
        if (*line <= 0) return;
        (*line)--;
        *sub = row_count(b, note_buffer_line_start(b, *line),
                            note_buffer_line_len(b, *line), wcols) - 1;
        n--;
    }
}

static void rows_fwd(note_buffer *b, int *line, int *sub, int n, int wcols,
                     int nlines)
{
    while (n > 0) {
        int rows = row_count(b, note_buffer_line_start(b, *line),
                                note_buffer_line_len(b, *line), wcols);
        if (*sub + n < rows) { *sub += n; return; }
        if (*line + 1 >= nlines) { *sub = rows - 1; return; }
        n -= rows - *sub;
        (*line)++;
        *sub = 0;
    }
}

/* How many rows lie between (line, sub) and the row below it at (tline, tsub),
 * giving up once the answer is `limit` or more.
 *
 * The giving up is the point: the only thing the callers want to know is
 * whether the caret is still on the screen, and stopping at the height of the
 * window is what keeps a caret ten thousand lines away from costing ten
 * thousand lines of measuring. */
static int rows_between(note_buffer *b, int line, int sub,
                        int tline, int tsub, int limit, int wcols)
{
    int n = 0;

    while (line < tline) {
        n += row_count(b, note_buffer_line_start(b, line),
                          note_buffer_line_len(b, line), wcols) - sub;
        sub = 0;
        line++;
        if (n >= limit) return limit;
    }
    n += tsub - sub;
    return n < 0 ? 0 : n;
}

static int caret_line(win_doc *d)
{
    return note_buffer_line_at(&d->buf, d->buf.caret);
}

/* The caret's row, and which of its line's rows it is. */
static int caret_row(win_doc *d, int wcols, view_row *r, int *line)
{
    note_buffer *b = &d->buf;
    int l = caret_line(d);

    if (line) *line = l;
    return row_of_pos(b, note_buffer_line_start(b, l),
                         note_buffer_line_len(b, l), wcols,
                      b->caret, d->rowend, r);
}

/* Puts `top`, `sub` and `xoff` back inside a document that may have been
 * edited, or a window that may have been resized, under them. */
static void view_clamp(note_host *h, HWND wnd, win_doc *d)
{
    note_buffer *b = &d->buf;
    int nlines = note_buffer_lines(b), wcols = wrap_cols(h, wnd), n;

    if (d->top >= nlines) d->top = nlines - 1;
    if (d->top < 0) d->top = 0;

    if (wcols <= 0) { d->sub = 0; return; }
    /* Nothing scrolls sideways when the text is wrapped to fit. */
    d->xoff = 0;
    if (d->sub <= 0) { d->sub = 0; return; }
    n = row_count(b, note_buffer_line_start(b, d->top),
                     note_buffer_line_len(b, d->top), wcols);
    if (d->sub >= n) d->sub = n - 1;
}

/* -------------------------------------------------------------------------
 * Scrolling and the caret
 * ------------------------------------------------------------------------- */

/* The vertical bar counts lines, not rows.
 *
 * Rows would want the row number of the top of the screen and the row count of
 * the document, and neither can be had for a wrapped ten-megabyte file without
 * measuring all of it.  A line is a position the buffer already knows, so the
 * thumb is exact at the start of every line and drifts by at most the height of
 * one paragraph inside one -- which is the same approximation as scrolling a
 * long document by a bar three hundred pixels tall.
 *
 * The horizontal bar counts columns, and is not there at all when the text is
 * wrapped to fit: there is nothing to its right to scroll to. */
static void sb_range(note_host *h, HWND wnd, win_doc *d, int horz,
                     int *pos, int *page, int *total)
{
    RECT rc;

    view_client(h, wnd, &rc);
    if (horz) {
        *page  = h->char_w > 0 ? rc.right / h->char_w : 1;
        *pos   = h->char_w > 0 ? d->xoff  / h->char_w : 0;
        *total = d->widest + 1;
    } else {
        *page  = view_rows(h, wnd);
        *pos   = d->top;
        *total = note_buffer_lines(&d->buf);
    }
    if (*page  < 1) *page  = 1;
    if (*total < 1) *total = 1;
    if (*pos   < 0) *pos   = 0;
}

/* Where the thumb sits in a track that long, and how long it is.
 *
 * Answers 0 when the whole document is already on screen.  That is how a bar
 * says it is disabled, and it says it the way Windows does: an empty track and
 * no thumb at all, rather than a thumb that fills it and cannot be moved. */
static int sb_thumb(int track, int minlen, int pos, int page, int total,
                    int *tp, int *tl)
{
    int span = total - page, len;

    if (span <= 0 || track <= 0) return 0;
    if (pos > span) pos = span;
    len = (int)((long)track * page / total);
    if (len < minlen) len = minlen;
    if (len > track)  len = track;
    *tl = len;
    *tp = (int)((long)(track - len) * pos / span);
    return 1;
}

/* The two square buttons at the ends of a bar, and the track between them.
 * Both buttons come off even when there is no room for the track, because a
 * bar with one arrow in it is worse than a bar with no track. */
static int sb_btn_size(note_host *h, int horz)
{
    (void)h;
    return horz ? sb_h() : sb_w();
}

/* Which part of a bar a point is over, and where the thumb was when it was
 * asked.  The point is assumed to be inside the bar already. */
static int sb_part_at(note_host *h, HWND wnd, win_doc *d, int horz,
                      int x, int y, int *tp, int *tl)
{
    RECT bar;
    int  btn = sb_btn_size(h, horz);
    int  at, lo, len, track, pos, page, total, minlen;

    if (horz) { sb_hrect(h, wnd, &bar); at = x; lo = bar.left; len = bar.right - bar.left; }
    else      { sb_vrect(h, wnd, &bar); at = y; lo = bar.top;  len = bar.bottom - bar.top; }

    *tp = *tl = 0;
    if (len <= btn * 2) return SBP_NONE;
    if (at < lo + btn)        return SBP_LINEUP;
    if (at >= lo + len - btn) return SBP_LINEDOWN;

    track  = len - btn * 2;
    minlen = GetSystemMetrics(horz ? SM_CXHTHUMB : SM_CYVTHUMB);
    if (minlen <= 0) minlen = btn;
    sb_range(h, wnd, d, horz, &pos, &page, &total);
    if (!sb_thumb(track, minlen, pos, page, total, tp, tl)) return SBP_NONE;

    at -= lo + btn;
    if (at < *tp)       return SBP_PAGEUP;
    if (at < *tp + *tl) return SBP_THUMB;
    return SBP_PAGEDOWN;
}

/* One arrow: the raised button, and a solid triangle pointing out of the bar.
 * Drawn as rows rather than with a pen, so it is the same triangle at every
 * size and there is no line-drawing rounding to disagree with. */
static void sb_arrow(HDC dc, const RECT *r, int dir, int pushed)
{
    COLORREF ink = chrome_ink();
    int w = r->right - r->left, h = r->bottom - r->top;
    int n = (w < h ? w : h) / 4, i, cx, cy;

    chrome_button_paint(dc, r, pushed);
    if (n < 2) n = 2;
    /* Pushed, the glyph steps down and right with the bevel, exactly as the
     * caption buttons do. */
    cx = r->left + w / 2 + (pushed ? 1 : 0);
    cy = r->top  + h / 2 + (pushed ? 1 : 0);

    for (i = 0; i < n; i++) {
        int half = (dir == 0 || dir == 2) ? i : n - 1 - i;
        if (dir == 0 || dir == 1)
            fill_px(dc, cx - half, cy - n / 2 + i, half * 2 + 1, 1, ink);
        else
            fill_px(dc, cx - n / 2 + i, cy - half, 1, half * 2 + 1, ink);
    }
}

/* One bar, whole: the trough, the two arrows and the thumb. */
/* A twelfth of the way from the chrome towards its text -- quieter than the
 * face, so the thumb reads as the thing that moves.  Kept off the face rather
 * than merely asked for: on a small palette a twelfth and a sixth land on the
 * same entry, and a thumb the colour of its own trough cannot be found. */
static COLORREF sb_trough(note_host *h)
{
    return chrome_apart(h->theme.ui_bg, h->theme.ui_fg, 1, 12,
                        chrome_snap(cr(chrome_face())));
}

static void sb_paint_bar(note_host *h, HWND wnd, HDC dc, win_doc *d, int horz)
{
    RECT bar, r;
    COLORREF trough = sb_trough(h);
    int btn = sb_btn_size(h, horz);
    int len, track, tp, tl, pos, page, total, minlen;
    int held = (h->sb_wnd == wnd && h->sb_horz == horz) ? h->sb_part : SBP_NONE;

    if (horz) sb_hrect(h, wnd, &bar); else sb_vrect(h, wnd, &bar);
    len = horz ? bar.right - bar.left : bar.bottom - bar.top;
    if (len <= 0 || bar.right <= bar.left || bar.bottom <= bar.top) return;

    fill_px(dc, bar.left, bar.top, bar.right - bar.left,
            bar.bottom - bar.top, trough);
    if (len <= btn * 2) return;

    r = bar;
    if (horz) r.right = bar.left + btn; else r.bottom = bar.top + btn;
    sb_arrow(dc, &r, horz ? 2 : 0, held == SBP_LINEUP);

    r = bar;
    if (horz) r.left = bar.right - btn; else r.top = bar.bottom - btn;
    sb_arrow(dc, &r, horz ? 3 : 1, held == SBP_LINEDOWN);

    track  = len - btn * 2;
    minlen = GetSystemMetrics(horz ? SM_CXHTHUMB : SM_CYVTHUMB);
    if (minlen <= 0) minlen = btn;
    sb_range(h, wnd, d, horz, &pos, &page, &total);
    if (!sb_thumb(track, minlen, pos, page, total, &tp, &tl)) return;

    r = bar;
    if (horz) { r.left = bar.left + btn + tp; r.right  = r.left + tl; }
    else      { r.top  = bar.top  + btn + tp; r.bottom = r.top  + tl; }
    /* The thumb is never drawn pushed: on a classic bar it is dragged, not
     * pressed, and its face stays raised the whole way. */
    chrome_button_paint(dc, &r, 0);
}

/* Both of them, and the square where they meet.
 *
 * Plus, where it is needed and only there, a rule along the edge each bar
 * presents to the text.  The trough is deliberately quiet, so on a display
 * that has collapsed the theme it can end up the colour of the page beside it
 * -- on the guest's Dark theme at 8bpp every dark tone in the theme is the
 * same black -- and then nothing would say where the page stops and the bar
 * begins.  A system scroll bar has the non-client edge for that; this one is
 * inside the client, so it draws the line itself.  It is asked for by
 * comparing what the two colours actually became, so a display with room for
 * both keeps the plain bar it already had.
 *
 * Drawn last and over the whole length, arrows included, so it is one
 * unbroken line rather than a mark beside the track. */
static void sb_paint(note_host *h, HWND wnd, win_doc *d, HDC dc)
{
    COLORREF face;
    COLORREF page, edge;
    int      rule;
    RECT cl, bar;

    if (!d || !sb_own(h)) return;

    face = chrome_snap(cr(chrome_face()));
    page = chrome_snap(cr(h->theme.bg));
    rule = (sb_trough(h) == page);
    edge = chrome_apart(h->theme.bg, h->theme.fg, 1, 5, page);
    sb_paint_bar(h, wnd, dc, d, 0);
    if (rule) {
        sb_vrect(h, wnd, &bar);
        fill_px(dc, bar.left, bar.top, 1, bar.bottom - bar.top, edge);
    }

    if (!sb_has_horz(h)) return;

    sb_paint_bar(h, wnd, dc, d, 1);
    if (rule) {
        sb_hrect(h, wnd, &bar);
        fill_px(dc, bar.left, bar.top, bar.right - bar.left, 1, edge);
    }

    GetClientRect(wnd, &cl);
    fill_px(dc, cl.right - sb_w(), cl.bottom - sb_h(), sb_w(), sb_h(), face);
}

/* Where the view stands, said to whoever is drawing the bars.
 *
 * The system's want SCROLLINFO: a range in lines, a page, and where the top of
 * the screen is.  The horizontal one goes away entirely when the text is
 * wrapped to fit -- an empty range with no SIF_DISABLENOSCROLL is how Windows
 * is asked to hide a bar the window was created with.
 *
 * Ours want nothing said at all: they are part of the client area and read the
 * same state the paint does, so what changed is a rectangle to repaint.  Only
 * the strips, not the text -- the callers that changed the text have already
 * asked for that. */
static void view_scrollbars(note_host *h, HWND wnd, win_doc *d)
{
    SCROLLINFO si;
    RECT rc;
    int  rows, cols;

    if (sb_own(h)) {
        RECT r;
        sb_vrect(h, wnd, &r);
        InvalidateRect(wnd, &r, FALSE);
        if (sb_has_horz(h)) {
            sb_hrect(h, wnd, &r);
            InvalidateRect(wnd, &r, FALSE);
        }
        return;
    }

    view_client(h, wnd, &rc);
    rows = view_rows(h, wnd);
    cols = h->char_w > 0 ? rc.right / h->char_w : 1;

    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin   = 0;
    si.nMax   = note_buffer_lines(&d->buf) - 1;
    si.nPage  = (UINT)(rows > 0 ? rows : 1);
    si.nPos   = d->top;
    SetScrollInfo(wnd, SB_VERT, &si, TRUE);

    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
    if (!h->app.wrap) {
        si.fMask |= SIF_DISABLENOSCROLL;
        si.nMax   = d->widest;
        si.nPage  = (UINT)(cols > 0 ? cols : 1);
        si.nPos   = h->char_w > 0 ? d->xoff / h->char_w : 0;
    }
    SetScrollInfo(wnd, SB_HORZ, &si, TRUE);
    /* Said outright as well as implied by the empty range: which of the two
     * Windows acts on has varied, and a wrapped view with a dead scroll bar
     * along the bottom is the sort of thing that gets noticed. */
    ShowScrollBar(wnd, SB_HORZ, h->app.wrap ? FALSE : TRUE);
}

/* Where the caret belongs in the client area, or 0 if it is not on screen.
 *
 * The column is measured within the row rather than within the line, because a
 * wrapped row starts at whatever column its line had reached and the screen
 * starts at the left margin either way. */
static int caret_xy(note_host *h, HWND wnd, win_doc *d, int *px_, int *py)
{
    view_row r;
    RECT rc;
    int  wcols = wrap_cols(h, wnd), rows = view_rows(h, wnd);
    int  line, sub, col, x, y;

    view_client(h, wnd, &rc);
    sub = caret_row(d, wcols, &r, &line);
    col = col_in_row(&d->buf, &r, d->buf.caret) - r.col;

    x = col * h->char_w - d->xoff;
    if (line < d->top || (line == d->top && sub < d->sub)) y = -h->line_h;
    else y = rows_between(&d->buf, d->top, d->sub, line, sub,
                          rows + 1, wcols) * h->line_h;

    *px_ = x;
    *py  = y;
    return y >= 0 && y < rc.bottom && x >= 0 && x <= rc.right;
}

static void caret_sync(note_host *h, HWND wnd, win_doc *d)
{
    int x, y;

    if (GetFocus() != wnd) return;

    /* A caret parked outside the client area is put well outside it rather
     * than clamped to the edge, where it would sit and blink on a row it is
     * not on. */
    if (!caret_xy(h, wnd, d, &x, &y)) { x = -32; y = -32; }
    SetCaretPos(x, y);
}

/* Brings the caret back onto the screen, vertically and horizontally, and
 * says whether anything moved -- the caller repaints only if something did.
 *
 * A caret that has left the screen downwards is brought to the bottom row by
 * walking back from the caret rather than forward from the top: the caret may
 * be a million rows below the view after Ctrl+End or a jump to a line, and
 * walking back a windowful is bounded where walking down to it is not. */
static int scroll_to_caret(note_host *h, HWND wnd, win_doc *d)
{
    note_buffer *b = &d->buf;
    view_row r;
    RECT rc;
    int  line, sub, col, x, w, rows, wcols;
    int  top = d->top, tsub = d->sub, xoff = d->xoff;

    view_client(h, wnd, &rc);
    wcols = wrap_cols(h, wnd);
    rows  = h->line_h > 0 ? rc.bottom / h->line_h : 1;
    if (rows < 1) rows = 1;

    sub = caret_row(d, wcols, &r, &line);

    if (line < top || (line == top && sub < tsub)) {
        top = line; tsub = sub;
    } else if (rows_between(b, top, tsub, line, sub, rows, wcols) >= rows) {
        top = line; tsub = sub;
        rows_back(b, &top, &tsub, rows - 1, wcols);
    }

    if (wcols > 0) {
        xoff = 0;
    } else {
        col = col_in_row(b, &r, b->caret) - r.col;
        x   = col * h->char_w;
        w   = rc.right - h->char_w;
        if (w < h->char_w) w = h->char_w;

        if (x < xoff) xoff = x;
        if (x > xoff + w) xoff = x - w;
        /* A few columns of air before the first, so the caret at column zero
         * is not flush against the gutter after a scroll back to the left. */
        if (xoff < h->char_w * 2) xoff = 0;
    }

    if (top == d->top && tsub == d->sub && xoff == d->xoff) return 0;
    d->top  = top;
    d->sub  = tsub;
    d->xoff = xoff;
    return 1;
}

/* Everything that changes what is on screen ends here: the scroll bars, the
 * caret, the gutter and the status bar all follow the same move. */
static void view_refresh(note_host *h, HWND wnd, win_doc *d, int repaint)
{
    view_clamp(h, wnd, d);
    if (scroll_to_caret(h, wnd, d)) repaint = 1;
    view_scrollbars(h, wnd, d);
    caret_sync(h, wnd, d);
    if (repaint) InvalidateRect(wnd, NULL, FALSE);
    if (h->gutter && h->app.linenums) InvalidateRect(h->gutter, NULL, FALSE);
    update_status(h);
}

static void view_scroll_to(note_host *h, HWND wnd, win_doc *d,
                           int top, int sub, int xoff)
{
    note_buffer *b = &d->buf;
    int nlines = note_buffer_lines(b), wcols = wrap_cols(h, wnd);

    if (top > nlines - 1) top = nlines - 1;
    if (top < 0) top = 0;
    if (sub < 0 || wcols <= 0) sub = 0;
    else {
        int n = row_count(b, note_buffer_line_start(b, top),
                             note_buffer_line_len(b, top), wcols);
        if (sub >= n) sub = n - 1;
    }
    if (xoff < 0 || wcols > 0) xoff = 0;
    if (top == d->top && sub == d->sub && xoff == d->xoff) return;

    d->top  = top;
    d->sub  = sub;
    d->xoff = xoff;
    view_scrollbars(h, wnd, d);
    caret_sync(h, wnd, d);
    InvalidateRect(wnd, NULL, FALSE);
    if (h->gutter && h->app.linenums) InvalidateRect(h->gutter, NULL, FALSE);
}

/* The wheel, the scroll bar's arrows and its pages all move the view by rows,
 * so a paragraph wrapped across twenty of them scrolls through them instead of
 * jumping over the whole paragraph at once. */
static void view_scroll_rows(note_host *h, HWND wnd, win_doc *d, int n)
{
    note_buffer *b = &d->buf;
    int top = d->top, sub = d->sub, wcols = wrap_cols(h, wnd);

    if (n < 0) rows_back(b, &top, &sub, -n, wcols);
    else       rows_fwd (b, &top, &sub,  n, wcols, note_buffer_lines(b));
    view_scroll_to(h, wnd, d, top, sub, d->xoff);
}

/* -------------------------------------------------------------------------
 * Working the scroll bars
 *
 * The three gestures a classic bar answers: an arrow, a page, and the thumb.
 * An arrow or a page held down repeats, after the pause the system uses before
 * a key repeats and then at the rate it repeats at, so holding the pointer on
 * the bottom arrow runs the document past at the speed the rest of the desktop
 * runs at.  The thumb does not repeat: it follows the pointer.
 * ------------------------------------------------------------------------- */

#define SB_DELAY  300         /* before a held arrow or page starts repeating */
#define SB_REPEAT  60         /* and between repeats after that               */

/* One step of whatever is being held. */
static void sb_act(note_host *h, HWND wnd, win_doc *d, int horz, int part)
{
    int pos, page, total;

    sb_range(h, wnd, d, horz, &pos, &page, &total);

    if (!horz) {
        switch (part) {
        case SBP_LINEUP:   view_scroll_rows(h, wnd, d, -1);    break;
        case SBP_LINEDOWN: view_scroll_rows(h, wnd, d,  1);    break;
        case SBP_PAGEUP:   view_scroll_rows(h, wnd, d, -page); break;
        case SBP_PAGEDOWN: view_scroll_rows(h, wnd, d,  page); break;
        default: break;
        }
        return;
    }

    switch (part) {
    case SBP_LINEUP:   pos -= 1;    break;
    case SBP_LINEDOWN: pos += 1;    break;
    case SBP_PAGEUP:   pos -= page; break;
    case SBP_PAGEDOWN: pos += page; break;
    default: return;
    }
    if (pos > total - page) pos = total - page;
    if (pos < 0) pos = 0;
    view_scroll_to(h, wnd, d, d->top, d->sub, pos * h->char_w);
}

/* The thumb has been dragged to `at`, measured from the start of the bar. */
static void sb_drag_to(note_host *h, HWND wnd, win_doc *d, int at)
{
    RECT bar;
    int  horz = h->sb_horz;
    int  btn  = sb_btn_size(h, horz);
    int  len, track, tp, tl, pos, page, total, minlen, span;

    if (horz) sb_hrect(h, wnd, &bar); else sb_vrect(h, wnd, &bar);
    len = horz ? bar.right - bar.left : bar.bottom - bar.top;
    if (len <= btn * 2) return;

    track  = len - btn * 2;
    minlen = GetSystemMetrics(horz ? SM_CXHTHUMB : SM_CYVTHUMB);
    if (minlen <= 0) minlen = btn;
    sb_range(h, wnd, d, horz, &pos, &page, &total);
    if (!sb_thumb(track, minlen, pos, page, total, &tp, &tl)) return;

    span = total - page;
    at  -= (horz ? bar.left : bar.top) + btn + h->sb_grab;   /* thumb's top */
    if (track <= tl) pos = 0;
    else             pos = (int)((long)at * span / (track - tl));
    if (pos > span) pos = span;
    if (pos < 0)    pos = 0;

    if (horz) view_scroll_to(h, wnd, d, d->top, d->sub, pos * h->char_w);
    else      view_scroll_to(h, wnd, d, pos, 0, d->xoff);
}

/* Which bar a client point is in, or -1.  Vertical first: where they meet, the
 * square in the corner belongs to neither and answers -1 through both. */
static int sb_bar_at(note_host *h, HWND wnd, int x, int y)
{
    RECT r;

    if (!sb_own(h)) return -1;
    sb_vrect(h, wnd, &r);
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return 0;
    if (!sb_has_horz(h)) return -1;
    sb_hrect(h, wnd, &r);
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return 1;
    return -1;
}

static void sb_release(note_host *h)
{
    HWND wnd = h->sb_wnd;

    if (!wnd) return;
    KillTimer(wnd, TIMER_SB);
    h->sb_wnd  = NULL;
    h->sb_part = SBP_NONE;
    InvalidateRect(wnd, NULL, FALSE);
}

/* A press in one of the bars.  Answers 0 when the point was not in one, so the
 * caller can go on and treat it as a click in the text. */
static int sb_press(note_host *h, HWND wnd, win_doc *d, int x, int y)
{
    int horz = sb_bar_at(h, wnd, x, y);
    int part, tp, tl;

    if (horz < 0) return 0;

    part = sb_part_at(h, wnd, d, horz, x, y, &tp, &tl);
    if (part == SBP_NONE) return 1;      /* a dead bar still swallows the click */

    h->sb_wnd  = wnd;
    h->sb_horz = horz;
    h->sb_part = part;
    SetCapture(wnd);

    if (part == SBP_THUMB) {
        /* Where in the thumb it was taken hold of, so it does not jump under
         * the pointer on the first move. */
        RECT bar;
        int  btn = sb_btn_size(h, horz);
        if (horz) sb_hrect(h, wnd, &bar); else sb_vrect(h, wnd, &bar);
        h->sb_grab = (horz ? x - bar.left : y - bar.top) - btn - tp;
    } else {
        sb_act(h, wnd, d, horz, part);
        SetTimer(wnd, TIMER_SB, SB_DELAY, NULL);
    }
    InvalidateRect(wnd, NULL, FALSE);
    return 1;
}

/* -------------------------------------------------------------------------
 * Colour
 * ------------------------------------------------------------------------- */

static COLORREF curline_rgb(note_host *h)
{
    return mix_rgb(h->theme.fg, h->theme.bg, 1, 12);
}

static int hl_room(note_host *h, int want)
{
    nchar *grown;

    if (h->hl_cap >= want) return 1;
    grown = (nchar *)h_alloc(h, (unsigned long)want * sizeof(nchar));
    if (!grown) return 0;
    if (h->hl) h_free(h, h->hl);
    h->hl     = grown;
    h->hl_cap = want;
    return 1;
}

/* Lexes exactly what is on screen, every paint.
 *
 * The control had to be told about colour a character at a time and remembered
 * it afterwards, so the highlighter's job there was to extend a coloured range
 * and never redo it.  Drawing our own pixels inverts that: a screenful is a
 * couple of milliseconds to lex and the result is thrown away with the frame,
 * so there is nothing to invalidate, nothing to schedule, and no way for the
 * colours to disagree with the text.
 *
 * The run starts above the viewport because a block comment opened off the top
 * of the screen still colours what is on it.  note_syntax_safe_start finds a
 * point in that run the lexer can begin from cold, and note_syntax_advance
 * carries its state forward to the first visible line -- so the span array
 * only ever holds spans that are actually on screen.
 */
static int view_lex(note_host *h, win_doc *d, int vis_from, int vis_to)
{
    int lang = h->app.syntax ? h->app.docs[h->app.active].lang : LANG_NONE;
    int from, got, head, safe;
    note_syn_state st;

    if (lang <= LANG_NONE) return 0;
    if (vis_to - vis_from > VIEW_LEX_MAX) vis_to = vis_from + VIEW_LEX_MAX;
    if (vis_to <= vis_from) return 0;

    from = vis_from - SAFE_START_WINDOW;
    if (from < 0) from = 0;
    if (!hl_room(h, vis_to - from + 2)) return 0;

    got  = note_buffer_copy(&d->buf, from, vis_to - from, h->hl, h->hl_cap);
    head = vis_from - from;
    if (head > got) head = got;

    safe = note_syntax_safe_start(lang, h->hl, got, head, SAFE_START_WINDOW);
    st   = note_syntax_advance(lang, h->hl + safe, head - safe, NOTE_SYN_NORMAL);

    return note_tokenize_from(lang, h->hl + head, got - head, vis_from,
                              h->spans, MAX_SPANS, st, NULL);
}

/* Spans arrive sorted and the screen is painted in document order, so one
 * index walking forward answers every cell. */
static note_color tok_colour(note_host *h, int nspans, int *at, int pos)
{
    while (*at < nspans &&
           h->spans[*at].start + h->spans[*at].len <= pos) (*at)++;
    if (*at < nspans && h->spans[*at].start <= pos)
        return h->theme.tok[h->spans[*at].kind];
    return h->theme.fg;
}

/* -------------------------------------------------------------------------
 * Painting
 * ------------------------------------------------------------------------- */

/* One row, expanded.  Static rather than automatic because /Gs turns off the
 * stack probes a frame this size would need. */
static nchar row_ch[VIEW_COLS];
static int   row_pos[VIEW_COLS];

/* Fills the window [cbase, cbase+n) of one row into row_ch/row_pos and returns
 * how many columns of it were filled.  The whole row is walked either way,
 * which is what tells the caller how wide it is.
 *
 * `col0` is the column the run starts at and `cbase` the column drawn at the
 * left edge of the window.  They differ by the horizontal scroll with wrap off,
 * and are the same with wrap on -- a wrapped row is drawn at the margin however
 * many columns into its line it began. */
static int row_build(note_buffer *b, int start, int len, int col0, int cbase,
                     int n, int *width)
{
    int col = col0, i = 0, filled = 0;

    if (n > VIEW_COLS) n = VIEW_COLS;

    while (i < len) {
        int rl, k = 0;
        const nchar *q = note_buffer_span(b, start + i, &rl);
        if (rl <= 0) break;
        if (rl > len - i) rl = len - i;
        while (k < rl) {
            nchar c = q[k];
            int   w = (c == (nchar)'\t') ? VIEW_TAB - (col % VIEW_TAB) : 1;
            int   j;
            for (j = 0; j < w; j++) {
                int at = col + j - cbase;
                if (at >= 0 && at < n) {
                    row_ch[at]  = (c == (nchar)'\t') ? (nchar)' ' : c;
                    row_pos[at] = start + i + k;
                    if (at + 1 > filled) filled = at + 1;
                }
            }
            col += w;
            k++;
        }
        i += rl;
    }

    *width = col;
    return filled;
}

/* Grows the off-screen bitmap to cover the client area.  It only ever grows:
 * a window being dragged smaller and larger again should not cost a bitmap
 * per pixel of the drag. */
static void back_buffer(note_host *h, HWND wnd, const RECT *rc)
{
    HDC dc;

    if (h->back && h->back_w >= rc->right && h->back_h >= rc->bottom) return;

    if (h->back) {
        SelectObject(h->back, h->backold);
        DeleteObject(h->backbm);
        DeleteDC(h->back);
        h->back = 0;
    }

    dc = GetDC(wnd);
    h->back   = CreateCompatibleDC(dc);
    h->backbm = CreateCompatibleBitmap(dc, rc->right, rc->bottom);
    ReleaseDC(wnd, dc);
    if (!h->back || !h->backbm) {
        if (h->backbm) { DeleteObject(h->backbm); h->backbm = 0; }
        if (h->back)   { DeleteDC(h->back); h->back = 0; }
        return;
    }
    h->backold = (HBITMAP)SelectObject(h->back, h->backbm);
    h->back_w  = rc->right;
    h->back_h  = rc->bottom;
}

static void view_paint(note_host *h, HWND wnd, win_doc *d)
{
    PAINTSTRUCT ps;
    RECT   rc, cl, r;
    HDC    dc, target;
    HFONT  oldfont;
    HBRUSH br_bg, br_cur, br_sel;
    note_buffer *b = &d->buf;
    view_walk w;
    int rows, nlines, total, c0, cols, i, wcols;
    int vis_from, vis_to, nspans, span_at = 0;
    int hassel, sello, selhi, cline;

    dc = BeginPaint(wnd, &ps);
    /* The whole client for the buffer that is blitted back, the text area for
     * everything drawn into it: the bars go over the difference. */
    GetClientRect(wnd, &cl);
    view_client(h, wnd, &rc);
    if (cl.right <= 0 || cl.bottom <= 0) { EndPaint(wnd, &ps); return; }

    back_buffer(h, wnd, &cl);
    target = h->back ? h->back : dc;

    rows   = view_rows(h, wnd);
    wcols  = wrap_cols(h, wnd);
    nlines = note_buffer_lines(b);
    total  = note_buffer_len(b);
    c0     = h->char_w > 0 ? d->xoff / h->char_w : 0;
    cols   = h->char_w > 0 ? rc.right / h->char_w + 2 : 1;
    if (cols > VIEW_COLS) cols = VIEW_COLS;

    sello = note_buffer_sel_lo(b);
    selhi = note_buffer_sel_hi(b);
    hassel = (sello != selhi);
    cline = caret_line(d);

    /* A line per row, which is more lines than are on screen as soon as
     * anything is wrapped.  Erring long is free -- the extra spans are simply
     * never asked for -- where erring short would leave the bottom of the
     * window uncoloured. */
    vis_from = note_buffer_line_start(b, d->top);
    vis_to   = (d->top + rows < nlines)
             ? note_buffer_line_start(b, d->top + rows) : total;
    nspans   = view_lex(h, d, vis_from, vis_to);

    br_bg  = CreateSolidBrush(cr(h->theme.bg));
    br_cur = CreateSolidBrush(curline_rgb(h));
    br_sel = CreateSolidBrush(cr(h->theme.sel_bg));

    FillRect(target, &rc, br_bg);
    SetBkMode(target, TRANSPARENT);
    oldfont = (HFONT)SelectObject(target, h->viewfont);

    walk_at(b, &w, d->top, d->sub, wcols);

    for (i = 0; i < rows; i++) {
        int y     = i * h->line_h;
        int cbase = w.r.col + c0;
        int lend  = w.lstart + w.llen;
        int width, n, k, run;

        r.left = 0; r.right = rc.right;
        r.top  = y; r.bottom = y + h->line_h;

        /* The wash under the caret's line.  Every row of it, so a wrapped line
         * reads as one band.  Nothing while a selection is up: two overlapping
         * washes say less than either of them alone. */
        if (!hassel && w.line == cline) FillRect(target, &r, br_cur);

        n = row_build(b, w.r.off, w.r.end - w.r.off, w.r.col, cbase, cols,
                      &width);
        /* The scroll range is the widest line, which is only asked for when
         * nothing is wrapped; a wrapped row is as wide as the window by
         * construction and would say nothing about how far right to scroll. */
        if (wcols <= 0 && width > d->widest) d->widest = width;

        if (hassel) {
            /* The break at the end of the line is part of the selection when
             * the selection carries on past it, and showing that is what makes
             * a multi-line selection read as a block rather than as a stack of
             * separate runs. */
            int lo = -1, hi = -1;
            for (k = 0; k < n; k++) {
                if (row_pos[k] >= sello && row_pos[k] < selhi) {
                    if (lo < 0) lo = k;
                    hi = k + 1;
                }
            }
            if (lo >= 0) {
                r.left  = lo * h->char_w - (d->xoff - c0 * h->char_w);
                r.right = hi * h->char_w - (d->xoff - c0 * h->char_w);
                FillRect(target, &r, br_sel);
            }
            /* Only on the row the line actually ends on: the break is one
             * character and a wrapped line has no break at the end of its
             * earlier rows. */
            if (w.r.last && lend >= sello && lend < selhi) {
                int e = (width - cbase) * h->char_w - (d->xoff - c0 * h->char_w);
                if (e < 0) e = 0;
                r.left  = e;
                r.right = e + h->char_w;
                if (r.right > rc.right) r.right = rc.right;
                if (r.right > r.left) FillRect(target, &r, br_sel);
            }
            r.left = 0; r.right = rc.right;
        }

        /* Runs of one colour, drawn in one call each: a TextOut per character
         * is what made the control slow, and the row is already in an array. */
        run = 0;
        while (run < n) {
            note_color colour = tok_colour(h, nspans, &span_at, row_pos[run]);
            int end = run + 1;
            while (end < n &&
                   tok_colour(h, nspans, &span_at, row_pos[end]) == colour) end++;
            SetTextColor(target, cr(colour));
            TextOutW(target, run * h->char_w - (d->xoff - c0 * h->char_w), y,
                     (LPCWSTR)(row_ch + run), end - run);
            run = end;
        }

        if (!walk_next(b, &w, wcols, nlines)) break;
    }

    SelectObject(target, oldfont);
    sb_paint(h, wnd, d, target);

    if (target != dc)
        BitBlt(dc, 0, 0, cl.right, cl.bottom, target, 0, 0, SRCCOPY);

    DeleteObject(br_bg);
    DeleteObject(br_cur);
    DeleteObject(br_sel);
    EndPaint(wnd, &ps);
}

/* -------------------------------------------------------------------------
 * Editing
 * ------------------------------------------------------------------------- */

/* Every change of the text ends here: the document is dirty, the caret is
 * brought back into view, and everything that reads the text repaints. */
static void view_edited(note_host *h, HWND wnd, win_doc *d, int doc)
{
    note_set_dirty(&h->app, doc, 1);
    d->modified = 1;
    d->goal     = -1;
    d->rowend   = 0;
    gutter_width(h);
    view_refresh(h, wnd, d, 1);
}

static int view_insert(note_host *h, win_doc *d, const nchar *s, int len)
{
    if (len < 0) len = n_len(s);
    if (len <= 0) return 0;
    /* Room for the text and for the gap the next keystroke will want. */
    if (!view_room(h, d, note_buffer_len(&d->buf) + len + 1024)) return 0;
    return note_buffer_insert(&d->buf, s, len);
}

/* Backspace and Delete over a CRLF.  The buffer counts one as a single break,
 * so taking one half of it away would leave a lone CR that is still a break --
 * the line would not join, and the key would have done nothing visible. */
static int break_before(note_buffer *b, int pos)
{
    return pos >= 2 && note_buffer_at(b, pos - 1) == (nchar)'\n' &&
                       note_buffer_at(b, pos - 2) == (nchar)'\r';
}

static int break_after(note_buffer *b, int pos)
{
    return note_buffer_at(b, pos) == (nchar)'\r' &&
           note_buffer_at(b, pos + 1) == (nchar)'\n';
}

/* -------------------------------------------------------------------------
 * Words
 * ------------------------------------------------------------------------- */

static int word_class(nchar c)
{
    if (c == (nchar)' ' || c == (nchar)'\t') return 0;
    if (c == (nchar)'\r' || c == (nchar)'\n') return 0;
    if ((c >= (nchar)'0' && c <= (nchar)'9') ||
        (c >= (nchar)'A' && c <= (nchar)'Z') ||
        (c >= (nchar)'a' && c <= (nchar)'z') ||
        c == (nchar)'_' || c >= 128) return 1;
    return 2;
}

/* The next word boundary in `dir`.  Ctrl+Right steps over the run the caret is
 * in and any spaces after it, which is where the next word begins.
 *
 * A line break is a boundary of its own and the step stops on either side of
 * it, so walking by words down a file does not silently swallow the ends of
 * lines -- but a caret already against one steps over it, or Ctrl+Left at the
 * start of a line would do nothing at all. */
static int word_step(note_buffer *b, int pos, int dir)
{
    int total = note_buffer_len(b), k;

    if (dir > 0) {
        if (note_buffer_at(b, pos) == (nchar)'\r' ||
            note_buffer_at(b, pos) == (nchar)'\n')
            return pos + (break_after(b, pos) ? 2 : 1);

        k = word_class(note_buffer_at(b, pos));
        if (k) while (pos < total && word_class(note_buffer_at(b, pos)) == k) pos++;
        while (pos < total && word_class(note_buffer_at(b, pos)) == 0 &&
               note_buffer_at(b, pos) != (nchar)'\r' &&
               note_buffer_at(b, pos) != (nchar)'\n') pos++;
        return pos;
    }

    if (pos > 0 && (note_buffer_at(b, pos - 1) == (nchar)'\n' ||
                    note_buffer_at(b, pos - 1) == (nchar)'\r'))
        return pos - (break_before(b, pos) ? 2 : 1);

    while (pos > 0 && word_class(note_buffer_at(b, pos - 1)) == 0 &&
           note_buffer_at(b, pos - 1) != (nchar)'\n') pos--;
    k = word_class(note_buffer_at(b, pos - 1));
    if (k) while (pos > 0 && word_class(note_buffer_at(b, pos - 1)) == k) pos--;
    return pos;
}

static void word_at(note_buffer *b, int pos, int *from, int *to)
{
    int total = note_buffer_len(b), k = word_class(note_buffer_at(b, pos));

    if (!k && pos > 0) k = word_class(note_buffer_at(b, pos - 1));
    *from = pos;
    *to   = pos;
    if (!k) return;
    while (*from > 0 && word_class(note_buffer_at(b, *from - 1)) == k) (*from)--;
    while (*to < total && word_class(note_buffer_at(b, *to)) == k) (*to)++;
}

/* -------------------------------------------------------------------------
 * The clipboard
 * ------------------------------------------------------------------------- */

static void view_copy(note_host *h, win_doc *d, int cut)
{
    int lo = note_buffer_sel_lo(&d->buf), hi = note_buffer_sel_hi(&d->buf);
    HGLOBAL mem;

    if (lo >= hi || !OpenClipboard(h->wnd)) return;
    EmptyClipboard();

    mem = GlobalAlloc(GMEM_MOVEABLE, (DWORD)(hi - lo + 1) * sizeof(WCHAR));
    if (mem) {
        WCHAR *p = (WCHAR *)GlobalLock(mem);
        if (p) {
            note_buffer_copy(&d->buf, lo, hi - lo, (nchar *)p, hi - lo + 1);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
    }
    CloseClipboard();

    if (cut) note_buffer_delete(&d->buf, 0);
}

/* Pasted text arrives with whatever line endings it was copied with; the
 * document keeps CRLF throughout, the way the core hands it over after
 * loading a file, so the paste is normalised on the way in. */
static int view_paste(note_host *h, win_doc *d)
{
    HANDLE hnd;
    const WCHAR *src;
    nchar *out;
    int n = 0, len = 0, i, ok = 0, failed = 0;

    if (!OpenClipboard(h->wnd)) return 0;
    hnd = GetClipboardData(CF_UNICODETEXT);
    src = hnd ? (const WCHAR *)GlobalLock(hnd) : 0;

    if (src) {
        while (src[len]) len++;
        out = (nchar *)h_alloc(h, (unsigned long)(len * 2 + 2) * sizeof(nchar));
        if (out) {
            for (i = 0; i < len; i++) {
                if (src[i] == L'\r' || src[i] == L'\n') {
                    if (src[i] == L'\r' && src[i + 1] == L'\n') i++;
                    out[n++] = (nchar)'\r';
                    out[n++] = (nchar)'\n';
                } else {
                    out[n++] = (nchar)src[i];
                }
            }
            out[n] = 0;
            ok = view_insert(h, d, out, n);
            h_free(h, out);
            if (!ok && n > 0) failed = 1;
        }
        GlobalUnlock(hnd);
    }

    CloseClipboard();

    /* A paste is the one edit large enough that failing it silently would look
     * like the clipboard was empty.  Said after the clipboard is closed: a
     * message box holding it open is a good way to hang the next program that
     * wants it. */
    if (failed) h_message(h, N("Not enough memory for that paste."), N("note"));
    return ok;
}

/* -------------------------------------------------------------------------
 * Indenting
 *
 * Tab over a selection that spans lines moves all of them, and Shift+Tab moves
 * them back.  One insertion or deletion per line, all inside one undo group,
 * so the whole block comes back in one Ctrl+Z.
 * ------------------------------------------------------------------------- */

#define INDENT_SPACES 4

static int indent_lines(note_host *h, win_doc *d, int out)
{
    note_buffer *b = &d->buf;
    int lo = note_buffer_sel_lo(b), hi = note_buffer_sel_hi(b);
    int first = note_buffer_line_at(b, lo);
    int last  = note_buffer_line_at(b, hi > lo ? hi - 1 : lo);
    int i, did = 0, keep_lo = lo, keep_hi = hi;

    if (!out && first == last) return 0;    /* a plain tab, as it always was */

    /* The caret is moved by hand rather than through note_buffer_caret_set,
     * which ends the undo group every time it is called: this is one gesture
     * and has to come back in one Ctrl+Z, however many lines it touched. */
    note_buffer_break_undo(b);

    /* Backwards, so that changing one line does not move the starts of the
     * lines still to be done. */
    for (i = last; i >= first; i--) {
        int start = note_buffer_line_start(b, i);
        int len   = note_buffer_line_len(b, i);

        if (out) {
            int k = 0;
            if (len > 0 && note_buffer_at(b, start) == (nchar)'\t') k = 1;
            else while (k < INDENT_SPACES && k < len &&
                        note_buffer_at(b, start + k) == (nchar)' ') k++;
            if (!k) continue;
            b->caret = start;
            b->anchor = start + k;
            note_buffer_delete(b, 0);
            if (keep_lo > start) keep_lo -= k;
            keep_hi -= k;
            did = 1;
        } else {
            static const nchar tab[2] = { (nchar)'\t', 0 };
            if (len <= 0) continue;         /* nothing to indent on an empty line */
            b->caret = b->anchor = start;
            if (!view_insert(h, d, tab, 1)) break;
            if (keep_lo > start) keep_lo += 1;
            keep_hi += 1;
            did = 1;
        }
    }

    note_buffer_break_undo(b);
    /* 2 is "mine, and nothing came of it" -- a Shift+Tab on lines that are
     * already at the left margin.  The caller has to tell that from a change,
     * or the document would be marked edited for a key that did nothing. */
    if (!did) return 2;

    /* The same lines, still selected, so a second Tab keeps working on them. */
    if (keep_lo < 0) keep_lo = 0;
    if (keep_hi < keep_lo) keep_hi = keep_lo;
    b->anchor = keep_lo;
    b->caret  = keep_hi;
    return 1;
}

/* -------------------------------------------------------------------------
 * Keys
 * ------------------------------------------------------------------------- */

/* Moving the caret puts it at the start of whatever row its offset falls on;
 * the two keys that mean the end of a row say so afterwards. */
static void move_caret(note_host *h, HWND wnd, win_doc *d, int pos, int extend)
{
    note_buffer_caret_set(&d->buf, pos, extend);
    d->rowend = 0;
    (void)h; (void)wnd;
}

static int view_key(note_host *h, HWND wnd, win_doc *d, int doc, int vk)
{
    note_buffer *b = &d->buf;
    int shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    int ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    int total = note_buffer_len(b);
    int line, edited = 0;
    int keep_goal = 0;

    switch (vk) {
    case VK_LEFT:
        if (!shift && note_buffer_has_sel(b))
            move_caret(h, wnd, d, note_buffer_sel_lo(b), 0);
        else if (ctrl)
            move_caret(h, wnd, d, word_step(b, b->caret, -1), shift);
        else if (break_before(b, b->caret))
            move_caret(h, wnd, d, b->caret - 2, shift);
        else
            move_caret(h, wnd, d, b->caret - 1, shift);
        break;

    case VK_RIGHT:
        if (!shift && note_buffer_has_sel(b))
            move_caret(h, wnd, d, note_buffer_sel_hi(b), 0);
        else if (ctrl)
            move_caret(h, wnd, d, word_step(b, b->caret, 1), shift);
        else if (break_after(b, b->caret))
            move_caret(h, wnd, d, b->caret + 2, shift);
        else
            move_caret(h, wnd, d, b->caret + 1, shift);
        break;

    case VK_UP:
    case VK_DOWN:
    case VK_PRIOR:
    case VK_NEXT: {
        view_walk w;
        view_row  r;
        int wcols = wrap_cols(h, wnd), nlines = note_buffer_lines(b);
        int rows = view_rows(h, wnd) - 1, step, sub, pos;

        if (rows < 1) rows = 1;
        step = (vk == VK_UP) ? -1 : (vk == VK_DOWN) ? 1
             : (vk == VK_PRIOR) ? -rows : rows;

        sub = caret_row(d, wcols, &r, &line);
        /* The goal is the column within the row, not within the line: two rows
         * of the same wrapped line start at different columns and the caret is
         * meant to stay under itself on the screen. */
        if (d->goal < 0) d->goal = col_in_row(b, &r, b->caret) - r.col;
        keep_goal = 1;

        if (step < 0) rows_back(b, &line, &sub, -step, wcols);
        else          rows_fwd (b, &line, &sub,  step, wcols, nlines);

        walk_at(b, &w, line, sub, wcols);
        pos = pos_in_row(b, &w.r, w.r.col + d->goal);
        move_caret(h, wnd, d, pos, shift);
        if (!w.r.last && pos >= w.r.end) d->rowend = 1;

        /* Page Up and Page Down move the view by the same amount they move the
         * caret, so the row under it keeps its place on the screen. */
        if (vk == VK_PRIOR || vk == VK_NEXT) {
            int top = d->top, tsub = d->sub;
            if (step < 0) rows_back(b, &top, &tsub, -step, wcols);
            else          rows_fwd (b, &top, &tsub,  step, wcols, nlines);
            d->top = top;
            d->sub = tsub;
        }
        break;
    }

    case VK_HOME:
        if (ctrl) move_caret(h, wnd, d, 0, shift);
        else {
            view_row r;
            caret_row(d, wrap_cols(h, wnd), &r, &line);
            move_caret(h, wnd, d, r.off, shift);
        }
        break;

    case VK_END:
        if (ctrl) move_caret(h, wnd, d, total, shift);
        else {
            view_row r;
            caret_row(d, wrap_cols(h, wnd), &r, &line);
            move_caret(h, wnd, d, r.end, shift);
            /* The end of a wrapped row is the start of the next one; without
             * this the caret would appear to have jumped down a row instead of
             * moving to the right. */
            d->rowend = !r.last;
        }
        break;

    case VK_BACK:
        if (note_buffer_has_sel(b))      note_buffer_delete(b, 0);
        else if (break_before(b, b->caret)) note_buffer_delete(b, -2);
        else                             note_buffer_delete(b, -1);
        edited = 1;
        break;

    case VK_DELETE:
        if (note_buffer_has_sel(b))     note_buffer_delete(b, 0);
        else if (break_after(b, b->caret)) note_buffer_delete(b, 2);
        else                            note_buffer_delete(b, 1);
        edited = 1;
        break;

    case VK_TAB: {
        int r;
        if (ctrl) return 0;
        r = indent_lines(h, d, shift);
        if (!r) {
            static const nchar tab[2] = { (nchar)'\t', 0 };
            view_insert(h, d, tab, 1);
            r = 1;
        }
        /* The character the key would also have produced is still on its way
         * and would replace the block that was just indented. */
        h->eat_tab = 1;
        edited = (r == 1);
        break;
    }

    default:
        return 0;
    }

    if (!keep_goal) d->goal = -1;
    if (edited) view_edited(h, wnd, d, doc);
    else        view_refresh(h, wnd, d, 1);
    return 1;
}

/* -------------------------------------------------------------------------
 * The mouse
 * ------------------------------------------------------------------------- */

/* The offset under a point in the client area.
 *
 * The row is found by walking from the top of the screen rather than by
 * dividing: with wrap on there is no arithmetic from a y to a line.  The walk
 * is a screenful at most, and a drag that has run off the top or the bottom of
 * the window is a row or two beyond that.
 *
 * It also settles which side of a wrap point a click on one means, which is why
 * a hit test writes to the document: clicking past the end of a wrapped row is
 * that row's end and not the start of the next. */
static int pos_at_point(note_host *h, HWND wnd, win_doc *d, int x, int y)
{
    note_buffer *b = &d->buf;
    view_walk w;
    int wcols = wrap_cols(h, wnd), nlines = note_buffer_lines(b);
    int line = d->top, sub = d->sub, n, col, pos;

    n = (y >= 0) ? y / h->line_h : (y - h->line_h + 1) / h->line_h;
    if (n < 0) rows_back(b, &line, &sub, -n, wcols);
    else       rows_fwd (b, &line, &sub,  n, wcols, nlines);

    walk_at(b, &w, line, sub, wcols);
    col = (x + d->xoff) / h->char_w;
    if (x + d->xoff < 0) col = 0;

    pos = pos_in_row(b, &w.r, w.r.col + col);
    d->rowend = (!w.r.last && pos >= w.r.end);
    return pos;
}

/* Dragging after a double or triple click keeps selecting by that unit, which
 * is what makes "double click, drag" select whole words. */
static void drag_extend(note_host *h, HWND wnd, win_doc *d, int pos)
{
    note_buffer *b = &d->buf;
    int from = d->drag_from, to = d->drag_to;

    if (d->drag == 2) {
        int wf, wt;
        word_at(b, pos, &wf, &wt);
        if (wf < from) from = wf;
        if (wt > to)   to   = wt;
        if (pos < d->drag_from) { note_buffer_caret_set(b, to, 0);
                                  note_buffer_caret_set(b, from, 1); }
        else                    { note_buffer_caret_set(b, from, 0);
                                  note_buffer_caret_set(b, to, 1); }
    } else if (d->drag == 3) {
        int line  = note_buffer_line_at(b, pos);
        int ls    = note_buffer_line_start(b, line);
        int le    = ls + note_buffer_line_len(b, line);
        if (ls < from) from = ls;
        if (le > to)   to   = le;
        if (pos < d->drag_from) { note_buffer_caret_set(b, to, 0);
                                  note_buffer_caret_set(b, from, 1); }
        else                    { note_buffer_caret_set(b, from, 0);
                                  note_buffer_caret_set(b, to, 1); }
    } else {
        note_buffer_caret_set(b, pos, 1);
    }
    (void)h; (void)wnd;
}

static void view_lbutton(note_host *h, HWND wnd, win_doc *d, int x, int y)
{
    note_buffer *b = &d->buf;
    DWORD now = GetTickCount();
    int   pos = pos_at_point(h, wnd, d, x, y);
    int   near_last = (x - d->click_x < 4 && d->click_x - x < 4 &&
                       y - d->click_y < 4 && d->click_y - y < 4);

    /* Windows tells us about a double click; a triple is the click after one,
     * inside the same interval and in the same place. */
    if (near_last && now - d->click_at <= GetDoubleClickTime()) d->click_n++;
    else                                                        d->click_n = 1;
    d->click_at = now;
    d->click_x  = x;
    d->click_y  = y;

    SetFocus(wnd);
    SetCapture(wnd);

    if (d->click_n >= 3) {
        int line = note_buffer_line_at(b, pos);
        d->drag_from = note_buffer_line_start(b, line);
        d->drag_to   = d->drag_from + note_buffer_line_len(b, line);
        /* Including the break, so a dragged run of lines reads as whole rows
         * and Ctrl+X takes the line away rather than emptying it. */
        if (line + 1 < note_buffer_lines(b))
            d->drag_to = note_buffer_line_start(b, line + 1);
        d->drag = 3;
        note_buffer_caret_set(b, d->drag_from, 0);
        note_buffer_caret_set(b, d->drag_to, 1);
    } else if (d->click_n == 2) {
        word_at(b, pos, &d->drag_from, &d->drag_to);
        d->drag = 2;
        note_buffer_caret_set(b, d->drag_from, 0);
        note_buffer_caret_set(b, d->drag_to, 1);
    } else {
        d->drag      = 1;
        d->drag_from = d->drag_to = pos;
        note_buffer_caret_set(b, pos, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
    }

    d->goal = -1;
    view_refresh(h, wnd, d, 1);
}

/* -------------------------------------------------------------------------
 * IME
 *
 * Enough to put the composition window where the text is going.  The candidate
 * list and the composition string itself are still the IME's own -- inline
 * composition would mean holding a provisional run of text that is not in the
 * document yet, which the buffer has no notion of.
 *
 * Resolved by name rather than imported, the way the rest of this backend
 * treats anything that might not be there: an executable that fails to load
 * because a DLL is missing is worse than one that quietly does without.
 * ------------------------------------------------------------------------- */

typedef HIMC (WINAPI *PFN_IMM_GET)(HWND);
typedef BOOL (WINAPI *PFN_IMM_REL)(HWND, HIMC);
typedef BOOL (WINAPI *PFN_IMM_SETCW)(HIMC, COMPOSITIONFORM *);

static void ime_place(note_host *h, HWND wnd, win_doc *d)
{
    static PFN_IMM_GET   imm_get;
    static PFN_IMM_REL   imm_rel;
    static PFN_IMM_SETCW imm_setcw;
    static int probed;

    COMPOSITIONFORM cf;
    POINT pt;
    HIMC  imc;
    int   x, y;

    if (!probed) {
        HMODULE imm = os_library(N("imm32.dll"));
        probed = 1;
        if (imm) {
            imm_get   = (PFN_IMM_GET)  GetProcAddress(imm, "ImmGetContext");
            imm_rel   = (PFN_IMM_REL)  GetProcAddress(imm, "ImmReleaseContext");
            imm_setcw = (PFN_IMM_SETCW)GetProcAddress(imm,
                            "ImmSetCompositionWindow");
        }
    }
    if (!imm_get || !imm_setcw) return;

    caret_xy(h, wnd, d, &x, &y);
    pt.x = x;
    pt.y = y;

    imc = imm_get(wnd);
    if (!imc) return;
    memset(&cf, 0, sizeof(cf));
    cf.dwStyle      = CFS_POINT;
    cf.ptCurrentPos = pt;
    imm_setcw(imc, &cf);
    if (imm_rel) imm_rel(wnd, imc);
}

/* -------------------------------------------------------------------------
 * The window procedure
 * ------------------------------------------------------------------------- */

static LRESULT CALLBACK ViewProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    note_host *h = &g;
    win_doc   *d = doc_of(wnd);
    int        doc;

    if (!d) return os_defproc(wnd, msg, wp, lp);
    doc = index_of(wnd);

    switch (msg) {

    case WM_PAINT:
        view_paint(h, wnd, d);
        return 0;

    case WM_ERASEBKGND:
        return 1;                        /* WM_PAINT covers every pixel */

    /* A resize changes the wrap width, so every row below the top of the screen
     * is somewhere else -- including the row `sub` counts into.  The class is
     * CS_HREDRAW | CS_VREDRAW, so the repaint comes on its own. */
    case WM_SIZE:
        view_clamp(h, wnd, d);
        view_scrollbars(h, wnd, d);
        scroll_to_caret(h, wnd, d);
        caret_sync(h, wnd, d);
        if (h->gutter && h->app.linenums)
            InvalidateRect(h->gutter, NULL, FALSE);
        return 0;

    case WM_SETFOCUS:
        CreateCaret(wnd, NULL, px(2), h->line_h);
        caret_sync(h, wnd, d);
        ShowCaret(wnd);
        return 0;

    case WM_KILLFOCUS:
        HideCaret(wnd);
        DestroyCaret();
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            /* The text cursor belongs over the text.  Over a bar the pointer
             * is an arrow, as it is over every other scroll bar on the
             * desktop -- and the bars are inside this window's client area, so
             * nothing else is going to say so. */
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(wnd, &pt);
            SetCursor(sb_bar_at(h, wnd, pt.x, pt.y) >= 0
                      ? os_arrow_cursor() : os_cursor(OS_CURSOR_IBEAM));
            return TRUE;
        }
        break;

    case WM_KEYDOWN:
        if (view_key(h, wnd, d, doc, (int)wp)) return 0;
        break;

    case WM_CHAR: {
        nchar c = (nchar)os_wm_char((unsigned)wp);
        if (c == (nchar)'\t' && h->eat_tab) { h->eat_tab = 0; return 0; }
        h->eat_tab = 0;

        if (c == (nchar)'\r' || c == (nchar)'\n') {
            static const nchar eol[3] = { (nchar)'\r', (nchar)'\n', 0 };
            view_insert(h, d, eol, 2);
        } else if (c >= 32 || c == (nchar)'\t') {
            nchar one[2];
            one[0] = c; one[1] = 0;
            view_insert(h, d, one, 1);
        } else {
            return 0;                    /* Ctrl+letter, Esc, the bell */
        }
        view_edited(h, wnd, d, doc);
        return 0;
    }

    /* The result of a composition arrives as characters once the IME is
     * finished with them, which is exactly what WM_CHAR already handles. */
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION:
        ime_place(h, wnd, d);
        break;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        /* The bars are inside the client area, so this is where a click on one
         * of them arrives -- before anything treats it as a click in text. */
        if (sb_press(h, wnd, d, (short)LOWORD(lp), (short)HIWORD(lp)))
            return 0;
        view_lbutton(h, wnd, d, (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;

    case WM_MOUSEMOVE:
        /* Windows 95 has no TrackMouseEvent, so a caption button the
         * pointer left by coming straight down into the text is never
         * told it was left, and stays lit.  The editor is where that
         * pointer went, so the editor is what can say so.
         *
         * Guarded on cap_hot rather than repainted unconditionally: this
         * runs on every mouse move, and on a machine with a compositor it
         * is always already -1. */
        if (g.cap_hot >= 0) {
            g.cap_hot = -1;
            InvalidateRect(g.wnd, NULL, FALSE);
        }
        if (h->sb_wnd == wnd && h->sb_part == SBP_THUMB) {
            sb_drag_to(h, wnd, d, h->sb_horz ? (short)LOWORD(lp)
                                             : (short)HIWORD(lp));
            return 0;
        }
        if (d->drag && GetCapture() == wnd) {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            RECT rc;
            view_client(h, wnd, &rc);
            drag_extend(h, wnd, d, pos_at_point(h, wnd, d, x, y));
            /* Off the top or the bottom of the window keeps scrolling on its
             * own, so a selection can run past what is on screen. */
            if (y < 0 || y > rc.bottom) SetTimer(wnd, TIMER_DRAG, 60, NULL);
            else                        KillTimer(wnd, TIMER_DRAG);
            view_refresh(h, wnd, d, 1);
        }
        return 0;

    case WM_LBUTTONUP:
        if (h->sb_wnd == wnd) {
            sb_release(h);
            ReleaseCapture();
            return 0;
        }
        if (d->drag) {
            d->drag = 0;
            KillTimer(wnd, TIMER_DRAG);
            ReleaseCapture();
        }
        return 0;

    /* The capture can be taken away -- Alt+Tab, a message box -- and then the
     * button up that would have ended the press never arrives. */
    case WM_CAPTURECHANGED:
        if (h->sb_wnd == wnd) sb_release(h);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_SB) {
            POINT pt;
            int   tp, tl;
            if (h->sb_wnd != wnd) return 0;
            GetCursorPos(&pt);
            ScreenToClient(wnd, &pt);
            /* Only while the pointer is still on the part it went down on:
             * sliding off an arrow stops the repeat and sliding back resumes
             * it, which is how a scroll bar has always behaved. */
            if (sb_bar_at(h, wnd, pt.x, pt.y) == h->sb_horz &&
                sb_part_at(h, wnd, d, h->sb_horz, pt.x, pt.y, &tp, &tl)
                    == h->sb_part)
                sb_act(h, wnd, d, h->sb_horz, h->sb_part);
            SetTimer(wnd, TIMER_SB, SB_REPEAT, NULL);
            return 0;
        }
        if (wp == TIMER_DRAG) {
            POINT pt;
            RECT  rc;
            GetCursorPos(&pt);
            ScreenToClient(wnd, &pt);
            view_client(h, wnd, &rc);
            if (pt.y < 0)              view_scroll_rows(h, wnd, d, -1);
            else if (pt.y > rc.bottom) view_scroll_rows(h, wnd, d,  1);
            drag_extend(h, wnd, d, pos_at_point(h, wnd, d, pt.x, pt.y));
            view_refresh(h, wnd, d, 1);
            return 0;
        }
        break;

    case WM_MOUSEWHEEL: {
        UINT lines = os_wheel_lines();
        int  delta = (short)HIWORD(wp);
        if (lines == 0 || lines > 100) lines = VIEW_WHEEL_ROWS;
        view_scroll_rows(h, wnd, d, -delta * (int)lines / WHEEL_DELTA);
        return 0;
    }

    /* The system's scroll bars, on the frame that has them.  Nothing sends
     * these where the view draws its own: there is no control to send them. */
    case WM_VSCROLL: {
        SCROLLINFO si;
        int rows = view_rows(h, wnd), n = 0;

        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask  = SIF_TRACKPOS;
        GetScrollInfo(wnd, SB_VERT, &si);

        /* The arrows and the pages move by rows; the thumb is in lines,
         * because that is what the bar's range is measured in, and lands on the
         * first row of whatever line it is dropped on. */
        switch (LOWORD(wp)) {
        case SB_LINEUP:    n = -1; break;
        case SB_LINEDOWN:  n =  1; break;
        case SB_PAGEUP:    n = -rows; break;
        case SB_PAGEDOWN:  n =  rows; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            view_scroll_to(h, wnd, d, si.nTrackPos, 0, d->xoff);
            return 0;
        case SB_TOP:
            view_scroll_to(h, wnd, d, 0, 0, d->xoff);
            return 0;
        case SB_BOTTOM:
            view_scroll_to(h, wnd, d, note_buffer_lines(&d->buf) - 1, 0,
                           d->xoff);
            return 0;
        default: return 0;
        }
        view_scroll_rows(h, wnd, d, n);
        return 0;
    }

    case WM_HSCROLL: {
        SCROLLINFO si;
        RECT rc;
        int  cols, x = d->xoff / h->char_w;

        view_client(h, wnd, &rc);
        cols = rc.right / h->char_w;
        if (cols < 1) cols = 1;

        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask  = SIF_TRACKPOS;
        GetScrollInfo(wnd, SB_HORZ, &si);

        switch (LOWORD(wp)) {
        case SB_LINELEFT:  x--; break;
        case SB_LINERIGHT: x++; break;
        case SB_PAGELEFT:  x -= cols; break;
        case SB_PAGERIGHT: x += cols; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: x = si.nTrackPos; break;
        default: return 0;
        }
        if (x < 0) x = 0;
        view_scroll_to(h, wnd, d, d->top, d->sub, x * h->char_w);
        return 0;
    }

    case WM_CONTEXTMENU: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (x == -1 && y == -1) {
            RECT rc; GetWindowRect(wnd, &rc);
            x = rc.left + 8; y = rc.top + 8;
        }
        TrackPopupMenu(g.ctxmenu, TPM_RIGHTBUTTON, x, y, 0, g.wnd, NULL);
        return 0;
    }

    case WM_NCDESTROY:
        if (d->text)  h_free(h, d->text);
        if (d->undo)  h_free(h, d->undo);
        if (d->utext) h_free(h, d->utext);
        if (d->lidx)  h_free(h, d->lidx);
        d->text = 0; d->undo = 0; d->utext = 0; d->lidx = 0;
        d->text_cap = 0;
        break;
    }

    return os_defproc(wnd, msg, wp, lp);
}

/* -------------------------------------------------------------------------
 * Documents
 * ------------------------------------------------------------------------- */

int h_tab_create(note_host *h, int doc)
{
    static int registered;
    win_doc *d;
    HWND e;

    if (doc < 0 || doc >= NOTE_MAX_DOCS) return 0;
    d = &h->d[doc];

    if (!registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = ViewProc;
        wc.hInstance     = h->inst;
        wc.hCursor       = os_cursor(OS_CURSOR_IBEAM);
        wc.lpszClassName = kViewClass;
        if (!os_register_class(&wc)) return 0;
        registered = 1;
    }

    d->text_cap = VIEW_TEXT_MIN;
    d->text  = (nchar *)h_alloc(h, (unsigned long)d->text_cap * sizeof(nchar));
    d->undo  = (note_edit *)h_alloc(h, VIEW_UNDO_RECS * sizeof(note_edit));
    d->utext = (nchar *)h_alloc(h, VIEW_UNDO_TEXT * sizeof(nchar));
    d->lidx  = (int *)h_alloc(h, VIEW_LINE_INTS * sizeof(int));
    if (!d->text) return 0;

    /* A missing ring is not an error: the buffer simply does without that
     * facility, which is better than refusing to open the document. */
    note_buffer_init(&d->buf, d->text, d->text_cap,
                     d->undo,  d->undo ? VIEW_UNDO_RECS : 0,
                     d->utext, d->utext ? VIEW_UNDO_TEXT : 0,
                     d->lidx,  d->lidx ? VIEW_LINE_INTS : 0);

    d->top = d->sub = d->xoff = d->widest = 0;
    d->rowend = 0;
    d->goal = -1;

    /* The system's scroll bars, or none at all because the view draws its own.
     * frame_custom() has already settled which frame this is, so the styles a
     * window is created with are the ones it keeps. */
    e = os_create_window(0, (const nchar *)kViewClass, N(""),
                         WS_CHILD | (sb_own(h) ? 0
                                               : (WS_VSCROLL | WS_HSCROLL)),
                         0, 0, 0, 0, h->wnd,
                         (HMENU)(UINT_PTR)(ID_EDIT0 + doc), h->inst, NULL);
    if (!e) return 0;

    d->edit    = e;
    d->title[0] = 0;
    view_font_sync(h, 0);
    /* A window made after the theme was applied still has to be told about it:
     * whose scroll bars these are, and which way the system should paint them
     * if they are its. */
    view_sb_sync(h);

    tabs_layout(h);
    if (h->tabs) InvalidateRect(h->tabs, NULL, FALSE);
    return 1;
}

/* -------------------------------------------------------------------------
 * Host ops -- text
 *
 * h_alloc and h_free stay in win32_edit.c: the heap never knew what the text
 * was, so it is one of the few things the switch does not touch.
 * ------------------------------------------------------------------------- */

int edit_len(HWND e)
{
    win_doc *d = e ? doc_of(e) : 0;
    return d ? note_buffer_len(&d->buf) : 0;
}

int h_text_len(note_host *h, int doc)
{
    if (doc < 0 || doc >= NOTE_MAX_DOCS || !h->d[doc].edit) return 0;
    return note_buffer_len(&h->d[doc].buf);
}

int h_text_get(note_host *h, int doc, nchar *buf, int cap)
{
    if (doc < 0 || doc >= NOTE_MAX_DOCS || !h->d[doc].edit) {
        if (cap > 0) buf[0] = 0;
        return 0;
    }
    return note_buffer_copy(&h->d[doc].buf, 0,
                            note_buffer_len(&h->d[doc].buf), buf, cap);
}

void h_text_set(note_host *h, int doc, const nchar *s)
{
    win_doc *d;
    int len;

    if (doc < 0 || doc >= NOTE_MAX_DOCS || !h->d[doc].edit) return;
    d   = &h->d[doc];
    len = n_len(s);

    /* Room for the file and for editing it afterwards, in one grow rather
     * than in a doubling per paragraph typed into a freshly opened file. */
    if (!view_room(h, d, len + len / 4 + 1024)) {
        h_message(h, N("Not enough memory for that file."), N("note"));
        return;
    }
    note_buffer_set(&d->buf, s);

    d->top = d->sub = d->xoff = d->widest = 0;
    d->rowend = 0;
    d->goal = -1;
    d->modified = 0;
    if (h->gutter) InvalidateRect(h->gutter, NULL, FALSE);
    InvalidateRect(d->edit, NULL, FALSE);
    if (doc == h->app.active) queue_view(h);
}

int edit_text_range(note_host *h, int from, int to, nchar *dst, int cap)
{
    win_doc *d = active_doc(h);
    if (!d) { if (cap > 0) dst[0] = 0; return 0; }
    return note_buffer_copy(&d->buf, from, to - from, dst, cap);
}

/* -------------------------------------------------------------------------
 * Host ops -- selection and editing
 * ------------------------------------------------------------------------- */

void h_sel_get(note_host *h, int *from, int *to)
{
    win_doc *d = active_doc(h);
    if (!d) { *from = *to = 0; return; }
    *from = note_buffer_sel_lo(&d->buf);
    *to   = note_buffer_sel_hi(&d->buf);
}

void h_sel_set(note_host *h, int from, int to)
{
    win_doc *d = active_doc(h);
    int total;

    if (!d) return;
    total = note_buffer_len(&d->buf);
    /* Select All arrives as (0, -1): the control's way of saying "to the end",
     * which the core has adopted. */
    if (to < 0) to = total;
    if (from < 0) from = 0;

    note_buffer_caret_set(&d->buf, from, 0);
    note_buffer_caret_set(&d->buf, to, 1);
    d->goal = -1;
    view_refresh(h, d->edit, d, 1);
}

void h_sel_replace(note_host *h, const nchar *s)
{
    win_doc *d = active_doc(h);
    if (!d) return;
    view_insert(h, d, s, -1);
    view_edited(h, d->edit, d, h->app.active);
}

void h_edit_op(note_host *h, int cmd)
{
    win_doc *d = active_doc(h);
    int edited = 0;

    if (!d) return;

    switch (cmd) {
    case CMD_EDIT_UNDO:   edited = note_buffer_undo(&d->buf); break;
    case CMD_EDIT_REDO:   edited = note_buffer_redo(&d->buf); break;
    case CMD_EDIT_COPY:   view_copy(h, d, 0); break;
    case CMD_EDIT_CUT:
        edited = note_buffer_has_sel(&d->buf);
        view_copy(h, d, 1);
        break;
    case CMD_EDIT_PASTE:  edited = view_paste(h, d); break;
    case CMD_EDIT_DELETE:
        if (note_buffer_has_sel(&d->buf))          note_buffer_delete(&d->buf, 0);
        else if (break_after(&d->buf, d->buf.caret)) note_buffer_delete(&d->buf, 2);
        else                                       note_buffer_delete(&d->buf, 1);
        edited = 1;
        break;
    }

    if (edited) view_edited(h, d->edit, d, h->app.active);
    else        view_refresh(h, d->edit, d, 1);
}

int h_can_undo(note_host *h)
{
    win_doc *d = active_doc(h);
    return d ? (d->buf.undo_count > 0) : 0;
}

void h_set_modified(note_host *h, int doc, int modified)
{
    if (doc < 0 || doc >= NOTE_MAX_DOCS) return;
    h->d[doc].modified = modified;
    if (!modified) note_buffer_break_undo(&h->d[doc].buf);
}

void edit_show_caret(note_host *h)
{
    win_doc *d = active_doc(h);
    if (!d) return;
    view_refresh(h, d->edit, d, 1);
}

void edit_status_pos(note_host *h, int pos, int *line, int *col)
{
    win_doc *d = active_doc(h);
    int l;

    *line = 1;
    *col  = 1;
    if (!d) return;
    l = note_buffer_line_at(&d->buf, pos);
    *line = l + 1;
    *col  = pos - note_buffer_line_start(&d->buf, l) + 1;
}

/* -------------------------------------------------------------------------
 * Find and go to
 * ------------------------------------------------------------------------- */

int h_find_text(note_host *h, const nchar *needle, unsigned flags)
{
    win_doc *d = active_doc(h);
    int from, at, len;

    if (!d) return 0;
    len = n_len(needle);
    if (!len) return 0;

    from = (flags & FIND_DOWN) ? note_buffer_sel_hi(&d->buf)
                               : note_buffer_sel_lo(&d->buf) - 1;
    at = note_buffer_find(&d->buf, needle, from, flags);
    if (at < 0) return 0;

    h_sel_set(h, at, at + len);
    return 1;
}

void h_goto_line(note_host *h, int line)
{
    win_doc *d = active_doc(h);
    int at;

    if (!d) return;
    if (line < 1) line = 1;
    if (line > note_buffer_lines(&d->buf)) line = note_buffer_lines(&d->buf);
    at = note_buffer_line_start(&d->buf, line - 1);
    h_sel_set(h, at, at);
    SetFocus(d->edit);
}

/* -------------------------------------------------------------------------
 * Printing
 *
 * The control could be asked to lay itself out onto a printer DC; a view that
 * owns its pixels has to say what goes on the page.  One line per line, at the
 * document's own font, an inch of margin, and a line wider than the paper is
 * clipped rather than wrapped -- which is what the screen does with it too.
 * ------------------------------------------------------------------------- */

void h_dlg_print(note_host *h)
{
    win_doc  *d = active_doc(h);
    PRINTDLGW pd;
    HFONT     font, oldfont;
    TEXTMETRICW tm;
    LOGFONTW  lf;
    nchar    *line;
    const nchar *docname;
    int logy, physh, offy, top, bottom, left, rowh, y, i, nlines, cap;

    if (!d) return;

    memset(&pd, 0, sizeof(pd));
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner   = h->wnd;
    pd.Flags       = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION;
    pd.nCopies     = 1;
    if (!os_print_dlg(&pd) || !pd.hDC) return;

    logy  = GetDeviceCaps(pd.hDC, LOGPIXELSY);
    physh = GetDeviceCaps(pd.hDC, PHYSICALHEIGHT);
    offy  = GetDeviceCaps(pd.hDC, PHYSICALOFFSETY);

    lf = h->font;
    lf.lfHeight = -MulDiv(h->fontpt > 0 ? h->fontpt : 110, logy, 720);
    lf.lfWidth  = 0;
    font = os_font(&lf);
    oldfont = (HFONT)SelectObject(pd.hDC, font);
    os_text_metrics(pd.hDC, &tm);
    rowh = tm.tmHeight;
    if (rowh <= 0) rowh = logy / 6;

    /* An inch all round, measured from the paper rather than from the area
     * the printer can reach, minus whatever it cannot reach. */
    left   = logy - GetDeviceCaps(pd.hDC, PHYSICALOFFSETX);
    top    = logy - offy;
    if (left < 0) left = 0;
    if (top  < 0) top  = 0;
    bottom = physh - offy - logy;

    cap  = 1024;
    line = (nchar *)h_alloc(h, (unsigned long)cap * sizeof(nchar));

    docname = h->app.docs[h->app.active].path[0]
              ? note_basename(h->app.docs[h->app.active].path)
              : N("Untitled");

    nlines = note_buffer_lines(&d->buf);

    if (line && os_start_doc(pd.hDC, docname) > 0) {
        y = top;
        if (StartPage(pd.hDC) > 0) {
            for (i = 0; i < nlines; i++) {
                int start = note_buffer_line_start(&d->buf, i);
                int len   = note_buffer_line_len(&d->buf, i);
                int n;

                if (y + rowh > bottom) {
                    if (EndPage(pd.hDC) <= 0) break;
                    if (StartPage(pd.hDC) <= 0) break;
                    y = top;
                }
                n = note_buffer_copy(&d->buf, start, len, line, cap);
                if (n > 0) TextOutW(pd.hDC, left, y, (LPCWSTR)line, n);
                y += rowh;
            }
            EndPage(pd.hDC);
        }
        EndDoc(pd.hDC);
    }

    if (line) h_free(h, line);
    SelectObject(pd.hDC, oldfont);
    DeleteObject(font);
    DeleteDC(pd.hDC);
    if (pd.hDevMode)  GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
}

/* -------------------------------------------------------------------------
 * The gutter
 *
 * Line numbers, and the caret line's wash carried across from the text so the
 * two read as one band.
 *
 * Numbers count lines, not rows: a wrapped line is numbered once, on the row it
 * begins on, and its continuations are blank.  That is what the RICHEDIT path
 * does -- see para_number in win32_edit.c -- and it is what a line number is
 * for, since Go To, the status bar and the compiler all count the same lines.
 *
 * So the gutter walks rows exactly as the paint loop does, from the same
 * (top, sub) and at the same wrap width, and prints a number when the row it is
 * on is the first of its line.  Two walks of a screenful rather than one shared
 * one, because the two windows paint independently and neither may assume the
 * other has been here first.
 * ------------------------------------------------------------------------- */

void gutter_width(note_host *h)
{
    win_doc *d = active_doc(h);
    int lines, digits = 3, w;

    if (!h->app.linenums) { h->gutter_w = 0; return; }

    lines = d ? note_buffer_lines(&d->buf) : 1;
    while (lines >= 1000) { lines /= 10; digits++; }

    w = h->char_w * (digits + 1) + px(8);
    if (w < px(28)) w = px(28);
    h->gutter_w = w;
}

static void gutter_paint(note_host *h, HWND wnd)
{
    PAINTSTRUCT ps;
    HDC   dc;
    RECT  rc, tr;
    HFONT old;
    HBRUSH br_cur;
    win_doc *d = active_doc(h);
    view_walk w;
    int   nlines, cline, wcols, y;

    dc = BeginPaint(wnd, &ps);
    GetClientRect(wnd, &rc);
    FillRect(dc, &rc, h->br_gutter);

    if (!d) { EndPaint(wnd, &ps); return; }

    nlines = note_buffer_lines(&d->buf);
    cline  = note_buffer_has_sel(&d->buf) ? -1 : caret_line(d);
    wcols  = wrap_cols(h, d->edit);
    br_cur = CreateSolidBrush(curline_rgb(h));

    SetBkMode(dc, TRANSPARENT);
    old = (HFONT)SelectObject(dc, h->viewfont ? h->viewfont : h->uifont);

    walk_at(&d->buf, &w, d->top, d->sub, wcols);

    for (y = 0; y < rc.bottom; y += h->line_h) {
        nchar num[12];
        int   n;

        tr.left = 0;  tr.right  = rc.right - 6;
        tr.top  = y;  tr.bottom = y + h->line_h;

        if (w.line == cline) {
            RECT band = tr;
            band.right = rc.right;
            FillRect(dc, &band, br_cur);
        }

        if (w.r.off == w.lstart) {
            n = n_utoa((unsigned)(w.line + 1), num);
            SetTextColor(dc, cr(w.line == cline ? h->theme.fg
                                                : h->theme.gutter_fg));
            os_draw_text(dc, num, n, &tr,
                         DT_RIGHT | DT_TOP | DT_SINGLELINE);
        }

        if (!walk_next(&d->buf, &w, wcols, nlines)) break;
    }

    SelectObject(dc, old);
    DeleteObject(br_cur);
    EndPaint(wnd, &ps);
}

LRESULT CALLBACK GutterProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT)      { gutter_paint(&g, wnd); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return os_defproc(wnd, msg, wp, lp);
}

/* -------------------------------------------------------------------------
 * The view refresh the rest of the backend asks for
 *
 * The highlighter's schedule went with the control: colours are worked out
 * during the paint that shows them, so there is nothing to invalidate and
 * nothing to spread across idle time.  The three hl_* entry points stay
 * because callers elsewhere still name them, and they now have nothing to do.
 * ------------------------------------------------------------------------- */

void hl_invalidate(note_host *h, int doc) { (void)h; (void)doc; }
void hl_touch(note_host *h, int doc, int off) { (void)h; (void)doc; (void)off; }
void hl_step(note_host *h) { KillTimer(h->wnd, TIMER_HL); }

/* Likewise the caret line: it is a FillRect during the paint, not a background
 * applied to the characters the caret happens to be among. */
void curline_update(note_host *h) { (void)h; }

void queue_view(note_host *h)
{
    if (h->view_pending) return;
    h->view_pending = 1;
    SetTimer(h->wnd, TIMER_VIEW, 40, NULL);
}

void service_view(note_host *h)
{
    win_doc *d = active_doc(h);

    h->view_pending = 0;
    KillTimer(h->wnd, TIMER_VIEW);

    view_font_sync(h, 0);
    gutter_width(h);
    relayout(h);

    if (d) {
        /* The caret is as tall as a line, so a change of font or of zoom means
         * a new one: its height is fixed when it is created. */
        if (GetFocus() == d->edit) {
            DestroyCaret();
            CreateCaret(d->edit, NULL, px(2), h->line_h);
            ShowCaret(d->edit);
        }
        /* A narrower character means more of them per row, so the row `sub`
         * counts into may no longer exist. */
        view_clamp(h, d->edit, d);
        view_scrollbars(h, d->edit, d);
        caret_sync(h, d->edit, d);
        InvalidateRect(d->edit, NULL, FALSE);
    }
    if (h->gutter) InvalidateRect(h->gutter, NULL, FALSE);
}

void h_rehighlight(note_host *h)
{
    queue_view(h);
}

void apply_font_to(HWND e, note_host *h)
{
    view_font_sync(h, 1);
    if (e) InvalidateRect(e, NULL, FALSE);
}

/* Turning wrap on and off is a change of layout and not of text: nothing is
 * reloaded, and the line at the top of each window stays at the top.  Which row
 * of it was showing does not survive -- with wrap off there is only one -- so
 * the view lands on the start of that line, which is where it would have been
 * had the document been opened this way.
 *
 * Every document, not just the active one: the flag is the application's, so a
 * tab switched to later must already agree with it. */
void h_set_wrap(note_host *h, int wrap)
{
    int i;

    for (i = 0; i < NOTE_MAX_DOCS; i++) {
        win_doc *d = &h->d[i];
        if (!d->edit) continue;
        d->sub    = 0;
        d->goal   = -1;
        d->rowend = 0;
        if (wrap) {
            d->xoff   = 0;
            d->widest = 0;
        }
        view_scrollbars(h, d->edit, d);
        InvalidateRect(d->edit, NULL, FALSE);
    }
    queue_view(h);
}

void h_set_zoom(note_host *h, int percent)
{
    (void)percent;
    view_font_sync(h, 1);
    queue_view(h);
}

#endif /* NOTE_OWN_VIEW */
