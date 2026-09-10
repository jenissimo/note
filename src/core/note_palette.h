/* note_palette.h — the command palette's row model.
 *
 * The palette is not a second list of what the editor can do.  Its command
 * list is built by walking note_menu, the same table the menu bar renders, so
 * a command cannot exist in one and be missing from the other, and a command
 * that has left the menu bar (MI_HIDDEN) is still here.
 *
 * The same overlay also stands in for the dialogs that existed only to show a
 * list — themes, fonts, go-to-line.  Those are "modes": the backend refills
 * the rows and supplies preview and commit callbacks, and everything about
 * holding rows, editing the query and ranking the matches stays here, where
 * the next backend gets it for free.
 */
#ifndef NOTE_PALETTE_H
#define NOTE_PALETTE_H

#include "note_core.h"

typedef struct {
    unsigned short id;        /* CMD_* in the command list; a mode's own key
                               * otherwise (a theme index, a line number...) */
    const nchar   *label;
    const nchar   *accel;     /* shown dimmed on the right; may be NULL     */
} note_pal_row;

typedef struct {
    note_pal_row rows[NOTE_PALETTE_MAX];
    int          nrows;

    /* Labels the palette had to build rather than borrow.  A mode listing
     * things that already have stable names (themes, font faces) points
     * straight at them and spends none of this. */
    nchar        pool[NOTE_PALETTE_POOL];
    int          pool_used;

    short        order[NOTE_PALETTE_MAX];   /* the matches, best first */
    int          nshown;

    /* The input line.  The palette draws its own — no backend has a text
     * control that would sit in an overlay like this — so the caret, the
     * selection and one level of undo live here, where every backend gets
     * the same editing behaviour rather than half of it. */
    nchar        query[NOTE_PALETTE_QUERY];
    /* Where the part of the query that filters begins.  Zero for every mode
     * whose query is a filter and nothing else; a mode whose query is a path
     * moves it past the last separator, so that typing a folder narrows the
     * names inside it instead of matching them against the whole path. */
    short        filter_from;
    short        caret;       /* insertion point, in nchars               */
    short        anchor;      /* the other end of the selection           */
    nchar        undo[NOTE_PALETTE_QUERY];
    short        undo_caret, undo_anchor;
    short        has_undo;
} note_palette;

/* Empties the rows, the pool and the query. */
void note_palette_reset(note_palette *p);

/* The same, but leaves the query where it is — for a mode whose rows are
 * recomputed from what has been typed, on every keystroke. */
void note_palette_reset_rows(note_palette *p);

/* Adds a row.  note_palette_add borrows the strings — they must outlive the
 * palette — while note_palette_add_copy takes its own copy in the pool.
 * Both return 1 if the row fit. */
int  note_palette_add     (note_palette *p, unsigned id,
                           const nchar *label, const nchar *accel);
int  note_palette_add_copy(note_palette *p, unsigned id,
                           const nchar *label, const nchar *accel);

/* Fills the rows with every command in note_menu and clears the query. */
void note_palette_commands(note_palette *p);

/* Ranks on the query from `from` onwards rather than on all of it, and keeps
 * doing so until the next reset.  See `filter_from`. */
void note_palette_filter_from(note_palette *p, int from);
/* What the ranking is actually matching against — the query, or its tail. */
const nchar *note_palette_filter_text(const note_palette *p);
/* And where that tail begins, for a backend drawing the two halves of a path
 * differently. */
int          note_palette_filter_at(const note_palette *p);

/* Re-ranks against the current query: rows whose label contains it as a
 * subsequence, best match first.  An empty query keeps every row in the
 * order it was added. */
void note_palette_filter(note_palette *p);

/* Query editing.  `ch` is a typed character; 8 is backspace.  A character
 * typed over a selection replaces it.  Returns 1 if the query changed, in
 * which case the rows were re-ranked. */
int  note_palette_type (note_palette *p, unsigned ch);
void note_palette_clear(note_palette *p);      /* empties just the query */
const nchar *note_palette_query(const note_palette *p);

/* The rest of what an input line is expected to do.  A backend maps its own
 * keys and clipboard onto these; none of the behaviour is platform-specific,
 * so none of it is written twice.
 *
 * note_palette_edit returns 1 if the text changed (the rows were re-ranked);
 * a move or a selection change returns 0 and only wants a repaint. */
enum {
    PAL_ED_LEFT = 0, PAL_ED_RIGHT,
    PAL_ED_WORD_LEFT, PAL_ED_WORD_RIGHT,
    PAL_ED_HOME, PAL_ED_END,
    PAL_ED_BACK, PAL_ED_DELETE,
    PAL_ED_BACK_WORD, PAL_ED_DELETE_WORD,
    PAL_ED_ALL, PAL_ED_UNDO
};
int  note_palette_edit(note_palette *p, int op, int extend);

/* Replaces the whole query; the caret lands at the end with nothing selected.
 * `sel_to` > 0 instead selects [0, sel_to) — how the rename mode arrives with
 * the stem of a file name already selected. */
void note_palette_set(note_palette *p, const nchar *s, int sel_to);

/* Inserts text at the caret, over the selection if there is one.  Line breaks
 * and control characters are dropped rather than pasted, since this is one
 * line.  Returns 1 if anything went in. */
int  note_palette_insert(note_palette *p, const nchar *s);

int  note_palette_caret (const note_palette *p);
/* The selection as an ordered pair; returns 1 if it is not empty. */
int  note_palette_sel   (const note_palette *p, int *from, int *to);
/* Copies the selected text out.  Returns its length. */
int  note_palette_selected(const note_palette *p, nchar *buf, int cap);
/* The query read as a decimal number, for a mode that asks for one. */
unsigned note_palette_number(const note_palette *p);

int  note_palette_count(const note_palette *p);
/* The i'th match, or NULL.  i counts matches, not rows. */
const note_pal_row *note_palette_at(const note_palette *p, int i);

/* Where in `label` the query's characters landed, so a backend can pick them
 * out of the row.  Writes ascending indices and returns how many; zero if the
 * label does not match at all. */
int  note_palette_marks(const nchar *label, const nchar *query,
                        unsigned char *out, int cap);

#endif /* NOTE_PALETTE_H */
