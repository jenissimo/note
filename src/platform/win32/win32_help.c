/* win32_help.c -- the key sheet
 *
 * Part of note's Win32 backend.  The shared state and the types every part of
 * it needs live in note_win32.h; see there for why the state is a header and
 * not a private static.
 *
 * F1 puts up a card in the theme's own colours listing every binding note
 * has.  It is the same kind of overlay as the command palette -- no caption,
 * no taskbar button, never focused -- but it is not a mode of it, because it
 * asks nothing: there is no query to type into and no row to choose, so the
 * only key it wants is the one that puts it away.
 *
 * What is on it comes from note_help_fill(), which reads the menu and
 * accelerator tables rather than a list written out again here.  A shortcut
 * that changes in those tables changes on this card, and a shortcut that is
 * only ever documented cannot exist.
 */

#include "note_win32.h"

#define HELP_MAX      96      /* rows the sheet can hold                  */
#define HELP_W       660      /* logical px, clamped to the window        */
#define HELP_PAD      16
#define HELP_ROW_H    21
#define HELP_HEAD_H   26      /* a group heading, with its space above it */
#define HELP_TITLE_H  38
#define HELP_COL_GAP  24
#define HELP_MIN_2COL 520     /* narrower than this and one column it is  */

static note_help_row g_help[HELP_MAX];
static int           g_nhelp;
static int           g_break;      /* first row of the second column      */
static int           g_cols;

/* "&Save &&As...\tCtrl+S" -> "Save &As...".  The mnemonic marker belongs to
 * the menus; on a sheet there is nothing to press a letter against. */
static void plain_label(const nchar *src, WCHAR *dst, int cap)
{
    int i = 0, j = 0;

    if (cap <= 0) return;
    while (src[i] && src[i] != (nchar)'\t' && j < cap - 1) {
        if (src[i] == (nchar)'&') {
            i++;
            if (!src[i]) break;
            if (src[i] != (nchar)'&') continue;
        }
        dst[j++] = (WCHAR)src[i++];
    }
    dst[j] = 0;
}

static int row_height(int i)
{
    return (g_help[i].group ? HELP_HEAD_H : 0) + HELP_ROW_H;
}

/* Where the second column starts.
 *
 * A group boundary first, because a heading at the foot of one column with its
 * rows in the next reads as two lists rather than one.  But a single group can
 * be longer than half the sheet -- Edit is -- and then the tidiest boundary
 * still leaves a column taller than the window, and the end of the list is
 * simply lost.  A break that fits beats a break that is tidy, so when no
 * boundary fits, the split goes wherever it balances and the column that
 * starts mid-group is given its heading back by help_draw_column(). */
static void help_split(int room)
{
    int total = 0, run = 0, i, best = -1, best_max = 0;

    for (i = 0; i < g_nhelp; i++) total += px(row_height(i));

    if (g_cols == 1) { g_break = g_nhelp; return; }

    for (i = 0; i < g_nhelp; i++) {
        if (g_help[i].group && i > 0) {
            int tall = (run > total - run) ? run : total - run;
            if (best < 0 || tall < best_max) { best = i; best_max = tall; }
        }
        run += px(row_height(i));
    }

    if (best < 0 || (room > 0 && best_max > room)) {
        int any = -1, any_max = 0;
        run = 0;
        for (i = 0; i < g_nhelp - 1; i++) {
            int tall;
            run += px(row_height(i));
            tall = (run > total - run) ? run : total - run;
            if (any < 0 || tall < any_max) { any = i + 1; any_max = tall; }
        }
        if (any > 0) best = any;
    }

    g_break = (best > 0 && best < g_nhelp) ? best : (g_nhelp + 1) / 2;
}

static int column_height(int from, int to)
{
    int i, n = 0;
    for (i = from; i < to; i++) n += px(row_height(i));
    return n;
}

