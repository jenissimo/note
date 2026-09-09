/* win32_main.c -- the window, the message loop and startup
 *
 * Part of note's Win32 backend.  The shared state and the types every part of
 * it needs live in note_win32.h; see there for why the state is a header and
 * not a private static.
 */

#include "note_win32.h"

/* -------------------------------------------------------------------------
 * CRT stubs.  The compiler still emits memset/memcpy for aggregate init.
 * ------------------------------------------------------------------------- */
#pragma function(memset)
void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

#pragma function(memcpy)
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}


menu_item_data g_mid[192];
int            g_nmid;

struct note_host g;

COLORREF cr(unsigned rgb)
{
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

/* Every hand-drawn measurement below is written for 96 DPI and scaled here.
 * Owner-drawn menus get no scaling from Windows, so a fixed pixel padding
 * looks cramped the moment the display is not at 100%. */
int px(int v)
{
    return MulDiv(v, g.dpi > 0 ? g.dpi : 96, 96);
}

void read_dpi(note_host *h)
{
    typedef UINT (WINAPI *PFN_GETDPI)(HWND);
    static PFN_GETDPI get_dpi;
    static int probed;

    if (!probed) {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        probed = 1;
        if (u) get_dpi = (PFN_GETDPI)GetProcAddress(u, "GetDpiForWindow");
    }

    h->dpi = 0;
    if (get_dpi && h->wnd) h->dpi = (int)get_dpi(h->wnd);
    if (h->dpi <= 0) {
        HDC dc = GetDC(h->wnd);
        h->dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(h->wnd, dc);
    }
    if (h->dpi <= 0) h->dpi = 96;
}

HWND active_edit(void)
{
    return g.d[g.app.active].edit;
}

/* -------------------------------------------------------------------------
 * Undo suspension.
 *
 * Colouring text through EM_SETCHARFORMAT would otherwise pile formatting
 * records onto the undo stack, so Ctrl+Z would undo the highlighter instead
 * of the user's typing.  RichEdit's Text Object Model can suspend undo
 * around it; if the interface is unavailable we simply colour without it.
 * ------------------------------------------------------------------------- */

static const GUID kIID_ITextDocument =
    { 0x8CC497C0, 0xA1DF, 0x11CE, { 0x80, 0x98, 0x00, 0xAA, 0x00, 0x47, 0xBE, 0x5D } };

ITextDocument *tom_open(HWND edit)
{
    IRichEditOle  *ole = 0;
    ITextDocument *doc = 0;

    SendMessageW(edit, EM_GETOLEINTERFACE, 0, (LPARAM)&ole);
    if (!ole) return 0;

    if (FAILED(ole->lpVtbl->QueryInterface(ole, &kIID_ITextDocument, (void **)&doc)))
        doc = 0;
    ole->lpVtbl->Release(ole);
    return doc;
}

/* -------------------------------------------------------------------------
/* -------------------------------------------------------------------------
/* -------------------------------------------------------------------------
/* -------------------------------------------------------------------------
/* -------------------------------------------------------------------------
/* -------------------------------------------------------------------------
 * Keyboard gestures the message loop owns
 *
 * Both of these are about keys that mean nothing on their own and something
 * only in a pattern, which no single window procedure sees reliably: focus
 * may be in the editor, the gutter or the tab strip, and Alt reaches the
 * frame only after the focused control's default handling has had it.
 * ------------------------------------------------------------------------- */

#define SHIFT_DOUBLE_MS 400

/* Two taps of Shift with nothing in between, IntelliJ style.  A Shift that
 * held long enough to auto-repeat, or that had another key pressed under it,
 * was a modifier and is not a tap. */
static int shift_gesture(note_host *h, const MSG *m)
{
    if (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN) {
        if (m->wParam == VK_SHIFT) {
            if (m->lParam & 0x40000000) { h->shift_chord = 1; h->shift_last = 0; }
            else                          h->shift_chord = 0;
        } else {
            h->shift_chord = 1;
            h->shift_last  = 0;
        }
        return 0;
    }

    if ((m->message == WM_KEYUP || m->message == WM_SYSKEYUP) &&
        m->wParam == VK_SHIFT) {
        DWORD now;
        if (h->shift_chord) { h->shift_chord = 0; h->shift_last = 0; return 0; }
        now = GetTickCount();
        if (h->shift_last && now - h->shift_last <= SHIFT_DOUBLE_MS) {
            h->shift_last = 0;
            return 1;
        }
        h->shift_last = now;
    }
    return 0;
}

/* Alt on its own toggles the menu bar; Alt with a letter reveals it and opens
 * that menu.  Both arrive here before the focused control can turn them into
 * Windows' own menu navigation.  Returns 1 when the message is spent. */
static int alt_gesture(note_host *h, const MSG *m)
{
    if (m->message == WM_KEYDOWN && m->wParam == VK_ESCAPE && h->menu_visible) {
        menubar_show(h, 0);
        return 1;
    }

    if (m->message == WM_SYSKEYDOWN) {
        if (m->wParam == VK_MENU) {
            /* Swallowed: what a bare Alt means is decided on the way up, and
             * letting Windows start menu navigation here would pre-empt it. */
            if (!(m->lParam & 0x40000000)) h->alt_chord = 0;
            return 1;
        }
        h->alt_chord = 1;
        if (!h->menu_visible && m->wParam >= 'A' && m->wParam <= 'Z') {
            /* Reveal first, then hand Windows the mnemonic: SC_KEYMENU only
             * finds a menu that is actually attached to the frame. */
            menubar_show(h, 1);
            PostMessageW(h->wnd, WM_SYSCOMMAND, SC_KEYMENU,
                         (LPARAM)(m->wParam + ('a' - 'A')));
            return 1;
        }
        return 0;
    }

    if (m->message == WM_SYSKEYUP && m->wParam == VK_MENU) {
        if (!h->alt_chord) {
            if (h->menu_visible) {
                menubar_show(h, 0);
            } else {
                /* Reveal the bar and hand the window into Windows' own menu
                 * mode, rather than reimplementing arrow navigation: File is
                 * highlighted, Left/Right walk the bar, Down or Enter opens,
                 * Esc leaves — and leaving is what hides the bar again, in
                 * WM_EXITMENULOOP. */
                menubar_show(h, 1);
                PostMessageW(h->wnd, WM_SYSCOMMAND, SC_KEYMENU, 0);
            }
        }
        h->alt_chord = 0;
        return 1;
    }
    return 0;
}

void measure_font(note_host *h)
{
    HDC dc = GetDC(h->wnd);
    HFONT f, old;
    TEXTMETRICW tm;

    f = CreateFontIndirectW(&h->font);
    old = (HFONT)SelectObject(dc, f);
    GetTextMetricsW(dc, &tm);
    h->line_h = tm.tmHeight;
    h->char_w = tm.tmAveCharWidth;
    SelectObject(dc, old);
    DeleteObject(f);
    ReleaseDC(h->wnd, dc);

    if (h->uifont) DeleteObject(h->uifont);
    h->uifont = CreateFontIndirectW(&h->font);
}

void relayout(note_host *h)
{
    RECT rc;
    int top, bottom, i;

    GetClientRect(h->wnd, &rc);
    /* Before anything is measured against it: maximising moves the buttons
     * relative to the client area. */
    caption_buttons_calc(h);

    top    = h->tabs_h;
    bottom = IsWindowVisible(h->status) ? h->status_h : 0;

    /* The summoned menu bar sits between the strip and the editor.  It is not
     * subtracted from the client rect — with the caption taken into the client
     * it cannot be, or the strip would go under it — so the editor is what has
     * to start below it. */
    if (h->menu_visible) {
        RECT band;
        int  mb = menubar_band(h, &band);
        if (mb > top) top = mb;
    }

    {   /* In the caption, the strip stops short of the system buttons. */
        int w = rc.right;
        if (h->title_tabs) w -= caption_buttons_w(h);
        if (w < 0) w = 0;
        MoveWindow(h->tabs, 0, 0, w, h->tabs_h, TRUE);
    }
    tabs_layout(h);

    if (h->app.linenums && h->gutter_w > 0) {
        MoveWindow(h->gutter, 0, top, h->gutter_w,
                   rc.bottom - top - bottom, TRUE);
    }

    for (i = 0; i < NOTE_MAX_DOCS; i++)
        if (h->d[i].edit)
            MoveWindow(h->d[i].edit,
                       h->app.linenums ? h->gutter_w : 0, top,
                       rc.right - (h->app.linenums ? h->gutter_w : 0),
                       rc.bottom - top - bottom, TRUE);

    if (bottom)
        MoveWindow(h->status, 0, rc.bottom - bottom, rc.right, bottom, TRUE);
}

void update_status(note_host *h)
{
    HWND e = active_edit();
    CHARRANGE c;
    LONG row, start;

    if (!h->app.status || !e) return;

    SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&c);
    row   = (LONG)SendMessageW(e, EM_EXLINEFROMCHAR, 0, (LPARAM)c.cpMin);
    start = (LONG)SendMessageW(e, EM_LINEINDEX, (WPARAM)row, 0);

    /* Report the paragraph the caret sits in, so the status bar agrees with
     * the gutter even when a long line wraps across several rows. */
    refresh_cache(h);
    note_status_at(&h->app, para_at(h, (int)c.cpMin),
                   (int)(c.cpMin - start) + 1);
}

