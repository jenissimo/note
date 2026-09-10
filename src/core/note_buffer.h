/* note_buffer.h — the text of a document, when nothing else owns it.
 *
 * The Win32 backend hands its text to a RICHEDIT control and asks it to do
 * the editing.  MS-DOS and the Commodore 64 have no such thing: there is a
 * screen, a keyboard, and whatever you write yourself.  This is what they
 * edit instead, and it implements the same operations note_host_ops asks for,
 * so a backend built on it is only drawing and reading keys.
 *
 * A gap buffer, because the shape of editing is a caret that stays put while
 * characters arrive: keeping the free space at the caret makes insertion a
 * store and a decrement, and the cost of moving the gap is paid only when the
 * caret jumps, where a pause is invisible anyway.  A piece table would beat it
 * on a very large file, but on a machine with 64 KB there is no very large
 * file, and a piece table's indirection costs more per character than a 6502
 * has to spare.
 *
 * All storage is the caller's: a buffer holds pointers into arrays it was
 * given and never allocates.  That is what lets the same code run where there
 * is no heap at all.
 */
#ifndef NOTE_BUFFER_H
#define NOTE_BUFFER_H

#include "note_core.h"

/* An undo record.  The text a record needs is kept in a separate ring so that
 * a long-lived record does not pin a large allocation. */
typedef struct {
    int  pos;            /* where the edit happened                        */
    int  len;            /* how many characters it covered                 */
    int  text;           /* offset into the undo text ring, -1 if none     */
    unsigned char kind;  /* NOTE_EDIT_*                                    */
    unsigned char group; /* consecutive records with the same group undo
                          * together, so typing a word is one undo         */
} note_edit;

enum {
    NOTE_EDIT_INSERT = 0,   /* text was inserted; undo removes it       */
    NOTE_EDIT_DELETE        /* text was removed; undo puts it back      */
};

typedef struct {
    /* The text.  Everything before `gap` and from `gapend` on is live; the
     * space between them is free. */
    nchar *buf;
    int    cap;
    int    gap;
    int    gapend;

    /* The caret, and the other end of the selection.  Equal means no
     * selection.  `caret` is where typing goes and where the gap wants to be. */
    int    caret;
    int    anchor;

    /* Undo.  A ring of records over a ring of text: when either wraps, the
     * oldest history is forgotten, which is the right thing to lose. */
    note_edit *undo;
    int        undo_cap;
    int        undo_head;    /* next record to write                     */
    int        undo_count;   /* records available to undo                */
    int        redo_count;   /* records undone and not yet redone        */
    nchar     *utext;
    int        utext_cap;
    int        utext_head;
    unsigned char group;     /* current grouping id                      */
    unsigned char in_undo;   /* re-entry guard: undo must not record      */

    /* The line index.  Not one entry per line -- a checkpoint every so many
     * lines, stored in the caller's array as (slot, line) pairs.  Resolving a
     * line is then a binary search over the checkpoints and a short scan of
     * the text between one and the answer.
     *
     * The offsets are *slots*, positions in `buf`, not positions in the
     * document.  That is the whole trick: inserting and deleting happen at the
     * gap, and text on either side of the gap does not move, so an edit that
     * carries no line break leaves every checkpoint alone.  Only moving the
     * gap relocates text, and only the run it steps over.
     *
     * `nlines` is the real line count, kept up to date by every edit; the
     * checkpoints are only a shortcut into the text and may be as sparse as
     * the array forces.  A document too big for the array gets a coarser
     * index, never a truncated one. */
    int   *lines;
    int    lines_cap;        /* ints the caller gave us                  */
    int    nlines;           /* lines in the document                    */
    int    lines_dirty;      /* the index is not current                 */

#if NOTE_LINE_CHECKPOINTS
    /* The two halves of `lines`, as pointers rather than as one array and an
     * offset.  A 6502 pays a runtime add and a 16-bit index scaling for every
     * `lines[ck_max + j]`, and the index is read several times per query; two
     * base pointers turn each of those into a plain indexed load. */
    int   *ck_slot;
    int   *ck_line;
    int    nck;              /* checkpoints in use                       */
    int    ck_max;           /* lines_cap / 2                            */
    int    ck_span;          /* lines a checkpoint tries to cover        */

    /* The last line resolved, so that walking consecutive lines -- which is
     * what drawing a screen does -- costs one line of scanning each rather
     * than a fresh search from the nearest checkpoint. */
    int    cur_line;
    int    cur_slot;         /* -1 when there is nothing cached          */
#else
    /* One entry per line, holding document offsets rather than slots -- an
     * offset does not care where the gap is, so moving the gap costs the
     * index nothing and only the edit itself has to be accounted for.
     *
     * `valid` is how many leading entries are still known to be right.  An
     * edit that moves a line break truncates it to the edited line instead of
     * throwing the index away, so typing at the end of a document rescans
     * nothing and typing at the top rescans what follows it. */
    int    valid;
#endif

    int    dirty;            /* modified since the last note_buffer_clean */
} note_buffer;

