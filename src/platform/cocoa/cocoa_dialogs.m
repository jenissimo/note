/* cocoa_dialogs.m -- the platform's own dialogs: open, save, font, find, print
 *
 * Part of note's Cocoa backend.  Every one of these is the system's, not a
 * drawing of one: NSOpenPanel can search, preview, sort and reach iCloud, and
 * nothing note could put on the screen instead would be worth losing that.
 */

#import "note_cocoa.h"

/* ==========================================================================
 * Open and save
 * ========================================================================== */

int h_dlg_open(note_host *h, nchar *path, int cap)
{
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    (void)h;

    path[0] = 0;
    [panel setCanChooseFiles:YES];
    [panel setCanChooseDirectories:NO];
    [panel setAllowsMultipleSelection:NO];
    [panel setTreatsFilePackagesAsDirectories:YES];

    if ([panel runModal] != NSModalResponseOK) return 0;
    n_from_ns([[panel URL] path], path, cap);
    return path[0] ? 1 : 0;
}

int h_dlg_save(note_host *h, nchar *path, int cap)
{
    NSSavePanel *panel = [NSSavePanel savePanel];
    NSString *current = ns_from_n(path);
    (void)h;

    [panel setTreatsFilePackagesAsDirectories:YES];
    [panel setExtensionHidden:NO];
    if ([current length]) {
        [panel setNameFieldStringValue:[current lastPathComponent]];
        [panel setDirectoryURL:[NSURL fileURLWithPath:
            [current stringByDeletingLastPathComponent]]];
    } else {
        [panel setNameFieldStringValue:@"Untitled.txt"];
    }

    if ([panel runModal] != NSModalResponseOK) return 0;
    n_from_ns([[panel URL] path], path, cap);
    return path[0] ? 1 : 0;
}

/* ==========================================================================
 * Font size and style
 *
 * The face itself is a list, so the palette shows it; this is the dialog for
 * everything a list cannot say, and it is the system's own font panel.
 * ========================================================================== */

@interface NoteFontTarget : NSObject
@end

@implementation NoteFontTarget
- (void)changeFont:(id)sender
{
    NSFont *old = [NSFont fontWithName:[NSString stringWithUTF8String:g.face]
                                  size:g.fontpt];
    NSFont *new_;
    if (!old) old = [NSFont monospacedSystemFontOfSize:g.fontpt
                                                weight:NSFontWeightRegular];
    new_ = [sender convertFont:old];
    if (!new_) return;

    strncpy(g.face, [[new_ fontName] UTF8String] ?: "Menlo", COCOA_FACE_MAX - 1);
    g.face[COCOA_FACE_MAX - 1] = 0;
    /* The panel changes the size on its own terms; zoom multiplies whatever
     * it settles on, so what is stored is the unzoomed point size. */
    g.fontpt = [new_ pointSize] * 100.0 / (double)g.app.zoom;
    edit_apply_font(&g);
}
- (NSFontPanelModeMask)validModesForFontPanel:(NSFontPanel *)panel
{
    (void)panel;
    return NSFontPanelModesMaskStandardModes;
}
@end

static NoteFontTarget *g_font_target;

void h_dlg_font(note_host *h)
{
    NSFontManager *fm = [NSFontManager sharedFontManager];
    NSFont *f = [NSFont fontWithName:[NSString stringWithUTF8String:h->face]
                                size:h->fontpt * (double)h->app.zoom / 100.0];

    if (!g_font_target) g_font_target = [[NoteFontTarget alloc] init];
    if (!f) f = [NSFont monospacedSystemFontOfSize:13.0 weight:NSFontWeightRegular];

    [fm setTarget:g_font_target];
    [fm setSelectedFont:f isMultiple:NO];
    [[fm fontPanel:YES] makeKeyAndOrderFront:nil];
}

/* ==========================================================================
 * Find and replace
 *
 * A panel of note's own, because there is no system one that edits the core's
 * search state.  What it is allowed to decide is nothing: the text, the flags
 * and the direction go into note_app, and the search itself is
 * note_find_again() -- so Find Next from this panel and F3 from the keyboard
 * are the same code path, which is the only way they stay the same answer.
 * ========================================================================== */

@interface NoteFindTarget : NSObject
- (void)findNext:(id)sender;
- (void)findPrev:(id)sender;
- (void)replaceOne:(id)sender;
- (void)replaceAll:(id)sender;
@end

