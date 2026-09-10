/* note_theme.c — theme registry and the built-in palettes. */

#include "note_theme.h"

/* ==========================================================================
 * Built-in palettes, in the same format a user's .theme file uses.
 * ========================================================================== */

static const char kThemeLight[] = (
    "name = Light\n"
    "dark = no\n"
    "background = #FFFFFF\n"
    "foreground = #1F2328\n"
    "gutter_bg  = #F6F8FA\n"
    "gutter_fg  = #8C959F\n"
    "selection  = #B4D8FE\n"
    "ui_bg      = #F0F0F0\n"
    "ui_fg      = #1F2328\n"
    "keyword    = #0550AE\n"
    "type       = #1B7C83\n"
    "comment    = #6E7781\n"
    "string     = #0A6E3B\n"
    "number     = #953800\n"
    "preproc    = #8250DF\n"
    "operator   = #1F2328\n");

static const char kThemeDark[] = (
    "name = Dark\n"
    "dark = yes\n"
    "background = #1E1E1E\n"
    "foreground = #D4D4D4\n"
    "gutter_bg  = #1E1E1E\n"
    "gutter_fg  = #6E7681\n"
    "selection  = #264F78\n"
    "ui_bg      = #252526\n"
    "ui_fg      = #CCCCCC\n"
    "keyword    = #569CD6\n"
    "type       = #4EC9B0\n"
    "comment    = #6A9955\n"
    "string     = #CE9178\n"
    "number     = #B5CEA8\n"
    "preproc    = #C586C0\n"
    "operator   = #D4D4D4\n");

/* A machine's own screen, written down the way every other theme is.
 *
 * The colours are the VIC-II's, measured rather than idealised -- the same set
 * note_reduce.c carries as note_pal_c64 -- so reducing this theme onto that
 * palette gives back the entries it was written from, and reducing it onto a
 * CGA card gives the nearest thing a PC can say.  It is here rather than in a
 * console backend because a theme is a theme: the retro ports used to keep
 * their own table of hardware indices, and this is the definition that table
 * turned out to be. */
static const char kThemeCommodore[] = (
    "name = Commodore 64\n"
    "dark = yes\n"
    "background = #352879\n"
    "foreground = #6C5EB5\n"
    "gutter_bg  = #352879\n"
    "gutter_fg  = #6C6C6C\n"
    "selection  = #6C5EB5\n"
    "caret      = #6C5EB5\n"
    "ui_bg      = #6C5EB5\n"
    "ui_fg      = #352879\n"
    "keyword    = #FFFFFF\n"
    "type       = #70A4B2\n"
    "comment    = #6C6C6C\n"
    "string     = #9AD284\n"
    "number     = #B8C76F\n"
    "preproc    = #6F4F25\n"
    "operator   = #959595\n");

/* The fallback set, used when no pack is found beside the executable. */
static const char *const note_builtin_themes[] = {
    kThemeLight, kThemeDark, kThemeCommodore, 0
};

/* ==========================================================================
 * Registry
 * ========================================================================== */

static note_theme g_themes[NOTE_MAX_THEMES];
static int        g_nthemes;

int note_theme_count(void) { return g_nthemes; }

const note_theme *note_theme_get(int i)
{
    if (i < 0 || i >= g_nthemes) return &g_themes[0];
    return &g_themes[i];
}

int note_theme_for(int dark)
{
    int i;
    for (i = 0; i < g_nthemes; i++)
        if (g_themes[i].dark == (dark ? 1 : 0)) return i;
    return 0;
}

int note_theme_find(const nchar *name)
{
    int i;
    if (!name || !name[0]) return -1;
    for (i = 0; i < g_nthemes; i++)
        if (g_themes[i].name && n_eq(g_themes[i].name, name)) return i;
    return -1;
}

int note_theme_add(note_arena *ar, const nchar *text)
{
    nchar key[64], val[128];
    const nchar *p = text;
    note_theme T;
    int i, slot = -1;

    for (i = 0; i < (int)sizeof(T); i++) ((unsigned char *)&T)[i] = 0;

    while (note_conf_next(&p, key, 64, val, 128)) {
        if      (n_eq(key, N("name")))       T.name      = note_arena_put(ar, val, -1);
        else if (n_eq(key, N("dark")))       T.dark      = note_conf_bool(val) ? 1 : 0;
        else if (n_eq(key, N("background"))) T.bg        = note_conf_color(val);
        else if (n_eq(key, N("foreground"))) T.fg        = note_conf_color(val);
        else if (n_eq(key, N("gutter_bg")))  T.gutter_bg = note_conf_color(val);
        else if (n_eq(key, N("gutter_fg")))  T.gutter_fg = note_conf_color(val);
        else if (n_eq(key, N("selection")))  T.sel_bg    = note_conf_color(val);
        else if (n_eq(key, N("caret")))      T.caret     = note_conf_color(val);
        else if (n_eq(key, N("ui_bg")))      T.ui_bg     = note_conf_color(val);
        else if (n_eq(key, N("ui_fg")))      T.ui_fg     = note_conf_color(val);
        else if (n_eq(key, N("keyword")))    T.tok[TOK_KEYWORD] = note_conf_color(val);
        else if (n_eq(key, N("type")))       T.tok[TOK_TYPE]    = note_conf_color(val);
        else if (n_eq(key, N("comment")))    T.tok[TOK_COMMENT] = note_conf_color(val);
        else if (n_eq(key, N("string")))     T.tok[TOK_STRING]  = note_conf_color(val);
        else if (n_eq(key, N("number")))     T.tok[TOK_NUMBER]  = note_conf_color(val);
        else if (n_eq(key, N("preproc")))    T.tok[TOK_PREPROC]  = note_conf_color(val);
        else if (n_eq(key, N("operator")))   T.tok[TOK_OPERATOR] = note_conf_color(val);
    }

    if (!T.name) return -1;

    /* Anything the file left out falls back to the plain text colour, so a
     * half-written theme still renders sensibly. */
    T.tok[TOK_TEXT] = T.fg;
    for (i = 1; i < TOK_COUNT; i++)
        if (!T.tok[i]) T.tok[i] = T.fg;
    if (!T.caret) T.caret = T.fg;

    for (i = 0; i < g_nthemes; i++)
        if (g_themes[i].name && n_eq(g_themes[i].name, T.name)) { slot = i; break; }

    if (slot < 0) {
        if (g_nthemes >= NOTE_MAX_THEMES) return -1;
        slot = g_nthemes++;
    }
    g_themes[slot] = T;
    return slot;
}

void note_theme_init(note_arena *ar)
{
    int i;
    g_nthemes = 0;
    for (i = 0; note_builtin_themes[i]; i++)
        note_theme_add(ar, note_syntax_widen(note_builtin_themes[i]));
}
