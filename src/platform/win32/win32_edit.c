/* win32_edit.c -- the RICHEDIT control, the line-number gutter and the highlighter
 *
 * Part of note's Win32 backend.  The shared state and the types every part of
 * it needs live in note_win32.h; see there for why the state is a header and
 * not a private static.
 *
 * Everything here that talks to the control is compiled only when the build
 * is on the RICHEDIT path.  With NOTE_OWN_VIEW the same jobs -- the text, the
 * selection, the gutter, the colouring -- are done by win32_view.c over the
 * core's own buffer, and none of this is reachable.  The heap and the status
 * bar are outside the switch because neither ever knew what the text was.
 */

#include "note_win32.h"

/* -------------------------------------------------------------------------
 * Host ops — memory, text, selection
 * ------------------------------------------------------------------------- */

void *h_alloc(note_host *h, unsigned long bytes)
{
    return HeapAlloc(h->heap, 0, bytes ? bytes : 1);
}

void h_free(note_host *h, void *p)
{
    if (p) HeapFree(h->heap, 0, p);
}

#if !NOTE_OWN_VIEW

int edit_len(HWND e)
{
    GETTEXTLENGTHEX gtl;
    if (!e) return 0;
    /* No GTL_USECRLF: RichEdit stores a paragraph break as a single CR, and
     * every offset we exchange with it — EM_LINEINDEX, EM_EXSETSEL, the lexer
     * spans — counts it as one.  Asking for CRLF here would count two and
     * drift the cache out of step with the control after the first line. */
    gtl.flags = GTL_DEFAULT | GTL_PRECISE | GTL_NUMCHARS;
    gtl.codepage = 1200;
    return (int)SendMessageW(e, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
}

int h_text_len(note_host *h, int doc)
{
    if (doc < 0 || doc >= NOTE_MAX_DOCS) return 0;
    return edit_len(h->d[doc].edit);
}

int h_text_get(note_host *h, int doc, nchar *buf, int cap)
{
    GETTEXTEX gt;
    if (doc < 0 || doc >= NOTE_MAX_DOCS || !h->d[doc].edit) { buf[0] = 0; return 0; }
    gt.cb = (DWORD)(cap * (int)sizeof(WCHAR));
    gt.flags = 0;             /* raw, single-CR breaks — see edit_len() */
    gt.codepage = 1200;
    gt.lpDefaultChar = NULL;
    gt.lpUsedDefChar = NULL;
    return (int)SendMessageW(h->d[doc].edit, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)buf);
}

void h_text_set(note_host *h, int doc, const nchar *s)
{
    SETTEXTEX st;
    if (doc < 0 || doc >= NOTE_MAX_DOCS || !h->d[doc].edit) return;
    st.flags = ST_DEFAULT;
    st.codepage = 1200;
    h->suppress++;
    SendMessageW(h->d[doc].edit, EM_SETTEXTEX, (WPARAM)&st, (LPARAM)s);
    h->suppress--;
    SendMessageW(h->d[doc].edit, EM_EMPTYUNDOBUFFER, 0, 0);
    hl_invalidate(h, doc);
    if (doc == h->app.active) h->cache_valid = 0;
}

void h_sel_get(note_host *h, int *from, int *to)
{
    CHARRANGE c;
    (void)h;
    SendMessageW(active_edit(), EM_EXGETSEL, 0, (LPARAM)&c);
    *from = c.cpMin; *to = c.cpMax;
}

void h_sel_set(note_host *h, int from, int to)
{
    CHARRANGE c;
    (void)h;
    c.cpMin = from; c.cpMax = to;
    SendMessageW(active_edit(), EM_EXSETSEL, 0, (LPARAM)&c);
}

void h_sel_replace(note_host *h, const nchar *s)
{
    (void)h;
    SendMessageW(active_edit(), EM_REPLACESEL, TRUE, (LPARAM)s);
}

void h_edit_op(note_host *h, int cmd)
{
    HWND e = active_edit();
    (void)h;
    switch (cmd) {
    case CMD_EDIT_UNDO:   SendMessageW(e, WM_UNDO, 0, 0); break;
    case CMD_EDIT_REDO:   SendMessageW(e, EM_REDO, 0, 0); break;
    case CMD_EDIT_CUT:    SendMessageW(e, WM_CUT,   0, 0); break;
    case CMD_EDIT_COPY:   SendMessageW(e, WM_COPY,  0, 0); break;
    case CMD_EDIT_PASTE:  SendMessageW(e, WM_PASTE, 0, 0); break;
    case CMD_EDIT_DELETE: SendMessageW(e, WM_CLEAR, 0, 0); break;
    }
}

int h_can_undo(note_host *h)
{
    (void)h;
    return (int)SendMessageW(active_edit(), EM_CANUNDO, 0, 0);
}

void h_set_modified(note_host *h, int doc, int modified)
{
    if (doc < 0 || doc >= NOTE_MAX_DOCS || !h->d[doc].edit) return;
    SendMessageW(h->d[doc].edit, EM_SETMODIFY, (WPARAM)(modified ? TRUE : FALSE), 0);
}

void edit_show_caret(note_host *h)
{
    (void)h;
    SendMessageW(active_edit(), EM_SCROLLCARET, 0, 0);
}

int edit_text_range(note_host *h, int from, int to, nchar *dst, int cap)
{
    TEXTRANGEW tr;
    (void)h;
    if (cap > 0) dst[0] = 0;
    if (to - from + 1 > cap) to = from + cap - 1;
    tr.chrg.cpMin = from;
    tr.chrg.cpMax = to;
    tr.lpstrText  = (LPWSTR)dst;
    return (int)SendMessageW(active_edit(), EM_GETTEXTRANGE, 0, (LPARAM)&tr);
}

/* -------------------------------------------------------------------------
 * Layout, gutter and highlighting
 * ------------------------------------------------------------------------- */

void relayout(note_host *h);
void queue_view(note_host *h);
void refresh_cache(note_host *h);
void menu_set_brush(HMENU m, HBRUSH br);
COLORREF blend_rgb(note_color a, note_color b);
/* The command palette, further down: the ops table needs to open it in one of
 * its list modes long before the overlay itself is defined. */
void pal_open_mode(note_host *h, int mode);
void measure_font(note_host *h);