static void help_layout(note_host *h)
{
    RECT  rc;
    POINT o;
    int   w, ht, avail;

    if (!h->help) return;

    GetClientRect(h->wnd, &rc);
    o.x = 0; o.y = 0;
    ClientToScreen(h->wnd, &o);

    w = px(HELP_W);
    if (w > rc.right - px(HELP_PAD) * 2) w = rc.right - px(HELP_PAD) * 2;
    if (w < px(240)) w = px(240);

    g_cols = (w >= px(HELP_MIN_2COL)) ? 2 : 1;
    avail  = rc.bottom - h->tabs_h - px(HELP_PAD) * 2;
    /* What is left for the rows once the title has had its band. */
    help_split(avail - px(HELP_TITLE_H) - px(HELP_PAD));

    ht = column_height(0, g_break);
    if (g_cols == 2) {
        int r = column_height(g_break, g_nhelp);
        /* Room for the heading the second column may have to carry over. */
        if (g_break < g_nhelp && !g_help[g_break].group) r += px(HELP_HEAD_H);
        if (r > ht) ht = r;
    }
    ht += px(HELP_TITLE_H) + px(HELP_PAD);

    if (ht > avail && avail > px(120)) ht = avail;

    MoveWindow(h->help, o.x + (rc.right - w) / 2,
               o.y + h->tabs_h + (rc.bottom - h->tabs_h - ht) / 3,
               w, ht, TRUE);
}

