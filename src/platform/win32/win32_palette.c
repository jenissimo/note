/* win32_palette.c -- the command palette overlay and its list modes
 *
 * Part of note's Win32 backend.  The shared state and the types every part of
 * it needs live in note_win32.h; see there for why the state is a header and
 * not a private static.
 */

#include "note_win32.h"

/* -------------------------------------------------------------------------
 * The command palette
 *
 * An overlay note paints itself rather than a dialog the window manager
 * frames: no caption, no taskbar button, and WS_EX_NOACTIVATE so the editor
 * keeps its active-title look while the palette is up.  Nothing ever focuses
 * it, which is why the message loop is what feeds it keys.
 *
 * What is in the list, how a query filters it and where the matched letters
 * fell is all note_palette.c's; here there is a window and a paint routine.
 *
 * The same overlay is also every list that used to be a dialog.  A mode is a
 * title, something that fills the rows, and two callbacks — one to show a row
 * while it is merely selected, one to keep it — so adding the next list is a
 * table entry rather than another DLGTEMPLATE.
 * ------------------------------------------------------------------------- */

#define PAL_W        520      /* logical px, clamped to the window */
#define PAL_ROW_H     24
#define PAL_INPUT_H   32
#define PAL_ROWS      10      /* rows on screen at once */
#define PAL_PAD        8
#define PAL_TOP       56      /* down from the top of the client area */

/* One mode more than note_win32.h's enum names.  Rename arrived with this
 * change and its enumerator belongs beside the others, but that header is
 * another author's this pass, so the mode is numbered here and reached
 * through pal_open_rename() rather than by name through pal_open_mode(). */
#define PAL_MODE_RENAME  PAL_MODE_COUNT
#define PAL_MODE_RUN    (PAL_MODE_COUNT + 1)
#define PAL_MODE_TOTAL  (PAL_MODE_COUNT + 2)

static note_palette g_pal;

/* Where the pointer was when the overlay last let it choose a row; see
 * WM_MOUSEMOVE in PaletteProc.  Screen coordinates, so that the panel
 * changing size underneath does not read as movement. */
static POINT g_pal_mouse;

void pal_close(note_host *h);

static int pal_rows_shown(void)
{
    int n = note_palette_count(&g_pal);
    return n > PAL_ROWS ? PAL_ROWS : n;
}

/* ---- the modes ----------------------------------------------------------
 *
 * fill() puts the rows in, preview() is called whenever the selection (or,
 * for a mode that reads a number, the query) moves, commit() keeps what is
 * selected and cancel() puts back whatever preview() has been changing.
 * Every one of them may be NULL.
 * ------------------------------------------------------------------------- */

/* Adding a field here means visiting every row of kPalModes, including the
 * ones another author owns.  C fills a short initialiser with zeroes rather
 * than refusing it, so a row nobody updated keeps compiling and quietly means
 * whatever the new field's zero happens to mean -- which is the one kind of
 * merge this file cannot tell you about. */
typedef struct {
    const WCHAR *title;       /* drawn ahead of the query, or NULL           */
    const WCHAR *hint;        /* placeholder while nothing has been typed    */
    const WCHAR *empty;       /* when the query matches nothing              */
    void (*fill)   (note_host *h);
    /* Called before every re-rank, for a mode whose rows depend on the query
     * rather than merely being filtered by it -- a listing of the folder the
     * path names, which changes as the path does. */
    void (*retype) (note_host *h);
    void (*enter)  (note_host *h);                        /* what to restore */
    void (*preview)(note_host *h, const note_pal_row *r);
    /* Returns 1 when the palette is done and may close, 0 to stay open —
     * which is how a rename that would clobber a file reports itself. */
    int  (*commit) (note_host *h, const note_pal_row *r);
    void (*cancel) (note_host *h);
    int   typed;              /* the query is the answer, not a filter       */
    int   digits;             /* ...and it may only be digits                */
    /* The query is a path and the rows are the names in its last folder: the
     * two halves of the query are drawn differently, a row whose id is set is
     * a folder rather than a file, and Tab walks the list. */
    int   paths;
} pal_mode;

/* --- commands --- */

static void pal_fill_cmds(note_host *h)
{
    (void)h;
    note_palette_commands(&g_pal);
}

static int pal_run_cmd(note_host *h, const note_pal_row *r)
{
    if (r && r->id && note_command(&h->app, (int)r->id)) sync_menu(h);
    return 1;
}

/* --- themes --- */

static void pal_fill_theme(note_host *h)
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

static void pal_enter_theme(note_host *h)
{
    h->pal_theme_prev = h->app.theme_index;
}

/* A palette is a thing you look at, so moving the selection paints the editor
 * rather than describing it.  Only this backend's copy changes; the core still
 * holds what the user had chosen, which is what cancelling puts back. */
static void pal_preview_theme(note_host *h, const note_pal_row *r)
{
    if (!r) return;
    h_set_theme(h, (int)r->id);
    service_view(h);
}

static int pal_commit_theme(note_host *h, const note_pal_row *r)
{
    if (r) note_set_theme_index(&h->app, (int)r->id);
    return 1;
}

static void pal_cancel_theme(note_host *h)
{
    /* Including the common case of nothing pinned at all (theme_index < 0,
     * following the system or a light/dark preset). */
    h->app.theme_index = h->pal_theme_prev;
    note_apply_theme(&h->app);
    service_view(h);
}

/* --- fonts --- */

static void pal_set_face(note_host *h, const WCHAR *face)
{
    int i;

    for (i = 0; face[i] && i < LF_FACESIZE - 1; i++) h->font.lfFaceName[i] = face[i];
    h->font.lfFaceName[i] = 0;

    measure_font(h);
    for (i = 0; i < NOTE_MAX_DOCS; i++)
        if (h->d[i].edit) apply_font_to(h->d[i].edit, h);
    service_view(h);
}

static int pal_font_enum(const nchar *face, unsigned pitch_family)
{
    int i;

    /* TMPF_FIXED_PITCH is set for the variable-pitch fonts — the flag is named
     * after the bit, not after what it means.  "@Face" is the same font laid
     * out for vertical CJK text and has no business in an editor. */
    if (pitch_family & TMPF_FIXED_PITCH) return 1;
    if (face[0] == (nchar)'@') return 1;

    /* One family is enumerated once per character set it covers. */
    for (i = 0; i < g_pal.nrows; i++)
        if (n_eq(g_pal.rows[i].label, face)) return 1;

    return note_palette_add_copy(&g_pal, 0, face, 0) ? 1 : 0;
}