/* Line breaks as the control stores them: a lone CR, but tolerate LF and CRLF
 * so a stray sequence never throws the numbering off. */
static int is_break(nchar c)
{
    return c == (nchar)'\r' || c == (nchar)'\n';
}

static int para_starts_at(const nchar *t, int len, int at)
{
    if (at <= 0) return 1;
    if (!t || at > len) return 0;
    return is_break(t[at - 1]);
}

/* Which paragraph the character at `upto` belongs to, counting from 1. */
int para_number(const nchar *t, int len, int upto)
{
    int i, n = 1;
    if (!t) return 1;
    if (upto > len) upto = len;
    for (i = 0; i < upto; i++) {
        if (t[i] == (nchar)'\r') {
            n++;
            if (i + 1 < upto && t[i + 1] == (nchar)'\n') i++;
        } else if (t[i] == (nchar)'\n') {
            n++;
        }
    }
    return n;
}

/* Character index of the first character of the given display line. */
static int line_start(HWND e, int line)
{
    return (int)SendMessageW(e, EM_LINEINDEX, (WPARAM)line, 0);
}

void gutter_width(note_host *h)
{
    int lines, digits = 3, w;

    if (!h->app.linenums) { h->gutter_w = 0; return; }

    lines = (int)SendMessageW(active_edit(), EM_GETLINECOUNT, 0, 0);
    while (lines >= 1000) { lines /= 10; digits++; }

    w = h->char_w * (digits + 1) + px(8);
    if (w < px(28)) w = px(28);
    h->gutter_w = w;
}

static void gutter_paint(HWND wnd)
{
    PAINTSTRUCT ps;
    HDC   dc;
    RECT  rc;
    CHARRANGE sel;
    HWND  e = active_edit();
    int   first, y, y0, rowh, para, i, caret_para;
    HFONT old;

    dc = BeginPaint(wnd, &ps);
    GetClientRect(wnd, &rc);
    FillRect(dc, &rc, g.br_gutter);

    if (!e) { EndPaint(wnd, &ps); return; }

    refresh_cache(&g);

    /* The paragraph the caret is in, so its number can be lit and the rest
     * left quiet.  Paragraph and not display row: a wrapped line is one line
     * as far as anyone reading the numbers is concerned. */
    SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&sel);
    caret_para = para_at(&g, (int)sel.cpMin);

    SetBkMode(dc, TRANSPARENT);
    old = (HFONT)SelectObject(dc, g.uifont);

    first = (int)SendMessageW(e, EM_GETFIRSTVISIBLELINE, 0, 0);

    /* Line numbers count paragraphs, not the wrapped rows RichEdit reports,
     * so scan the cached text up to the first visible row once and then keep
     * count as we walk down the screen. */
    para = para_at(&g, line_start(e, first));

    /* One font per document means every row is the same height: ask the
     * control where the first two rows sit and step by the difference, rather
     * than querying a position for every row down the page. */
    {
        POINTL p0, p1;
        int s0 = line_start(e, first);
        int s1 = line_start(e, first + 1);

        p0.x = p0.y = 0;
        SendMessageW(e, EM_POSFROMCHAR, (WPARAM)&p0, (LPARAM)(s0 < 0 ? 0 : s0));
        y0 = p0.y;

        rowh = 0;
        if (s1 >= 0) {
            p1.x = p1.y = 0;
            SendMessageW(e, EM_POSFROMCHAR, (WPARAM)&p1, (LPARAM)s1);
            rowh = p1.y - p0.y;
        }
        if (rowh <= 0) rowh = g.line_h;
        if (rowh <= 0) rowh = 16;
    }

    for (i = first, y = y0; y < rc.bottom; i++, y += rowh) {
        int  start = line_start(e, i);
        int  head, row_para;
        RECT tr;

        if (start < 0) break;

        /* A wrapped continuation row gets no number of its own -- but it is
         * still part of the paragraph above it, and the wash has to cover it
         * too or a wrapped current line would be striped. */
        head     = para_starts_at(g.cache, g.cache_len, start);
        row_para = head ? para : para - 1;

        tr.left = 0;  tr.right  = rc.right;
        tr.top  = y;  tr.bottom = y + rowh;

        /* The same wash the line itself carries, so the number and its line
         * read as one band across the window. */
        if (row_para == caret_para) FillRect(dc, &tr, g.br_curline);

        if (head) {
            nchar num[12];
            int   n = n_utoa((unsigned)para, num);
            tr.right -= 6;
            SetTextColor(dc, cr(row_para == caret_para ? g.theme.fg
                                                       : g.theme.gutter_fg));
            /* os_draw_text: DrawTextW is a stub on Windows 95 -- see
             * StatusProc below -- and the numbers simply would not appear. */
            os_draw_text(dc, num, n, &tr, DT_RIGHT | DT_TOP | DT_SINGLELINE);
            para++;
        }
    }

    SelectObject(dc, old);
    EndPaint(wnd, &ps);
}

LRESULT CALLBACK GutterProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT)      { gutter_paint(wnd); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(wnd, msg, wp, lp);
}

#endif /* !NOTE_OWN_VIEW */

LRESULT CALLBACK StatusProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_ERASEBKGND) return 1;   /* WM_PAINT covers every pixel */

    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC   dc = BeginPaint(wnd, &ps);
        RECT  rc;
        HFONT old;

        GetClientRect(wnd, &rc);
        FillRect(dc, &rc, g.br_ui);

        /* One rule along the top.  The band is filled with br_ui and so is the
         * frame behind it, so without a line there is nothing to see: a status
         * bar whose text has not been written yet, or could not be, is exactly
         * the window background and reads as no status bar at all.
         *
         * Through chrome_apart() rather than br_sep, which is a fifth of the
         * way from the band towards its text and on a dark theme at 8bpp lands
         * back on the band's own black.  A rule that is the colour of what it
         * separates is not a quiet rule, it is no rule. */
        fill_px(dc, rc.left, rc.top, rc.right - rc.left, 1,
                chrome_apart(g.theme.ui_bg, g.theme.ui_fg, 1, 5,
                             chrome_snap(cr(g.theme.ui_bg))));
        rc.top += 1;

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, cr(g.theme.ui_fg));
        old = (HFONT)SelectObject(dc, g.menufont);
        rc.left += px(8);
        /* os_draw_text, not DrawTextW: DrawText is user32, where Windows 95
         * answers the W entry point with a stub that draws nothing and returns
         * zero.  Nothing fails and nothing is reported -- the band is painted,
         * the text simply never appears, and against a background of the same
         * colour that looks like a status bar that was never created. */
        os_draw_text(dc, (const nchar *)g.status_text, -1, &rc,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);

        EndPaint(wnd, &ps);
        return 0;
    }
    /* os_defproc for the same reason: on Windows 95 this class is registered
     * through RegisterClassExA, and the default handling has to be the half
     * that matches. */
    return os_defproc(wnd, msg, wp, lp);
}

