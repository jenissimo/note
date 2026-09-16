/* cocoa_main.m -- the window, the menu bar, the key routing, startup
 *
 * Part of note's Cocoa backend.  A backend is note_host_ops plus a message
 * loop; on macOS the loop is NSApplication's, so what is left here is the
 * window it runs, the menus built out of the core's own tables, and the one
 * place keys are decided.
 *
 * Why the keys are decided in one place.  AppKit would happily dispatch every
 * shortcut from the menu bar, and for Cut, Copy and Paste it does -- those
 * are the text control's own and belong to it.  Everything note names itself
 * comes out of note_accels, the same table the Win32 accelerator table and
 * the F1 key sheet are built from, so a binding that changes there changes
 * here: a local event monitor matches against it before the menu bar or the
 * control ever see the event.  Two dispatch paths for one table would be two
 * chances to disagree with it.
 */

#import "note_cocoa.h"

/* The core names its keys with the Windows virtual-key numbers it was first
 * written against.  They are just numbers -- what they mean on this platform
 * is decided here. */
#define NKEY_TAB   0x09
#define NKEY_F1    0x70
#define NKEY_F2    0x71
#define NKEY_F3    0x72
#define NKEY_F5    0x74
#define NKEY_PLUS  0xBB
#define NKEY_MINUS 0xBD

static void tabs_to_content(note_host *h);
static void tabs_to_titlebar(note_host *h);

static NSMenu *g_ctxmenu;
static NSPanel *g_helppanel;
static NSTimeInterval g_last_shift;

/* ==========================================================================
 * Menu labels
 *
 * The core writes a label the way Windows menus want it: "&Open...\tCtrl+O".
 * The ampersand marks a mnemonic macOS does not have, and the tab introduces
 * a shortcut the menu item draws for itself from its key equivalent.  Both
 * come off here; neither is the core's business to know about.
 * ========================================================================== */

static NSString *menu_label(const nchar *raw)
{
    char buf[128];
    int i = 0, j = 0;

    for (; raw[i] && raw[i] != '\t' && j < (int)sizeof(buf) - 1; i++) {
        if (raw[i] == '&') continue;
        buf[j++] = (char)raw[i];
    }
    buf[j] = 0;
    return [NSString stringWithUTF8String:buf] ?: @"";
}

/* The same shortcut, spelled the way this platform spells it.  The core
 * writes "Ctrl+Shift+S" because that is what it is on the machine note grew
 * up on; here the keys have symbols and a user reads them nowhere else.  Only
 * the drawing changes -- what the key does is still accel_mac()'s. */
NSString *accel_text(const nchar *raw)
{
    NSString *s = ns_from_n(raw);
    s = [s stringByReplacingOccurrencesOfString:@"Ctrl+"  withString:@"\u2318"];
    s = [s stringByReplacingOccurrencesOfString:@"Shift+" withString:@"\u21E7"];
    s = [s stringByReplacingOccurrencesOfString:@"Alt+"   withString:@"\u2325"];
    s = [s stringByReplacingOccurrencesOfString:@"Tab"    withString:@"\u21E5"];
    return s;
}

/* What a note_accel is on this keyboard.  Ctrl is Command, which is what the
 * same gesture is called here -- except on Tab, where Command+Tab belongs to
 * the window switcher and every editor on the platform uses Control+Tab. */
