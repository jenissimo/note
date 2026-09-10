/* note_buffer.c — a gap buffer with selection, undo and a sparse line index.
 *
 * No CRT, no allocation, C89.  See note_buffer.h for why a gap buffer.
 */

#include "note_buffer.h"

/* ==========================================================================
 * The gap
 * ========================================================================== */

int note_buffer_len(const note_buffer *b)
{
    return b->cap - (b->gapend - b->gap);
}

/* A position in the document turned into a slot in the storage, and back.
 * No slot ever lands inside the gap, so slots and positions sort the same
 * way -- which is what lets the line index be kept in slots. */
static int slot(const note_buffer *b, int i)
{
    return i < b->gap ? i : i + (b->gapend - b->gap);
}

#if NOTE_LINE_CHECKPOINTS
static int unslot(const note_buffer *b, int s)
{
    return s < b->gap ? s : s - (b->gapend - b->gap);
}
#endif

/* Unchecked: for the inside of this file, where the caller has already
 * established that `i` is in range. */
static nchar raw_at(const note_buffer *b, int i)
{
    return b->buf[slot(b, i)];
}

nchar note_buffer_at(const note_buffer *b, int i)
{
    if (i < 0 || i >= note_buffer_len(b)) return 0;
    return b->buf[slot(b, i)];
}

const nchar *note_buffer_span(const note_buffer *b, int from, int *len)
{
    int total = note_buffer_len(b);

    if (from < 0) from = 0;
    if (from >= total) { *len = 0; return b->buf; }

    if (from < b->gap) { *len = b->gap - from; return b->buf + from; }
    *len = total - from;
    return b->buf + from + (b->gapend - b->gap);
}

int note_buffer_copy(const note_buffer *b, int from, int len,
                     nchar *dst, int cap)
{
    int n = 0, total = note_buffer_len(b);

    if (cap <= 0) return 0;
    if (from < 0) from = 0;
    if (from + len > total) len = total - from;
    if (len > cap - 1) len = cap - 1;

    while (n < len) {
        int rl, k = 0;
        const nchar *q = note_buffer_span(b, from + n, &rl);
        if (rl <= 0) break;
        if (rl > len - n) rl = len - n;
        while (k < rl) { dst[n + k] = q[k]; k++; }
        n += rl;
    }
    if (cap > 0) dst[n] = 0;
    return n;
}

/* ==========================================================================
 * The line index
 *
 * A checkpoint is a (slot, line) pair: this line of the document starts at
 * this slot.  They are kept in the caller's array, two ints each, sorted --
 * by slot and by line at once, since both orders agree.
 *
 * What a checkpoint is *not* is mandatory.  Line 0 starts at 0 and always
 * has; every other checkpoint is a hint the code may drop whenever keeping it
 * correct would cost more than losing it does.  Everything a query actually
 * needs is `nlines` and the text.  That is what makes the edit paths cheap:
 * they repair or discard the few checkpoints an edit disturbs and never have
 * to rebuild the rest.
 * ========================================================================== */

/* The two halves of a checkpoint live in two halves of the array rather than
 * interleaved.  A pair would read better, but `lines[j]` is an index a 6502
 * can reach with one shift where `lines[j * 2 + 1]` is a multiply and an add,
 * and this file is walked by every keystroke on a machine that has neither. */
#if NOTE_LINE_CHECKPOINTS
#define CK_SLOT(b, j) ((b)->ck_slot[(j)])
#define CK_LINE(b, j) ((b)->ck_line[(j)])
#endif

/* Beyond this a span is wide enough that doubling it again buys nothing and
 * risks overflowing a 16-bit int in the comparisons below. */
#define CK_SPAN_MAX 8192

/* Whether the character at `i` ends a line.  A newline always does; a
 * carriage return does unless the newline after it will, which is how CRLF
 * comes out as one break rather than two. */
static int ends_line(const note_buffer *b, int i, int total)
{
    nchar c;

    if (i < 0 || i >= total) return 0;
    c = raw_at(b, i);
    if (c == (nchar)'\n') return 1;
    if (c != (nchar)'\r') return 0;
    return i + 1 >= total || raw_at(b, i + 1) != (nchar)'\n';
}

/* Walks forward from a line start, stopping at `to` or once it has passed
 * `want` breaks, whichever comes first.  Returns where the line it stopped in
 * begins, and reports through `seen` how many breaks it went by.
 *
 * Every question about lines comes down to a call here, which is why it reads
 * through spans rather than one call and one gap test per character: a redraw
 * pays this fifty times.  It is also the only place the CRLF rule is written
 * down, which is worth more than the few bytes the merged shape saves. */
