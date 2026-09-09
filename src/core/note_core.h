/* note — a small, fast Notepad replacement.
 *
 * note_core.h — the portable core.  Everything in the core layer is free of
 * OS headers: it owns the document model, the menu/command model (the shared
 * slice of UX), the dirty/discard flow, the on-disk encoding, the session
 * that survives a crash, the syntax lexer and the colour palettes.  It
 * reaches the outside world only through note_host_ops, which each platform
 * backend fills in with its own native widgets.
 */
#ifndef NOTE_CORE_H
#define NOTE_CORE_H

#include "note_config.h"

/* --------------------------------------------------------------------------
 * nchar — the character type the native text control speaks.
 * Windows controls are UTF-16, GTK/AppKit are UTF-8, so the core is written
 * once against nchar and N("literal") rather than picking a side.
 * -------------------------------------------------------------------------- */
#ifdef _WIN32
  typedef unsigned short nchar;
  #define N(s) L##s
  #define NOTE_NCHAR_UTF16 1
#else
  typedef char nchar;
  #define N(s) s
  #define NOTE_NCHAR_UTF16 0
#endif

/* --------------------------------------------------------------------------
 * Commands.  One id per user-visible action; the same ids drive the menu bar,
 * the context menu and the accelerator table, so a backend never invents its
 * own numbering.
 * -------------------------------------------------------------------------- */
enum {
    CMD_NONE = 0,

    CMD_FILE_NEW = 0x100,
    CMD_FILE_OPEN,
    CMD_FILE_SAVE,
    CMD_FILE_SAVEAS,
    CMD_FILE_CLOSE,
    CMD_FILE_PAGESETUP,
    CMD_FILE_PRINT,
    CMD_FILE_EXIT,
    /* Renaming the file the active tab is editing.  It sits beside Save As in
     * the menu but at the end of the block here, so adding it does not
     * renumber the ids a build already in flight is using. */
    CMD_FILE_RENAME,

    CMD_EDIT_UNDO = 0x200,
    CMD_EDIT_REDO,
    CMD_EDIT_CUT,
    CMD_EDIT_COPY,
    CMD_EDIT_PASTE,
    CMD_EDIT_DELETE,
    CMD_EDIT_FIND,
    CMD_EDIT_FINDNEXT,
    CMD_EDIT_FINDPREV,
    CMD_EDIT_REPLACE,
    CMD_EDIT_GOTO,
    CMD_EDIT_SELALL,
    CMD_EDIT_TIMEDATE,

    CMD_FMT_WRAP = 0x300,
    CMD_FMT_FONT,
    CMD_FMT_FONTDLG,   /* size and style, in the platform's own font dialog */

    CMD_VIEW_STATUS = 0x400,
    CMD_VIEW_LINENUM,
    CMD_VIEW_SYNTAX,
    CMD_VIEW_ZOOMIN,
    CMD_VIEW_ZOOMOUT,
    CMD_VIEW_ZOOMRESET,
    CMD_VIEW_THEME_SYSTEM,
    CMD_VIEW_THEME_LIGHT,
    CMD_VIEW_THEME_DARK,
    CMD_VIEW_THEME_PICK,
    CMD_VIEW_PALETTE,   /* opens the command palette; a backend must own it */

    CMD_TAB_NEXT = 0x480,
    CMD_TAB_PREV,

    CMD_HELP_ABOUT = 0x500
};

/* Menu model ------------------------------------------------------------- */
enum {
    MI_ITEM = 0,    /* plain command item                     */
    MI_SEP,         /* separator                              */
    MI_CHECK,       /* checkable item (state comes from core) */
    MI_RADIO,       /* one-of-N item (state comes from core)  */
    MI_POPUP,       /* begins a drop-down; label is its name  */
    MI_END,         /* ends a drop-down / ends the table      */
    /* A real command that the menu bar does not draw.  It still belongs to
     * the menu the model puts it in, so the command palette and any other
     * reader of the model finds it there; only a backend building menus
     * skips it.  This is how a command leaves a crowded menu without
     * leaving the editor. */
    MI_HIDDEN
};

typedef struct {
    unsigned short id;      /* CMD_*, or 0 for MI_SEP / MI_POPUP / MI_END */
    unsigned char  kind;    /* MI_*                                       */
    const nchar   *label;   /* "&Open...\tCtrl+O"                         */
} note_menu_item;

/* The whole menu bar, terminated by an MI_END with a NULL label. */
extern const note_menu_item note_menu[];
/* The right-click menu, same encoding. */
extern const note_menu_item note_ctxmenu[];

/* Accelerators ----------------------------------------------------------- */
enum { ACC_CTRL = 1, ACC_SHIFT = 2, ACC_ALT = 4 };

typedef struct {
    unsigned short id;      /* CMD_*                    */
    unsigned char  mods;    /* ACC_* bitmask            */
    unsigned short key;     /* 'A'..'Z', '0'..'9' or VK */
} note_accel;

extern const note_accel note_accels[];
extern const int        note_accel_count;

