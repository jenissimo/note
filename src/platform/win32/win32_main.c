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

#ifdef _M_IX86
/* The third CRT helper, and the one whose absence cost the most.
 *
 * A thread's stack is reserved whole but committed a page at a time: below
 * the live stack sits one guard page, and touching it is the signal to
 * commit another and move the guard down.  A function whose frame is larger
 * than a page can therefore step clean over the guard and land on reserved
 * address space that will never be committed, and the fault that follows is
 * not recoverable -- it is the "stack fault in KERNEL32" that Windows 95
 * reports.  So the compiler emits a call to _chkstk at the top of any such
 * function, whose job is to walk the frame downward touching one page at a
 * time in order, never skipping the guard.
 *
 * _chkstk lives in the CRT, and this program has no CRT, so the build asked
 * for /Gs1000000 -- "no frame is ever big enough to need probing" -- which
 * is a promise the code does not keep.  Twice now a routine has quietly
 * grown past a page (note_syntax_add with two string buffers,
 * theme_fit_display with a 256-entry palette twice over) and crashed on 95
 * while running perfectly on 11, where the stack happened to be committed
 * far enough down already.  Nothing warns about it: not the compiler, not
 * the linker, not dumpbin.  Supplying the helper costs forty bytes and ends
 * the whole class of bug, so the flag is gone and this stands in its place.
 *
 * Contract, which is why this is asm and not C: EAX holds the frame size,
 * the return address is on top of the stack, and every register except EAX
 * must come back unchanged -- the caller is mid-prologue and has its
 * arguments live.  The name is _chkstk with one underscore in the object
 * file, so the C identifier is spelled without it and __cdecl adds it. */
__declspec(naked) void __cdecl chkstk(void)
{
    __asm {
        push    ecx
        lea     ecx, [esp + 8]      ; the caller's esp, before the call
        cmp     eax, 0x1000
        jb      last
    probe:
        sub     ecx, 0x1000
        sub     eax, 0x1000
        test    dword ptr [ecx], eax    ; touch it; the value is irrelevant
        cmp     eax, 0x1000
        jae     probe
    last:
        sub     ecx, eax
        mov     eax, esp
        test    dword ptr [ecx], eax
        mov     esp, ecx                ; the frame is now backed by pages
        mov     ecx, dword ptr [eax]        ; ecx as the caller left it
        mov     eax, dword ptr [eax + 4]    ; the return address
        push    eax
        ret
    }
}
#endif /* _M_IX86 */


menu_item_data g_mid[192];
int            g_nmid;

struct note_host g;

COLORREF cr(note_color rgb)
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
        HMODULE u = os_module(N("user32.dll"));
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

#if !NOTE_OWN_VIEW

static const GUID kIID_ITextDocument =
    { 0x8CC497C0, 0xA1DF, 0x11CE, { 0x80, 0x98, 0x00, 0xAA, 0x00, 0x47, 0xBE, 0x5D } };

ITextDocument *tom_open(HWND edit)
{
    IRichEditOle  *ole = 0;
    ITextDocument *doc = 0;

    os_send(edit, EM_GETOLEINTERFACE, 0, (LPARAM)&ole);
    if (!ole) return 0;

    if (FAILED(ole->lpVtbl->QueryInterface(ole, &kIID_ITextDocument, (void **)&doc)))
        doc = 0;
    ole->lpVtbl->Release(ole);
    return doc;
}

