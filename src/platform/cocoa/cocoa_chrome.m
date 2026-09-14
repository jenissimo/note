/* cocoa_chrome.m -- tabs, the status bar, the gutter and applying a theme
 *
 * Part of note's Cocoa backend.  Everything here is what surrounds the text:
 * the strip of tabs across the top, the line-number ruler down the side, the
 * status line along the bottom, and the one routine that pushes a palette
 * from the core's theme registry into all of them at once.
 *
 * The tab strip is drawn rather than assembled out of controls.  AppKit has
 * NSTabView, and it draws tabs for a settings window: fixed width, centred,
 * no close box, no dirty marker, no room for forty of them.  What note wants
 * is the strip every editor has, which is a row of documents -- so this is
 * the one piece of chrome that is note's own drawing on both backends, for
 * the same reason it is on Win32.
 */

#import "note_cocoa.h"

#define TABS_H    28.0
#define STATUS_H  22.0
#define TAB_MAX  200.0
#define TAB_MIN   70.0
#define CLOSE_W   18.0

NSColor *chrome_color(note_color c)
{
    return [NSColor colorWithSRGBRed:((c >> 16) & 0xFF) / 255.0
                               green:((c >>  8) & 0xFF) / 255.0
                                blue:( c        & 0xFF) / 255.0
                               alpha:1.0];
}

const note_theme *chrome_theme(note_host *h)
{
    int idx = h->theme_index;
    if (idx < 0 || idx >= note_theme_count()) idx = note_theme_for(h->app.dark);
    if (idx < 0 || idx >= note_theme_count()) return NULL;
    return note_theme_get(idx);
}

/* ==========================================================================
 * The gutter
 * ========================================================================== */

@implementation NoteRuler

- (CGFloat)wantedThickness
{
    NSTextView *tv = (NSTextView *)[self clientView];
    CGFloat w;
    if (!g.linenums) return 0.0;
    w = [[tv font] pointSize] * 0.62 * 5.0 + 12.0;
    return w < 34.0 ? 34.0 : w;
}

/* The number beside a line is the number of line breaks above it, so the only
 * thing that has to be counted is the text before the top of the screen --
 * once per draw, not once per line. */
- (NSUInteger)lineAtIndex:(NSUInteger)idx inString:(NSString *)s
{
    CFStringInlineBuffer buf;
    NSUInteger i, line = 1;
    CFStringInitInlineBuffer((__bridge CFStringRef)s, &buf, CFRangeMake(0, (CFIndex)idx));
    for (i = 0; i < idx; i++)
        if (CFStringGetCharacterFromInlineBuffer(&buf, (CFIndex)i) == '\n') line++;
    return line;
}

/* Why the ruler draws itself rather than letting NSRulerView do the framing.
 *
 * A ruler view turns default clipping off -- it is built to let hash marks
 * spill past its own edge -- and the rect it is handed to draw is the scroll
 * view's, not its own.  NSRulerView's -drawRect: takes that at its word and
 * runs its divider line the full height of it, so the line carried on up out
 * of the gutter and across the tab strip above the editor.  Clipping to our
 * own bounds and drawing only what the gutter is made of keeps it where it
 * belongs; the divider is not missed, because the gutter has a background
 * colour of its own and the Win32 gutter draws no line either.
 */
- (BOOL)wantsDefaultClipping { return YES; }

- (void)drawRect:(NSRect)rect
{
    [NSGraphicsContext saveGraphicsState];
    NSRectClip([self bounds]);
    [self drawHashMarksAndLabelsInRect:NSIntersectionRect(rect, [self bounds])];
    [NSGraphicsContext restoreGraphicsState];
}

