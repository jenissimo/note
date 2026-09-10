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
            os_append_menu(m, MF_OWNERDRAW, 0, NULL);
        else
            os_append_menu(m, MF_OWNERDRAW, it->id,
                           mid_new(it->label, 0, checkcol));
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
    if (caps.set_menu_info) caps.set_menu_info(m, &mi);
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
        os_append_menu(h->menubar, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)sub,
                       mid_new(name, 1, 0));
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
/* Every colour this backend computes rather than reads from the theme comes
 * through here, so this is where it is snapped to something the display can
 * actually show.  On a sixteen-colour Windows a brush made from a colour
 * that is not in the palette is not the nearest colour -- it is a dithered
 * checkerboard of the two either side, and the wash under the caret line
 * comes out visibly hatched.
 *
 * Reducing the theme is not enough on its own: a mixture of two palette
 * colours is not itself a palette colour. */
/* The mixture itself, before any display has had a say in it. */
static COLORREF mix_raw(note_color a, note_color b, int num, int den)
{
    unsigned r  = ((((a >> 16) & 0xFF) * num) + (((b >> 16) & 0xFF) * (den - num))) / den;
    unsigned gg = ((((a >>  8) & 0xFF) * num) + (((b >>  8) & 0xFF) * (den - num))) / den;
    unsigned bb = (((a & 0xFF) * num) + ((b & 0xFF) * (den - num))) / den;
    return RGB(r, gg, bb);
}

COLORREF mix_rgb(note_color a, note_color b, int num, int den)
{
    return chrome_snap(mix_raw(a, b, num, den));
}

COLORREF blend_rgb(note_color a, note_color b)
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
    os_draw_text(dc, text, -1, &rc, DT_CALCRECT | DT_SINGLELINE);
    mis->itemWidth  = (UINT)(item_text_left(d) + rc.right + px(MENU_PAD));
    mis->itemHeight = (UINT)(rc.bottom + px(8));

    if (accel[0]) {
        RECT ar;
        ar.left = ar.top = ar.right = ar.bottom = 0;
        os_draw_text(dc, accel, -1, &ar, DT_CALCRECT | DT_SINGLELINE);
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

    /* A top-level item can arrive here as well as through the UAH message --
     * Windows sends WM_DRAWITEM for the one the pointer is over -- and this
     * routine lays an item out the way a drop-down wants it: left, past the
     * check column.  On the bar that is the wrong shape, and the item visibly
     * jumped as the pointer crossed it.  One drawer for the bar, wherever the
     * request came from. */
    if (d->onbar) {
        menubar_item_draw(h, dis->hDC, &rc, d, selected);
        return;
    }

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
    os_draw_text(dis->hDC, text, -1, &tr, fmt);

    if (accel[0]) {
        tr = rc;
        tr.right -= px(MENU_PAD);
        SetTextColor(dis->hDC, blend_rgb(h->theme.ui_fg, h->theme.ui_bg));
        os_draw_text(dis->hDC, accel, -1, &tr, fmt | DT_RIGHT);
    }

    SelectObject(dis->hDC, old);
}

/* ---- the caption ---------------------------------------------------------
 *
 * The tab strip is drawn in the title bar rather than under it.  Two pieces do
 * most of that work either way: the caption band is taken back into the client
 * rect by WM_NCCALCSIZE, and WM_NCHITTEST hands the empty part of the strip
 * back to the window manager as HTCAPTION so dragging, double-click and the
 * system menu still work.  The window stays an ordinary overlapped one in both
 * frames, which is what keeps moving, sizing against the work area and — where
 * it exists — Aero Snap the window manager's business rather than ours.
 *
 * What differs is who draws what is left.  With DWM the frame is extended into
 * the client area, so the compositor goes on painting the shadow, the rounded
 * corners and the three system buttons behind note's pixels, and note keeps a
 * corner of the strip clear for them.  Without DWM — Windows 95 through XP, and
 * any later Windows with composition off — there is nobody behind the pixels:
 * note draws the buttons in that corner itself, tracks their hover and press,
 * and paints the resize border on WM_NCPAINT, because the caption that used to
 * come with it is now inside the client area.
 *
 * title_tabs says the caption is note's to draw; frame_dwm says which of the
 * two it is.
 * ------------------------------------------------------------------------- */

/* The resize border, which is also how much taller a maximised window's
 * caption is: maximised, the window rect hangs this far off every edge. */
int frame_edge(void)
{
    int b = GetSystemMetrics(SM_CXSIZEFRAME);

    /* SM_CXPADDEDBORDER is Vista's, and from Windows 8 it carries most of the
     * sizing border, so it cannot simply be left out.  It cannot simply be
     * asked for either: an index Windows 95 has never heard of does not come
     * back as zero, it comes back as one -- which is why note's frame measured
     * five pixels on the guest against a four-pixel frame on every other
     * window there, and why its caption sat a pixel lower than theirs.
     *
     * The same trap as OPENFILENAMEA and CS_DROPSHADOW, in the one form that
     * has no compiler and no dumpbin to catch it: a metric that answers.  So
     * it is asked for only where the compositor that introduced it exists --
     * dwmapi.dll is absent before Vista, and caps_probe() has already been
     * that far. */
    if (caps.dwm_set_attr) b += GetSystemMetrics(SM_CXPADDEDBORDER);
    return b > 0 ? b : px(8);
}

