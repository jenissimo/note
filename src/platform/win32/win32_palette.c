/* win32_palette.c -- the command palette overlay and its list modes
 *
 * Part of note's Win32 backend.  The shared state and the types every part of
 * it needs live in note_win32.h; see there for why the state is a header and
 * not a private static.
 */

#include "note_win32.h"

/* -------------------------------------------------------------------------
 * The command palette
 *
 * An overlay note paints itself rather than a dialog the window manager
 * frames: no caption, no taskbar button, and WS_EX_NOACTIVATE so the editor
 * keeps its active-title look while the palette is up.  Nothing ever focuses
 * it, which is why the message loop is what feeds it keys.
 *
 * What is in the list, how a query filters it and where the matched letters
 * fell is all note_palette.c's; here there is a window and a paint routine.
 *
 * The same overlay is also every list that used to be a dialog.  A mode is a
 * title, something that fills the rows, and two callbacks — one to show a row
 * while it is merely selected, one to keep it — so adding the next list is a
 * table entry rather than another DLGTEMPLATE.
 * ------------------------------------------------------------------------- */

#define PAL_W        520      /* logical px, clamped to the window */
#define PAL_ROW_H     24
#define PAL_INPUT_H   32
#define PAL_ROWS      10      /* rows on screen at once */
#define PAL_PAD        8
#define PAL_TOP       56      /* down from the top of the client area */

/* One mode more than note_win32.h's enum names.  Rename arrived with this
 * change and its enumerator belongs beside the others, but that header is
 * another author's this pass, so the mode is numbered here and reached
 * through pal_open_rename() rather than by name through pal_open_mode(). */
#define PAL_MODE_RENAME  PAL_MODE_COUNT
#define PAL_MODE_TOTAL  (PAL_MODE_COUNT + 1)

static note_palette g_pal;

void pal_close(note_host *h);

static int pal_rows_shown(void)
{
    int n = note_palette_count(&g_pal);
    return n > PAL_ROWS ? PAL_ROWS : n;
}

/* ---- the modes ----------------------------------------------------------
 *
 * fill() puts the rows in, preview() is called whenever the selection (or,
 * for a mode that reads a number, the query) moves, commit() keeps what is
 * selected and cancel() puts back whatever preview() has been changing.
 * Every one of them may be NULL.
 * ------------------------------------------------------------------------- */

typedef struct {
    const WCHAR *title;       /* drawn ahead of the query, or NULL           */
    const WCHAR *hint;        /* placeholder while nothing has been typed    */
    const WCHAR *empty;       /* when the query matches nothing              */
    void (*fill)   (note_host *h);
    void (*enter)  (note_host *h);                        /* what to restore */
    void (*preview)(note_host *h, const note_pal_row *r);
    /* Returns 1 when the palette is done and may close, 0 to stay open —
     * which is how a rename that would clobber a file reports itself. */
    int  (*commit) (note_host *h, const note_pal_row *r);
    void (*cancel) (note_host *h);
    int   typed;              /* the query is the answer, not a filter       */
    int   digits;             /* ...and it may only be digits                */
} pal_mode;

/* --- commands --- */

static void pal_fill_cmds(note_host *h)
{
    (void)h;
    note_palette_commands(&g_pal);
}

static int pal_run_cmd(note_host *h, const note_pal_row *r)
{
    if (r && r->id && note_command(&h->app, (int)r->id)) sync_menu(h);
    return 1;
}

/* --- themes --- */

static void pal_fill_theme(note_host *h)
{
    int i, n = note_theme_count();
    (void)h;

    note_palette_reset(&g_pal);
    /* Borrowed, not copied: the registry owns these names for the life of the
     * process, and 338 of them would not fit in the palette's pool. */
    for (i = 0; i < n; i++)
        note_palette_add(&g_pal, (unsigned)i, note_theme_get(i)->name, 0);
    note_palette_filter(&g_pal);
}