/* One column of the sheet, from row `from` up to but not including `to`. */
static void help_draw_column(note_host *h, HDC dc, int from, int to,
                             int x, int w, int y, int bottom)
{
    WCHAR  text[NOTE_PALETTE_LABEL * 2];
    HBRUSH cap;
    int    i;

    cap = CreateSolidBrush(mix_rgb(h->theme.ui_fg, h->theme.ui_bg, 1, 8));

    /* Carried over: the split may have landed inside a group, and a column of
     * rows with nothing saying what they belong to is worse than a heading
     * written twice. */
    if (from > 0 && from < g_nhelp && !g_help[from].group) {
        int  k;
        RECT gr;
        for (k = from; k >= 0; k--) if (g_help[k].group) break;
        if (k >= 0) {
            gr.left = x; gr.right = x + w;
            gr.top  = y; gr.bottom = y + px(HELP_HEAD_H);
            plain_label(g_help[k].group, text, NOTE_PALETTE_LABEL * 2);
            SetTextColor(dc, cr(h->theme.tok[TOK_KEYWORD]));
            os_draw_text(dc, text, -1, &gr,
                      DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
            y += px(HELP_HEAD_H);
        }
    }

    for (i = from; i < to && y < bottom; i++) {
        RECT r;
        SIZE sz;
        int  klen;

        if (g_help[i].group) {
            RECT gr;
            gr.left = x; gr.right = x + w;
            gr.top  = y; gr.bottom = y + px(HELP_HEAD_H);
            plain_label(g_help[i].group, text, NOTE_PALETTE_LABEL * 2);
            SetTextColor(dc, cr(h->theme.tok[TOK_KEYWORD]));
            os_draw_text(dc, text, -1, &gr,
                      DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
            y += px(HELP_HEAD_H);
            if (y >= bottom) break;
        }

        r.left = x; r.right = x + w;
        r.top  = y; r.bottom = y + px(HELP_ROW_H);

        /* The key first, as a cap on the right: it is what the eye is looking
         * for, and giving it a fixed edge keeps the column readable when the
         * labels are of every length. */
        klen = n_len(g_help[i].keys);
        GetTextExtentPoint32W(dc, (LPCWSTR)g_help[i].keys, klen, &sz);
        {
            RECT kr = r;
            kr.left   = r.right - sz.cx - px(10);
            kr.top    = r.top + px(2);
            kr.bottom = r.bottom - px(2);
            if (kr.left < r.left + px(40)) kr.left = r.left + px(40);
            FillRect(dc, &kr, cap);
            SetTextColor(dc, cr(h->theme.ui_fg));
            os_draw_text(dc, g_help[i].keys, klen, &kr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            r.right = kr.left - px(8);
        }

        plain_label(g_help[i].label, text, NOTE_PALETTE_LABEL * 2);
        SetTextColor(dc, mix_rgb(h->theme.ui_fg, h->theme.ui_bg, 4, 5));
        os_draw_text(dc, text, -1, &r,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                  DT_END_ELLIPSIS);

        y += px(HELP_ROW_H);
    }

    DeleteObject(cap);
}

static void help_paint(note_host *h, HWND wnd)
{
    PAINTSTRUCT ps;
    HDC     dc, mem;
    HBITMAP bmp, oldbmp;
    RECT    rc, tr;
    HFONT   old;
    HBRUSH  br;
    int     colw, body;

    dc = BeginPaint(wnd, &ps);
    GetClientRect(wnd, &rc);

    mem    = CreateCompatibleDC(dc);
    bmp    = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    oldbmp = (HBITMAP)SelectObject(mem, bmp);

    FillRect(mem, &rc, h->br_ui);
    old = (HFONT)SelectObject(mem, h->menufont);
    SetBkMode(mem, TRANSPARENT);

    tr = rc;
    tr.left   = px(HELP_PAD);
    tr.right  = rc.right - px(HELP_PAD);
    tr.bottom = px(HELP_TITLE_H);
    SetTextColor(mem, cr(h->theme.ui_fg));
    os_draw_text(mem, N("Keyboard shortcuts"), -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(mem, blend_rgb(h->theme.ui_fg, h->theme.ui_bg));
    os_draw_text(mem, N("Esc"), -1, &tr,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    {   /* A rule under the title, the same one the palette draws. */
        RECT line = tr;
        line.left   = 0;
        line.right  = rc.right;
        line.top    = tr.bottom - 1;
        line.bottom = tr.bottom;
        FillRect(mem, &line, h->br_sep);
    }

    body = px(HELP_TITLE_H) + px(HELP_PAD) / 2;
    colw = (rc.right - px(HELP_PAD) * 2 -
            (g_cols == 2 ? px(HELP_COL_GAP) : 0)) / g_cols;

    help_draw_column(h, mem, 0, g_break, px(HELP_PAD), colw, body, rc.bottom);
    if (g_cols == 2)
        help_draw_column(h, mem, g_break, g_nhelp,
                         px(HELP_PAD) + colw + px(HELP_COL_GAP), colw,
                         body, rc.bottom);

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

void help_close(note_host *h)
{
    if (!h->help_open) return;
    h->help_open = 0;
    if (h->help) ShowWindow(h->help, SW_HIDE);
}

void help_show(note_host *h)
{
    /* Up already?  F1 twice in a row means "put it away". */
    if (h->help_open) { help_close(h); return; }

    if (!h->help) {
        h->help = os_create_window(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                   N("noteHelp"), N(""), WS_POPUP,
                                   0, 0, 10, 10, h->wnd, NULL, h->inst, NULL);
        if (!h->help) return;
    }

    /* The bar, the palette and the sheet are three answers to one question. */
    menubar_show(h, 0);
    pal_close(h);

    g_nhelp = note_help_fill(g_help, HELP_MAX);
    h->help_open = 1;

    help_layout(h);
    ShowWindow(h->help, SW_SHOWNOACTIVATE);
    InvalidateRect(h->help, NULL, FALSE);
}

/* Returns 1 when the key was the sheet's.  Anything at all puts it away --
 * it is a thing you glance at, not a thing you navigate -- but only Esc and
 * F1 are swallowed; a key meant for the editor still gets there. */
int help_key(note_host *h, int vk)
{
    if (!h->help_open) return 0;
    help_close(h);
    return vk == VK_ESCAPE || vk == VK_F1;
}

LRESULT CALLBACK HelpProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;                          /* WM_PAINT covers every pixel */

    case WM_PAINT:
        help_paint(&g, wnd);
        return 0;

    /* Clicking it must not pull activation away from the frame. */
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        help_close(&g);
        return 0;
    }
    return os_defproc(wnd, msg, wp, lp);
}