static void pal_fill_font(note_host *h)
{
    HDC dc;

    note_palette_reset(&g_pal);

    dc = GetDC(h->wnd);
    os_enum_fonts(dc, pal_font_enum);
    ReleaseDC(h->wnd, dc);

    note_palette_filter(&g_pal);
}

static void pal_enter_font(note_host *h)
{
    int i;
    for (i = 0; i < LF_FACESIZE; i++) h->pal_face_prev[i] = h->font.lfFaceName[i];
}

static void pal_preview_font(note_host *h, const note_pal_row *r)
{
    if (r) pal_set_face(h, (const WCHAR *)r->label);
}

static void pal_cancel_font(note_host *h)
{
    pal_set_face(h, h->pal_face_prev);
}

/* --- go to line --- */

static void pal_enter_line(note_host *h)
{
    int from, to;
    h_sel_get(h, &from, &to);
    h->pal_caret_prev = from;
}

/* Nothing is selected here: the query itself is the answer, so the preview
 * runs on every digit and the row list stays empty. */
static void pal_preview_line(note_host *h, const note_pal_row *r)
{
    unsigned line = note_palette_number(&g_pal);
    (void)r;
    if (line) h_goto_line(h, (int)line);
}

static int pal_commit_line(note_host *h, const note_pal_row *r)
{
    unsigned line = note_palette_number(&g_pal);
    (void)r;
    if (line) h_goto_line(h, (int)line);
    return 1;
}

static void pal_cancel_line(note_host *h)
{
    h_sel_set(h, h->pal_caret_prev, h->pal_caret_prev);
    edit_show_caret(h);
}

static void pal_fill_none(note_host *h)
{
    (void)h;
    note_palette_reset(&g_pal);
    note_palette_filter(&g_pal);
}

/* --- the open tabs ---
 *
 * Shift+Shift is "where do I go", so it lists the documents rather than the
 * commands.  note has no project to search yet; when it has one, this mode
 * gains its files and nothing else about it changes. */

/* The folder a document is in, which is what tells two main.c apart. */
static void folder_of(const nchar *path, nchar *out, int cap)
{
    int end, start;

    out[0] = 0;
    end = n_len(path);
    while (end > 0 && path[end - 1] != (nchar)'\\' && path[end - 1] != (nchar)'/')
        end--;
    if (end <= 1) return;                       /* no folder, or the root */
    end--;                                      /* off the separator */
    start = end;
    while (start > 0 && path[start - 1] != (nchar)'\\' && path[start - 1] != (nchar)'/')
        start--;

    {
        int i = 0;
        while (start + i < end && i < cap - 1) { out[i] = path[start + i]; i++; }
        out[i] = 0;
    }
}

static void pal_fill_tabs(note_host *h)
{
    int i;

    note_palette_reset(&g_pal);
    for (i = 0; i < h->app.ndocs; i++) {
        nchar label[NOTE_PALETTE_LABEL], folder[NOTE_PALETTE_ACCEL];
        /* The core's own tab title, so a modified document is starred here
         * exactly as it is on the strip. */
        note_doc_title(&h->app, i, label, NOTE_PALETTE_LABEL);
        folder_of(h->app.docs[i].path, folder, NOTE_PALETTE_ACCEL);
        note_palette_add_copy(&g_pal, (unsigned)i, label, folder);
    }
    note_palette_filter(&g_pal);
}

static int pal_commit_tab(note_host *h, const note_pal_row *r)
{
    if (r) note_select_doc(&h->app, (int)r->id);
    return 1;
}

/* --- rename ---
 *
 * The name is the answer, not a filter, so there are no rows: the mode is the
 * input line with the file's own name already in it.  Everything about what a
 * typed name means — bare name or move, unchanged, already taken — is
 * note_rename()'s, so a second backend gets the same rules for free. */

static void pal_fill_rename(note_host *h)
{
    const nchar *base = note_basename(h->app.docs[h->app.active].path);
    int i, stem = n_len(base);

    note_palette_reset(&g_pal);

    /* Selected as far as the last dot, so typing straight away replaces the
     * stem and keeps the extension.  A leading dot is the whole name of a
     * dotfile rather than the start of an extension, hence i > 0. */
    for (i = stem - 1; i > 0; i--)
        if (base[i] == (nchar)'.') { stem = i; break; }

    note_palette_set(&g_pal, base, stem);
}

/* Returns what note_rename returns: 0 leaves the overlay up with the reason
 * on the input line, so the name can be corrected rather than retyped. */
static int pal_commit_rename(note_host *h, const note_pal_row *r)
{
    (void)r;
    return note_rename(&h->app, h->app.active, note_palette_query(&g_pal));
}

/* --- run a command ---
 *
 * The smallest thing that makes an editor somewhere you can work: a command
 * line that starts in the folder of the file on screen.  The rows are what has
 * been run before, so the common case -- run the same thing again -- is two
 * keys, and the query is still free text, so a new command needs no ceremony.
 */

#define RUN_HIST 12

static nchar g_run_hist[RUN_HIST][NOTE_PALETTE_LABEL];
static int   g_run_nhist;

static void run_hist_push(const nchar *cmd)
{
    int i, dup = -1;

    if (!cmd || !cmd[0]) return;

    for (i = 0; i < g_run_nhist; i++)
        if (n_eq(g_run_hist[i], cmd)) { dup = i; break; }

    /* A repeat moves to the front rather than adding a second copy: the list
     * is meant to answer "what do I keep running", not "what did I type". */
    if (dup < 0 && g_run_nhist < RUN_HIST) dup = g_run_nhist++;
    if (dup < 0) dup = RUN_HIST - 1;

    for (i = dup; i > 0; i--) n_copy(g_run_hist[i], g_run_hist[i - 1],
                                     NOTE_PALETTE_LABEL);
    n_copy(g_run_hist[0], cmd, NOTE_PALETTE_LABEL);
}

static void pal_fill_run(note_host *h)
{
    int i;

    (void)h;
    note_palette_reset(&g_pal);
    for (i = 0; i < g_run_nhist; i++)
        note_palette_add_copy(&g_pal, (unsigned)i, g_run_hist[i], N("again"));
    note_palette_filter(&g_pal);
}

