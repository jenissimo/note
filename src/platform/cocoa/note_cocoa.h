/* note_cocoa.h -- the state and types note's Cocoa backend shares across its parts
 *
 * Same shape as the Win32 backend's note_win32.h, and for the same reason: the
 * backend outgrew one file, and everything in it -- the tab strip, the gutter,
 * the palette, the message routing -- is already reading the same handles.  A
 * header says that out loud instead of pretending otherwise.
 *
 * The core never sees any of this.  It reaches the platform through
 * note_host_ops and nothing else; note_host is opaque to it by design.
 */
#ifndef NOTE_COCOA_H
#define NOTE_COCOA_H

#include "../../core/note_core.h"
#include "../../core/note_syntax.h"
#include "../../core/note_theme.h"
#include "../../core/note_palette.h"

#ifdef __OBJC__
#import <Cocoa/Cocoa.h>

/* The four AppKit classes the backend adds.  Declared together here because
 * every file below touches at least two of them. */
@interface NoteTextView : NSTextView
@property (nonatomic, assign) int docIndex;
@end

/* The line-number gutter: a real NSRulerView, so scrolling, magnification and
 * the text view's own layout move it without anything being kept in step by
 * hand. */
@interface NoteRuler : NSRulerView
- (CGFloat)wantedThickness;
@end

/* The tab strip and the status bar are drawn: AppKit has no control shaped
 * like either, and both are chrome the core describes rather than owns. */
@interface NoteTabs : NSView
@end

@interface NoteStatus : NSView
@property (retain) NSString *text;
@end

/* The command palette overlay: a view over the editor rather than a window,
 * so the text control never loses focus while it is up -- the same shape the
 * Win32 backend uses, and for the same reason. */
@interface NotePalette : NSView
@end

@interface NoteDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate,
                                    NSTextViewDelegate, NSMenuItemValidation>
- (NSMenu *)contextMenu;
- (void)noteCommand:(id)sender;
@end
#endif

/* AppKit objects are held as void * so this header is includable from plain C
 * as well as from Objective-C.  Nothing here is retained twice: each field is
 * the one owning reference, released when the window goes away. */

#define COCOA_FACE_MAX 64

/* The height of the tab strip.  It is in the header because the strip is a
 * title bar accessory now: chrome draws it, and the window has to say how
 * tall the accessory is when it builds it. */
#define TABS_H 28.0

typedef struct {
    void *edit;      /* NoteTextView *  -- the native control, one per doc */
    void *scroll;    /* NSScrollView *                                     */
    void *ruler;     /* NoteRuler *     -- the line-number gutter          */
    void *title;     /* NSString *      -- what the tab strip draws        */
    int   busy;      /* set while the core is writing: suppress dirtying   */
} cocoa_doc;

struct note_host {
    note_app app;

    void *window;      /* NSWindow *        */
    void *content;     /* NSView *          */
    void *tabs;        /* NoteTabs *        */
    void *tabsbar;     /* NSTitlebarAccessoryViewController * -- where tabs live
                        * while there is a title bar to live in              */
    int   fullscreen;  /* no title bar: the strip moves into the content     */
    void *status;      /* NoteStatus *      */
    void *stack;       /* NSView *  -- the scroll views live here          */
    void *delegate;    /* NoteDelegate *    */
    void *overlay;     /* NotePalette *  -- the command palette            */
    void *findpanel;   /* NSPanel *                                        */
    void *findfield;   /* NSTextField *                                    */
    void *replfield;   /* NSTextField *                                    */
    void *findcase;    /* NSButton *                                       */
    void *findword;    /* NSButton *                                       */
    void *keymon;      /* the local key monitor, while an overlay is up    */

    cocoa_doc d[NOTE_MAX_DOCS];

    /* The font every editor uses.  A face and a point size rather than an
     * NSFont, because zoom multiplies the size and the palette replaces the
     * face, and rebuilding from the two is cheaper than editing a font. */
    char  face[COCOA_FACE_MAX];
    double fontpt;

    int   theme_index;   /* what is on screen, which a preview may move  */
    int   pal_theme_prev;
    char  pal_face_prev[COCOA_FACE_MAX];
    int   pal_open;
    int   help_open;
    int   quitting;
    int   status_visible;
    int   linenums;

    /* The active document's text as the core spells it -- UTF-8 bytes --
     * rebuilt only when the control says it changed.  Every offset the core
     * hands over or asks for is a byte offset into this, and every offset
     * AppKit speaks is a UTF-16 index, so one of the two has to be mapped on
     * to the other; doing it from a copy we already hold beats asking the
     * control the same question per span. */
    char         *cache;
    long          cache_len;
    int           cache_doc;
    int           cache_dirty;