static void pal_enter_theme(note_host *h)
{
    h->pal_theme_prev = h->app.theme_index;
}

/* A palette is a thing you look at, so moving the selection paints the editor
 * rather than describing it.  Only this backend's copy changes; the core still
 * holds what the user had chosen, which is what cancelling puts back. */
static void pal_preview_theme(note_host *h, const note_pal_row *r)
{
    if (!r) return;
    h_set_theme(h, (int)r->id);
    service_view(h);
}

static int pal_commit_theme(note_host *h, const note_pal_row *r)
{
    if (r) note_set_theme_index(&h->app, (int)r->id);
    return 1;
}

static void pal_cancel_theme(note_host *h)
{
    /* Including the common case of nothing pinned at all (theme_index < 0,
     * following the system or a light/dark preset). */
    h->app.theme_index = h->pal_theme_prev;
    note_apply_theme(&h->app);
    service_view(h);
}

/* --- fonts --- */

static void pal_set_face(note_host *h, const WCHAR *face)
{
    int i;

    for (i = 0; face[i] && i < LF_FACESIZE - 1; i++) h->font.lfFaceName[i] = face[i];
    h->font.lfFaceName[i] = 0;

    measure_font(h);
    for (i = 0; i < NOTE_MAX_DOCS; i++)
        if (h->d[i].edit) apply_font_to(h->d[i].edit, h);
    service_view(h);
}

static int CALLBACK pal_font_enum(const LOGFONTW *lf, const TEXTMETRICW *tm,
                                  DWORD type, LPARAM lp)
{
    int i;
    (void)type; (void)lp;

    /* TMPF_FIXED_PITCH is set for the variable-pitch fonts — the flag is named
     * after the bit, not after what it means.  "@Face" is the same font laid
     * out for vertical CJK text and has no business in an editor. */
    if (tm->tmPitchAndFamily & TMPF_FIXED_PITCH) return 1;
    if (lf->lfFaceName[0] == L'@') return 1;

    /* One family is enumerated once per character set it covers. */
    for (i = 0; i < g_pal.nrows; i++)
        if (n_eq(g_pal.rows[i].label, (const nchar *)lf->lfFaceName)) return 1;

    return note_palette_add_copy(&g_pal, 0, (const nchar *)lf->lfFaceName, 0)
           ? 1 : 0;
}

static void pal_fill_font(note_host *h)
{
    LOGFONTW want;
    HDC dc;

    note_palette_reset(&g_pal);

    memset(&want, 0, sizeof(want));
    want.lfCharSet = DEFAULT_CHARSET;

    dc = GetDC(h->wnd);
    EnumFontFamiliesExW(dc, &want, pal_font_enum, 0, 0);
    ReleaseDC(h->wnd, dc);

    note_palette_filter(&g_pal);
}

static void pal_enter_font(note_host *h)
{
    int i;
    for (i = 0; i < LF_FACESIZE; i++) h->pal_face_prev[i] = h->font.lfFaceName[i];
}

static void pal_preview_font(note_host *h, const note_pal_row *r)
{
    if (r) pal_set_face(h, (const WCHAR *)r->label);
}

static void pal_cancel_font(note_host *h)
{
    pal_set_face(h, h->pal_face_prev);
}

/* --- go to line --- */

static void pal_enter_line(note_host *h)
{
    CHARRANGE c;
    SendMessageW(active_edit(), EM_EXGETSEL, 0, (LPARAM)&c);
    h->pal_caret_prev = (int)c.cpMin;
}

/* Nothing is selected here: the query itself is the answer, so the preview
 * runs on every digit and the row list stays empty. */
static void pal_preview_line(note_host *h, const note_pal_row *r)
{
    unsigned line = note_palette_number(&g_pal);
    (void)r;
    if (line) h_goto_line(h, (int)line);
}

static int pal_commit_line(note_host *h, const note_pal_row *r)
{
    unsigned line = note_palette_number(&g_pal);
    (void)r;
    if (line) h_goto_line(h, (int)line);
    return 1;
}