#if !NOTE_OWN_VIEW

/* One pass over the cache recording where each paragraph starts, so that
 * "which line is this offset on" stops being a scan from the top of the
 * document.
 *
 * One pass into an array that grows, and is then kept.  It used to be two
 * passes -- count, allocate exactly, fill -- which reads a thirteen-megabyte
 * document twice and hands the heap a megabyte-sized block to free and
 * reallocate on every keystroke.  Doubling wastes at most half the array once
 * and costs nothing after the file settles. */
static int lines_room(note_host *h, int want)
{
    int  cap = h->lines_cap;
    int *grown;

    if (cap >= want) return 1;
    if (cap < 256) cap = 256;
    while (cap < want) cap *= 2;

    grown = (int *)h_alloc(h, (unsigned long)cap * sizeof(int));
    if (!grown) return 0;
    if (h->lines) {
        int i;
        for (i = 0; i < h->nlines; i++) grown[i] = h->lines[i];
        h_free(h, h->lines);
    }
    h->lines     = grown;
    h->lines_cap = cap;
    return 1;
}

static void build_line_index(note_host *h)
{
    int i;

    h->nlines = 0;
    if (!h->cache || !lines_room(h, 1)) return;

    h->lines[0] = 0;
    h->nlines   = 1;

    for (i = 0; i < h->cache_len; i++) {
        if (!is_break(h->cache[i])) continue;
        if (h->cache[i] == (nchar)'\r' && h->cache[i + 1] == (nchar)'\n') i++;
        if (!lines_room(h, h->nlines + 1)) return;
        h->lines[h->nlines++] = i + 1;
    }
}

/* The paragraph containing `off`, counting from 1. */
int para_at(note_host *h, int off)
{
    int lo = 0, hi = h->nlines - 1, best = 0;

    if (!h->lines || h->nlines <= 0) return 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (h->lines[mid] <= off) { best = mid; lo = mid + 1; }
        else                      { hi = mid - 1; }
    }
    return best + 1;
}

/* The paragraph and the column an offset is on, which is what the status bar
 * says.  A paragraph and not a display row: a long line wrapped across three
 * rows is one line as far as anyone reading the number is concerned. */
void edit_status_pos(note_host *h, int pos, int *line, int *col)
{
    HWND e = active_edit();
    LONG row, start;

    row   = (LONG)SendMessageW(e, EM_EXLINEFROMCHAR, 0, (LPARAM)pos);
    start = (LONG)SendMessageW(e, EM_LINEINDEX, (WPARAM)row, 0);

    refresh_cache(h);
    *line = para_at(h, pos);
    *col  = (int)(pos - start) + 1;
}

/* The cache buffer is kept between refreshes and only ever grows, with a
 * quarter of slack on top.  Freeing and reallocating twenty-six megabytes on
 * every keystroke is not the sort of cost the heap absorbs quietly, and the
 * length of a document being typed into changes by one. */
static int cache_room(note_host *h, int want)
{
    int    cap = h->cache_cap;
    nchar *grown;

    if (cap >= want) return 1;

    cap = want + want / 4 + 64;
    grown = (nchar *)h_alloc(h, (unsigned long)cap * sizeof(nchar));
    if (!grown) return 0;

    if (h->cache) h_free(h, h->cache);
    h->cache     = grown;
    h->cache_cap = cap;
    return 1;
}

void refresh_cache(note_host *h)
{
    int len;

    if (h->cache_valid && h->cache_doc == h->app.active) return;

    len = edit_len(active_edit());
    if (!cache_room(h, len + 2)) { h->cache_len = 0; h->cache_valid = 0; return; }

    /* EM_GETTEXTEX reports what it copied, so the length is already known: a
     * strlen over the whole document afterwards is a second walk of it for an
     * answer we were just handed. */
    h->cache_len   = h_text_get(h, h->app.active, h->cache, len + 1);
    if (h->cache_len < 0 || h->cache_len > len) h->cache_len = len;
    h->cache[h->cache_len] = 0;
    h->cache_doc   = h->app.active;
    h->cache_valid = 1;
    build_line_index(h);
}

/* -------------------------------------------------------------------------
 * The highlighter
 *
 * RichEdit remembers formatting once it has been set, so the useful unit of
 * work is not "colour the screen" but "extend the range that is already
 * coloured".  Each document therefore carries hl_from/hl_to -- the stretch of
 * it known to be coloured -- and a pass only ever fills in what those do not
 * cover.  Holding Page Down then costs one screenful of colouring per press
 * instead of recolouring the same viewport on every frame, and it costs the
 * same on a thirteen-megabyte file as on a small one.
 *
 * Two rules keep it responsive:
 *
 *   - The viewport is always finished in the pass that asks for it.  Half a
 *     coloured screen is worse than an uncoloured one.
 *   - Everything beyond the viewport -- a screenful ahead and a screenful
 *     behind, so that the next scroll finds its text already done -- is filled
 *     in HL_CHUNK at a time from a timer, giving the message loop its turn
 *     back between chunks.
 *
 * A worker thread would not help and is not used.  The expensive half is not
 * the lexing (a screenful is a couple of milliseconds) but the
 * EM_SETCHARFORMAT calls, and those have to happen on the thread that owns
 * the control: sending them from anywhere else only blocks this thread to do
 * the same work in a less predictable order.  Chunking on the UI thread buys
 * the same responsiveness with none of that.
 * ------------------------------------------------------------------------- */

