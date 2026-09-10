/* win32_chrome.c -- tabs, the status bar, fonts and applying a theme
 *
 * Part of note's Win32 backend.  The shared state and the types every part of
 * it needs live in note_win32.h; see there for why the state is a header and
 * not a private static.
 */

#include "note_win32.h"

/* -------------------------------------------------------------------------
 * Capabilities
 * ------------------------------------------------------------------------- */

win_caps caps;

/* Resolved once, before the first window exists.  The DLLs are left loaded
 * for the life of the process, which is what the pointers into them require;
 * on a Windows that has them they were going to be mapped anyway. */
void caps_probe(void)
{
    HMODULE dwm = os_library(N("dwmapi.dll"));
    HMODULE ux  = os_library(N("uxtheme.dll"));

    if (dwm) {
        caps.dwm_set_attr    = (HRESULT (WINAPI *)(HWND, DWORD, LPCVOID, DWORD))
            GetProcAddress(dwm, "DwmSetWindowAttribute");
        caps.dwm_get_attr    = (HRESULT (WINAPI *)(HWND, DWORD, PVOID, DWORD))
            GetProcAddress(dwm, "DwmGetWindowAttribute");
        caps.dwm_composition = (HRESULT (WINAPI *)(BOOL *))
            GetProcAddress(dwm, "DwmIsCompositionEnabled");
        caps.dwm_extend      = (HRESULT (WINAPI *)(HWND, const MARGINS *))
            GetProcAddress(dwm, "DwmExtendFrameIntoClientArea");
        caps.dwm_defproc     = (BOOL (WINAPI *)(HWND, UINT, WPARAM, LPARAM, LRESULT *))
            GetProcAddress(dwm, "DwmDefWindowProc");
    }

    if (ux) {
        caps.set_window_theme = (HRESULT (WINAPI *)(HWND, LPCWSTR, LPCWSTR))
            GetProcAddress(ux, "SetWindowTheme");
        caps.set_app_mode     = (int (WINAPI *)(int))
            GetProcAddress(ux, MAKEINTRESOURCEA(135));
        caps.flush_themes     = (void (WINAPI *)(void))
            GetProcAddress(ux, MAKEINTRESOURCEA(136));
    }

    {
        HMODULE user = GetModuleHandleW(L"user32.dll");
        if (user) {
            caps.track_mouse   = (BOOL (WINAPI *)(TRACKMOUSEEVENT *))
                GetProcAddress(user, "TrackMouseEvent");
            caps.set_menu_info = (BOOL (WINAPI *)(HMENU, LPCMENUINFO))
                GetProcAddress(user, "SetMenuInfo");
            caps.menu_bar_info = (BOOL (WINAPI *)(HWND, LONG, LONG, PMENUBARINFO))
                GetProcAddress(user, "GetMenuBarInfo");
        }
    }

    caps.dwm_frame = caps.dwm_composition && caps.dwm_extend &&
                     caps.dwm_defproc     && caps.dwm_set_attr;
}

/* -------------------------------------------------------------------------
 * Tabs
 * ------------------------------------------------------------------------- */

void apply_font_to(HWND e, note_host *h);
#if !NOTE_OWN_VIEW
static void apply_theme_to(HWND e, note_host *h);

int h_tab_create(note_host *h, int doc)
{
    HWND e;

    if (doc < 0 || doc >= NOTE_MAX_DOCS) return 0;

    e = os_create_window(0, (const nchar *)MSFTEDIT_CLASS, N(""),
                         WS_CHILD | WS_VSCROLL | ES_MULTILINE |
                         ES_WANTRETURN | ES_NOHIDESEL | ES_AUTOVSCROLL,
                         0, 0, 0, 0, h->wnd, (HMENU)(UINT_PTR)(ID_EDIT0 + doc),
                         h->inst, NULL);
    if (!e) return 0;

    h->d[doc].edit    = e;
    h->d[doc].oldproc = os_set_wndproc(e, EditProc);

    os_send(e, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE | ENM_SCROLLEVENTS);
    os_send(e, EM_EXLIMITTEXT, 0, 0x7FFFFFF0);
    os_send(e, EM_SETTARGETDEVICE, 0, h->app.wrap ? 0 : 1);
    os_send(e, EM_SETZOOM, (WPARAM)h->app.zoom, (LPARAM)100);
    apply_font_to(e, h);
    apply_theme_to(e, h);

    h->d[doc].title[0] = 0;
    tabs_layout(h);
    if (h->tabs) InvalidateRect(h->tabs, NULL, FALSE);
    return 1;
}
#endif /* !NOTE_OWN_VIEW -- win32_view.c makes its own window instead */