    nchar pal_msg[160];   /* the hint line under the palette's input */
};

extern struct note_host g;

/* --- what each part of the backend hands the others ---------------------- */

/* cocoa_host.m */
extern const note_host_ops kOps;
void *h_alloc(note_host *h, unsigned long bytes);
void  h_free (note_host *h, void *p);
void  h_set_hint_text(note_host *h, const nchar *text);
void  h_file_delete(note_host *h, const nchar *path);

/* cocoa_edit.m */
int   h_text_len (note_host *h, int doc);
int   h_text_get (note_host *h, int doc, nchar *buf, int cap);
void  h_text_set (note_host *h, int doc, const nchar *s);
void  h_sel_get  (note_host *h, int *from, int *to);
void  h_sel_set  (note_host *h, int from, int to);
void  h_sel_replace(note_host *h, const nchar *s);
void  h_edit_op  (note_host *h, int cmd);
int   h_can_undo (note_host *h);
void  h_set_modified(note_host *h, int doc, int modified);
int   h_find_text(note_host *h, const nchar *needle, unsigned flags);
void  h_goto_line(note_host *h, int line);
void  h_rehighlight(note_host *h);
void  edit_apply_font(note_host *h);
void  edit_focus(note_host *h);
void  edit_update_status(note_host *h);
const char *edit_cache(note_host *h, long *len);   /* the active doc, UTF-8 */
void  edit_invalidate_cache(note_host *h);

/* cocoa_chrome.m */
int   h_tab_create (note_host *h, int doc);
void  h_tab_destroy(note_host *h, int doc);
void  h_tab_select (note_host *h, int doc);
void  h_tab_title  (note_host *h, int doc, const nchar *title);
void  h_set_title  (note_host *h, const nchar *s);
void  h_set_status (note_host *h, const nchar *s);
void  h_show_status(note_host *h, int visible);
void  h_set_wrap   (note_host *h, int wrap);
void  h_set_linenums(note_host *h, int on);
void  h_set_zoom   (note_host *h, int percent);
void  h_set_theme  (note_host *h, int idx);
void  chrome_layout(note_host *h);
void  chrome_repaint(note_host *h);
const note_theme *chrome_theme(note_host *h);
#ifdef __OBJC__
NSColor *chrome_color(note_color c);
#endif

/* cocoa_dialogs.m */
int   h_dlg_open (note_host *h, nchar *path, int cap);
int   h_dlg_save (note_host *h, nchar *path, int cap);
void  h_dlg_font (note_host *h);
void  h_dlg_find (note_host *h, int replace);
void  h_dlg_pagesetup(note_host *h);
void  h_dlg_print(note_host *h);
int   h_ask_save (note_host *h, const nchar *name);
void  h_message  (note_host *h, const nchar *text, const nchar *title);
void  dlg_run_command(note_host *h, const nchar *cmdline);

/* cocoa_palette.m */
void  pal_open_commands(note_host *h);
void  pal_open_theme(note_host *h);
void  pal_open_font(note_host *h);
void  pal_open_line(note_host *h);
void  pal_open_rename(note_host *h);
void  pal_open_path(note_host *h);
void  pal_open_run(note_host *h);
void  pal_open_tabs(note_host *h);
void  pal_close(note_host *h);
#ifdef __OBJC__
int   pal_key(note_host *h, NSEvent *e);   /* 1 = the palette kept the key */
#endif

/* cocoa_main.m */
void  h_show_help(note_host *h);
void  h_quit(note_host *h);
void  menu_sync(note_host *h);
#ifdef __OBJC__
NSString *accel_text(const nchar *raw);   /* "Ctrl+S" as this platform says it */
#endif

/* --- small shared conversions -------------------------------------------- */
#ifdef __OBJC__
NSString *ns_from_n(const nchar *s);          /* UTF-8 bytes -> NSString    */
void      n_from_ns(NSString *s, nchar *buf, int cap);
/* Byte offsets are what the core counts in; UTF-16 indices are what AppKit
 * counts in.  Neither side is wrong, so the backend pays the conversion. */
NSUInteger u16_of_byte(NSString *s, long byteoff);
NSUInteger u16_count(const char *utf8, long bytes);
long       byte_of_u16(NSString *s, NSUInteger u16);
#endif

#endif /* NOTE_COCOA_H */
