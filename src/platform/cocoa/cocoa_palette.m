/* cocoa_palette.m -- the command palette overlay and its list modes
 *
 * Part of note's Cocoa backend.  What is in the list, how a query filters it
 * and where the matched letters fell is note_palette.c's and is shared with
 * every other backend; here there is a view, a paint routine and the keys.
 *
 * It is a view over the editor rather than a window, so nothing ever takes
 * focus away from the text control -- which is also why the keys arrive from
 * the event monitor in cocoa_main.m instead of from a responder chain.
 *
 * A mode is a title, something that fills the rows, and two callbacks: one to
 * show a row while it is merely selected, one to keep it.  Adding the next
 * list is a row in kModes rather than another window.
 */

#import "note_cocoa.h"

#define PAL_W        560.0
#define PAL_ROW_H     24.0
#define PAL_INPUT_H   34.0
#define PAL_ROWS      10
#define PAL_PAD        8.0
#define PAL_TOP       48.0

enum { PAL_CMDS = 0, PAL_THEME, PAL_FONT, PAL_LINE, PAL_RENAME,
       PAL_PATH, PAL_RUN, PAL_TABS, PAL_MODE_COUNT };

typedef struct {
    const char *title;    /* drawn ahead of the query, or NULL            */
    const char *hint;     /* placeholder while nothing has been typed     */
    const char *empty;    /* when the query matches nothing               */
    void (*fill)   (note_host *h);
    void (*retype) (note_host *h);   /* rows depend on the query itself   */
    void (*enter)  (note_host *h);   /* remember what a preview may move  */
    void (*preview)(note_host *h, const note_pal_row *r);
    int  (*commit) (note_host *h, const note_pal_row *r);  /* 1 = may close */
    void (*cancel) (note_host *h);
    int   typed;          /* the query is the answer, not a filter        */
    int   digits;         /* ...and it may only be digits                 */
    int   paths;          /* ...and it is a path, whose folder is listed  */
} pal_mode;

static note_palette g_pal;
static int          g_mode;
static int          g_sel;
static NSMutableArray *g_history;   /* what Run has been asked for before */

static const note_pal_row *pal_current(void)
{
    return note_palette_at(&g_pal, g_sel);
}

/* ==========================================================================
 * Commands
 * ========================================================================== */

static void fill_cmds(note_host *h)
{
    (void)h;
    note_palette_commands(&g_pal);
}

static int commit_cmd(note_host *h, const note_pal_row *r)
{
    if (r && r->id && note_command(&h->app, (int)r->id)) menu_sync(h);
    return 1;
}

/* ==========================================================================
 * Themes -- a list you look at, so moving the selection paints the editor
 * ========================================================================== */

static void fill_theme(note_host *h)
{
    int i, n = note_theme_count();
    (void)h;
    note_palette_reset(&g_pal);
    /* Borrowed, not copied: the registry owns these names for the life of the
     * process, and 338 of them would not fit in the palette's pool. */
    for (i = 0; i < n; i++)
        note_palette_add(&g_pal, (unsigned)i, note_theme_get(i)->name, 0);
    note_palette_filter(&g_pal);
}

static void enter_theme(note_host *h) { h->pal_theme_prev = h->app.theme_index; }

static void preview_theme(note_host *h, const note_pal_row *r)
{
    if (!r) return;
    h_set_theme(h, (int)r->id);
    h_rehighlight(h);
}

static int commit_theme(note_host *h, const note_pal_row *r)
{
    if (r) note_set_theme_index(&h->app, (int)r->id);
    return 1;
}

static void cancel_theme(note_host *h)
{
    h->app.theme_index = h->pal_theme_prev;
    note_apply_theme(&h->app);
}

/* ==========================================================================
 * Fonts
 * ========================================================================== */