static void pal_cancel_line(note_host *h)
{
    h_sel_set(h, h->pal_caret_prev, h->pal_caret_prev);
    SendMessageW(active_edit(), EM_SCROLLCARET, 0, 0);
}

static void pal_fill_none(note_host *h)
{
    (void)h;
    note_palette_reset(&g_pal);
    note_palette_filter(&g_pal);
}

/* --- the open tabs ---
 *
 * Shift+Shift is "where do I go", so it lists the documents rather than the
 * commands.  note has no project to search yet; when it has one, this mode
 * gains its files and nothing else about it changes. */

/* The folder a document is in, which is what tells two main.c apart. */
static void folder_of(const nchar *path, nchar *out, int cap)
{
    int end, start;

    out[0] = 0;
    end = n_len(path);
    while (end > 0 && path[end - 1] != (nchar)'\\' && path[end - 1] != (nchar)'/')
        end--;
    if (end <= 1) return;                       /* no folder, or the root */
    end--;                                      /* off the separator */
    start = end;
    while (start > 0 && path[start - 1] != (nchar)'\\' && path[start - 1] != (nchar)'/')
        start--;

    {
        int i = 0;
        while (start + i < end && i < cap - 1) { out[i] = path[start + i]; i++; }
        out[i] = 0;
    }
}

static void pal_fill_tabs(note_host *h)
{
    int i;

    note_palette_reset(&g_pal);
    for (i = 0; i < h->app.ndocs; i++) {
        nchar label[NOTE_PALETTE_LABEL], folder[NOTE_PALETTE_ACCEL];
        /* The core's own tab title, so a modified document is starred here
         * exactly as it is on the strip. */
        note_doc_title(&h->app, i, label, NOTE_PALETTE_LABEL);
        folder_of(h->app.docs[i].path, folder, NOTE_PALETTE_ACCEL);
        note_palette_add_copy(&g_pal, (unsigned)i, label, folder);
    }
    note_palette_filter(&g_pal);
}

static int pal_commit_tab(note_host *h, const note_pal_row *r)
{
    if (r) note_select_doc(&h->app, (int)r->id);
    return 1;
}

/* --- rename ---
 *
 * The name is the answer, not a filter, so there are no rows: the mode is the
 * input line with the file's own name already in it.  Everything about what a
 * typed name means — bare name or move, unchanged, already taken — is
 * note_rename()'s, so a second backend gets the same rules for free. */

static void pal_fill_rename(note_host *h)
{
    const nchar *base = note_basename(h->app.docs[h->app.active].path);
    int i, stem = n_len(base);

    note_palette_reset(&g_pal);

    /* Selected as far as the last dot, so typing straight away replaces the
     * stem and keeps the extension.  A leading dot is the whole name of a
     * dotfile rather than the start of an extension, hence i > 0. */
    for (i = stem - 1; i > 0; i--)
        if (base[i] == (nchar)'.') { stem = i; break; }

    note_palette_set(&g_pal, base, stem);
}

/* Returns what note_rename returns: 0 leaves the overlay up with the reason
 * on the input line, so the name can be corrected rather than retyped. */
static int pal_commit_rename(note_host *h, const note_pal_row *r)
{
    (void)r;
    return note_rename(&h->app, h->app.active, note_palette_query(&g_pal));
}

