/* win32_edit.c -- the RICHEDIT control, the line-number gutter and the highlighter
 *
 * Part of note's Win32 backend.  The shared state and the types every part of
 * it needs live in note_win32.h; see there for why the state is a header and
 * not a private static.
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

/* -------------------------------------------------------------------------
 * Layout, gutter and highlighting
 * ------------------------------------------------------------------------- */

void relayout(note_host *h);
void queue_view(note_host *h);
void refresh_cache(note_host *h);
void menu_set_brush(HMENU m, HBRUSH br);
COLORREF blend_rgb(unsigned a, unsigned b);
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
    HWND  e = active_edit();
    int   first, y, y0, rowh, para, i;
    HFONT old;

    dc = BeginPaint(wnd, &ps);
    GetClientRect(wnd, &rc);
    FillRect(dc, &rc, g.br_gutter);

    if (!e) { EndPaint(wnd, &ps); return; }

    refresh_cache(&g);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, cr(g.theme.gutter_fg));
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
        int start = line_start(e, i);
        if (start < 0) break;

        /* A wrapped continuation row gets no number of its own. */
        if (para_starts_at(g.cache, g.cache_len, start)) {
            nchar num[12];
            int n = n_utoa((unsigned)para, num);
            RECT tr;
            tr.left = 0;  tr.right  = rc.right - 6;
            tr.top  = y;  tr.bottom = y + rowh;
            DrawTextW(dc, (LPCWSTR)num, n, &tr, DT_RIGHT | DT_TOP | DT_SINGLELINE);
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

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, cr(g.theme.ui_fg));
        old = (HFONT)SelectObject(dc, g.menufont);
        rc.left += px(8);
        DrawTextW(dc, g.status_text, -1, &rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);

        EndPaint(wnd, &ps);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

/* One pass over the cache recording where each paragraph starts, so that
 * "which line is this offset on" stops being a scan from the top of the
 * document.  Two passes rather than a growing array: counting first costs a
 * second walk of memory the CPU has just streamed, and buys one allocation of
 * exactly the right size. */