static int scan(const note_buffer *b, int from, int to, int want, int *seen)
{
    int total = note_buffer_len(b), i = from, n = 0, start = from;

    if (to > total) to = total;
    while (i < to && n < want) {
        int rl, k = 0;
        const nchar *q = note_buffer_span(b, i, &rl);
        if (rl > to - i) rl = to - i;
        while (k < rl && n < want) {
            nchar c = q[k++];
            if (c == (nchar)'\n' ||
                (c == (nchar)'\r' &&
                 (i + k >= total || raw_at(b, i + k) != (nchar)'\n'))) {
                n++;
                start = i + k;
            }
        }
        i += k;
    }
    *seen = n;
    return start;
}

#if NOTE_LINE_CHECKPOINTS

/* The first checkpoint whose slot (or line, when `byline`) is past `v`, or
 * at it when `ge`.  One search rather than three: the callers want the same
 * shape and a 6502 pays for every copy of it. */
static int ck_find(const note_buffer *b, int v, int byline, int ge)
{
    const int *a = byline ? b->ck_line : b->ck_slot;
    int lo = 0, hi = b->nck;

    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (ge ? a[mid] < v : a[mid] <= v) lo = mid + 1; else hi = mid;
    }
    return lo;
}

/* The last checkpoint at or before slot `s`; -1 for "none, start from 0". */
#define ck_at(b, s)      (ck_find((b), (s), 0, 0) - 1)
#define ck_past(b, s)     ck_find((b), (s), 0, 0)
#define ck_from(b, s)     ck_find((b), (s), 0, 1)
#define ck_for_line(b, l) (ck_find((b), (l), 1, 0) - 1)

static void ck_drop(note_buffer *b, int j, int n)
{
    int k;

    for (k = j; k + n < b->nck; k++) {
        CK_SLOT(b, k) = CK_SLOT(b, k + n);
        CK_LINE(b, k) = CK_LINE(b, k + n);
    }
    b->nck -= n;
}

/* An edit that changed the line count shifts the numbering of every line
 * after it, and slot `s` is where "after it" starts. */
static void ck_renumber(note_buffer *b, int s, int d)
{
    int j;

    for (j = ck_past(b, s); j < b->nck; j++) CK_LINE(b, j) += d;
}

/* Throws away every other checkpoint and doubles the span to match.  This is
 * how a document outgrows its array without the index going wrong: the index
 * gets coarser and queries scan a little further, which is a cost you can
 * feel your way to rather than a line number that is quietly false. */
static void ck_compact(note_buffer *b)
{
    int j, w = 0;

    for (j = 1; j < b->nck; j += 2) {
        CK_SLOT(b, w) = CK_SLOT(b, j);
        CK_LINE(b, w) = CK_LINE(b, j);
        w++;
    }
    b->nck = w;
    if (b->ck_span < CK_SPAN_MAX) b->ck_span *= 2;
}

/* Moving the gap is the one thing that relocates text, so it is the one place
 * slots have to be corrected.  Only the run the gap stepped over moves, and
 * the checkpoints inside it are contiguous in the array. */
static void ck_relocate(note_buffer *b, int from, int to, int delta)
{
    int j;

    if (b->lines_dirty) return;
    if (b->cur_slot >= from && b->cur_slot < to) b->cur_slot += delta;
    if (!b->lines) return;
    for (j = ck_from(b, from); j < b->nck && CK_SLOT(b, j) < to; j++)
        CK_SLOT(b, j) += delta;
}

/* Reads the whole document once and lays checkpoints down as it goes, packing
 * tighter than the array can hold and compacting when it fills.  Only file
 * loading and note_buffer_init should ever reach here. */
static void rebuild(note_buffer *b)
{
    int total = note_buffer_len(b), at = 0, line = 0, keep;

    b->nck      = 0;
    b->ck_span  = 1;
    b->cur_line = 0;
    b->cur_slot = -1;

    keep = (b->lines && b->ck_max > 0);

    for (;;) {
        int n, step = keep ? b->ck_span : 32767;
        at = scan(b, at, total, step, &n);
        line += n;
        if (n < step) break;                 /* the document ran out */
        if (!keep) continue;
        if (b->nck >= b->ck_max) ck_compact(b);
        if (b->nck < b->ck_max) {
            CK_SLOT(b, b->nck) = slot(b, at);
            CK_LINE(b, b->nck) = line;
            b->nck++;
        }
    }

    b->nlines = line + 1;
    b->lines_dirty = 0;
}

static void ensure(note_buffer *b)
{
    if (b->lines_dirty) rebuild(b);
}