static void fill_font(note_host *h)
{
    NSArray *families = [[NSFontManager sharedFontManager] availableFontFamilies];
    (void)h;

    note_palette_reset(&g_pal);
    for (NSString *family in [families sortedArrayUsingSelector:@selector(compare:)]) {
        NSFont *probe = [NSFont fontWithName:family size:12.0];
        nchar name[COCOA_FACE_MAX];
        /* Only fixed-pitch faces are offered, because only those work here:
         * the gutter measures a line in columns.  Filtering the list is what
         * makes the refusal invisible. */
        if (!probe || ![[probe fontDescriptor] symbolicTraits]
                       || !([[probe fontDescriptor] symbolicTraits]
                            & NSFontDescriptorTraitMonoSpace)) continue;
        n_from_ns(family, name, COCOA_FACE_MAX);
        note_palette_add_copy(&g_pal, 0, name, 0);
    }
    note_palette_filter(&g_pal);
}

static void enter_font(note_host *h)
{
    memcpy(h->pal_face_prev, h->face, COCOA_FACE_MAX);
}

static void set_face(note_host *h, const nchar *face)
{
    n_copy((nchar *)h->face, face, COCOA_FACE_MAX);
    edit_apply_font(h);
}

static void preview_font(note_host *h, const note_pal_row *r)
{
    if (r) set_face(h, r->label);
}

static int commit_font(note_host *h, const note_pal_row *r)
{
    if (r) set_face(h, r->label);
    return 1;
}

static void cancel_font(note_host *h)
{
    set_face(h, (const nchar *)h->pal_face_prev);
}

/* ==========================================================================
 * Go to line
 * ========================================================================== */

static void fill_line(note_host *h)
{
    (void)h;
    note_palette_reset(&g_pal);
}

static int commit_line(note_host *h, const note_pal_row *r)
{
    unsigned n = note_palette_number(&g_pal);
    (void)r;
    if (n) h_goto_line(h, (int)n);
    return 1;
}

/* ==========================================================================
 * Rename
 * ========================================================================== */

static void fill_rename(note_host *h)
{
    const nchar *base = note_basename(h->app.docs[h->app.active].path);
    int stem = n_len(base), i;

    note_palette_reset(&g_pal);
    /* The extension is almost never what is being changed, so the stem
     * arrives selected and typing replaces it. */
    for (i = n_len(base) - 1; i > 0; i--)
        if (base[i] == (nchar)'.') { stem = i; break; }
    note_palette_set(&g_pal, base, stem);
}

static int commit_rename(note_host *h, const note_pal_row *r)
{
    (void)r;
    /* 0 means the name was refused and the input must stay open; the reason
     * is already on the hint line by then. */
    return note_rename(&h->app, h->app.active, note_palette_query(&g_pal));
}

/* ==========================================================================
 * Open by path
 *
 * Everything up to the last separator is a folder, which gets listed; what
 * follows filters the listing.  A row with id 1 is a folder.
 * ========================================================================== */

static void fill_path(note_host *h)
{
    note_doc *d = &h->app.docs[h->app.active];
    NSString *dir = d->path[0]
        ? [ns_from_n(d->path) stringByDeletingLastPathComponent]
        : NSHomeDirectory();
    nchar start[NOTE_PATH_MAX];

    note_palette_reset(&g_pal);
    n_from_ns([dir stringByAppendingString:@"/"], start, NOTE_PATH_MAX);
    note_palette_set(&g_pal, start, 0);
}