static int accel_mac(const note_accel *a, NSString **key, NSEventModifierFlags *mods)
{
    NSEventModifierFlags m = 0;
    NSString *k = nil;

    if (a->mods & ACC_CTRL)  m |= (a->key == NKEY_TAB) ? NSEventModifierFlagControl
                                                       : NSEventModifierFlagCommand;
    if (a->mods & ACC_SHIFT) m |= NSEventModifierFlagShift;
    if (a->mods & ACC_ALT)   m |= NSEventModifierFlagOption;

    switch (a->key) {
    case NKEY_TAB:   k = @"\t"; break;
    case NKEY_F1:    k = [NSString stringWithFormat:@"%C", (unichar)NSF1FunctionKey]; break;
    case NKEY_F2:    k = [NSString stringWithFormat:@"%C", (unichar)NSF2FunctionKey]; break;
    case NKEY_F3:    k = [NSString stringWithFormat:@"%C", (unichar)NSF3FunctionKey]; break;
    case NKEY_F5:    k = [NSString stringWithFormat:@"%C", (unichar)NSF5FunctionKey]; break;
    case NKEY_PLUS:  k = @"+"; break;
    case NKEY_MINUS: k = @"-"; break;
    default:
        if (a->key >= 'A' && a->key <= 'Z')
            k = [[NSString stringWithFormat:@"%c", (char)a->key] lowercaseString];
        else if (a->key >= '0' && a->key <= '9')
            k = [NSString stringWithFormat:@"%c", (char)a->key];
        break;
    }
    if (!k) return 0;
    *key = k;
    *mods = m;
    return 1;
}

static const note_accel *accel_for(int id)
{
    int i;
    for (i = 0; i < note_accel_count; i++)
        if (note_accels[i].id == id) return &note_accels[i];
    return NULL;
}

/* ==========================================================================
 * The delegate: commands, validation, the text view's own notifications
 * ========================================================================== */

@implementation NoteDelegate

- (void)noteCommand:(id)sender
{
    int cmd = (int)[(NSMenuItem *)sender tag];
    if (cmd == CMD_VIEW_PALETTE) { pal_open_commands(&g); return; }
    if (note_command(&g.app, cmd)) menu_sync(&g);
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    int cmd = (int)[item tag];
    const note_menu_item *it;

    for (it = note_menu; it->kind != MI_END || it->label; it++) {
        if (it->id != cmd) continue;
        if (it->kind == MI_CHECK || it->kind == MI_RADIO)
            [item setState:note_menu_check(&g.app, cmd) ? NSControlStateValueOn
                                                        : NSControlStateValueOff];
        break;
    }
    if (cmd == CMD_EDIT_UNDO) return h_can_undo(&g) ? YES : NO;
    return YES;
}

- (NSMenu *)contextMenu { return g_ctxmenu; }

/* --- the text control tells the core what the user did ------------------- */

- (void)textDidChange:(NSNotification *)note
{
    NoteTextView *tv = (NoteTextView *)[note object];
    int doc = [tv docIndex];

    if (doc < 0 || doc >= NOTE_MAX_DOCS || g.d[doc].busy) return;
    edit_invalidate_cache(&g);
    note_set_dirty(&g.app, doc, 1);
    h_rehighlight(&g);
    edit_update_status(&g);
}

- (void)textViewDidChangeSelection:(NSNotification *)note
{
    (void)note;
    edit_update_status(&g);
}

/* Scrolling is the other half of "colour the screen": the region that needs
 * attributes is wherever the viewport has moved to. */
- (void)viewScrolled:(NSNotification *)note
{
    (void)note;
    if (!g.quitting) h_rehighlight(&g);
}

/* --- the application ----------------------------------------------------- */

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app
{
    (void)app;
    if (g.quitting) return NSTerminateNow;
    if (!note_can_close_all(&g.app)) return NSTerminateCancel;
    note_session_save(&g.app);
    g.quitting = 1;
    return NSTerminateNow;
}

- (BOOL)windowShouldClose:(NSWindow *)window
{
    (void)window;
    note_command(&g.app, CMD_FILE_EXIT);
    return g.quitting ? YES : NO;
}

- (void)windowDidResize:(NSNotification *)note
{
    (void)note;
    chrome_layout(&g);
    h_rehighlight(&g);
}

/* Full screen moves two things at once: the title bar stops reserving its
 * height from the content, and the traffic lights stop reserving their width
 * from the strip.  Both are read at layout and paint time, so both need to be
 * asked again once the window has settled. */