static void find_pull(note_host *h)
{
    unsigned flags = FIND_DOWN;
    n_from_ns([(NSTextField *)h->findfield stringValue], h->app.find, NOTE_FIND_MAX);
    n_from_ns([(NSTextField *)h->replfield stringValue], h->app.replace, NOTE_FIND_MAX);
    if ([(NSButton *)h->findcase state] == NSControlStateValueOn) flags |= FIND_MATCHCASE;
    if ([(NSButton *)h->findword state] == NSControlStateValueOn) flags |= FIND_WHOLEWORD;
    h->app.find_flags = flags;
}

@implementation NoteFindTarget

- (void)findNext:(id)sender { (void)sender; find_pull(&g); note_find_again(&g.app, 0); }
- (void)findPrev:(id)sender { (void)sender; find_pull(&g); note_find_again(&g.app, 1); }

- (void)replaceOne:(id)sender
{
    int from, to;
    nchar sel[NOTE_FIND_MAX];
    NoteTextView *tv = (NoteTextView *)g.d[g.app.active].edit;

    (void)sender;
    find_pull(&g);
    if (!tv) return;

    /* Replace means "this match", so it only acts when a match is what is
     * selected; otherwise it finds the next one and leaves it there. */
    h_sel_get(&g, &from, &to);
    if (to > from) {
        NSRange r = [tv selectedRange];
        n_from_ns([[tv string] substringWithRange:r], sel, NOTE_FIND_MAX);
        if ((g.app.find_flags & FIND_MATCHCASE)
                ? n_eq(sel, g.app.find)
                : [[[tv string] substringWithRange:r]
                    caseInsensitiveCompare:ns_from_n(g.app.find)] == NSOrderedSame) {
            h_sel_replace(&g, g.app.replace);
        }
    }
    note_find_again(&g.app, 0);
}

- (void)replaceAll:(id)sender
{
    int guard = 0;
    (void)sender;
    find_pull(&g);
    if (!g.app.find[0]) return;

    h_sel_set(&g, 0, 0);
    while (h_find_text(&g, g.app.find, g.app.find_flags | FIND_DOWN)) {
        h_sel_replace(&g, g.app.replace);
        /* A replacement that contains the needle would otherwise be found
         * again for ever; the count is what stops that being a hang. */
        if (++guard > 100000) break;
    }
}
@end

static NoteFindTarget *g_find_target;

static NSButton *find_button(NSString *title, id target, SEL action, NSRect frame)
{
    NSButton *b = [[[NSButton alloc] initWithFrame:frame] autorelease];
    [b setTitle:title];
    [b setBezelStyle:NSBezelStyleRounded];
    [b setTarget:target];
    [b setAction:action];
    return b;
}

void h_dlg_find(note_host *h, int replace)
{
    NSPanel *panel = (NSPanel *)h->findpanel;
    NSView *v;

    if (!g_find_target) g_find_target = [[NoteFindTarget alloc] init];

    if (!panel) {
        NSTextField *ff, *rf;
        NSButton *cs, *ww;

        panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 420, 132)
                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                              | NSWindowStyleMaskUtilityWindow
                      backing:NSBackingStoreBuffered defer:NO];
        [panel setTitle:@"Find"];
        [panel setHidesOnDeactivate:NO];
        [panel setReleasedWhenClosed:NO];
        v = [panel contentView];

        [v addSubview:({
            NSTextField *l = [NSTextField labelWithString:@"Find:"];
            [l setFrame:NSMakeRect(12, 100, 70, 18)]; l; })];
        [v addSubview:({
            NSTextField *l = [NSTextField labelWithString:@"Replace:"];
            [l setFrame:NSMakeRect(12, 72, 70, 18)]; l; })];

        ff = [[[NSTextField alloc] initWithFrame:NSMakeRect(86, 98, 320, 22)] autorelease];
        rf = [[[NSTextField alloc] initWithFrame:NSMakeRect(86, 70, 320, 22)] autorelease];
        [ff setTarget:g_find_target]; [ff setAction:@selector(findNext:)];
        [v addSubview:ff];
        [v addSubview:rf];

        cs = [[[NSButton alloc] initWithFrame:NSMakeRect(86, 44, 120, 18)] autorelease];
        [cs setButtonType:NSButtonTypeSwitch]; [cs setTitle:@"Match case"];
        ww = [[[NSButton alloc] initWithFrame:NSMakeRect(212, 44, 140, 18)] autorelease];
        [ww setButtonType:NSButtonTypeSwitch]; [ww setTitle:@"Whole word"];
        [v addSubview:cs];
        [v addSubview:ww];

        [v addSubview:find_button(@"Previous", g_find_target, @selector(findPrev:),
                                  NSMakeRect(12, 8, 90, 28))];
        [v addSubview:find_button(@"Replace", g_find_target, @selector(replaceOne:),
                                  NSMakeRect(106, 8, 90, 28))];
        [v addSubview:find_button(@"All", g_find_target, @selector(replaceAll:),
                                  NSMakeRect(200, 8, 70, 28))];
        [v addSubview:find_button(@"Find Next", g_find_target, @selector(findNext:),
                                  NSMakeRect(274, 8, 130, 28))];

        h->findpanel = panel;
        h->findfield = ff;
        h->replfield = rf;
        h->findcase  = cs;
        h->findword  = ww;
        [panel center];
    }

    [(NSTextField *)h->findfield setStringValue:ns_from_n(h->app.find)];
    [(NSTextField *)h->replfield setStringValue:ns_from_n(h->app.replace)];
    [panel setTitle:replace ? @"Find and Replace" : @"Find"];
    [panel makeKeyAndOrderFront:nil];
    [panel makeFirstResponder:(NSTextField *)h->findfield];
}