static void retype_path(note_host *h)
{
    NSString *query = ns_from_n(note_palette_query(&g_pal));
    NSString *dir = [query stringByDeletingLastPathComponent];
    NSArray *names;
    int at;

    (void)h;
    note_palette_reset_rows(&g_pal);

    /* The filter starts after the last separator, so typing a folder narrows
     * the names inside it rather than matching the whole path. */
    at = n_len(note_palette_query(&g_pal));
    while (at > 0 && note_palette_query(&g_pal)[at - 1] != (nchar)'/') at--;
    note_palette_filter_from(&g_pal, at);

    if (![dir length]) dir = @"/";
    names = [[[NSFileManager defaultManager] contentsOfDirectoryAtPath:
                [dir stringByExpandingTildeInPath] error:NULL]
             sortedArrayUsingSelector:@selector(caseInsensitiveCompare:)];

    for (NSString *name in names) {
        BOOL isdir = NO;
        nchar row[NOTE_PATH_MAX];
        NSString *full = [[dir stringByExpandingTildeInPath]
                          stringByAppendingPathComponent:name];
        if ([name hasPrefix:@"."]) continue;
        [[NSFileManager defaultManager] fileExistsAtPath:full isDirectory:&isdir];
        n_from_ns(isdir ? [name stringByAppendingString:@"/"] : name,
                  row, NOTE_PATH_MAX);
        if (!note_palette_add_copy(&g_pal, isdir ? 1 : 0, row, 0)) break;
    }
    note_palette_filter(&g_pal);
}

/* Tab completes on to the selected name; a folder brings its own separator,
 * so completing to one descends into it. */
static void complete_path(note_host *h)
{
    const note_pal_row *r = pal_current();
    nchar path[NOTE_PATH_MAX];
    int at;

    if (!r) return;
    n_copy(path, note_palette_query(&g_pal), NOTE_PATH_MAX);
    at = n_len(path);
    while (at > 0 && path[at - 1] != (nchar)'/') at--;
    path[at] = 0;
    n_cat(path, r->label, NOTE_PATH_MAX);
    note_palette_set(&g_pal, path, 0);
    retype_path(h);
}

static int commit_path(note_host *h, const note_pal_row *r)
{
    nchar path[NOTE_PATH_MAX];
    NSString *full;

    if (r && r->id) { complete_path(h); return 0; }   /* a folder: descend */

    if (r) complete_path(h);
    n_copy(path, note_palette_query(&g_pal), NOTE_PATH_MAX);
    full = [ns_from_n(path) stringByExpandingTildeInPath];
    n_from_ns(full, path, NOTE_PATH_MAX);

    if (!path[0]) return 1;
    note_open(&h->app, path);
    return 1;
}

/* ==========================================================================
 * Run a command
 * ========================================================================== */

static void fill_run(note_host *h)
{
    (void)h;
    note_palette_reset(&g_pal);
    /* The rows are what has been run before, most recent first, so repeating
     * the last command is two keys. */
    for (NSString *cmd in g_history) {
        nchar row[NOTE_PALETTE_QUERY];
        n_from_ns(cmd, row, NOTE_PALETTE_QUERY);
        if (!note_palette_add_copy(&g_pal, 0, row, 0)) break;
    }
    note_palette_filter(&g_pal);
}

static int commit_run(note_host *h, const note_pal_row *r)
{
    const nchar *q = note_palette_query(&g_pal);
    nchar line[NOTE_PALETTE_QUERY];
    NSString *cmd;

    /* Typing anything replaces the history row, because in this mode the
     * query is the answer rather than a filter. */
    if (q && q[0]) n_copy(line, q, NOTE_PALETTE_QUERY);
    else if (r)    n_copy(line, r->label, NOTE_PALETTE_QUERY);
    else           return 1;

    if (!line[0]) return 1;

    /* A named document with unsaved changes is written out first: running the
     * file as it was two edits ago is the one way this wastes an afternoon. */
    if (h->app.docs[h->app.active].dirty && h->app.docs[h->app.active].path[0])
        note_command(&h->app, CMD_FILE_SAVE);

    if (!g_history) g_history = [[NSMutableArray alloc] init];
    cmd = ns_from_n(line);
    [g_history removeObject:cmd];
    [g_history insertObject:cmd atIndex:0];
    while ([g_history count] > 20) [g_history removeLastObject];

    dlg_run_command(h, line);
    return 1;
}

/* ==========================================================================
 * Go to an open tab
 * ========================================================================== */