- (void)windowDidEnterFullScreen:(NSNotification *)note
{
    (void)note;
    g.fullscreen = 1;
    tabs_to_content(&g);
    chrome_layout(&g);
    chrome_repaint(&g);
}

- (void)windowDidExitFullScreen:(NSNotification *)note
{
    (void)note;
    g.fullscreen = 0;
    tabs_to_titlebar(&g);
    chrome_layout(&g);
    chrome_repaint(&g);
}

- (BOOL)application:(NSApplication *)app openFile:(NSString *)filename
{
    nchar path[NOTE_PATH_MAX];
    (void)app;
    n_from_ns(filename, path, NOTE_PATH_MAX);
    return note_open(&g.app, path) ? YES : NO;
}

- (void)sessionTick:(NSTimer *)timer
{
    (void)timer;
    if (!g.quitting) note_session_save(&g.app);
}
@end

/* ==========================================================================
 * Building the menus
 * ========================================================================== */

/* Cut, Copy, Paste and Delete belong to whatever is focused, not to the
 * document the core is holding -- and note_accels says so by leaving all four
 * out of the shortcut table.  Left at that, though, they had no shortcut at
 * all: the menu item was note's own command with no key equivalent, so
 * Command-C did nothing anywhere, in the editor as much as in a text field.
 *
 * Giving them the standard selectors with a nil target hands them back to the
 * responder chain, which is where AppKit looks for them: the editor answers
 * when the editor is focused, the find field when it is, and the item greys
 * itself out through the responder's own validation rather than ours.  The
 * core still routes the same four through note_command when something else
 * asks for them, which is why h_edit_op stays as it is.
 */
static SEL standard_edit_action(int cmd, NSString **key)
{
    *key = @"";
    switch (cmd) {
    case CMD_EDIT_CUT:    *key = @"x"; return @selector(cut:);
    case CMD_EDIT_COPY:   *key = @"c"; return @selector(copy:);
    case CMD_EDIT_PASTE:  *key = @"v"; return @selector(paste:);
    case CMD_EDIT_DELETE:              return @selector(delete:);
    }
    return NULL;
}

static NSMenuItem *menu_item(const note_menu_item *it, id target)
{
    NSString *stdkey = nil;
    SEL std = standard_edit_action(it->id, &stdkey);
    const note_accel *acc = accel_for(it->id);
    NSMenuItem *item;
    NSString *key = nil;
    NSEventModifierFlags mods = 0;

    if (std) {
        item = [[[NSMenuItem alloc] initWithTitle:menu_label(it->label)
                                           action:std
                                    keyEquivalent:stdkey] autorelease];
        [item setTag:it->id];
        [item setTarget:nil];
        return item;
    }

    item = [[[NSMenuItem alloc] initWithTitle:menu_label(it->label)
                                       action:@selector(noteCommand:)
                                keyEquivalent:@""] autorelease];
    [item setTag:it->id];
    [item setTarget:target];

    /* The shortcut is drawn from the same table that dispatches it.  AppKit
     * would also dispatch it from here, and is deliberately never given the
     * chance: the monitor in note_key() takes the event first, so there is
     * one path and one table rather than two of each. */
    if (acc && accel_mac(acc, &key, &mods)) {
        [item setKeyEquivalent:key];
        [item setKeyEquivalentModifierMask:mods];
    }
    return item;
}