- (void)drawHashMarksAndLabelsInRect:(NSRect)rect
{
    NSTextView *tv = (NSTextView *)[self clientView];
    NSLayoutManager *lm = [tv layoutManager];
    NSTextContainer *tc = [tv textContainer];
    NSString *s = [tv string];
    const note_theme *th = chrome_theme(&g);
    NSRange glyphs, chars;
    NSUInteger line, i;
    NSDictionary *attrs;
    NSFont *font = [tv font];
    CGFloat inset = [tv textContainerOrigin].y;

    if (!th) return;

    /* Still bounds rather than the rect handed in: -drawRect: above has
     * already narrowed it, and the whole gutter is repainted in one go
     * anyway, so there is nothing to gain by filling a slice of it. */
    (void)rect;
    [chrome_color(th->gutter_bg) set];
    NSRectFill([self bounds]);

    attrs = @{ NSFontAttributeName: font,
               NSForegroundColorAttributeName: chrome_color(th->gutter_fg) };

    glyphs = [lm glyphRangeForBoundingRect:[tv visibleRect] inTextContainer:tc];
    chars  = [lm characterRangeForGlyphRange:glyphs actualGlyphRange:NULL];
    line   = [self lineAtIndex:chars.location inString:s];

    i = chars.location;
    while (i <= NSMaxRange(chars) && i <= [s length]) {
        NSRange para = [s lineRangeForRange:NSMakeRange(i, 0)];
        NSRange gl   = [lm glyphRangeForCharacterRange:NSMakeRange(para.location, 0)
                                  actualCharacterRange:NULL];
        NSRect  frag = [lm lineFragmentRectForGlyphAtIndex:gl.location
                                            effectiveRange:NULL];
        NSPoint p = [self convertPoint:NSMakePoint(0, NSMinY(frag) + inset) fromView:tv];
        NSString *num = [NSString stringWithFormat:@"%lu", (unsigned long)line];
        NSSize sz = [num sizeWithAttributes:attrs];

        [num drawAtPoint:NSMakePoint([self ruleThickness] - sz.width - 6.0, p.y)
          withAttributes:attrs];

        if (NSMaxRange(para) <= para.location) break;
        i = NSMaxRange(para);
        line++;
        if (i >= [s length]) break;
    }
}
@end

/* ==========================================================================
 * The tab strip
 * ========================================================================== */

static CGFloat tab_width(note_host *h, NSRect bounds)
{
    CGFloat w;
    if (h->app.ndocs <= 0) return TAB_MAX;
    w = NSWidth(bounds) / (CGFloat)h->app.ndocs;
    if (w > TAB_MAX) w = TAB_MAX;
    if (w < TAB_MIN) w = TAB_MIN;
    return w;
}

@implementation NoteTabs

- (BOOL)isFlipped { return YES; }

- (void)drawRect:(NSRect)dirty
{
    const note_theme *th = chrome_theme(&g);
    CGFloat w = tab_width(&g, [self bounds]);
    int i;
    NSFont *font = [NSFont systemFontOfSize:12.0];

    (void)dirty;
    if (!th) return;

    [chrome_color(th->ui_bg) set];
    NSRectFill([self bounds]);

    for (i = 0; i < g.app.ndocs; i++) {
        NSRect r = NSMakeRect((CGFloat)i * w, 0, w - 1.0, NSHeight([self bounds]));
        int active = (i == g.app.active);
        NSString *title = (NSString *)g.d[i].title ?: @"Untitled";
        NSDictionary *attrs = @{
            NSFontAttributeName: font,
            NSForegroundColorAttributeName:
                chrome_color(active ? th->fg : th->ui_fg) };
        NSRect text = NSMakeRect(NSMinX(r) + 10.0, 5.0,
                                 NSWidth(r) - 10.0 - CLOSE_W, 16.0);

        [chrome_color(active ? th->bg : th->ui_bg) set];
        NSRectFill(r);

        if (active) {
            [chrome_color(th->tok[TOK_KEYWORD]) set];
            NSRectFill(NSMakeRect(NSMinX(r), NSHeight(r) - 2.0, NSWidth(r), 2.0));
        }

        /* drawInRect: rather than drawAtPoint:, so a long name is clipped to
         * its tab instead of running across the one beside it. */
        [title drawInRect:text withAttributes:attrs];

        /* The close box is only drawn on the tab under the pointer on other
         * editors; here it is always there, because a strip that changes
         * shape under the mouse is harder to hit than one that does not. */
        [chrome_color(th->ui_fg) set];
        {
            NSBezierPath *x = [NSBezierPath bezierPath];
            CGFloat cx = NSMaxX(r) - CLOSE_W / 2.0 - 4.0, cy = NSHeight(r) / 2.0, d = 3.5;
            [x moveToPoint:NSMakePoint(cx - d, cy - d)];
            [x lineToPoint:NSMakePoint(cx + d, cy + d)];
            [x moveToPoint:NSMakePoint(cx + d, cy - d)];
            [x lineToPoint:NSMakePoint(cx - d, cy + d)];
            [x setLineWidth:1.2];
            [x stroke];
        }
    }
}

- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    CGFloat w = tab_width(&g, [self bounds]);
    int hit = (int)(p.x / w);

    if (hit < 0 || hit >= g.app.ndocs) return;

    note_select_doc(&g.app, hit);
    if (p.x > (CGFloat)(hit + 1) * w - CLOSE_W - 8.0)
        note_command(&g.app, CMD_FILE_CLOSE);
    menu_sync(&g);
    [self setNeedsDisplay:YES];
}
@end

/* ==========================================================================
 * The status bar
 * ========================================================================== */

@implementation NoteStatus

- (BOOL)isFlipped { return YES; }

- (void)drawRect:(NSRect)dirty
{
    const note_theme *th = chrome_theme(&g);
    (void)dirty;
    if (!th) return;

    [chrome_color(th->ui_bg) set];
    NSRectFill([self bounds]);

    [(self.text ?: @"") drawAtPoint:NSMakePoint(10.0, 3.0)
                     withAttributes:@{
        NSFontAttributeName: [NSFont systemFontOfSize:11.0],
        NSForegroundColorAttributeName: chrome_color(th->ui_fg) }];
}
@end

/* ==========================================================================
 * Layout
 * ========================================================================== */

void chrome_layout(note_host *h)
{
    NSView *content = (NSView *)h->content;
    NSRect b = [content bounds];
    CGFloat status_h = h->status_visible ? STATUS_H : 0.0;
    CGFloat tabs_h = TABS_H;

    [(NSView *)h->tabs setFrame:NSMakeRect(0, NSMaxY(b) - tabs_h, NSWidth(b), tabs_h)];
    [(NSView *)h->status setFrame:NSMakeRect(0, 0, NSWidth(b), status_h)];
    [(NSView *)h->status setHidden:!h->status_visible];
    [(NSView *)h->stack setFrame:NSMakeRect(0, status_h, NSWidth(b),
                                            NSHeight(b) - tabs_h - status_h)];
    {
        int i;
        for (i = 0; i < NOTE_MAX_DOCS; i++)
            if (h->d[i].scroll)
                [(NSView *)h->d[i].scroll setFrame:[(NSView *)h->stack bounds]];
    }
    if (h->overlay) [(NotePalette *)h->overlay setNeedsDisplay:YES];
}

void chrome_repaint(note_host *h)
{
    [(NSView *)h->tabs setNeedsDisplay:YES];
    [(NSView *)h->status setNeedsDisplay:YES];
}

/* ==========================================================================
 * note_host_ops: tabs
 * ========================================================================== */

