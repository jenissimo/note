/* cocoa_edit.m -- the native text control, the gutter and the highlighter
 *
 * Part of note's Cocoa backend.  The one rule this file exists to keep is the
 * one the project is built on: the text control is the platform's own.  This
 * is a real NSTextView with its own undo, its own IME, its own accessibility
 * and its own emoji picker -- not a drawing of one.  What note adds is the
 * colour, the gutter beside it, and the arithmetic that lets a core counting
 * UTF-8 bytes and an AppKit counting UTF-16 units talk about the same place.
 */

#import "note_cocoa.h"

#define MAX_SPANS   2048
/* How far back the lexer is allowed to look for a point it knows is outside
 * every string and comment.  Bounded on purpose: it is what keeps a repaint
 * costing the same on a thirteen-megabyte file as on a small one. */
#define SAFE_WINDOW 65536

/* ==========================================================================
 * Strings and offsets
 * ========================================================================== */

NSString *ns_from_n(const nchar *s)
{
    if (!s) return @"";
    return [NSString stringWithUTF8String:(const char *)s] ?: @"";
}

void n_from_ns(NSString *s, nchar *buf, int cap)
{
    if (cap <= 0) return;
    buf[0] = 0;
    if (!s) return;
    [s getCString:(char *)buf maxLength:(NSUInteger)cap encoding:NSUTF8StringEncoding];
}

/* One direction is a CFStringGetBytes with no buffer, which reports what a
 * range would weigh in UTF-8 and is exactly the question.  The other is not:
 * with a NULL buffer the byte budget is ignored, so asking "how many
 * characters fit in eighteen bytes" answers "all of them" -- which is a
 * highlighter that paints the whole file the colour of its first token and
 * looks for all the world like a text view that has stopped drawing.  So the
 * byte-to-character direction is counted here, off the UTF-8 itself. */
NSUInteger u16_count(const char *utf8, long bytes)
{
    NSUInteger units = 0;
    long i;
    for (i = 0; i < bytes; i++) {
        unsigned char c = (unsigned char)utf8[i];
        if ((c & 0xC0) == 0x80) continue;          /* a continuation byte */
        units += (c >= 0xF0) ? 2 : 1;              /* outside the BMP: a pair */
    }
    return units;
}

NSUInteger u16_of_byte(NSString *s, long byteoff)
{
    const char *utf8 = [s UTF8String];
    long n;
    if (byteoff <= 0 || !utf8) return 0;
    n = (long)strlen(utf8);
    if (byteoff > n) byteoff = n;
    return u16_count(utf8, byteoff);
}

long byte_of_u16(NSString *s, NSUInteger u16)
{
    CFIndex used = 0;
    CFIndex len = (CFIndex)[s length];
    if ((CFIndex)u16 > len) u16 = (NSUInteger)len;
    if (!u16) return 0;
    CFStringGetBytes((__bridge CFStringRef)s, CFRangeMake(0, (CFIndex)u16),
                     kCFStringEncodingUTF8, 0, false, NULL, 0, &used);
    return (long)used;
}

/* ==========================================================================
 * The control
 * ========================================================================== */

@implementation NoteTextView
- (void)setDocIndex:(int)i { _docIndex = i; }
/* The palette and the key sheet are overlays the editor keeps focus under, so
 * Esc must not be swallowed here as "cancel the field editor". */
- (void)cancelOperation:(id)sender { (void)sender; }
/* What arrives from the pasteboard is text, never formatting: note is a plain
 * text editor whose colours are its own. */
- (void)paste:(id)sender { (void)sender; [self pasteAsPlainText:nil]; }
/* Right-click gets the core's context menu rather than AppKit's spell-check
 * one: the commands are note's, and they are the same list every backend
 * shows because they come out of note_ctxmenu. */
- (NSMenu *)menuForEvent:(NSEvent *)event
{
    (void)event;
    return [(NoteDelegate *)g.delegate contextMenu];
}
@end

static NoteTextView *edit_of(note_host *h, int doc)
{
    if (doc < 0 || doc >= NOTE_MAX_DOCS) return nil;
    return (NoteTextView *)h->d[doc].edit;
}

static NoteTextView *active_edit(void)
{
    return edit_of(&g, g.app.active);
}

/* ==========================================================================
 * The UTF-8 mirror of the active document
 *
 * The core asks for text by byte offset, the lexer wants a run of bytes, and
 * AppKit will hand over a UTF-8 copy but not a stable pointer into one.  So
 * the backend keeps the copy, and rebuilds it only when the control says the
 * text changed -- which a highlight pass, being attributes only, does not.
 * ========================================================================== */