static void fill_tabs(note_host *h)
{
    int i;
    note_palette_reset(&g_pal);
    for (i = 0; i < h->app.ndocs; i++) {
        nchar title[NOTE_PATH_MAX];
        note_doc_title(&h->app, i, title, NOTE_PATH_MAX);
        note_palette_add_copy(&g_pal, (unsigned)i, title, 0);
    }
    note_palette_filter(&g_pal);
}

static int commit_tabs(note_host *h, const note_pal_row *r)
{
    if (r) note_select_doc(&h->app, (int)r->id);
    return 1;
}

/* ==========================================================================
 * The table of modes
 * ========================================================================== */

static const pal_mode kModes[PAL_MODE_COUNT] = {
    { NULL, "Type a command", "No matching command",
      fill_cmds, NULL, NULL, NULL, commit_cmd, NULL, 0, 0, 0 },
    { "Theme", "Filter palettes", "No matching palette",
      fill_theme, NULL, enter_theme, preview_theme, commit_theme, cancel_theme, 0, 0, 0 },
    { "Font", "Filter faces", "No matching face",
      fill_font, NULL, enter_font, preview_font, commit_font, cancel_font, 0, 0, 0 },
    { "Go to line", "Line number", NULL,
      fill_line, NULL, NULL, NULL, commit_line, NULL, 1, 1, 0 },
    { "Rename", "New name", NULL,
      fill_rename, NULL, NULL, NULL, commit_rename, NULL, 1, 0, 0 },
    { "Open path", "Type a path", "Nothing there",
      fill_path, retype_path, NULL, NULL, commit_path, NULL, 1, 0, 1 },
    { "Run", "Command to run", NULL,
      fill_run, NULL, NULL, NULL, commit_run, NULL, 1, 0, 0 },
    { "Go to tab", "Filter tabs", "No matching tab",
      fill_tabs, NULL, NULL, NULL, commit_tabs, NULL, 0, 0, 0 }
};

/* ==========================================================================
 * The overlay
 * ========================================================================== */

static int pal_rows_shown(void)
{
    int n = note_palette_count(&g_pal);
    return n > PAL_ROWS ? PAL_ROWS : n;
}

static NSRect pal_frame(note_host *h)
{
    NSRect b = [(NSView *)h->content bounds];
    CGFloat w = NSWidth(b) - 40.0;
    CGFloat rows = (CGFloat)pal_rows_shown();
    CGFloat hh = h->pal_msg[0] ? 22.0 : 0.0;
    CGFloat height = PAL_INPUT_H + rows * PAL_ROW_H + hh + PAL_PAD;

    if (w > PAL_W) w = PAL_W;
    return NSMakeRect((NSWidth(b) - w) / 2.0,
                      NSMaxY(b) - PAL_TOP - height, w, height);
}

@implementation NotePalette

- (BOOL)isFlipped { return YES; }