static void build_menus(note_host *h)
{
    NSMenu *bar = [[[NSMenu alloc] init] autorelease];
    const note_menu_item *it = note_menu;
    id target = (id)h->delegate;

    /* The application menu, which is macOS's own and has no equivalent in the
     * core's table: About and Quit live here on this platform, so they are
     * taken out of Help and File rather than shown twice. */
    {
        NSMenuItem *hold = [[[NSMenuItem alloc] init] autorelease];
        NSMenu *app = [[[NSMenu alloc] init] autorelease];
        NSMenuItem *about = [[[NSMenuItem alloc] initWithTitle:@"About note"
                                action:@selector(noteCommand:) keyEquivalent:@""] autorelease];
        NSMenuItem *quit = [[[NSMenuItem alloc] initWithTitle:@"Quit note"
                                action:@selector(noteCommand:) keyEquivalent:@"q"] autorelease];
        [about setTag:CMD_HELP_ABOUT]; [about setTarget:target];
        [quit  setTag:CMD_FILE_EXIT];  [quit  setTarget:target];
        [app addItem:about];
        [app addItem:[NSMenuItem separatorItem]];
        [app addItem:quit];
        [hold setSubmenu:app];
        [bar addItem:hold];
    }

    while (it->kind == MI_POPUP) {
        NSMenuItem *hold = [[[NSMenuItem alloc] init] autorelease];
        NSMenu *menu = [[[NSMenu alloc] initWithTitle:menu_label(it->label)] autorelease];

        [menu setAutoenablesItems:YES];
        for (it++; it->kind != MI_END; it++) {
            if (it->kind == MI_HIDDEN) continue;   /* a command, not an item */
            if (it->kind == MI_SEP) { [menu addItem:[NSMenuItem separatorItem]]; continue; }
            if (it->id == CMD_FILE_EXIT || it->id == CMD_HELP_ABOUT) continue;
            [menu addItem:menu_item(it, target)];
        }
        it++;

        [hold setSubmenu:menu];
        [hold setTitle:[menu title]];
        [bar addItem:hold];
    }

    [NSApp setMainMenu:bar];

    g_ctxmenu = [[NSMenu alloc] init];
    for (it = note_ctxmenu; it->kind != MI_END; it++) {
        if (it->kind == MI_SEP) { [g_ctxmenu addItem:[NSMenuItem separatorItem]]; continue; }
        [g_ctxmenu addItem:menu_item(it, target)];
    }
}

void menu_sync(note_host *h)
{
    (void)h;
    /* Nothing to push: check marks are answered by validateMenuItem: out of
     * note_menu_check() at the moment the menu is opened, which cannot fall
     * behind the way a pushed state can. */
    chrome_repaint(h);
}

/* ==========================================================================
 * The key sheet
 * ========================================================================== */

void h_show_help(note_host *h)
{
    note_help_row rows[128];
    int n = note_help_fill(rows, 128), i;
    NSMutableString *text = [NSMutableString string];
    NSTextView *view;
    NSScrollView *scroll;

    (void)h;
    for (i = 0; i < n; i++) {
        if (rows[i].group)
            [text appendFormat:@"%@%@\n", i ? @"\n" : @"", menu_label(rows[i].group)];
        {
            /* Padded by hand: %-34@ is not a width an NSString format honours,
             * and a sheet whose two columns do not line up is harder to read
             * than one column. */
            NSMutableString *label = [[menu_label(rows[i].label) mutableCopy] autorelease];
            while ([label length] < 30) [label appendString:@" "];
            [text appendFormat:@"    %@%@\n", label, accel_text(rows[i].keys)];
        }
    }

    if (!g_helppanel) {
        g_helppanel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 460, 560)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                     | NSWindowStyleMaskUtilityWindow | NSWindowStyleMaskResizable
              backing:NSBackingStoreBuffered defer:NO];
        [g_helppanel setTitle:@"Keyboard Shortcuts"];
        [g_helppanel setReleasedWhenClosed:NO];

        scroll = [[[NSScrollView alloc] initWithFrame:
                   [[g_helppanel contentView] bounds]] autorelease];
        [scroll setHasVerticalScroller:YES];
        [scroll setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        view = [[[NSTextView alloc] initWithFrame:[scroll bounds]] autorelease];
        [view setEditable:NO];
        [view setAutoresizingMask:NSViewWidthSizable];
        [view setFont:[NSFont monospacedSystemFontOfSize:12.0 weight:NSFontWeightRegular]];
        [scroll setDocumentView:view];
        [[g_helppanel contentView] addSubview:scroll];
        [g_helppanel center];
    }

    view = (NSTextView *)[(NSScrollView *)[[[g_helppanel contentView] subviews]
                                            objectAtIndex:0] documentView];
    [view setString:text];
    [g_helppanel makeKeyAndOrderFront:nil];
}