/* Find flags ------------------------------------------------------------- */
enum {
    FIND_DOWN      = 1,
    FIND_MATCHCASE = 2,
    FIND_WHOLEWORD = 4
};

/* Line endings and encodings the core round-trips. */
enum { EOL_CRLF = 0, EOL_LF, EOL_CR };
enum { ENC_UTF8 = 0, ENC_UTF8_BOM, ENC_UTF16LE, ENC_UTF16BE };

/* Theme preference (what the user picked) vs. what is actually shown. */
enum { THEME_SYSTEM = 0, THEME_LIGHT, THEME_DARK };

/* Answers from ops->ask_save */
enum { ASK_YES = 1, ASK_NO = 0, ASK_CANCEL = -1 };

/* Lists only the backend can enumerate, asked for through ops->pick.  The
 * core names the list and stops there: the choosing is asynchronous and the
 * backend applies the result itself, which is what lets one backend answer
 * with a dialog and another — as the Win32 one does — with a mode of its
 * command palette. */
enum { PICK_THEME = 0, PICK_FONT, PICK_LINE, PICK_RENAME };

typedef struct note_host note_host;   /* opaque, owned by the backend */
typedef struct note_app  note_app;

/* --------------------------------------------------------------------------
 * One open document.  The backend keeps a native editor per document and
 * addresses them by the same index the core uses.
 * -------------------------------------------------------------------------- */
typedef struct {
    nchar    path[NOTE_PATH_MAX];  /* empty string => never saved */
    int      dirty;
    int      eol;                  /* EOL_*  — preserved across load/save */
    int      encoding;             /* ENC_*  — preserved across load/save */
    int      lang;                 /* NOTE_LANG_*, from the extension     */
    int      untitled_no;          /* 1,2,3… for unsaved documents        */
    unsigned id;                   /* stable id; names the session blob   */
} note_doc;

/* --------------------------------------------------------------------------
 * What the core needs from a platform.  A backend is exactly this table plus
 * a message loop; nothing in the core knows how any of it is implemented.
 * Wherever a call takes `doc`, it is an index into note_app.docs.
 * -------------------------------------------------------------------------- */
typedef struct {
    /* memory */
    void *(*alloc)(note_host *, unsigned long bytes);
    void  (*free) (note_host *, void *p);

    /* the native text control of one document */
    int   (*text_len)   (note_host *, int doc);                       /* nchars */
    int   (*text_get)   (note_host *, int doc, nchar *buf, int cap);
    void  (*text_set)   (note_host *, int doc, const nchar *s);
    void  (*sel_get)    (note_host *, int *from, int *to);   /* active doc */
    void  (*sel_set)    (note_host *, int from, int to);
    void  (*sel_replace)(note_host *, const nchar *s);
    void  (*edit_op)    (note_host *, int cmd);   /* undo/redo/cut/copy/... */
    int   (*can_undo)   (note_host *);
    void  (*set_modified)(note_host *, int doc, int modified);

    /* tabs */
    int   (*tab_create) (note_host *, int doc);   /* make an editor; 1 = ok */
    void  (*tab_destroy)(note_host *, int doc);
    void  (*tab_select) (note_host *, int doc);
    void  (*tab_title)  (note_host *, int doc, const nchar *title);

    /* chrome */
    void  (*set_title)   (note_host *, const nchar *s);
    void  (*set_status)  (note_host *, const nchar *s);
    void  (*show_status) (note_host *, int visible);
    void  (*set_wrap)    (note_host *, int wrap);
    void  (*set_linenums)(note_host *, int on);
    void  (*set_zoom)    (note_host *, int percent);
    void  (*set_theme)   (note_host *, int theme);   /* registry index */
    void  (*rehighlight) (note_host *);

    /* dialogs — always the platform's own, never drawn by the core */
    int   (*dlg_open)     (note_host *, nchar *path, int cap);
    int   (*dlg_save)     (note_host *, nchar *path, int cap);
    void  (*dlg_font)     (note_host *);   /* size and style */
    void  (*dlg_find)     (note_host *, int replace);
    void  (*pick)         (note_host *, int what);   /* PICK_* */
    void  (*dlg_pagesetup)(note_host *);
    void  (*dlg_print)    (note_host *);
    int   (*ask_save)     (note_host *, const nchar *name);  /* ASK_* */
    void  (*message)      (note_host *, const nchar *text, const nchar *title);

    /* services */
    int   (*find_text)  (note_host *, const nchar *needle, unsigned flags);
    void  (*goto_line)  (note_host *, int line);
    int   (*time_date)  (note_host *, nchar *buf, int cap);
    int   (*system_dark)(note_host *);            /* 1 if the OS is in dark */

    /* Where definition files and the saved session live. */
    int   (*state_dir)  (note_host *, nchar *buf, int cap);  /* per-user   */
    int   (*exe_dir)    (note_host *, nchar *buf, int cap);  /* portable   */
    int   (*dir_make)   (note_host *, const nchar *dir);
    /* Full paths of dir\*.ext, NUL separated and double-NUL terminated.
     * Returns how many were written. */
    int   (*dir_list)   (note_host *, const nchar *dir, const nchar *ext,
                         nchar *out, int cap);
    int   (*file_read)  (note_host *, const nchar *path,
                         unsigned char **bytes, unsigned long *len);
    int   (*file_write) (note_host *, const nchar *path,
                         const unsigned char *bytes, unsigned long len);
    void  (*file_delete)(note_host *, const nchar *path);

    /* A definition pack compiled into the executable: 0 for languages, 1
     * for themes.  Returns 1 and a writable, NUL-terminated buffer the core
     * frees, or 0 if this build embeds none.  May be a null pointer on a
     * platform that has no notion of resources. */
    int   (*embedded_pack)(note_host *, int which, nchar **text);
    /* 1 if something is already there.  Rename asks before it moves: a name
     * that would land on an existing file is refused, never overwritten. */
    int   (*file_exists)(note_host *, const nchar *path);
    /* Moves a file, across volumes as well as within one.  1 on success; the
     * core reports the failure and leaves the document alone. */
    int   (*file_rename)(note_host *, const nchar *from, const nchar *to);
    /* A line of text for whatever surface the backend is currently asking a
     * question on — on Win32, the palette's input line.  An empty string or
     * NULL clears it.  This is the only way the core can answer "no, and here
     * is why" without a modal box, which is what a mode that must stay open
     * to be corrected needs. */
    void  (*set_hint)   (note_host *, const nchar *text);
    void  (*quit)       (note_host *);
} note_host_ops;