static void build_line_index(note_host *h)
{
    int i, n;

    if (h->lines) { h_free(h, h->lines); h->lines = 0; }
    h->nlines = 0;
    if (!h->cache) return;

    n = 1;
    for (i = 0; i < h->cache_len; i++) {
        if (is_break(h->cache[i])) {
            if (h->cache[i] == (nchar)'\r' && h->cache[i + 1] == (nchar)'\n') i++;
            n++;
        }
    }

    h->lines = (int *)h_alloc(h, (unsigned long)n * sizeof(int));
    if (!h->lines) return;

    h->lines[0] = 0;
    h->nlines = 1;
    for (i = 0; i < h->cache_len && h->nlines < n; i++) {
        if (is_break(h->cache[i])) {
            if (h->cache[i] == (nchar)'\r' && h->cache[i + 1] == (nchar)'\n') i++;
            h->lines[h->nlines++] = i + 1;
        }
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

void refresh_cache(note_host *h)
{
    int len;

    if (h->cache_valid && h->cache_doc == h->app.active) return;

    len = edit_len(active_edit());
    if (h->cache) { h_free(h, h->cache); h->cache = 0; }
    h->cache = (nchar *)h_alloc(h, (unsigned long)(len + 2) * sizeof(nchar));
    if (!h->cache) { h->cache_len = 0; h->cache_valid = 0; return; }

    h_text_get(h, h->app.active, h->cache, len + 1);
    h->cache_len   = n_len(h->cache);
    h->cache_doc   = h->app.active;
    h->cache_valid = 1;
    build_line_index(h);
}

/* Paint the visible range: default colour first, then the lexer's spans. */
static void highlight(note_host *h)
{
    HWND e = active_edit();
    CHARFORMAT2W cf;
    CHARRANGE    sel, all;
    ITextDocument *tom;
    POINT scroll;
    int first, rows, vis_start, vis_end, scan_start, nspans, i, lang, was_mod;

    if (!e) return;

    refresh_cache(h);

    lang = h->app.syntax ? h->app.docs[h->app.active].lang : LANG_NONE;

    first = (int)SendMessageW(e, EM_GETFIRSTVISIBLELINE, 0, 0);
    {
        RECT rc;
        GetClientRect(e, &rc);
        rows = (h->line_h > 0) ? (rc.bottom / h->line_h) + 2 : 60;
    }
    vis_start = line_start(e, first);
    vis_end   = line_start(e, first + rows);
    if (vis_start < 0) vis_start = 0;
    if (vis_end   < 0) vis_end = h->cache_len;
    if (vis_end > h->cache_len) vis_end = h->cache_len;

    /* Block comments and long strings begin before the viewport, so the lexer
     * cannot simply start at the top of the view.  It used to start at the top
     * of the *document* instead, which is correct but re-lexes the whole
     * prefix on every frame: measured at 61 ms per frame on a 427 KB file and
     * 74 ms on a 677 KB one, against a 40 ms tick — which is exactly what made
     * holding Page Down lag.  Asking the core for the nearest point known to
     * be outside any construct costs a backwards character scan instead, and
     * brings both files to about 2.5 ms, independent of their size. */
    scan_start = note_syntax_safe_start(lang, h->cache, h->cache_len,
                                        vis_start, SAFE_START_WINDOW);

    nspans = (lang == LANG_NONE) ? 0
           : note_tokenize(lang, h->cache + scan_start,
                           (vis_end > scan_start) ? vis_end - scan_start : 0,
                           scan_start, h->spans, MAX_SPANS);

    SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&sel);
    SendMessageW(e, EM_GETSCROLLPOS, 0, (LPARAM)&scroll);

    /* Recolouring sets the control's modify flag, and its EN_CHANGE can
     * arrive after we have stopped suppressing.  Remember the real state and
     * put it back, so opening a file never leaves it looking edited. */
    was_mod = (int)SendMessageW(e, EM_GETMODIFY, 0, 0);

    tom = tom_open(e);
    if (tom) tom->lpVtbl->Undo(tom, tomSuspend, 0);

    h->suppress++;
    SendMessageW(e, WM_SETREDRAW, FALSE, 0);

    memset(&cf, 0, sizeof(cf));
    cf.cbSize  = sizeof(cf);
    cf.dwMask  = CFM_COLOR;
    cf.crTextColor = cr(h->theme.fg);

    all.cpMin = vis_start; all.cpMax = vis_end;
    SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&all);
    SendMessageW(e, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

    for (i = 0; i < nspans; i++) {
        CHARRANGE r;
        if (h->spans[i].start + h->spans[i].len <= vis_start) continue;
        if (h->spans[i].start >= vis_end) break;
        r.cpMin = h->spans[i].start;
        r.cpMax = h->spans[i].start + h->spans[i].len;
        if (r.cpMin < vis_start) r.cpMin = vis_start;
        if (r.cpMax > vis_end)   r.cpMax = vis_end;
        cf.crTextColor = cr(h->theme.tok[h->spans[i].kind]);
        SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&r);
        SendMessageW(e, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    }

    SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&sel);
    SendMessageW(e, EM_SETSCROLLPOS, 0, (LPARAM)&scroll);
    SendMessageW(e, EM_SETMODIFY, (WPARAM)(was_mod ? TRUE : FALSE), 0);
    SendMessageW(e, WM_SETREDRAW, TRUE, 0);
    h->suppress--;

    if (tom) {
        tom->lpVtbl->Undo(tom, tomResume, 0);
        tom->lpVtbl->Release(tom);
    }

    InvalidateRect(e, NULL, FALSE);
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
    if (h->gutter) InvalidateRect(h->gutter, NULL, FALSE);
}

void h_rehighlight(note_host *h)
{
    h->cache_valid = 0;
    queue_view(h);
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
    case WM_CONTEXTMENU: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (x == -1 && y == -1) {
            RECT rc; GetWindowRect(wnd, &rc);
            x = rc.left + 8; y = rc.top + 8;
        }
        TrackPopupMenu(g.ctxmenu, TPM_RIGHTBUTTON, x, y, 0, g.wnd, NULL);
        return 0;
    }
    case WM_VSCROLL:
    case WM_HSCROLL:
    case WM_MOUSEWHEEL:
    case WM_KEYDOWN:
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

