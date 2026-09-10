/* note_palette.c — rows, query editing and ranking for the command palette.
 *
 * No OS headers, no CRT; see note_config.h for the rules.
 */
#include "note_palette.h"

/* ==========================================================================
 * Rows
 * ========================================================================== */

void note_palette_reset_rows(note_palette *p)
{
    p->nrows     = 0;
    p->pool_used = 0;
    p->nshown    = 0;
}

void note_palette_reset(note_palette *p)
{
    note_palette_reset_rows(p);
    p->query[0]    = 0;
    p->caret       = 0;
    p->anchor      = 0;
    p->has_undo    = 0;
    p->filter_from = 0;
}

void note_palette_filter_from(note_palette *p, int from)
{
    int len = n_len(p->query);
    if (from < 0)   from = 0;
    if (from > len) from = len;
    p->filter_from = (short)from;
}

int note_palette_filter_at(const note_palette *p)
{
    int len = n_len(p->query);
    return (p->filter_from > len) ? len : p->filter_from;
}

const nchar *note_palette_filter_text(const note_palette *p)
{
    int len = n_len(p->query);
    int at  = p->filter_from;
    if (at > len) at = len;
    return p->query + at;
}

int note_palette_add(note_palette *p, unsigned id,
                     const nchar *label, const nchar *accel)
{
    note_pal_row *r;

    if (p->nrows >= NOTE_PALETTE_MAX || !label) return 0;
    r = &p->rows[p->nrows++];
    r->id    = (unsigned short)id;
    r->label = label;
    r->accel = accel;
    return 1;
}

/* Copies a string into the pool and returns it, or NULL if it will not fit. */
static const nchar *pool_put(note_palette *p, const nchar *s)
{
    int len = n_len(s);
    nchar *at;

    if (p->pool_used + len + 1 > NOTE_PALETTE_POOL) return 0;
    at = &p->pool[p->pool_used];
    n_copy(at, s, len + 1);
    p->pool_used += len + 1;
    return at;
}

int note_palette_add_copy(note_palette *p, unsigned id,
                          const nchar *label, const nchar *accel)
{
    const nchar *l, *a = 0;

    if (p->nrows >= NOTE_PALETTE_MAX || !label) return 0;
    l = pool_put(p, label);
    if (!l) return 0;
    if (accel && accel[0]) {
        a = pool_put(p, accel);
        if (!a) return 0;
    }
    return note_palette_add(p, id, l, a);
}

/* ==========================================================================
 * The command list, read off the menu model
 * ========================================================================== */

/* "&Save &&As...\tCtrl+S" -> "Save &As...".  The mnemonic marker is a menu's
 * business; in the palette the keyboard is the filter box. */
static void strip_amp(nchar *dst, const nchar *src, int cap)
{
    int i = 0, j = 0;

    if (cap <= 0) return;
    while (src[i] && src[i] != (nchar)'\t' && j < cap - 1) {
        if (src[i] == (nchar)'&') {
            i++;
            if (!src[i]) break;
            if (src[i] != (nchar)'&') continue;   /* "&&" is a literal & */
        }
        dst[j++] = src[i++];
    }
    dst[j] = 0;
}

/* The part after the tab, which the menus already show right-aligned. */
static void accel_of(nchar *dst, const nchar *src, int cap)
{
    int i = 0, j = 0;

    dst[0] = 0;
    while (src[i] && src[i] != (nchar)'\t') i++;
    if (!src[i]) return;
    i++;
    while (src[i] && j < cap - 1) dst[j++] = src[i++];
    dst[j] = 0;
}

void note_palette_commands(note_palette *p)
{
    const note_menu_item *it = note_menu;
    nchar group[NOTE_PALETTE_LABEL];

    note_palette_reset(p);
    group[0] = 0;

    /* The same walk build_popup() does: a run of popups, each closed by an
     * MI_END, the run itself closed by a non-popup entry. */
    while (it->kind == MI_POPUP) {
        strip_amp(group, it->label, NOTE_PALETTE_LABEL);

        for (it++; it->kind != MI_END; it++) {
            nchar label[NOTE_PALETTE_LABEL], accel[NOTE_PALETTE_ACCEL], text[NOTE_PALETTE_LABEL];

            if (it->kind == MI_SEP || !it->id) continue;

            /* "File: Save As..." — the menu a command lives under is part of
             * how people name it, and it gives the filter more to bite on. */
            n_copy(label, group, NOTE_PALETTE_LABEL);
            n_cat (label, N(": "), NOTE_PALETTE_LABEL);
            strip_amp(text, it->label, NOTE_PALETTE_LABEL);
            n_cat (label, text, NOTE_PALETTE_LABEL);

            accel_of(accel, it->label, NOTE_PALETTE_ACCEL);
            note_palette_add_copy(p, it->id, label, accel);
        }
        it++;                       /* step past the popup's MI_END */
    }

    note_palette_filter(p);
}