void h_tab_destroy(note_host *h, int doc)
{
    int i;

    if (doc < 0 || doc >= NOTE_MAX_DOCS) return;
    if (h->d[doc].edit) DestroyWindow(h->d[doc].edit);

    /* Slide the editors down to match the core's compacted array. */
    for (i = doc; i < NOTE_MAX_DOCS - 1; i++) h->d[i] = h->d[i + 1];
    memset(&h->d[NOTE_MAX_DOCS - 1], 0, sizeof(win_doc));

    h->tab_hot = -1;
    h->cache_valid = 0;
    tabs_layout(h);
    if (h->tabs) InvalidateRect(h->tabs, NULL, FALSE);
}

void h_tab_select(note_host *h, int doc)
{
    int i;
    if (doc < 0 || doc >= NOTE_MAX_DOCS) return;

    for (i = 0; i < NOTE_MAX_DOCS; i++)
        if (h->d[i].edit) ShowWindow(h->d[i].edit, i == doc ? SW_SHOW : SW_HIDE);

    h->cache_valid = 0;
    relayout(h);
    if (h->tabs) InvalidateRect(h->tabs, NULL, FALSE);
    SetFocus(h->d[doc].edit);
    queue_view(h);
}

void h_tab_title(note_host *h, int doc, const nchar *title)
{
    if (doc < 0 || doc >= NOTE_MAX_DOCS) return;
    n_copy((nchar *)h->d[doc].title, title, 96);
    tabs_layout(h);
    if (h->tabs) InvalidateRect(h->tabs, NULL, FALSE);
}

/* -------------------------------------------------------------------------
 * Chrome
 * ------------------------------------------------------------------------- */

void h_set_title(note_host *h, const nchar *s)
{
    os_set_window_text(h->wnd, s);
}

void h_set_status(note_host *h, const nchar *s)
{
    if (!h->status) return;
    /* Repaint only when the text actually changed: most caret moves within a
     * line leave it identical. */
    if (n_eq((const nchar *)h->status_text, s)) return;
    n_copy((nchar *)h->status_text, s, 160);
    InvalidateRect(h->status, NULL, FALSE);
}

void h_show_status(note_host *h, int visible)
{
    ShowWindow(h->status, visible ? SW_SHOW : SW_HIDE);
    relayout(h);
}

#if !NOTE_OWN_VIEW
void h_set_wrap(note_host *h, int wrap)
{
    int i;
    h->suppress++;
    for (i = 0; i < NOTE_MAX_DOCS; i++)
        if (h->d[i].edit)
            os_send(h->d[i].edit, EM_SETTARGETDEVICE, 0, wrap ? 0 : 1);
    h->suppress--;
    queue_view(h);
}
#endif

void h_set_linenums(note_host *h, int on)
{
    ShowWindow(h->gutter, on ? SW_SHOW : SW_HIDE);
    queue_view(h);
}

#if !NOTE_OWN_VIEW
void h_set_zoom(note_host *h, int percent)
{
    int i;
    h->suppress++;
    for (i = 0; i < NOTE_MAX_DOCS; i++)
        if (h->d[i].edit)
            os_send(h->d[i].edit, EM_SETZOOM, (WPARAM)percent, (LPARAM)100);
    h->suppress--;
    queue_view(h);
}