/* -------------------------------------------------------------------------
 * Find/replace message routing
 * ------------------------------------------------------------------------- */

void on_find_msg(note_host *h, FINDREPLACEW *fr)
{
    HWND e = active_edit();

    if (fr->Flags & FR_DIALOGTERM) { h->finddlg = NULL; return; }

    n_copy(h->app.find,    (const nchar *)fr->lpstrFindWhat,    NOTE_FIND_MAX);
    n_copy(h->app.replace, (const nchar *)fr->lpstrReplaceWith, NOTE_FIND_MAX);

    h->app.find_flags = 0;
    if (fr->Flags & FR_DOWN)      h->app.find_flags |= FIND_DOWN;
    if (fr->Flags & FR_MATCHCASE) h->app.find_flags |= FIND_MATCHCASE;
    if (fr->Flags & FR_WHOLEWORD) h->app.find_flags |= FIND_WHOLEWORD;

    if (fr->Flags & FR_FINDNEXT) {
        if (!h_find_text(h, h->app.find, h->app.find_flags))
            h_message(h, N("Cannot find that text."), N("note"));

    } else if (fr->Flags & FR_REPLACE) {
        CHARRANGE sel;
        int len = n_len(h->app.find);
        SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&sel);
        if (sel.cpMax - sel.cpMin == len) {
            WCHAR *cur = (WCHAR *)h_alloc(h, (DWORD)(len + 2) * sizeof(WCHAR));
            if (cur) {
                TEXTRANGEW tr;
                tr.chrg = sel;
                tr.lpstrText = cur;
                SendMessageW(e, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
                if (n_eq((const nchar *)cur, h->app.find))
                    h_sel_replace(h, h->app.replace);
                h_free(h, cur);
            }
        }
        if (!h_find_text(h, h->app.find, h->app.find_flags))
            h_message(h, N("Cannot find that text."), N("note"));

    } else if (fr->Flags & FR_REPLACEALL) {
        int n = 0, guard = 0;
        LONG last = -1;
        h_sel_set(h, 0, 0);
        while (h_find_text(h, h->app.find, h->app.find_flags | FIND_DOWN)) {
            CHARRANGE sel;
            SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&sel);
            if (sel.cpMin <= last) break;        /* wrapped around */
            last = sel.cpMin;
            h_sel_replace(h, h->app.replace);
            n++;
            if (++guard > 1000000) break;
        }
        if (!n) h_message(h, N("Cannot find that text."), N("note"));
    }
}