/* --------------------------------------------------------------------------
 * Application state.  The backend allocates one of these.
 * -------------------------------------------------------------------------- */
struct note_app {
    note_host           *host;
    const note_host_ops *ops;

    note_doc docs[NOTE_MAX_DOCS];
    int      ndocs;
    int      active;
    unsigned next_id;

    int wrap;        /* word wrap on            */
    int status;      /* status bar visible      */
    int linenums;    /* line-number gutter on   */
    int syntax;      /* syntax highlighting on  */
    int zoom;        /* percent, 100 = default  */
    int theme;       /* THEME_* preference      */
    int theme_index; /* a palette chosen by name, or -1 to follow `theme` */
    int dark;        /* what is actually shown  */

    int restoring;   /* set while the session is being reloaded */

    nchar    find[NOTE_FIND_MAX];
    nchar    replace[NOTE_FIND_MAX];
    unsigned find_flags;
};

/* Core API --------------------------------------------------------------- */
void note_init       (note_app *a, note_host *h, const note_host_ops *ops);
int  note_command    (note_app *a, int cmd);   /* 1 = handled */
int  note_menu_check (note_app *a, int cmd);   /* state for MI_CHECK/MI_RADIO */
void note_set_dirty  (note_app *a, int doc, int dirty);
void note_update_title(note_app *a);
int  note_can_close_doc(note_app *a, int doc); /* 1 = ok to discard */
int  note_can_close_all(note_app *a);
int  note_open       (note_app *a, const nchar *path);  /* new tab or focus */
int  note_load       (note_app *a, int doc, const nchar *path);
int  note_save       (note_app *a, int doc, const nchar *path);
int  note_new_doc    (note_app *a);            /* index, or -1 */
/* Renames the file `doc` is editing.  `input` is what the user typed: a bare
 * name renames within the same folder, anything carrying a separator or a
 * drive is honoured as a move.  Returns 1 when the caller may close the input
 * it was typed into, and 0 when it must stay open because the name was
 * refused — the reason is on ops->set_hint by then. */
int  note_rename     (note_app *a, int doc, const nchar *input);
void note_close_doc  (note_app *a, int doc);
void note_select_doc (note_app *a, int doc);
void note_status_at  (note_app *a, int line, int col);
void note_find_again (note_app *a, int backwards);
void note_apply_theme(note_app *a);
/* Pins a palette by registry index, or -1 to go back to following `theme`.
 * How a backend asked the user is its own business; this is the answer. */
void note_set_theme_index(note_app *a, int idx);
void note_doc_title  (note_app *a, int doc, nchar *buf, int cap);

/* Definition files: built-ins first, then <exe>/ and the per-user state dir,
 * each layer overriding the one before it by name. */
void note_defs_load(note_app *a);

/* Decodes on-disk bytes into nchars.  Returns the length written. */
int  note_decode(int enc, const unsigned char *b, unsigned long len,
                 nchar *out, int cap);

/* Session: unsaved work that must survive a crash or a reboot. */
void note_session_save   (note_app *a);
int  note_session_restore(note_app *a);   /* number of documents restored */
void note_session_clear  (note_app *a);

/* Small string helpers the core and backends share (no CRT dependency). */
int  n_len (const nchar *s);
void n_copy(nchar *dst, const nchar *src, int cap);
void n_cat (nchar *dst, const nchar *src, int cap);
int  n_eq  (const nchar *a, const nchar *b);
int  n_utoa(unsigned v, nchar *buf);
const nchar *note_basename(const nchar *path);

#endif /* NOTE_CORE_H */