/* ==========================================================================
 * The query
 * ========================================================================== */

/* A word for the purpose of Ctrl+Left and Ctrl+Backspace: anything that is
 * not punctuation or a space.  Everything above ASCII counts, which is wrong
 * for no script the palette is likely to see a file name in. */
static int is_word(nchar c)
{
    if (c >= (nchar)'0' && c <= (nchar)'9') return 1;
    if (c >= (nchar)'A' && c <= (nchar)'Z') return 1;
    if (c >= (nchar)'a' && c <= (nchar)'z') return 1;
    if (c == (nchar)'_') return 1;
    return c > 127;
}

static void sel_clamp(note_palette *p)
{
    int len = n_len(p->query);
    if (p->caret  > len) p->caret  = (short)len;
    if (p->anchor > len) p->anchor = (short)len;
    if (p->caret  < 0)   p->caret  = 0;
    if (p->anchor < 0)   p->anchor = 0;
}

int note_palette_caret(const note_palette *p)
{
    return p->caret;
}

int note_palette_sel(const note_palette *p, int *from, int *to)
{
    int a = p->anchor, b = p->caret, t;
    if (a > b) { t = a; a = b; b = t; }
    if (from) *from = a;
    if (to)   *to   = b;
    return b > a;
}

int note_palette_selected(const note_palette *p, nchar *buf, int cap)
{
    int a, b, i, n = 0;
    note_palette_sel(p, &a, &b);
    for (i = a; i < b && n < cap - 1; i++) buf[n++] = p->query[i];
    if (cap > 0) buf[n] = 0;
    return n;
}

/* One level, taken before anything that changes the text. */
static void undo_save(note_palette *p)
{
    n_copy(p->undo, p->query, NOTE_PALETTE_QUERY);
    p->undo_caret  = p->caret;
    p->undo_anchor = p->anchor;
    p->has_undo    = 1;
}

/* Removes [a,b) and leaves the caret where the text was. */
static void cut_range(note_palette *p, int a, int b)
{
    int len = n_len(p->query), i;

    if (a < 0) a = 0;
    if (b > len) b = len;
    if (a >= b) return;

    for (i = b; i <= len; i++) p->query[a + i - b] = p->query[i];
    p->caret = p->anchor = (short)a;
}

static int cut_selection(note_palette *p)
{
    int a, b;
    if (!note_palette_sel(p, &a, &b)) return 0;
    cut_range(p, a, b);
    return 1;
}

int note_palette_insert(note_palette *p, const nchar *s)
{
    int len, i, n = 0, room, cut;

    if (!s || !s[0]) return 0;
    undo_save(p);
    cut = cut_selection(p);

    len  = n_len(p->query);
    room = NOTE_PALETTE_QUERY - 1 - len;

    /* One line: a pasted newline or tab is dropped, not turned into a glyph
     * the filter would then never match. */
    for (i = 0; s[i] && n < room; i++)
        if (s[i] >= 32 && s[i] != 127) n++;
    if (!n) {
        /* Nothing to put in, but a selection typed over is still gone. */
        if (cut) note_palette_filter(p);
        return cut;
    }

    /* Open the gap, then fill it. */
    for (i = len; i >= p->caret; i--) p->query[i + n] = p->query[i];
    {
        int at = p->caret, k;
        for (k = 0; s[k] && at < p->caret + n; k++)
            if (s[k] >= 32 && s[k] != 127) p->query[at++] = s[k];
    }
    p->caret  = (short)(p->caret + n);
    p->anchor = p->caret;

    note_palette_filter(p);
    return 1;
}

int note_palette_type(note_palette *p, unsigned ch)
{
    nchar one[2];

    if (ch == 8) return note_palette_edit(p, PAL_ED_BACK, 0);
    if (ch < 32 || ch == 127) return 0;

    one[0] = (nchar)ch;
    one[1] = 0;
    return note_palette_insert(p, one);
}