/* An edit that adds lines widens the segment it lands in and no other, so one
 * check after it is enough to keep every segment near the span -- which is
 * what bounds the scan a query pays once the binary search is done. */
static void ck_split(note_buffer *b, int pos)
{
    int j, sline, soff, eline, want, off, k, n;

    if (!b->lines || b->ck_max <= 0) return;

    j     = ck_at(b, slot(b, pos));
    sline = (j >= 0) ? CK_LINE(b, j) : 0;
    soff  = (j >= 0) ? unslot(b, CK_SLOT(b, j)) : 0;
    eline = (j + 1 < b->nck) ? CK_LINE(b, j + 1) : b->nlines;

    if (eline - sline <= b->ck_span + b->ck_span) return;
    if (b->nck >= b->ck_max) {
        /* No room for another: coarsen the whole index instead and leave the
         * split to the next edit that still wants one. */
        if (b->ck_span < CK_SPAN_MAX) ck_compact(b);
        return;
    }

    want = sline + b->ck_span;
    off  = scan(b, soff, note_buffer_len(b), b->ck_span, &n);

    for (k = b->nck; k > j + 1; k--) {
        CK_SLOT(b, k) = CK_SLOT(b, k - 1);
        CK_LINE(b, k) = CK_LINE(b, k - 1);
    }
    b->nck++;
    CK_SLOT(b, j + 1) = slot(b, off);
    CK_LINE(b, j + 1) = want;
}

#else   /* one entry per line -- see NOTE_LINE_CHECKPOINTS in note_config.h */

/* Offsets, not slots, so moving the gap costs the index nothing. */
#define ck_relocate(b, from, to, delta) ((void)(from), (void)(to), (void)(delta))

/* How many of the entries known to be right start at or before `pos`.  Both
 * maintenance calls below want exactly this. */
static int idx_upto(const note_buffer *b, int pos)
{
    int lo = 0, hi = b->valid, mid;

    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        if (b->lines[mid] <= pos) lo = mid + 1; else hi = mid;
    }
    return lo;
}

/* An edit that moved no line break: every line after it starts `delta`
 * further along and nothing else changed. */
static void idx_shift(note_buffer *b, int pos, int delta)
{
    int i;

    if (!b->lines || b->lines_cap <= 0) return;
    for (i = idx_upto(b, pos); i < b->valid; i++) b->lines[i] += delta;
}

/* An edit that did move one: the line it landed in still starts where it
 * started, and everything after that has to be found again.  Truncating the
 * valid prefix rather than dropping the index is what makes typing at the end
 * of a document rescan nothing at all. */
static void idx_touch(note_buffer *b, int pos)
{
    b->lines_dirty = 1;
    if (!b->lines || b->lines_cap <= 0) { b->valid = 0; return; }
    /* Strictly before, not up to: a line that began exactly at `pos` may not
     * survive the edit.  Inserting an LF where a CR already sits in front of
     * it merges two breaks into one and that line simply goes, so the entry
     * naming it cannot be trusted -- and resuming a line earlier costs one
     * line of scanning to be sure. */
    b->valid = idx_upto(b, pos - 1);
}

static void ensure(note_buffer *b)
{
    int total, n, at, seen;

    if (!b->lines_dirty) return;

    n = b->valid;
    if (n < 1 || !b->lines || b->lines_cap <= 0) {
        n = 1;
        if (b->lines && b->lines_cap > 0) b->lines[0] = 0;
    }
    at    = (b->lines && b->lines_cap > 0) ? b->lines[n - 1] : 0;
    total = note_buffer_len(b);

    for (;;) {
        at = scan(b, at, total, 1, &seen);
        if (!seen) break;
        if (b->lines && n < b->lines_cap) b->lines[n] = at;
        n++;
    }

    b->nlines = n;
    b->valid  = (b->lines && n > b->lines_cap) ? b->lines_cap : n;
    b->lines_dirty = 0;
}

/* The nearest known line start at or before what is being asked for.  A
 * document with more lines than the array holds keeps the first `lines_cap`
 * exactly and scans on from the last of them, which is slower for the tail
 * and never wrong -- the same bargain the checkpoints make, made cruder. */
static int line_base(note_buffer *b, int key, int byline, int *sline)
{
    int hi;

    *sline = 0;
    if (!b->lines || b->lines_cap <= 0) return 0;

    hi = (b->nlines < b->lines_cap) ? b->nlines : b->lines_cap;
    if (hi <= 0) return 0;

    if (byline) {
        if (key >= hi) key = hi - 1;
        if (key < 0) return 0;
        *sline = key;
        return b->lines[key];
    }
    {
        int lo = 0, mid, at = hi;
        while (lo < at) {
            mid = lo + (at - lo) / 2;
            if (b->lines[mid] <= key) lo = mid + 1; else at = mid;
        }
        if (lo <= 0) return 0;
        *sline = lo - 1;
        return b->lines[lo - 1];
    }
}

