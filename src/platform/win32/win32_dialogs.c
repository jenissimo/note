/* win32_dialogs.c -- the platform's own dialogs: open, save, font, find, print
 *
 * Part of note's Win32 backend.  The shared state and the types every part of
 * it needs live in note_win32.h; see there for why the state is a header and
 * not a private static.
 */

#include "note_win32.h"

/* -------------------------------------------------------------------------
 * Dialogs
 * ------------------------------------------------------------------------- */

static const WCHAR kFilter[] =
    L"Text Documents (*.txt)\0*.txt\0All Files (*.*)\0*.*\0\0";

int h_dlg_open(note_host *h, nchar *path, int cap)
{
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    path[0] = 0;
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = h->wnd;
    ofn.lpstrFilter = kFilter;
    ofn.lpstrFile   = (LPWSTR)path;
    ofn.nMaxFile    = cap;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    return os_choose_file(&ofn, 0);
}

int h_dlg_save(note_host *h, nchar *path, int cap)
{
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = h->wnd;
    ofn.lpstrFilter = kFilter;
    ofn.lpstrFile   = (LPWSTR)path;
    ofn.nMaxFile    = cap;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    return os_choose_file(&ofn, 1);
}

void h_dlg_font(note_host *h)
{
    CHOOSEFONTW cf;
    int i;
    memset(&cf, 0, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner   = h->wnd;
    cf.lpLogFont   = &h->font;
    /* Only fixed-pitch faces are offered, because only those work: the view
     * measures everything in columns.  Filtering the list is what makes the
     * refusal invisible -- a choice that cannot be made needs no explaining,
     * and the silent fallback behind it is left as a backstop for a config
     * naming a face this machine does not have. */
    cf.Flags       = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS |
                     CF_FIXEDPITCHONLY;
    if (!os_choose_font(&cf)) return;

    h->fontpt = cf.iPointSize;
    for (i = 0; i < NOTE_MAX_DOCS; i++)
        if (h->d[i].edit) apply_font_to(h->d[i].edit, h);
    queue_view(h);
}

void h_dlg_find(note_host *h, int replace)
{
    if (h->finddlg) { SetFocus(h->finddlg); return; }

    memset(&h->fr, 0, sizeof(h->fr));
    h->fr.lStructSize      = sizeof(h->fr);
    h->fr.hwndOwner        = h->wnd;
    h->fr.lpstrFindWhat    = h->fr_find;
    h->fr.wFindWhatLen     = NOTE_FIND_MAX;
    h->fr.lpstrReplaceWith = h->fr_repl;
    h->fr.wReplaceWithLen  = NOTE_FIND_MAX;
    h->fr.Flags            = FR_DOWN;

    n_copy((nchar *)h->fr_find, h->app.find, NOTE_FIND_MAX);
    n_copy((nchar *)h->fr_repl, h->app.replace, NOTE_FIND_MAX);

    h->finddlg = os_find_dlg(&h->fr, replace);
}

/* Find, go-to and print all reach into the control, so with the view they are
 * win32_view.c's: the buffer has a search of its own, a line index of its own,
 * and no EM_FORMATRANGE to lay itself out on a page with. */
#if !NOTE_OWN_VIEW

int h_find_text(note_host *h, const nchar *needle, unsigned flags)
{
    HWND e = active_edit();
    FINDTEXTEXW ft;
    CHARRANGE   sel;
    DWORD       fl = 0;
    LONG        found;
    int         total = edit_len(e);

    (void)h;
    os_send(e, EM_EXGETSEL, 0, (LPARAM)&sel);

    if (flags & FIND_DOWN) { fl |= FR_DOWN; ft.chrg.cpMin = sel.cpMax; ft.chrg.cpMax = total; }
    else                   {                ft.chrg.cpMin = sel.cpMin; ft.chrg.cpMax = 0;     }
    if (flags & FIND_MATCHCASE) fl |= FR_MATCHCASE;
    if (flags & FIND_WHOLEWORD) fl |= FR_WHOLEWORD;
    ft.lpstrText = (LPCWSTR)needle;

    found = (LONG)os_send(e, EM_FINDTEXTEXW, (WPARAM)fl, (LPARAM)&ft);
    if (found < 0) {
        if (flags & FIND_DOWN) { ft.chrg.cpMin = 0;     ft.chrg.cpMax = total; }
        else                   { ft.chrg.cpMin = total; ft.chrg.cpMax = 0;     }
        found = (LONG)os_send(e, EM_FINDTEXTEXW, (WPARAM)fl, (LPARAM)&ft);
        if (found < 0) return 0;
    }

    os_send(e, EM_EXSETSEL, 0, (LPARAM)&ft.chrgText);
    os_send(e, EM_SCROLLCARET, 0, 0);
    queue_view(&g);
    return 1;
}

/* --- go to line ---------------------------------------------------------- */

void h_goto_line(note_host *h, int line)
{
    HWND e = active_edit();
    LONG idx, count;
    (void)h;
    if (line < 1) line = 1;
    count = (LONG)os_send(e, EM_GETLINECOUNT, 0, 0);
    if (line > count) line = count;
    idx = (LONG)os_send(e, EM_LINEINDEX, (WPARAM)(line - 1), 0);
    if (idx < 0) return;
    h_sel_set(&g, idx, idx);
    os_send(e, EM_SCROLLCARET, 0, 0);
    SetFocus(e);
    queue_view(&g);
}

#endif /* !NOTE_OWN_VIEW */

/* --- page setup / print -------------------------------------------------- */

void h_dlg_pagesetup(note_host *h)
{
    h->page.lStructSize = sizeof(h->page);
    h->page.hwndOwner   = h->wnd;
    h->page.Flags |= PSD_MARGINS | PSD_INHUNDREDTHSOFMILLIMETERS;
    os_page_setup(&h->page);
}

#if !NOTE_OWN_VIEW
void h_dlg_print(note_host *h)
{
    HWND         e = active_edit();
    PRINTDLGW    pd;
    const nchar *docname;
    FORMATRANGE  fr;
    LONG         text_len, next;
    int          logx, logy, physw, physh, offx, offy;

    memset(&pd, 0, sizeof(pd));
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner   = h->wnd;
    pd.Flags       = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION;
    pd.nCopies     = 1;
    if (!os_print_dlg(&pd) || !pd.hDC) return;

    logx  = GetDeviceCaps(pd.hDC, LOGPIXELSX);
    logy  = GetDeviceCaps(pd.hDC, LOGPIXELSY);
    physw = GetDeviceCaps(pd.hDC, PHYSICALWIDTH);
    physh = GetDeviceCaps(pd.hDC, PHYSICALHEIGHT);
    offx  = GetDeviceCaps(pd.hDC, PHYSICALOFFSETX);
    offy  = GetDeviceCaps(pd.hDC, PHYSICALOFFSETY);

    memset(&fr, 0, sizeof(fr));
    fr.hdc = fr.hdcTarget = pd.hDC;

    fr.rcPage.left = fr.rcPage.top = 0;
    fr.rcPage.right  = MulDiv(physw, 1440, logx);
    fr.rcPage.bottom = MulDiv(physh, 1440, logy);

    fr.rc.left = 1440 - MulDiv(offx, 1440, logx);
    fr.rc.top  = 1440 - MulDiv(offy, 1440, logy);
    if (fr.rc.left < 0) fr.rc.left = 0;
    if (fr.rc.top  < 0) fr.rc.top  = 0;
    fr.rc.right  = fr.rcPage.right  - fr.rc.left;
    fr.rc.bottom = fr.rcPage.bottom - fr.rc.top;

    text_len = (LONG)edit_len(e);
    fr.chrg.cpMin = 0;
    fr.chrg.cpMax = text_len;

    docname = h->app.docs[h->app.active].path[0]
              ? note_basename(h->app.docs[h->app.active].path)
              : N("Untitled");

    if (os_start_doc(pd.hDC, docname) > 0) {
        do {
            if (StartPage(pd.hDC) <= 0) break;
            next = (LONG)os_send(e, EM_FORMATRANGE, TRUE, (LPARAM)&fr);
            if (EndPage(pd.hDC) <= 0) break;
            if (next <= fr.chrg.cpMin) break;
            fr.chrg.cpMin = next;
        } while (next < text_len);
        EndDoc(pd.hDC);
    }

    os_send(e, EM_FORMATRANGE, FALSE, 0);
    DeleteDC(pd.hDC);
    if (pd.hDevMode)  GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
}

#endif /* !NOTE_OWN_VIEW */

/* --- misc services ------------------------------------------------------- */

int h_ask_save(note_host *h, const nchar *name)
{
    WCHAR msg[NOTE_PATH_MAX + 64];
    int r;
    msg[0] = 0;
    n_cat((nchar *)msg, N("Do you want to save changes to "), NOTE_PATH_MAX + 64);
    n_cat((nchar *)msg, name, NOTE_PATH_MAX + 64);
    n_cat((nchar *)msg, N("?"), NOTE_PATH_MAX + 64);
    r = os_message_box(h->wnd, (const nchar *)msg, N("note"),
                       MB_YESNOCANCEL | MB_ICONWARNING);
    if (r == IDYES) return ASK_YES;
    if (r == IDNO)  return ASK_NO;
    return ASK_CANCEL;
}

void h_message(note_host *h, const nchar *text, const nchar *title)
{
    os_message_box(h->wnd, text, title, MB_OK | MB_ICONINFORMATION);
}