/* How far the client area starts below the top of the window, once note has
 * taken the caption into it.
 *
 * With a compositor the whole top of the window is the client, and that is not
 * a stylistic choice: DWM draws its three buttons inside whatever non-client
 * top area WM_NCCALCSIZE leaves, so the size of that area is the size they get.
 * Leaving the resize border up there was tried -- an ordinary window keeps
 * SM_CYSIZEFRAME + SM_CXPADDEDBORDER of frame above its caption, which is why
 * charmap measures thirty-one from its window top to its client top against a
 * caption of twenty-three -- on the theory that DWM would then start its
 * caption below the border, as it does for charmap, and the buttons would come
 * down with it.  They do not.  Measured off the screen rather than through
 * PrintWindow, which does not render the composited non-client area at all:
 * with the border kept, the whole cluster is drawn into those eight pixels and
 * clipped, and only the bottom three rows of the glyphs survive.  With the
 * client at the window top they are whole, and centred in the first
 * SM_CYCAPTION rows -- which is what caption_height() returns, so they are
 * centred in the band.
 *
 * Without a compositor the border stays where it was and only the caption comes
 * in -- otherwise the band note draws is the border and the caption together,
 * taller than the caption on every other window on that desktop.
 *
 * Maximised is the exception either way, and the one WM_NCCALCSIZE has always
 * made here: the window rect hangs a border off every edge of the monitor, so
 * the client has to come in by that much or the strip is drawn off screen. */
int frame_client_top(note_host *h)
{
    return (h->frame_dwm && !IsZoomed(h->wnd)) ? 0 : frame_edge();
}

/* The rows of the band the caption itself occupies.
 *
 * SM_CYCAPTION is nineteen on Windows 95 and the caption there is eighteen
 * pixels of colour: the last row is the border between the caption and the
 * client, and Windows paints it in the face colour rather than the caption's.
 * Every window on that desktop has it, so note draws it too -- and the buttons
 * are centred in the eighteen rather than the nineteen, which is what put ours
 * a pixel below every other window's.
 *
 * With a compositor there is no such row: the band is the caption and the
 * frame it is extended over, and DWM paints the join. */
int caption_band(void)
{
    int h = caption_height();
    int b = GetSystemMetrics(SM_CYBORDER);

    if (g.frame_dwm || b <= 0 || b >= h) return h;
    return h - b;
}

/* How far down the window DWM composites its own caption, and so how far the
 * frame is extended for it.
 *
 * The caption's height, not the band's.  The two were the same thing until the
 * band grew to what a tab wants: the frame was extended over the whole band, so
 * every row of the corner kept clear for the buttons was glass -- and DWM
 * paints its caption for SM_CYCAPTION rows and nothing at all below that, which
 * left the rows under the buttons transparent black.  A strip of night under
 * the three buttons, on a caption that is otherwise ui_bg the whole way across.
 *
 * Extending by only the caption puts those rows back in the ordinary client
 * area, where WM_ERASEBKGND's br_ui is opaque and covers them.  DWM still has
 * every row its caption and its buttons occupy. */
int caption_dwm_top(void)
{
    int band = caption_height();
    int cap  = GetSystemMetrics(SM_CYCAPTION);

    if (cap <= 0 || cap > band) cap = band;
    return cap;
}

/* The band the caption occupies: on either frame, the system caption's own
 * height and nothing else.
 *
 * Without a compositor the resize border above it is still the border, still
 * non-client, and still painted by frame_border_paint().  Eighteen pixels on
 * Windows 95, the same eighteen every other window there has.
 *
 * With one it used to be the caption plus the border DWM extends over, floored
 * at what a tab wants -- thirty-one where the caption is twenty-three.  That
 * was wrong in a way only the buttons showed: DWM places its caption buttons
 * from the real caption height and pays no attention to how far the frame was
 * extended, so they sat centred in the top twenty-three of a thirty-one pixel
 * band and read four pixels high.  Extending further does not move them; it was
 * tried, and the glyphs did not shift a pixel.  Nor is there an attribute that
 * asks DWM for a taller caption: the modern windowing stack has one --
 * AppWindowTitleBar's tall title bar -- and it belongs to WinUI 3 and the app
 * SDK, not to a plain Win32 window, and there is nothing in dwmapi that does
 * it.  So the band comes down to the buttons rather than the buttons being
 * pushed down the band, and the caption is the height every other window on
 * that desktop has. */
int caption_height(void)
{
    int c = GetSystemMetrics(SM_CYCAPTION);

    if (c <= 0) c = px(24);
    return c;
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

    /* Windows 95 and NT 4 cannot be asked -- GetMenuBarInfo is Windows 2000 --
     * so the answer is worked out from where the default frame must have put
     * the bar: directly under the caption Windows still believes it has, which
     * is the caption note took into the client area.  Maximised, the client
     * already starts a resize border down, so the border comes off again. */
    if (!caps.menu_bar_info) {
        band->left   = 0;
        band->right  = 0;
        band->top    = GetSystemMetrics(SM_CYCAPTION) + frame_edge() -
                       frame_client_top(h);
        band->bottom = band->top + GetSystemMetrics(SM_CYMENU);
        if (GetClientRect(h->wnd, &wr)) band->right = wr.right;
        return band->bottom;
    }

    memset(&mbi, 0, sizeof(mbi));
    mbi.cbSize = sizeof(mbi);
    if (!caps.menu_bar_info(h->wnd, OBJID_MENU, 0, &mbi)) return 0;
    if (!GetWindowRect(h->wnd, &wr)) return 0;
    if (mbi.rcBar.bottom <= mbi.rcBar.top) return 0;

    /* Where WM_NCCALCSIZE put the client top, which is the one thing the bar's
     * screen rectangle has to be measured against. */
    top = wr.top + (h->title_tabs ? frame_client_top(h) : 0);

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
    os_draw_text(dc, d->label, -1, &r, fmt);
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
        const menu_item_data *d;
        UINT state;
        RECT r;

        if (!GetMenuItemRect(h->wnd, h->menubar, (UINT)i, &r)) continue;
        MapWindowPoints(NULL, h->wnd, (POINT *)&r, 2);

        d = (const menu_item_data *)
            os_menu_item_data(h->menubar, (UINT)i, &state);
        if (!d) continue;

        /* Which item is lit is Windows' business and it will say: MFS_HILITE
         * is on whichever one menu mode has walked to. */
        menubar_item_draw(h, dc, &r, d,
                          (state & MFS_HILITE) != 0 || i == h->menu_hot);
    }
    return 1;
}