void note_palette_set(note_palette *p, const nchar *s, int sel_to)
{
    int len;

    n_copy(p->query, s ? s : N(""), NOTE_PALETTE_QUERY);
    len = n_len(p->query);

    if (sel_to > 0) {
        if (sel_to > len) sel_to = len;
        p->anchor = 0;
        p->caret  = (short)sel_to;
    } else {
        p->anchor = p->caret = (short)len;
    }
    p->has_undo = 0;
    note_palette_filter(p);
}

/* Where Ctrl+Left and Ctrl+Right land: past the run of separators, then past
 * the run of word characters. */
static int word_left(const note_palette *p, int at)
{
    while (at > 0 && !is_word(p->query[at - 1])) at--;
    while (at > 0 &&  is_word(p->query[at - 1])) at--;
    return at;
}

static int word_right(const note_palette *p, int at)
{
    int len = n_len(p->query);
    while (at < len && !is_word(p->query[at])) at++;
    while (at < len &&  is_word(p->query[at])) at++;
    return at;
}

int note_palette_edit(note_palette *p, int op, int extend)
{
    int len = n_len(p->query);
    int to  = p->caret;

    sel_clamp(p);

    switch (op) {
    case PAL_ED_LEFT:
    case PAL_ED_RIGHT: {
        int a, b;
        /* Without Shift, an arrow key collapses the selection to its edge
         * rather than moving from the caret — as every text field does. */
        if (!extend && note_palette_sel(p, &a, &b)) {
            p->caret = p->anchor = (short)(op == PAL_ED_LEFT ? a : b);
            return 0;
        }
        to = p->caret + (op == PAL_ED_LEFT ? -1 : 1);
        break;
    }
    case PAL_ED_WORD_LEFT:  to = word_left(p, p->caret);  break;
    case PAL_ED_WORD_RIGHT: to = word_right(p, p->caret); break;
    case PAL_ED_HOME:       to = 0;                       break;
    case PAL_ED_END:        to = len;                     break;

    case PAL_ED_ALL:
        p->anchor = 0;
        p->caret  = (short)len;
        return 0;

    case PAL_ED_BACK:
        if (note_palette_sel(p, 0, 0)) { undo_save(p); cut_selection(p); break; }
        if (!p->caret) return 0;
        undo_save(p);
        cut_range(p, p->caret - 1, p->caret);
        break;

    case PAL_ED_DELETE:
        if (note_palette_sel(p, 0, 0)) { undo_save(p); cut_selection(p); break; }
        if (p->caret >= len) return 0;
        undo_save(p);
        cut_range(p, p->caret, p->caret + 1);
        break;

    case PAL_ED_BACK_WORD: {
        int a = word_left(p, p->caret);
        if (a == p->caret) return 0;
        undo_save(p);
        cut_range(p, a, p->caret);
        break;
    }
    case PAL_ED_DELETE_WORD: {
        int b = word_right(p, p->caret);
        if (b == p->caret) return 0;
        undo_save(p);
        cut_range(p, p->caret, b);
        break;
    }

    case PAL_ED_UNDO: {
        nchar was[NOTE_PALETTE_QUERY];
        short wc, wa;
        if (!p->has_undo) return 0;
        n_copy(was, p->query, NOTE_PALETTE_QUERY);
        wc = p->caret; wa = p->anchor;
        n_copy(p->query, p->undo, NOTE_PALETTE_QUERY);
        p->caret  = p->undo_caret;
        p->anchor = p->undo_anchor;
        n_copy(p->undo, was, NOTE_PALETTE_QUERY);   /* undo of the undo */
        p->undo_caret = wc; p->undo_anchor = wa;
        sel_clamp(p);
        note_palette_filter(p);
        return 1;
    }

    default:
        return 0;
    }

    /* A movement fell through to here; a deletion has already returned its
     * text change through the switch. */
    if (op <= PAL_ED_END) {
        if (to < 0)   to = 0;
        if (to > len) to = len;
        p->caret = (short)to;
        if (!extend) p->anchor = p->caret;
        return 0;
    }

    note_palette_filter(p);
    return 1;
}

void note_palette_clear(note_palette *p)
{
    p->query[0] = 0;
    p->caret = p->anchor = 0;
    p->has_undo = 0;
    note_palette_filter(p);
}

const nchar *note_palette_query(const note_palette *p)
{
    return p->query;
}