static int pal_commit_run(note_host *h, const note_pal_row *r)
{
    nchar  cmd[NOTE_PALETTE_LABEL], dir[NOTE_PATH_MAX], line[NOTE_PATH_MAX];
    const nchar        *path = h->app.docs[h->app.active].path;
    int                 i, cut = -1;

    /* Typed text wins; an untouched query means the highlighted row. */
    n_copy(cmd, note_palette_query(&g_pal), NOTE_PALETTE_LABEL);
    if (!cmd[0] && r) n_copy(cmd, r->label, NOTE_PALETTE_LABEL);
    if (!cmd[0]) return 0;

    run_hist_push(cmd);

    /* Running the file as it was two edits ago is the one way this can waste
     * an afternoon, so a named, modified document is written out first. */
    if (path[0] && h->app.docs[h->app.active].dirty)
        note_command(&h->app, CMD_FILE_SAVE);

    for (i = 0; path[i]; i++)
        if (path[i] == (nchar)'\\' || path[i] == (nchar)'/') cut = i;
    if (cut > 0) {
        for (i = 0; i < cut && i < NOTE_PATH_MAX - 1; i++) dir[i] = path[i];
        dir[i] = 0;
    } else {
        dir[0] = 0;
    }

    /* cmd.exe /k, in a console of its own: the output is the point, and a
     * window that closed the moment the command finished would hide it. */
    n_copy(line, N("cmd.exe /k "), NOTE_PATH_MAX);
    n_cat (line, cmd, NOTE_PATH_MAX);

    if (os_run(line, dir)) return 1;

    h_set_hint_text(h, N("Could not start that command"));
    return 0;
}

/* --- open a file by typing its path ---
 *
 * The query is the answer, as in rename, but unlike rename it also names a
 * place -- so the rows are what is in that place.  Everything up to the last
 * separator is the folder, which gets listed; what follows it is the fragment
 * being completed, which is what the rows are filtered on.  That is the whole
 * of the mode: Tab keeps the selected name, a folder brings its own separator
 * with it, and the next listing follows from the query the way the first one
 * did.
 *
 * It does not replace the system dialog on Ctrl+O.  Someone who knows where
 * the file is should not have to go and find it in a tree, and someone who
 * does not should still get the dialog that can search, preview and sort.
 */

static void pal_reveal(note_host *h);      /* further down; the walk scrolls */

static int is_sep(nchar c)
{
    return c == (nchar)'\\' || c == (nchar)'/';
}

/* Where the fragment being completed starts: just past the last separator. */
static int leaf_at(const nchar *q)
{
    int i, at = 0;
    for (i = 0; q[i]; i++) if (is_sep(q[i])) at = i + 1;
    return at;
}

/* The query's folder, with its trailing separator, into `out`. */
static void folder_part(const nchar *q, nchar *out, int cap)
{
    int at = leaf_at(q), i;
    for (i = 0; i < at && i < cap - 1; i++) out[i] = q[i];
    out[i] = 0;
}