/* -------------------------------------------------------------------------
 * Window procedure
 * ------------------------------------------------------------------------- */

static LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    note_host *h = &g;

    if (h->findmsg && msg == h->findmsg) {
        on_find_msg(h, (FINDREPLACEW *)lp);
        return 0;
    }

    /* The buttons DWM draws in the caption answer here first, so hovering and
     * clicking minimise/maximise/close still behave like a real title bar. */
    if (h->title_tabs) {
        LRESULT dwm = 0;
        if (DwmDefWindowProc(wnd, msg, wp, lp, &dwm)) return dwm;
    }

    switch (msg) {

    /* Take the caption band back into the client rect, leaving the resize
     * borders where they were.  Maximised, the window rect hangs a border's
     * width off every edge of the monitor, so the top has to come back in by
     * that much or the tabs would be drawn off the screen. */
    case WM_NCCALCSIZE:
        if (h->title_tabs && wp) {
            NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
            LONG    top = p->rgrc[0].top;
            LRESULT lr  = DefWindowProcW(wnd, msg, wp, lp);
            p->rgrc[0].top = top + (IsZoomed(wnd) ? frame_edge() : 0);
            return lr;
        }
        break;

    /* What the pointer is over, in a caption we draw ourselves: the top edge
     * is still the resize edge, a tab is ours to handle, and everything else
     * is the caption — which is what makes dragging move the window and a
     * double click maximise it. */
    case WM_NCHITTEST: {
        POINT pt;
        RECT  wr;
        int   edge;

        if (!h->title_tabs) break;

        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        GetWindowRect(wnd, &wr);
        edge = frame_edge();

        if (!IsZoomed(wnd) && pt.y < wr.top + edge && pt.y >= wr.top) {
            if (pt.x < wr.left  + edge * 2) return HTTOPLEFT;
            if (pt.x > wr.right - edge * 2) return HTTOPRIGHT;
            return HTTOP;
        }

        ScreenToClient(wnd, &pt);

        /* The system buttons.  DWM draws them and DwmDefWindowProc lights them
         * up, but only this answer makes them clickable — and HTMAXBUTTON is
         * also what the Snap Layouts flyout waits for on hover. */
        if (pt.y >= 0 && pt.y < h->sysbtn.bottom &&
            pt.x >= h->sysbtn.left && pt.x < h->sysbtn.right) {
            int w   = h->sysbtn.right - h->sysbtn.left;
            int off = pt.x - h->sysbtn.left;
            if (off * 3 < w)     return HTMINBUTTON;
            if (off * 3 < w * 2) return HTMAXBUTTON;
            return HTCLOSE;
        }

        if (pt.x >= 0 && pt.y >= 0 && pt.y < h->tabs_h) {
            /* The icon corner is the system menu.  Saying so is enough:
             * Windows opens the menu on a click and closes the window on a
             * double click, both without a line of code here. */
            if (caption_plain(h) && pt.x < tabs_left(h)) return HTSYSMENU;
            /* HTCLIENT is what lets the hit test descend into the strip;
             * anything else and the tabs would never see a click. */
            if (tabs_hit(h, pt.x, pt.y, NULL) >= 0) return HTCLIENT;
            return HTCAPTION;
        }
        break;
    }

    /* The wheel over the caption band belongs to the strip, which the hit test
     * has just declared is not there. */
    case WM_MOUSEWHEEL: {
        POINT pt;
        if (!h->title_tabs) break;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        ScreenToClient(wnd, &pt);
        if (pt.y >= 0 && pt.y < h->tabs_h)
            return SendMessageW(h->tabs, WM_MOUSEWHEEL, wp, lp);
        break;
    }

    case WM_SIZE:
        relayout(h);
        if (h->pal_open) pal_layout(h);
        queue_view(h);
        return 0;

    case WM_MOVE:
        if (h->pal_open) pal_layout(h);
        return 0;

    /* The overlay belongs to this window's focus, not its own. */
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) pal_close(h);
        break;

    case WM_SETFOCUS:
        SetFocus(active_edit());
        return 0;

    case WM_INITMENUPOPUP:
        /* The window menu is Windows' own: it is the one that knows whether
         * Restore or Maximise is the greyed one just now. */
        if (HIWORD(lp)) break;
        sync_menu(h);
        return 0;

    /* The system menu on a right click in the caption, and on the icon in
     * single-document mode.  In an ordinary window the default handling puts
     * it up; here the caption is inside the client rect and that path never
     * runs, so note asks for the menu by name. */
    case WM_NCRBUTTONUP:
        if (h->title_tabs && (wp == HTCAPTION || wp == HTSYSMENU)) {
            HMENU sys = GetSystemMenu(wnd, FALSE);
            if (sys) {
                int cmd = (int)TrackPopupMenu(sys,
                              TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
                              (short)LOWORD(lp), (short)HIWORD(lp), 0, wnd, NULL);
                if (cmd) PostMessageW(wnd, WM_SYSCOMMAND, (WPARAM)cmd, 0);
            }
            return 0;
        }
        break;

    /* Every item is owner-drawn, so where its text would be there is a
     * pointer instead: Windows has nothing to match a mnemonic against and
     * asks us.  Without this, Alt+F would leave menu mode instead of opening
     * the File menu. */
    case WM_MENUCHAR: {
        HMENU m = (HMENU)lp;
        int   n = GetMenuItemCount(m), i;
        nchar want = (nchar)LOWORD(wp);

        if (want >= (nchar)'A' && want <= (nchar)'Z') want = (nchar)(want + 32);

        for (i = 0; i < n; i++) {
            MENUITEMINFOW mii;
            const menu_item_data *d;
            const nchar *s;

            memset(&mii, 0, sizeof(mii));
            mii.cbSize = sizeof(mii);
            mii.fMask  = MIIM_DATA;
            if (!GetMenuItemInfoW(m, (UINT)i, TRUE, &mii)) continue;

            d = (const menu_item_data *)mii.dwItemData;
            if (!d || !d->label) continue;

            for (s = d->label; *s; s++) {
                nchar c;
                if (*s != (nchar)'&' || !s[1]) continue;
                c = s[1];
                if (c >= (nchar)'A' && c <= (nchar)'Z') c = (nchar)(c + 32);
                if (c == want) return MAKELRESULT(i, MNC_EXECUTE);
                break;                  /* only the first & is the mnemonic */
            }
        }
        return MAKELRESULT(0, MNC_IGNORE);
    }

    /* Menu mode is over — by Esc, by a click away, or by picking something.
     * Either way the bar has done its job and goes back out of the way.
     * wParam is TRUE for a TrackPopupMenu, which is the context menu. */
    case WM_EXITMENULOOP:
        if (!wp) menubar_show(h, 0);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_VIEW)    { service_view(h); return 0; }
        if (wp == TIMER_SESSION) { note_session_save(&h->app); return 0; }
        break;

    case WM_DPICHANGED: {
        RECT *sug = (RECT *)lp;
        read_dpi(h);
        h->status_h = px(STATUS_H);
        if (h->title_tabs) frame_custom(h);      /* the band changes height */
        h->tabs_h = h->title_tabs ? caption_height() : px(TABS_H);
        if (sug)
            SetWindowPos(wnd, NULL, sug->left, sug->top,
                         sug->right - sug->left, sug->bottom - sug->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        DrawMenuBar(wnd);
        relayout(h);
        queue_view(h);
        return 0;
    }

    case WM_SETTINGCHANGE:
        /* The user flipped the system palette while we were running. */
        if (h->app.theme == THEME_SYSTEM) note_apply_theme(&h->app);
        return 0;

    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == h->status) {
            SetTextColor((HDC)wp, cr(h->theme.ui_fg));
            SetBkColor((HDC)wp, cr(h->theme.ui_bg));
            return (LRESULT)h->br_ui;
        }
        break;

    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(wnd, &rc);
        FillRect((HDC)wp, &rc, h->br_ui);

        /* The corner kept clear for the system buttons is DWM's to paint, and
         * inside the extended frame a pixel is glass only when it is
         * transparent *black*.  GDI leaves the alpha byte at zero but br_ui
         * leaves its colour behind it, and DWM adds that colour to the caption
         * it composites underneath — which is the seam beside the buttons,
         * lighter than the strip by exactly one ui_bg. */
        if (h->title_tabs && h->sysbtn.right > h->sysbtn.left) {
            RECT b = h->sysbtn;
            b.bottom = h->tabs_h;
            FillRect((HDC)wp, &b, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        return 1;
    }

    /* The frame's own client is nothing but the band the menu bar sits in when
     * it is up; everything else is covered by a child window. */
    case WM_PAINT:
        if (h->menu_visible) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(wnd, &ps);
            menubar_paint(h, dc);
            EndPaint(wnd, &ps);
            return 0;
        }
        break;

    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lp;
        if (mis && mis->CtlType == ODT_MENU) {
            measure_menu_item(h, mis);
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lp;
        if (!dis) break;
        if (dis->CtlType == ODT_MENU) { draw_menu_item(h, dis); return TRUE; }
        break;
    }

    /* WM_UAHDRAWMENU.  Undocumented, but it is the only hook for the strip of
     * menu bar that lies outside any item; without it that strip stays in the
     * system's light colour no matter what the items do.  If a future Windows
     * stops sending it we simply fall through and lose the fill. */
    case 0x0091: {
        UAHMENU *um = (UAHMENU *)lp;
        MENUBARINFO mbi;
        RECT wr, rc;

        memset(&mbi, 0, sizeof(mbi));
        mbi.cbSize = sizeof(mbi);
        if (!um || !GetMenuBarInfo(wnd, OBJID_MENU, 0, &mbi)) break;
        if (!GetWindowRect(wnd, &wr)) break;

        rc = mbi.rcBar;
        OffsetRect(&rc, -wr.left, -wr.top);
        rc.bottom += 1;                 /* cover the seam under the bar */
        FillRect(um->hdc, &rc, h->br_ui);
        return 1;
    }

    /* WM_UAHDRAWMENUITEM — one top-level item of the menu bar. */
    case 0x0092: {
        UAHDRAWMENUITEM *udmi = (UAHDRAWMENUITEM *)lp;
        MENUITEMINFOW mii;
        const menu_item_data *d;
        int   hot;

        if (!udmi) break;

        memset(&mii, 0, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask  = MIIM_DATA;
        if (!GetMenuItemInfoW(udmi->um.hmenu, (UINT)udmi->umi.iPosition, TRUE, &mii))
            break;

        d = (const menu_item_data *)mii.dwItemData;
        if (!d) break;

        hot = (udmi->dis.itemState & (ODS_HOTLIGHT | ODS_SELECTED)) != 0;
        /* Remembered, because a repaint of the band has to light the same item
         * Windows last lit — nothing else will tell us which that is. */
        if (hot) h->menu_hot = udmi->umi.iPosition;
        else if (h->menu_hot == udmi->umi.iPosition) h->menu_hot = -1;

        menubar_item_draw(h, udmi->um.hdc, &udmi->dis.rcItem, d, hot);
        return 1;
    }

    case WM_COMMAND:
        if (lp) {
            /* EN_CHANGE can trail our own edits, so the control's modify flag
             * — which we restore after every programmatic change — is the
             * authority on whether a person actually typed something. */
            if (HIWORD(wp) == EN_CHANGE && !h->suppress &&
                SendMessageW((HWND)lp, EM_GETMODIFY, 0, 0)) {
                int doc;
                for (doc = 0; doc < NOTE_MAX_DOCS; doc++)
                    if (h->d[doc].edit == (HWND)lp) {
                        note_set_dirty(&h->app, doc, 1);
                        if (doc == h->app.active) {
                            h->cache_valid = 0;
                            queue_view(h);
                            update_status(h);
                        }
                        break;
                    }
            }
            return 0;
        }
        /* The palette is the backend's own window, so the core hands its
         * command straight back rather than pretending to run it. */
        if (LOWORD(wp) == CMD_VIEW_PALETTE) { pal_show(h); return 0; }

        if (note_command(&h->app, (int)LOWORD(wp))) {
            /* Whatever the bar was summoned for has happened. */
            menubar_show(h, 0);
            sync_menu(h);
            return 0;
        }
        break;

    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (!nm) break;
        if (nm->code == EN_SELCHANGE) {
            update_status(h);
            return 0;
        }
        break;
    }

    case WM_DROPFILES: {
        WCHAR path[NOTE_PATH_MAX];
        HDROP drop = (HDROP)wp;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0), i;
        for (i = 0; i < n; i++)
            if (DragQueryFileW(drop, i, path, NOTE_PATH_MAX))
                note_open(&h->app, (const nchar *)path);
        DragFinish(drop);
        SetForegroundWindow(wnd);
        return 0;
    }

    case WM_CLOSE:
        /* Nothing is lost: every unsaved buffer is in the session. */
        note_session_save(&h->app);
        h->quitting = 1;
        DestroyWindow(wnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(wnd, msg, wp, lp);
}

