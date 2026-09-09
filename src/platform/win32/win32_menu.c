/* win32_menu.c -- menus and the tab strip, both drawn by hand from the theme
 *
 * Part of note's Win32 backend.  The shared state and the types every part of
 * it needs live in note_win32.h; see there for why the state is a header and
 * not a private static.
 */

#include "note_win32.h"

/* -------------------------------------------------------------------------
 * Menus, accelerators, layout — driven by the core's tables
 * ------------------------------------------------------------------------- */

menu_item_data *mid_new(const nchar *label, int onbar, int checkcol)
{
    menu_item_data *d;
    if (g_nmid >= (int)(sizeof(g_mid) / sizeof(g_mid[0]))) return 0;
    d = &g_mid[g_nmid++];
    d->label    = label;
    d->onbar    = onbar;
    d->checkcol = checkcol;
    return d;
}

static HMENU build_popup(const note_menu_item *items, const note_menu_item **out)
{
    HMENU m = CreatePopupMenu();
    const note_menu_item *it;
    int   checkcol = 0;

    /* The check gutter belongs to the menu, not to the item: a popup where
     * nothing is ever checked wants its text where the gutter would be, which
     * is the difference between a native-width menu and one that is a
     * finger-width too wide everywhere. */
    for (it = items; it->kind != MI_END; it++)
        if (it->kind == MI_CHECK || it->kind == MI_RADIO) checkcol = 1;

    for (it = items; ; it++) {
        if (it->kind == MI_END) break;
        /* Still a command, just not one this menu draws — the palette finds
         * it through the same table. */
        if (it->kind == MI_HIDDEN) continue;
        if (it->kind == MI_SEP)
            AppendMenuW(m, MF_OWNERDRAW, 0, NULL);
        else
            AppendMenuW(m, MF_OWNERDRAW, it->id,
                        (LPCWSTR)mid_new(it->label, 0, checkcol));
    }
    if (out) *out = it;
    return m;
}

/* The background brush is the one thing Windows will honour for a popup, so
 * set it alongside the owner-drawn items: it covers the border padding the
 * items themselves never reach. */
void menu_set_brush(HMENU m, HBRUSH br)
{
    MENUINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize  = sizeof(mi);
    mi.fMask   = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
    mi.hbrBack = br;
    SetMenuInfo(m, &mi);
}

void build_menus(note_host *h)
{
    const note_menu_item *it = note_menu;

    g_nmid = 0;
    h->menubar = CreateMenu();
    while (it->kind == MI_POPUP) {
        const note_menu_item *end;
        const nchar *name = it->label;
        HMENU sub = build_popup(it + 1, &end);
        AppendMenuW(h->menubar, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)sub,
                    (LPCWSTR)mid_new(name, 1, 0));
        it = end + 1;
    }
    /* Built, but not attached: the bar stays out of the way until Alt asks
     * for it.  See menubar_show(). */
    h->menu_visible = 0;
    h->ctxmenu = build_popup(note_ctxmenu, NULL);
}

/* Attaching and detaching the bar rather than hiding a window: the menu bar
 * lives in the frame, so this is also what gives the editor the height back.
 * SWP_FRAMECHANGED is what makes Windows recompute the client area and send
 * the WM_SIZE that reflows everything below. */
void menubar_show(note_host *h, int show)
{
    /* A command may have been the one that closed the window. */
    if (!h->menubar || !h->wnd || h->quitting) return;
    show = show ? 1 : 0;
    if (h->menu_visible == show) return;

    h->menu_visible = show;
    h->menu_hot     = -1;
    SetMenu(h->wnd, show ? h->menubar : NULL);
    DrawMenuBar(h->wnd);
    SetWindowPos(h->wnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                 SWP_FRAMECHANGED);
    relayout(h);
    /* Last, and only now: relayout has just moved the editor off the band the
     * bar sits in, and what it uncovers is note's to paint.  Hiding needs no
     * such thing — the editor moves back over the band and paints it. */
    if (show) {
        RECT band;
        if (menubar_band(h, &band)) InvalidateRect(h->wnd, &band, TRUE);
    }
}

/* ---- owner drawing ------------------------------------------------------ */

/* An item is padding, an optional check gutter, the label, a gap and the
 * accelerator.  Windows 11's own popups are looser than this — a native menu
 * measured on the same screen indents its labels by 37px and holds the
 * accelerator 40px clear — but every one of those pixels is spent on a column
 * note's menus have nothing to put in, which is what made items visibly wider
 * than their text.  What is kept from the system is the item height and the
 * check gutter's width, and the gutter only in a menu that has a check. */
#define MENU_CHECK_W   22
#define MENU_PAD       10
#define MENU_ACCEL_GAP 18

/* Splits "&Open...\tCtrl+O" into its label and its accelerator. */
static void split_accel(const WCHAR *src, WCHAR *text, WCHAR *accel, int cap)
{
    int i = 0, j = 0;
    text[0] = accel[0] = 0;
    if (!src) return;
    while (src[i] && src[i] != L'\t' && i < cap - 1) { text[i] = src[i]; i++; }
    text[i] = 0;
    if (src[i] == L'\t') {
        i++;
        while (src[i] && j < cap - 1) accel[j++] = src[i++];
    }
    accel[j] = 0;
}