static void pal_fill_open(note_host *h)
{
    const nchar     *q = note_palette_query(&g_pal);
    const nchar     *frag;
    nchar            pattern[NOTE_PATH_MAX];
    os_find          find;
    int              at = leaf_at(q);

    note_palette_reset_rows(&g_pal);

    /* While Tab is walking the matches the list belongs to the stem that was
     * typed, not to the name Tab has just completed to -- otherwise the first
     * Tab would leave one row and there would be nothing left to walk.  The
     * filter is switched off for the duration and the stem is applied here
     * instead, which also keeps the rows in the folder's own order. */
    if (h->pal_cycle >= 0) {
        note_palette_filter_from(&g_pal, n_len(q));
        frag = (const nchar *)h->pal_stem;
    } else {
        note_palette_filter_from(&g_pal, at);
        frag = note_palette_filter_text(&g_pal);
    }

    /* Nothing that names a folder yet -- a bare "C" or an empty line.  There
     * is nothing to list, and guessing a drive would be worse than waiting. */
    if (!at) { note_palette_filter(&g_pal); return; }

    folder_part(q, pattern, NOTE_PATH_MAX - 2);
    n_cat(pattern, N("*"), NOTE_PATH_MAX);

    if (os_find_open(pattern, &find)) {
        do {
            nchar         name[NOTE_PALETTE_LABEL];
            unsigned char mark[NOTE_PALETTE_LABEL];
            int           dir = (find.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;

            if (find.name[0] == (nchar)'.' &&
                (!find.name[1] ||
                 (find.name[1] == (nchar)'.' && !find.name[2]))) continue;

            n_copy(name, find.name, NOTE_PALETTE_LABEL);
            /* A folder carries its own separator, so completing to one
             * descends into it rather than stopping on its name. */
            if (dir) n_cat(name, N("\\"), NOTE_PALETTE_LABEL);

            /* Filtered on the way in rather than afterwards: a folder with ten
             * thousand files in it would otherwise fill the row pool long
             * before reaching the name being typed. */
            if (frag[0] &&
                !note_palette_marks(name, frag, mark, NOTE_PALETTE_LABEL))
                continue;

            /* The id says which rows are folders, which is all the difference
             * committing one has to know about. */
            if (!note_palette_add_copy(&g_pal, (unsigned)dir, name, 0)) break;
        } while (os_find_step(&find));
        os_find_close(&find);
    }

    note_palette_filter(&g_pal);

    /* Ranking is done; point the filter back at the last segment so that the
     * input line still draws the folder quietly and the name in the text
     * colour.  Nothing re-ranks until the next keystroke, and that ends the
     * walk anyway. */
    if (h->pal_cycle >= 0) note_palette_filter_from(&g_pal, at);
}

/* Somewhere to start from: the folder of the document on screen, and failing
 * that -- an untitled buffer -- wherever note was started. */
static void pal_enter_open(note_host *h)
{
    nchar seed[NOTE_PATH_MAX];
    const nchar *p = h->app.docs[h->app.active].path;

    if (leaf_at(p) > 0) {
        folder_part(p, seed, NOTE_PATH_MAX);
    } else {
        DWORD n = os_current_dir(seed, NOTE_PATH_MAX);
        if (!n || n >= NOTE_PATH_MAX) seed[0] = 0;
        else if (!is_sep(seed[n - 1])) n_cat(seed, N("\\"), NOTE_PATH_MAX);
    }
    h->pal_stem[0] = 0;
    h->pal_cycle   = -1;
    note_palette_set(&g_pal, seed, 0);
}

/* Replaces the query and relists, which is what both Tab and a folder taken
 * with Enter come down to. */
static void pal_open_goto(note_host *h, const nchar *path)
{
    h->pal_cycle = -1;
    note_palette_set(&g_pal, path, 0);
    pal_fill_open(h);
    h->pal_sel = 0;
    h->pal_top = 0;
    h->pal_msg[0] = 0;
    pal_layout(h);
    if (h->pal) InvalidateRect(h->pal, NULL, FALSE);
}

/* Tab and Shift+Tab: the next name the stem matched, in place of the
 * fragment.  The first press remembers what was typed and takes the best
 * match; every press after that walks the same list, forwards or back, the
 * way a shell's completion does. */
static int pal_open_complete(note_host *h, int step)
{
    const note_pal_row *r;
    nchar path[NOTE_PATH_MAX];
    int   n;

    if (h->pal_cycle < 0) {
        /* What has been typed is the stem from here on. */
        const nchar *q = note_palette_query(&g_pal);
        n_copy((nchar *)h->pal_stem, q + leaf_at(q), NOTE_PATH_MAX);
        h->pal_cycle = 0;
        pal_fill_open(h);            /* the same rows, now held by the stem */
        if (step < 0) h->pal_cycle = note_palette_count(&g_pal) - 1;
    } else {
        h->pal_cycle += step;
    }

    n = note_palette_count(&g_pal);
    if (n <= 0) { h->pal_cycle = -1; return 0; }

    /* Round and round: a list of two is quicker to walk than to aim at. */
    while (h->pal_cycle < 0)  h->pal_cycle += n;
    while (h->pal_cycle >= n) h->pal_cycle -= n;

    r = note_palette_at(&g_pal, h->pal_cycle);
    if (!r) { h->pal_cycle = -1; return 0; }

    folder_part(note_palette_query(&g_pal), path, NOTE_PATH_MAX);
    n_cat(path, r->label, NOTE_PATH_MAX);

    note_palette_set(&g_pal, path, 0);
    pal_fill_open(h);
    h->pal_sel = h->pal_cycle;
    h->pal_msg[0] = 0;
    pal_reveal(h);
    pal_layout(h);
    if (h->pal) InvalidateRect(h->pal, NULL, FALSE);
    return 1;
}

static int pal_commit_open(note_host *h, const note_pal_row *r)
{
    nchar path[NOTE_PATH_MAX];
    DWORD attr;

    n_copy(path, note_palette_query(&g_pal), NOTE_PATH_MAX);
    attr = os_file_attrs(path);

    /* A file, exactly as typed: that is the answer. */
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        note_open(&h->app, path);
        return 1;
    }

    /* Otherwise Enter does what Tab does, so the same key that got here keeps
     * getting there: take the selected row and stay open. */
    if (r && pal_open_complete(h, 0)) return 0;

    /* A folder with nothing in it worth showing still deserves its separator,
     * so that the next thing typed is a name inside it. */
    if (attr != INVALID_FILE_ATTRIBUTES) {
        if (!is_sep(path[n_len(path) - 1])) {
            n_cat(path, N("\\"), NOTE_PATH_MAX);
            pal_open_goto(h, path);
        }
        return 0;
    }

    h_set_hint_text(h, N("No such file"));
    return 0;
}

/* One row per mode, in the order the enum names them; the array bound is what
 * catches a mode added without a row.  Nothing catches a row missing a field
 * -- see the note on pal_mode. */
static const pal_mode kPalModes[PAL_MODE_TOTAL] = {
    { NULL,       L"Type a command",  L"No matching command",
      pal_fill_cmds,  NULL,            NULL,            NULL,
      pal_run_cmd,    NULL,            0, 0, 0 },

    { L"Theme",   L"Filter palettes", L"No matching palette",
      pal_fill_theme, NULL,            pal_enter_theme, pal_preview_theme,
      pal_commit_theme, pal_cancel_theme, 0, 0, 0 },

    { L"Font",    L"Filter faces",    L"No matching face",
      pal_fill_font,  NULL,            pal_enter_font,  pal_preview_font,
      NULL,           pal_cancel_font, 0, 0, 0 },

    { L"Go to line", L"Line number",  L"Type a line number",
      pal_fill_none,  NULL,            pal_enter_line,  pal_preview_line,
      pal_commit_line, pal_cancel_line, 1, 1, 0 },

    { L"Go to tab", L"Filter open tabs", L"No matching tab",
      pal_fill_tabs,  NULL,            NULL,            NULL,
      pal_commit_tab, NULL,            0, 0, 0 },

    { L"Open",    L"Path to a file",  L"Nothing here by that name",
      pal_fill_open,  pal_fill_open,   pal_enter_open,  NULL,
      pal_commit_open, NULL,           1, 0, 1 },

    { L"Rename",  L"New file name",   L"Enter returns the new name",
      pal_fill_rename, NULL,           NULL,            NULL,
      pal_commit_rename, NULL,         1, 0, 0 },

    { L"Run",     L"Command, from this file's folder", L"Nothing run yet",
      pal_fill_run, NULL,             NULL,            NULL,
      pal_commit_run, NULL,           1, 0, 0 }
};

static const pal_mode *pal_cur(note_host *h)
{
    return &kPalModes[(h->pal_mode >= 0 && h->pal_mode < PAL_MODE_TOTAL)
                      ? h->pal_mode : PAL_MODE_CMDS];
}

/* Near the top and horizontally centred over the editor, growing and
 * shrinking with the number of matches. */

void pal_layout(note_host *h)
{
    RECT  rc;
    POINT o;
    int   w, ht;

    if (!h->pal) return;

    GetClientRect(h->wnd, &rc);
    o.x = 0; o.y = 0;
    ClientToScreen(h->wnd, &o);

    w = px(PAL_W);
    if (w > rc.right - px(PAL_PAD) * 2) w = rc.right - px(PAL_PAD) * 2;
    if (w < px(220)) w = px(220);

    /* An empty list still gets one row's worth of height: that is where the
     * "nothing matches" line goes, and a mode that has no rows at all — go to
     * line — says what it wants there. */
    ht = px(PAL_INPUT_H) + px(PAL_PAD) / 2 +
         (pal_rows_shown() ? pal_rows_shown() : 1) * px(PAL_ROW_H);

    MoveWindow(h->pal, o.x + (rc.right - w) / 2, o.y + px(PAL_TOP), w, ht, TRUE);
}