unsigned note_palette_number(const note_palette *p)
{
    const nchar *q = p->query;
    unsigned v = 0;

    while (*q >= (nchar)'0' && *q <= (nchar)'9') {
        /* Clamped rather than wrapped: a core that may only have 16-bit ints
         * must not turn a long paste of digits into a small line number. */
        if (v > 6000) return 60000;
        v = v * 10 + (unsigned)(*q++ - (nchar)'0');
    }
    return v;
}

/* ==========================================================================
 * Filtering
 * ========================================================================== */

static nchar lower_ch(nchar c)
{
    if (c >= (nchar)'A' && c <= (nchar)'Z') return (nchar)(c + ('a' - 'A'));
    return c;
}

/* A word start, where a typist's initials are most likely to have come from. */
static int is_boundary(const nchar *s, int i)
{
    nchar prev;
    if (i == 0) return 1;
    prev = s[i - 1];
    return prev == (nchar)' '  || prev == (nchar)':' || prev == (nchar)'-' ||
           prev == (nchar)'/'  || prev == (nchar)'.' || prev == (nchar)',';
}

/* Subsequence match, scored.  "svas" has to reach "File: Save As..." rather
 * than only exact prefixes, but it must also not rank every command that
 * happens to contain those four letters equally: runs and word starts earn,
 * skipped characters cost.
 *
 * The scan is greedy-leftmost rather than optimal.  That is wrong for a needle
 * whose best alignment lies further right, but the labels here are a handful
 * of words and getting it exactly right costs a matrix the core would have to
 * find room for.
 *
 * Returns -1 for no match at all; scores are only ever compared. */
static int score_of(const nchar *hay, const nchar *needle)
{
    /* A base the penalties below can eat into, so that "matched, but poorly"
     * still comes out above -1, which means "did not match at all". */
    int at = 0, score = 1000, prev_end = -1, qi = 0, hits = 0;

    while (needle[qi]) {
        nchar q = lower_ch(needle[qi]);
        int i = at, hit = -1;

        if (q == (nchar)' ') { qi++; continue; }

        while (hay[i]) {
            if (lower_ch(hay[i]) == q) { hit = i; break; }
            i++;
        }
        if (hit < 0) return -1;

        if (hit == prev_end)       score += 15;
        if (is_boundary(hay, hit)) score += 12;
        {
            int gap = hit - at;
            if (gap > 8) gap = 8;      /* one long skip is no worse than eight */
            score -= gap;
        }

        at = hit + 1;
        prev_end = at;
        qi++;
        hits++;
    }

    /* Among rows that all match, the shortest is usually the one meant — but
     * only once something has been typed: with no query at all this would
     * sort the list by label length instead of leaving it as it was built. */
    if (hits) score -= n_len(hay) / 8;
    return score;
}

void note_palette_filter(note_palette *p)
{
    static int score[NOTE_PALETTE_MAX];
    int i, n = 0;

    for (i = 0; i < p->nrows; i++) {
        int s = score_of(p->rows[i].label, note_palette_filter_text(p));
        int j;

        if (s < 0) continue;

        /* Insertion sort: stable, so equal scores keep the order the rows
         * were added in — which for the command list is the menu's own. */
        j = n;
        while (j > 0 && score[j - 1] < s) {
            score[j]    = score[j - 1];
            p->order[j] = p->order[j - 1];
            j--;
        }
        score[j]    = s;
        p->order[j] = (short)i;
        n++;
    }

    p->nshown = n;
}

/* The same greedy walk score_of() makes, reported instead of scored, so a
 * backend highlighting the matched characters marks exactly the ones the
 * ranking was based on. */
int note_palette_marks(const nchar *label, const nchar *query,
                       unsigned char *out, int cap)
{
    int at = 0, qi = 0, n = 0;

    while (query[qi] && n < cap) {
        nchar q = lower_ch(query[qi]);
        int i = at, hit = -1;

        if (q == (nchar)' ') { qi++; continue; }

        while (label[i]) {
            if (lower_ch(label[i]) == q) { hit = i; break; }
            i++;
        }
        if (hit < 0) return 0;
        if (hit > 255) break;          /* out is a byte per position */

        out[n++] = (unsigned char)hit;
        at = hit + 1;
        qi++;
    }
    return n;
}

int note_palette_count(const note_palette *p)
{
    return p->nshown;
}

const note_pal_row *note_palette_at(const note_palette *p, int i)
{
    if (i < 0 || i >= p->nshown) return 0;
    return &p->rows[p->order[i]];
}
