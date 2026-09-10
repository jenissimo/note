/* note_reduce.c — mapping a theme onto a palette the hardware chose.
 *
 * No OS headers, no CRT, no allocation, and no arithmetic that assumes an int
 * is wider than sixteen bits; see note_reduce.h for what this is for.
 */
#include "note_reduce.h"


/* CGA/EGA/VGA index order, which is also the order Windows lists its sixteen
 * system colours in at 4 bits per pixel. */
const note_color note_pal_ega16[16] = {
    0x000000UL, 0x000080UL, 0x008000UL, 0x008080UL,
    0x800000UL, 0x800080UL, 0x808000UL, 0xC0C0C0UL,
    0x808080UL, 0x0000FFUL, 0x00FF00UL, 0x00FFFFUL,
    0xFF0000UL, 0xFF00FFUL, 0xFFFF00UL, 0xFFFFFFUL
};

/* The VIC-II's sixteen, in the order the C64 numbers them.  The RGB is the
 * measured "Pepto" set rather than the idealised one: the point of reducing
 * to this palette is to predict what the screen will look like, and the
 * C64's colours are muddier than their names suggest — its "red" is a brick
 * and its "yellow" a pale olive, which is why nearest-match against the
 * bright names gets the wrong entries. */
const note_color note_pal_c64[16] = {
    0x000000UL, 0xFFFFFFUL, 0x68372BUL, 0x70A4B2UL,
    0x6F3D86UL, 0x588D43UL, 0x352879UL, 0xB8C76FUL,
    0x6F4F25UL, 0x433900UL, 0x9A6759UL, 0x444444UL,
    0x6C6C6CUL, 0x9AD284UL, 0x6C5EB5UL, 0x959595UL
};

#define PAL_R(c) ((int)(((c) >> 16) & 0xFFUL))
#define PAL_G(c) ((int)(((c) >>  8) & 0xFFUL))
#define PAL_B(c) ((int)( (c)        & 0xFFUL))

/* Green carries most of the eye's sense of brightness and blue almost none,
 * so a plain sum of squares would call two blues further apart than two
 * greens that are visibly further apart.  3:6:1 is the cheapest weighting
 * that gets the order right; the worst case is 10 * 255 * 255, which is why
 * this is long arithmetic and not int — int is sixteen bits on two of the
 * four targets. */
static long pal_dist(note_color a, note_color b)
{
    long dr = (long)PAL_R(a) - PAL_R(b);
    long dg = (long)PAL_G(a) - PAL_G(b);
    long db = (long)PAL_B(a) - PAL_B(b);
    return 3L * dr * dr + 6L * dg * dg + 1L * db * db;
}

/* What "close enough" means everywhere below: distance, plus the charge for
 * answering a grey with a colour.  Defined after the two halves. */
static long pal_chroma_pen(note_color want, note_color got);

static long pal_cost(note_color want, note_color got)
{
    return pal_dist(want, got) + pal_chroma_pen(want, got);
}

static int pal_luma(note_color c)
{
    return (3 * PAL_R(c) + 6 * PAL_G(c) + PAL_B(c)) / 10;
}

/* How far from grey a colour is. */
static int pal_chroma(note_color c)
{
    int r = PAL_R(c), g = PAL_G(c), b = PAL_B(c);
    int hi = r > g ? r : g, lo = r < g ? r : g;
    if (b > hi) hi = b;
    if (b < lo) lo = b;
    /* Clamped, because past a point everything simply reads as "a colour":
     * the distinction worth charging for is grey against not-grey, not one
     * saturated hue against a more saturated one.  Without the clamp a teal
     * would refuse a cyan on the grounds that cyan is too vivid. */
    if (hi - lo > 128) return 128;
    return hi - lo;
}

/* Weighted distance alone will answer a grey with a colour.  The C64's
 * "yellow" is a pale olive, near enough to a light grey in raw RGB to beat
 * the machine's own white — and on screen it is unmistakably yellow, which is
 * not what a theme asking for grey text wanted.  Distance does not see
 * saturation at all, so it is charged for separately: wanting a grey and
 * being offered a colour costs, and so does the reverse. */