void apply_font_to(HWND e, note_host *h)
{
    CHARFORMAT2W cf;
    int i = 0;

    memset(&cf, 0, sizeof(cf));
    cf.cbSize  = sizeof(cf);
    cf.dwMask  = CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_ITALIC;
    cf.yHeight = h->fontpt * 2;                  /* twips = 1/10pt * 2 */
    if (h->font.lfWeight >= FW_BOLD) cf.dwEffects |= CFE_BOLD;
    if (h->font.lfItalic)            cf.dwEffects |= CFE_ITALIC;
    while (h->font.lfFaceName[i] && i < LF_FACESIZE - 1) {
        cf.szFaceName[i] = h->font.lfFaceName[i];
        i++;
    }
    cf.szFaceName[i] = 0;

    h->suppress++;
    os_send(e, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    h->suppress--;
}
#endif /* !NOTE_OWN_VIEW -- the view answers all four of these itself */

#if !NOTE_OWN_VIEW
static void apply_theme_to(HWND e, note_host *h)
{
    CHARFORMAT2W cf;

    os_send(e, EM_SETBKGNDCOLOR, 0, (LPARAM)cr(h->theme.bg));

    memset(&cf, 0, sizeof(cf));
    cf.cbSize      = sizeof(cf);
    cf.dwMask      = CFM_COLOR;
    cf.crTextColor = cr(h->theme.fg);
    h->suppress++;
    os_send(e, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    h->suppress--;

    if (caps.set_window_theme)
        caps.set_window_theme(e, h->theme.dark ? L"DarkMode_Explorer"
                                               : L"Explorer", NULL);
}
#endif

void set_app_dark(int dark)
{
    if (caps.set_app_mode) caps.set_app_mode(dark ? 2 : 3);  /* Dark : Light */
    if (caps.flush_themes) caps.flush_themes();
}

/* The display's palette, once it has been asked for, and 0 when the display
 * has colours to spare and nothing needs snapping. */
static note_color g_pal[NOTE_PAL_MAX];
static int        g_npal;

/* The nearest palette entry, by the same weighted distance the core uses.
 * Called on colours that are computed rather than authored, so there is no
 * question of keeping anything distinct -- only of naming a colour the
 * display can actually produce, so GDI has nothing left to dither. */
COLORREF chrome_snap(COLORREF c)
{
    long best = -1;
    int  i, at = 0;
    long want_r = GetRValue(c), want_g = GetGValue(c), want_b = GetBValue(c);

    if (g_npal <= 0) return c;

    for (i = 0; i < g_npal; i++) {
        long pr = (long)((g_pal[i] >> 16) & 0xFF);
        long pg = (long)((g_pal[i] >>  8) & 0xFF);
        long pb = (long)( g_pal[i]        & 0xFF);
        long d  = 3 * (want_r - pr) * (want_r - pr) +
                  6 * (want_g - pg) * (want_g - pg) +
                      (want_b - pb) * (want_b - pb);
        if (best < 0 || d < best) { best = d; at = i; }
    }
    return RGB((g_pal[at] >> 16) & 0xFF, (g_pal[at] >> 8) & 0xFF,
               g_pal[at] & 0xFF);
}

/* Whether anything on this display needs snapping at all.  Zero above eight
 * bits per pixel, where the theme is showable as its author wrote it -- and
 * every correction that exists to survive a small palette has to be inert
 * when this is, or it would be nudging colours that were already right. */
int chrome_palettised(void)
{
    return g_npal > 0;
}

/* How far a colour is from grey: the spread between its channels. */
static int chrome_tint(long r, long g_, long b)
{
    long hi = r > g_ ? r : g_;
    long lo = r < g_ ? r : g_;

    if (b > hi) hi = b;
    if (b < lo) lo = b;
    return (int)(hi - lo);
}

/* The nearest palette entry, keeping a grey a grey.
 *
 * chrome_snap() answers with the nearest entry there is, and on the twenty a
 * Windows 95 desktop reserves at 8bpp the nearest entry to a light grey is not
 * a grey.  #DCDCDC -- which is what a sixth of the way from a white caption
 * towards its text comes to -- is nearer to #C0DFC0, the pale green of the
 * "money" set, than it is to #C0C0C0, by any distance you care to measure:
 * green is 223 against 192 and the ask was 220.
 *
 * Nearer is not better on a surface.  The eye reads hue long before it reads
 * lightness, so a scroll bar and three caption buttons the colour of mint are
 * wrong in a way that a shade of grey never is -- and they came out mint.
 *
 * So a colour asked for as a neutral is answered with a neutral, where the
 * palette has one at all.  Everything else -- text, tokens, the caption itself
 * -- goes on through chrome_snap() and takes the nearest entry there is,
 * because for those the nearest really is what is wanted. */
COLORREF chrome_snap_grey(COLORREF c)
{
    COLORREF got = chrome_snap(c);
    long want_r = GetRValue(c), want_g = GetGValue(c), want_b = GetBValue(c);
    long best = -1;
    int  i, at = -1;

    /* Only when a neutral was asked for and something tinted came back.  Eight
     * is the widest spread that still reads as grey; A0A0A4, the palette's own
     * dark grey, is four off and must not be ruled out. */
    if (g_npal <= 0) return got;
    if (chrome_tint(want_r, want_g, want_b) > 8) return got;
    if (chrome_tint(GetRValue(got), GetGValue(got), GetBValue(got)) <= 8)
        return got;

    for (i = 0; i < g_npal; i++) {
        long pr = (long)((g_pal[i] >> 16) & 0xFF);
        long pg = (long)((g_pal[i] >>  8) & 0xFF);
        long pb = (long)( g_pal[i]        & 0xFF);
        long d;

        if (chrome_tint(pr, pg, pb) > 8) continue;
        d = 3 * (want_r - pr) * (want_r - pr) +
            6 * (want_g - pg) * (want_g - pg) +
                (want_b - pb) * (want_b - pb);
        if (best < 0 || d < best) { best = d; at = i; }
    }
    if (at < 0) return got;          /* a palette with no greys in it at all */
    return RGB((g_pal[at] >> 16) & 0xFF, (g_pal[at] >> 8) & 0xFF,
               g_pal[at] & 0xFF);
}

/* Snaps the theme onto the colours the display can actually show.
 *
 * On a display with few colours, CreateSolidBrush given a colour that is not
 * in the palette does not return the nearest one -- it returns a *dithered*
 * brush, a checkerboard of the two entries either side.  Windows 95 on a
 * generic VGA driver has sixteen colours, and the gutter comes out visibly
 * hatched rather than merely the wrong grey.  Text is worse: distinct token
 * colours are mapped to whatever is nearest, and a keyword and a comment can
 * land on the same black.
 *
 * So the theme itself is rewritten to hold palette colours, rather than every
 * paint site being taught to translate.  Everything downstream keeps reading
 * h->theme and asking for exact colours, brushes come out solid, and GDI has
 * nothing left to approximate.  note_theme_reduce does the hard half: keeping
 * the token kinds apart matters more than keeping each of them faithful.
 *
 * Above eight bits per pixel there is nothing to do; the theme is already
 * showable and the reduction would only coarsen it. */
static void theme_fit_display(note_host *h)
{
    note_color     pal[NOTE_PAL_MAX];
    PALETTEENTRY   pe[NOTE_PAL_MAX];
    note_theme_map m;
    HDC  dc;
    int  bpp, npal, i;

    dc = GetDC(h->wnd);
    if (!dc) return;
    bpp = GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES);

    if (bpp > 8) { ReleaseDC(h->wnd, dc); return; }

    /* A palettised device can say what it is showing; a 4bpp VGA cannot, and
     * there the sixteen are fixed and known -- which is why the core ships
     * them rather than each backend writing them down. */
    /* Only the *static* entries, and this is the whole of the problem.
     *
     * A palettised display has 256 slots, but an application that has not
     * realized a logical palette of its own may only count on the twenty the
     * system reserves -- ten at each end of the table.  Ask GDI for a colour
     * outside those and it does not pick the nearest: it dithers, and the
     * fill comes out as a checkerboard of the two entries either side.
     *
     * Handing the reduction all 256 was therefore worse than handing it
     * sixteen.  It found exact matches, faithfully, among colours this
     * process cannot actually paint with -- so every fill it chose came back
     * dithered, while white survived only by being one of the twenty.
     *
     * NUMRESERVED says how many are reserved rather than assuming twenty,
     * and they sit at the two ends of the table, not the front. */
    npal = 0;
    if (GetDeviceCaps(dc, RASTERCAPS) & RC_PALETTE) {
        int total = GetDeviceCaps(dc, SIZEPALETTE);
        int keep  = GetDeviceCaps(dc, NUMRESERVED);
        int half  = keep / 2;

        if (total > NOTE_PAL_MAX) total = NOTE_PAL_MAX;
        if (half > 0 && total >= keep) {
            int got = GetSystemPaletteEntries(dc, 0, total, pe);
            if (got >= total) {
                for (i = 0; i < half; i++) {
                    pal[npal++] = ((note_color)pe[i].peRed   << 16) |
                                  ((note_color)pe[i].peGreen <<  8) |
                                   (note_color)pe[i].peBlue;
                }
                for (i = total - half; i < total; i++) {
                    pal[npal++] = ((note_color)pe[i].peRed   << 16) |
                                  ((note_color)pe[i].peGreen <<  8) |
                                   (note_color)pe[i].peBlue;
                }
            }
        }
    }
    ReleaseDC(h->wnd, dc);

    if (npal <= 0) {
        for (i = 0; i < 16; i++) pal[i] = note_pal_ega16[i];
        npal = 16;
    }

    note_theme_reduce(&m, &h->theme, pal, npal);

    /* Kept, because the theme is not the only source of colour: anything
     * mixed from two theme colours -- the wash under the caret line, a
     * disabled menu label -- lands between palette entries and dithers just
     * as badly as an unreduced theme did.  chrome_snap() puts those on an
     * entry too. */
    for (i = 0; i < npal; i++) g_pal[i] = pal[i];
    g_npal = npal;

    h->theme.bg        = pal[m.bg];
    h->theme.fg        = pal[m.fg];
    h->theme.gutter_bg = pal[m.gutter_bg];
    h->theme.gutter_fg = pal[m.gutter_fg];
    h->theme.sel_bg    = pal[m.sel_bg];
    h->theme.caret     = pal[m.caret];
    h->theme.ui_bg     = pal[m.ui_bg];
    h->theme.ui_fg     = pal[m.ui_fg];
    for (i = 0; i < TOK_COUNT; i++) h->theme.tok[i] = pal[m.tok[i]];

}

void h_set_theme(note_host *h, int theme)
{
    const note_theme *t = note_theme_get(theme);
    int  dark = t->dark;
    BOOL on   = dark ? TRUE : FALSE;
    int  i;
#if NOTE_OWN_VIEW
    int  sb_synced = 0;
#endif

    h->theme = *t;
    theme_fit_display(h);

    if (h->br_gutter)  DeleteObject(h->br_gutter);
    if (h->br_curline) DeleteObject(h->br_curline);
    if (h->br_ui)      DeleteObject(h->br_ui);
    if (h->br_menu)    DeleteObject(h->br_menu);
    if (h->br_menusel) DeleteObject(h->br_menusel);
    if (h->br_sep)     DeleteObject(h->br_sep);
    if (h->br_edit)    DeleteObject(h->br_edit);

    /* From h->theme, never from *t.  theme_fit_display() has just rewritten
     * h->theme onto colours this display can actually paint solid; *t is
     * still the theme as its author wrote it.  A brush built from *t asks
     * GDI for a colour that is not in the palette, and GDI answers with a
     * checkerboard of the two entries either side rather than the nearest
     * one -- which is what the gutter was doing while everything around it
     * came out clean.  The neighbours were only ever right by accident:
     * mix_rgb() snaps its result, and cr() does not. */
    h->br_gutter  = CreateSolidBrush(cr(h->theme.gutter_bg));
    /* A wash a twelfth of the way to the text colour: enough to find the line
     * the caret is on out of the corner of an eye, not enough to read as a
     * highlight over the text sitting on it.  No theme names such a colour,
     * so it is mixed from two that every theme does -- which is what lets all
     * 338 of the shipped palettes have one. */
    h->br_curline = CreateSolidBrush(
                        mix_rgb(h->theme.fg, h->theme.gutter_bg, 1, 12));
    h->br_ui      = CreateSolidBrush(cr(h->theme.ui_bg));
    h->br_menu    = CreateSolidBrush(cr(h->theme.ui_bg));
    h->br_menusel = CreateSolidBrush(cr(h->theme.sel_bg));
    /* A fifth of the way to the text colour: enough to separate two quiet
     * tabs, not enough to read as a rule drawn between them. */
    h->br_sep     = CreateSolidBrush(
                        mix_rgb(h->theme.ui_fg, h->theme.ui_bg, 1, 5));
    h->br_edit    = CreateSolidBrush(cr(h->theme.bg));

    set_app_dark(dark);
    if (caps.dwm_set_attr) {
        /* The strip is drawn over the caption but the corner kept clear for
         * the system buttons is still DWM's to paint, so tell it the colour —
         * otherwise that corner goes pale the moment the window deactivates.
         * Windows 11 only; older builds simply refuse the attribute. */
        COLORREF cap = cr(h->theme.ui_bg);
        caps.dwm_set_attr(h->wnd, 20 /* USE_IMMERSIVE_DARK_MODE */,
                          &on, sizeof(on));
        caps.dwm_set_attr(h->wnd, 35 /* DWMWA_CAPTION_COLOR */,
                          &cap, sizeof(cap));
    }

    /* apply_theme_to repaints every character in the default colour, so
     * nothing anywhere is coloured any more. */
    hl_invalidate(h, -1);
    for (i = 0; i < NOTE_MAX_DOCS; i++)
        if (h->d[i].edit) {
#if NOTE_OWN_VIEW
            /* The view reads the theme as it paints, so the whole of it is
             * repainted and none of it is stored anywhere. */
            InvalidateRect(h->d[i].edit, NULL, FALSE);
            sb_synced = 1;
#else
            apply_theme_to(h->d[i].edit, h);
#endif
        }

    if (caps.set_window_theme)
        caps.set_window_theme(h->tabs, dark ? L"DarkMode_Explorer"
                                            : L"Explorer", NULL);

    if (h->menubar) {
        menu_set_brush(h->menubar, h->br_menu);
        DrawMenuBar(h->wnd);
    }
    if (h->ctxmenu) menu_set_brush(h->ctxmenu, h->br_menu);

#if NOTE_OWN_VIEW
    /* Whose scroll bars these are is a question about the theme's colours, so
     * changing the theme can change the answer -- see sb_own(). */
    if (sb_synced) view_sb_sync(h);
#endif

    /* Every child, and the status bar is the one that has to be named.  The
     * frame's own invalidation does not reach a child window, and the others
     * are repainted for their own reasons -- the view because the text was
     * recoloured, the strip because the caption colour changed.  The status
     * bar is only ever repainted when its *text* changes, and changing the
     * theme does not change it: it kept the white band it was painted with,
     * across the bottom of an otherwise black window, until the caret next
     * moved. */
    InvalidateRect(h->wnd, NULL, TRUE);
    if (h->tabs)   InvalidateRect(h->tabs, NULL, TRUE);
    if (h->gutter) InvalidateRect(h->gutter, NULL, TRUE);
    if (h->status) InvalidateRect(h->status, NULL, TRUE);
}