/* One caption button, the size the system draws its own.
 *
 * SM_CXSIZE and SM_CYSIZE are the cell, not the button: Windows 95 calls the
 * pair "Caption Buttons" and sizes them 18 by 18, and what it paints in that
 * cell measures 16 by 14 -- one 3-D edge off the width, one off the top and one
 * off the bottom.  Measured off the guest rather than assumed: a column down
 * the middle of a native minimise button is one row of highlight, eight of
 * face, two of glyph, one of face, then the shadow and the shadow outside it.
 *
 * Every one of them is a metric rather than a number, so a desktop set up with
 * larger caption buttons gets larger ones here too. */
static int caption_btn_w(void)
{
    int bw = GetSystemMetrics(SM_CXSIZE) - GetSystemMetrics(SM_CXEDGE);
    if (bw <= 0) bw = px(16);
    return bw;
}

static int caption_btn_h(void)
{
    int bh = GetSystemMetrics(SM_CYSIZE) - GetSystemMetrics(SM_CYEDGE) * 2;
    if (bh <= 0) bh = px(14);
    return bh;
}

/* What separates close from the other two, and close from the corner. */
static int caption_btn_gap(void)
{
    int gap = GetSystemMetrics(SM_CXEDGE);
    return gap > 0 ? gap : 2;
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
 * ones that still draw the old caption buttons, whose width is SM_CXSIZE — and
 * so are the frames where note draws the buttons itself, since then there is
 * nothing of DWM's back there to measure. */
void caption_buttons_calc(note_host *h)
{
    RECT rc, b;
    int  w = 0, ht = 0;

    h->sysbtn.left = h->sysbtn.top = h->sysbtn.right = h->sysbtn.bottom = 0;
    if (!h->title_tabs || !h->wnd) return;
    GetClientRect(h->wnd, &rc);

    if (h->frame_dwm && caps.dwm_get_attr &&
        SUCCEEDED(caps.dwm_get_attr(h->wnd, DWMWA_CAPTION_BUTTON_BOUNDS,
                                    &b, sizeof(b)))) {
        w  = b.right - b.left;
        ht = b.bottom - b.top;
    }
    if (w <= 0 || w >= rc.right) {
        /* Widened to the band only where the buttons back there are DWM's.
         * On those builds -- Vista through Windows 10 before the bounds
         * attribute existed -- the compositor's buttons are wider than
         * SM_CXSIZE and there is no way to ask how much wider, so the corner
         * kept clear has to be generous or the tabs run under them. */
        if (h->frame_dwm) {
            int band = h->tabs_h > 0 ? h->tabs_h : caption_height();
            int bw   = GetSystemMetrics(SM_CXSIZE);
            if (bw <= 0) bw = px(32);
            if (bw < band) bw = band;
            w = bw * 3;
        } else {
            /* Where note draws them itself there is nothing to guess: exactly
             * the three buttons, the gap that groups them and the one that
             * holds the last of them off the corner.  Reserving more than that
             * spread them across a wider corner than any other window on the
             * desktop has. */
            w = caption_btn_w() * 3 + caption_btn_gap() * 2;
        }
    }
    /* The caption's own rows, not the whole band: the last of them is the
     * border under the caption and the buttons sit above it, centred in what
     * is left exactly as the system centres its own. */
    if (ht <= 0 || ht > h->tabs_h) ht = h->frame_dwm ? h->tabs_h : caption_band();

    h->sysbtn.right  = rc.right;
    h->sysbtn.left   = rc.right - w > 0 ? rc.right - w : 0;
    h->sysbtn.bottom = ht;
}

int caption_buttons_w(note_host *h)
{
    if (h->sysbtn.right <= h->sysbtn.left) caption_buttons_calc(h);
    return h->sysbtn.right - h->sysbtn.left;
}

/* Where one of the three sits inside the corner caption_buttons_calc() kept
 * clear.  Measured from the right, because that is the edge the arrangement is
 * anchored to and the one thing about it nobody has ever had to look for:
 * close is in the corner.  Minimise and maximise touch each other, then a gap,
 * then close -- the grouping is Windows 95's and it is what says at a glance
 * that the button on the end is the one that ends the program.
 *
 * The same rectangles are drawn and hit-tested, so the two cannot drift. */
static void caption_btn_box(note_host *h, int which, RECT *r)
{
    int bw  = caption_btn_w();
    int bh  = caption_btn_h();
    int gap = caption_btn_gap();
    int right = h->sysbtn.right - gap;

    if (which != CAPBTN_CLOSE) {
        right -= bw + gap;                        /* past close and its gap */
        if (which == CAPBTN_MIN) right -= bw;     /* and past maximise      */
    }
    if (bh > h->sysbtn.bottom) bh = h->sysbtn.bottom;

    r->right  = right;
    r->left   = right - bw;
    r->top    = (h->sysbtn.bottom - bh) / 2;
    r->bottom = r->top + bh;
}

/* Which of the three a client point is over.  The gaps between them answer as
 * the button to their left rather than as nothing, so there is no dead pixel to
 * find between two targets.
 *
 * Answers CAPBTN_NONE for the DWM frame however close the point is: there the
 * buttons are the compositor's, and note neither draws nor tracks them. */
int caption_btn_at(note_host *h, int x, int y)
{
    RECT r;

    if (!h->title_tabs || h->frame_dwm) return CAPBTN_NONE;
    if (y < 0 || y >= h->sysbtn.bottom)  return CAPBTN_NONE;
    if (x < h->sysbtn.left || x >= h->sysbtn.right) return CAPBTN_NONE;

    caption_btn_box(h, CAPBTN_CLOSE, &r);
    if (x >= r.left) return CAPBTN_CLOSE;
    caption_btn_box(h, CAPBTN_MAX, &r);
    if (x >= r.left) return CAPBTN_MAX;
    return CAPBTN_MIN;
}

/* What is left of the non-client area once the caption is inside the client:
 * the resize border, on three sides.
 *
 * It has to be painted rather than left to DefWindowProc, and for the same
 * reason the menus are owner-drawn — the system draws it in the system's
 * colours, and a raised grey border around a dark editor reads as a window
 * belonging to some other program.  The window DC covers the whole window, so
 * the client rect is excluded and what remains is exactly the border. */
void frame_border_paint(note_host *h, HWND wnd)
{
    HDC   dc;
    RECT  wr, cl;
    POINT o;

    if (!h->br_ui) return;
    dc = GetWindowDC(wnd);
    if (!dc) return;

    GetWindowRect(wnd, &wr);
    GetClientRect(wnd, &cl);
    o.x = 0;
    o.y = 0;
    ClientToScreen(wnd, &o);
    OffsetRect(&cl, o.x - wr.left, o.y - wr.top);
    OffsetRect(&wr, -wr.left, -wr.top);

    ExcludeClipRect(dc, cl.left, cl.top, cl.right, cl.bottom);
    FillRect(dc, &wr, h->br_ui);
    /* One line of something other than the background around the outside.  A
     * border in the background colour is no border at all, and a window with
     * no edge against a desktop of about the same brightness is a window whose
     * corner cannot be found to drag. */
    if (h->br_sep) FrameRect(dc, &wr, h->br_sep);
    ReleaseDC(wnd, dc);
}

/* The seam.  DWM paints the corner behind its own buttons and note paints the
 * rest of the band, so the two have to be told the same colour — and the brush
 * the strip fills with is br_ui, built from the same ui_bg handed over here.
 * Windows 11 22000+ only; older builds refuse the attributes and keep the
 * system caption, which is what they had before any of this. */
void caption_colours(note_host *h)
{
    COLORREF cap, txt, brd;

    if (!h->title_tabs || !h->wnd || !caps.dwm_set_attr) return;
    if (h->cap_set && h->cap_rgb == h->theme.ui_bg) return;
    h->cap_rgb = h->theme.ui_bg;
    h->cap_set = 1;

    cap = cr(h->theme.ui_bg);
    txt = cr(h->theme.ui_fg);
    brd = cr(h->theme.ui_bg);
    caps.dwm_set_attr(h->wnd, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof(cap));
    caps.dwm_set_attr(h->wnd, 36 /* DWMWA_TEXT_COLOR    */, &txt, sizeof(txt));
    caps.dwm_set_attr(h->wnd, 34 /* DWMWA_BORDER_COLOR  */, &brd, sizeof(brd));
}

/* Takes the caption over, whichever way this Windows allows. */
void frame_custom(note_host *h)
{
    BOOL    comp = FALSE;
    MARGINS m;

    h->title_tabs = 0;
    if (!h->wnd) { h->frame_dwm = 0; return; }

    /* A DWM with all four calls and composition actually on.  Extended by the
     * caption rather than by the whole band: that is the region DWM composites
     * the caption — and its buttons — behind our pixels, and anything below it
     * is ours to paint opaquely.  See caption_dwm_top().
     *
     * frame_dwm is cleared only when the compositor is genuinely gone, and that
     * distinction is the whole of a bug that was here.  This function is called
     * again whenever the frame has to be reconsidered -- a change of DPI, a
     * change of the band's height -- and it used to clear the flag first and
     * set it again only if the extension succeeded.  But a *refused* extension
     * does not undo the one already in force: the margins stay exactly as they
     * were and DWM goes on compositing its caption, and its three buttons,
     * behind our pixels.  Clearing the flag there says the opposite, and note
     * believes it: relayout() stops keeping the corner clear, the strip is
     * painted across the whole width and made opaque, and caption_buttons_draw
     * puts note's own Windows 95 buttons on top of the compositor's -- at the
     * band height and in the theme the flag was last right for.  One raised
     * grey box over DWM's caption, which is exactly what was reported.
     *
     * DwmExtendFrameIntoClientArea can refuse: it is a call into another
     * process's compositor and it fails while a session is locked or switched,
     * over a remote desktop as it connects, and during a display change.  Every
     * one of those is a moment when the extension we already have is still
     * there.  So a failure now leaves the flag as it was, and only
     * DwmIsCompositionEnabled saying "off" -- which is authoritative, because
     * then there is no caption behind us at all -- turns it off.
     *
     * The same distinction has to be made about the *question* as about the
     * answer, and that is the half that was missed.  DwmIsCompositionEnabled
     * is a call into the same compositor and it fails in the same moments, and
     * this test used to read the failure as "composition is off": SUCCEEDED()
     * and comp were folded into one condition, so an HRESULT that never
     * reached comp at all cleared the flag exactly as a compositor that had
     * really gone away would.  Which brings the reported bug back by the other
     * door -- a raised grey caption button over DWM's and note's own scroll
     * bars down the side of the page, because sb_own() reads the same flag.
     *
     * The drag that reaches it is a drag between two monitors of different
     * scaling.  note is PER_MONITOR_AWARE_V2 (see enable_dpi()), so the move
     * itself delivers WM_DPICHANGED, which calls this function while the
     * pointer is still down -- and a DPI change is a display change, which is
     * one of the moments the compositor will not answer.  So it is asked, and
     * only an answer counts: a query that fails leaves the flag exactly as it
     * was, since whatever the frame was a moment ago it still is. */
    if (!caps.dwm_frame) {
        h->frame_dwm = 0;
    } else {
        HRESULT hr = caps.dwm_composition(&comp);
        /* No answer: believe the frame we already have.  comp is not read on
         * that path except to decide whether to re-extend, and re-extending a
         * frame that is already extended costs nothing. */
        if (FAILED(hr)) comp = h->frame_dwm ? TRUE : FALSE;
        if (!comp) {
            if (SUCCEEDED(hr)) h->frame_dwm = 0;   /* authoritative: gone */
        } else {
            m.cxLeftWidth = 0;
            m.cxRightWidth = 0;
            m.cyBottomHeight = 0;
            m.cyTopHeight = caption_dwm_top();
            if (SUCCEEDED(caps.dwm_extend(h->wnd, &m))) {
                h->frame_dwm = 1;
                h->cap_set   = 0;   /* a new frame has the system's colours */
            }
        }
    }

    /* Either way the caption is note's now.  Without DWM nothing was asked of
     * Windows to make that so and nothing can refuse: WM_NCCALCSIZE decides how
     * much non-client area there is, and it is answered by this backend. */
    h->title_tabs = 1;
    h->cap_hot    = CAPBTN_NONE;
    h->cap_press  = CAPBTN_NONE;
    if (h->frame_dwm) caption_colours(h);
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
#define TAB_DRAG_W    48      /* caption the tabs may never grow into        */

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

/* And where it stops.  With DWM the strip window itself stops short of the
 * system buttons, so this is simply its right edge; without, the strip spans
 * the whole band and draws the buttons at that edge, so the tabs have to be
 * kept out of their corner the same way they are kept out of the icon's.
 *
 * And then TAB_DRAG_W further in, because the caption is the tab strip and a
 * window has to be draggable.  Measured on a 900-pixel window with seven
 * documents open: the strip was tabs from the left edge to the buttons, every
 * pixel of the row answered HTCLIENT, and the only HTCAPTION left in the whole
 * window was the seven pixels between the close button and the resize corner.
 * A window nobody can pick up.
 *
 * The reserve is the fix Chrome and Edge both use -- the tabs shrink and then
 * scroll against a strip that is narrower than the band by a fixed amount, so
 * there is always caption there whatever is open.  The alternative, making the
 * top rows of the strip drag instead of hitting the tab under them, was not
 * taken: those rows are the top of the window, they are where a maximised
 * window's tabs are thrown at by an infinite edge, and taking them costs the
 * easiest click in the strip to buy a grip that is hard to find on purpose.
 * A reserve is somewhere to grab that stays in the same place.
 *
 * It gives way rather than squeezing the strip out of existence: a window too
 * narrow for one whole tab beside the reserve keeps the tab, since there the
 * resize border and the three buttons are grip enough. */
int tabs_right(note_host *h)
{
    RECT rc;
    int  right, keep, room;

    if (!h->tabs) return 0;
    GetClientRect(h->tabs, &rc);
    right = rc.right;
    if (h->title_tabs && !h->frame_dwm) right -= caption_buttons_w(h);

    /* Only where there are tabs to keep out of it.  With one document the
     * band is already an ordinary title bar the whole way across, and taking
     * the reserve there would only shorten the name it shows. */
    if (h->title_tabs && !caption_plain(h)) {
        keep = right - px(TAB_DRAG_W);
        room = tabs_left(h) + px(TAB_MIN_W);
        if (keep < room) keep = right < room ? right : room;
        right = keep;
    }
    return right > 0 ? right : 0;
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

/* The width the tabs themselves get: the band, less the system buttons at one
 * end and the window icon at the other. */
static int tabs_view_w(note_host *h)
{
    int w = tabs_right(h) - tabs_left(h);
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
        os_draw_text(dc, h->d[i].title, -1, &tr, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
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
    if (x >= tabs_right(h)) return -1;        /* the buttons, not a tab */

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

/* One rectangle of flat colour.
 *
 * ExtTextOut with ETO_OPAQUE and no characters, rather than FillRect: a bevel
 * is eight lines in four colours and a glyph is a dozen more, and every one of
 * them through FillRect would mean creating, selecting and destroying a brush.
 * The background colour is also the one thing GDI never dithers -- a brush made
 * from a colour the display does not have comes back as a checkerboard of the
 * two either side, which is exactly what a one-pixel bevel line cannot be. */
void fill_px(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r;

    if (w <= 0 || h <= 0) return;
    r.left   = x;
    r.top    = y;
    r.right  = x + w;
    r.bottom = y + h;
    SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, L"", 0, NULL);
}

/* How light a colour is, on the same weighting chrome_snap() matches with. */
static int luma(note_color c)
{
    return (int)((((c >> 16) & 0xFF) * 3 + ((c >> 8) & 0xFF) * 6 +
                  (c & 0xFF)) / 10);
}

/* A COLORREF back into the theme's own byte order, so a colour that has been
 * through mix_rgb() can be mixed from again. */
static note_color as_note_color(COLORREF c)
{
    return ((note_color)GetRValue(c) << 16) |
           ((note_color)GetGValue(c) <<  8) |
            (note_color)GetBValue(c);
}

/* A colour `num`/`den` of the way from `from` towards `to`, pushed further
 * along that line until the display stops confusing it with `apart`.
 *
 * Naming a distance is not enough on a display that has few colours.
 * theme_fit_display() rewrites the theme onto the twenty entries a Windows 95
 * desktop reserves at 8bpp, chrome_snap() does the same to everything mixed
 * from it afterwards, and those twenty have nothing at all between black and
 * #808080 -- so on a dark theme a sixth of the way from ui_bg towards ui_fg
 * snaps straight back onto ui_bg.  A button whose face is the colour behind it
 * is not a quiet button, it is a wireframe outline of one, and that is what the
 * scroll bars and the caption buttons came out as on the guest's Dark theme.
 *
 * So the colour is asked for rather than named: keep walking out along the line
 * the theme itself defines -- background towards text, face towards white,
 * face towards black -- and stop at the first step that lands on a different
 * entry.  On a true-colour display g_npal is zero, chrome_snap() is the
 * identity, the first step is always different and this is exactly the mixture
 * it always was.
 *
 * win32_palette.c's pal_legible() is the same technique for *text*, where black
 * or white is always available to fall back on.  A surface has no such
 * fallback -- black is not an acceptable button face for a light theme -- so
 * this one steps along the theme's axis rather than jumping to an extreme. */
COLORREF chrome_apart(note_color from, note_color to, int num, int den,
                      COLORREF apart)
{
    COLORREF c = mix_raw(to, from, num, den);
    int      i;

    /* Nothing to correct where nothing was snapped.  Above eight bits per
     * pixel the mixture is the answer, exactly as it was before any of this
     * existed: chrome_snap() is the identity there, so the walk could only
     * ever move a colour the theme's author had already got right. */
    if (!chrome_palettised()) return c;

    c = chrome_snap_grey(c);
    for (i = num + 1; i <= den && c == apart; i++)
        c = chrome_snap_grey(mix_raw(to, from, i, den));
    return c;
}

/* The face every raised control in this chrome is made of.
 *
 * Not the caption colour: on Windows 95 a caption button is the grey of a
 * dialog control sitting on a blue caption, and a button painted the colour of
 * the band it is in is a bevel around nothing.  A sixth of the way from the
 * chrome towards its text is the same distance br_sep keeps between a tab and
 * its neighbour, and on the classic scheme -- white caption, black text -- it
 * lands on the grey that snaps straight back to #C0C0C0.  Where a small
 * palette would have collapsed that sixth back onto the background, it walks
 * on until it does not. */
note_color chrome_face(void)
{
    return as_note_color(chrome_apart(g.theme.ui_bg, g.theme.ui_fg, 1, 6,
                                      chrome_snap(cr(g.theme.ui_bg))));
}

/* What a glyph on that face is drawn in.
 *
 * Chosen against the face the control actually ended up with, not against the
 * colour the theme was written in: theme_fit_display() and chrome_snap()
 * collapse colours together on a display with few of them, and a close box
 * whose cross is the colour of its own face is not a close box.  Where they
 * have collided, the far end of the range the face sits at is the one colour
 * guaranteed still to be distinct from it. */
COLORREF chrome_ink(void)
{
    note_color facec = chrome_face();
    COLORREF   face  = chrome_snap(cr(facec));
    COLORREF   ink   = chrome_snap(cr(g.theme.ui_fg));

    if (ink != face) return ink;
    return chrome_snap(luma(facec) > 127 ? RGB(0, 0, 0) : RGB(255, 255, 255));
}

/* The face and the bevel of one raised control -- a caption button, a scroll
 * bar arrow, a scroll bar thumb.
 *
 * The tones are the ones Windows 95 uses, expressed as distances from the face
 * rather than as the greys they happen to be there: two thirds of the way to
 * white for the highlight, a third of the way to black for the shadow, five
 * sixths for the one outside it.  With a #C0C0C0 face those come out as
 * #F1F1F1, #808080 and #232323, which on the palette of an 8-bit Windows 95
 * desktop snap to exactly the #FFFFFF, #808080 and #000000 the system draws;
 * with a dark theme they stay a bevel rather than becoming a white box on
 * black, which is what mixing to the extremes outright would give.
 *
 * Each of them through chrome_apart(), so a shadow that a small palette would
 * have put on the same entry as the face steps out until it is a shadow again:
 * a bevel that has merged into its own face has taken the button with it.
 *
 * The bevel is not symmetrical, and neither is the system's: one highlight line
 * above and to the left, two shadow lines below and to the right.  A scan down
 * the middle of a native minimise button is one row of highlight, face, the
 * glyph, face, shadow, outer shadow -- and that is the shape this reproduces,
 * inverted when the control is pushed. */
void chrome_button_paint(HDC dc, const RECT *box, int pushed)
{
    note_color facec  = chrome_face();
    COLORREF   face   = chrome_snap(cr(facec));
    COLORREF   lite   = chrome_apart(facec, 0xFFFFFFul, 2, 3, face);
    COLORREF   dark   = chrome_apart(facec, 0x000000ul, 1, 3, face);
    COLORREF   darker = chrome_apart(facec, 0x000000ul, 5, 6, dark);
    COLORREF   tl_out = pushed ? darker : lite;
    COLORREF   tl_in  = pushed ? dark   : face;
    COLORREF   br_out = pushed ? lite   : darker;
    COLORREF   br_in  = pushed ? face   : dark;
    int x = box->left, y = box->top;
    int w = box->right - box->left, h = box->bottom - box->top;

    if (w < 4 || h < 4) {
        fill_px(dc, x, y, w, h, face);
        return;
    }

    fill_px(dc, x, y, w, h, face);

    /* The inner ring first, so the outer one wins where they meet. */
    fill_px(dc, x + 1,     y + 1,     w - 2, 1,     tl_in);
    fill_px(dc, x + 1,     y + 1,     1,     h - 2, tl_in);
    fill_px(dc, x + 1,     y + h - 2, w - 2, 1,     br_in);
    fill_px(dc, x + w - 2, y + 1,     1,     h - 2, br_in);

    fill_px(dc, x,         y,         w, 1, tl_out);
    fill_px(dc, x,         y,         1, h, tl_out);
    fill_px(dc, x,         y + h - 1, w, 1, br_out);
    fill_px(dc, x + w - 1, y,         1, h, br_out);
}

enum { GLYPH_MIN, GLYPH_MAX, GLYPH_RESTORE, GLYPH_CLOSE };

/* One caption button: the Windows 95 button, in note's colours.
 *
 * Two requirements pull opposite ways here and this is where they meet.  The
 * shape is the system's -- the same box, the same double bevel, the same
 * glyphs, the same pushed state where the bevel inverts and the glyph steps a
 * pixel down and right -- because a caption that is 1:1 with the desktop's is
 * the whole point of the classic frame.  The colours are not, because
 * DrawFrameControl paints from GetSysColor, and a light grey button on a
 * caption note painted dark is the one part of the window that belongs to
 * another program.
 *
 * The face, the bevel and the glyph colour are chrome_button_paint()'s and
 * chrome_ink()'s, so a caption button and a scroll bar arrow are made of the
 * same material.  What is left here is the three glyphs. */
static void caption_button_paint(HDC dc, const RECT *box, int glyph, int pushed)
{
    COLORREF face = chrome_snap(cr(chrome_face()));
    COLORREF ink  = chrome_ink();
    int x = box->left, y = box->top;
    int w = box->right - box->left, h = box->bottom - box->top;
    int mw, mh, mx, my, s, k;

    if (w < 6 || h < 6) return;

    chrome_button_paint(dc, box, pushed);

    /* The glyph box: nine wide and as tall as fits clear of the bevel, which
     * inside the sixteen by thirteen button of a Windows 95 caption is nine by
     * eight.  All three glyphs are built against it, so the minimise bar lands
     * on the same two rows the system's does and the three line up with each
     * other however large the desktop's caption buttons are. */
    mw = w - 7;
    if (mw < 5)     mw = 5;
    if (mw > w - 4) mw = w - 4;
    mh = mw;
    if (mh > h - 5) mh = h - 5;
    if (mh < 5)     mh = 5;
    mx = x + (w - mw) / 2;
    my = y + (h - mh) / 2;
    if (pushed) { mx++; my++; }

    switch (glyph) {
    case GLYPH_MIN:
        /* A bar along the bottom of the glyph box, two pixels thick: what a
         * window looks like once it is nothing but its caption. */
        fill_px(dc, mx, my + mh - 2, mw - 2, 2, ink);
        break;

    case GLYPH_MAX:
        /* An outline with a thick top edge -- the caption of the window it
         * would grow into. */
        fill_px(dc, mx,          my,          mw, 2,  ink);
        fill_px(dc, mx,          my,          1,  mh, ink);
        fill_px(dc, mx + mw - 1, my,          1,  mh, ink);
        fill_px(dc, mx,          my + mh - 1, mw, 1,  ink);
        break;

    case GLYPH_RESTORE:
        /* Two of those, one behind the other.  The front one is cleared back
         * to the face first, so the rear one is drawn *behind* it rather than
         * showing through. */
        s = mw - 2;
        fill_px(dc, mx + 2,      my,         s, 2, ink);
        fill_px(dc, mx + 2,      my,         1, s, ink);
        fill_px(dc, mx + mw - 1, my,         1, s, ink);
        fill_px(dc, mx + 2,      my + s - 1, s, 1, ink);

        fill_px(dc, mx,         my + 2,         s, s, face);
        fill_px(dc, mx,         my + 2,         s, 2, ink);
        fill_px(dc, mx,         my + 2,         1, s, ink);
        fill_px(dc, mx + s - 1, my + 2,         1, s, ink);
        fill_px(dc, mx,         my + 2 + s - 1, s, 1, ink);
        break;

    default: {
        /* The cross, two pixels thick, drawn a row at a time: a pen would give
         * a one-pixel diagonal and no way to thicken it that GDI draws the
         * same way twice. */
        int n  = mw - 2;
        int cx = x + (w - (n + 1)) / 2;
        int cy = y + (h - n) / 2;
        if (pushed) { cx++; cy++; }
        for (k = 0; k < n; k++) {
            fill_px(dc, cx + k,         cy + k, 2, 1, ink);
            fill_px(dc, cx + n - 1 - k, cy + k, 2, 1, ink);
        }
        break;
    }
    }
}

/* Minimise, maximise/restore and close, where nothing else will draw them.
 *
 * This only ever runs where there is no compositor -- Windows 95 through XP
 * with the classic frame, and any later Windows with composition off.  Where
 * DWM is running the three buttons stay the compositor's: it paints them
 * behind our pixels in the corner the strip keeps clear, DwmDefWindowProc
 * lights them, and HTMAXBUTTON is what the Snap Layouts flyout hangs off.
 * Drawing over them would cost all of that and gain a button that no longer
 * matched the Windows it was running on.
 *
 * There is no hover state, and that is not an omission.  A caption button
 * before Windows XP had two appearances, normal and pushed; lighting one under
 * the pointer is a later idea and would give the game away immediately.
 *
 * Both the drawing and the hit test read caption_btn_box(), so what is pressed
 * is always what was pointed at. */
static void caption_buttons_draw(HDC dc)
{
    static const int kWhich[3] = { CAPBTN_MIN, CAPBTN_MAX, CAPBTN_CLOSE };
    static const int kGlyph[3] = { GLYPH_MIN,  GLYPH_MAX,  GLYPH_CLOSE  };
    int i;

    if (!g.title_tabs || g.frame_dwm) return;
    if (g.sysbtn.right <= g.sysbtn.left) return;

    for (i = 0; i < 3; i++) {
        int  which = kWhich[i];
        int  gl    = kGlyph[i];
        RECT r;

        /* Maximise and restore are one button showing which way it will go. */
        if (which == CAPBTN_MAX && IsZoomed(g.wnd)) gl = GLYPH_RESTORE;

        caption_btn_box(&g, which, &r);
        caption_button_paint(dc, &r, gl,
                             g.cap_press == which && g.cap_hot == which);
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
        HICON ic = os_class_icon(g.wnd, GCLP_HICONSM);
        RECT  lr;

        if (!ic) ic = os_class_icon(g.wnd, GCLP_HICON);
        if (ic)
            DrawIconEx(mem, px(TAB_PAD), g.tab_mid - px(TAB_ICON) / 2, ic,
                       px(TAB_ICON), px(TAB_ICON), 0, NULL, DI_NORMAL);

        if (g.app.ndocs == 1) {
            lr.left   = left;
            lr.right  = tabs_right(&g) - px(TAB_PAD);
            lr.top    = 0;
            lr.bottom = g.tab_mid * 2;
            SetTextColor(mem, cr(g.theme.ui_fg));
            if (lr.right > lr.left)
                os_draw_text(mem, g.d[0].title, -1, &lr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                          DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        goto done;
    }

    /* Nothing of a scrolled strip may spill into either corner inset. */
    IntersectClipRect(mem, left, 0, tabs_right(&g), rc.bottom);

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
        os_draw_text(mem, g.d[i].title, -1, &lr,
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
            fade_edge(bits, bw, bh, tabs_right(&g) - fw, fw, 1, g.theme.ui_bg);
    }

done:
    SelectClipRgn(mem, NULL);
    /* Last, so a scrolled tab that reached the corner is already clipped away
     * from under them. */
    caption_buttons_draw(mem);

    /* The border between the caption and the client, on the frame that has
     * one.  Windows 95 counts it inside SM_CYCAPTION and paints it in the face
     * colour rather than the caption's, which is why a native caption there is
     * eighteen pixels of colour and not nineteen.  Every window on that desktop
     * has this row; without it ours was a pixel taller than all of them. */
    if (!g.frame_dwm && rc.bottom > 0 && rc.bottom > caption_band())
        fill_px(mem, 0, rc.bottom - 1, rc.right, 1,
                chrome_snap(cr(chrome_face())));

    DeleteObject(br_hot);
    SelectObject(mem, old);

    /* GDI leaves the alpha byte at zero, and inside the extended frame zero
     * alpha means "this pixel is the glass".  Without this the whole strip
     * would disappear into the caption; the only part that is meant to stay
     * transparent is the space kept clear on the right, which belongs to the
     * frame and not to this window. */
    if (bits && g.frame_dwm) {
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

        /* A pointer that came off a caption button and onto a tab left the
         * frame's non-client area without the frame hearing about it: these
         * are the strip's messages, and the frame gets none of them.  Where
         * TrackMouseEvent exists it would say so eventually; this is what puts
         * the light out on the Windows where it does not. */
        if (g.cap_hot != CAPBTN_NONE && g.cap_press == CAPBTN_NONE) {
            g.cap_hot = CAPBTN_NONE;
            InvalidateRect(wnd, NULL, FALSE);
        }

        if (!g.tab_tracking) {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = wnd;
            tme.dwHoverTime = 0;
            if (caps.track_mouse && caps.track_mouse(&tme))
                g.tab_tracking = 1;
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

    return os_defproc(wnd, msg, wp, lp);
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
    h->accel = os_accel_table(a, i);
}