#endif /* !NOTE_OWN_VIEW -- the view's undo is the buffer's, and nothing
        * programmatic ever writes to it */

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
            os_post(h->wnd, WM_SYSCOMMAND, SC_KEYMENU,
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
                os_post(h->wnd, WM_SYSCOMMAND, SC_KEYMENU, 0);
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

    f = os_font(&h->font);
    old = (HFONT)SelectObject(dc, f);
    os_text_metrics(dc, &tm);
    h->line_h = tm.tmHeight;
    h->char_w = tm.tmAveCharWidth;
    SelectObject(dc, old);
    DeleteObject(f);
    ReleaseDC(h->wnd, dc);

    if (h->uifont) DeleteObject(h->uifont);
    h->uifont = os_font(&h->font);
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

    {   /* In the DWM caption the strip stops short of the system buttons, so
         * that the corner stays glass and the compositor's buttons show
         * through it.  Where note draws the buttons itself the strip spans the
         * whole band and keeps the tabs out of that corner instead — see
         * tabs_right(). */
        int w = rc.right;
        if (h->frame_dwm) w -= caption_buttons_w(h);
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
    int from, to, line, col;

    if (!h->app.status || !active_edit()) return;

    h_sel_get(h, &from, &to);
    edit_status_pos(h, from, &line, &col);
    note_status_at(&h->app, line, col);
}

/* -------------------------------------------------------------------------
 * Find/replace message routing
 * ------------------------------------------------------------------------- */

void on_find_msg(note_host *h, FINDREPLACEW *fr)
{
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
        int from, to, len = n_len(h->app.find);
        h_sel_get(h, &from, &to);
        if (to - from == len) {
            nchar *cur = (nchar *)h_alloc(h, (DWORD)(len + 2) * sizeof(nchar));
            if (cur) {
                edit_text_range(h, from, to, cur, len + 2);
                if (n_eq(cur, h->app.find))
                    h_sel_replace(h, h->app.replace);
                h_free(h, cur);
            }
        }
        if (!h_find_text(h, h->app.find, h->app.find_flags))
            h_message(h, N("Cannot find that text."), N("note"));

    } else if (fr->Flags & FR_REPLACEALL) {
        int n = 0, guard = 0, from, to;
        int last = -1;
        h_sel_set(h, 0, 0);
        while (h_find_text(h, h->app.find, h->app.find_flags | FIND_DOWN)) {
            h_sel_get(h, &from, &to);
            if (from <= last) break;             /* wrapped around */
            last = from;
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

/* The caption buttons, where they are note's own.  Hover and press are the
 * compositor's business only in the DWM frame; without it nothing lights them
 * up unless this does. */
static void cap_btn_state(note_host *h, int hot, int press)
{
    if (h->cap_hot == hot && h->cap_press == press) return;
    h->cap_hot   = hot;
    h->cap_press = press;
    if (h->tabs) InvalidateRect(h->tabs, NULL, FALSE);
}

/* Asks to be told when the pointer leaves the non-client area, so a button the
 * pointer left over the top edge of the screen does not stay lit.  TME_NONCLIENT
 * arrived with TrackMouseEvent itself, in Windows 98 and NT 4; on 95 there is
 * no call to make and the light goes out on the next move over the caption. */
#define OS_TME_NONCLIENT 0x00000010

static void cap_btn_track(note_host *h)
{
    TRACKMOUSEEVENT tme;
    if (!caps.track_mouse) return;
    tme.cbSize      = sizeof(tme);
    tme.dwFlags     = TME_LEAVE | OS_TME_NONCLIENT;
    tme.hwndTrack   = h->wnd;
    tme.dwHoverTime = 0;
    caps.track_mouse(&tme);
}

/* A point given in screen coordinates, as every WM_NC* mouse message gives it,
 * in the client coordinates sysbtn is measured in. */
static int cap_btn_at_screen(note_host *h, HWND wnd, LPARAM lp)
{
    POINT pt;
    pt.x = (short)LOWORD(lp);
    pt.y = (short)HIWORD(lp);
    ScreenToClient(wnd, &pt);
    return caption_btn_at(h, pt.x, pt.y);
}

static LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    note_host *h = &g;

    if (h->findmsg && msg == h->findmsg) {
        on_find_msg(h, os_find_msg((void *)lp));
        return 0;
    }

    /* The buttons DWM draws in the caption answer here first, so hovering and
     * clicking minimise/maximise/close still behave like a real title bar.
     * frame_dwm is only ever set once frame_custom() has the whole DWM frame,
     * so the pointer is there whenever the flag is.  The frame without DWM has
     * no compositor to ask and answers for its own buttons below. */
    if (h->frame_dwm) {
        LRESULT dwm = 0;
        if (caps.dwm_defproc(wnd, msg, wp, lp, &dwm)) return dwm;
    }

    switch (msg) {

    /* Take the caption band back into the client rect, leaving the resize
     * borders where they were.  How far down the client then starts is
     * frame_client_top(): with a compositor the whole top of the window, since
     * DWM draws no border of ours up there; without one only the caption, so
     * the resize border stays non-client and the band note paints is the
     * caption and nothing more.  Maximised, the window rect hangs a border's
     * width off every edge of the monitor, so the top has to come back in by
     * that much either way or the tabs would be drawn off the screen. */
    case WM_NCCALCSIZE:
        if (h->title_tabs && wp) {
            NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
            LONG    top = p->rgrc[0].top;
            LRESULT lr  = os_defproc(wnd, msg, wp, lp);
            p->rgrc[0].top = top + frame_client_top(h);
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

        /* The system buttons.  In the DWM frame the compositor draws them and
         * DwmDefWindowProc lights them up, but only this answer makes them
         * clickable — and HTMAXBUTTON is also what the Snap Layouts flyout
         * waits for on hover.  Without DWM the same three codes are what bring
         * the presses below to a window that has no caption left to click. */
        if (pt.y >= 0 && pt.y < h->sysbtn.bottom &&
            pt.x >= h->sysbtn.left && pt.x < h->sysbtn.right) {
            /* Where note draws them, caption_btn_at() is the one place that
             * knows where they ended up -- they are not evenly spread, and a
             * third of the corner each would answer for the wrong one. */
            if (!h->frame_dwm) {
                int b = caption_btn_at(h, pt.x, pt.y);
                if (b == CAPBTN_MIN)   return HTMINBUTTON;
                if (b == CAPBTN_MAX)   return HTMAXBUTTON;
                if (b == CAPBTN_CLOSE) return HTCLOSE;
            } else {
                int w   = h->sysbtn.right - h->sysbtn.left;
                int off = pt.x - h->sysbtn.left;
                if (off * 3 < w)     return HTMINBUTTON;
                if (off * 3 < w * 2) return HTMAXBUTTON;
                return HTCLOSE;
            }
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

    /* ---- the frame nobody else draws -----------------------------------
     *
     * Everything from here to WM_MOUSEWHEEL is the frame without DWM.  With
     * the caption inside the client rect the system has no band left to draw
     * in, but it will still try; and the three buttons that DWM would have
     * composited behind the strip are, here, three rectangles note painted,
     * which nothing will light up or act on unless it is done below. */

    case WM_NCACTIVATE:
        /* Nothing in this caption changes when the window is activated -- the
         * tabs are the same tabs -- and the default handling would draw the
         * system's own caption over the client area to say otherwise.  TRUE is
         * "the frame is drawn", which it is. */
        if (h->title_tabs && !h->frame_dwm) return TRUE;
        break;

    case WM_NCPAINT:
        if (h->title_tabs && !h->frame_dwm) {
            frame_border_paint(h, wnd);
            return 0;
        }
        break;

    /* WM_NCUAHDRAWCAPTION and WM_NCUAHDRAWFRAME.  Undocumented, and sent by the
     * themed frame of XP and Vista when it wants the caption or the border
     * repainted in the system's style -- which, with the caption inside the
     * client rect, means over note's own pixels.  Answering them is what keeps
     * that paint from happening; on a Windows that never sends them this costs
     * nothing. */
    case 0x00AE:
    case 0x00AF:
        if (h->title_tabs && !h->frame_dwm) return 0;
        break;

    case WM_NCMOUSEMOVE:
        if (h->title_tabs && !h->frame_dwm) {
            int b = cap_btn_at_screen(h, wnd, lp);
            /* Once per entry, not once per move. */
            if (b != CAPBTN_NONE && h->cap_hot == CAPBTN_NONE) cap_btn_track(h);
            cap_btn_state(h, b, h->cap_press);
            return 0;
        }
        break;

    /* WM_NCMOUSELEAVE, which only arrives where cap_btn_track() could ask. */
    case 0x02A2:
        if (h->title_tabs && !h->frame_dwm) {
            cap_btn_state(h, CAPBTN_NONE, h->cap_press);
            return 0;
        }
        break;

    /* A press on one of the three.  Held rather than acted on, and tracked
     * through the mouse capture, so that sliding off the button before letting
     * go cancels it the way a button is expected to. */
    case WM_NCLBUTTONDOWN: {
        int b;
        if (!h->title_tabs || h->frame_dwm) break;
        b = cap_btn_at_screen(h, wnd, lp);
        if (b == CAPBTN_NONE) break;
        cap_btn_state(h, b, b);
        SetCapture(wnd);
        return 0;
    }

    case WM_MOUSEMOVE:
        if (h->cap_press != CAPBTN_NONE) {
            cap_btn_state(h, caption_btn_at(h, (short)LOWORD(lp),
                                            (short)HIWORD(lp)), h->cap_press);
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (h->cap_press != CAPBTN_NONE) {
            int held = h->cap_press;
            int over = caption_btn_at(h, (short)LOWORD(lp), (short)HIWORD(lp));
            ReleaseCapture();
            cap_btn_state(h, over, CAPBTN_NONE);
            if (over == held) {
                WPARAM sc = held == CAPBTN_MIN   ? SC_MINIMIZE
                          : held == CAPBTN_CLOSE ? SC_CLOSE
                          : (IsZoomed(wnd) ? SC_RESTORE : SC_MAXIMIZE);
                os_post(wnd, WM_SYSCOMMAND, sc, 0);
            }
            return 0;
        }
        break;

    /* The capture can be taken away -- Alt+Tab, a message box -- and then the
     * button up that would have finished the press never arrives here. */
    case WM_CAPTURECHANGED:
        if (h->cap_press != CAPBTN_NONE)
            cap_btn_state(h, CAPBTN_NONE, CAPBTN_NONE);
        break;

    /* The wheel over the caption band belongs to the strip, which the hit test
     * has just declared is not there. */
    case WM_MOUSEWHEEL: {
        POINT pt;
        if (!h->title_tabs) break;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        ScreenToClient(wnd, &pt);
        if (pt.y >= 0 && pt.y < h->tabs_h)
            return os_send(h->tabs, WM_MOUSEWHEEL, wp, lp);
        break;
    }

    case WM_SIZE:
        relayout(h);
        if (h->help_open) help_close(h);
        if (h->pal_open) pal_layout(h);
        queue_view(h);
        return 0;

    case WM_MOVE:
        if (h->pal_open) pal_layout(h);
        return 0;

    /* The overlay belongs to this window's focus, not its own. */
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) { pal_close(h); help_close(h); }
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
                if (cmd) os_post(wnd, WM_SYSCOMMAND, (WPARAM)cmd, 0);
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
            const menu_item_data *d;
            const nchar *s;

            d = (const menu_item_data *)os_menu_item_data(m, (UINT)i, 0);
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
        if (wp == TIMER_HL)      { hl_step(h); return 0; }
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
        if (h->frame_dwm && h->sysbtn.right > h->sysbtn.left) {
            RECT b = h->sysbtn;
            /* Only as far down as the frame is actually extended.  Below that
             * the corner is ordinary client area and the br_ui already in it is
             * what belongs there; blacking it would leave the strip of night
             * under the buttons that caption_dwm_top() exists to fix. */
            b.bottom = caption_dwm_top();
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
        if (!um || !caps.menu_bar_info) break;
        if (!caps.menu_bar_info(wnd, OBJID_MENU, 0, &mbi)) break;
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
        const menu_item_data *d;
        int   hot;

        if (!udmi) break;

        d = (const menu_item_data *)
            os_menu_item_data(udmi->um.hmenu, (UINT)udmi->umi.iPosition, 0);
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
#if !NOTE_OWN_VIEW
            /* EN_CHANGE can trail our own edits, so the control's modify flag
             * — which we restore after every programmatic change — is the
             * authority on whether a person actually typed something. */
            if (HIWORD(wp) == EN_CHANGE && !h->suppress &&
                os_send((HWND)lp, EM_GETMODIFY, 0, 0)) {
                int doc;
                for (doc = 0; doc < NOTE_MAX_DOCS; doc++)
                    if (h->d[doc].edit == (HWND)lp) {
                        CHARRANGE at;
                        note_set_dirty(&h->app, doc, 1);
                        /* Where the edit landed, so the colouring above it is
                         * kept rather than redone. */
                        os_send((HWND)lp, EM_EXGETSEL, 0, (LPARAM)&at);
                        hl_touch(h, doc, (int)at.cpMin);
                        if (doc == h->app.active) {
                            h->cache_valid = 0;
                            queue_view(h);
                            update_status(h);
                        }
                        break;
                    }
            }
#endif  /* the view tells the core about its own edits as it makes them */
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

#if !NOTE_OWN_VIEW
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (!nm) break;
        if (nm->code == EN_SELCHANGE) {
            /* Our own selection round-trips -- the highlighter's, and the
             * caret band's -- send this back at us while they are restoring
             * what they found.  The same flag that keeps them from marking
             * the document dirty keeps them from recursing here. */
            if (h->suppress) return 0;
            update_status(h);
            /* The caret's line is washed in both the gutter and the text, so
             * both follow it -- including the moves no key of ours sees, like
             * a drag or a click that lands in another paragraph. */
            curline_update(h);
            if (h->gutter && h->app.linenums)
                InvalidateRect(h->gutter, NULL, FALSE);
            return 0;
        }
        break;
    }
#endif  /* the view moves its own caret, and knows when it did */

    case WM_DROPFILES: {
        nchar path[NOTE_PATH_MAX];
        HDROP drop = (HDROP)wp;
        UINT n = os_drag_query(drop, 0xFFFFFFFF, NULL, 0), i;
        for (i = 0; i < n; i++)
            if (os_drag_query(drop, i, path, NOTE_PATH_MAX))
                note_open(&h->app, path);
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

    return os_defproc(wnd, msg, wp, lp);
}

/* -------------------------------------------------------------------------
 * Startup
 * ------------------------------------------------------------------------- */

static void first_arg(nchar *out, int cap)
{
    nchar line[NOTE_PATH_MAX * 2];
    const nchar *p = line;
    const nchar quote = (nchar)'"', space = (nchar)' ', tab = (nchar)'\t';
    int i = 0;

    out[0] = 0;
    os_command_line(line, NOTE_PATH_MAX * 2);

    if (*p == quote) { p++; while (*p && *p != quote) p++; if (*p) p++; }
    else             { while (*p && *p != space && *p != tab) p++; }
    while (*p == space || *p == tab) p++;
    if (!*p) return;

    if (*p == quote) {
        p++;
        while (*p && *p != quote && i < cap - 1) out[i++] = *p++;
    } else {
        while (*p && i < cap - 1) out[i++] = *p++;
        while (i > 0 && (out[i - 1] == space || out[i - 1] == tab)) i--;
    }
    out[i] = 0;
}

static void enable_dpi(void)
{
    typedef BOOL (WINAPI *PFN)(HANDLE);
    HMODULE u = os_module(N("user32.dll"));
    PFN p = u ? (PFN)GetProcAddress(u, "SetProcessDpiAwarenessContext") : 0;
    if (p) p((HANDLE)-4);          /* PER_MONITOR_AWARE_V2 */
}

static int note_main(void)
{
    WNDCLASSEXW wc;
    MSG   msg;
    nchar arg[NOTE_PATH_MAX];
    int   i, restored;

    /* Before anything else, caps_probe() included: that already goes through
     * the boundary to load a library by name, and nothing may call a Windows
     * function carrying text until this has settled which half of the API
     * answers. */
    wide_probe();

    /* Every later decision about the frame and the menus reads the answers
     * this fills in. */
    caps_probe();

    enable_dpi();

    g.inst = os_module(0);
    g.heap = GetProcessHeap();
    g.tab_hot  = -1;
    g.menu_hot = -1;

    /* Ask for the dark palette before the frame exists: the non-client area
     * reads it once at creation.  note_apply_theme() corrects it afterwards
     * if the user prefers a fixed light or dark theme. */
    set_app_dark(h_system_dark(&g));

#if !NOTE_OWN_VIEW
    if (!os_library(N("Msftedit.dll"))) {
        os_message_box(NULL, N("Msftedit.dll is missing."), N("note"),
                       MB_OK | MB_ICONERROR);
        return 1;
    }
#endif

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = g.inst;
    /* The application icon, compiled in as resource 1.  A build without the
     * resource still runs; it just gets the shell's default. */
    wc.hIcon         = os_icon(g.inst, 1);
    if (!wc.hIcon) wc.hIcon = os_icon(NULL, 32512 /* IDI_APPLICATION */);
    wc.hIconSm       = wc.hIcon;
    wc.hCursor       = os_arrow_cursor();
    wc.hbrBackground = NULL;          /* WM_ERASEBKGND paints from the theme */
    wc.lpszClassName = L"noteWindow";
    /* Checked, every one of them: a class that fails to register is not a
     * degraded feature, it is a window that can never be created, and the
     * only symptom is a shortcut that does nothing. */
    if (!os_register_class(&wc)) return 1;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = GutterProc;
    wc.hInstance     = g.inst;
    wc.hCursor       = os_arrow_cursor();
    wc.lpszClassName = L"noteGutter";
    if (!os_register_class(&wc)) return 1;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = StatusProc;
    wc.hInstance     = g.inst;
    wc.hCursor       = os_arrow_cursor();
    wc.lpszClassName = L"noteStatus";
    if (!os_register_class(&wc)) return 1;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DBLCLKS;      /* the strip answers double clicks */
    wc.lpfnWndProc   = TabsProc;
    wc.hInstance     = g.inst;
    wc.hCursor       = os_arrow_cursor();
    wc.lpszClassName = L"noteTabs";
    if (!os_register_class(&wc)) return 1;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DROPSHADOW;   /* a card over the editor, not a frame */
    wc.lpfnWndProc   = PaletteProc;
    wc.hInstance     = g.inst;
    wc.hCursor       = os_arrow_cursor();
    wc.lpszClassName = L"notePalette";
    if (!os_register_class(&wc)) return 1;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DROPSHADOW;
    wc.lpfnWndProc   = HelpProc;
    wc.hInstance     = g.inst;
    wc.hCursor       = os_arrow_cursor();
    wc.lpszClassName = L"noteHelp";
    if (!os_register_class(&wc)) return 1;

    /* WS_CLIPCHILDREN because the caption is the tab strip's window and the
     * frame must not be able to draw over it.  Without a compositor in front
     * of the frame, the default handling still repaints the caption it thinks
     * it has -- on a change of window text, on activation -- and it does that
     * through a window DC, which reaches the client area unless the children
     * are clipped out of it. */
    g.wnd = os_create_window(WS_EX_ACCEPTFILES, N("noteWindow"), N("note"),
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 900, 660,
                             NULL, NULL, g.inst, NULL);
    if (!g.wnd) return 1;

    g.tabs = os_create_window(0, N("noteTabs"), N(""), WS_CHILD | WS_VISIBLE,
                              0, 0, 0, 0, g.wnd, (HMENU)ID_TABS, g.inst, NULL);
    g.status = os_create_window(0, N("noteStatus"), N(""), WS_CHILD | WS_VISIBLE,
                                0, 0, 0, 0, g.wnd, (HMENU)ID_STATUS, g.inst, NULL);
    g.gutter = os_create_window(0, N("noteGutter"), N(""), WS_CHILD | WS_VISIBLE,
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

    /* The shell's own menu font, so owner-drawn items still look native. */
    g.menufont = os_menu_font();
    if (!g.menufont) g.menufont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    note_init(&g.app, &g, &kOps);
    build_menus(&g);
    build_accels(&g);

    note_defs_load(&g.app);

    note_apply_theme(&g.app);

    os_send(g.tabs,   WM_SETFONT, (WPARAM)g.menufont, TRUE);
    os_send(g.status, WM_SETFONT, (WPARAM)g.menufont, TRUE);

    /* FINDMSGSTRING, spelled out: the constant comes in a W and an A form and
     * the name they register is the same either way. */
    g.findmsg = os_register_message(N("commdlg_FindReplace"));

    restored = note_session_restore(&g.app);
    first_arg(arg, NOTE_PATH_MAX);

    /* The file named on the command line is opened below, once there is a
     * window to open it into.  Only the empty document is settled here: with a
     * path to open there is no need for one, since note_open() takes over an
     * untouched Untitled tab rather than adding to it. */
    if (!arg[0] && !restored) {
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

    /* And now the command line.  Asking for a file by name is the user's own
     * request, so a name that is not there is still reported -- but reported
     * over a window that is on the screen.  Opened before ShowWindow, that
     * message box was a modal in front of nothing: note appeared not to have
     * started at all until the invisible dialog was found and dismissed, which
     * is the same trap a restored session used to fall into. */
    if (arg[0]) note_open(&g.app, arg);

    relayout(&g);
    SetFocus(active_edit());
    update_status(&g);
    queue_view(&g);

    SetTimer(g.wnd, TIMER_SESSION, 4000, NULL);

    while (os_get_message(&msg) > 0) {
        if (g.finddlg && os_is_dialog_message(g.finddlg, &msg)) continue;

        /* The palette never takes focus, so the keys are still addressed to
         * the editor: intercept them here, before the accelerator table or
         * the control itself can act on them. */
        /* The sheet answers nothing, so it wants far less than the palette:
         * a key to put it away and a click anywhere to do the same. */
        if (g.help_open) {
            if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
                if (help_key(&g, (int)msg.wParam)) continue;
            } else if (msg.message == WM_LBUTTONDOWN ||
                       msg.message == WM_RBUTTONDOWN ||
                       msg.message == WM_NCLBUTTONDOWN) {
                if (msg.hwnd != g.help) help_close(&g);
            }
        }

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
                pal_char(&g, os_wm_char((unsigned)msg.wParam));
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
        if (g.accel && os_translate_accel(g.wnd, g.accel, &msg)) continue;
        TranslateMessage(&msg);
        os_dispatch(&msg);
    }
    return (int)msg.wParam;
}

/* No CRT: this is the raw entry point named by the linker. */
void noteEntry(void)
{
    ExitProcess((UINT)note_main());
}