static long pal_chroma_pen(note_color want, note_color got)
{
    long d = (long)pal_chroma(want) - pal_chroma(got);
    if (d < 0) d = -d;
    return d * d * 4L;
}

/* Being a different palette entry is not the same as being legible on it.
 * Text one shade off its background is gone as surely as text exactly on it,
 * and on a coarse palette that pairing is easy to fall into, so wanting a
 * colour that is close to what it sits on costs something.  Below the floor
 * the charge grows as the square of the shortfall, so it is a nudge at the
 * edge and a refusal at the middle. */
#define PAL_CONTRAST 72

static long pal_contrast_pen(note_color c, note_color on)
{
    int d = pal_luma(c) - pal_luma(on);
    if (d < 0) d = -d;
    if (d >= PAL_CONTRAST) return 0;
    return (long)(PAL_CONTRAST - d) * (PAL_CONTRAST - d) * 16L;
}

/* Which entry of the palette first held this entry's colour.  Everything here
 * compares canonical indices, so a palette that lists the same colour twice
 * cannot be talked into handing out "two" colours that are one. */
static int pal_canon(const note_color *pal, int i)
{
    int j;
    for (j = 0; j < i; j++)
        if (pal[j] == pal[i]) return j;
    return i;
}

static int pal_banned(const unsigned char *ban, int nban, int i)
{
    int k;
    for (k = 0; k < nban; k++)
        if ((int)ban[k] == i) return 1;
    return 0;
}

/* The nearest canonical entry to `want` that is not in `ban`, charged for
 * sitting too close to pal[on] when `on` is not negative.  If the ban list
 * has taken everything, the ban is dropped rather than the answer: a wrong
 * colour is still a colour, and returning -1 here would only push the same
 * decision onto the caller. */
static int pal_pick(const note_color *pal, int npal, note_color want,
                    int on, const unsigned char *ban, int nban)
{
    long best = 0, cost;
    int  bi = -1, i, pass;

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < npal; i++) {
            if (pal_canon(pal, i) != i) continue;
            if (pass == 0 && pal_banned(ban, nban, i)) continue;
            cost = pal_cost(want, pal[i]);
            if (on >= 0) cost += pal_contrast_pen(pal[i], pal[on]);
            if (bi < 0 || cost < best) { best = cost; bi = i; }
        }
        if (bi >= 0) return bi;
    }
    return 0;
}

/* The order token kinds give up having a colour of their own.
 *
 * Sixteen entries minus a background does not leave eight comfortable
 * choices, and on a four-colour display it leaves three.  Letting the
 * arithmetic decide which kinds collide gives a different, arbitrary loss for
 * every theme; deciding here gives the same, defensible one for all of them.
 *
 * Each row merges the first kind into the second, and the second keeps its
 * own colour as the pair's.  The sequence runs from the merges nobody
 * notices to the ones that hurt:
 *   operators are punctuation, and most themes already paint them as text;
 *   a type is a name the language knows, which is what a keyword is;
 *   a directive reads as a keyword;
 *   a number and a string are both literals;
 *   and only then do keywords, and last of all strings, fall back to text.
 * Comments are never merged into anything.  With two colours left, "this is
 * code and this is prose about it" is the distinction worth keeping, and a
 * keyword painted as text is still a keyword while a comment painted as code
 * is a trap. */
static const unsigned char kPalMerge[6][2] = {
    { TOK_OPERATOR, TOK_TEXT    },
    { TOK_TYPE,     TOK_KEYWORD },
    { TOK_PREPROC,  TOK_KEYWORD },
    { TOK_NUMBER,   TOK_STRING  },
    { TOK_KEYWORD,  TOK_TEXT    },
    { TOK_STRING,   TOK_TEXT    }
};

/* Landing on the selection wash is not fatal the way landing on the
 * background is — it costs a kind its colour only while it is selected — so
 * it is priced rather than forbidden.  Roughly the cost of being one strong
 * shade wrong. */
#define PAL_SEL_PEN 30000L