/* Set up a buffer over storage the caller owns.  `text`, `undo`, `utext` and
 * `lines` may each be null, in which case that facility is simply absent —
 * a buffer with no undo ring still edits, it just cannot undo. */
/* Moves the document into a larger array the caller has allocated.
 *
 * A buffer never allocates, so growing one is the caller's job -- but where the
 * text has to land in the new array is not, and a backend working that out for
 * itself is a backend that knows about the gap.  The live text either side of
 * the gap keeps its position in the document and its slot before the gap; only
 * the run after the gap moves, to the end of the new array, which is what makes
 * the gap grow rather than the text shift.
 *
 * `newcap` must be at least the current length.  Returns 0 and changes nothing
 * if it is not, or if either argument is missing. */
#if NOTE_LINE_CHECKPOINTS
int note_buffer_regrow(note_buffer *b, nchar *newbuf, int newcap);
#endif

void note_buffer_init(note_buffer *b, nchar *text, int text_cap,
                      note_edit *undo, int undo_cap,
                      nchar *utext, int utext_cap,
                      int *lines, int lines_cap);

/* Contents ---------------------------------------------------------------- */

int   note_buffer_len (const note_buffer *b);
nchar note_buffer_at  (const note_buffer *b, int i);
/* The longest run of characters stored contiguously from `from`, so a caller
 * walking a line can read them straight out of storage instead of paying a
 * call and a gap test per character.  Writes the run's length to `len`, which
 * is 0 at the end of the text; the pointer is valid until the next edit. */
const nchar *note_buffer_span(const note_buffer *b, int from, int *len);
/* Copies [from, from+len) out, NUL-terminated.  Returns how many it wrote. */
int   note_buffer_copy(const note_buffer *b, int from, int len,
                       nchar *dst, int cap);

/* Replaces everything, and forgets the undo history: this is loading a file,
 * not an edit.  Returns 0 if the text does not fit. */
int   note_buffer_set (note_buffer *b, const nchar *text);

/* Editing ----------------------------------------------------------------- */

/* Inserts at the caret, replacing the selection if there is one. */
int  note_buffer_insert(note_buffer *b, const nchar *text, int len);
/* Deletes the selection, or `count` characters forward (or back, if
 * negative) when there is none. */
int  note_buffer_delete(note_buffer *b, int count);

int  note_buffer_undo(note_buffer *b);
int  note_buffer_redo(note_buffer *b);
/* Ends the current undo group, so the next edit starts a new one.  Called
 * when the caret moves, when the file is saved, at a word boundary. */
void note_buffer_break_undo(note_buffer *b);

/* Selection and the caret -------------------------------------------------- */

void note_buffer_caret_set(note_buffer *b, int pos, int extend);

/* Sets both ends of the selection without ending the current undo group.
 *
 * note_buffer_caret_set exists to end it: moving the caret means what is typed
 * next is a separate thing from what was typed before.  An operation that moves
 * the caret as part of itself -- indenting a block, replacing a match -- wants
 * the opposite, because it has to come back in one Ctrl+Z. */
void note_buffer_select(note_buffer *b, int from, int to);
int  note_buffer_sel_lo(const note_buffer *b);
int  note_buffer_sel_hi(const note_buffer *b);
int  note_buffer_has_sel(const note_buffer *b);

/* Lines -------------------------------------------------------------------- */

int  note_buffer_lines   (note_buffer *b);              /* how many         */
int  note_buffer_line_at (note_buffer *b, int pos);     /* 0-based          */
int  note_buffer_line_start(note_buffer *b, int line);
int  note_buffer_line_len(note_buffer *b, int line);    /* without the break */

/* Searching ---------------------------------------------------------------- */

/* The next match at or after `from`, or -1.  Searches backwards when
 * `flags` lacks FIND_DOWN, and wraps once. */
int  note_buffer_find(note_buffer *b, const nchar *needle, int from,
                      unsigned flags);

#endif /* NOTE_BUFFER_H */