/* An edit, rather than a change of theme or language.
 *
 * Nothing above the paragraph the edit landed in can lex differently because
 * of it -- a quote or a comment opener only ever reaches forwards -- so the
 * coloured range is trimmed back to the start of that paragraph instead of
 * being thrown away.  Typing then recolours the paragraph under the caret and
 * whatever of the viewport lies below it, rather than the whole screen on
 * every keystroke, and each of those screenfuls was a few hundred
 * EM_SETCHARFORMAT calls.
 *
 * The trimming itself waits for hl_settle(): the line index it needs is
 * rebuilt from the control after the edit, and this runs during it. */
void hl_touch(note_host *h, int doc, int caret)
{
    win_doc *d;
    int      off = caret;

    if (doc < 0 || doc >= NOTE_MAX_DOCS) return;
    d = &h->d[doc];

    if (!d->hl_valid) return;

    /* The caret is where the edit *ended*.  After a deletion that is also
     * where it began, but after an insertion the new text lies behind the
     * caret, and pasting a screenful of it would otherwise leave everything
     * but its last paragraph coloured as it was.  How much the document grew
     * says how far back to reach; several edits arriving before a refresh
     * only make that reach further, which errs the safe way. */
    /* Not cache_valid: the first keystroke of a burst clears it, and the
     * length recorded at the last refresh is still the right baseline for
     * every keystroke after that one. */
    if (h->cache && h->cache_doc == doc) {
        int grew = edit_len(h->d[doc].edit) - h->cache_len;
        if (grew > 0) off -= grew;
    } else {
        off = 0;                  /* no idea what changed: colour it all */
    }
    if (off < 0) off = 0;

    if (d->hl_dirty_at < 0 || off < d->hl_dirty_at) d->hl_dirty_at = off;
}

/* Resolves a pending hl_touch now that the cache and the line index agree
 * with the control again.  Called from the two places that colour. */
static void hl_settle(note_host *h, win_doc *d)
{
    int start;

    if (d->hl_dirty_at < 0) return;
    d->hl_dirty_at = (d->hl_dirty_at > h->cache_len) ? h->cache_len
                                                     : d->hl_dirty_at;
    start = 0;
    if (h->lines && h->nlines > 0)
        start = h->lines[para_at(h, d->hl_dirty_at) - 1];

    if (!d->hl_valid || start <= d->hl_from) {
        d->hl_valid = 0;
        d->hl_from  = d->hl_to = 0;
    } else if (d->hl_to > start) {
        d->hl_to = start;
    }
    d->hl_dirty_at = -1;
}

void hl_invalidate(note_host *h, int doc)
{
    int i;
    for (i = 0; i < NOTE_MAX_DOCS; i++)
        if (doc < 0 || doc == i) {
            h->d[i].hl_valid = 0;
            h->d[i].hl_from = h->d[i].hl_to = 0;
            h->d[i].hl_dirty_at = -1;
            /* The caret's band is a background rather than a colour, so
             * nothing above repaints it -- but it was mixed from the theme
             * and sits on offsets the text may have moved, so it goes too. */
            h->d[i].cur_valid = 0;
        }
}

/* Colours [from,to) from the lexer: the default colour over the whole range
 * first, then the spans on top of it.  Tokenising is done in slices, because
 * the span array is fixed and a large range would overflow it -- and each
 * slice still starts from the nearest point the core can promise is outside
 * any string or block comment, which is what keeps the cost independent of
 * how far down the file the range happens to be. */
/* -------------------------------------------------------------------------
 * Setting a colour on a range
 *
 * Every colour the editor puts on text -- the lexer's, and the caret band's
 * background -- goes through here, and it has two ways of doing it.
 *
 * The one it wants is the Text Object Model: ITextRange names the characters
 * directly, so nothing is done to the selection, nothing is done to the
 * scroll position, and the control invalidates the characters that changed
 * and no others.
 *
 * The one it falls back to is EM_SETCHARFORMAT, which can only format the
 * selection.  That means moving the selection and putting it back, and moving
 * it is visible, so the whole control has to be frozen with WM_SETREDRAW
 * around the lot -- and thawing it invalidates every pixel.  On each
 * keystroke that was the whole client area repainted twice, which is what the
 * caret's line was flickering with.  The fallback stays for a RichEdit whose
 * OLE interface will not answer; Msftedit's always does.
 * ------------------------------------------------------------------------- */

/* "Whatever the page is", for either half of the call -- CFE_AUTO* for the
 * message, tomAutoColor for the model. */
#define FMT_AUTO ((COLORREF)0xFF000000u)

typedef struct {
    note_host     *h;
    HWND           e;
    ITextDocument *tom;     /* null: the selection path below */
    CHARRANGE      sel;     /* only saved when there is no model */
    POINT          scroll;
    int            was_mod;
} fmt_ctx;

static void fmt_begin(fmt_ctx *c, note_host *h, HWND e)
{
    c->h = h;
    c->e = e;

    /* Colouring sets the control's modify flag, and its EN_CHANGE can arrive
     * after we have stopped suppressing.  Remember the real state and put it
     * back, so opening a file never leaves it looking edited. */
    c->was_mod = (int)SendMessageW(e, EM_GETMODIFY, 0, 0);

    /* Formatting would otherwise pile records onto the undo stack, and Ctrl+Z
     * would undo the highlighter instead of the typing. */
    c->tom = tom_open(e);
    if (c->tom) c->tom->lpVtbl->Undo(c->tom, tomSuspend, 0);

    h->suppress++;

    if (!c->tom) {
        SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&c->sel);
        SendMessageW(e, EM_GETSCROLLPOS, 0, (LPARAM)&c->scroll);
        SendMessageW(e, WM_SETREDRAW, FALSE, 0);
    }
}

/* `back` picks which of the two colours a character has: the glyphs, or the
 * band behind them. */