- (void)drawRect:(NSRect)dirty
{
    const note_theme *th = chrome_theme(&g);
    const pal_mode *m = &kModes[g_mode];
    NSRect b = [self bounds];
    NSFont *font = [NSFont systemFontOfSize:13.0];
    NSFont *small = [NSFont systemFontOfSize:11.0];
    const nchar *query = note_palette_query(&g_pal);
    CGFloat y;
    int i, shown = pal_rows_shown();

    (void)dirty;
    if (!th) return;

    [chrome_color(th->ui_bg) set];
    NSRectFill(b);
    [chrome_color(th->sel_bg) set];
    NSFrameRect(b);

    /* The input line: a title for the mode, then what has been typed, then a
     * caret.  The palette owns the query -- caret, selection and one level of
     * undo are note_palette.c's, so every backend gets the same input line
     * rather than half of one. */
    y = PAL_PAD / 2.0;
    {
        CGFloat x = 10.0;
        NSDictionary *dim = @{ NSFontAttributeName: font,
            NSForegroundColorAttributeName: chrome_color(th->ui_fg) };
        NSDictionary *on = @{ NSFontAttributeName: font,
            NSForegroundColorAttributeName: chrome_color(th->fg) };
        NSString *typed = ns_from_n(query);
        int from, to;

        if (m->title) {
            NSString *t = [NSString stringWithFormat:@"%s  ", m->title];
            [t drawAtPoint:NSMakePoint(x, y + 6.0) withAttributes:dim];
            x += [t sizeWithAttributes:dim].width;
        }

        if ([typed length]) {
            /* The selection the palette holds is drawn, so that a rename
             * arriving with its stem selected looks like what it is. */
            if (note_palette_sel(&g_pal, &from, &to)) {
                NSUInteger a = u16_of_byte(typed, from), z = u16_of_byte(typed, to);
                NSRect r;
                NSString *head = [typed substringToIndex:a];
                NSString *mid  = [typed substringWithRange:NSMakeRange(a, z - a)];
                CGFloat hx = x + [head sizeWithAttributes:on].width;
                r = NSMakeRect(hx, y + 5.0, [mid sizeWithAttributes:on].width, 18.0);
                [chrome_color(th->sel_bg) set];
                NSRectFill(r);
            }
            [typed drawAtPoint:NSMakePoint(x, y + 6.0) withAttributes:on];
            x += [typed sizeWithAttributes:on].width;
        } else if (m->hint) {
            [[NSString stringWithUTF8String:m->hint]
                drawAtPoint:NSMakePoint(x, y + 6.0) withAttributes:dim];
        }

        if ([typed length] || !m->hint) {
            NSString *head = [typed substringToIndex:
                u16_of_byte(typed, note_palette_caret(&g_pal))];
            CGFloat cx = 10.0 + (m->title ? [[NSString stringWithFormat:@"%s  ", m->title]
                                             sizeWithAttributes:dim].width : 0.0)
                              + [head sizeWithAttributes:on].width;
            [chrome_color(th->caret ? th->caret : th->fg) set];
            NSRectFill(NSMakeRect(cx, y + 5.0, 1.5, 18.0));
        }
    }

    y = PAL_INPUT_H;
    for (i = 0; i < shown; i++) {
        const note_pal_row *r = note_palette_at(&g_pal, i);
        NSRect row = NSMakeRect(0, y, NSWidth(b), PAL_ROW_H);
        int sel = (i == g_sel);
        NSDictionary *attrs;

        if (!r) break;
        if (sel) { [chrome_color(th->sel_bg) set]; NSRectFill(row); }

        attrs = @{ NSFontAttributeName: font,
            NSForegroundColorAttributeName: chrome_color(sel ? th->fg : th->ui_fg) };
        [ns_from_n(r->label) drawAtPoint:NSMakePoint(14.0, y + 3.0) withAttributes:attrs];

        if (r->accel) {
            NSDictionary *acc = @{ NSFontAttributeName: small,
                NSForegroundColorAttributeName: chrome_color(th->tok[TOK_COMMENT]) };
            NSString *a = accel_text(r->accel);
            [a drawAtPoint:NSMakePoint(NSWidth(b) - 14.0
                                       - [a sizeWithAttributes:acc].width, y + 5.0)
             withAttributes:acc];
        }
        y += PAL_ROW_H;
    }

    if (!shown && kModes[g_mode].empty && !kModes[g_mode].typed) {
        [[NSString stringWithUTF8String:kModes[g_mode].empty]
            drawAtPoint:NSMakePoint(14.0, y + 3.0)
         withAttributes:@{ NSFontAttributeName: font,
            NSForegroundColorAttributeName: chrome_color(th->ui_fg) }];
    }

    /* "No, and here is why", which is the one thing a mode that must stay
     * open to be corrected needs and a modal box cannot do. */
    if (g.pal_msg[0]) {
        [ns_from_n(g.pal_msg) drawAtPoint:NSMakePoint(14.0, NSMaxY(b) - 20.0)
         withAttributes:@{ NSFontAttributeName: small,
            NSForegroundColorAttributeName: chrome_color(th->tok[TOK_NUMBER]) }];
    }
}

- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    int row = (int)((p.y - PAL_INPUT_H) / PAL_ROW_H);

    if (p.y < PAL_INPUT_H || row < 0 || row >= pal_rows_shown()) return;
    g_sel = row;
    if (kModes[g_mode].preview) kModes[g_mode].preview(&g, pal_current());
    if (kModes[g_mode].commit && kModes[g_mode].commit(&g, pal_current()))
        pal_close(&g);
    else
        [self setNeedsDisplay:YES];
}
@end

/* ==========================================================================
 * Opening, closing, keys
 * ========================================================================== */

static void pal_relayout(note_host *h)
{
    [(NotePalette *)h->overlay setFrame:pal_frame(h)];
    [(NotePalette *)h->overlay setNeedsDisplay:YES];
}

static void pal_open_mode(note_host *h, int mode)
{
    const pal_mode *m = &kModes[mode];

    if (h->pal_open && g_mode == mode) { pal_close(h); return; }
    if (h->pal_open) pal_close(h);

    g_mode = mode;
    g_sel  = 0;
    h->pal_msg[0] = 0;

    note_palette_reset(&g_pal);
    if (m->enter)  m->enter(h);
    if (m->fill)   m->fill(h);
    if (m->retype) m->retype(h);

    if (!h->overlay) {
        NotePalette *view = [[NotePalette alloc] initWithFrame:pal_frame(h)];
        h->overlay = view;
        [(NSView *)h->content addSubview:view];
    }
    [(NSView *)h->overlay setHidden:NO];
    h->pal_open = 1;
    pal_relayout(h);
    if (m->preview) m->preview(h, pal_current());
}

void pal_open_commands(note_host *h) { pal_open_mode(h, PAL_CMDS);   }
void pal_open_theme   (note_host *h) { pal_open_mode(h, PAL_THEME);  }
void pal_open_font    (note_host *h) { pal_open_mode(h, PAL_FONT);   }
void pal_open_line    (note_host *h) { pal_open_mode(h, PAL_LINE);   }
void pal_open_rename  (note_host *h) { pal_open_mode(h, PAL_RENAME); }
void pal_open_path    (note_host *h) { pal_open_mode(h, PAL_PATH);   }
void pal_open_run     (note_host *h) { pal_open_mode(h, PAL_RUN);    }
void pal_open_tabs    (note_host *h) { pal_open_mode(h, PAL_TABS);   }

void pal_close(note_host *h)
{
    if (!h->pal_open) return;
    h->pal_open = 0;
    h->pal_msg[0] = 0;
    [(NSView *)h->overlay setHidden:YES];
    edit_focus(h);
}

static void pal_after_type(note_host *h)
{
    if (kModes[g_mode].retype) kModes[g_mode].retype(h);
    g_sel = 0;
    if (kModes[g_mode].preview) kModes[g_mode].preview(h, pal_current());
    pal_relayout(h);
}

static void pal_move(note_host *h, int delta)
{
    int n = note_palette_count(&g_pal);
    if (n <= 0) return;
    g_sel += delta;
    if (g_sel < 0) g_sel = n - 1;
    if (g_sel >= n) g_sel = 0;
    if (kModes[g_mode].preview) kModes[g_mode].preview(h, pal_current());
    pal_relayout(h);
}