static void pal_reveal(note_host *h)
{
    int n = note_palette_count(&g_pal);

    if (h->pal_sel >= n) h->pal_sel = n - 1;
    if (h->pal_sel < 0)  h->pal_sel = 0;

    if (h->pal_sel < h->pal_top)             h->pal_top = h->pal_sel;
    if (h->pal_sel >= h->pal_top + PAL_ROWS) h->pal_top = h->pal_sel - PAL_ROWS + 1;
    if (h->pal_top < 0) h->pal_top = 0;
}

/* Draws one label, colouring the characters the filter matched.  Runs of the
 * same colour go out together so the text still kerns as a word rather than
 * as a column of letters.
 *
 * Returns the x it stopped at, so that whatever follows the label -- a
 * toggle's state -- starts where the label ended rather than at a column
 * guessed from the longest one. */
static int pal_draw_label(HDC dc, const nchar *s, const unsigned char *mark,
                          int nmark, int x, int y, int right,
                          COLORREF plain, COLORREF hit)
{
    int i = 0, m = 0, len = n_len(s);

    while (i < len && x < right) {
        int  on  = (m < nmark && mark[m] == (unsigned char)i);
        int  run = 1;
        SIZE sz;

        if (on) {
            m++;
            while (m < nmark && mark[m] == (unsigned char)(i + run)) { run++; m++; }
        } else {
            while (i + run < len &&
                   !(m < nmark && mark[m] == (unsigned char)(i + run))) run++;
        }

        SetTextColor(dc, on ? hit : plain);
        TextOutW(dc, x, y, (LPCWSTR)(s + i), run);
        GetTextExtentPoint32W(dc, (LPCWSTR)(s + i), run, &sz);
        x += sz.cx;
        i += run;
    }

    return x;
}

/* How readable `want` is on `bg`: the difference in weighted brightness, by
 * the same weights the core reduces a theme with. */
static int pal_contrast(COLORREF bg, COLORREF want)
{
    int a = (GetRValue(bg)   * 77 + GetGValue(bg)   * 151 + GetBValue(bg)   * 28) >> 8;
    int b = (GetRValue(want) * 77 + GetGValue(want) * 151 + GetBValue(want) * 28) >> 8;
    return a > b ? a - b : b - a;
}

/* A colour that can still be read on `bg`.
 *
 * Two colours an author wrote as different can land on the same palette entry
 * once the display has had its say -- note_theme_reduce() picks the nearest
 * entry for each, and chrome_snap() does the same to everything mixed from
 * them afterwards.  A label drawn in the colour of the band underneath it is
 * not dim, it is absent, which is what the selected row was.  So the panel
 * asks for a colour rather than naming one: the theme's, while it reads, and
 * plain black or white -- both in every palette there is -- when it does
 * not. */
static COLORREF pal_legible(COLORREF bg, COLORREF want)
{
    if (pal_contrast(bg, want) >= 48) return want;
    return chrome_snap(pal_contrast(bg, RGB(0, 0, 0)) >
                       pal_contrast(bg, RGB(255, 255, 255))
                       ? RGB(0, 0, 0) : RGB(255, 255, 255));
}

/* The hairline that says where the panel stops.
 *
 * It matters more than it looks: a theme whose ui_bg reduces onto the
 * editor's own background leaves an opaque panel that is the same colour as
 * the file behind it, and with the frame reduced onto that colour as well
 * there is nothing at all to say the list is a surface rather than text
 * floating over the document.  Quiet if it can be, the text colour if it
 * cannot, and failing both whatever is legible. */
static COLORREF pal_edge(note_host *h, COLORREF surface)
{
    COLORREF c = blend_rgb(h->theme.ui_fg, h->theme.ui_bg);

    if (pal_contrast(surface, c) < 48) c = cr(h->theme.ui_fg);
    return pal_legible(surface, c);
}

/* -1 when the command is not a toggle, otherwise the state it is in.  Which
 * commands are checkable comes from note_menu, the table the menu bar is built
 * from, so an entry cannot be a toggle in one list and plain in the other. */
static int pal_check_state(note_host *h, unsigned id)
{
    const note_menu_item *it = note_menu;

    if (h->pal_mode != PAL_MODE_CMDS || !id) return -1;

    while (it->kind == MI_POPUP) {
        for (it++; it->kind != MI_END; it++)
            if ((unsigned)it->id == id &&
                (it->kind == MI_CHECK || it->kind == MI_RADIO))
                return note_menu_check(&h->app, it->id) ? 1 : 0;
        it++;                       /* step past the popup's MI_END */
    }
    return -1;
}

/* What a toggle's state is called on a row.  Words rather than a tick: a
 * drawn mark is a foreground on a background, and on a display that has
 * reduced the theme those two can be the same entry -- the very failure the
 * selected row was showing.  Text goes down the same path the label does and
 * is legible for the same reason.  It is drawn, never matched: the filter
 * still sees "View: Word Wrap" and typing "on" does not select every switch
 * that happens to be on. */
static const nchar *pal_state_text(int on)
{
    return on ? N(" (on)") : N(" (off)");
}