/* -------------------------------------------------------------------------
 * Startup
 * ------------------------------------------------------------------------- */

static void first_arg(WCHAR *out, int cap)
{
    const WCHAR *p = GetCommandLineW();
    int i = 0;

    out[0] = 0;
    if (!p) return;

    if (*p == L'"') { p++; while (*p && *p != L'"') p++; if (*p) p++; }
    else            { while (*p && *p != L' ' && *p != L'\t') p++; }
    while (*p == L' ' || *p == L'\t') p++;
    if (!*p) return;

    if (*p == L'"') {
        p++;
        while (*p && *p != L'"' && i < cap - 1) out[i++] = *p++;
    } else {
        while (*p && i < cap - 1) out[i++] = *p++;
        while (i > 0 && (out[i - 1] == L' ' || out[i - 1] == L'\t')) i--;
    }
    out[i] = 0;
}

static void enable_dpi(void)
{
    typedef BOOL (WINAPI *PFN)(HANDLE);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    PFN p = u ? (PFN)GetProcAddress(u, "SetProcessDpiAwarenessContext") : 0;
    if (p) p((HANDLE)-4);          /* PER_MONITOR_AWARE_V2 */
}

static int note_main(void)
{
    WNDCLASSEXW wc;
    MSG   msg;
    WCHAR arg[NOTE_PATH_MAX];
    int   i, restored;

    enable_dpi();

    g.inst = GetModuleHandleW(NULL);
    g.heap = GetProcessHeap();
    g.tab_hot  = -1;
    g.menu_hot = -1;

    /* Ask for the dark palette before the frame exists: the non-client area
     * reads it once at creation.  note_apply_theme() corrects it afterwards
     * if the user prefers a fixed light or dark theme. */
    set_app_dark(h_system_dark(&g));

    if (!LoadLibraryW(L"Msftedit.dll")) {
        MessageBoxW(NULL, L"Msftedit.dll is missing.", L"note", MB_OK | MB_ICONERROR);
        return 1;
    }

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = g.inst;
    /* The application icon, compiled in as resource 1.  A build without the
     * resource still runs; it just gets the shell's default. */
    wc.hIcon         = LoadIconW(g.inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm       = wc.hIcon;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;          /* WM_ERASEBKGND paints from the theme */
    wc.lpszClassName = L"noteWindow";
    if (!RegisterClassExW(&wc)) return 1;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = GutterProc;
    wc.hInstance     = g.inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"noteGutter";
    RegisterClassExW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = StatusProc;
    wc.hInstance     = g.inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"noteStatus";
    RegisterClassExW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DBLCLKS;      /* the strip answers double clicks */
    wc.lpfnWndProc   = TabsProc;
    wc.hInstance     = g.inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"noteTabs";
    RegisterClassExW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DROPSHADOW;   /* a card over the editor, not a frame */
    wc.lpfnWndProc   = PaletteProc;
    wc.hInstance     = g.inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"notePalette";
    RegisterClassExW(&wc);

    g.wnd = CreateWindowExW(WS_EX_ACCEPTFILES, L"noteWindow", L"note",
                            WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 900, 660,
                            NULL, NULL, g.inst, NULL);
    if (!g.wnd) return 1;

    g.tabs = CreateWindowExW(0, L"noteTabs", L"", WS_CHILD | WS_VISIBLE,
                             0, 0, 0, 0, g.wnd, (HMENU)ID_TABS, g.inst, NULL);
    g.status = CreateWindowExW(0, L"noteStatus", L"", WS_CHILD | WS_VISIBLE,
                               0, 0, 0, 0, g.wnd, (HMENU)ID_STATUS, g.inst, NULL);
    g.gutter = CreateWindowExW(0, L"noteGutter", L"", WS_CHILD | WS_VISIBLE,
                               0, 0, 0, 0, g.wnd, NULL, g.inst, NULL);

    /* A monospaced default, like the editor this replaces. */
    for (i = 0; L"Consolas"[i]; i++) g.font.lfFaceName[i] = L"Consolas"[i];
    g.font.lfFaceName[i] = 0;
    g.font.lfHeight  = -15;
    g.font.lfWeight  = FW_NORMAL;
    g.font.lfCharSet = DEFAULT_CHARSET;
    g.fontpt = 110;
    measure_font(&g);

    read_dpi(&g);
    frame_custom(&g);
    g.tabs_h   = g.title_tabs ? caption_height() : px(TABS_H);
    g.status_h = px(STATUS_H);

    {   /* The shell's own menu font, so owner-drawn items still look native. */
        NONCLIENTMETRICSW ncm;
        memset(&ncm, 0, sizeof(ncm));
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            g.menufont = CreateFontIndirectW(&ncm.lfMenuFont);
        if (!g.menufont) g.menufont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }

    note_init(&g.app, &g, &kOps);
    build_menus(&g);
    build_accels(&g);

    note_defs_load(&g.app);

    note_apply_theme(&g.app);

    SendMessageW(g.tabs,   WM_SETFONT, (WPARAM)g.menufont, TRUE);
    SendMessageW(g.status, WM_SETFONT, (WPARAM)g.menufont, TRUE);

    g.findmsg = RegisterWindowMessageW(FINDMSGSTRINGW);

    restored = note_session_restore(&g.app);
    first_arg(arg, NOTE_PATH_MAX);

    if (arg[0]) {
        note_open(&g.app, (const nchar *)arg);
    } else if (!restored) {
        int doc = note_new_doc(&g.app);
        if (doc >= 0) note_select_doc(&g.app, doc);
    }
    if (g.app.ndocs == 0) {
        int doc = note_new_doc(&g.app);
        if (doc >= 0) note_select_doc(&g.app, doc);
    }

    note_update_title(&g.app);
    sync_menu(&g);
    gutter_width(&g);

    /* Not SW_SHOWDEFAULT: that defers to the parent's STARTUPINFO, so being
     * launched from a hidden process would leave the editor invisible. */
    ShowWindow(g.wnd, SW_SHOWNORMAL);
    UpdateWindow(g.wnd);
    relayout(&g);
    SetFocus(active_edit());
    update_status(&g);
    queue_view(&g);

    SetTimer(g.wnd, TIMER_SESSION, 4000, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g.finddlg && IsDialogMessageW(g.finddlg, &msg)) continue;

        /* The palette never takes focus, so the keys are still addressed to
         * the editor: intercept them here, before the accelerator table or
         * the control itself can act on them. */
        if (g.pal_open) {
            int spent = 1;
            switch (msg.message) {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                /* Not consumed by the palette: still translate it, so the
                 * typing arrives as the WM_CHAR the query is built from. */
                if (!pal_key(&g, (int)msg.wParam)) TranslateMessage(&msg);
                break;
            case WM_CHAR:
                pal_char(&g, (unsigned)msg.wParam);
                break;
            case WM_KEYUP: case WM_SYSKEYUP: case WM_SYSCHAR:
                break;
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_NCLBUTTONDOWN:
                /* A click anywhere else dismisses it. */
                if (msg.hwnd != g.pal) pal_close(&g);
                spent = 0;
                break;
            default:
                spent = 0;
                break;
            }
            if (spent) continue;
        }

        if (alt_gesture(&g, &msg)) continue;
        /* Ctrl+K asks what note can do; Shift+Shift asks where to go.  They
         * are the same overlay in two different modes. */
        if (shift_gesture(&g, &msg)) { pal_open_mode(&g, PAL_MODE_TABS); continue; }
        if (g.accel && TranslateAcceleratorW(g.wnd, g.accel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

/* No CRT: this is the raw entry point named by the linker. */
void noteEntry(void)
{
    ExitProcess((UINT)note_main());
}