#endif

/* ==========================================================================
 * The gap, moved
 * ========================================================================== */

static void move_gap(note_buffer *b, int pos)
{
    int n, gs = b->gapend - b->gap;

    if (pos == b->gap) return;

    if (pos < b->gap) {
        /* Slide the run between pos and the gap up to the gap's far end. */
        ck_relocate(b, pos, b->gap, gs);
        n = b->gap - pos;
        while (n--) b->buf[--b->gapend] = b->buf[--b->gap];
    } else {
        ck_relocate(b, b->gapend, b->gapend + (pos - b->gap), -gs);
        n = pos - b->gap;
        while (n--) b->buf[b->gap++] = b->buf[b->gapend++];
    }
}

/* ==========================================================================
 * Undo
 * ========================================================================== */

void note_buffer_break_undo(note_buffer *b)
{
    b->group++;
}

/* Puts `len` characters into the text ring and returns where they went, or
 * -1 when there is no ring or the text will not fit in it at all. */
static int utext_put(note_buffer *b, int pos, int len)
{
    int at, i;

    if (!b->utext || len <= 0 || len > b->utext_cap) return -1;

    if (b->utext_head + len > b->utext_cap) b->utext_head = 0;
    at = b->utext_head;
    for (i = 0; i < len; i++)
        b->utext[at + i] = b->buf[slot(b, pos + i)];
    b->utext_head += len;
    return at;
}

static void record(note_buffer *b, int kind, int pos, int len, int text)
{
    note_edit *e;

    if (!b->undo || b->undo_cap <= 0 || b->in_undo) return;

    e = &b->undo[b->undo_head % b->undo_cap];
    e->kind  = (unsigned char)kind;
    e->pos   = pos;
    e->len   = len;
    e->text  = text;
    e->group = b->group;

    b->undo_head++;
    if (b->undo_count < b->undo_cap) b->undo_count++;
    b->redo_count = 0;          /* a fresh edit discards the redo branch */
}

/* ==========================================================================
 * Editing
 *
 * The raw operations.  They do not record undo; the public ones do, so that
 * undo can use these to put things back without recording its own work.
 *
 * Both follow the same shape.  Work out, before the text changes, how many
 * lines the edit adds or removes and whether it disturbs the break that
 * precedes it; do the edit; then touch only the checkpoints that could have
 * been affected.  An edit carrying no break -- which is nearly all of them --
 * disturbs none at all, because slots do not move when the gap does not.
 * ========================================================================== */

/* How the line count changes, for an insertion of `t` at `pos`.
 *
 * Only two things can move: the break status of the character just before the
 * edit, whose successor the edit replaces, and whatever the inserted text
 * brings with it.  Everything past the edit keeps both its character and its
 * successor, so it keeps its answer.
 *
 * The first of those is the CRLF subtlety, and it cuts both ways.  Typing
 * between a CR and its LF splits one break into two.  Typing a LF right after
 * a lone CR joins two into one, and then no line starts at `pos` any more --
 * which the caller has to know about separately from the count, so `starts`
 * reports it. */
/* Whether a line starts at `pos` once `next` is the character following the
 * one before it. */
static int starts_at(const note_buffer *b, int pos, nchar next)
{
    nchar c;

    if (pos <= 0) return 0;
    c = raw_at(b, pos - 1);
    if (c == (nchar)'\n') return 1;
    return c == (nchar)'\r' && next != (nchar)'\n';
}

static int insert_delta(const note_buffer *b, int pos, const nchar *t, int len,
                        int *starts)
{
    int total = note_buffer_len(b), d, j;
    nchar c;

    *starts = starts_at(b, pos, t[0]);
    d = *starts - ends_line(b, pos - 1, total);

    for (j = 0; j < len; j++) {
        c = t[j];
        if (c == (nchar)'\n') d++;
        else if (c == (nchar)'\r') {
            nchar next;
            if (j + 1 < len)      next = t[j + 1];
            else if (pos < total) next = raw_at(b, pos);
            else                  next = 0;
            if (next != (nchar)'\n') d++;
        }
    }
    return d;
}