void note_theme_reduce(note_theme_map *out, const note_theme *th,
                       const note_color *pal, int npal)
{
    unsigned char grp[TOK_COUNT];      /* kind -> the kind it shares with  */
    unsigned char reps[TOK_COUNT];     /* the distinct group leaders       */
    unsigned char used[TOK_COUNT + 2]; /* canonical indices spoken for     */
    short         at[TOK_COUNT];       /* rep -> palette index, -1 if not  */
    int nuniq = 0, ngrp, nreps, nused = 0;
    int i, k, r, cap, pass;

    if (npal < 1) npal = 1;
    if (npal > NOTE_PAL_MAX) npal = NOTE_PAL_MAX;
    for (i = 0; i < npal; i++)
        if (pal_canon(pal, i) == i) nuniq++;

    /* Chrome first, and in this order, because every later choice is made
     * against an earlier one: the background is the only field with nothing
     * to be legible against, and the text colour anchors the token pass. */
    out->bg = (unsigned char)pal_pick(pal, npal, th->bg, -1, (const unsigned char *)0, 0);
    used[nused++] = out->bg;
    out->fg = (unsigned char)pal_pick(pal, npal, th->fg, out->bg, used, nused);

    /* The gutter's own background is allowed to collapse onto the editor's.
     * Themes separate the two by a shade or two and a coarse palette cannot
     * hold that shade; what must survive is the line numbers against
     * whichever entry they end up on, and that is the next line. */
    out->gutter_bg = (unsigned char)pal_pick(pal, npal, th->gutter_bg,
                                             -1, (const unsigned char *)0, 0);
    out->gutter_fg = (unsigned char)pal_pick(pal, npal, th->gutter_fg,
                                             out->gutter_bg,
                                             &out->gutter_bg, 1);
    /* A wash the same colour as what it washes over is not a wash — but it
     * only has to differ, not to contrast.  The legibility of a selection is
     * carried by the text drawn on top of it, and charging a wash for being
     * close to the background would push a navy selection on black towards
     * some brighter colour nobody asked for. */
    out->sel_bg = (unsigned char)pal_pick(pal, npal, th->sel_bg,
                                          -1, &out->bg, 1);
    /* A caret is the opposite case, and gets the foreground's rule rather
     * than the selection's.  The retro backends draw it as a reversed cell:
     * the block is this colour and the character it swallows is punched out
     * of it in the background colour, so a caret a shade off the background
     * is not a dim caret, it is an invisible one with a hole in it. */
    out->caret = (unsigned char)pal_pick(pal, npal, th->caret,
                                         out->bg, &out->bg, 1);
    out->ui_bg  = (unsigned char)pal_pick(pal, npal, th->ui_bg, -1, (const unsigned char *)0, 0);
    out->ui_fg  = (unsigned char)pal_pick(pal, npal, th->ui_fg,
                                          out->ui_bg, &out->ui_bg, 1);

    /* One entry is the background, which no token may take, and one is the
     * text colour, which the TOK_TEXT group already holds — so the palette
     * carries nuniq - 1 token colours in all, counting text's. */
    cap = nuniq - 1;
    if (cap < 1) cap = 1;
    if (cap > TOK_COUNT) cap = TOK_COUNT;

    /* Kinds the theme painted the same colour stay the same colour.  Keeping
     * kinds apart is worth a shade of fidelity, but only where the theme
     * meant them to be apart: most themes paint operators as ordinary text,
     * and prising those two onto different entries would spend a palette
     * entry on a distinction the author declined to make — and take it from
     * one they did. */
    for (k = 0; k < TOK_COUNT; k++) grp[k] = (unsigned char)k;
    ngrp = TOK_COUNT;
    for (k = 1; k < TOK_COUNT; k++)
        for (i = 0; i < k; i++)
            if (th->tok[i] == th->tok[k]) {
                grp[k] = grp[i];
                ngrp--;
                break;
            }

    for (i = 0; i < 6 && ngrp > cap; i++) {
        unsigned char from = grp[kPalMerge[i][0]], to = grp[kPalMerge[i][1]], t;
        if (from == to) continue;
        /* A theme that leaves a kind uncoloured gets the text colour for it,
         * so the pass above can already have put, say, types in with plain
         * text — and then "merge types into keywords" would carry TOK_TEXT
         * off into the keyword group and leave the foreground unpinned.
         * Whichever side holds TOK_TEXT is the side that survives. */
        if (from == grp[TOK_TEXT]) { t = from; from = to; to = t; }
        for (k = 0; k < TOK_COUNT; k++)
            if (grp[k] == from) grp[k] = to;
        ngrp--;
    }

    nreps = 0;
    for (k = 0; k < TOK_COUNT; k++)
        if (grp[k] == (unsigned char)k) {
            at[nreps] = -1;
            reps[nreps++] = (unsigned char)k;
        }

    /* TOK_TEXT never merges into anything, so its group leads with itself and
     * takes the foreground already chosen.  Pinning it costs nothing and
     * keeps "plain text" and "the theme's foreground" from drifting apart. */
    for (r = 0; r < nreps; r++)
        if (reps[r] == TOK_TEXT) at[r] = out->fg;
    used[nused++] = out->fg;

    /* Assignment, not eight lookups.  Repeatedly commit the cheapest
     * (kind, entry) pair still open: that alone beats first-come nearest
     * match, because a kind with a good match nobody else wants gets it
     * whatever order the kinds are listed in. */
    for (;;) {
        long best = 0;
        int  br = -1, bi = -1;
        for (r = 0; r < nreps; r++) {
            if (at[r] >= 0) continue;
            for (i = 0; i < npal; i++) {
                long cost;
                if (pal_canon(pal, i) != i) continue;
                if (pal_banned(used, nused, i)) continue;
                cost = pal_cost(th->tok[reps[r]], pal[i])
                     + pal_contrast_pen(pal[i], pal[out->bg]);
                if (i == (int)out->sel_bg) cost += PAL_SEL_PEN;
                if (br < 0 || cost < best) { best = cost; br = r; bi = i; }
            }
        }
        if (br < 0) break;
        at[br] = (short)bi;
        used[nused++] = (unsigned char)bi;
    }

    /* Greedy commits early and cannot take a commitment back, so it can spend
     * the one good green on a kind that had a passable alternative and leave
     * a kind with no alternative at all stranded.  Swapping two kinds'
     * entries is the only move that fixes that without disturbing anyone
     * else, and a handful of passes settles eight of them. */
    for (pass = 0; pass < TOK_COUNT; pass++) {
        int moved = 0;
        for (r = 0; r < nreps; r++) {
            int s;
            if (at[r] < 0 || reps[r] == TOK_TEXT) continue;
            for (s = r + 1; s < nreps; s++) {
                long now, swapped;
                if (at[s] < 0 || reps[s] == TOK_TEXT) continue;
                now = pal_cost(th->tok[reps[r]], pal[at[r]])
                    + pal_cost(th->tok[reps[s]], pal[at[s]]);
                swapped = pal_cost(th->tok[reps[r]], pal[at[s]])
                        + pal_cost(th->tok[reps[s]], pal[at[r]]);
                if (swapped < now) {
                    short t = at[r]; at[r] = at[s]; at[s] = t;
                    moved = 1;
                }
            }
        }
        if (!moved) break;
    }

    /* A group with no entry left can only be text; cap keeps that from
     * happening, but the fallback is here rather than assumed. */
    for (k = 0; k < TOK_COUNT; k++) {
        out->tok[k] = out->fg;
        for (r = 0; r < nreps; r++)
            if (reps[r] == grp[k] && at[r] >= 0)
                out->tok[k] = (unsigned char)at[r];
    }

    out->ncolors = (unsigned char)(nuniq > 255 ? 255 : nuniq);

    /* Counted from the answer rather than from the plan: a palette with
     * fewer entries than the merge table has merges runs out of both. */
    ngrp = 0;
    for (k = 0; k < TOK_COUNT; k++) {
        int seen = 0;
        for (i = 0; i < k; i++) if (out->tok[i] == out->tok[k]) seen = 1;
        if (!seen) ngrp++;
    }
    out->ntok = (unsigned char)ngrp;
}