/* `num`/`den` of the way from b to a.  Half way is right for dimmed text but
 * far too strong for a hairline, which wants to be only just visible. */
COLORREF mix_rgb(unsigned a, unsigned b, int num, int den)
{
    unsigned r  = ((((a >> 16) & 0xFF) * num) + (((b >> 16) & 0xFF) * (den - num))) / den;
    unsigned gg = ((((a >>  8) & 0xFF) * num) + (((b >>  8) & 0xFF) * (den - num))) / den;
    unsigned bb = (((a & 0xFF) * num) + ((b & 0xFF) * (den - num))) / den;
    return RGB(r, gg, bb);
}

COLORREF blend_rgb(unsigned a, unsigned b)
{
    return mix_rgb(a, b, 1, 2);
}

/* Where an item's text starts.  On the bar that is plain padding; in a popup
 * it clears the check gutter, but only in a menu that has something checkable
 * in it — otherwise the text would be indented past a column nothing will ever
 * be drawn in.  Measuring and drawing both read this, so they cannot drift. */
static int item_text_left(const menu_item_data *d)
{
    if (!d || d->onbar || !d->checkcol) return px(MENU_PAD);
    return px(MENU_CHECK_W);
}

void measure_menu_item(note_host *h, MEASUREITEMSTRUCT *mis)
{
    const menu_item_data *d = (const menu_item_data *)mis->itemData;
    WCHAR text[256], accel[64];
    HDC   dc;
    HFONT old;
    RECT  rc;

    if (!d) { mis->itemWidth = 0; mis->itemHeight = (UINT)px(7); return; }

    split_accel((const WCHAR *)d->label, text, accel, 256);

    dc  = GetDC(h->wnd);
    old = (HFONT)SelectObject(dc, h->menufont);

    rc.left = rc.top = rc.right = rc.bottom = 0;
    DrawTextW(dc, text, -1, &rc, DT_CALCRECT | DT_SINGLELINE);
    mis->itemWidth  = (UINT)(item_text_left(d) + rc.right + px(MENU_PAD));
    mis->itemHeight = (UINT)(rc.bottom + px(8));

    if (accel[0]) {
        RECT ar;
        ar.left = ar.top = ar.right = ar.bottom = 0;
        DrawTextW(dc, accel, -1, &ar, DT_CALCRECT | DT_SINGLELINE);
        mis->itemWidth += (UINT)(ar.right + px(MENU_ACCEL_GAP));
    }
    if (mis->itemHeight < (UINT)px(24)) mis->itemHeight = (UINT)px(24);

    SelectObject(dc, old);
    ReleaseDC(h->wnd, dc);
}

static void draw_check(HDC dc, const RECT *rc, COLORREF c)
{
    HPEN pen = CreatePen(PS_SOLID, 2, c);
    HPEN old = (HPEN)SelectObject(dc, pen);
    int  cx  = (rc->left + rc->right) / 2;
    int  cy  = (rc->top + rc->bottom) / 2;

    MoveToEx(dc, cx - 4, cy, NULL);
    LineTo(dc, cx - 1, cy + 3);
    LineTo(dc, cx + 5, cy - 4);

    SelectObject(dc, old);
    DeleteObject(pen);
}