static int erase_delta(const note_buffer *b, int pos, int len)
{
    int total = note_buffer_len(b), gone;
    nchar after = (pos + len < total) ? raw_at(b, pos + len) : (nchar)0;

    scan(b, pos, pos + len, len, &gone);
    return starts_at(b, pos, after) - ends_line(b, pos - 1, total) - gone;
}

static int raw_insert(note_buffer *b, int pos, const nchar *text, int len)
{
    int i, d = 0, starts = 0, live, e;
#if !NOTE_LINE_CHECKPOINTS
    int moved_break;
#endif

    if (len <= 0) return 1;
    if (b->gapend - b->gap < len) return 0;      /* no room */

#if NOTE_LINE_CHECKPOINTS
    live = !b->lines_dirty;
    if (live) d = insert_delta(b, pos, text, len, &starts);
#else
    /* The valid prefix has to be maintained whether or not a rescan is
     * already owed: an earlier edit lower down the document left entries
     * before it correct, and those still move. */
    live = 1;
    moved_break = ends_line(b, pos - 1, note_buffer_len(b));
    d = insert_delta(b, pos, text, len, &starts);
    (void)d;

    /* Whether the edit touched a line break at all -- which is not the same
     * question as whether the line *count* changed.  Inserting an LF in front
     * of a CR merges two breaks into one and contributes one of its own, so
     * the count holds still while a break has plainly moved. */
    moved_break = (moved_break != starts);
    if (!moved_break)
        for (i = 0; i < len; i++)
            if (text[i] == (nchar)'\n' || text[i] == (nchar)'\r') {
                moved_break = 1;
                break;
            }
#endif

    move_gap(b, pos);
    e = b->gapend;         /* the slot holding what is currently at `pos` */

#if NOTE_LINE_CHECKPOINTS
    if (live && b->lines) {
        int j = ck_at(b, e);
        /* A line that started at `pos` still starts at `pos`, but the text it
         * begins with is the text now arriving, so the checkpoint follows the
         * insertion rather than the character it used to name.  Unless a CR
         * before it has just found an LF, in which case the line is gone. */
        if (j >= 0 && CK_SLOT(b, j) == e) {
            if (starts) CK_SLOT(b, j) = b->gap;
            else        ck_drop(b, j, 1);
        }
        if (d) ck_renumber(b, e, d);
    }
    if (live && b->cur_slot >= 0) {
        int co = unslot(b, b->cur_slot);
        if (co > pos)       b->cur_line += d;
        else if (co == pos) b->cur_slot = starts ? b->gap : -1;
    }

#else
    (void)e;
    if (moved_break) idx_touch(b, pos);
    else             idx_shift(b, pos, len);
#endif

    for (i = 0; i < len; i++) b->buf[b->gap++] = text[i];

#if NOTE_LINE_CHECKPOINTS
    if (live) {
        b->nlines += d;
        if (d > 0) ck_split(b, pos);
    }
#else
    if (!d) b->nlines += 0;      /* a shift changes offsets, never the count */
#endif
    b->dirty = 1;
    return 1;
}

static int raw_erase(note_buffer *b, int pos, int len)
{
    int total = note_buffer_len(b);
    int d = 0, live, e;
#if !NOTE_LINE_CHECKPOINTS
    int moved_break;
#endif

    if (len <= 0) return 1;
    if (pos < 0 || pos + len > total) return 0;

#if NOTE_LINE_CHECKPOINTS
    live = !b->lines_dirty;
    if (live) d = erase_delta(b, pos, len);
#else
    live = 1;
    d = erase_delta(b, pos, len);
    (void)d;

    /* Same question as the insert side, asked of the range going out: a break
     * inside it, or one either side of it changing its mind about being one. */
    {
        nchar after = (pos + len < total) ? raw_at(b, pos + len) : (nchar)0;
        int   was   = ends_line(b, pos - 1, total);
        int   gone;

        scan(b, pos, pos + len, len, &gone);
        moved_break = gone || (was != starts_at(b, pos, after));
    }
#endif

    move_gap(b, pos);
    e = b->gapend;

#if NOTE_LINE_CHECKPOINTS
    if (live && b->lines) {
        /* Slots from `e` to `e + len` are about to be swallowed by the gap:
         * they name the erased characters and the one just past them, and a
         * line that began at any of them has either gone or has to be found
         * again.  Checkpoints are hints, so the cheap answer is to let them
         * go and let ck_split put one back when a segment needs it. */
        int lo = ck_from(b, e), hi = ck_past(b, e + len);
        if (hi > lo) ck_drop(b, lo, hi - lo);
        if (d) ck_renumber(b, e + len, d);
    }
    if (live && b->cur_slot >= 0) {
        int co = unslot(b, b->cur_slot);
        if (co > pos + len) b->cur_line += d;
        else if (co >= pos) b->cur_slot = -1;
    }

#else
    (void)e;
    if (moved_break) idx_touch(b, pos);
    else             idx_shift(b, pos, -len);
#endif

    b->gapend += len;

#if NOTE_LINE_CHECKPOINTS
    if (live) b->nlines += d;
#endif
    b->dirty = 1;
    return 1;
}

