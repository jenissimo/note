/* cocoa_host.m -- files, folders, the clock: the plain services the core asks for
 *
 * Part of note's Cocoa backend.  Nothing here is interesting on purpose: the
 * core cannot call the system, so this is where the system is called, and the
 * table at the bottom is the whole of what note knows about macOS.
 */

#import "note_cocoa.h"
#include <sys/stat.h>

struct note_host g;

/* ==========================================================================
 * Memory
 * ========================================================================== */

void *h_alloc(note_host *h, unsigned long bytes)
{
    (void)h;
    return malloc(bytes ? (size_t)bytes : 1);
}

void h_free(note_host *h, void *p)
{
    (void)h;
    if (p) free(p);
}

/* ==========================================================================
 * The clock and the system's own idea of light or dark
 * ========================================================================== */

static int h_time_date(note_host *h, nchar *buf, int cap)
{
    NSDateFormatter *f = [[NSDateFormatter alloc] init];
    NSString *s;
    (void)h;

    [f setDateStyle:NSDateFormatterShortStyle];
    [f setTimeStyle:NSDateFormatterShortStyle];
    s = [f stringFromDate:[NSDate date]];
    [f release];

    n_from_ns(s, buf, cap);
    return n_len(buf) > 0;
}

int h_system_dark(note_host *h)
{
    NSAppearance *ap;
    NSAppearanceName name;
    (void)h;

    ap = [NSApp effectiveAppearance];
    name = [ap bestMatchFromAppearancesWithNames:
        @[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
    return [name isEqualToString:NSAppearanceNameDarkAqua] ? 1 : 0;
}

/* ==========================================================================
 * Where things live
 *
 * The core joins a folder and a leaf with the platform's separator and asks
 * for both of these by name, so all this decides is which folders they are.
 * `state_dir` is the per-user one, the same place every other Mac application
 * keeps what it did not ask the user about; `exe_dir` is the portable one,
 * which for a bundle is Contents/Resources -- that is where a .app carries
 * files that are not code, and where build.sh puts the packs.
 * ========================================================================== */

static int h_state_dir(note_host *h, nchar *buf, int cap)
{
    NSArray *dirs = NSSearchPathForDirectoriesInDomains(
        NSApplicationSupportDirectory, NSUserDomainMask, YES);
    (void)h;
    if (![dirs count]) return 0;
    n_from_ns([[dirs objectAtIndex:0] stringByAppendingPathComponent:@"note"], buf, cap);
    return n_len(buf) > 0;
}

static int h_exe_dir(note_host *h, nchar *buf, int cap)
{
    NSBundle *bundle = [NSBundle mainBundle];
    NSString *path = [bundle resourcePath];
    (void)h;

    /* Run straight out of the build directory rather than as a bundle and
     * there is no Resources: the executable's own folder is then the
     * portable one, which is what every other backend means by this. */
    if (!path) path = [[[bundle executablePath] stringByDeletingLastPathComponent]
                       ?: @"." copy];
    n_from_ns(path, buf, cap);
    return n_len(buf) > 0;
}

static int h_dir_make(note_host *h, const nchar *dir)
{
    (void)h;
    return [[NSFileManager defaultManager]
             createDirectoryAtPath:ns_from_n(dir)
       withIntermediateDirectories:YES attributes:nil error:NULL] ? 1 : 0;
}

static int h_dir_list(note_host *h, const nchar *dir, const nchar *ext,
                      nchar *out, int cap)
{
    NSString *folder = ns_from_n(dir);
    NSString *suffix = [@"." stringByAppendingString:ns_from_n(ext)];
    NSArray *names;
    int used = 0, count = 0;
    (void)h;

    out[0] = 0;
    names = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:folder error:NULL];
    for (NSString *name in names) {
        NSString *full;
        const char *utf8;
        int len;

        if (![name hasSuffix:suffix]) continue;
        full = [folder stringByAppendingPathComponent:name];
        utf8 = [full UTF8String];
        if (!utf8) continue;
        len = (int)strlen(utf8);
        if (used + len + 2 >= cap) break;
        memcpy(out + used, utf8, (size_t)len + 1);
        used += len + 1;
        count++;
    }
    out[used] = 0;
    return count;
}

/* ==========================================================================
 * Files
 * ========================================================================== */

static int h_file_read(note_host *h, const nchar *path,
                       unsigned char **bytes, unsigned long *len)
{
    NSData *data = [NSData dataWithContentsOfFile:ns_from_n(path)];
    unsigned char *p;

    if (!data) return 0;
    p = (unsigned char *)h_alloc(h, (unsigned long)[data length] + 1);
    if (!p) return 0;
    memcpy(p, [data bytes], [data length]);
    p[[data length]] = 0;
    *bytes = p;
    *len   = (unsigned long)[data length];
    return 1;
}

static int h_file_write(note_host *h, const nchar *path,
                        const unsigned char *bytes, unsigned long len)
{
    NSData *data = [NSData dataWithBytes:bytes length:(NSUInteger)len];
    (void)h;
    return [data writeToFile:ns_from_n(path) atomically:YES] ? 1 : 0;
}

void h_file_delete(note_host *h, const nchar *path)
{
    (void)h;
    if (path && path[0])
        [[NSFileManager defaultManager] removeItemAtPath:ns_from_n(path) error:NULL];
}

static int h_file_exists(note_host *h, const nchar *path)
{
    (void)h;
    if (!path || !path[0]) return 0;
    return [[NSFileManager defaultManager] fileExistsAtPath:ns_from_n(path)] ? 1 : 0;
}

/* Across volumes as well as within one: the user asked for the file to end up
 * in the new place, not for a lecture about partitions. */
static int h_file_rename(note_host *h, const nchar *from, const nchar *to)
{
    NSFileManager *fm = [NSFileManager defaultManager];
    (void)h;
    if (!from || !from[0] || !to || !to[0]) return 0;
    return [fm moveItemAtPath:ns_from_n(from) toPath:ns_from_n(to) error:NULL] ? 1 : 0;
}

/* ==========================================================================
 * The hint line, the pickers, quitting
 * ========================================================================== */

void h_set_hint_text(note_host *h, const nchar *text)
{
    n_copy(h->pal_msg, text ? text : N(""),
           (int)(sizeof(h->pal_msg) / sizeof(h->pal_msg[0])));
    if (h->overlay && h->pal_open) [(NotePalette *)h->overlay setNeedsDisplay:YES];
}

static void h_set_hint(note_host *h, const nchar *text)
{
    h_set_hint_text(h, text);
}

/* The core names a list and stops there.  Every one of them is a mode of the
 * command palette here, exactly as on Win32: an overlay that is already the
 * right shape for "type, and pick one of these" beats six dialogs. */
static void h_pick(note_host *h, int what)
{
    switch (what) {
    case PICK_THEME:  pal_open_theme(h);  break;
    case PICK_FONT:   pal_open_font(h);   break;
    case PICK_LINE:   pal_open_line(h);   break;
    case PICK_RENAME: pal_open_rename(h); break;
    case PICK_PATH:   pal_open_path(h);   break;
    case PICK_RUN:    pal_open_run(h);    break;
    default: break;
    }
}

/* ==========================================================================
 * The table
 *
 * Positional, like every other backend's: the core's contract is the order of
 * these fields, and naming them here would only hide a field that moved.
 *
 * `embedded_pack` is a null pointer, which the core allows.  There is nothing
 * for it to read: a Mach-O bundle has no resource fork worth the name, and a
 * .app already has a folder for files that are not code -- so the packs ship
 * in Contents/Resources and arrive through exe_dir with everything else.
 * ========================================================================== */

const note_host_ops kOps = {
    h_alloc, h_free,
    h_text_len, h_text_get, h_text_set,
    h_sel_get, h_sel_set, h_sel_replace,
    h_edit_op, h_can_undo, h_set_modified,
    h_tab_create, h_tab_destroy, h_tab_select, h_tab_title,
    h_set_title, h_set_status, h_show_status, h_set_wrap, h_set_linenums,
    h_set_zoom, h_set_theme, h_rehighlight,
    h_dlg_open, h_dlg_save, h_dlg_font, h_dlg_find, h_pick,
    h_dlg_pagesetup, h_dlg_print, h_ask_save, h_message,
    h_find_text, h_goto_line, h_time_date, h_system_dark,
    h_state_dir, h_exe_dir, h_dir_make, h_dir_list,
    h_file_read, h_file_write, h_file_delete, 0,
    h_file_exists, h_file_rename, h_set_hint, h_show_help, h_quit
};