static void fmt_set(fmt_ctx *c, int from, int to, COLORREF col, int back)
{
    if (to <= from) return;

    if (c->tom) {
        ITextRange *r = 0;
        ITextFont  *f = 0;
        long        v = (col == FMT_AUTO) ? tomAutoColor : (long)col;

        if (FAILED(c->tom->lpVtbl->Range(c->tom, from, to, &r)) || !r) return;
        if (SUCCEEDED(r->lpVtbl->GetFont(r, &f)) && f) {
            if (back) f->lpVtbl->SetBackColor(f, v);
            else      f->lpVtbl->SetForeColor(f, v);
            f->lpVtbl->Release(f);
        }
        r->lpVtbl->Release(r);
        return;
    }

    {
        CHARFORMAT2W cf;
        CHARRANGE    r;

        memset(&cf, 0, sizeof(cf));
        cf.cbSize = sizeof(cf);
        if (back) {
            cf.dwMask = CFM_BACKCOLOR;
            if (col == FMT_AUTO) cf.dwEffects   = CFE_AUTOBACKCOLOR;
            else                 cf.crBackColor = col;
        } else {
            cf.dwMask = CFM_COLOR;
            if (col == FMT_AUTO) cf.dwEffects   = CFE_AUTOCOLOR;
            else                 cf.crTextColor = col;
        }

        r.cpMin = from; r.cpMax = to;
        SendMessageW(c->e, EM_EXSETSEL, 0, (LPARAM)&r);
        SendMessageW(c->e, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    }
}

static void fmt_end(fmt_ctx *c)
{
    if (!c->tom) {
        SendMessageW(c->e, EM_EXSETSEL, 0, (LPARAM)&c->sel);
        SendMessageW(c->e, EM_SETSCROLLPOS, 0, (LPARAM)&c->scroll);
    }
    SendMessageW(c->e, EM_SETMODIFY, (WPARAM)(c->was_mod ? TRUE : FALSE), 0);
    if (!c->tom) SendMessageW(c->e, WM_SETREDRAW, TRUE, 0);

    c->h->suppress--;

    if (c->tom) {
        c->tom->lpVtbl->Undo(c->tom, tomResume, 0);
        c->tom->lpVtbl->Release(c->tom);
        c->tom = 0;
    }
}

/* Colours [from,to) from the lexer: the default colour over the whole range
 * first, then the spans on top of it.  Tokenising is done in slices, because
 * the span array is fixed and a large range would overflow it -- and each
 * slice still starts from the nearest point the core can promise is outside
 * any string or block comment, which is what keeps the cost independent of
 * how far down the file the range happens to be. */
static void colour_range(note_host *h, HWND e, int lang, int from, int to)
{
    fmt_ctx c;
    int     at;

    if (from < 0) from = 0;
    if (to > h->cache_len) to = h->cache_len;
    if (to <= from) return;

    /* The band is a background on characters, and the sweep below takes every
     * background in this range off.  Saying so here is what gets it put back:
     * curline_update() re-applies a band it no longer believes in, and every
     * path that colours ends up there. */
    {
        win_doc *d = &h->d[h->app.active];
        if (e == d->edit && d->cur_valid &&
            d->cur_from < to && d->cur_to > from)
            d->cur_valid = 0;
    }

    fmt_begin(&c, h, e);

    fmt_set(&c, from, to, cr(h->theme.fg), 0);
    /* And the page behind them.  A caret band is a background on the
     * characters of one line, and an edit moves those characters out from
     * under the offsets the band was recorded on -- so the band that is about
     * to be put back is also the only one allowed to survive this.  Without
     * this line every split paragraph left its wash behind and the document
     * slowly filled with stripes the caret had visited. */
    fmt_set(&c, from, to, FMT_AUTO, 1);

    for (at = from; lang != LANG_NONE && at < to; ) {
        int end = at + HL_SLICE;
        int scan, nspans, i;

        if (end > to) end = to;

        scan   = note_syntax_safe_start(lang, h->cache, h->cache_len,
                                        at, SAFE_START_WINDOW);
        nspans = note_tokenize(lang, h->cache + scan, end - scan, scan,
                               h->spans, MAX_SPANS);

        for (i = 0; i < nspans; i++) {
            int lo = h->spans[i].start, hi = h->spans[i].start + h->spans[i].len;
            if (hi <= at)  continue;
            if (lo >= end) break;
            if (lo < at)  lo = at;
            if (hi > end) hi = end;
            fmt_set(&c, lo, hi, cr(h->theme.tok[h->spans[i].kind]), 0);
        }

        /* A slice dense enough to fill the span array would leave its tail
         * uncoloured, and the caller is about to record it as done.  Stop at
         * the last span that did fit and let the next slice carry on there. */
        if (nspans >= MAX_SPANS) {
            int fit = h->spans[MAX_SPANS - 1].start;
            if (fit > at) end = fit;
        }
        at = end;
    }

    fmt_end(&c);
}

/* -------------------------------------------------------------------------
 * The caret's line
 *
 * RichEdit has no notion of a highlighted line and no way to paint underneath
 * one: the control owns every pixel of its client area and its background is
 * one colour for the whole document.  What it does have is a background
 * colour per character, so the band under the text is exactly that -- a
 * background given to the characters the caret's paragraph is made of.
 *
 * That alone would stop the band where the text stops, which is not a band at
 * all but a highlighted word.  The rest of each row is empty -- the margin the
 * control keeps to the left of the first character, and everything from the
 * last character out to the scroll bar -- so both ends can be filled in after
 * the control has painted, without drawing over anything.  The three pieces
 * use one colour and read as one stripe.
 *
 * The paragraph break is included in the character range on purpose: it is
 * what gives an empty line something to show.
 * ------------------------------------------------------------------------- */

/* Mixed rather than named: no theme has an opinion about a caret line, but
 * every theme has a foreground and a background.  Defined with the rest of the
 * band's drawing, further down. */
static COLORREF curline_rgb(note_host *h);

/* Both halves of the work are the same call twice, so it is written once. */
static void band_apply(note_host *h, HWND e, int from, int to, int on)
{
    fmt_ctx c;

    if (to <= from) return;

    fmt_begin(&c, h, e);
    fmt_set(&c, from, to, on ? curline_rgb(h) : FMT_AUTO, 1);
    fmt_end(&c);
}

/* The rows [from,to) occupies, and nothing else.  The band's ends are drawn
 * by hand in WM_PAINT -- see curline_tail() -- so a band that moves has to
 * ask for the rows it left and the rows it landed on to be repainted.  Asking
 * for the whole client area instead is what a caret moving down a file looked
 * like: every row redrawn on every press of an arrow key. */
static void band_rows(note_host *h, HWND e, int from, int to)
{
    RECT   rc, r;
    POINTL p;
    int    rowh;

    if (to < from) return;

    GetClientRect(e, &rc);

    p.x = p.y = 0;
    SendMessageW(e, EM_POSFROMCHAR, (WPARAM)&p, (LPARAM)from);
    r.top = p.y;

    p.x = p.y = 0;
    SendMessageW(e, EM_POSFROMCHAR, (WPARAM)&p, (LPARAM)(to > from ? to - 1 : from));

    rowh = MulDiv(h->line_h, h->app.zoom, 100);
    if (rowh <= 0) rowh = 16;
    r.bottom = p.y + rowh;

    /* Nothing the control would answer for -- an offset it has scrolled past,
     * or one it has not laid out yet.  The whole area then, which is correct
     * and merely more work. */
    if (r.bottom <= r.top) { r.top = rc.top; r.bottom = rc.bottom; }

    r.left  = rc.left;
    r.right = rc.right;
    if (r.top    < rc.top)    r.top    = rc.top;
    if (r.bottom > rc.bottom) r.bottom = rc.bottom;
    if (r.bottom > r.top) InvalidateRect(e, &r, FALSE);
}

void curline_update(note_host *h)
{
    HWND      e = active_edit();
    win_doc  *d;
    CHARRANGE sel;
    int       para, from, to, want;

    if (!e) return;
    d = &h->d[h->app.active];

    refresh_cache(h);
    SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&sel);

    /* Nothing while a selection is up: two overlapping washes say less than
     * either of them alone. */
    want = (sel.cpMin == sel.cpMax);

    from = to = 0;
    if (want && h->lines && h->nlines > 0) {
        para = para_at(h, (int)sel.cpMin);
        from = h->lines[para - 1];
        to   = (para < h->nlines) ? h->lines[para] : h->cache_len;
        if (to > h->cache_len) to = h->cache_len;
        if (to < from) to = from;
    } else {
        want = 0;
    }

    if (d->cur_valid && d->cur_from == from && d->cur_to == to && want) return;

    /* Still the same line, only longer or shorter: what typing does, on every
     * keystroke.  Taking the band off and putting it back would be a frame
     * with no band in it -- the blink the caret's line used to have under
     * every character typed -- so the band is extended over the characters
     * that were added and withdrawn from the ones that went. */
    if (want && d->cur_valid && d->cur_from == from) {
        /* Over the whole line when it grew, not only over the tail: a
         * character typed at the start of it takes its formatting from the
         * one before it, which is the previous line's break and carries no
         * band.  One range either way, so the wider one costs nothing. */
        if (to > d->cur_to) band_apply(h, e, from, to, 1);
        else                band_apply(h, e, to, d->cur_to, 0);
        band_rows(h, e, from, to > d->cur_to ? to : d->cur_to);
        d->cur_to = to;
        return;
    }

    /* Taking the old band off first: after an edit the offsets it was on may
     * no longer be the line they were, but putting a background back to the
     * page can only ever remove a band, never leave one in the wrong place --
     * and the recolouring that follows an edit clears the backgrounds over
     * everything it touches, which is what catches the rest. */
    if (d->cur_valid) {
        band_apply(h, e, d->cur_from, d->cur_to, 0);
        band_rows(h, e, d->cur_from, d->cur_to);
    }
    d->cur_valid = 0;

    if (want) {
        band_apply(h, e, from, to, 1);
        band_rows(h, e, from, to);
        d->cur_from  = from;
        d->cur_to    = to;
        d->cur_valid = 1;
    }
}