int note_buffer_set(note_buffer *b, const nchar *text)
{
    int len = n_len(text), i;

    if (len > b->cap) return 0;

    b->gap    = 0;
    b->gapend = b->cap;
    for (i = 0; i < len; i++) b->buf[b->gap++] = text[i];

    b->caret = b->anchor = 0;
    b->undo_head = b->undo_count = b->redo_count = 0;
    b->utext_head = 0;
    b->lines_dirty = 1;
#if !NOTE_LINE_CHECKPOINTS
    /* Nothing of the old document's index survives it.  The rebuild carries
     * on from the last entry it still trusts, so leaving a prefix behind here
     * would have it resume from a line of the file that just went. */
    b->valid = 0;
#endif
    b->dirty = 0;
    return 1;
}

int note_buffer_delete(note_buffer *b, int count)
{
    int lo = note_buffer_sel_lo(b), hi = note_buffer_sel_hi(b), len, at;

    if (lo != hi) {
        at  = lo;
        len = hi - lo;
    } else if (count > 0) {
        at  = b->caret;
        len = count;
        if (at + len > note_buffer_len(b)) len = note_buffer_len(b) - at;
    } else if (count < 0) {
        len = -count;
        if (len > b->caret) len = b->caret;
        at  = b->caret - len;
    } else {
        return 1;
    }
    if (len <= 0) return 1;

    /* Keep what is about to go, so undo can put it back. */
    record(b, NOTE_EDIT_DELETE, at, len, utext_put(b, at, len));
    if (!raw_erase(b, at, len)) return 0;

    b->caret = b->anchor = at;
    return 1;
}

int note_buffer_insert(note_buffer *b, const nchar *text, int len)
{
    int at;

    if (len < 0) len = n_len(text);
    if (note_buffer_has_sel(b) && !note_buffer_delete(b, 0)) return 0;

    at = b->caret;
    if (!raw_insert(b, at, text, len)) return 0;
    /* Keep a copy even though undo only has to erase: redo has to put the
     * same characters back, and by then the buffer no longer holds them. */
    record(b, NOTE_EDIT_INSERT, at, len, utext_put(b, at, len));

    b->caret = b->anchor = at + len;
    return 1;
}

/* Applies one record in reverse.  Shared by undo and redo, which differ only
 * in which direction they walk the ring. */
static void unapply(note_buffer *b, const note_edit *e, int reverse)
{
    int kind = e->kind;
    if (reverse) kind = (kind == NOTE_EDIT_INSERT) ? NOTE_EDIT_DELETE
                                                   : NOTE_EDIT_INSERT;

    if (kind == NOTE_EDIT_INSERT) {
        /* It was an insertion, so take it out again. */
        raw_erase(b, e->pos, e->len);
        b->caret = b->anchor = e->pos;
    } else if (e->text >= 0) {
        raw_insert(b, e->pos, &b->utext[e->text], e->len);
        b->caret = b->anchor = e->pos + e->len;
    }
}

int note_buffer_undo(note_buffer *b)
{
    unsigned char group;
    int did = 0;

    if (!b->undo || b->undo_count <= 0) return 0;

    b->in_undo = 1;
    group = b->undo[(b->undo_head - 1) % b->undo_cap].group;

    /* Everything typed in one burst comes back in one step. */
    while (b->undo_count > 0) {
        note_edit *e = &b->undo[(b->undo_head - 1) % b->undo_cap];
        if (did && e->group != group) break;
        unapply(b, e, 0);
        b->undo_head--;
        b->undo_count--;
        b->redo_count++;
        did = 1;
    }

    b->in_undo = 0;
    b->group++;
    return did;
}

int note_buffer_redo(note_buffer *b)
{
    unsigned char group;
    int did = 0;

    if (!b->undo || b->redo_count <= 0) return 0;

    b->in_undo = 1;
    group = b->undo[b->undo_head % b->undo_cap].group;

    while (b->redo_count > 0) {
        note_edit *e = &b->undo[b->undo_head % b->undo_cap];
        if (did && e->group != group) break;
        unapply(b, e, 1);
        b->undo_head++;
        b->undo_count++;
        b->redo_count--;
        did = 1;
    }

    b->in_undo = 0;
    return did;
}

