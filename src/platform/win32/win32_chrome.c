/* win32_chrome.c -- tabs, the status bar, fonts and applying a theme
 *
 * Part of note's Win32 backend.  The shared state and the types every part of
 * it needs live in note_win32.h; see there for why the state is a header and
 * not a private static.
 */

#include "note_win32.h"

/* -------------------------------------------------------------------------
 * Tabs
 * ------------------------------------------------------------------------- */

void apply_font_to(HWND e, note_host *h);
static void apply_theme_to(HWND e, note_host *h);

int h_tab_create(note_host *h, int doc)
{
    HWND e;

    if (doc < 0 || doc >= NOTE_MAX_DOCS) return 0;

    e = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                        WS_CHILD | WS_VSCROLL | ES_MULTILINE |
                        ES_WANTRETURN | ES_NOHIDESEL | ES_AUTOVSCROLL,
                        0, 0, 0, 0, h->wnd, (HMENU)(UINT_PTR)(ID_EDIT0 + doc),
                        h->inst, NULL);
    if (!e) return 0;

    h->d[doc].edit    = e;
    h->d[doc].oldproc = (WNDPROC)SetWindowLongPtrW(e, GWLP_WNDPROC, (LONG_PTR)EditProc);

    SendMessageW(e, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE | ENM_SCROLLEVENTS);
    SendMessageW(e, EM_EXLIMITTEXT, 0, 0x7FFFFFF0);
    SendMessageW(e, EM_SETTARGETDEVICE, 0, h->app.wrap ? 0 : 1);
    SendMessageW(e, EM_SETZOOM, (WPARAM)h->app.zoom, (LPARAM)100);
    apply_font_to(e, h);
    apply_theme_to(e, h);

    h->d[doc].title[0] = 0;
    tabs_layout(h);
    if (h->tabs) InvalidateRect(h->tabs, NULL, FALSE);
    return 1;
}

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
    SetWindowTextW(h->wnd, (LPCWSTR)s);
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

void h_set_wrap(note_host *h, int wrap)
{
    int i;
    h->suppress++;
    for (i = 0; i < NOTE_MAX_DOCS; i++)
        if (h->d[i].edit)
            SendMessageW(h->d[i].edit, EM_SETTARGETDEVICE, 0, wrap ? 0 : 1);
    h->suppress--;
    queue_view(h);
}

void h_set_linenums(note_host *h, int on)
{
    ShowWindow(h->gutter, on ? SW_SHOW : SW_HIDE);
    queue_view(h);
}

void h_set_zoom(note_host *h, int percent)
{
    int i;
    h->suppress++;
    for (i = 0; i < NOTE_MAX_DOCS; i++)
        if (h->d[i].edit)
            SendMessageW(h->d[i].edit, EM_SETZOOM, (WPARAM)percent, (LPARAM)100);
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
    SendMessageW(e, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    h->suppress--;
}

static void apply_theme_to(HWND e, note_host *h)
{
    CHARFORMAT2W cf;

    SendMessageW(e, EM_SETBKGNDCOLOR, 0, (LPARAM)cr(h->theme.bg));

    memset(&cf, 0, sizeof(cf));
    cf.cbSize      = sizeof(cf);
    cf.dwMask      = CFM_COLOR;
    cf.crTextColor = cr(h->theme.fg);
    h->suppress++;
    SendMessageW(e, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    h->suppress--;

    SetWindowTheme(e, h->theme.dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
}

/* Undocumented but long-standing: lets menus, scrollbars and the non-client
 * area follow a dark palette.  Entirely optional — if it is not there, note
 * still works, it just keeps light menus. */
void set_app_dark(int dark)
{
    typedef int (WINAPI *PFN_SETMODE)(int);
    typedef void (WINAPI *PFN_FLUSH)(void);
    static PFN_SETMODE set_mode;
    static PFN_FLUSH   flush;
    static int         probed;

    if (!probed) {
        HMODULE ux = LoadLibraryW(L"uxtheme.dll");
        probed = 1;
        if (ux) {
            set_mode = (PFN_SETMODE)GetProcAddress(ux, MAKEINTRESOURCEA(135));
            flush    = (PFN_FLUSH)  GetProcAddress(ux, MAKEINTRESOURCEA(136));
        }
    }
    if (set_mode) set_mode(dark ? 2 : 3);     /* ForceDark : ForceLight */
    if (flush)    flush();
}

void h_set_theme(note_host *h, int theme)
{
    const note_theme *t = note_theme_get(theme);
    int  dark = t->dark;
    BOOL on   = dark ? TRUE : FALSE;
    int  i;

    h->theme = *t;

    if (h->br_gutter)  DeleteObject(h->br_gutter);
    if (h->br_ui)      DeleteObject(h->br_ui);
    if (h->br_menu)    DeleteObject(h->br_menu);
    if (h->br_menusel) DeleteObject(h->br_menusel);
    if (h->br_sep)     DeleteObject(h->br_sep);
    if (h->br_edit)    DeleteObject(h->br_edit);

    h->br_gutter  = CreateSolidBrush(cr(t->gutter_bg));
    h->br_ui      = CreateSolidBrush(cr(t->ui_bg));
    h->br_menu    = CreateSolidBrush(cr(t->ui_bg));
    h->br_menusel = CreateSolidBrush(cr(t->sel_bg));
    /* A fifth of the way to the text colour: enough to separate two quiet
     * tabs, not enough to read as a rule drawn between them. */
    h->br_sep     = CreateSolidBrush(mix_rgb(t->ui_fg, t->ui_bg, 1, 5));
    h->br_edit    = CreateSolidBrush(cr(t->bg));

    set_app_dark(dark);
    DwmSetWindowAttribute(h->wnd, 20 /* USE_IMMERSIVE_DARK_MODE */,
                          &on, sizeof(on));

    {   /* The strip is drawn over the caption but the corner kept clear for
         * the system buttons is still DWM's to paint, so tell it the colour —
         * otherwise that corner goes pale the moment the window deactivates.
         * Windows 11 only; older builds simply refuse it. */
        COLORREF cap = cr(t->ui_bg);
        DwmSetWindowAttribute(h->wnd, 35 /* DWMWA_CAPTION_COLOR */,
                              &cap, sizeof(cap));
    }

    for (i = 0; i < NOTE_MAX_DOCS; i++)
        if (h->d[i].edit) apply_theme_to(h->d[i].edit, h);

    SetWindowTheme(h->tabs, dark ? L"DarkMode_Explorer" : L"Explorer", NULL);

    if (h->menubar) {
        menu_set_brush(h->menubar, h->br_menu);
        DrawMenuBar(h->wnd);
    }
    if (h->ctxmenu) menu_set_brush(h->ctxmenu, h->br_menu);

    InvalidateRect(h->wnd, NULL, TRUE);
    if (h->tabs)   InvalidateRect(h->tabs, NULL, TRUE);
    if (h->gutter) InvalidateRect(h->gutter, NULL, TRUE);
}