void h_quit(note_host *h)
{
    h->quitting = 1;
    [NSApp terminate:nil];
}

/* ==========================================================================
 * Keys
 * ========================================================================== */

static int key_matches(NSEvent *e, const note_accel *a)
{
    NSString *key = nil, *typed = [e charactersIgnoringModifiers];
    NSEventModifierFlags want = 0, have;

    if (!accel_mac(a, &key, &want)) return 0;

    have = [e modifierFlags] & (NSEventModifierFlagCommand | NSEventModifierFlagShift |
                                NSEventModifierFlagOption | NSEventModifierFlagControl);
    if (have != want) return 0;
    if (![typed length]) return 0;

    /* Shift turns the typed character into its upper case, and on a few keys
     * into a different character altogether, so the comparison is made
     * without it wherever the key has a plain form. */
    return [[typed lowercaseString] isEqualToString:[key lowercaseString]];
}

/* An editing shortcut typed into one of note's own panels belongs to the
 * field it was typed into, not to the document behind it.
 *
 * The monitor below sees every key the application gets, whichever window is
 * in front, and note_accels claims Select All and Undo for the core -- so
 * Command-A in the Find panel selected the whole file and left the search box
 * alone.  Handing the event back is not enough either: the menu bar would
 * then dispatch the very same command out of the very same table.  So when
 * the key window is a panel rather than the editor's window, the two are sent
 * to the first responder as the standard actions they are, and whatever is
 * focused there -- the find field, the replace field, the key sheet --
 * answers them itself.
 *
 * Cut, Copy and Paste are not here because they never needed to be: they are
 * the responder chain's everywhere, from the Edit menu (see menu_item), and a
 * panel's field editor is in that chain already.
 *
 * Everything else in the table still works from a panel: Command-S is still
 * save and F3 is still find-again, because neither is a thing a text field
 * has an opinion about.
 */
static int panel_edit_key(NSEvent *e)
{
    NSWindow *key = [NSApp keyWindow];
    NSString *ch;
    SEL sel = NULL;

    if (!key || key == (NSWindow *)g.window) return 0;
    if (!([e modifierFlags] & NSEventModifierFlagCommand)) return 0;

    ch = [[e charactersIgnoringModifiers] lowercaseString];
    if      ([ch isEqualToString:@"a"]) sel = @selector(selectAll:);
    else if ([ch isEqualToString:@"z"]) sel = ([e modifierFlags] & NSEventModifierFlagShift)
                                            ? @selector(redo:) : @selector(undo:);
    if (!sel) return 0;

    /* to:nil is what makes this the responder chain's answer rather than
     * ours: the field editor takes it if it can, and if nothing in the chain
     * can, nothing happens -- which is still the right answer, because the
     * document behind a panel is not what was being edited. */
    [NSApp sendAction:sel to:nil from:nil];
    return 1;
}

static NSEvent *note_key(NSEvent *e)
{
    int i;

    if (panel_edit_key(e)) return nil;
    if (g.pal_open) return pal_key(&g, e) ? nil : e;

    for (i = 0; i < note_accel_count; i++) {
        if (!key_matches(e, &note_accels[i])) continue;
        if (note_accels[i].id == CMD_VIEW_PALETTE) { pal_open_commands(&g); return nil; }
        if (note_command(&g.app, note_accels[i].id)) menu_sync(&g);
        return nil;
    }
    return e;
}