int h_tab_create(note_host *h, int doc)
{
    NSScrollView *scroll;
    NoteTextView *tv;
    NoteRuler *ruler;
    NSRect frame;

    if (doc < 0 || doc >= NOTE_MAX_DOCS || h->d[doc].edit) return 1;

    frame = [(NSView *)h->stack bounds];
    scroll = [[NSScrollView alloc] initWithFrame:frame];
    [scroll setHasVerticalScroller:YES];
    [scroll setHasHorizontalScroller:!h->app.wrap];
    [scroll setAutohidesScrollers:YES];
    [scroll setBorderType:NSNoBorder];
    [scroll setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [scroll setDrawsBackground:YES];

    /* Built on an explicit TextKit 1 stack rather than by handing a frame to
     * -initWithFrame:.  A text view made the short way comes up on TextKit 2,
     * and the first thing that asks it for an NSLayoutManager -- the gutter,
     * every repaint -- drops it into compatibility mode halfway through its
     * own layout: the line fragments are there, so the ruler numbers every
     * line, and not one glyph is drawn.  Assembling the stack says which
     * TextKit this is before there is anything to convert. */
    {
        NSTextStorage *store = [[[NSTextStorage alloc] init] autorelease];
        NSLayoutManager *layout = [[[NSLayoutManager alloc] init] autorelease];
        NSTextContainer *box = [[[NSTextContainer alloc]
            initWithContainerSize:NSMakeSize(NSWidth(frame), CGFLOAT_MAX)] autorelease];

        [store addLayoutManager:layout];
        [layout addTextContainer:box];
        tv = [[NoteTextView alloc] initWithFrame:frame textContainer:box];
    }
    [tv setDocIndex:doc];
    [tv setDelegate:(id<NSTextViewDelegate>)h->delegate];
    [tv setAllowsUndo:YES];
    /* Rich text is on for one reason and one only: a text view with it off
     * enforces one set of attributes across the whole document, and quietly
     * flattens every colour the highlighter has just set into whichever one
     * it saw first -- a file that comes out entirely the colour of its first
     * token.  Everything rich text would otherwise let in is refused instead:
     * no graphics, no fonts or colours from the pasteboard (NoteTextView's
     * -paste: is -pasteAsPlainText:), and no substitutions. */
    [tv setRichText:YES];
    [tv setImportsGraphics:NO];
    [tv setAllowsDocumentBackgroundColorChange:NO];
    [tv setAllowsImageEditing:NO];
    [tv setUsesFindPanel:NO];
    [tv setAutomaticQuoteSubstitutionEnabled:NO];
    [tv setAutomaticDashSubstitutionEnabled:NO];
    [tv setAutomaticTextReplacementEnabled:NO];
    [tv setAutomaticSpellingCorrectionEnabled:NO];
    [tv setSmartInsertDeleteEnabled:NO];
    [tv setContinuousSpellCheckingEnabled:NO];
    [tv setVerticallyResizable:YES];
    [tv setMinSize:NSMakeSize(0, 0)];
    [tv setMaxSize:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX)];
    [tv setTextContainerInset:NSMakeSize(4.0, 4.0)];
    [[tv textContainer] setWidthTracksTextView:YES];
    [tv setAutoresizingMask:NSViewWidthSizable];

    [scroll setDocumentView:tv];

    ruler = [[NoteRuler alloc] initWithScrollView:scroll orientation:NSVerticalRuler];
    [ruler setClientView:tv];
    [scroll setVerticalRulerView:ruler];
    [scroll setHasVerticalRuler:YES];
    [scroll setRulersVisible:h->linenums];

    h->d[doc].scroll = scroll;
    h->d[doc].edit   = tv;
    h->d[doc].ruler  = ruler;
    h->d[doc].title  = [@"Untitled" retain];

    [(NSView *)h->stack addSubview:scroll];
    [scroll setHidden:YES];

    h_set_wrap(h, h->app.wrap);
    edit_apply_font(h);
    h_set_theme(h, h->theme_index);
    return 1;
}

void h_tab_destroy(note_host *h, int doc)
{
    int i;
    if (doc < 0 || doc >= NOTE_MAX_DOCS || !h->d[doc].edit) return;

    [(NSView *)h->d[doc].scroll removeFromSuperview];
    [(NSScrollView *)h->d[doc].scroll release];
    [(NSString *)h->d[doc].title release];
    h->d[doc].scroll = h->d[doc].edit = h->d[doc].ruler = h->d[doc].title = NULL;

    /* The core addresses documents by index and closes one by sliding the
     * rest down; the controls have to slide with them or tab 3 would be
     * editing what tab 4 shows. */
    for (i = doc; i < NOTE_MAX_DOCS - 1; i++) {
        h->d[i] = h->d[i + 1];
        if (h->d[i].edit) [(NoteTextView *)h->d[i].edit setDocIndex:i];
    }
    h->d[NOTE_MAX_DOCS - 1].scroll = h->d[NOTE_MAX_DOCS - 1].edit = NULL;
    h->d[NOTE_MAX_DOCS - 1].ruler  = h->d[NOTE_MAX_DOCS - 1].title = NULL;

    h->cache_dirty = 1;
    chrome_repaint(h);
}

