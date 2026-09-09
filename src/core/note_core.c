/* note_core.c — the portable half of note.
 *
 * No OS headers, no CRT.  Everything the core cannot do itself it asks the
 * backend for through note_host_ops.
 */
#include "note_core.h"
#include "note_conf.h"
#include "note_syntax.h"
#include "note_theme.h"

/* ==========================================================================
 * String helpers (no CRT)
 * ========================================================================== */

int n_len(const nchar *s)
{
    int i = 0;
    if (!s) return 0;
    while (s[i]) i++;
    return i;
}

void n_copy(nchar *dst, const nchar *src, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    if (src) while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

void n_cat(nchar *dst, const nchar *src, int cap)
{
    int i = n_len(dst), j = 0;
    if (!src) return;
    while (src[j] && i < cap - 1) dst[i++] = src[j++];
    dst[i] = 0;
}

int n_eq(const nchar *a, const nchar *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

const nchar *note_basename(const nchar *path)
{
    const nchar *p = path, *last = path;
    for (; *p; p++)
        if (*p == (nchar)'\\' || *p == (nchar)'/') last = p + 1;
    return last;
}

int n_utoa(unsigned v, nchar *buf)
{
    nchar tmp[12];
    int n = 0, i = 0;
    do { tmp[n++] = (nchar)((nchar)'0' + (v % 10)); v /= 10; } while (v);
    while (n) buf[i++] = tmp[--n];
    buf[i] = 0;
    return i;
}

static unsigned n_atou(const nchar **p)
{
    unsigned v = 0;
    while (**p >= (nchar)'0' && **p <= (nchar)'9') {
        v = v * 10 + (unsigned)(*(*p)++ - (nchar)'0');
    }
    return v;
}

/* ==========================================================================
 * Menu model — the shared slice of UX.  Every backend renders this same
 * table with its own native menu API.
 * ========================================================================== */

const note_menu_item note_menu[] = {
    { 0,                     MI_POPUP, N("&File")                       },
    { CMD_FILE_NEW,          MI_ITEM,  N("&New Tab\tCtrl+N")            },
    { CMD_FILE_OPEN,         MI_ITEM,  N("&Open...\tCtrl+O")            },
    { CMD_FILE_SAVE,         MI_ITEM,  N("&Save\tCtrl+S")               },
    { CMD_FILE_SAVEAS,       MI_ITEM,  N("Save &As...\tCtrl+Shift+S")   },
    { CMD_FILE_RENAME,       MI_ITEM,  N("Rena&me...\tF2")              },
    { CMD_FILE_CLOSE,        MI_ITEM,  N("&Close Tab\tCtrl+W")          },
    { 0,                     MI_SEP,   0                                },
    { CMD_FILE_EXIT,         MI_ITEM,  N("E&xit")                       },
    /* Printing is rare enough that it costs more as two lines of every File
     * menu than it saves; the palette reaches it in four keystrokes. */
    { CMD_FILE_PAGESETUP,    MI_HIDDEN, N("Page Setup...")              },
    { CMD_FILE_PRINT,        MI_HIDDEN, N("Print...\tCtrl+P")           },
    { 0,                     MI_END,   0                                },

    { 0,                     MI_POPUP, N("&Edit")                       },
    { CMD_EDIT_UNDO,         MI_ITEM,  N("&Undo\tCtrl+Z")               },
    { CMD_EDIT_REDO,         MI_ITEM,  N("&Redo\tCtrl+Y")               },
    { 0,                     MI_SEP,   0                                },
    { CMD_EDIT_CUT,          MI_ITEM,  N("Cu&t\tCtrl+X")                },
    { CMD_EDIT_COPY,         MI_ITEM,  N("&Copy\tCtrl+C")               },
    { CMD_EDIT_PASTE,        MI_ITEM,  N("&Paste\tCtrl+V")              },
    { CMD_EDIT_DELETE,       MI_ITEM,  N("De&lete\tDel")                },
    { 0,                     MI_SEP,   0                                },
    { CMD_EDIT_FIND,         MI_ITEM,  N("&Find...\tCtrl+F")            },
    { CMD_EDIT_FINDNEXT,     MI_ITEM,  N("Find &Next\tF3")              },
    { CMD_EDIT_FINDPREV,     MI_ITEM,  N("Find Pre&vious\tShift+F3")    },
    { CMD_EDIT_REPLACE,      MI_ITEM,  N("&Replace...\tCtrl+H")         },
    { CMD_EDIT_GOTO,         MI_ITEM,  N("&Go To...\tCtrl+G")           },
    { 0,                     MI_SEP,   0                                },
    { CMD_EDIT_SELALL,       MI_ITEM,  N("Select &All\tCtrl+A")         },
    { CMD_EDIT_TIMEDATE,     MI_ITEM,  N("Time/&Date\tF5")              },
    { 0,                     MI_END,   0                                },

    { 0,                     MI_POPUP, N("F&ormat")                     },
    { CMD_FMT_WRAP,          MI_CHECK, N("&Word Wrap")                  },
    { CMD_FMT_FONT,          MI_ITEM,  N("&Font...")                    },
    /* The face is a list, so the palette shows it; size and style still want
     * the platform's own dialog, which the palette can still reach. */
    { CMD_FMT_FONTDLG,       MI_HIDDEN, N("Font Size and Style...")     },
    { 0,                     MI_END,   0                                },

    { 0,                     MI_POPUP, N("&View")                       },
    { CMD_VIEW_LINENUM,      MI_CHECK, N("&Line Numbers")               },
    { CMD_VIEW_SYNTAX,       MI_CHECK, N("Synta&x Highlighting")        },
    { CMD_VIEW_STATUS,       MI_CHECK, N("&Status Bar")                 },
    { 0,                     MI_SEP,   0                                },
    { CMD_VIEW_THEME_SYSTEM, MI_RADIO, N("Theme: &System")              },
    { CMD_VIEW_THEME_LIGHT,  MI_RADIO, N("Theme: L&ight")               },
    { CMD_VIEW_THEME_DARK,   MI_RADIO, N("Theme: &Dark")                },
    { CMD_VIEW_THEME_PICK,   MI_ITEM,  N("&Choose Theme...")            },
    { 0,                     MI_SEP,   0                                },
    { CMD_VIEW_ZOOMIN,       MI_ITEM,  N("Zoom &In\tCtrl++")            },
    { CMD_VIEW_ZOOMOUT,      MI_ITEM,  N("Zoom &Out\tCtrl+-")           },
    { CMD_VIEW_ZOOMRESET,    MI_ITEM,  N("&Restore Zoom\tCtrl+0")       },
    { 0,                     MI_END,   0                                },

    { 0,                     MI_POPUP, N("&Help")                       },
    { CMD_HELP_ABOUT,        MI_ITEM,  N("&About note")                 },
    { 0,                     MI_END,   0                                },

    { 0,                     MI_END,   0                                }
};

const note_menu_item note_ctxmenu[] = {
    { CMD_EDIT_UNDO,   MI_ITEM, N("&Undo")      },
    { 0,               MI_SEP,  0               },
    { CMD_EDIT_CUT,    MI_ITEM, N("Cu&t")       },
    { CMD_EDIT_COPY,   MI_ITEM, N("&Copy")      },
    { CMD_EDIT_PASTE,  MI_ITEM, N("&Paste")     },
    { CMD_EDIT_DELETE, MI_ITEM, N("De&lete")    },
    { 0,               MI_SEP,  0               },
    { CMD_EDIT_SELALL, MI_ITEM, N("Select &All")},
    { 0,               MI_END,  0               }
};

/* Virtual keys the core refers to by name; backends map these to their own. */
#define NKEY_TAB   0x09
#define NKEY_F2    0x71
#define NKEY_F3    0x72
#define NKEY_F5    0x74
#define NKEY_PLUS  0xBB
#define NKEY_MINUS 0xBD

const note_accel note_accels[] = {
    { CMD_FILE_NEW,       ACC_CTRL,             'N'        },
    { CMD_FILE_OPEN,      ACC_CTRL,             'O'        },
    { CMD_FILE_SAVE,      ACC_CTRL,             'S'        },
    { CMD_FILE_SAVEAS,    ACC_CTRL | ACC_SHIFT, 'S'        },
    { CMD_FILE_CLOSE,     ACC_CTRL,             'W'        },
    { CMD_FILE_RENAME,    0,                    NKEY_F2    },
    { CMD_FILE_PRINT,     ACC_CTRL,             'P'        },
    { CMD_EDIT_UNDO,      ACC_CTRL,             'Z'        },
    { CMD_EDIT_REDO,      ACC_CTRL,             'Y'        },
    { CMD_EDIT_FIND,      ACC_CTRL,             'F'        },
    { CMD_EDIT_REPLACE,   ACC_CTRL,             'H'        },
    { CMD_EDIT_GOTO,      ACC_CTRL,             'G'        },
    { CMD_EDIT_SELALL,    ACC_CTRL,             'A'        },
    { CMD_EDIT_FINDNEXT,  0,                    NKEY_F3    },
    { CMD_EDIT_FINDPREV,  ACC_SHIFT,            NKEY_F3    },
    { CMD_EDIT_TIMEDATE,  0,                    NKEY_F5    },
    { CMD_TAB_NEXT,       ACC_CTRL,             NKEY_TAB   },
    { CMD_TAB_PREV,       ACC_CTRL | ACC_SHIFT, NKEY_TAB   },
    { CMD_VIEW_ZOOMIN,    ACC_CTRL,             NKEY_PLUS  },
    { CMD_VIEW_ZOOMOUT,   ACC_CTRL,             NKEY_MINUS },
    { CMD_VIEW_ZOOMRESET, ACC_CTRL,             '0'        },
    { CMD_VIEW_PALETTE,   ACC_CTRL,             'K'        }
};

const int note_accel_count = (int)(sizeof(note_accels) / sizeof(note_accels[0]));

/* ==========================================================================
 * Text codec.  Bytes on disk are UTF-8/UTF-16 (BOM sniffed and preserved);
 * in memory the core speaks whatever the native control wants.  Line endings
 * are normalised to CRLF for the control and written back in the file's own
 * style.
 * ========================================================================== */

#if NOTE_NCHAR_UTF16
static unsigned nc_next(const nchar **p)
{
    unsigned c = *(*p)++;
    if (c >= 0xD800 && c <= 0xDBFF && **p >= 0xDC00 && **p <= 0xDFFF) {
        unsigned lo = *(*p)++;
        return 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
    }
    return c;
}
static void nc_put(nchar **p, unsigned cp)
{
    if (cp >= 0x10000) {
        cp -= 0x10000;
        *(*p)++ = (nchar)(0xD800 + (cp >> 10));
        *(*p)++ = (nchar)(0xDC00 + (cp & 0x3FF));
    } else {
        *(*p)++ = (nchar)cp;
    }
}
#else
static unsigned nc_next(const nchar **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    unsigned c = *s++;
    int extra = 0;
    if      (c < 0x80) { *p = (const nchar *)s; return c; }
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
    else                         { c = 0xFFFD; }
    while (extra-- && (*s & 0xC0) == 0x80) c = (c << 6) | (*s++ & 0x3F);
    *p = (const nchar *)s;
    return c;
}
static void nc_put(nchar **p, unsigned cp)
{
    unsigned char *d = (unsigned char *)*p;
    if      (cp < 0x80)    { *d++ = (unsigned char)cp; }
    else if (cp < 0x800)   { *d++ = (unsigned char)(0xC0 | (cp >> 6));
                             *d++ = (unsigned char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { *d++ = (unsigned char)(0xE0 | (cp >> 12));
                             *d++ = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                             *d++ = (unsigned char)(0x80 | (cp & 0x3F)); }
    else                   { *d++ = (unsigned char)(0xF0 | (cp >> 18));
                             *d++ = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
                             *d++ = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                             *d++ = (unsigned char)(0x80 | (cp & 0x3F)); }
    *p = (nchar *)d;
}
#endif

static unsigned b_next(const unsigned char **p, const unsigned char *end, int enc)
{
    if (enc == ENC_UTF16LE || enc == ENC_UTF16BE) {
        unsigned c, lo;
        if (*p + 1 >= end) { *p = end; return 0; }
        c = (enc == ENC_UTF16LE) ? (unsigned)((*p)[0] | ((*p)[1] << 8))
                                 : (unsigned)((*p)[1] | ((*p)[0] << 8));
        *p += 2;
        if (c >= 0xD800 && c <= 0xDBFF && *p + 1 < end) {
            lo = (enc == ENC_UTF16LE) ? (unsigned)((*p)[0] | ((*p)[1] << 8))
                                      : (unsigned)((*p)[1] | ((*p)[0] << 8));
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                *p += 2;
                return 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
            }
        }
        return c;
    } else {
        unsigned c = *(*p)++;
        int extra = 0;
        if      (c < 0x80) return c;
        else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
        else return 0xFFFD;
        while (extra-- && *p < end && (**p & 0xC0) == 0x80)
            c = (c << 6) | (*(*p)++ & 0x3F);
        return c;
    }
}

static void b_put(unsigned char **p, unsigned cp, int enc)
{
    if (enc == ENC_UTF16LE || enc == ENC_UTF16BE) {
        unsigned units[2];
        int n = 1, i;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            units[0] = 0xD800 + (cp >> 10);
            units[1] = 0xDC00 + (cp & 0x3FF);
            n = 2;
        } else {
            units[0] = cp;
        }
        for (i = 0; i < n; i++) {
            if (enc == ENC_UTF16LE) {
                *(*p)++ = (unsigned char)(units[i] & 0xFF);
                *(*p)++ = (unsigned char)(units[i] >> 8);
            } else {
                *(*p)++ = (unsigned char)(units[i] >> 8);
                *(*p)++ = (unsigned char)(units[i] & 0xFF);
            }
        }
    } else {
        if      (cp < 0x80)    { *(*p)++ = (unsigned char)cp; }
        else if (cp < 0x800)   { *(*p)++ = (unsigned char)(0xC0 | (cp >> 6));
                                 *(*p)++ = (unsigned char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { *(*p)++ = (unsigned char)(0xE0 | (cp >> 12));
                                 *(*p)++ = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                                 *(*p)++ = (unsigned char)(0x80 | (cp & 0x3F)); }
        else                   { *(*p)++ = (unsigned char)(0xF0 | (cp >> 18));
                                 *(*p)++ = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
                                 *(*p)++ = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                                 *(*p)++ = (unsigned char)(0x80 | (cp & 0x3F)); }
    }
}

int note_decode(int enc, const unsigned char *b, unsigned long len,
                nchar *out, int cap)
{
    const unsigned char *p = b, *end = b + len;
    nchar *w = out, *wend = out + (cap - 4);

    while (p < end && w < wend) {
        unsigned cp = b_next(&p, end, enc);
        if (!cp) continue;
        nc_put(&w, cp);
    }
    *w = 0;
    return (int)(w - out);
}

/* Sniffs a BOM, returning the encoding and advancing past it. */
static int sniff_bom(const unsigned char **p, unsigned long len)
{
    const unsigned char *b = *p;
    if (len >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) { *p += 3; return ENC_UTF8_BOM; }
    if (len >= 2 && b[0] == 0xFF && b[1] == 0xFE) { *p += 2; return ENC_UTF16LE; }
    if (len >= 2 && b[0] == 0xFE && b[1] == 0xFF) { *p += 2; return ENC_UTF16BE; }
    return ENC_UTF8;
}

/* ==========================================================================
 * Documents
 * ========================================================================== */

static void doc_reset(note_doc *d)
{
    int i;
    unsigned char *raw = (unsigned char *)d;
    for (i = 0; i < (int)sizeof(*d); i++) raw[i] = 0;
    d->eol      = EOL_CRLF;
    d->encoding = ENC_UTF8;
    d->lang     = LANG_NONE;
}

static int next_untitled(note_app *a)
{
    int n = 1, i, again = 1;
    while (again) {
        again = 0;
        for (i = 0; i < a->ndocs; i++)
            if (!a->docs[i].path[0] && a->docs[i].untitled_no == n) {
                n++; again = 1; break;
            }
    }
    return n;
}

void note_doc_title(note_app *a, int doc, nchar *buf, int cap)
{
    note_doc *d;
    buf[0] = 0;
    if (doc < 0 || doc >= a->ndocs) return;
    d = &a->docs[doc];

    if (d->dirty) n_cat(buf, N("*"), cap);
    if (d->path[0]) {
        n_cat(buf, note_basename(d->path), cap);
    } else {
        nchar num[12];
        n_cat(buf, N("Untitled"), cap);
        if (d->untitled_no > 1) {
            n_cat(buf, N(" "), cap);
            n_utoa((unsigned)d->untitled_no, num);
            n_cat(buf, num, cap);
        }
    }
}

int note_new_doc(note_app *a)
{
    int i;
    if (a->ndocs >= NOTE_MAX_DOCS) return -1;
    i = a->ndocs;
    doc_reset(&a->docs[i]);
    a->docs[i].id          = ++a->next_id;
    a->docs[i].untitled_no = next_untitled(a);
    a->ndocs++;
    if (!a->ops->tab_create(a->host, i)) { a->ndocs--; return -1; }
    {
        nchar t[NOTE_PATH_MAX];
        note_doc_title(a, i, t, NOTE_PATH_MAX);
        a->ops->tab_title(a->host, i, t);
    }
    return i;
}

void note_select_doc(note_app *a, int doc)
{
    if (doc < 0 || doc >= a->ndocs) return;
    a->active = doc;
    a->ops->tab_select(a->host, doc);
    note_update_title(a);
    a->ops->rehighlight(a->host);
}

void note_close_doc(note_app *a, int doc)
{
    int i;
    if (doc < 0 || doc >= a->ndocs) return;

    a->ops->tab_destroy(a->host, doc);
    for (i = doc; i < a->ndocs - 1; i++) a->docs[i] = a->docs[i + 1];
    a->ndocs--;

    if (a->ndocs == 0) {
        note_new_doc(a);
        a->active = 0;
    } else if (a->active >= a->ndocs) {
        a->active = a->ndocs - 1;
    }
    note_select_doc(a, a->active);
}

static void refresh_tab(note_app *a, int doc)
{
    nchar t[NOTE_PATH_MAX];
    note_doc_title(a, doc, t, NOTE_PATH_MAX);
    a->ops->tab_title(a->host, doc, t);
}

void note_update_title(note_app *a)
{
    nchar title[NOTE_PATH_MAX + 32];
    note_doc_title(a, a->active, title, NOTE_PATH_MAX);
    n_cat(title, N(" - note"), (int)(sizeof(title) / sizeof(nchar)));
    a->ops->set_title(a->host, title);
}

void note_set_dirty(note_app *a, int doc, int dirty)
{
    if (doc < 0 || doc >= a->ndocs) return;
    if (a->docs[doc].dirty == dirty) return;
    a->docs[doc].dirty = dirty;
    refresh_tab(a, doc);
    if (doc == a->active) note_update_title(a);
}

int note_can_close_doc(note_app *a, int doc)
{
    nchar name[NOTE_PATH_MAX];
    int r;

    if (doc < 0 || doc >= a->ndocs || !a->docs[doc].dirty) return 1;

    note_doc_title(a, doc, name, NOTE_PATH_MAX);
    r = a->ops->ask_save(a->host, name[0] == (nchar)'*' ? name + 1 : name);
    if (r == ASK_CANCEL) return 0;
    if (r == ASK_NO)     return 1;

    if (!a->docs[doc].path[0]) {
        nchar path[NOTE_PATH_MAX];
        path[0] = 0;
        if (!a->ops->dlg_save(a->host, path, NOTE_PATH_MAX)) return 0;
        return note_save(a, doc, path);
    }
    return note_save(a, doc, a->docs[doc].path);
}

int note_can_close_all(note_app *a)
{
    int i;
    for (i = 0; i < a->ndocs; i++)
        if (!note_can_close_doc(a, i)) return 0;
    return 1;
}

/* ==========================================================================
 * Load / save
 * ========================================================================== */

int note_load(note_app *a, int doc, const nchar *path)
{
    unsigned char *bytes = 0;
    unsigned long  len = 0;
    const unsigned char *p, *end;
    nchar *text, *w;
    int enc, eol = EOL_CRLF, eol_seen = 0;
    unsigned long cap;
    note_doc *d;

    if (doc < 0 || doc >= a->ndocs) return 0;
    d = &a->docs[doc];

    if (!a->ops->file_read(a->host, path, &bytes, &len)) {
        a->ops->message(a->host, N("Cannot open that file."), N("note"));
        return 0;
    }

    p   = bytes;
    end = bytes + len;
    enc = sniff_bom(&p, len);

    /* Worst case: every byte becomes one nchar, and every bare LF grows to
     * CRLF.  Plus a terminator. */
    cap = (unsigned long)len * 2 + 4;
    text = (nchar *)a->ops->alloc(a->host, cap * sizeof(nchar));
    if (!text) {
        a->ops->free(a->host, bytes);
        a->ops->message(a->host, N("Not enough memory for that file."), N("note"));
        return 0;
    }

    w = text;
    while (p < end) {
        unsigned cp = b_next(&p, end, enc);
        if (cp == '\r') {
            const unsigned char *save = p;
            unsigned nxt = (p < end) ? b_next(&p, end, enc) : 0;
            if (nxt != '\n') { p = save; if (!eol_seen) { eol = EOL_CR; eol_seen = 1; } }
            else if (!eol_seen) { eol = EOL_CRLF; eol_seen = 1; }
            nc_put(&w, '\r'); nc_put(&w, '\n');
        } else if (cp == '\n') {
            if (!eol_seen) { eol = EOL_LF; eol_seen = 1; }
            nc_put(&w, '\r'); nc_put(&w, '\n');
        } else {
            nc_put(&w, cp);
        }
    }
    *w = 0;

    a->ops->free(a->host, bytes);
    a->ops->text_set(a->host, doc, text);
    a->ops->free(a->host, text);

    n_copy(d->path, path, NOTE_PATH_MAX);
    d->encoding = enc;
    d->eol      = eol;
    d->lang     = note_lang_from_path(path);
    d->dirty    = 0;
    a->ops->set_modified(a->host, doc, 0);

    refresh_tab(a, doc);
    if (doc == a->active) {
        note_update_title(a);
        a->ops->rehighlight(a->host);
    }
    return 1;
}

int note_save(note_app *a, int doc, const nchar *path)
{
    note_doc *d;
    int count;
    nchar *text;
    unsigned char *bytes, *b;
    const nchar *r;
    unsigned long cap;
    int ok;

    if (doc < 0 || doc >= a->ndocs) return 0;
    d = &a->docs[doc];
    count = a->ops->text_len(a->host, doc);

    text = (nchar *)a->ops->alloc(a->host, (unsigned long)(count + 2) * sizeof(nchar));
    if (!text) {
        a->ops->message(a->host, N("Not enough memory to save."), N("note"));
        return 0;
    }
    a->ops->text_get(a->host, doc, text, count + 1);

    /* Worst case per nchar: 3 UTF-8 bytes, or 2 UTF-16 bytes; plus a BOM. */
    cap = (unsigned long)(count + 2) * 4 + 4;
    bytes = (unsigned char *)a->ops->alloc(a->host, cap);
    if (!bytes) {
        a->ops->free(a->host, text);
        a->ops->message(a->host, N("Not enough memory to save."), N("note"));
        return 0;
    }

    b = bytes;
    if      (d->encoding == ENC_UTF8_BOM) { *b++ = 0xEF; *b++ = 0xBB; *b++ = 0xBF; }
    else if (d->encoding == ENC_UTF16LE)  { *b++ = 0xFF; *b++ = 0xFE; }
    else if (d->encoding == ENC_UTF16BE)  { *b++ = 0xFE; *b++ = 0xFF; }

    r = text;
    while (*r) {
        unsigned cp = nc_next(&r);
        if (cp == '\r' || cp == '\n') {
            if (cp == '\r' && *r == (nchar)'\n') r++;   /* swallow CRLF's LF */
            if      (d->eol == EOL_CRLF) { b_put(&b, '\r', d->encoding);
                                           b_put(&b, '\n', d->encoding); }
            else if (d->eol == EOL_LF)     b_put(&b, '\n', d->encoding);
            else                           b_put(&b, '\r', d->encoding);
        } else {
            b_put(&b, cp, d->encoding);
        }
    }

    ok = a->ops->file_write(a->host, path, bytes, (unsigned long)(b - bytes));
    a->ops->free(a->host, bytes);
    a->ops->free(a->host, text);

    if (!ok) {
        a->ops->message(a->host, N("Cannot write that file."), N("note"));
        return 0;
    }

    if (path != d->path) {
        n_copy(d->path, path, NOTE_PATH_MAX);
        d->lang = note_lang_from_path(d->path);
        if (doc == a->active) a->ops->rehighlight(a->host);
    }
    d->dirty = 0;
    a->ops->set_modified(a->host, doc, 0);
    refresh_tab(a, doc);
    if (doc == a->active) note_update_title(a);
    return 1;
}

int note_open(note_app *a, const nchar *path)
{
    int i, doc;

    /* Already open?  Just go there. */
    for (i = 0; i < a->ndocs; i++)
        if (a->docs[i].path[0] && n_eq(a->docs[i].path, path)) {
            note_select_doc(a, i);
            return 1;
        }

    /* An untouched Untitled tab is a better home than a new one. */
    doc = -1;
    if (a->ndocs == 1 && !a->docs[0].path[0] && !a->docs[0].dirty &&
        a->ops->text_len(a->host, 0) == 0) {
        doc = 0;
    }
    if (doc < 0) doc = note_new_doc(a);
    if (doc < 0) return 0;

    if (!note_load(a, doc, path)) {
        if (doc > 0) note_close_doc(a, doc);
        return 0;
    }
    note_select_doc(a, doc);
    return 1;
}

/* ==========================================================================
 * Rename
 *
 * The whole of it is here rather than in a backend: what a bare name means,
 * what counts as no change, and the refusal to land on a file that already
 * exists are decisions a second backend would otherwise have to make again,
 * and would make differently.  All the platform is asked for is "is anything
 * there" and "move this".
 * ========================================================================== */

/* ASCII-case-insensitive equality.  Enough to recognise that "Notes.txt" and
 * "notes.TXT" name the same file on a case-folding filesystem, which is the
 * one thing rename has to know before it asks whether the target exists. */
static int n_ieq(const nchar *a, const nchar *b)
{
    for (;;) {
        nchar x = *a++, y = *b++;
        if (x >= (nchar)'A' && x <= (nchar)'Z') x = (nchar)(x + ('a' - 'A'));
        if (y >= (nchar)'A' && y <= (nchar)'Z') y = (nchar)(y + ('a' - 'A'));
        /* Separators are the same separator, so a typed '/' still matches the
         * '\\' the path was stored with. */
        if (x == (nchar)'/') x = (nchar)'\\';
        if (y == (nchar)'/') y = (nchar)'\\';
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* Everything up to and including the last separator, so appending a bare name
 * to it lands in the same folder. */
static void dir_of(const nchar *path, nchar *out, int cap)
{
    const nchar *base = note_basename(path);
    int n = (int)(base - path), i;

    if (n > cap - 1) n = cap - 1;
    for (i = 0; i < n; i++) out[i] = path[i];
    out[i] = 0;
}

/* A name the user meant as a location rather than as a file name: it carries
 * a separator, or it starts with a drive letter. */
static int is_pathy(const nchar *s)
{
    int i;
    if (s[0] && s[1] == (nchar)':') return 1;
    for (i = 0; s[i]; i++)
        if (s[i] == (nchar)'\\' || s[i] == (nchar)'/') return 1;
    return 0;
}

static int is_blank(nchar c)
{
    return c == (nchar)' ' || c == (nchar)'\t';
}

int note_rename(note_app *a, int doc, const nchar *input)
{
    const note_host_ops *o = a->ops;
    note_host *h = a->host;
    nchar name[NOTE_PATH_MAX], target[NOTE_PATH_MAX], old[NOTE_PATH_MAX];
    note_doc *d;
    int i, j, end;

    if (doc < 0 || doc >= a->ndocs) return 1;
    d = &a->docs[doc];

    /* Nothing on disk to move.  Save As is the same question asked of a
     * document that has never had a name, so ask it that way. */
    if (!d->path[0]) { note_command(a, CMD_FILE_SAVEAS); return 1; }

    /* Trim, so that a stray space cannot become part of a file name. */
    i = 0;
    while (input[i] && is_blank(input[i])) i++;
    end = n_len(input);
    while (end > i && is_blank(input[end - 1])) end--;
    for (j = 0; i < end && j < NOTE_PATH_MAX - 1; i++) name[j++] = input[i];
    name[j] = 0;

    /* Empty or whitespace only: the user changed their mind. */
    if (!name[0]) return 1;

    if (is_pathy(name)) {
        n_copy(target, name, NOTE_PATH_MAX);
    } else {
        dir_of(d->path, target, NOTE_PATH_MAX);
        n_cat(target, name, NOTE_PATH_MAX);
    }

    if (n_eq(target, d->path)) return 1;          /* unchanged */

    /* A rename that only changes case names the same file on Windows, so
     * asking whether the target exists would refuse it against itself. */
    if (!n_ieq(target, d->path) && o->file_exists && o->file_exists(h, target)) {
        if (o->set_hint) o->set_hint(h, N("A file with that name already exists."));
        return 0;
    }

    if (!o->file_rename || !o->file_rename(h, d->path, target)) {
        if (o->set_hint) o->set_hint(h, N("Could not rename that file."));
        return 0;
    }

    /* From here the file has moved and the document must catch up.  The
     * buffer is deliberately left alone: a modified document stays modified,
     * because renaming is not saving. */
    n_copy(old, d->path, NOTE_PATH_MAX);

    for (i = 0; i < a->ndocs; i++) {
        /* The same file may be open in more than one tab; every one of them
         * was pointing at a path that no longer exists. */
        if (!a->docs[i].path[0] || !n_eq(a->docs[i].path, old)) continue;
        n_copy(a->docs[i].path, target, NOTE_PATH_MAX);
        a->docs[i].lang = note_lang_from_path(target);
        refresh_tab(a, i);
    }

    if (o->set_hint) o->set_hint(h, N(""));
    note_update_title(a);
    o->rehighlight(h);
    /* So a restart does not bring the old path back from the session index. */
    note_session_save(a);
    return 1;
}

/* ==========================================================================
 * Definition files
 * ========================================================================== */

/* Reads one definition file and hands its text to `add`. */
static void load_defs_from(note_app *a, const nchar *dir, const nchar *ext,
                           int (*add)(note_arena *, const nchar *),
                           note_arena *ar)
{
    nchar *list, *p;
    int n, listcap = NOTE_MAX_DOCS * NOTE_PATH_MAX;

    list = (nchar *)a->ops->alloc(a->host, (unsigned long)listcap * sizeof(nchar));
    if (!list) return;

    n = a->ops->dir_list(a->host, dir, ext, list, listcap);
    if (n <= 0) { a->ops->free(a->host, list); return; }

    for (p = list; *p; p += n_len(p) + 1) {
        unsigned char *bytes = 0;
        unsigned long  len = 0;
        nchar *text;
        const unsigned char *bp;
        int enc, cap;

        if (!a->ops->file_read(a->host, p, &bytes, &len)) continue;

        cap  = (int)len + 8;
        text = (nchar *)a->ops->alloc(a->host, (unsigned long)cap * sizeof(nchar));
        if (text) {
            bp  = bytes;
            enc = sniff_bom(&bp, len);
            note_decode(enc, bp, len - (unsigned long)(bp - bytes), text, cap);
            add(ar, text);
            a->ops->free(a->host, text);
        }
        a->ops->free(a->host, bytes);
    }

    a->ops->free(a->host, list);
}

/* Is this line just a run of dashes?  That is a pack's document separator. */
static int is_pack_sep(const nchar *line, const nchar *end)
{
    int dashes = 0;
    while (line < end && *line == (nchar)'-') { dashes++; line++; }
    while (line < end && (*line == (nchar)' ' || *line == (nchar)'\r')) line++;
    return dashes >= 3 && line == end;
}

/* A pack holds many definitions in one file, separated by a line of dashes.
 * note reads these at every launch, and several hundred individual files
 * would be a visible delay in an editor meant to open instantly. */
/* Splits pack text into its documents and registers each.  The text is
 * modified in place — a separator line is where one document is terminated —
 * so the caller owns a writable copy.  Kept apart from reading a file so the
 * same code serves a pack compiled into the executable. */
static void add_pack_text(nchar *text, int (*add)(note_arena *, const nchar *),
                          note_arena *ar)
{
    nchar *p = text, *seg = 0;

    while (*p) {
        nchar *line = p, *eol;
        while (*p && *p != (nchar)'\n') p++;
        eol = p;
        if (*p) p++;

        if (is_pack_sep(line, eol)) {
            if (seg) { *line = 0; add(ar, seg); }
            seg = p;
        }
    }
    if (seg && *seg) add(ar, seg);
}

static void load_pack(note_app *a, const nchar *path,
                      int (*add)(note_arena *, const nchar *), note_arena *ar)
{
    unsigned char *bytes = 0;
    unsigned long  len = 0;
    const unsigned char *bp;
    nchar *text;
    int enc, cap;

    if (!a->ops->file_read(a->host, path, &bytes, &len)) return;

    cap  = (int)len + 8;
    text = (nchar *)a->ops->alloc(a->host, (unsigned long)cap * sizeof(nchar));
    if (!text) { a->ops->free(a->host, bytes); return; }

    bp  = bytes;
    enc = sniff_bom(&bp, len);
    note_decode(enc, bp, len - (unsigned long)(bp - bytes), text, cap);
    a->ops->free(a->host, bytes);

    add_pack_text(text, add, ar);
    a->ops->free(a->host, text);
}

/* The packs compiled into the executable, so that note.exe on its own is a
 * complete editor.  They load before anything on disk, so a pack or a single
 * definition beside the executable still overrides them by name. */
static void load_embedded(note_app *a, note_arena *ar)
{
    int which;

    if (!a->ops->embedded_pack) return;

    for (which = 0; which < 2; which++) {
        nchar *text = 0;
        if (!a->ops->embedded_pack(a->host, which, &text) || !text) continue;
        add_pack_text(text, which ? note_theme_add : note_syntax_add, ar);
        a->ops->free(a->host, text);
    }
}

/* One arena backs both registries; it outlives them by living here. */
static note_arena g_arena;

void note_defs_load(note_app *a)
{
    nchar dir[NOTE_PATH_MAX];
    int i;

    note_syntax_init(&g_arena);
    note_theme_init(&g_arena);
    load_embedded(a, &g_arena);

    for (i = 0; i < 2; i++) {
        int ok = i ? a->ops->state_dir(a->host, dir, NOTE_PATH_MAX)
                   : a->ops->exe_dir  (a->host, dir, NOTE_PATH_MAX);
        if (!ok) continue;
        {
            nchar sub[NOTE_PATH_MAX];

            /* Packs first, then loose files, so a definition a user drops in
             * beside a pack replaces the pack's copy of the same name. */
            n_copy(sub, dir, NOTE_PATH_MAX);
            n_cat (sub, N("\\syntax.pack"), NOTE_PATH_MAX);
            load_pack(a, sub, note_syntax_add, &g_arena);

            n_copy(sub, dir, NOTE_PATH_MAX);
            n_cat (sub, N("\\themes.pack"), NOTE_PATH_MAX);
            load_pack(a, sub, note_theme_add, &g_arena);

            n_copy(sub, dir, NOTE_PATH_MAX);
            n_cat (sub, N("\\syntax"), NOTE_PATH_MAX);
            load_defs_from(a, sub, N("syntax"), note_syntax_add, &g_arena);

            n_copy(sub, dir, NOTE_PATH_MAX);
            n_cat (sub, N("\\themes"), NOTE_PATH_MAX);
            load_defs_from(a, sub, N("theme"), note_theme_add, &g_arena);
        }
    }
}

/* Encodes an nchar string as UTF-8 into a fresh buffer the caller frees.
 * Line breaks are written as CRLF: these files are meant to be readable in
 * any editor, and in memory a break is whatever the native control uses. */
static unsigned char *to_utf8(note_app *a, const nchar *s, unsigned long *out_len)
{
    int count = n_len(s);
    unsigned char *buf, *b;
    const nchar *r = s;

    buf = (unsigned char *)a->ops->alloc(a->host, (unsigned long)count * 4 + 8);
    if (!buf) return 0;

    b = buf;
    while (*r) {
        unsigned cp = nc_next(&r);
        if (cp == '\r' || cp == '\n') {
            if (cp == '\r' && *r == (nchar)'\n') r++;
            b_put(&b, '\r', ENC_UTF8);
            b_put(&b, '\n', ENC_UTF8);
        } else {
            b_put(&b, cp, ENC_UTF8);
        }
    }
    *out_len = (unsigned long)(b - buf);
    return buf;
}

/* ==========================================================================
 * Session — unsaved work survives a crash, a reboot, or just closing note
 * ========================================================================== */

static void session_path(note_app *a, nchar *buf, int cap,
                         const nchar *leaf, unsigned id)
{
    nchar num[12];
    buf[0] = 0;
    if (!a->ops->state_dir(a->host, buf, cap)) return;
    n_cat(buf, N("\\"), cap);
    if (id) {
        n_cat(buf, N("buf"), cap);
        n_utoa(id, num);
        n_cat(buf, num, cap);
        n_cat(buf, N(".txt"), cap);
    } else {
        n_cat(buf, leaf, cap);
    }
}

void note_session_save(note_app *a)
{
    nchar path[NOTE_PATH_MAX];
    nchar *idx;
    unsigned char *utf8;
    unsigned long ulen;
    int i, cap = (NOTE_PATH_MAX + 96) * NOTE_MAX_DOCS + 128;
    nchar num[12];

    if (!NOTE_ENABLE_SESSION || a->restoring) return;

    idx = (nchar *)a->ops->alloc(a->host, (unsigned long)cap * sizeof(nchar));
    if (!idx) return;

    idx[0] = 0;
    n_cat(idx, N("version = 1\nactive = "), cap);
    n_utoa((unsigned)a->active, num);
    n_cat(idx, num, cap);

    /* Remember the look as well as the documents: a palette that reset on
     * every launch would not be worth choosing. */
    n_cat(idx, N("\nthemepref = "), cap);
    n_utoa((unsigned)a->theme, num);
    n_cat(idx, num, cap);
    if (a->theme_index >= 0 && a->theme_index < note_theme_count()) {
        n_cat(idx, N("\ntheme = "), cap);
        n_cat(idx, note_theme_get(a->theme_index)->name, cap);
    }
    n_cat(idx, N("\n"), cap);

    for (i = 0; i < a->ndocs; i++) {
        note_doc *d = &a->docs[i];

        /* A modified buffer is written out whole; a clean one only needs its
         * path, because the file on disk already holds the text. */
        if (d->dirty) {
            int count = a->ops->text_len(a->host, i);
            nchar *text = (nchar *)a->ops->alloc(
                a->host, (unsigned long)(count + 2) * sizeof(nchar));
            if (text) {
                a->ops->text_get(a->host, i, text, count + 1);
                utf8 = to_utf8(a, text, &ulen);
                if (utf8) {
                    session_path(a, path, NOTE_PATH_MAX, 0, d->id);
                    a->ops->file_write(a->host, path, utf8, ulen);
                    a->ops->free(a->host, utf8);
                }
                a->ops->free(a->host, text);
            }
        } else {
            session_path(a, path, NOTE_PATH_MAX, 0, d->id);
            a->ops->file_delete(a->host, path);
        }

        n_cat(idx, N("doc = "), cap);
        n_utoa(d->id, num);          n_cat(idx, num, cap); n_cat(idx, N("|"), cap);
        n_utoa((unsigned)d->dirty, num); n_cat(idx, num, cap); n_cat(idx, N("|"), cap);
        n_utoa((unsigned)d->eol, num);   n_cat(idx, num, cap); n_cat(idx, N("|"), cap);
        n_utoa((unsigned)d->encoding, num); n_cat(idx, num, cap); n_cat(idx, N("|"), cap);
        n_cat(idx, d->path, cap);
        n_cat(idx, N("\n"), cap);
    }

    utf8 = to_utf8(a, idx, &ulen);
    if (utf8) {
        session_path(a, path, NOTE_PATH_MAX, N("session.idx"), 0);
        a->ops->file_write(a->host, path, utf8, ulen);
        a->ops->free(a->host, utf8);
    }
    a->ops->free(a->host, idx);
}

void note_session_clear(note_app *a)
{
    nchar path[NOTE_PATH_MAX];
    int i;

    for (i = 0; i < a->ndocs; i++) {
        session_path(a, path, NOTE_PATH_MAX, 0, a->docs[i].id);
        a->ops->file_delete(a->host, path);
    }
    session_path(a, path, NOTE_PATH_MAX, N("session.idx"), 0);
    a->ops->file_delete(a->host, path);
}

int note_session_restore(note_app *a)
{
    nchar path[NOTE_PATH_MAX], key[64], val[NOTE_PATH_MAX + 96];
    unsigned char *bytes = 0;
    unsigned long len = 0;
    nchar *text;
    const unsigned char *bp;
    const nchar *p;
    int enc, cap, restored = 0, active = 0, styled = 0;

    if (!NOTE_ENABLE_SESSION) return 0;

    session_path(a, path, NOTE_PATH_MAX, N("session.idx"), 0);
    if (!a->ops->file_read(a->host, path, &bytes, &len)) return 0;

    cap  = (int)len + 8;
    text = (nchar *)a->ops->alloc(a->host, (unsigned long)cap * sizeof(nchar));
    if (!text) { a->ops->free(a->host, bytes); return 0; }

    bp  = bytes;
    enc = sniff_bom(&bp, len);
    note_decode(enc, bp, len - (unsigned long)(bp - bytes), text, cap);
    a->ops->free(a->host, bytes);

    a->restoring = 1;
    p = text;

    while (note_conf_next(&p, key, 64, val, NOTE_PATH_MAX + 96)) {
        const nchar *v = val;
        unsigned id, dirty, eol, encoding;
        int doc;

        if (n_eq(key, N("active"))) { active = (int)n_atou(&v); continue; }
        if (n_eq(key, N("themepref"))) {
            a->theme = (int)n_atou(&v);
            styled = 1;
            continue;
        }
        if (n_eq(key, N("theme"))) {
            /* Matched by name: indices shift as definitions load. */
            a->theme_index = note_theme_find(v);
            styled = 1;
            continue;
        }
        if (!n_eq(key, N("doc"))) continue;

        id       = n_atou(&v); if (*v == (nchar)'|') v++;
        dirty    = n_atou(&v); if (*v == (nchar)'|') v++;
        eol      = n_atou(&v); if (*v == (nchar)'|') v++;
        encoding = n_atou(&v); if (*v == (nchar)'|') v++;

        doc = note_new_doc(a);
        if (doc < 0) break;

        a->docs[doc].id       = id;
        a->docs[doc].eol      = (int)eol;
        a->docs[doc].encoding = (int)encoding;
        n_copy(a->docs[doc].path, v, NOTE_PATH_MAX);
        a->docs[doc].lang = a->docs[doc].path[0]
                          ? note_lang_from_path(a->docs[doc].path) : LANG_NONE;
        if (id > a->next_id) a->next_id = id;

        if (dirty) {
            /* The buffer as it was, not what is on disk. */
            unsigned char *bb = 0;
            unsigned long  bl = 0;
            session_path(a, path, NOTE_PATH_MAX, 0, id);
            if (a->ops->file_read(a->host, path, &bb, &bl)) {
                int bcap = (int)bl + 8;
                nchar *bt = (nchar *)a->ops->alloc(
                    a->host, (unsigned long)bcap * sizeof(nchar));
                if (bt) {
                    const unsigned char *q = bb;
                    int be = sniff_bom(&q, bl);
                    note_decode(be, q, bl - (unsigned long)(q - bb), bt, bcap);
                    a->ops->text_set(a->host, doc, bt);
                    a->ops->free(a->host, bt);
                }
                a->ops->free(a->host, bb);
                a->docs[doc].dirty = 1;
                a->ops->set_modified(a->host, doc, 1);
            }
        } else if (a->docs[doc].path[0]) {
            note_load(a, doc, a->docs[doc].path);
        }

        refresh_tab(a, doc);
        restored++;
    }

    a->ops->free(a->host, text);
    a->restoring = 0;

    if (styled) note_apply_theme(a);

    if (restored) {
        if (active < 0 || active >= a->ndocs) active = 0;
        note_select_doc(a, active);
    }
    return restored;
}

/* ==========================================================================
 * Find, status bar, theme
 * ========================================================================== */

void note_find_again(note_app *a, int backwards)
{
    unsigned flags;
    if (!a->find[0]) { a->ops->dlg_find(a->host, 0); return; }
    flags = a->find_flags;
    if (backwards) flags &= ~(unsigned)FIND_DOWN;
    else           flags |=  (unsigned)FIND_DOWN;
    if (!a->ops->find_text(a->host, a->find, flags))
        a->ops->message(a->host, N("Cannot find that text."), N("note"));
}

void note_status_at(note_app *a, int line, int col)
{
    static const nchar *const kEol[] = { N("CRLF"), N("LF"), N("CR") };
    static const nchar *const kEnc[] = { N("UTF-8"), N("UTF-8 BOM"),
                                         N("UTF-16 LE"), N("UTF-16 BE") };
    nchar s[128], num[12];
    note_doc *d = &a->docs[a->active];

    s[0] = 0;
    n_cat(s, N("Ln "), 128);
    n_utoa((unsigned)line, num); n_cat(s, num, 128);
    n_cat(s, N(", Col "), 128);
    n_utoa((unsigned)col, num);  n_cat(s, num, 128);

    n_cat(s, N("      "), 128);
    n_cat(s, kEnc[d->encoding & 3], 128);
    n_cat(s, N("      "), 128);
    n_cat(s, kEol[d->eol % 3], 128);

    if (a->syntax && d->lang != LANG_NONE) {
        n_cat(s, N("      "), 128);
        n_cat(s, note_lang_get(d->lang)->name, 128);
    }

    a->ops->set_status(a->host, s);
}

void note_apply_theme(note_app *a)
{
    int idx = a->theme_index;

    if (idx < 0 || idx >= note_theme_count()) {
        int dark;
        if      (a->theme == THEME_LIGHT) dark = 0;
        else if (a->theme == THEME_DARK)  dark = 1;
        else                              dark = a->ops->system_dark(a->host);
        idx = note_theme_for(dark);
    }

    a->dark = note_theme_get(idx)->dark;
    a->ops->set_theme(a->host, idx);
    a->ops->rehighlight(a->host);
}

void note_set_theme_index(note_app *a, int idx)
{
    a->theme_index = (idx >= 0 && idx < note_theme_count()) ? idx : -1;
    note_apply_theme(a);
}

/* ==========================================================================
 * Init and dispatch
 * ========================================================================== */

void note_init(note_app *a, note_host *h, const note_host_ops *ops)
{
    int i;
    unsigned char *raw = (unsigned char *)a;
    for (i = 0; i < (int)sizeof(*a); i++) raw[i] = 0;

    a->host   = h;
    a->ops    = ops;
    a->wrap   = 1;
    a->status = 1;
    a->syntax = 1;
    a->linenums = 1;
    a->zoom   = 100;
    a->theme  = THEME_SYSTEM;
    a->theme_index = -1;
    a->find_flags = FIND_DOWN;
}

int note_menu_check(note_app *a, int cmd)
{
    switch (cmd) {
    case CMD_FMT_WRAP:          return a->wrap;
    case CMD_VIEW_STATUS:       return a->status;
    case CMD_VIEW_LINENUM:      return a->linenums;
    case CMD_VIEW_SYNTAX:       return a->syntax;
    case CMD_VIEW_THEME_SYSTEM: return a->theme_index < 0 && a->theme == THEME_SYSTEM;
    case CMD_VIEW_THEME_LIGHT:  return a->theme_index < 0 && a->theme == THEME_LIGHT;
    case CMD_VIEW_THEME_DARK:   return a->theme_index < 0 && a->theme == THEME_DARK;
    }
    return 0;
}

int note_command(note_app *a, int cmd)
{
    const note_host_ops *o = a->ops;
    note_host *h = a->host;

    switch (cmd) {

    case CMD_FILE_NEW: {
        int doc = note_new_doc(a);
        if (doc >= 0) note_select_doc(a, doc);
        return 1;
    }

    case CMD_FILE_OPEN: {
        nchar path[NOTE_PATH_MAX];
        path[0] = 0;
        if (o->dlg_open(h, path, NOTE_PATH_MAX)) note_open(a, path);
        return 1;
    }

    case CMD_FILE_SAVE:
        if (!a->docs[a->active].path[0]) return note_command(a, CMD_FILE_SAVEAS);
        return note_save(a, a->active, a->docs[a->active].path);

    case CMD_FILE_SAVEAS: {
        nchar path[NOTE_PATH_MAX];
        n_copy(path, a->docs[a->active].path, NOTE_PATH_MAX);
        if (!o->dlg_save(h, path, NOTE_PATH_MAX)) return 0;
        return note_save(a, a->active, path);
    }

    /* Never saved?  There is no file to move, so this is Save As after all. */
    case CMD_FILE_RENAME:
        if (!a->docs[a->active].path[0]) return note_command(a, CMD_FILE_SAVEAS);
        o->pick(h, PICK_RENAME);
        return 1;

    case CMD_FILE_CLOSE:
        if (note_can_close_doc(a, a->active)) {
            nchar p[NOTE_PATH_MAX];
            session_path(a, p, NOTE_PATH_MAX, 0, a->docs[a->active].id);
            o->file_delete(h, p);
            note_close_doc(a, a->active);
            note_session_save(a);
        }
        return 1;

    case CMD_FILE_PAGESETUP: o->dlg_pagesetup(h); return 1;
    case CMD_FILE_PRINT:     o->dlg_print(h);     return 1;

    case CMD_FILE_EXIT:
        /* Nothing is lost on exit: the session keeps every unsaved buffer. */
        note_session_save(a);
        o->quit(h);
        return 1;

    case CMD_TAB_NEXT:
        if (a->ndocs > 1) note_select_doc(a, (a->active + 1) % a->ndocs);
        return 1;

    case CMD_TAB_PREV:
        if (a->ndocs > 1) note_select_doc(a, (a->active + a->ndocs - 1) % a->ndocs);
        return 1;

    case CMD_EDIT_UNDO:
    case CMD_EDIT_REDO:
    case CMD_EDIT_CUT:
    case CMD_EDIT_COPY:
    case CMD_EDIT_PASTE:
    case CMD_EDIT_DELETE:
        o->edit_op(h, cmd);
        return 1;

    case CMD_EDIT_SELALL:
        o->sel_set(h, 0, -1);
        return 1;

    case CMD_EDIT_TIMEDATE: {
        nchar buf[64];
        if (o->time_date(h, buf, 64)) o->sel_replace(h, buf);
        return 1;
    }

    case CMD_EDIT_FIND:     o->dlg_find(h, 0); return 1;
    case CMD_EDIT_REPLACE:  o->dlg_find(h, 1); return 1;
    case CMD_EDIT_FINDNEXT: note_find_again(a, 0); return 1;
    case CMD_EDIT_FINDPREV: note_find_again(a, 1); return 1;
    case CMD_EDIT_GOTO:     o->pick(h, PICK_LINE); return 1;

    case CMD_FMT_WRAP:
        a->wrap = !a->wrap;
        o->set_wrap(h, a->wrap);
        return 1;

    case CMD_FMT_FONT:
        o->pick(h, PICK_FONT);
        return 1;

    case CMD_FMT_FONTDLG:
        o->dlg_font(h);
        return 1;

    case CMD_VIEW_STATUS:
        a->status = !a->status;
        o->show_status(h, a->status);
        return 1;

    case CMD_VIEW_LINENUM:
        a->linenums = !a->linenums;
        o->set_linenums(h, a->linenums);
        return 1;

    case CMD_VIEW_SYNTAX:
        a->syntax = !a->syntax;
        o->rehighlight(h);
        return 1;

    /* The three presets follow the system or force a side; picking a
     * palette by name pins it until one of them is chosen again. */
    case CMD_VIEW_THEME_SYSTEM:
        a->theme = THEME_SYSTEM; a->theme_index = -1; note_apply_theme(a); return 1;
    case CMD_VIEW_THEME_LIGHT:
        a->theme = THEME_LIGHT;  a->theme_index = -1; note_apply_theme(a); return 1;
    case CMD_VIEW_THEME_DARK:
        a->theme = THEME_DARK;   a->theme_index = -1; note_apply_theme(a); return 1;

    case CMD_VIEW_THEME_PICK:
        o->pick(h, PICK_THEME);
        return 1;

    case CMD_VIEW_ZOOMIN:
        if (a->zoom < 500) a->zoom += 10;
        o->set_zoom(h, a->zoom);
        return 1;

    case CMD_VIEW_ZOOMOUT:
        if (a->zoom > 20) a->zoom -= 10;
        o->set_zoom(h, a->zoom);
        return 1;

    case CMD_VIEW_ZOOMRESET:
        a->zoom = 100;
        o->set_zoom(h, a->zoom);
        return 1;

    case CMD_HELP_ABOUT:
        o->message(h,
            N("note — a small, fast text editor.\r\n\r\n")
            N("A portable C core with native platform backends.\r\n")
            N("No runtime, no installer, one executable.\r\n\r\n")
            N("Languages and themes are plain text files. Drop one into\r\n")
            N("either folder to add or replace it:\r\n\r\n")
            N("    <folder containing note.exe>\\syntax  and  \\themes\r\n")
            N("    %LOCALAPPDATA%\\note\\syntax  and  \\themes"),
            N("About note"));
        return 1;
    }

    return 0;
}