static void pal_paint(note_host *h, HWND wnd)
{
    PAINTSTRUCT ps;
    HDC     dc, mem;
    HBITMAP bmp, oldbmp;
    RECT    rc, band, line;
    HFONT   old;
    HBRUSH  br;
    TEXTMETRICW tm;
    COLORREF surface, edge;
    int     i, rows, y, textx;

    dc = BeginPaint(wnd, &ps);
    GetClientRect(wnd, &rc);

    /* Off-screen: every keystroke repaints the whole panel. */
    mem    = CreateCompatibleDC(dc);
    bmp    = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    oldbmp = (HBITMAP)SelectObject(mem, bmp);

    surface = cr(h->theme.ui_bg);
    edge    = pal_edge(h, surface);

    FillRect(mem, &rc, h->br_ui);

    band = rc;
    band.bottom = px(PAL_INPUT_H);
    FillRect(mem, &band, h->br_edit);

    old = (HFONT)SelectObject(mem, h->menufont);
    os_text_metrics(mem, &tm);
    SetBkMode(mem, TRANSPARENT);
    textx = px(PAL_PAD) + px(4);

    {   /* The query, and a caret of our own: an overlay that never takes
         * focus gets no caret from the system. */
        const pal_mode *m  = pal_cur(h);
        const nchar    *q  = note_palette_query(&g_pal);
        RECT qr = band;
        SIZE sz;

        qr.left  = textx;
        qr.right = rc.right - px(PAL_PAD);
        sz.cx = 0;

        /* A refusal the core wants read — "that name is taken" — sits at the
         * right-hand end of the input line, and the query gives up the room
         * rather than running underneath it. */
        if (h->pal_msg[0]) {
            SIZE ms;
            RECT mr = band;
            int  mlen = n_len((const nchar *)h->pal_msg);

            GetTextExtentPoint32W(mem, h->pal_msg, mlen, &ms);
            mr.left = qr.right - ms.cx;
            if (mr.left < qr.left) mr.left = qr.left;
            SetTextColor(mem, cr(h->theme.tok[TOK_NUMBER]));
            os_draw_text(mem, h->pal_msg, -1, &mr,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            qr.right = mr.left - px(10);
        }

        /* Which list this is, ahead of the query — the overlay is the same
         * window whatever it is showing, so it has to say. */
        if (m->title) {
            SIZE ts;
            SetTextColor(mem, pal_legible(cr(h->theme.bg),
                                          cr(h->theme.tok[TOK_KEYWORD])));
            os_draw_text(mem, m->title, -1, &qr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            GetTextExtentPoint32W(mem, m->title, n_len((const nchar *)m->title), &ts);
            qr.left += ts.cx + px(10);
        }

        if (q[0] && m->paths) {
            /* A path reads as a place and a name, so it is drawn as two: the
             * folder quietly, because it is where the eye has already been,
             * and what is being typed in the text colour. */
            int   cut = note_palette_filter_at(&g_pal);
            int   ty  = qr.top + (qr.bottom - qr.top - tm.tmHeight) / 2;
            int   len = n_len(q);
            SIZE  hs;

            SetTextColor(mem, blend_rgb(h->theme.fg, h->theme.bg));
            TextOutW(mem, qr.left, ty, (LPCWSTR)q, cut);
            GetTextExtentPoint32W(mem, (LPCWSTR)q, cut, &hs);
            SetTextColor(mem, cr(h->theme.fg));
            TextOutW(mem, qr.left + hs.cx, ty, (LPCWSTR)(q + cut), len - cut);
            GetTextExtentPoint32W(mem, (LPCWSTR)q, note_palette_caret(&g_pal), &sz);

        } else if (q[0]) {
            int a, b;

            /* The selection goes down first, in the editor's own selection
             * colour, so the input line reads as one. */
            if (note_palette_sel(&g_pal, &a, &b)) {
                RECT sr;
                SIZE pre, sel;
                GetTextExtentPoint32W(mem, (LPCWSTR)q, a, &pre);
                GetTextExtentPoint32W(mem, (LPCWSTR)(q + a), b - a, &sel);
                sr.left   = qr.left + pre.cx;
                sr.right  = sr.left + sel.cx;
                sr.top    = band.top + px(6);
                sr.bottom = band.bottom - px(6);
                FillRect(mem, &sr, h->br_menusel);
            }

            SetTextColor(mem, cr(h->theme.fg));
            os_draw_text(mem, q, -1, &qr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            /* The caret sits where the caret is, not at the end. */
            GetTextExtentPoint32W(mem, (LPCWSTR)q, note_palette_caret(&g_pal), &sz);
        } else {
            SetTextColor(mem, pal_legible(cr(h->theme.bg),
                                          blend_rgb(h->theme.fg, h->theme.bg)));
            os_draw_text(mem, m->hint, -1, &qr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        line.left   = qr.left + sz.cx + px(1);
        line.right  = line.left + (px(1) < 1 ? 1 : px(1));
        line.top    = band.top + px(7);
        line.bottom = band.bottom - px(7);
        br = CreateSolidBrush(cr(h->theme.fg));
        FillRect(mem, &line, br);
        DeleteObject(br);
    }

    line = band;
    line.top = line.bottom - 1;
    FillRect(mem, &line, h->br_sep);

    rows = pal_rows_shown();
    y    = px(PAL_INPUT_H);

    for (i = 0; i < rows; i++) {
        int idx = h->pal_top + i;
        const note_pal_row *c = note_palette_at(&g_pal, idx);
        unsigned char mark[NOTE_PALETTE_LABEL];
        RECT r;
        COLORREF rbg, plain, dim;
        int  nmark, check, tx, ty, tw;

        if (!c) break;

        r.left = 0; r.right = rc.right;
        r.top  = y; r.bottom = y + px(PAL_ROW_H);
        rbg = surface;
        if (idx == h->pal_sel) {
            FillRect(mem, &r, h->br_menusel);
            rbg = cr(h->theme.sel_bg);
        }

        /* Every colour in the row is asked for against the band it is landing
         * on, not against the one the theme was written for. */
        plain = pal_legible(rbg, (pal_cur(h)->paths && c->id)
                                 ? cr(h->theme.tok[TOK_TYPE])
                                 : cr(h->theme.ui_fg));
        dim   = pal_legible(rbg, blend_rgb(h->theme.ui_fg, h->theme.ui_bg));

        nmark = note_palette_marks(c->label, note_palette_filter_text(&g_pal),
                                   mark, NOTE_PALETTE_LABEL);
        /* In a folder listing a folder is not a file, and saying so in
         * colour saves a column of icons. */
        ty = r.top + (px(PAL_ROW_H) - tm.tmHeight) / 2;
        tw = rc.right - px(96);
        tx = pal_draw_label(mem, c->label, mark, nmark, textx, ty, tw,
                            plain,
                            pal_legible(rbg, cr(h->theme.tok[TOK_KEYWORD])));

        /* A toggle says which way it is set, quietly, just past its name.
         * Read at paint time, so it is the state as it stands and not as it
         * was when the list was built. */
        check = pal_check_state(h, c->id);
        if (check >= 0 && tx < tw) {
            const nchar *st = pal_state_text(check);
            SetTextColor(mem, dim);
            TextOutW(mem, tx, ty, (LPCWSTR)st, n_len(st));
        }

        /* The accelerator on the right, as the menus show it: the palette is
         * meant to teach the shortcut, not to replace it.  A mode's rows are
         * names, not commands, and carry none. */
        if (c->accel && c->accel[0]) {
            RECT ar = r;
            ar.right -= textx;
            SetTextColor(mem, dim);
            os_draw_text(mem, (LPCWSTR)c->accel, -1, &ar,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        y = r.bottom;
    }

    if (!rows) {
        RECT nr = rc;
        nr.top    = px(PAL_INPUT_H);
        nr.bottom = nr.top + px(PAL_ROW_H);
        nr.left   = textx;
        SetTextColor(mem, pal_legible(surface,
                                      blend_rgb(h->theme.ui_fg, h->theme.ui_bg)));
        os_draw_text(mem, pal_cur(h)->empty, -1, &nr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    /* One hairline in the theme's own colours, so the panel reads as a card
     * over the editor without a system frame around it.  It is the last thing
     * that says where the panel stops, so it is the one colour that is never
     * allowed to come out the same as the surface behind it. */
    br = CreateSolidBrush(edge);
    FrameRect(mem, &rc, br);
    DeleteObject(br);

    SelectObject(mem, old);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteDC(mem);

    EndPaint(wnd, &ps);
}

/* Shows the selected row in the editor itself.  A mode that reads a number
 * has no selection: it previews whatever has been typed. */
static void pal_preview(note_host *h)
{
    const pal_mode *m = pal_cur(h);
    if (!m->preview) return;
    m->preview(h, m->typed ? (const note_pal_row *)0
                           : note_palette_at(&g_pal, h->pal_sel));
}

static void pal_refilter(note_host *h)
{
    const pal_mode *m = pal_cur(h);

    /* The complaint was about the text as it stood; it has just changed. */
    h->pal_msg[0] = 0;
    /* And so has the stem Tab was walking. */
    h->pal_cycle = -1;
    if (m->retype) m->retype(h);
    note_palette_filter(&g_pal);
    h->pal_sel = 0;
    h->pal_top = 0;
    pal_layout(h);
    InvalidateRect(h->pal, NULL, FALSE);
    pal_preview(h);
}

/* Hides the overlay without undoing anything: the caller either committed or
 * has already put its preview back. */
static void pal_hide(note_host *h)
{
    if (!h->pal_open) return;
    h->pal_open = 0;
    if (h->pal) ShowWindow(h->pal, SW_HIDE);
}

static void pal_run(note_host *h, int row)
{
    const pal_mode     *m = pal_cur(h);
    const note_pal_row *c = note_palette_at(&g_pal, row);

    if (!m->typed && !c) return;

    /* Hidden first: a command that opens a dialog — or reopens this overlay in
     * another mode — should not do it behind the palette.  A commit that
     * refuses (a name that would clobber a file) brings it straight back. */
    pal_hide(h);
    if (m->commit && !m->commit(h, c)) {
        h->pal_open = 1;
        pal_layout(h);
        ShowWindow(h->pal, SW_SHOWNOACTIVATE);
        InvalidateRect(h->pal, NULL, FALSE);
    }
}

static int pal_row_at(note_host *h, int y)
{
    int row;
    if (y < px(PAL_INPUT_H)) return -1;
    row = h->pal_top + (y - px(PAL_INPUT_H)) / px(PAL_ROW_H);
    if (row < h->pal_top || row >= h->pal_top + PAL_ROWS) return -1;
    if (row >= note_palette_count(&g_pal)) return -1;
    return row;
}

/* The input line draws itself, so it does not get the clipboard for free
 * either.  Both directions go through note_palette, which is where the
 * selection lives. */
static void pal_clip_copy(note_host *h, int cut)
{
    nchar   buf[NOTE_PALETTE_QUERY];
    HGLOBAL mem;
    int     n = note_palette_selected(&g_pal, buf, NOTE_PALETTE_QUERY);

    if (!n || !OpenClipboard(h->wnd)) return;
    EmptyClipboard();
    mem = GlobalAlloc(GMEM_MOVEABLE, (DWORD)(n + 1) * sizeof(WCHAR));
    if (mem) {
        WCHAR *p = (WCHAR *)GlobalLock(mem);
        if (p) {
            n_copy((nchar *)p, buf, n + 1);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
    }
    CloseClipboard();

    if (cut && note_palette_edit(&g_pal, PAL_ED_BACK, 0)) pal_refilter(h);
}

static void pal_clip_paste(note_host *h)
{
    HANDLE  hnd;
    int     changed = 0;

    if (!OpenClipboard(h->wnd)) return;
    hnd = GetClipboardData(CF_UNICODETEXT);
    if (hnd) {
        const WCHAR *s = (const WCHAR *)GlobalLock(hnd);
        if (s) {
            if (pal_cur(h)->digits) {
                /* A line number is a line number however it arrived. */
                nchar only[NOTE_PALETTE_QUERY];
                int i = 0, j = 0;
                while (s[i] && j < NOTE_PALETTE_QUERY - 1)
                    if (s[i] >= L'0' && s[i] <= L'9') only[j++] = (nchar)s[i++];
                    else i++;
                only[j] = 0;
                changed = note_palette_insert(&g_pal, only);
            } else {
                changed = note_palette_insert(&g_pal, (const nchar *)s);
            }
            GlobalUnlock(hnd);
        }
    }
    CloseClipboard();
    if (changed) pal_refilter(h);
}

/* Returns 1 when the key was the palette's; the loop then does not let the
 * accelerator table or the editor see it. */
int pal_key(note_host *h, int vk)
{
    int ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    int shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    int op    = -1;

    /* Tab belongs to the mode that has something to complete, and to nothing
     * else: there is no focus ring here for it to move around. */
    if (vk == VK_TAB) {
        if (pal_cur(h)->paths) pal_open_complete(h, shift ? -1 : 1);
        return 1;
    }

    /* The input line first: Up and Down belong to the list, everything else
     * that an edit field answers belongs to the text. */
    switch (vk) {
    case VK_LEFT:   op = ctrl ? PAL_ED_WORD_LEFT  : PAL_ED_LEFT;  break;
    case VK_RIGHT:  op = ctrl ? PAL_ED_WORD_RIGHT : PAL_ED_RIGHT; break;
    case VK_HOME:   op = PAL_ED_HOME;   break;
    case VK_END:    op = PAL_ED_END;    break;
    case VK_BACK:   op = ctrl ? PAL_ED_BACK_WORD   : PAL_ED_BACK;   break;
    case VK_DELETE: op = ctrl ? PAL_ED_DELETE_WORD : PAL_ED_DELETE; break;
    case 'A':       if (ctrl) op = PAL_ED_ALL;  break;
    case 'Z':       if (ctrl) op = PAL_ED_UNDO; break;
    case 'C':       if (ctrl) { pal_clip_copy(h, 0); return 1; } break;
    case 'X':       if (ctrl) { pal_clip_copy(h, 1); return 1; } break;
    case 'V':       if (ctrl) { pal_clip_paste(h);   return 1; } break;
    default: break;
    }

    if (op >= 0) {
        if (note_palette_edit(&g_pal, op, shift)) pal_refilter(h);
        else InvalidateRect(h->pal, NULL, FALSE);
        return 1;
    }

    switch (vk) {
    /* Esc leaves a mode for the command list it was reached from, and only
     * closes the overlay when there is nothing left to go back to. */
    case VK_ESCAPE:
        if (h->pal_mode != PAL_MODE_CMDS) {
            const pal_mode *m = pal_cur(h);
            if (m->cancel) m->cancel(h);
            pal_open_mode(h, PAL_MODE_CMDS);
        } else {
            pal_hide(h);
        }
        return 1;
    case VK_RETURN: pal_run(h, h->pal_sel);  return 1;
    case VK_UP:     h->pal_sel--;            break;
    case VK_DOWN:   h->pal_sel++;            break;
    case VK_PRIOR:  h->pal_sel -= PAL_ROWS;  break;
    case VK_NEXT:   h->pal_sel += PAL_ROWS;  break;
    default: return 0;
    }

    pal_reveal(h);
    InvalidateRect(h->pal, NULL, FALSE);
    pal_preview(h);
    return 1;
}

void pal_char(note_host *h, unsigned ch)
{
    /* A mode that asks for a line number takes digits and nothing else, so a
     * stray letter cannot leave the query meaning something it does not. */
    if (pal_cur(h)->digits && ch != 8 && (ch < '0' || ch > '9')) return;

    if (note_palette_type(&g_pal, ch)) pal_refilter(h);
}

void pal_open_mode(note_host *h, int mode)
{
    const pal_mode *m;

    if (mode < 0 || mode >= PAL_MODE_TOTAL) mode = PAL_MODE_CMDS;

    /* Whatever the last mode was told to say does not belong to this one. */
    h->pal_msg[0] = 0;

    if (!h->pal) {
        h->pal = os_create_window(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                  N("notePalette"), N(""), WS_POPUP,
                                  0, 0, 10, 10, h->wnd, NULL, h->inst, NULL);
        if (!h->pal) return;
    }

    /* The bar and the palette are two answers to the same question. */
    menubar_show(h, 0);

    /* The list opens at its first row, not at whatever the pointer happens to
     * be resting over: the panel appearing under a still mouse is the window
     * moving, not the mouse. */
    GetCursorPos(&g_pal_mouse);

    h->pal_mode = mode;
    m = pal_cur(h);
    if (m->enter) m->enter(h);
    if (m->fill)  m->fill(h);

    h->pal_sel  = 0;
    h->pal_top  = 0;
    h->pal_open = 1;

    pal_layout(h);
    ShowWindow(h->pal, SW_SHOWNOACTIVATE);
    InvalidateRect(h->pal, NULL, FALSE);
}

void pal_open_path(note_host *h)
{
    pal_open_mode(h, PAL_MODE_OPEN);
}

void pal_show(note_host *h)
{
    if (h->pal_open) return;
    pal_open_mode(h, PAL_MODE_CMDS);
}

/* Named rather than numbered, because PAL_MODE_RENAME is local to this file
 * until the mode enum in note_win32.h gains it. */
void pal_open_rename(note_host *h)
{
    pal_open_mode(h, PAL_MODE_RENAME);
}

void pal_open_run(note_host *h)
{
    pal_open_mode(h, PAL_MODE_RUN);
}

/* Dismissed rather than answered — a click elsewhere, the window losing
 * activation — so whatever the preview was showing goes back. */
void pal_close(note_host *h)
{
    const pal_mode *m;

    if (!h->pal_open) return;
    m = pal_cur(h);
    if (m->cancel) m->cancel(h);
    pal_hide(h);
}

LRESULT CALLBACK PaletteProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_ERASEBKGND:
        return 1;                          /* WM_PAINT covers every pixel */

    case WM_PAINT:
        pal_paint(&g, wnd);
        return 0;

    /* Clicking a row must not pull activation away from the frame. */
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    /* A move that did not move is not a hover.
     *
     * Windows sends WM_MOUSEMOVE whenever the window under a pointer changes
     * out from under it -- shown, moved, restacked -- and not only when the
     * pointer itself has gone somewhere.  Previewing a theme does all of
     * that: every arrow key repaints and relays out the frame, the panel
     * comes back under a pointer that never moved, and the move that follows
     * dragged the selection straight back to whatever row the pointer was
     * resting on.  Down and Up looked dead for as long as the mouse happened
     * to be over the list.  So the row under the pointer is taken only when
     * the pointer has actually been somewhere new since the last time. */
    case WM_MOUSEMOVE: {
        DWORD at = GetMessagePos();
        int   row;

        if ((short)LOWORD(at) == g_pal_mouse.x &&
            (short)HIWORD(at) == g_pal_mouse.y) return 0;
        g_pal_mouse.x = (short)LOWORD(at);
        g_pal_mouse.y = (short)HIWORD(at);

        row = pal_row_at(&g, (short)HIWORD(lp));
        if (row >= 0 && row != g.pal_sel) {
            g.pal_sel = row;
            InvalidateRect(wnd, NULL, FALSE);
            pal_preview(&g);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int row = pal_row_at(&g, (short)HIWORD(lp));
        if (row >= 0) pal_run(&g, row);
        return 0;
    }

    case WM_MOUSEWHEEL:
        g.pal_sel += (GET_WHEEL_DELTA_WPARAM(wp) > 0) ? -3 : 3;
        pal_reveal(&g);
        InvalidateRect(wnd, NULL, FALSE);
        pal_preview(&g);
        return 0;
    }

    return os_defproc(wnd, msg, wp, lp);
}