static COLORREF curline_rgb(note_host *h)
{
    return mix_rgb(h->theme.fg, h->theme.bg, 1, 12);
}

/* Fills the empty ends of every row the caret's paragraph occupies, after the
 * control has drawn the row itself.  Called from EditProc's WM_PAINT, which is
 * the only moment at which those pixels are known to be background and nothing
 * else. */
void curline_tail(note_host *h, HWND e, const RECT *clip)
{
    win_doc *d = &h->d[h->app.active];
    HDC      dc;
    HBRUSH   br;
    RECT     rc;
    POINTL   p;
    int      first, last, top, rowh, charw, i;

    if (!d->cur_valid || e != d->edit) return;

    GetClientRect(e, &rc);
    /* Only the rows the control has just redrawn: anything outside them is
     * still showing pixels this fill has no business on. */
    if (clip) {
        if (clip->top    > rc.top)    rc.top    = clip->top;
        if (clip->bottom < rc.bottom) rc.bottom = clip->bottom;
        if (rc.bottom <= rc.top) return;
    }

    first = (int)SendMessageW(e, EM_EXLINEFROMCHAR, 0, (LPARAM)d->cur_from);
    last  = (int)SendMessageW(e, EM_EXLINEFROMCHAR, 0,
                              (LPARAM)(d->cur_to > d->cur_from ? d->cur_to - 1
                                                               : d->cur_from));

    p.x = p.y = 0;
    SendMessageW(e, EM_POSFROMCHAR, (WPARAM)&p, (LPARAM)d->cur_from);
    top = p.y;

    /* Row height and character width the way the control is drawing them now,
     * which is the measured font scaled by whatever the zoom is. */
    rowh  = MulDiv(h->line_h, h->app.zoom, 100);
    charw = MulDiv(h->char_w, h->app.zoom, 100);
    if (rowh  <= 0) rowh  = 16;
    if (charw <= 0) charw = 8;

    /* Off the top or the bottom: nothing of it is on screen. */
    if (top + (last - first + 1) * rowh < 0 || top > rc.bottom) return;

    br = CreateSolidBrush(curline_rgb(h));
    dc = GetDC(e);

    for (i = first; i <= last; i++) {
        int   start = (int)SendMessageW(e, EM_LINEINDEX, (WPARAM)i, 0);
        int   len   = (int)SendMessageW(e, EM_LINELENGTH, (WPARAM)start, 0);
        int   y     = top + (i - first) * rowh;
        RECT  fill;

        if (start < 0) break;
        if (y + rowh < 0 || y > rc.bottom) continue;

        fill.top    = y;
        fill.bottom = y + rowh;
        if (fill.top    < 0)         fill.top    = 0;
        if (fill.bottom > rc.bottom) fill.bottom = rc.bottom;

        /* The margin before the first character.  Without it the band starts a
         * few pixels in from the gutter and the two do not meet. */
        p.x = p.y = 0;
        SendMessageW(e, EM_POSFROMCHAR, (WPARAM)&p, (LPARAM)start);
        fill.left  = 0;
        fill.right = p.x;
        if (fill.right > fill.left && fill.bottom > fill.top)
            FillRect(dc, &fill, br);

        p.x = p.y = 0;
        SendMessageW(e, EM_POSFROMCHAR, (WPARAM)&p, (LPARAM)(start + len));

        /* On a wrapped row there is no break to ask about: the character just
         * past the row is the first of the next one, and the control answers
         * with that row's origin.  One character further back and one
         * character's width forward lands in the same place -- the font is
         * always a monospaced one here, which is what makes that exact. */
        if (p.y != y && len > 0) {
            p.x = p.y = 0;
            SendMessageW(e, EM_POSFROMCHAR, (WPARAM)&p, (LPARAM)(start + len - 1));
            p.x += charw;
        }

        fill.left  = p.x;
        fill.right = rc.right;
        if (fill.right > fill.left && fill.bottom > fill.top)
            FillRect(dc, &fill, br);
    }

    ReleaseDC(e, dc);
    DeleteObject(br);
}