void draw_menu_item(note_host *h, DRAWITEMSTRUCT *dis)
{
    const menu_item_data *d = (const menu_item_data *)dis->itemData;
    RECT  rc = dis->rcItem, tr;
    WCHAR text[256], accel[64];
    HFONT old;
    UINT  fmt = DT_SINGLELINE | DT_VCENTER;
    int   selected, disabled;

    if (!d) {                                    /* separator */
        RECT ln = rc;
        FillRect(dis->hDC, &rc, h->br_menu);
        ln.top    = (rc.top + rc.bottom) / 2;
        ln.bottom = ln.top + 1;
        /* A separator carries no item data, so it cannot know whether its menu
         * has a check gutter; the plain padding is what Windows 11 uses and it
         * is right either way. */
        ln.left  += px(MENU_PAD);
        ln.right -= px(MENU_PAD);
        FillRect(dis->hDC, &ln, h->br_sep);
        return;
    }

    selected = (dis->itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
    disabled = (dis->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;

    FillRect(dis->hDC, &rc, selected ? h->br_menusel : h->br_menu);

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, disabled ? blend_rgb(h->theme.ui_fg, h->theme.ui_bg)
                                    : cr(h->theme.ui_fg));
    old = (HFONT)SelectObject(dis->hDC, h->menufont);

    if (dis->itemState & ODS_NOACCEL) fmt |= DT_HIDEPREFIX;

    split_accel((const WCHAR *)d->label, text, accel, 256);

    if (!d->onbar && (dis->itemState & ODS_CHECKED)) {
        RECT ck = rc;
        ck.right = ck.left + px(MENU_CHECK_W);
        draw_check(dis->hDC, &ck, cr(h->theme.ui_fg));
    }

    tr = rc;
    tr.left += item_text_left(d);
    DrawTextW(dis->hDC, text, -1, &tr, fmt);

    if (accel[0]) {
        tr = rc;
        tr.right -= px(MENU_PAD);
        SetTextColor(dis->hDC, blend_rgb(h->theme.ui_fg, h->theme.ui_bg));
        DrawTextW(dis->hDC, accel, -1, &tr, fmt | DT_RIGHT);
    }

    SelectObject(dis->hDC, old);
}

/* ---- the caption ---------------------------------------------------------
 *
 * The tab strip is drawn in the title bar rather than under it.  Three pieces
 * make that work: the frame is extended into the client area so the window
 * keeps its shadow, its snap behaviour and — crucially — the system's own
 * minimise/maximise/close buttons, which DWM goes on drawing for us; the
 * caption band is then taken back into the client rect by WM_NCCALCSIZE; and
 * WM_NCHITTEST hands the empty part of the strip back to the window manager
 * as HTCAPTION so dragging and double-click still work.
 *
 * All three are optional.  If composition is off or the frame will not extend,
 * title_tabs stays 0 and the layout is the one that was there before.
 * ------------------------------------------------------------------------- */

/* The resize border, which is also how much taller a maximised window's
 * caption is: maximised, the window rect hangs this far off every edge. */
int frame_edge(void)
{
    int b = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
    return b > 0 ? b : px(8);
}

/* Tall enough for the system buttons DWM draws into it, and never shorter
 * than the strip needs. */
int caption_height(void)
{
    int c = GetSystemMetrics(SM_CYCAPTION) + frame_edge();
    int t = px(TABS_H);
    return c > t ? c : t;
}

/* The band the menu bar occupies, in client pixels; 0 when the bar is not up.
 *
 * With the caption taken into the client rect the bar cannot be subtracted
 * from that rect the way it is in an ordinary window — the strip would go
 * under it — so the bar is laid out inside note's client area, just below the
 * band the tabs are drawn in.  Two things follow, and neither was being done:
 * the editor has to start below the bar rather than behind it, and whatever
 * else paints the client has to leave those rows alone, because nothing tells
 * Windows to draw the bar again after they are erased.
 *
 * Where exactly Windows put it is worth asking rather than working out twice:
 * the bar's height is the tallest item note measured, and its top is whatever
 * caption height Windows believes it has. */
int menubar_band(note_host *h, RECT *band)
{
    MENUBARINFO mbi;
    RECT wr;
    int  top;

    band->left = band->top = band->right = band->bottom = 0;
    if (!h->menu_visible || !h->wnd) return 0;

    memset(&mbi, 0, sizeof(mbi));
    mbi.cbSize = sizeof(mbi);
    if (!GetMenuBarInfo(h->wnd, OBJID_MENU, 0, &mbi)) return 0;
    if (!GetWindowRect(h->wnd, &wr)) return 0;
    if (mbi.rcBar.bottom <= mbi.rcBar.top) return 0;

    /* WM_NCCALCSIZE puts the client top at the window top, except maximised,
     * where it comes in by the resize border. */
    top = wr.top + (h->title_tabs && IsZoomed(h->wnd) ? frame_edge() : 0);

    band->left   = 0;
    band->right  = wr.right - wr.left;
    band->top    = mbi.rcBar.top - top;
    band->bottom = mbi.rcBar.bottom - top;
    return band->bottom;
}

/* One top-level item, from the UAH message and from menubar_paint() alike, so
 * a bar note repaints looks the same as one Windows touches. */
void menubar_item_draw(note_host *h, HDC dc, const RECT *rc,
                       const menu_item_data *d, int hot)
{
    HFONT old;
    UINT  fmt = DT_CENTER | DT_VCENTER | DT_SINGLELINE;
    RECT  r = *rc;

    FillRect(dc, &r, hot ? h->br_menusel : h->br_ui);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, cr(h->theme.ui_fg));
    old = (HFONT)SelectObject(dc, h->menufont);
    DrawTextW(dc, (LPCWSTR)d->label, -1, &r, fmt);
    SelectObject(dc, old);
}

/* The whole menu bar, into a client DC.
 *
 * Windows draws the bar during WM_NCPAINT, and with the caption taken into the
 * client rect there is no non-client area left above it for that paint to be
 * clipped to — so the bar was arriving one item at a time, only as the
 * selection moved over them, on top of whatever the editor had left behind.
 * Individual items still come through the UAH message; the rest of the bar is
 * note's to draw, from the rectangles Windows laid out.
 *
 * Returns 0 when there is no bar up, so the caller can paint as it always did.
 */