static const pal_mode kPalModes[PAL_MODE_TOTAL] = {
    { NULL,       L"Type a command",  L"No matching command",
      pal_fill_cmds,  NULL,            NULL,
      pal_run_cmd,    NULL,            0, 0 },

    { L"Theme",   L"Filter palettes", L"No matching palette",
      pal_fill_theme, pal_enter_theme, pal_preview_theme,
      pal_commit_theme, pal_cancel_theme, 0, 0 },

    { L"Font",    L"Filter faces",    L"No matching face",
      pal_fill_font,  pal_enter_font,  pal_preview_font,
      NULL,           pal_cancel_font, 0, 0 },

    { L"Go to line", L"Line number",  L"Type a line number",
      pal_fill_none,  pal_enter_line,  pal_preview_line,
      pal_commit_line, pal_cancel_line, 1, 1 },

    { L"Go to tab", L"Filter open tabs", L"No matching tab",
      pal_fill_tabs,  NULL,            NULL,
      pal_commit_tab, NULL,            0, 0 },

    { L"Rename",  L"New file name",   L"Enter returns the new name",
      pal_fill_rename, NULL,           NULL,
      pal_commit_rename, NULL,         1, 0 }
};

static const pal_mode *pal_cur(note_host *h)
{
    return &kPalModes[(h->pal_mode >= 0 && h->pal_mode < PAL_MODE_TOTAL)
                      ? h->pal_mode : PAL_MODE_CMDS];
}

/* Near the top and horizontally centred over the editor, growing and
 * shrinking with the number of matches. */
void pal_layout(note_host *h)
{
    RECT  rc;
    POINT o;
    int   w, ht;

    if (!h->pal) return;

    GetClientRect(h->wnd, &rc);
    o.x = 0; o.y = 0;
    ClientToScreen(h->wnd, &o);

    w = px(PAL_W);
    if (w > rc.right - px(PAL_PAD) * 2) w = rc.right - px(PAL_PAD) * 2;
    if (w < px(220)) w = px(220);

    /* An empty list still gets one row's worth of height: that is where the
     * "nothing matches" line goes, and a mode that has no rows at all — go to
     * line — says what it wants there. */
    ht = px(PAL_INPUT_H) + px(PAL_PAD) / 2 +
         (pal_rows_shown() ? pal_rows_shown() : 1) * px(PAL_ROW_H);

    MoveWindow(h->pal, o.x + (rc.right - w) / 2, o.y + px(PAL_TOP), w, ht, TRUE);
}

static void pal_reveal(note_host *h)
{
    int n = note_palette_count(&g_pal);

    if (h->pal_sel >= n) h->pal_sel = n - 1;
    if (h->pal_sel < 0)  h->pal_sel = 0;

    if (h->pal_sel < h->pal_top)             h->pal_top = h->pal_sel;
    if (h->pal_sel >= h->pal_top + PAL_ROWS) h->pal_top = h->pal_sel - PAL_ROWS + 1;
    if (h->pal_top < 0) h->pal_top = 0;
}

/* Draws one label, colouring the characters the filter matched.  Runs of the
 * same colour go out together so the text still kerns as a word rather than
 * as a column of letters. */
static void pal_draw_label(HDC dc, const nchar *s, const unsigned char *mark,
                           int nmark, int x, int y, int right,
                           COLORREF plain, COLORREF hit)
{
    int i = 0, m = 0, len = n_len(s);

    while (i < len && x < right) {
        int  on  = (m < nmark && mark[m] == (unsigned char)i);
        int  run = 1;
        SIZE sz;

        if (on) {
            m++;
            while (m < nmark && mark[m] == (unsigned char)(i + run)) { run++; m++; }
        } else {
            while (i + run < len &&
                   !(m < nmark && mark[m] == (unsigned char)(i + run))) run++;
        }

        SetTextColor(dc, on ? hit : plain);
        TextOutW(dc, x, y, (LPCWSTR)(s + i), run);
        GetTextExtentPoint32W(dc, (LPCWSTR)(s + i), run, &sz);
        x += sz.cx;
        i += run;
    }
}