const char *edit_cache(note_host *h, long *len)
{
    NoteTextView *tv = active_edit();
    NSString *s;
    const char *utf8;
    long n;

    if (!tv) { if (len) *len = 0; return ""; }

    if (!h->cache_dirty && h->cache_doc == h->app.active && h->cache) {
        if (len) *len = h->cache_len;
        return h->cache;
    }

    s = [tv string];
    utf8 = [s UTF8String];
    n = utf8 ? (long)strlen(utf8) : 0;

    if (h->cache) free(h->cache);
    h->cache = (char *)malloc((size_t)n + 1);
    if (!h->cache) { h->cache_len = 0; if (len) *len = 0; return ""; }
    if (n) memcpy(h->cache, utf8, (size_t)n);
    h->cache[n] = 0;
    h->cache_len   = n;
    h->cache_doc   = h->app.active;
    h->cache_dirty = 0;

    if (len) *len = n;
    return h->cache;
}

void edit_invalidate_cache(note_host *h) { h->cache_dirty = 1; }

/* ==========================================================================
 * note_host_ops: the text of one document
 * ========================================================================== */

int h_text_len(note_host *h, int doc)
{
    NoteTextView *tv = edit_of(h, doc);
    if (!tv) return 0;
    return (int)byte_of_u16([tv string], [[tv string] length]);
}

int h_text_get(note_host *h, int doc, nchar *buf, int cap)
{
    NoteTextView *tv = edit_of(h, doc);
    const char *utf8;
    int n;

    if (cap <= 0) return 0;
    buf[0] = 0;
    if (!tv) return 0;

    utf8 = [[tv string] UTF8String];
    if (!utf8) return 0;
    n = (int)strlen(utf8);
    if (n > cap - 1) n = cap - 1;
    memcpy(buf, utf8, (size_t)n);
    buf[n] = 0;
    return n;
}

/* The core hands text over with CRLF line endings, because that is what the
 * control it was written against wanted.  A Cocoa text view wants LF and will
 * put LF back in as soon as anyone presses Return, so the conversion happens
 * here rather than being carried around as a mixed document: note_save reads
 * either and writes whatever the file's own ending was. */
void h_text_set(note_host *h, int doc, const nchar *s)
{
    NoteTextView *tv = edit_of(h, doc);
    NSString *str;

    if (!tv) return;
    str = ns_from_n(s);
    str = [str stringByReplacingOccurrencesOfString:@"\r\n" withString:@"\n"];
    str = [str stringByReplacingOccurrencesOfString:@"\r" withString:@"\n"];

    h->d[doc].busy = 1;
    [tv setString:str];
    /* Loading a file is not an edit anyone can undo their way out of. */
    [[tv undoManager] removeAllActions];
    h->d[doc].busy = 0;
    h->cache_dirty = 1;
    if (doc == h->app.active) h_rehighlight(h);
}

void h_sel_get(note_host *h, int *from, int *to)
{
    NoteTextView *tv = active_edit();
    NSRange r;
    NSString *s;

    *from = *to = 0;
    if (!tv) return;
    (void)h;
    r = [tv selectedRange];
    s = [tv string];
    *from = (int)byte_of_u16(s, r.location);
    *to   = (int)byte_of_u16(s, r.location + r.length);
}

void h_sel_set(note_host *h, int from, int to)
{
    NoteTextView *tv = active_edit();
    NSString *s;
    NSUInteger a, b;

    if (!tv) return;
    (void)h;
    s = [tv string];
    a = u16_of_byte(s, from);
    b = (to < 0) ? [s length] : u16_of_byte(s, to);
    if (b < a) b = a;
    [tv setSelectedRange:NSMakeRange(a, b - a)];
    [tv scrollRangeToVisible:NSMakeRange(a, b - a)];
}

void h_sel_replace(note_host *h, const nchar *s)
{
    NoteTextView *tv = active_edit();
    NSRange r;

    if (!tv) return;
    (void)h;
    r = [tv selectedRange];
    /* Through the text view's own insertion path, so this lands on its undo
     * stack like anything the user typed. */
    if ([tv shouldChangeTextInRange:r replacementString:ns_from_n(s)]) {
        [[tv textStorage] replaceCharactersInRange:r withString:ns_from_n(s)];
        [tv didChangeText];
    }
}

void h_edit_op(note_host *h, int cmd)
{
    NoteTextView *tv = active_edit();
    if (!tv) return;
    (void)h;
    switch (cmd) {
    case CMD_EDIT_UNDO:   [[tv undoManager] undo];  break;
    case CMD_EDIT_REDO:   [[tv undoManager] redo];  break;
    case CMD_EDIT_CUT:    [tv cut:nil];             break;
    case CMD_EDIT_COPY:   [tv copy:nil];            break;
    case CMD_EDIT_PASTE:  [tv paste:nil];           break;
    case CMD_EDIT_DELETE: [tv delete:nil];          break;
    }
}