int menubar_paint(note_host *h, HDC dc)
{
    RECT band;
    int  i, n;

    if (!h->menubar || !menubar_band(h, &band)) return 0;
    if (band.bottom <= band.top) return 0;

    FillRect(dc, &band, h->br_ui);

    n = GetMenuItemCount(h->menubar);
    for (i = 0; i < n; i++) {
        MENUITEMINFOW mii;
        RECT r;

        if (!GetMenuItemRect(h->wnd, h->menubar, (UINT)i, &r)) continue;
        MapWindowPoints(NULL, h->wnd, (POINT *)&r, 2);

        memset(&mii, 0, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask  = MIIM_DATA | MIIM_STATE;
        if (!GetMenuItemInfoW(h->menubar, (UINT)i, TRUE, &mii)) continue;
        if (!mii.dwItemData) continue;

        /* Which item is lit is Windows' business and it will say: MFS_HILITE
         * is on whichever one menu mode has walked to. */
        menubar_item_draw(h, dc, &r, (const menu_item_data *)mii.dwItemData,
                          (mii.fState & MFS_HILITE) != 0 || i == h->menu_hot);
    }
    return 1;
}

/* What to keep clear on the right, and where exactly it is.
 *
 * Anchored to the client rect rather than to the window rect: a maximised
 * window hangs frame_edge() off every edge of the monitor, so the same
 * rectangle measured from the window would put the close button that far past
 * the corner of the screen — half of it unclickable.  Recomputed whenever the
 * window is sized, because that is when maximising changes the answer.
 *
 * Windows will say how big its buttons are; the builds that will not are the
 * ones that still draw the old caption buttons, whose width is SM_CXSIZE. */
void caption_buttons_calc(note_host *h)
{
    RECT rc, b;
    int  w = 0, ht = 0;

    h->sysbtn.left = h->sysbtn.top = h->sysbtn.right = h->sysbtn.bottom = 0;
    if (!h->title_tabs || !h->wnd) return;
    GetClientRect(h->wnd, &rc);

    if (SUCCEEDED(DwmGetWindowAttribute(h->wnd, DWMWA_CAPTION_BUTTON_BOUNDS,
                                        &b, sizeof(b)))) {
        w  = b.right - b.left;
        ht = b.bottom - b.top;
    }
    if (w <= 0 || w >= rc.right) {
        int bw = GetSystemMetrics(SM_CXSIZE);
        if (bw <= 0) bw = px(32);
        w = bw * 3;
    }
    if (ht <= 0 || ht > h->tabs_h) ht = h->tabs_h;

    h->sysbtn.right  = rc.right;
    h->sysbtn.left   = rc.right - w > 0 ? rc.right - w : 0;
    h->sysbtn.bottom = ht;
}

int caption_buttons_w(note_host *h)
{
    if (h->sysbtn.right <= h->sysbtn.left) caption_buttons_calc(h);
    return h->sysbtn.right - h->sysbtn.left;
}

/* The seam.  DWM paints the corner behind its own buttons and note paints the
 * rest of the band, so the two have to be told the same colour — and the brush
 * the strip fills with is br_ui, built from the same ui_bg handed over here.
 * Windows 11 22000+ only; older builds refuse the attributes and keep the
 * system caption, which is what they had before any of this. */
void caption_colours(note_host *h)
{
    COLORREF cap, txt, brd;

    if (!h->title_tabs || !h->wnd) return;
    if (h->cap_set && h->cap_rgb == h->theme.ui_bg) return;
    h->cap_rgb = h->theme.ui_bg;
    h->cap_set = 1;

    cap = cr(h->theme.ui_bg);
    txt = cr(h->theme.ui_fg);
    brd = cr(h->theme.ui_bg);
    DwmSetWindowAttribute(h->wnd, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof(cap));
    DwmSetWindowAttribute(h->wnd, 36 /* DWMWA_TEXT_COLOR    */, &txt, sizeof(txt));
    DwmSetWindowAttribute(h->wnd, 34 /* DWMWA_BORDER_COLOR  */, &brd, sizeof(brd));
}

/* Takes the caption over, or leaves it alone and says so. */
void frame_custom(note_host *h)
{
    BOOL    comp = FALSE;
    MARGINS m;

    h->title_tabs = 0;
    if (FAILED(DwmIsCompositionEnabled(&comp)) || !comp) return;

    /* Extended by exactly the band we are about to draw in: that is the region
     * DWM composites the caption — and its buttons — behind our pixels. */
    m.cxLeftWidth = 0;
    m.cxRightWidth = 0;
    m.cyBottomHeight = 0;
    m.cyTopHeight = caption_height();
    if (FAILED(DwmExtendFrameIntoClientArea(h->wnd, &m))) return;

    h->title_tabs = 1;
    h->cap_set = 0;                 /* a new frame has the system's colours */
    caption_colours(h);
    /* WM_NCCALCSIZE is only asked again when the frame is declared stale. */
    SetWindowPos(h->wnd, NULL, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE);
}

/* ---- the tab strip ------------------------------------------------------
 *
 * Drawn from scratch rather than with SysTabControl32.  That control renders
 * its own border and the shelf beneath the tabs from the system theme, and
 * TCS_OWNERDRAWFIXED only hands over the interior of each tab, so a dark
 * palette was always framed in pale seams.  A plain window costs about the
 * same amount of code, themes exactly, and leaves room for the close box.
 * -------------------------------------------------------------------------- */

/* One set of numbers, in logical pixels, that both the layout and the paint
 * read: a tab is PAD, label, GAP, close box, PAD, and nothing in it is
 * positioned by an offset invented at the point of drawing. */
#define TAB_MIN_W    120      /* proportional shrink stops here; then scroll */
#define TAB_MAX_W    220
#define TAB_PAD        8      /* left of the label and right of the box      */
#define TAB_GAP        4      /* label to close box                          */
#define TAB_SLACK      4      /* so a title that just fits is not ellipsised */
#define TAB_CLOSE_W   16      /* the box, which is also the hit target       */
#define TAB_CLOSE_R    3      /* half the cross                              */
#define TAB_CLOSE_DIM 80      /* below this the box shows on active/hot only */
#define TAB_CLOSE_OFF 56      /* below this there is no box at all           */
#define TAB_FADE      28      /* the strip dissolves over this at an edge    */
#define TAB_ICON      16      /* the window icon, single-document mode only  */
#define TAB_ICON_GAP  11      /* icon to title: caption, not tab, colour     */
#define TAB_SEP_INSET  8      /* the divider is about half the band's height */

/* One document is not a tab, it is the window.  With a single document the
 * caption is an ordinary title bar — the window icon, the document's name as
 * caption text, and nothing in it to click; the tabs come back the moment
 * there is a second document to tell apart from the first. */
int caption_plain(note_host *h)
{
    return h->title_tabs && h->app.ndocs <= 1;
}

/* Where the strip's content starts.  With one document that is past the window
 * icon, whose square is also the system menu — see WM_NCHITTEST.  With tabs
 * there is no icon at all: the tabs already say what the window holds, and the
 * first of them runs to the very edge, where the window's rounded corner clips
 * it.  A tab rounded on its outer corner reads as the frame; a gap left to
 * keep it square reads as a mistake. */
int tabs_left(note_host *h)
{
    if (!h->title_tabs || !caption_plain(h)) return 0;
    return px(TAB_PAD) + px(TAB_ICON) + px(TAB_ICON_GAP);
}

/* One centre line for the whole strip, recomputed wherever the band can have
 * changed height — maximising makes the caption taller by the resize border.
 * Everything drawn in the strip is centred on this and nothing works out a
 * centre of its own. */
static void tabs_centre(note_host *h)
{
    RECT rc;
    if (!h->tabs) return;
    GetClientRect(h->tabs, &rc);
    h->tab_mid = (rc.bottom + 1) / 2;      /* whole pixels, after scaling */
}

/* The width the tabs themselves get.  The strip window already stops short of
 * the system buttons; this takes off the icon at the other end. */
static int tabs_view_w(note_host *h)
{
    RECT rc;
    int  w;
    GetClientRect(h->tabs, &rc);
    w = rc.right - tabs_left(h);
    return w > 0 ? w : 0;
}

static int tabs_total_w(note_host *h)
{
    int n = h->app.ndocs;
    return n > 0 ? h->d[n - 1].x + h->d[n - 1].w : 0;
}

/* Keeps the scroll inside the content and the active tab wholly visible.
 * Ctrl+Tab, a click in the palette's tab list and closing a tab all arrive
 * here through tabs_layout(). */
static void tabs_scroll_clamp(note_host *h)
{
    int view = tabs_view_w(h);
    int max  = tabs_total_w(h) - view;
    int a    = h->app.active;

    if (max < 0) max = 0;

    if (a >= 0 && a < h->app.ndocs) {
        if (h->d[a].x < h->tab_scroll) h->tab_scroll = h->d[a].x;
        if (h->d[a].x + h->d[a].w > h->tab_scroll + view)
            h->tab_scroll = h->d[a].x + h->d[a].w - view;
    }
    if (h->tab_scroll > max) h->tab_scroll = max;
    if (h->tab_scroll < 0)   h->tab_scroll = 0;
}

/* d[i].x is a position in the strip's own content, not on screen. */
static int tabs_screen_x(note_host *h, int i)
{
    return tabs_left(h) + h->d[i].x - h->tab_scroll;
}

void tabs_layout(note_host *h)
{
    HDC   dc;
    HFONT old;
    int   i, x = 0, total = 0, avail;

    if (!h->tabs) return;
    tabs_centre(h);
    avail = tabs_view_w(h);

    dc  = GetDC(h->tabs);
    old = (HFONT)SelectObject(dc, h->menufont);

    for (i = 0; i < h->app.ndocs; i++) {
        RECT tr;
        int w;
        tr.left = tr.top = tr.right = tr.bottom = 0;
        DrawTextW(dc, h->d[i].title, -1, &tr, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        w = px(TAB_PAD) + tr.right + px(TAB_GAP) + px(TAB_CLOSE_W) + px(TAB_PAD) +
            px(TAB_SLACK);
        if (w < px(TAB_MIN_W)) w = px(TAB_MIN_W);
        if (w > px(TAB_MAX_W)) w = px(TAB_MAX_W);
        h->d[i].w = w;
        total += w;
    }

    SelectObject(dc, old);
    ReleaseDC(h->tabs, dc);

    /* Too many tabs for the width: shrink them, but only to the point where a
     * tab still reads as one — about ten characters and the close box.  Past
     * that the strip scrolls instead, which is the only thing that does not
     * degrade further as tabs are added. */
    if (total > avail && h->app.ndocs > 0 && avail > 0) {
        int minw = px(TAB_MIN_W);
        for (i = 0; i < h->app.ndocs; i++) {
            int w = MulDiv(h->d[i].w, avail, total);
            h->d[i].w = w < minw ? minw : w;
        }
    }

    for (i = 0; i < h->app.ndocs; i++) {
        h->d[i].x = x;
        x += h->d[i].w;
    }

    tabs_scroll_clamp(h);
}

/* Is this tab's close box drawn — and therefore clickable?  It is the first
 * thing to go as tabs get narrow; middle click still closes either way. */
static int tab_close_shown(note_host *h, int i)
{
    int w = h->d[i].w;
    if (w < px(TAB_CLOSE_OFF)) return 0;
    if (w < px(TAB_CLOSE_DIM))
        return i == h->app.active || i == h->tab_hot;
    return 1;
}

/* The close box of tab i, on screen.  The drawn box and the hit target are
 * the same rectangle by construction, and both sit on the strip's centre. */
static void tab_close_box(note_host *h, int i, RECT *box)
{
    int bw = px(TAB_CLOSE_W);

    box->right  = tabs_screen_x(h, i) + h->d[i].w - px(TAB_PAD);
    box->left   = box->right - bw;
    box->top    = h->tab_mid - bw / 2;
    box->bottom = box->top + bw;
}

/* Which tab is at this point, and is the pointer on its close box? */
int tabs_hit(note_host *h, int x, int y, int *on_close)
{
    int i;
    RECT rc;

    if (on_close) *on_close = 0;
    if (caption_plain(h)) return -1;          /* a title bar, not a strip */
    GetClientRect(h->tabs, &rc);
    if (y < 0 || y > rc.bottom) return -1;
    if (x < tabs_left(h)) return -1;          /* the corner, not a tab */

    for (i = 0; i < h->app.ndocs; i++) {
        int sx = tabs_screen_x(h, i);
        if (x >= sx && x < sx + h->d[i].w) {
            if (on_close && tab_close_shown(h, i)) {
                RECT box;
                tab_close_box(h, i, &box);
                *on_close = (x >= box.left && x < box.right);
            }
            return i;
        }
    }
    return -1;
}

static void draw_close_box(HDC dc, const RECT *box, COLORREF colour, int hot,
                           HBRUSH hot_brush)
{
    int cx = (box->left + box->right) / 2;
    int cy = (box->top + box->bottom) / 2;
    int r  = px(TAB_CLOSE_R);
    int wd = px(1) < 1 ? 1 : px(1);
    HPEN pen, old;

    if (hot) FillRect(dc, box, hot_brush);

    pen = CreatePen(PS_SOLID, wd, colour);
    old = (HPEN)SelectObject(dc, pen);
    /* LineTo stops one short of its endpoint, so each stroke runs from -r to
     * +r about the centre and the cross is symmetric. */
    MoveToEx(dc, cx - r, cy - r, NULL);
    LineTo  (dc, cx + r + 1, cy + r + 1);
    MoveToEx(dc, cx + r, cy - r, NULL);
    LineTo  (dc, cx - r - 1, cy + r + 1);
    SelectObject(dc, old);
    DeleteObject(pen);
}

/* There are tabs past this edge.  Chrome and VS Code say so by letting the
 * strip dissolve into the caption, and a fade beats any glyph note could draw:
 * a chevron three strokes wide is crude at 100% and asymmetric at 125%, while
 * a fade has no shape to get wrong, needs no icon font, and swallows the half
 * tab the edge cuts through instead of covering it with a band.
 *
 * The strip is already painted into a DIB section, so this is arithmetic on
 * the pixels: GDI has no alpha to blend with, but the bytes are right there.
 * `bits` is 32bpp top-down, so a row is `stride` unsigned values. */
static void fade_edge(void *bits, int stride, int height, int x0, int w,
                      int right, unsigned bg)
{
    int y, x;

    if (!bits || w <= 0 || stride <= 0) return;

    for (y = 0; y < height; y++) {
        unsigned *row = (unsigned *)bits + (unsigned)y * (unsigned)stride;
        for (x = 0; x < w; x++) {
            int      cx = x0 + x;
            int      t  = right ? x + 1 : w - x;     /* 0 at the tabs, w at the rim */
            unsigned c;
            unsigned r, g_, b;

            if (cx < 0 || cx >= stride) continue;
            c  = row[cx];
            r  = ((((c >> 16) & 0xFF) * (unsigned)(w - t)) +
                  (((bg >> 16) & 0xFF) * (unsigned)t)) / (unsigned)w;
            g_ = ((((c >>  8) & 0xFF) * (unsigned)(w - t)) +
                  (((bg >>  8) & 0xFF) * (unsigned)t)) / (unsigned)w;
            b  = (((c & 0xFF) * (unsigned)(w - t)) +
                  ((bg & 0xFF) * (unsigned)t)) / (unsigned)w;
            row[cx] = (r << 16) | (g_ << 8) | b;
        }
    }
}

static void tabs_paint(HWND wnd)
{
    PAINTSTRUCT ps;
    HDC     dc, mem;
    HBITMAP bmp, oldbmp;
    RECT    rc;
    HFONT   old;
    HBRUSH  br_hot;
    BITMAPINFO bi;
    void   *bits = 0;
    int     i, left, bw, bh;

    dc = BeginPaint(wnd, &ps);
    GetClientRect(wnd, &rc);
    tabs_centre(&g);
    /* The band note paints and the corner DWM paints have to agree; this is
     * the moment we know which colour that is. */
    caption_colours(&g);

    /* Drawn off-screen: the strip repaints on every hover and the tabs sit
     * directly against each other, so painting in place would flicker.  A DIB
     * rather than a compatible bitmap because the alpha byte has to be written
     * by hand afterwards — see below. */
    bw = rc.right  > 0 ? rc.right  : 1;
    bh = rc.bottom > 0 ? rc.bottom : 1;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = bw;
    bi.bmiHeader.biHeight      = -bh;                                /* top down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    mem    = CreateCompatibleDC(dc);
    bmp    = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    oldbmp = (HBITMAP)SelectObject(mem, bmp);

    FillRect(mem, &rc, g.br_ui);
    SetBkMode(mem, TRANSPARENT);
    old = (HFONT)SelectObject(mem, g.menufont);

    br_hot = CreateSolidBrush(blend_rgb(g.theme.ui_bg, g.theme.fg));

    left = tabs_left(&g);

    /* One document: an ordinary title bar.  The icon is whatever is on the
     * window, so a build with the resource icon and one without both put the
     * right thing here, and the name is the document's — no tab shape, no
     * close box, no separator, because none of them would be telling the user
     * anything the window itself does not. */
    if (caption_plain(&g)) {
        HICON ic = (HICON)GetClassLongPtrW(g.wnd, GCLP_HICONSM);
        RECT  lr;

        if (!ic) ic = (HICON)GetClassLongPtrW(g.wnd, GCLP_HICON);
        if (ic)
            DrawIconEx(mem, px(TAB_PAD), g.tab_mid - px(TAB_ICON) / 2, ic,
                       px(TAB_ICON), px(TAB_ICON), 0, NULL, DI_NORMAL);

        if (g.app.ndocs == 1) {
            lr.left   = left;
            lr.right  = rc.right - px(TAB_PAD);
            lr.top    = 0;
            lr.bottom = g.tab_mid * 2;
            SetTextColor(mem, cr(g.theme.ui_fg));
            if (lr.right > lr.left)
                DrawTextW(mem, g.d[0].title, -1, &lr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                          DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        goto done;
    }

    /* Nothing of a scrolled strip may spill into the corner inset. */
    IntersectClipRect(mem, left, 0, rc.right, rc.bottom);

    for (i = 0; i < g.app.ndocs; i++) {
        RECT tr, box, lr;
        int  sel = (i == g.app.active);
        int  hot = (i == g.tab_hot);
        int  sx  = tabs_screen_x(&g, i);

        if (sx + g.d[i].w <= left || sx >= rc.right) continue;   /* scrolled out */

        tr.left   = sx;
        tr.right  = sx + g.d[i].w;
        tr.top    = 0;
        tr.bottom = rc.bottom;

        FillRect(mem, &tr, sel ? g.br_edit : (hot ? br_hot : g.br_ui));

        if (sel) {
            /* A line along the top marks the active tab without a border. */
            RECT accent = tr;
            accent.bottom = accent.top + px(2);
            FillRect(mem, &accent, g.br_menusel);
        } else {
            /* A divider between two quiet tabs, and only there.  Full height
             * it read as a partition rather than a hairline, and next to the
             * active or hovered tab it fought the fill that already separates
             * them — so it is inset like the one beside the icon, and skipped
             * wherever a change of background is doing the job already. */
            int next = i + 1;
            int quiet = next < g.app.ndocs &&
                        next != g.app.active && next != g.tab_hot && !hot;
            if (quiet) {
                RECT edge = tr;
                edge.left   = edge.right - (px(1) < 1 ? 1 : px(1));
                edge.top    = px(TAB_SEP_INSET);
                edge.bottom = rc.bottom - px(TAB_SEP_INSET);
                if (edge.bottom > edge.top) FillRect(mem, &edge, g.br_sep);
            }
        }

        tab_close_box(&g, i, &box);

        /* The label and the cross share the strip's centre line: DT_VCENTER
         * against a rect symmetric about tab_mid is the same axis the box was
         * built on, and the icon above sits on it too. */
        lr.left   = sx + px(TAB_PAD);
        lr.right  = tab_close_shown(&g, i) ? box.left - px(TAB_GAP)
                                           : tr.right - px(TAB_PAD);
        lr.top    = 0;
        lr.bottom = g.tab_mid * 2;
        SetTextColor(mem, cr(sel ? g.theme.fg : g.theme.ui_fg));
        DrawTextW(mem, g.d[i].title, -1, &lr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        if (tab_close_shown(&g, i))
            draw_close_box(mem, &box, cr(sel ? g.theme.fg : g.theme.ui_fg),
                           hot && g.tab_hot_close, g.br_menusel);
    }

    /* Say which way the rest of the tabs are.  The blend reads back what GDI
     * has drawn, so the batch has to have reached the bitmap first. */
    if (g.tab_scroll > 0 || tabs_total_w(&g) - g.tab_scroll > tabs_view_w(&g)) {
        int fw = px(TAB_FADE);
        GdiFlush();
        if (g.tab_scroll > 0)
            fade_edge(bits, bw, bh, left, fw, 0, g.theme.ui_bg);
        if (tabs_total_w(&g) - g.tab_scroll > tabs_view_w(&g))
            fade_edge(bits, bw, bh, rc.right - fw, fw, 1, g.theme.ui_bg);
    }

done:
    SelectClipRgn(mem, NULL);
    DeleteObject(br_hot);
    SelectObject(mem, old);

    /* GDI leaves the alpha byte at zero, and inside the extended frame zero
     * alpha means "this pixel is the glass".  Without this the whole strip
     * would disappear into the caption; the only part that is meant to stay
     * transparent is the space kept clear on the right, which belongs to the
     * frame and not to this window. */
    if (bits && g.title_tabs) {
        unsigned *p = (unsigned *)bits;
        int n = bw * bh;
        GdiFlush();
        while (n--) *p++ |= 0xFF000000u;
    }

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteDC(mem);

    EndPaint(wnd, &ps);
}

LRESULT CALLBACK TabsProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    /* Everything in the band that is not note's own is the title bar.  A child
     * window is hit-tested before its parent, so without this the strip would
     * swallow the presses that Windows turns into dragging the window, the
     * system menu on a right click and maximise on a double click.  Saying the
     * pixel is not ours lets the frame answer for it, and the frame's own
     * WM_NCHITTEST calls it HTCAPTION. */
    case WM_NCHITTEST: {
        POINT pt;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        ScreenToClient(wnd, &pt);
        /* With one document there is nothing in the band that is ours: the
         * icon is the frame's system menu and the rest is caption, so drag,
         * double-click and right-click all reach the window manager. */
        if (tabs_hit(&g, pt.x, pt.y, NULL) >= 0) return HTCLIENT;
        return HTTRANSPARENT;
    }

    case WM_ERASEBKGND:
        return 1;                          /* WM_PAINT covers every pixel */

    case WM_PAINT:
        tabs_paint(wnd);
        return 0;

    case WM_SIZE:
        tabs_layout(&g);
        InvalidateRect(wnd, NULL, FALSE);
        return 0;

    case WM_MOUSEMOVE: {
        int on_close = 0;
        int hit = tabs_hit(&g, (short)LOWORD(lp), (short)HIWORD(lp), &on_close);

        if (!g.tab_tracking) {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = wnd;
            tme.dwHoverTime = 0;
            if (TrackMouseEvent(&tme)) g.tab_tracking = 1;
        }
        if (hit != g.tab_hot || on_close != g.tab_hot_close) {
            g.tab_hot = hit;
            g.tab_hot_close = on_close;
            InvalidateRect(wnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g.tab_tracking = 0;
        if (g.tab_hot != -1) {
            g.tab_hot = -1;
            g.tab_hot_close = 0;
            InvalidateRect(wnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        int on_close = 0;
        int hit = tabs_hit(&g, (short)LOWORD(lp), (short)HIWORD(lp), &on_close);
        if (hit < 0) return 0;

        /* Closing a tab that is not the active one still goes through the
         * core's close path, which is written in terms of the active tab. */
        note_select_doc(&g.app, hit);
        if (on_close) note_command(&g.app, CMD_FILE_CLOSE);
        return 0;
    }

    case WM_MBUTTONDOWN: {
        int hit = tabs_hit(&g, (short)LOWORD(lp), (short)HIWORD(lp), NULL);
        if (hit >= 0) {
            note_select_doc(&g.app, hit);
            note_command(&g.app, CMD_FILE_CLOSE);
        }
        return 0;
    }

    /* No WM_LBUTTONDBLCLK: a double click on the empty band is the title
     * bar's, and it belongs to the frame, which maximises and restores.  The
     * hit test above is what gets it there. */

    /* The wheel over the strip scrolls it, which is the only way to reach a
     * tab that the width could not shrink far enough to show. */
    case WM_MOUSEWHEEL: {
        int max = tabs_total_w(&g) - tabs_view_w(&g);
        if (max <= 0) return 0;
        g.tab_scroll -= GET_WHEEL_DELTA_WPARAM(wp) * px(60) / WHEEL_DELTA;
        if (g.tab_scroll > max) g.tab_scroll = max;
        if (g.tab_scroll < 0)   g.tab_scroll = 0;
        InvalidateRect(wnd, NULL, FALSE);
        return 0;
    }
    }

    return DefWindowProcW(wnd, msg, wp, lp);
}

/* Walks the same shape build_menus() does: the table is a run of popups, each
 * ending in MI_END, and the run itself ends with a non-popup entry.  The size
 * is never taken with sizeof — note_menu is an extern array of unknown bound
 * here, so only the sentinels can say where it stops. */
void sync_menu(note_host *h)
{
    const note_menu_item *it = note_menu;

    while (it->kind == MI_POPUP) {
        for (it++; it->kind != MI_END; it++)
            if (it->kind == MI_CHECK || it->kind == MI_RADIO)
                CheckMenuItem(h->menubar, it->id,
                              MF_BYCOMMAND | (note_menu_check(&h->app, it->id)
                                              ? MF_CHECKED : MF_UNCHECKED));
        it++;                       /* step past the popup's MI_END */
    }
}

void build_accels(note_host *h)
{
    ACCEL a[64];
    int i;
    for (i = 0; i < note_accel_count && i < 64; i++) {
        BYTE f = FVIRTKEY;
        if (note_accels[i].mods & ACC_CTRL)  f |= FCONTROL;
        if (note_accels[i].mods & ACC_SHIFT) f |= FSHIFT;
        if (note_accels[i].mods & ACC_ALT)   f |= FALT;
        a[i].fVirt = f;
        a[i].key   = note_accels[i].key;
        a[i].cmd   = note_accels[i].id;
    }
    h->accel = CreateAcceleratorTableW(a, i);
}