/* Makes sure [a,b) is coloured, spending at most `budget` characters on it.
 * The coloured stretch is one range, so a request either extends it at one or
 * both ends or -- after a jump -- replaces it.  Returns 1 when the whole of
 * [a,b) is covered, and 0 when the budget ran out first and there is more to
 * do on the next pass. */
static int hl_cover(note_host *h, HWND e, win_doc *d, int lang,
                    int a, int b, int budget)
{
    if (a < 0) a = 0;
    if (b > h->cache_len) b = h->cache_len;
    if (b < a) b = a;

    /* Nothing coloured, or nothing of it near enough to build on. */
    if (!d->hl_valid || b < d->hl_from || a > d->hl_to) {
        int to = (b - a > budget) ? a + budget : b;
        colour_range(h, e, lang, a, to);
        d->hl_from  = a;
        d->hl_to    = to;
        d->hl_valid = 1;
        return to >= b;
    }

    /* Forwards first: that is the direction reading and scrolling go. */
    if (b > d->hl_to) {
        int to = (b - d->hl_to > budget) ? d->hl_to + budget : b;
        budget -= to - d->hl_to;
        colour_range(h, e, lang, d->hl_to, to);
        d->hl_to = to;
    }
    if (a < d->hl_from && budget > 0) {
        int from = (d->hl_from - a > budget) ? d->hl_from - budget : a;
        colour_range(h, e, lang, from, d->hl_from);
        d->hl_from = from;
    }
    return d->hl_from <= a && d->hl_to >= b;
}

/* The viewport in characters, and the same again a screenful either side. */
static void hl_window(note_host *h, HWND e,
                      int *vis_from, int *vis_to, int *want_from, int *want_to)
{
    int first, rows, over;

    first = (int)SendMessageW(e, EM_GETFIRSTVISIBLELINE, 0, 0);
    {
        RECT rc;
        GetClientRect(e, &rc);
        rows = (h->line_h > 0) ? (rc.bottom / h->line_h) + 2 : 60;
    }

    *vis_from = line_start(e, first);
    *vis_to   = line_start(e, first + rows);
    if (*vis_from < 0) *vis_from = 0;
    if (*vis_to   < 0) *vis_to = h->cache_len;

    over       = (first > rows) ? first - rows : 0;
    *want_from = line_start(e, over);
    if (*want_from < 0) *want_from = 0;
    *want_to = line_start(e, first + rows * 2);
    if (*want_to < 0) *want_to = h->cache_len;
}

static int hl_lang(note_host *h)
{
    return h->app.syntax ? h->app.docs[h->app.active].lang : LANG_NONE;
}

/* One idle chunk of the work either side of the viewport.  Stops the timer as
 * soon as there is nothing left to do, so an editor sitting still costs
 * nothing. */
void hl_step(note_host *h)
{
    HWND     e = active_edit();
    win_doc *d = &h->d[h->app.active];
    int      vis_from, vis_to, want_from, want_to;

    if (!e || !d->hl_valid) { KillTimer(h->wnd, TIMER_HL); return; }

    refresh_cache(h);
    hl_settle(h, d);
    hl_window(h, e, &vis_from, &vis_to, &want_from, &want_to);

    if (hl_cover(h, e, d, hl_lang(h), want_from, want_to, HL_CHUNK))
        KillTimer(h->wnd, TIMER_HL);

    /* A chunk that reached the caret's line has just cleared its band. */
    curline_update(h);
}

/* Colours what is on screen, now, and arms the timer for the rest. */
static void highlight(note_host *h)
{
    HWND     e = active_edit();
    win_doc *d;
    int      vis_from, vis_to, want_from, want_to, had_from, had_to, valid;

    if (!e) return;

    refresh_cache(h);
    d = &h->d[h->app.active];
    hl_settle(h, d);

    hl_window(h, e, &vis_from, &vis_to, &want_from, &want_to);

    had_from = d->hl_from;
    had_to   = d->hl_to;
    valid    = d->hl_valid;

    /* No budget: whatever the screen shows is coloured before this returns. */
    hl_cover(h, e, d, hl_lang(h), vis_from, vis_to, h->cache_len + 1);

    /* Only when something actually changed on screen -- a scroll that lands
     * inside the coloured range repaints nothing. */
    if (!valid || d->hl_from != had_from || d->hl_to != had_to)
        InvalidateRect(e, NULL, FALSE);

    if (d->hl_from > want_from || d->hl_to < want_to)
        SetTimer(h->wnd, TIMER_HL, 25, NULL);
}

void queue_view(note_host *h)
{
    if (h->view_pending) return;
    h->view_pending = 1;
    SetTimer(h->wnd, TIMER_VIEW, 40, NULL);
}

void service_view(note_host *h)
{
    h->view_pending = 0;
    KillTimer(h->wnd, TIMER_VIEW);

    gutter_width(h);
    relayout(h);
    highlight(h);
    curline_update(h);
    if (h->gutter) InvalidateRect(h->gutter, NULL, FALSE);
}