int pal_key(note_host *h, NSEvent *e)
{
    const pal_mode *m = &kModes[g_mode];
    NSString *chars = [e charactersIgnoringModifiers];
    NSEventModifierFlags mods = [e modifierFlags];
    unichar key = [chars length] ? [chars characterAtIndex:0] : 0;
    int cmd = (mods & NSEventModifierFlagCommand) != 0;
    int shift = (mods & NSEventModifierFlagShift) != 0;
    int alt = (mods & NSEventModifierFlagOption) != 0;

    if (!h->pal_open) return 0;

    switch (key) {
    case 27:   /* Esc */
        if (m->cancel) m->cancel(h);
        pal_close(h);
        return 1;
    case '\r':
    case 3:    /* Enter */
        if (m->commit && m->commit(h, pal_current())) pal_close(h);
        else pal_relayout(h);
        return 1;
    case NSUpArrowFunctionKey:   pal_move(h, -1); return 1;
    case NSDownArrowFunctionKey: pal_move(h, +1); return 1;
    case NSLeftArrowFunctionKey:
        note_palette_edit(&g_pal, alt ? PAL_ED_WORD_LEFT : PAL_ED_LEFT, shift);
        pal_relayout(h);
        return 1;
    case NSRightArrowFunctionKey:
        note_palette_edit(&g_pal, alt ? PAL_ED_WORD_RIGHT : PAL_ED_RIGHT, shift);
        pal_relayout(h);
        return 1;
    case NSHomeFunctionKey:
        note_palette_edit(&g_pal, PAL_ED_HOME, shift); pal_relayout(h); return 1;
    case NSEndFunctionKey:
        note_palette_edit(&g_pal, PAL_ED_END, shift); pal_relayout(h); return 1;
    case NSDeleteFunctionKey:
        if (note_palette_edit(&g_pal, PAL_ED_DELETE, 0)) pal_after_type(h);
        else pal_relayout(h);
        return 1;
    case '\t':
        if (m->paths) {
            /* Tab walks the matches rather than completing blindly: the
             * listing is already the answer, and picking from it is what
             * descending into a folder means here. */
            if (shift) pal_move(h, -1); else pal_move(h, +1);
            return 1;
        }
        return 1;
    case 127:  /* Backspace */
        if (note_palette_edit(&g_pal, alt ? PAL_ED_BACK_WORD : PAL_ED_BACK, 0))
            pal_after_type(h);
        else pal_relayout(h);
        return 1;
    default: break;
    }

    if (cmd) {
        switch (key) {
        case 'a': note_palette_edit(&g_pal, PAL_ED_ALL, 0); pal_relayout(h); return 1;
        case 'z': if (note_palette_edit(&g_pal, PAL_ED_UNDO, 0)) pal_after_type(h);
                  return 1;
        case 'c': {
            nchar buf[NOTE_PALETTE_QUERY];
            if (note_palette_selected(&g_pal, buf, NOTE_PALETTE_QUERY)) {
                NSPasteboard *pb = [NSPasteboard generalPasteboard];
                [pb clearContents];
                [pb setString:ns_from_n(buf) forType:NSPasteboardTypeString];
            }
            return 1;
        }
        case 'v': {
            NSString *s = [[NSPasteboard generalPasteboard]
                            stringForType:NSPasteboardTypeString];
            nchar buf[NOTE_PALETTE_QUERY];
            if (s) {
                n_from_ns(s, buf, NOTE_PALETTE_QUERY);
                if (note_palette_insert(&g_pal, buf)) pal_after_type(h);
            }
            return 1;
        }
        case 'k': pal_close(h); return 1;
        default: return 1;   /* the editor is not listening while this is up */
        }
    }

    /* Anything printable goes into the query, which is where the ranking and
     * the caret already live. */
    {
        NSString *typed = [e characters];
        NSUInteger i;
        int changed = 0;
        for (i = 0; i < [typed length]; i++) {
            unichar c = [typed characterAtIndex:i];
            if (c < 32 || c == 127) continue;
            if (m->digits && (c < '0' || c > '9')) continue;
            if (note_palette_type(&g_pal, (unsigned)c)) changed = 1;
        }
        if (changed) pal_after_type(h);
    }
    return 1;
}