/* ==========================================================================
 * Selection
 * ========================================================================== */

void note_buffer_select(note_buffer *b, int from, int to)
{
    int total = note_buffer_len(b);

    if (from < 0) from = 0;
    if (to   < 0) to   = 0;
    if (from > total) from = total;
    if (to   > total) to   = total;

    b->anchor = from;
    b->caret  = to;
}

/* Guarded by the same switch as the sparse index, and deliberately so rather
 * than by a second one: both answer the question "is this a machine that can
 * hold a document large enough to need it".  A target with no heap hands the
 * buffer one fixed array at startup and can never hand it another, so this is
 * a few hundred bytes of 6502 that nothing there could ever call. */
#if NOTE_LINE_CHECKPOINTS
int note_buffer_regrow(note_buffer *b, nchar *newbuf, int newcap)
{
    int total, tail, i;

    if (!b || !newbuf) return 0;
    total = note_buffer_len(b);
    if (newcap < total) return 0;
    tail = b->cap - b->gapend;           /* live characters after the gap */

    /* Everything before the gap is at the same slot in both arrays, so it is
     * copied where it stands. */
    for (i = 0; i < b->gap; i++) newbuf[i] = b->buf[i];

    /* The run after the gap goes to the end of the new array.  Copied from the
     * top down, so it is safe even when the caller handed us the same array
     * made longer in place. */
    for (i = 1; i <= tail; i++) newbuf[newcap - i] = b->buf[b->cap - i];

    b->buf    = newbuf;
    b->cap    = newcap;
    b->gapend = newcap - tail;

    /* Every slot after the gap has moved, and the checkpoint index is kept in
     * slots.  The offsets-only index does not care, but saying so here would
     * make this function know which one it was compiled with. */
    b->lines_dirty = 1;
    return 1;
}
#endif

void note_buffer_caret_set(note_buffer *b, int pos, int extend)
{
    int total = note_buffer_len(b);

    if (pos < 0) pos = 0;
    if (pos > total) pos = total;

    b->caret = pos;
    if (!extend) b->anchor = pos;

    /* Moving the caret ends a typing run: undo should not merge what was
     * typed here with what is typed somewhere else. */
    note_buffer_break_undo(b);
}

int note_buffer_has_sel(const note_buffer *b) { return b->caret != b->anchor; }

int note_buffer_sel_lo(const note_buffer *b)
{
    return b->caret < b->anchor ? b->caret : b->anchor;
}

int note_buffer_sel_hi(const note_buffer *b)
{
    return b->caret > b->anchor ? b->caret : b->anchor;
}

/* ==========================================================================
 * Lines, asked about
 * ========================================================================== */

int note_buffer_lines(note_buffer *b)
{
    ensure(b);
    return b->nlines;
}

#if NOTE_LINE_CHECKPOINTS

/* The nearest line start that is known and not past what is being asked for:
 * the last checkpoint before it, or the cached line when that is nearer.
 *
 * Both queries want this and both can express what they are looking for as a
 * key the checkpoints are already sorted by -- a slot, or a line number --
 * which is `byline`.  The cursor is comparable on the same key, since its slot
 * and its line are the two halves of the pair it caches. */
static int line_base(note_buffer *b, int key, int byline, int *sline)
{
    int off = 0;

    *sline = 0;
    if (b->lines && b->nck > 0) {
        int j = ck_find(b, key, byline, 0) - 1;
        if (j >= 0) { *sline = CK_LINE(b, j); off = unslot(b, CK_SLOT(b, j)); }
    }
    if (b->cur_slot >= 0 && b->cur_line > *sline &&
        (byline ? b->cur_line : b->cur_slot) <= key) {
        *sline = b->cur_line;
        off    = unslot(b, b->cur_slot);
    }
    return off;
}

#endif

int note_buffer_line_at(note_buffer *b, int pos)
{
    int total, sline, soff, start, n;

    ensure(b);
    total = note_buffer_len(b);
    if (pos < 0) pos = 0;
    if (pos > total) pos = total;

#if NOTE_LINE_CHECKPOINTS
    soff  = line_base(b, slot(b, pos), 0, &sline);
#else
    soff  = line_base(b, pos, 0, &sline);
#endif
    start = scan(b, soff, pos, pos - soff, &n);
#if NOTE_LINE_CHECKPOINTS
    b->cur_line = sline + n;
    b->cur_slot = slot(b, start);
#else
    (void)start;
#endif
    return sline + n;
}