void h_rehighlight(note_host *h)
{
    h->cache_valid = 0;
    /* The theme or the language changed under every document, so nothing that
     * is on screen anywhere is the right colour any more. */
    hl_invalidate(h, -1);
    queue_view(h);
}

/* -------------------------------------------------------------------------
 * Indenting
 *
 * Tab over a selection that spans lines moves all of them, and Shift+Tab
 * moves them back -- the one editing gesture RichEdit has no idea about.  A
 * single caret keeps the plain tab it always had.
 *
 * The whole run of lines is replaced in one EM_REPLACESEL, which is what puts
 * it on the undo stack as one action rather than as one per line.
 * ------------------------------------------------------------------------- */

#define INDENT_SPACES 4      /* how many a Shift+Tab will take off instead */

/* Shifts the lines the selection touches.  Returns 0 when there was nothing
 * to shift, and the caller lets the key through as an ordinary tab. */
static int indent_lines(note_host *h, HWND e, int out)
{
    CHARRANGE sel;
    nchar    *buf;
    int       first, last, from, to, i, n = 0, cap;

    refresh_cache(h);

    SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&sel);

    first = para_at(h, (int)sel.cpMin);
    last  = para_at(h, (int)(sel.cpMax > sel.cpMin ? sel.cpMax - 1 : sel.cpMin));

    if (!h->cache || !h->lines || h->nlines <= 0) return 0;

    /* Indenting a caret sitting in a line is what the Tab key already does.
     * Outdenting it is not, so that one goes ahead on its own. */
    if (!out && first == last) return 0;

    from = h->lines[first - 1];
    to   = (last < h->nlines) ? h->lines[last] : h->cache_len;
    if (to > h->cache_len) to = h->cache_len;
    if (to <= from && !(to == from && first == last)) return 0;

    /* One extra character per line is the worst indenting can do. */
    cap = (to - from) + (last - first + 1) + 2;
    buf = (nchar *)h_alloc(h, (unsigned long)cap * sizeof(nchar));
    if (!buf) return 0;

    for (i = first; i <= last; i++) {
        int ls = h->lines[i - 1];
        int le = (i < h->nlines) ? h->lines[i] : h->cache_len;
        int at = ls;

        if (le > h->cache_len) le = h->cache_len;

        if (out) {
            /* One tab, or up to a tab's worth of spaces -- whichever the line
             * actually starts with. */
            int k = 0;
            if (at < le && h->cache[at] == (nchar)'\t') at++;
            else while (at < le && k < INDENT_SPACES &&
                        h->cache[at] == (nchar)' ') { at++; k++; }
        } else if (le > ls) {
            /* An empty line gains nothing: trailing whitespace on a line with
             * nothing on it is not an indent, it is litter. */
            buf[n++] = (nchar)'\t';
        }

        while (at < le && n < cap - 1) buf[n++] = h->cache[at++];
    }
    buf[n] = 0;

    /* Nothing to do -- every line was already at the left margin. */
    if (n == to - from) {
        int same = 1, k;
        for (k = 0; k < n; k++)
            if (buf[k] != h->cache[from + k]) { same = 0; break; }
        if (same) { h_free(h, buf); return out ? 1 : 0; }
    }

    sel.cpMin = from; sel.cpMax = to;
    SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&sel);
    SendMessageW(e, EM_REPLACESEL, TRUE, (LPARAM)buf);

    /* The same lines, still selected, so a second Tab keeps working on them. */
    sel.cpMin = from; sel.cpMax = from + n;
    SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&sel);

    h_free(h, buf);
    h->cache_valid = 0;
    hl_invalidate(h, h->app.active);
    service_view(h);
    return 1;
}

/* -------------------------------------------------------------------------
 * The editor subclass — everything that can scroll or change the text asks
 * for a gutter repaint and a recolour.
 * ------------------------------------------------------------------------- */

LRESULT CALLBACK EditProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    int doc;
    WNDPROC old = 0;

    for (doc = 0; doc < NOTE_MAX_DOCS; doc++)
        if (g.d[doc].edit == wnd) { old = g.d[doc].oldproc; break; }
    if (!old) return DefWindowProcW(wnd, msg, wp, lp);

    switch (msg) {
    /* After the control, never instead of it: the tail is drawn over pixels
     * the control has just painted as background and nothing else. */
    case WM_PAINT: {
        RECT    upd;
        LRESULT r;
        /* What the control is about to repaint.  The tail is drawn through a
         * window DC of our own -- the control's paint DC is gone by the time
         * it returns -- and a window DC is clipped to nothing, so without
         * this the fill reaches rows the control never touched and leaves
         * stripes of band colour behind on them. */
        if (!GetUpdateRect(wnd, &upd, FALSE)) upd.left = upd.right = 0;
        r = CallWindowProcW(old, wnd, msg, wp, lp);
        if (upd.right > upd.left) curline_tail(&g, wnd, &upd);
        return r;
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
    /* The tab the key would have typed.  The message loop translates a key
     * into its character before anything dispatches the key itself, so the
     * WM_CHAR is already on its way by the time the shift below has run --
     * and it would land on the block that was just indented and replace the
     * whole of it with one tab.  Indenting therefore has to eat the character
     * as well as the key. */
    case WM_CHAR:
        if (wp == (WPARAM)'\t' && g.eat_tab) { g.eat_tab = 0; return 0; }
        g.eat_tab = 0;
        break;

    case WM_KEYDOWN:
        if (wp == VK_TAB && !(GetKeyState(VK_CONTROL) & 0x8000)) {
            int out = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (indent_lines(&g, wnd, out)) { g.eat_tab = 1; return 0; }
        }
        /* fall through to the refresh every key gets */

    case WM_VSCROLL:
    case WM_HSCROLL:
    case WM_MOUSEWHEEL:
    case WM_LBUTTONDOWN:
    case WM_SIZE: {
        LRESULT r = CallWindowProcW(old, wnd, msg, wp, lp);
        /* Straight through rather than through the timer.  None of these
         * changes the text, so there is nothing to coalesce, and a refresh now
         * costs a couple of milliseconds — while a delayed gutter visibly
         * trails the text it is numbering. */
        if (!g.suppress) service_view(&g);
        return r;
    }
    }
    return CallWindowProcW(old, wnd, msg, wp, lp);
}

#endif /* !NOTE_OWN_VIEW */