/* Shift, Shift asks where to go.  Two taps of the same key with nothing in
 * between them is a gesture rather than a shortcut, so it is watched for in
 * the modifier stream rather than looked up in the table. */
static NSEvent *note_flags(NSEvent *e)
{
    NSEventModifierFlags m = [e modifierFlags];
    NSTimeInterval now = [e timestamp];

    if ([e keyCode] != 56 && [e keyCode] != 60) return e;   /* the two shifts */
    if (m & NSEventModifierFlagShift) {
        if (now - g_last_shift < 0.4 && !g.pal_open) {
            g_last_shift = 0;
            pal_open_tabs(&g);
            return e;
        }
        g_last_shift = now;
    }
    return e;
}

/* ==========================================================================
 * Startup
 * ========================================================================== */

/* The tab strip sits in the title bar rather than under it.
 *
 * A window's worth of chrome is a real cost in an editor: the strip was 28
 * points of window that could have been text, and the title bar above it was
 * saying the name of the very file whose tab was already highlighted.  The
 * platform has one way to put a view up there -- a title bar accessory --
 * and it is the way that keeps the traffic lights, the drag, the double
 * click and full screen working, because the window is still doing all of
 * them itself.
 *
 * Not the other way: NSWindow's own tabs (tabbingMode) are a tab per window,
 * and note's documents live in the core, one window, several documents.
 * Native tabs would mean several cores.
 *
 * fullScreenMinHeight is what keeps the strip on the screen in full screen,
 * where the title bar itself slides away; without it the tabs would go with
 * it and there would be no way to change document.
 */
static void build_titlebar_tabs(note_host *h)
{
    NSWindow *window = (NSWindow *)h->window;
    NSTitlebarAccessoryViewController *bar =
        [[NSTitlebarAccessoryViewController alloc] init];

    h->tabs = [[NoteTabs alloc] initWithFrame:
        NSMakeRect(0, 0, NSWidth([[window contentView] bounds]), TABS_H)];
    [(NSView *)h->tabs setAutoresizingMask:NSViewWidthSizable];

    [bar setView:(NSView *)h->tabs];
    [bar setLayoutAttribute:NSLayoutAttributeRight];
    [window addTitlebarAccessoryViewController:bar];
    h->tabsbar = bar;
}

/* Full screen has no title bar to put anything in.  It hides itself, and it
 * takes its accessories with it -- fullScreenMinHeight keeps a bottom
 * accessory on the screen, but not one laid out inside the bar, which is the
 * only kind that is compact.  So for the length of full screen the strip
 * stops being title bar and becomes the top of the content, which is where
 * the window's top now is.  The same view either way: nothing about it is
 * built for one place or the other. */
static void tabs_to_content(note_host *h)
{
    NSWindow *window = (NSWindow *)h->window;
    if ([window titlebarAccessoryViewControllers].count)
        [window removeTitlebarAccessoryViewControllerAtIndex:0];
    [(NSView *)h->content addSubview:(NSView *)h->tabs];
}

static void tabs_to_titlebar(note_host *h)
{
    NSWindow *window = (NSWindow *)h->window;
    [(NSView *)h->tabs removeFromSuperview];
    if (![window titlebarAccessoryViewControllers].count) {
        [(NSTitlebarAccessoryViewController *)h->tabsbar setView:(NSView *)h->tabs];
        [window addTitlebarAccessoryViewController:
            (NSTitlebarAccessoryViewController *)h->tabsbar];
    }
}