/* ==========================================================================
 * Page setup and print
 * ========================================================================== */

void h_dlg_pagesetup(note_host *h)
{
    (void)h;
    [[NSPageLayout pageLayout] runModal];
}

void h_dlg_print(note_host *h)
{
    NoteTextView *tv = (NoteTextView *)h->d[h->app.active].edit;
    NSTextView *sheet;
    NSPrintInfo *info = [NSPrintInfo sharedPrintInfo];
    NSSize paper = [info paperSize];
    NSRect body = NSMakeRect(0, 0,
                             paper.width - [info leftMargin] - [info rightMargin],
                             paper.height - [info topMargin] - [info bottomMargin]);

    if (!tv) return;

    /* Printed off a view of its own rather than the editor: the one on screen
     * is wrapped to the window, and a page is not the window. */
    sheet = [[[NSTextView alloc] initWithFrame:body] autorelease];
    [sheet setString:[tv string]];
    [sheet setFont:[tv font]];
    [sheet setTextColor:[NSColor blackColor]];
    [sheet setBackgroundColor:[NSColor whiteColor]];

    [[NSPrintOperation printOperationWithView:sheet printInfo:info] runOperation];
}

/* ==========================================================================
 * Questions
 * ========================================================================== */

int h_ask_save(note_host *h, const nchar *name)
{
    NSAlert *alert = [[[NSAlert alloc] init] autorelease];
    NSModalResponse r;

    [alert setMessageText:[NSString stringWithFormat:
        @"Do you want to save changes to %@?", ns_from_n(name)]];
    [alert setInformativeText:@"Your changes will be lost if you don't save them."];
    [alert addButtonWithTitle:@"Save"];
    [alert addButtonWithTitle:@"Don't Save"];
    [alert addButtonWithTitle:@"Cancel"];
    [alert setAlertStyle:NSAlertStyleWarning];
    (void)h;

    r = [alert runModal];
    if (r == NSAlertFirstButtonReturn)  return ASK_YES;
    if (r == NSAlertSecondButtonReturn) return ASK_NO;
    return ASK_CANCEL;
}

void h_message(note_host *h, const nchar *text, const nchar *title)
{
    NSAlert *alert = [[[NSAlert alloc] init] autorelease];
    (void)h;
    [alert setMessageText:ns_from_n(title)];
    /* The core writes its longer messages with CRLF, which is what the
     * control it grew up with wanted; a label here would show the CRs. */
    [alert setInformativeText:[ns_from_n(text)
        stringByReplacingOccurrencesOfString:@"\r\n" withString:@"\n"]];
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
}

/* ==========================================================================
 * Running a command in the folder of the file on screen
 *
 * On Windows this is cmd.exe /k in a console of its own, because the output
 * is the point.  The same argument here names Terminal: a command whose
 * output vanished with the process would be worse than no Run at all.
 * ========================================================================== */

void dlg_run_command(note_host *h, const nchar *cmdline)
{
    note_doc *d = &h->app.docs[h->app.active];
    NSString *dir = @"~";
    NSString *cmd = ns_from_n(cmdline);
    NSString *script;
    NSTask *task;

    if (![cmd length]) return;

    if (d->path[0])
        dir = [ns_from_n(d->path) stringByDeletingLastPathComponent];

    /* A file name with a quote in it is the user's own, so it is escaped
     * rather than refused. */
    script = [NSString stringWithFormat:
        @"tell application \"Terminal\"\n"
        @"  activate\n"
        @"  do script \"cd %@ && %@\"\n"
        @"end tell",
        [[dir stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"]
              stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""],
        [[cmd stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"]
              stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""]];

    task = [[[NSTask alloc] init] autorelease];
    [task setLaunchPath:@"/usr/bin/osascript"];
    [task setArguments:@[@"-e", script]];
    @try { [task launch]; } @catch (NSException *e) { (void)e; }
}