int note_buffer_line_start(note_buffer *b, int line)
{
    int sline, soff, n;

    ensure(b);
    if (line <= 0) return 0;
    if (line >= b->nlines) return note_buffer_len(b);

    soff = line_base(b, line, 1, &sline);
    soff = scan(b, soff, note_buffer_len(b), line - sline, &n);
#if NOTE_LINE_CHECKPOINTS
    b->cur_line = line;
    b->cur_slot = slot(b, soff);
#endif
    return soff;
}

int note_buffer_line_len(note_buffer *b, int line)
{
    int total, start, end, n;

    ensure(b);
    total = note_buffer_len(b);
    start = note_buffer_line_start(b, line);
    end   = scan(b, start, total, 1, &n);
    if (n == 0) return total - start;

    /* `end` is the start of the next line, so it is past the break; step back
     * over it, which is two characters when the break was a CRLF. */
    end--;
    if (end > start && raw_at(b, end) == (nchar)'\n' &&
        raw_at(b, end - 1) == (nchar)'\r') end--;
    return end - start;
}

/* ==========================================================================
 * Searching
 * ========================================================================== */

static nchar fold(nchar c)
{
    return (c >= (nchar)'A' && c <= (nchar)'Z') ? (nchar)(c + 32) : c;
}

static int matches_at(const note_buffer *b, int at, const nchar *needle,
                      int nlen, int nocase, int whole)
{
    int i, total = note_buffer_len(b);

    if (at + nlen > total) return 0;

    for (i = 0; i < nlen; i++) {
        nchar a = b->buf[slot(b, at + i)], c = needle[i];
        if (nocase) { a = fold(a); c = fold(c); }
        if (a != c) return 0;
    }

    if (whole) {
        nchar before = at > 0 ? b->buf[slot(b, at - 1)] : (nchar)' ';
        nchar after  = at + nlen < total ? b->buf[slot(b, at + nlen)] : (nchar)' ';
        if ((before >= (nchar)'0' && before <= (nchar)'9') ||
            (fold(before) >= (nchar)'a' && fold(before) <= (nchar)'z') ||
             before == (nchar)'_') return 0;
        if ((after >= (nchar)'0' && after <= (nchar)'9') ||
            (fold(after) >= (nchar)'a' && fold(after) <= (nchar)'z') ||
             after == (nchar)'_') return 0;
    }
    return 1;
}

int note_buffer_find(note_buffer *b, const nchar *needle, int from,
                     unsigned flags)
{
    int nlen = n_len(needle), total = note_buffer_len(b), i;
    int nocase = (flags & FIND_MATCHCASE) ? 0 : 1;
    int whole  = (flags & FIND_WHOLEWORD) ? 1 : 0;

    if (nlen <= 0 || nlen > total) return -1;

    if (flags & FIND_DOWN) {
        if (from < 0) from = 0;
        for (i = from; i + nlen <= total; i++)
            if (matches_at(b, i, needle, nlen, nocase, whole)) return i;
        /* Wrap once: a search that stops at the end of the file is a search
         * the user has to repeat from the top by hand. */
        for (i = 0; i < from && i + nlen <= total; i++)
            if (matches_at(b, i, needle, nlen, nocase, whole)) return i;
        return -1;
    }

    if (from > total - nlen) from = total - nlen;
    for (i = from; i >= 0; i--)
        if (matches_at(b, i, needle, nlen, nocase, whole)) return i;
    for (i = total - nlen; i > from; i--)
        if (matches_at(b, i, needle, nlen, nocase, whole)) return i;
    return -1;
}

/* ==========================================================================
 * Setup
 * ========================================================================== */

void note_buffer_init(note_buffer *b, nchar *text, int text_cap,
                      note_edit *undo, int undo_cap,
                      nchar *utext, int utext_cap,
                      int *lines, int lines_cap)
{
    int i;
    unsigned char *raw = (unsigned char *)b;
    for (i = 0; i < (int)sizeof(*b); i++) raw[i] = 0;

    b->buf       = text;
    b->cap       = text_cap;
    b->gap       = 0;
    b->gapend    = text_cap;

    b->undo      = undo;
    b->undo_cap  = undo_cap;
    b->utext     = utext;
    b->utext_cap = utext_cap;

    b->lines      = lines;
    b->lines_cap  = lines_cap;
#if NOTE_LINE_CHECKPOINTS
    b->ck_max     = lines ? lines_cap / 2 : 0;
    b->ck_slot    = lines;
    b->ck_line    = lines ? lines + b->ck_max : 0;
#endif
    /* Everything else about the index is set by the rebuild `lines_dirty`
     * forces before any query, and no edit touches it while it is dirty. */
    b->lines_dirty = 1;
}