int h_can_undo(note_host *h)
{
    NoteTextView *tv = active_edit();
    (void)h;
    return tv && [[tv undoManager] canUndo];
}

/* The control has no "modified" bit of its own to set -- what is dirty and
 * what is not is the core's own bookkeeping, and the tab title it drives is
 * refreshed from there.  Kept as a real entry rather than a null pointer so
 * that a control which grows one later has somewhere to say so. */
void h_set_modified(note_host *h, int doc, int modified)
{
    (void)h; (void)doc; (void)modified;
}

/* ==========================================================================
 * Find and go to
 * ========================================================================== */

static int is_word_char(unichar c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c > 127;
}

static int whole_word_at(NSString *s, NSRange r)
{
    if (r.location > 0 && is_word_char([s characterAtIndex:r.location - 1])) return 0;
    if (NSMaxRange(r) < [s length] &&
        is_word_char([s characterAtIndex:NSMaxRange(r)])) return 0;
    return 1;
}

int h_find_text(note_host *h, const nchar *needle, unsigned flags)
{
    NoteTextView *tv = active_edit();
    NSString *s, *what = ns_from_n(needle);
    NSStringCompareOptions opt = 0;
    NSRange sel, found, scan;
    int down = (flags & FIND_DOWN) != 0;
    int wrapped = 0;

    if (!tv || ![what length]) return 0;
    (void)h;
    s   = [tv string];
    sel = [tv selectedRange];

    if (!(flags & FIND_MATCHCASE)) opt |= NSCaseInsensitiveSearch;
    if (!down) opt |= NSBackwardsSearch;

    scan = down ? NSMakeRange(NSMaxRange(sel), [s length] - NSMaxRange(sel))
                : NSMakeRange(0, sel.location);

    for (;;) {
        found = [s rangeOfString:what options:opt range:scan];
        if (found.location == NSNotFound) {
            if (wrapped) return 0;
            /* One wrap, then give up: a search that circles for ever would
             * hang on a needle that is only ever half a word. */
            wrapped = 1;
            scan = down ? NSMakeRange(0, [s length])
                        : NSMakeRange(0, [s length]);
            continue;
        }
        if ((flags & FIND_WHOLEWORD) && !whole_word_at(s, found)) {
            if (down) {
                NSUInteger next = found.location + 1;
                if (next >= [s length]) { if (wrapped) return 0; wrapped = 1; next = 0; }
                scan = NSMakeRange(next, [s length] - next);
            } else {
                if (!found.location) { if (wrapped) return 0; wrapped = 1;
                                       scan = NSMakeRange(0, [s length]); }
                else scan = NSMakeRange(0, found.location);
            }
            continue;
        }
        break;
    }

    [tv setSelectedRange:found];
    [tv scrollRangeToVisible:found];
    [(NSWindow *)g.window makeFirstResponder:tv];
    h_rehighlight(&g);
    return 1;
}

void h_goto_line(note_host *h, int line)
{
    NoteTextView *tv = active_edit();
    NSString *s;
    NSUInteger idx = 0, n, at = 0;
    int have = 1;

    if (!tv) return;
    (void)h;
    if (line < 1) line = 1;
    s = [tv string];
    n = [s length];

    while (have < line && at < n) {
        NSRange para = [s lineRangeForRange:NSMakeRange(at, 0)];
        at = NSMaxRange(para);
        if (at <= para.location) break;
        have++;
        idx = at;
    }
    if (idx > n) idx = n;

    [tv setSelectedRange:NSMakeRange(idx, 0)];
    [tv scrollRangeToVisible:NSMakeRange(idx, 0)];
    [(NSWindow *)g.window makeFirstResponder:tv];
}

/* ==========================================================================
 * Highlighting
 *
 * The core lexes; the backend colours.  What is coloured is the screen and a
 * little either side of it, found by asking the layout manager which glyphs
 * are actually laid out in the visible rect -- so the cost of a repaint is
 * the cost of a screenful, whatever the document weighs.
 * ========================================================================== */