static void build_window(note_host *h)
{
    NSRect frame = NSMakeRect(0, 0, 900, 620);
    NSWindow *window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                  NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable |
                  NSWindowStyleMaskFullSizeContentView
          backing:NSBackingStoreBuffered defer:NO];
    NSView *content = [window contentView];

    [window setTitle:@"note"];
    [window setDelegate:(id<NSWindowDelegate>)h->delegate];
    [window setFrameAutosaveName:@"noteWindow"];
    /* The title text is hidden rather than empty: the name of the file is
     * already on its tab, and a second copy of it would be sitting on top of
     * the strip.  The window still has a title -- the Window menu, Mission
     * Control and the Dock all ask for it. */
    [window setTitlebarAppearsTransparent:YES];
    [window setTitleVisibility:NSWindowTitleHidden];
    [window center];

    h->window  = window;
    h->content = content;

    build_titlebar_tabs(h);

    h->stack = [[NSView alloc] initWithFrame:NSZeroRect];
    [(NSView *)h->stack setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [content addSubview:(NSView *)h->stack];

    h->status = [[NoteStatus alloc] initWithFrame:NSZeroRect];
    [(NSView *)h->status setAutoresizingMask:NSViewWidthSizable | NSViewMaxYMargin];
    [content addSubview:(NSView *)h->status];
}

int main(int argc, const char **argv)
{
    @autoreleasepool {
        NoteDelegate *delegate = [[NoteDelegate alloc] init];
        int restored, i;

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp setDelegate:delegate];

        g.delegate = delegate;
        strncpy(g.face, "Menlo", COCOA_FACE_MAX - 1);
        g.fontpt = 13.0;
        g.theme_index = -1;
        g.status_visible = 1;
        g.linenums = 1;
        g.cache_doc = -1;

        build_window(&g);

        note_init(&g.app, &g, &kOps);
        build_menus(&g);

        /* Nothing in the core creates this: on Windows the build script drops
         * the packs into it and the folder exists by the time note looks.
         * Here the first run would otherwise have nowhere to write a session,
         * and unsaved work would be lost by exactly the thing meant to keep
         * it -- so the backend makes it before anything asks. */
        {
            nchar dir[NOTE_PATH_MAX];
            if (kOps.state_dir(&g, dir, NOTE_PATH_MAX)) kOps.dir_make(&g, dir);
        }

        note_defs_load(&g.app);
        note_apply_theme(&g.app);

        [[NSNotificationCenter defaultCenter] addObserver:delegate
            selector:@selector(viewScrolled:)
                name:NSScrollViewDidLiveScrollNotification object:nil];
        [[NSNotificationCenter defaultCenter] addObserver:delegate
            selector:@selector(viewScrolled:)
                name:NSScrollViewDidEndLiveScrollNotification object:nil];

        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                              handler:^NSEvent *(NSEvent *e) {
            return note_key(e);
        }];
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskFlagsChanged
                                              handler:^NSEvent *(NSEvent *e) {
            return note_flags(e);
        }];

        restored = note_session_restore(&g.app);

        /* A file named on the command line takes over an untouched Untitled
         * tab, so only the empty document is settled here. */
        {
            nchar arg[NOTE_PATH_MAX];
            arg[0] = 0;
            for (i = 1; i < argc; i++)
                if (argv[i][0] != '-') { n_copy(arg, (const nchar *)argv[i],
                                                NOTE_PATH_MAX); break; }

            if (!arg[0] && !restored) {
                int doc = note_new_doc(&g.app);
                if (doc >= 0) note_select_doc(&g.app, doc);
            }
            if (g.app.ndocs == 0) {
                int doc = note_new_doc(&g.app);
                if (doc >= 0) note_select_doc(&g.app, doc);
            }

            note_update_title(&g.app);
            chrome_layout(&g);
            [(NSWindow *)g.window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];

            /* And now the command line: a name that is not there is still
             * reported, but over a window that is already on the screen. */
            if (arg[0]) note_open(&g.app, arg);
        }

        edit_focus(&g);
        edit_update_status(&g);
        h_rehighlight(&g);

        [NSTimer scheduledTimerWithTimeInterval:4.0 target:delegate
            selector:@selector(sessionTick:) userInfo:nil repeats:YES];

        [NSApp run];
    }
    return 0;
}