void h_tab_select(note_host *h, int doc)
{
    int i;
    for (i = 0; i < NOTE_MAX_DOCS; i++)
        if (h->d[i].scroll) [(NSView *)h->d[i].scroll setHidden:(i != doc)];

    h->cache_dirty = 1;
    edit_focus(h);
    chrome_repaint(h);
    h_rehighlight(h);
    edit_update_status(h);
}

void h_tab_title(note_host *h, int doc, const nchar *title)
{
    if (doc < 0 || doc >= NOTE_MAX_DOCS) return;
    [(NSString *)h->d[doc].title release];
    h->d[doc].title = [ns_from_n(title) retain];
    chrome_repaint(h);
}

/* ==========================================================================
 * note_host_ops: the rest of the chrome
 * ========================================================================== */

void h_set_title(note_host *h, const nchar *s)
{
    [(NSWindow *)h->window setTitle:ns_from_n(s)];
}

void h_set_status(note_host *h, const nchar *s)
{
    NoteStatus *st = (NoteStatus *)h->status;
    [st setText:ns_from_n(s)];
    [st setNeedsDisplay:YES];
}

void h_show_status(note_host *h, int visible)
{
    h->status_visible = visible;
    chrome_layout(h);
}

void h_set_wrap(note_host *h, int wrap)
{
    int i;
    for (i = 0; i < NOTE_MAX_DOCS; i++) {
        NoteTextView *tv = (NoteTextView *)h->d[i].edit;
        NSScrollView *sc = (NSScrollView *)h->d[i].scroll;
        if (!tv) continue;

        if (wrap) {
            [sc setHasHorizontalScroller:NO];
            [tv setHorizontallyResizable:NO];
            [[tv textContainer] setWidthTracksTextView:YES];
            [[tv textContainer] setContainerSize:
                NSMakeSize([sc contentSize].width, CGFLOAT_MAX)];
            [tv setFrameSize:NSMakeSize([sc contentSize].width, NSHeight([tv frame]))];
        } else {
            [sc setHasHorizontalScroller:YES];
            [tv setHorizontallyResizable:YES];
            [[tv textContainer] setWidthTracksTextView:NO];
            [[tv textContainer] setContainerSize:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX)];
        }
        [tv setNeedsDisplay:YES];
    }
}

void h_set_linenums(note_host *h, int on)
{
    int i;
    h->linenums = on;
    for (i = 0; i < NOTE_MAX_DOCS; i++) {
        if (!h->d[i].scroll) continue;
        [(NSScrollView *)h->d[i].scroll setRulersVisible:on ? YES : NO];
        [(NoteRuler *)h->d[i].ruler setRuleThickness:
            [(NoteRuler *)h->d[i].ruler wantedThickness]];
    }
}

void h_set_zoom(note_host *h, int percent)
{
    (void)percent;
    edit_apply_font(h);
}

void h_set_theme(note_host *h, int idx)
{
    const note_theme *th;
    int i;

    h->theme_index = idx;
    th = chrome_theme(h);
    if (!th) return;

    [(NSWindow *)h->window setAppearance:
        [NSAppearance appearanceNamed:th->dark ? NSAppearanceNameDarkAqua
                                               : NSAppearanceNameAqua]];
    [(NSWindow *)h->window setBackgroundColor:chrome_color(th->bg)];

    for (i = 0; i < NOTE_MAX_DOCS; i++) {
        NoteTextView *tv = (NoteTextView *)h->d[i].edit;
        if (!tv) continue;
        [tv setBackgroundColor:chrome_color(th->bg)];
        [tv setTextColor:chrome_color(th->fg)];
        [tv setInsertionPointColor:chrome_color(th->caret ? th->caret : th->fg)];
        [tv setSelectedTextAttributes:@{
            NSBackgroundColorAttributeName: chrome_color(th->sel_bg) }];
        [(NSScrollView *)h->d[i].scroll setBackgroundColor:chrome_color(th->bg)];
        [(NoteRuler *)h->d[i].ruler setNeedsDisplay:YES];
    }
    chrome_repaint(h);
}