static void pal_paint(note_host *h, HWND wnd)
{
    PAINTSTRUCT ps;
    HDC     dc, mem;
    HBITMAP bmp, oldbmp;
    RECT    rc, band, line;
    HFONT   old;
    HBRUSH  br;
    TEXTMETRICW tm;
    int     i, rows, y, textx;

    dc = BeginPaint(wnd, &ps);
    GetClientRect(wnd, &rc);

    /* Off-screen: every keystroke repaints the whole panel. */
    mem    = CreateCompatibleDC(dc);
    bmp    = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    oldbmp = (HBITMAP)SelectObject(mem, bmp);

    FillRect(mem, &rc, h->br_ui);

    band = rc;
    band.bottom = px(PAL_INPUT_H);
    FillRect(mem, &band, h->br_edit);

    old = (HFONT)SelectObject(mem, h->menufont);
    GetTextMetricsW(mem, &tm);
    SetBkMode(mem, TRANSPARENT);
    textx = px(PAL_PAD) + px(4);

    {   /* The query, and a caret of our own: an overlay that never takes
         * focus gets no caret from the system. */
        const pal_mode *m  = pal_cur(h);
        const nchar    *q  = note_palette_query(&g_pal);
        RECT qr = band;
        SIZE sz;

        qr.left  = textx;
        qr.right = rc.right - px(PAL_PAD);
        sz.cx = 0;

        /* A refusal the core wants read — "that name is taken" — sits at the
         * right-hand end of the input line, and the query gives up the room
         * rather than running underneath it. */
        if (h->pal_msg[0]) {
            SIZE ms;
            RECT mr = band;
            int  mlen = n_len((const nchar *)h->pal_msg);

            GetTextExtentPoint32W(mem, h->pal_msg, mlen, &ms);
            mr.left = qr.right - ms.cx;
            if (mr.left < qr.left) mr.left = qr.left;
            SetTextColor(mem, cr(h->theme.tok[TOK_NUMBER]));
            DrawTextW(mem, h->pal_msg, -1, &mr,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            qr.right = mr.left - px(10);
        }

        /* Which list this is, ahead of the query — the overlay is the same
         * window whatever it is showing, so it has to say. */
        if (m->title) {
            SIZE ts;
            SetTextColor(mem, cr(h->theme.tok[TOK_KEYWORD]));
            DrawTextW(mem, m->title, -1, &qr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            GetTextExtentPoint32W(mem, m->title, n_len((const nchar *)m->title), &ts);
            qr.left += ts.cx + px(10);
        }

        if (q[0]) {
            int a, b;

            /* The selection goes down first, in the editor's own selection
             * colour, so the input line reads as one. */
            if (note_palette_sel(&g_pal, &a, &b)) {
                RECT sr;
                SIZE pre, sel;
                GetTextExtentPoint32W(mem, (LPCWSTR)q, a, &pre);
                GetTextExtentPoint32W(mem, (LPCWSTR)(q + a), b - a, &sel);
                sr.left   = qr.left + pre.cx;
                sr.right  = sr.left + sel.cx;
                sr.top    = band.top + px(6);
                sr.bottom = band.bottom - px(6);
                FillRect(mem, &sr, h->br_menusel);
            }

            SetTextColor(mem, cr(h->theme.fg));
            DrawTextW(mem, (LPCWSTR)q, -1, &qr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            /* The caret sits where the caret is, not at the end. */
            GetTextExtentPoint32W(mem, (LPCWSTR)q, note_palette_caret(&g_pal), &sz);
        } else {
            SetTextColor(mem, blend_rgb(h->theme.fg, h->theme.bg));
            DrawTextW(mem, m->hint, -1, &qr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        line.left   = qr.left + sz.cx + px(1);
        line.right  = line.left + (px(1) < 1 ? 1 : px(1));
        line.top    = band.top + px(7);
        line.bottom = band.bottom - px(7);
        br = CreateSolidBrush(cr(h->theme.fg));
        FillRect(mem, &line, br);
        DeleteObject(br);
    }

    line = band;
    line.top = line.bottom - 1;
    FillRect(mem, &line, h->br_sep);

    rows = pal_rows_shown();
    y    = px(PAL_INPUT_H);

    for (i = 0; i < rows; i++) {
        int idx = h->pal_top + i;
        const note_pal_row *c = note_palette_at(&g_pal, idx);
        unsigned char mark[NOTE_PALETTE_LABEL];
        RECT r;
        int  nmark;

        if (!c) break;

        r.left = 0; r.right = rc.right;
        r.top  = y; r.bottom = y + px(PAL_ROW_H);
        if (idx == h->pal_sel) FillRect(mem, &r, h->br_menusel);

        nmark = note_palette_marks(c->label, note_palette_query(&g_pal), mark,
                                   NOTE_PALETTE_LABEL);
        pal_draw_label(mem, c->label, mark, nmark,
                       textx, r.top + (px(PAL_ROW_H) - tm.tmHeight) / 2,
                       rc.right - px(96),
                       cr(h->theme.ui_fg), cr(h->theme.tok[TOK_KEYWORD]));

        /* The accelerator on the right, as the menus show it: the palette is
         * meant to teach the shortcut, not to replace it.  A mode's rows are
         * names, not commands, and carry none. */
        if (c->accel && c->accel[0]) {
            RECT ar = r;
            ar.right -= textx;
            SetTextColor(mem, blend_rgb(h->theme.ui_fg, h->theme.ui_bg));
            DrawTextW(mem, (LPCWSTR)c->accel, -1, &ar,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        y = r.bottom;
    }

    if (!rows) {
        RECT nr = rc;
        nr.top    = px(PAL_INPUT_H);
        nr.bottom = nr.top + px(PAL_ROW_H);
        nr.left   = textx;
        SetTextColor(mem, blend_rgb(h->theme.ui_fg, h->theme.ui_bg));
        DrawTextW(mem, pal_cur(h)->empty, -1, &nr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    /* One hairline in the theme's own colours, so the panel reads as a card
     * over the editor without a system frame around it. */
    br = CreateSolidBrush(blend_rgb(h->theme.ui_fg, h->theme.ui_bg));
    FrameRect(mem, &rc, br);
    DeleteObject(br);

    SelectObject(mem, old);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteDC(mem);

    EndPaint(wnd, &ps);
}

/* Shows the selected row in the editor itself.  A mode that reads a number
 * has no selection: it previews whatever has been typed. */
static void pal_preview(note_host *h)
{
    const pal_mode *m = pal_cur(h);
    if (!m->preview) return;
    m->preview(h, m->typed ? (const note_pal_row *)0
                           : note_palette_at(&g_pal, h->pal_sel));
}

static void pal_refilter(note_host *h)
{
    /* The complaint was about the text as it stood; it has just changed. */
    h->pal_msg[0] = 0;
    note_palette_filter(&g_pal);
    h->pal_sel = 0;
    h->pal_top = 0;
    pal_layout(h);
    InvalidateRect(h->pal, NULL, FALSE);
    pal_preview(h);
}

/* Hides the overlay without undoing anything: the caller either committed or
 * has already put its preview back. */
static void pal_hide(note_host *h)
{
    if (!h->pal_open) return;
    h->pal_open = 0;
    if (h->pal) ShowWindow(h->pal, SW_HIDE);
}

static void pal_run(note_host *h, int row)
{
    const pal_mode     *m = pal_cur(h);
    const note_pal_row *c = note_palette_at(&g_pal, row);

    if (!m->typed && !c) return;

    /* Hidden first: a command that opens a dialog — or reopens this overlay in
     * another mode — should not do it behind the palette.  A commit that
     * refuses (a name that would clobber a file) brings it straight back. */
    pal_hide(h);
    if (m->commit && !m->commit(h, c)) {
        h->pal_open = 1;
        pal_layout(h);
        ShowWindow(h->pal, SW_SHOWNOACTIVATE);
        InvalidateRect(h->pal, NULL, FALSE);
    }
}

static int pal_row_at(note_host *h, int y)
{
    int row;
    if (y < px(PAL_INPUT_H)) return -1;
    row = h->pal_top + (y - px(PAL_INPUT_H)) / px(PAL_ROW_H);
    if (row < h->pal_top || row >= h->pal_top + PAL_ROWS) return -1;
    if (row >= note_palette_count(&g_pal)) return -1;
    return row;
}

/* The input line draws itself, so it does not get the clipboard for free
 * either.  Both directions go through note_palette, which is where the
 * selection lives. */
static void pal_clip_copy(note_host *h, int cut)
{
    nchar   buf[NOTE_PALETTE_QUERY];
    HGLOBAL mem;
    int     n = note_palette_selected(&g_pal, buf, NOTE_PALETTE_QUERY);

    if (!n || !OpenClipboard(h->wnd)) return;
    EmptyClipboard();
    mem = GlobalAlloc(GMEM_MOVEABLE, (DWORD)(n + 1) * sizeof(WCHAR));
    if (mem) {
        WCHAR *p = (WCHAR *)GlobalLock(mem);
        if (p) {
            n_copy((nchar *)p, buf, n + 1);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
    }
    CloseClipboard();

    if (cut && note_palette_edit(&g_pal, PAL_ED_BACK, 0)) pal_refilter(h);
}

static void pal_clip_paste(note_host *h)
{
    HANDLE  hnd;
    int     changed = 0;

    if (!OpenClipboard(h->wnd)) return;
    hnd = GetClipboardData(CF_UNICODETEXT);
    if (hnd) {
        const WCHAR *s = (const WCHAR *)GlobalLock(hnd);
        if (s) {
            if (pal_cur(h)->digits) {
                /* A line number is a line number however it arrived. */
                nchar only[NOTE_PALETTE_QUERY];
                int i = 0, j = 0;
                while (s[i] && j < NOTE_PALETTE_QUERY - 1)
                    if (s[i] >= L'0' && s[i] <= L'9') only[j++] = (nchar)s[i++];
                    else i++;
                only[j] = 0;
                changed = note_palette_insert(&g_pal, only);
            } else {
                changed = note_palette_insert(&g_pal, (const nchar *)s);
            }
            GlobalUnlock(hnd);
        }
    }
    CloseClipboard();
    if (changed) pal_refilter(h);
}

/* Returns 1 when the key was the palette's; the loop then does not let the
 * accelerator table or the editor see it. */
int pal_key(note_host *h, int vk)
{
    int ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    int shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    int op    = -1;

    /* The input line first: Up and Down belong to the list, everything else
     * that an edit field answers belongs to the text. */
    switch (vk) {
    case VK_LEFT:   op = ctrl ? PAL_ED_WORD_LEFT  : PAL_ED_LEFT;  break;
    case VK_RIGHT:  op = ctrl ? PAL_ED_WORD_RIGHT : PAL_ED_RIGHT; break;
    case VK_HOME:   op = PAL_ED_HOME;   break;
    case VK_END:    op = PAL_ED_END;    break;
    case VK_BACK:   op = ctrl ? PAL_ED_BACK_WORD   : PAL_ED_BACK;   break;
    case VK_DELETE: op = ctrl ? PAL_ED_DELETE_WORD : PAL_ED_DELETE; break;
    case 'A':       if (ctrl) op = PAL_ED_ALL;  break;
    case 'Z':       if (ctrl) op = PAL_ED_UNDO; break;
    case 'C':       if (ctrl) { pal_clip_copy(h, 0); return 1; } break;
    case 'X':       if (ctrl) { pal_clip_copy(h, 1); return 1; } break;
    case 'V':       if (ctrl) { pal_clip_paste(h);   return 1; } break;
    default: break;
    }

    if (op >= 0) {
        if (note_palette_edit(&g_pal, op, shift)) pal_refilter(h);
        else InvalidateRect(h->pal, NULL, FALSE);
        return 1;
    }

    switch (vk) {
    /* Esc leaves a mode for the command list it was reached from, and only
     * closes the overlay when there is nothing left to go back to. */
    case VK_ESCAPE:
        if (h->pal_mode != PAL_MODE_CMDS) {
            const pal_mode *m = pal_cur(h);
            if (m->cancel) m->cancel(h);
            pal_open_mode(h, PAL_MODE_CMDS);
        } else {
            pal_hide(h);
        }
        return 1;
    case VK_RETURN: pal_run(h, h->pal_sel);  return 1;
    case VK_UP:     h->pal_sel--;            break;
    case VK_DOWN:   h->pal_sel++;            break;
    case VK_PRIOR:  h->pal_sel -= PAL_ROWS;  break;
    case VK_NEXT:   h->pal_sel += PAL_ROWS;  break;
    default: return 0;
    }

    pal_reveal(h);
    InvalidateRect(h->pal, NULL, FALSE);
    pal_preview(h);
    return 1;
}

void pal_char(note_host *h, unsigned ch)
{
    /* A mode that asks for a line number takes digits and nothing else, so a
     * stray letter cannot leave the query meaning something it does not. */
    if (pal_cur(h)->digits && ch != 8 && (ch < '0' || ch > '9')) return;

    if (note_palette_type(&g_pal, ch)) pal_refilter(h);
}

void pal_open_mode(note_host *h, int mode)
{
    const pal_mode *m;

    if (mode < 0 || mode >= PAL_MODE_TOTAL) mode = PAL_MODE_CMDS;

    /* Whatever the last mode was told to say does not belong to this one. */
    h->pal_msg[0] = 0;

    if (!h->pal) {
        h->pal = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                 L"notePalette", L"", WS_POPUP,
                                 0, 0, 10, 10, h->wnd, NULL, h->inst, NULL);
        if (!h->pal) return;
    }

    /* The bar and the palette are two answers to the same question. */
    menubar_show(h, 0);

    h->pal_mode = mode;
    m = pal_cur(h);
    if (m->enter) m->enter(h);
    if (m->fill)  m->fill(h);

    h->pal_sel  = 0;
    h->pal_top  = 0;
    h->pal_open = 1;

    pal_layout(h);
    ShowWindow(h->pal, SW_SHOWNOACTIVATE);
    InvalidateRect(h->pal, NULL, FALSE);
}

void pal_show(note_host *h)
{
    if (h->pal_open) return;
    pal_open_mode(h, PAL_MODE_CMDS);
}

/* Named rather than numbered, because PAL_MODE_RENAME is local to this file
 * until the mode enum in note_win32.h gains it. */
void pal_open_rename(note_host *h)
{
    pal_open_mode(h, PAL_MODE_RENAME);
}

/* Dismissed rather than answered — a click elsewhere, the window losing
 * activation — so whatever the preview was showing goes back. */
void pal_close(note_host *h)
{
    const pal_mode *m;

    if (!h->pal_open) return;
    m = pal_cur(h);
    if (m->cancel) m->cancel(h);
    pal_hide(h);
}

LRESULT CALLBACK PaletteProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_ERASEBKGND:
        return 1;                          /* WM_PAINT covers every pixel */

    case WM_PAINT:
        pal_paint(&g, wnd);
        return 0;

    /* Clicking a row must not pull activation away from the frame. */
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_MOUSEMOVE: {
        int row = pal_row_at(&g, (short)HIWORD(lp));
        if (row >= 0 && row != g.pal_sel) {
            g.pal_sel = row;
            InvalidateRect(wnd, NULL, FALSE);
            pal_preview(&g);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int row = pal_row_at(&g, (short)HIWORD(lp));
        if (row >= 0) pal_run(&g, row);
        return 0;
    }

    case WM_MOUSEWHEEL:
        g.pal_sel += (GET_WHEEL_DELTA_WPARAM(wp) > 0) ? -3 : 3;
        pal_reveal(&g);
        InvalidateRect(wnd, NULL, FALSE);
        pal_preview(&g);
        return 0;
    }

    return DefWindowProcW(wnd, msg, wp, lp);
}