void h_rehighlight(note_host *h)
{
    NoteTextView *tv = active_edit();
    NSLayoutManager *lm;
    NSTextContainer *tc;
    NSTextStorage *ts;
    NSString *s;
    NSRange glyphs, chars;
    const char *text;
    long len, from, to, start;
    note_span spans[MAX_SPANS];
    int nspans, i, lang;
    const note_theme *th = chrome_theme(h);
    NSColor *base;

    if (!tv || !th) return;

    ts = [tv textStorage];
    s  = [tv string];
    lm = [tv layoutManager];
    tc = [tv textContainer];
    base = chrome_color(th->fg);

    glyphs = [lm glyphRangeForBoundingRect:[tv visibleRect] inTextContainer:tc];
    chars  = [lm characterRangeForGlyphRange:glyphs actualGlyphRange:NULL];

    /* A screenful either side, so scrolling by a line does not repaint. */
    {
        NSUInteger pad = chars.length ? chars.length : 512;
        NSUInteger lo = chars.location > pad ? chars.location - pad : 0;
        NSUInteger hi = NSMaxRange(chars) + pad;
        if (hi > [s length]) hi = [s length];
        chars = NSMakeRange(lo, hi - lo);
    }
    if (!chars.length) chars = NSMakeRange(0, [s length] > 4096 ? 4096 : [s length]);

    [ts beginEditing];
    [ts addAttribute:NSForegroundColorAttributeName value:base range:chars];

    lang = h->app.docs[h->app.active].lang;
    if (h->app.syntax && lang != LANG_NONE) {
        text = edit_cache(h, &len);
        from = byte_of_u16(s, chars.location);
        to   = byte_of_u16(s, NSMaxRange(chars));
        if (to > len) to = len;

        /* Where it is safe to start lexing: the nearest point behind the
         * region that is provably outside every string and comment. */
        start = note_syntax_safe_start(lang, text, (int)len, (int)from, SAFE_WINDOW);
        if (start < 0) start = 0;

        nspans = note_tokenize(lang, text + start, (int)(to - start), (int)start,
                               spans, MAX_SPANS);
        /* Spans arrive in order, so the byte-to-character mapping is walked
         * once across the whole run rather than recomputed from the top of
         * the document for each of them. */
        {
            long bpos = from;
            NSUInteger upos = u16_count(text, from);

            for (i = 0; i < nspans; i++) {
                long s0 = spans[i].start, s1 = s0 + spans[i].len;
                NSUInteger a, run;

                if (s1 <= from) continue;
                if (s0 >= to) break;
                if (s0 < from) s0 = from;
                if (s1 > to)   s1 = to;
                if (s1 <= s0)  continue;

                upos += u16_count(text + bpos, s0 - bpos);
                bpos = s0;
                a = upos;
                run = u16_count(text + bpos, s1 - s0);
                upos += run;
                bpos = s1;

                if (!run || a + run > [s length]) continue;
                [ts addAttribute:NSForegroundColorAttributeName
                           value:chrome_color(th->tok[spans[i].kind])
                           range:NSMakeRange(a, run)];
            }
        }
    }
    [ts endEditing];

    [(NoteRuler *)h->d[h->app.active].ruler setNeedsDisplay:YES];
}

/* ==========================================================================
 * Font, focus, status
 * ========================================================================== */

void edit_apply_font(note_host *h)
{
    NSFont *f;
    double pt = h->fontpt * (double)h->app.zoom / 100.0;
    int i;

    if (pt < 4) pt = 4;
    f = [NSFont fontWithName:[NSString stringWithUTF8String:h->face] size:pt];
    if (!f) f = [NSFont monospacedSystemFontOfSize:pt weight:NSFontWeightRegular];

    for (i = 0; i < NOTE_MAX_DOCS; i++) {
        NoteTextView *tv = edit_of(h, i);
        if (!tv) continue;
        [tv setFont:f];
        [(NoteRuler *)h->d[i].ruler setRuleThickness:
            [(NoteRuler *)h->d[i].ruler wantedThickness]];
        [(NoteRuler *)h->d[i].ruler setNeedsDisplay:YES];
    }
    h_rehighlight(h);
}

void edit_focus(note_host *h)
{
    NoteTextView *tv = active_edit();
    if (tv) [(NSWindow *)h->window makeFirstResponder:tv];
}

void edit_update_status(note_host *h)
{
    NoteTextView *tv = active_edit();
    const char *text;
    long len, at, i, line = 1, col;

    if (!tv) return;

    /* Counted off the UTF-8 mirror rather than by walking paragraphs: this
     * runs on every caret move, and a walk that visits one line at a time
     * turns a long document into a slow keystroke. */
    text = edit_cache(h, &len);
    at = byte_of_u16([tv string], [tv selectedRange].location);
    if (at > len) at = len;

    col = 1;
    for (i = 0; i < at; i++) {
        if (text[i] == '\n') { line++; col = 1; }
        else if ((text[i] & 0xC0) != 0x80) col++;   /* one per character */
    }
    note_status_at(&h->app, (int)line, (int)col);
}
