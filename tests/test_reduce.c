/* test_reduce.c — note_theme_reduce, the RGB-to-fixed-palette mapping.
 *
 * A host program, so the CRT is fair game here; the code under test is not.
 *   cl /nologo /W4 /TC tests\test_theme.c src\core\note_theme.c
 *
 * note_theme.c also holds the theme registry, which parses note_conf
 * documents and interns names in an arena.  Linking that in for real would
 * drag note_core.c and note_conf.c and, behind them, a platform; the
 * reduction touches none of it, so the handful of symbols the registry
 * refers to are supplied here instead.
 *
 * Most of what follows is property testing rather than golden values: the
 * guarantee is not "this theme gives these indices" but "for any theme and
 * any palette, these things cannot happen", and a fuzz over generated themes
 * and generated palettes is the only way to say that.
 */

#include <stdio.h>
#include <string.h>

#include "../src/core/note_reduce.h"

/* ---- harness ------------------------------------------------------------ */

static int checks, failures;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL  %s\n", what); }
}

static void okn(int cond, const char *what, long n)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL  %s (n=%ld)\n", what, n); }
}

/* ---- material ----------------------------------------------------------- */

/* The two built-in themes, written out as structs: the parser that would
 * normally build them is stubbed above, and these are the pair every port
 * ships with, so they are the pair the properties matter most for. */
static const note_theme kLight = {
    0, 0,
    0xFFFFFFUL, 0x1F2328UL, 0xF6F8FAUL, 0x8C959FUL,
    0xB4D8FEUL, 0x1F2328UL, 0xF0F0F0UL, 0x1F2328UL,
    { 0x1F2328UL, 0x0550AEUL, 0x1B7C83UL, 0x6E7781UL,
      0x0A6E3BUL, 0x953800UL, 0x8250DFUL, 0x1F2328UL }
};

static const note_theme kDark = {
    0, 1,
    0x1E1E1EUL, 0xD4D4D4UL, 0x1E1E1EUL, 0x6E7681UL,
    0x264F78UL, 0xAEAFADUL, 0x252526UL, 0xCCCCCCUL,
    { 0xD4D4D4UL, 0x569CD6UL, 0x4EC9B0UL, 0x6A9955UL,
      0xCE9178UL, 0xB5CEA8UL, 0xC586C0UL, 0xD4D4D4UL }
};

/* A near-black theme with a gutter one shade off the background, which is the
 * case the brief calls out: nearest-match sends both to the same black. */
static const note_theme kNearBlack = {
    0, 1,
    0x101010UL, 0xC8C8C8UL, 0x141414UL, 0x2A2A2AUL,
    /* A caret only a shade off the background, which is the trap: reversed,
     * it would swallow the character and show nothing in its place. */
    0x1C2A3AUL, 0x1A1A1AUL, 0x121212UL, 0xB0B0B0UL,
    { 0xC8C8C8UL, 0x4080D0UL, 0x30B0A0UL, 0x407040UL,
      0xC08060UL, 0xA0C080UL, 0xB070C0UL, 0xA0A0A0UL }
};

static unsigned long rng = 12345UL;

static unsigned long rnd(void)
{
    rng = rng * 1103515245UL + 12345UL;
    return (rng >> 8) & 0xFFFFFFUL;
}

static void gen_theme(note_theme *t)
{
    int i;
    t->name = 0;
    t->dark = (int)(rnd() & 1);
    t->bg        = rnd();
    t->fg        = rnd();
    t->gutter_bg = rnd();
    t->gutter_fg = rnd();
    t->sel_bg    = rnd();
    t->caret     = rnd();
    t->ui_bg     = rnd();
    t->ui_fg     = rnd();
    for (i = 0; i < TOK_COUNT; i++) t->tok[i] = rnd();
    t->tok[TOK_TEXT] = t->fg;
    /* Themes routinely leave kinds uncoloured and the parser fills those in
     * with the foreground, so token colours that are equal to each other are
     * the ordinary case here, not an edge one. */
    for (i = 1; i < TOK_COUNT; i++)
        if ((rnd() & 3) == 0) t->tok[i] = t->tok[(int)(rnd() % (unsigned long)i)];
}

static int uniq_count(const note_color *pal, int npal)
{
    int i, j, n = 0;
    for (i = 0; i < npal; i++) {
        int seen = 0;
        for (j = 0; j < i; j++) if (pal[j] == pal[i]) seen = 1;
        if (!seen) n++;
    }
    return n;
}

static int distinct_toks(const note_theme_map *m)
{
    int i, j, n = 0;
    for (i = 0; i < TOK_COUNT; i++) {
        int seen = 0;
        for (j = 0; j < i; j++) if (m->tok[j] == m->tok[i]) seen = 1;
        if (!seen) n++;
    }
    return n;
}

/* ---- the properties ----------------------------------------------------- */

/* Everything that must hold for every theme on every palette worth the name.
 * Returns nothing; failures are counted by ok(). */
static void check_map(const note_theme *th, const note_color *pal, int npal,
                      const char *what)
{
    note_theme_map m;
    int nuniq = uniq_count(pal, npal);
    int i, want;

    note_theme_reduce(&m, th, pal, npal);

    okn(m.ncolors == (unsigned char)nuniq, what, (long)m.ncolors);

    for (i = 0; i < TOK_COUNT; i++)
        okn(m.tok[i] < npal, what, (long)m.tok[i]);
    okn(m.bg < npal && m.fg < npal && m.gutter_bg < npal &&
        m.gutter_fg < npal && m.sel_bg < npal && m.caret < npal &&
        m.ui_bg < npal && m.ui_fg < npal, what, (long)m.bg);

    if (nuniq < 2) return;

    /* Nothing that is drawn on something else may be that something else. */
    ok(pal[m.fg] != pal[m.bg], what);
    ok(pal[m.gutter_fg] != pal[m.gutter_bg], what);
    ok(pal[m.ui_fg] != pal[m.ui_bg], what);
    ok(pal[m.sel_bg] != pal[m.bg], what);
    ok(pal[m.caret]  != pal[m.bg], what);
    for (i = 0; i < TOK_COUNT; i++)
        okn(pal[m.tok[i]] != pal[m.bg], what, (long)i);

    /* Plain text and the theme's foreground are one decision, not two. */
    ok(m.tok[TOK_TEXT] == m.fg, what);

    /* ntok is what the output actually holds, not what was hoped for. */
    okn(m.ntok == (unsigned char)distinct_toks(&m), what, (long)m.ntok);

    /* Kinds the theme painted alike stay alike; the reduction spends entries
     * only on distinctions the theme actually made. */
    for (i = 1; i < TOK_COUNT; i++) {
        int j;
        for (j = 0; j < i; j++)
            if (th->tok[j] == th->tok[i])
                okn(m.tok[j] == m.tok[i], what, (long)i);
    }

    /* And the palette is spent on those distinctions up to the point where it
     * runs out: one entry goes to the background and the rest to a kind. */
    want = 0;
    for (i = 0; i < TOK_COUNT; i++) {
        int j, seen = 0;
        for (j = 0; j < i; j++) if (th->tok[j] == th->tok[i]) seen = 1;
        if (!seen) want++;
    }
    if (want > nuniq - 1) want = nuniq - 1;
    okn(m.ntok == (unsigned char)want, what, (long)m.ntok);
}

static void test_builtins(void)
{
    printf("built-in themes on the two hardware palettes\n");
    check_map(&kLight,     note_pal_ega16, 16, "light/ega");
    check_map(&kDark,      note_pal_ega16, 16, "dark/ega");
    check_map(&kNearBlack, note_pal_ega16, 16, "nearblack/ega");
    check_map(&kLight,     note_pal_c64,   16, "light/c64");
    check_map(&kDark,      note_pal_c64,   16, "dark/c64");
    check_map(&kNearBlack, note_pal_c64,   16, "nearblack/c64");
}

/* The bug that started this: on a sixteen-colour desktop the dark theme's
 * keyword blue and comment green are each nearest to some entry, and a
 * per-colour lookup is free to give both the same one. */
static void test_no_collapse(void)
{
    note_theme_map m;

    printf("sixteen colours keep all eight kinds apart\n");

    /* The dark theme paints operators as plain text, so seven is all it
     * asks for and seven is what it should get. */
    note_theme_reduce(&m, &kDark, note_pal_ega16, 16);
    ok(m.ntok == 7, "dark on EGA keeps every colour it asked for");
    ok(m.tok[TOK_KEYWORD] != m.tok[TOK_COMMENT], "keyword is not comment");
    ok(m.tok[TOK_STRING]  != m.tok[TOK_COMMENT], "string is not comment");
    ok(m.gutter_fg != m.gutter_bg, "the gutter has not vanished");

    note_theme_reduce(&m, &kNearBlack, note_pal_ega16, 16);
    ok(m.ntok == 8, "near-black on EGA keeps eight token colours");
    ok(m.gutter_fg != m.gutter_bg, "a dark gutter is still legible");
    ok(m.sel_bg != m.bg, "a dark selection wash still shows");

    note_theme_reduce(&m, &kLight, note_pal_c64, 16);
    ok(m.ntok == 7, "light on the C64 keeps every colour it asked for");
}

/* How far apart two colours read in brightness — the reduction's own measure,
 * repeated here so the test can assert on it without reaching inside. */
static int luma(note_color c)
{
    return (3 * (int)((c >> 16) & 0xFF) + 6 * (int)((c >> 8) & 0xFF)
              + (int)(c & 0xFF)) / 10;
}

static int lgap(note_color a, note_color b)
{
    int d = luma(a) - luma(b);
    return d < 0 ? -d : d;
}

/* The caret and the selection wash both sit behind text and both must differ
 * from the background, but only one of them has to contrast with it.  A
 * selection is read through the text drawn on top; a reversed caret cell IS
 * the colour, with the character punched out of it in the background, so a
 * caret a shade off the background is invisible with a hole in it. */
static void test_caret(void)
{
    note_theme_map m;

    printf("the caret contrasts, the selection only differs\n");

    note_theme_reduce(&m, &kNearBlack, note_pal_ega16, 16);
    ok(m.caret != m.bg, "the caret is not the background");
    okn(lgap(note_pal_ega16[m.caret], note_pal_ega16[m.bg]) >= 72,
        "a near-black caret is pulled clear of the background",
        (long)lgap(note_pal_ega16[m.caret], note_pal_ega16[m.bg]));

    /* The dark theme's own caret is a light grey and needs no rescuing. */
    note_theme_reduce(&m, &kDark, note_pal_ega16, 16);
    okn(lgap(note_pal_ega16[m.caret], note_pal_ega16[m.bg]) >= 72,
        "and a light caret stays light", (long)m.caret);

    note_theme_reduce(&m, &kDark, note_pal_c64, 16);
    ok(m.caret != m.bg, "the same holds on the C64");
    okn(lgap(note_pal_c64[m.caret], note_pal_c64[m.bg]) >= 72,
        "with the same floor", (long)m.caret);

    /* Two colours is the floor, and there the caret can only be the one
     * entry that is not the background. */
    {
        static const note_color p2[2] = { 0x000000UL, 0xFFFFFFUL };
        note_theme_reduce(&m, &kNearBlack, p2, 2);
        ok(m.caret != m.bg && m.caret == m.fg, "two colours leave one caret");
    }
}

/* A theme whose colours are already palette entries must come back as those
 * entries: the reduction is allowed to compromise only when it has to. */
static void test_exact(void)
{
    note_theme t;
    note_theme_map m;
    int i;
    static const unsigned char want[TOK_COUNT] = { 15, 9, 11, 2, 12, 14, 13, 7 };

    printf("colours already in the palette survive unchanged\n");

    t.name = 0; t.dark = 1;
    t.bg = note_pal_ega16[0];
    t.fg = note_pal_ega16[15];
    t.gutter_bg = note_pal_ega16[0];
    t.gutter_fg = note_pal_ega16[8];
    t.sel_bg = note_pal_ega16[1];
    t.caret = note_pal_ega16[15];
    t.ui_bg = note_pal_ega16[8];
    t.ui_fg = note_pal_ega16[15];
    for (i = 0; i < TOK_COUNT; i++) t.tok[i] = note_pal_ega16[want[i]];

    note_theme_reduce(&m, &t, note_pal_ega16, 16);
    ok(m.bg == 0 && m.fg == 15, "chrome recovered");
    ok(m.caret == 15, "caret recovered");
    ok(m.gutter_fg == 8 && m.sel_bg == 1, "gutter and selection recovered");
    for (i = 0; i < TOK_COUNT; i++)
        okn(m.tok[i] == want[i], "token recovered", (long)i);
}

/* Which kinds give up their colour first, and in what order.  These are the
 * merges note_theme.c documents; if they change, this test is the record of
 * the decision and should change with it deliberately. */
static void test_merge_order(void)
{
    /* Four and five distinct colours, well separated so that the assignment
     * has no reason to do anything but the obvious. */
    static const note_color p4[4] = { 0x000000UL, 0xFFFFFFUL,
                                      0xFF0000UL, 0x00FF00UL };
    static const note_color p5[5] = { 0x000000UL, 0xFFFFFFUL, 0xFF0000UL,
                                      0x00FF00UL, 0x0000FFUL };
    note_theme_map m;

    printf("a palette too small merges kinds in the documented order\n");

    note_theme_reduce(&m, &kDark, p5, 5);
    ok(m.ntok == 4, "five colours carry four token colours");
    ok(m.tok[TOK_OPERATOR] == m.tok[TOK_TEXT],    "operator joins text");
    ok(m.tok[TOK_TYPE]     == m.tok[TOK_KEYWORD], "type joins keyword");
    ok(m.tok[TOK_PREPROC]  == m.tok[TOK_KEYWORD], "preproc joins keyword");
    ok(m.tok[TOK_NUMBER]   == m.tok[TOK_STRING],  "number joins string");
    ok(m.tok[TOK_KEYWORD]  != m.tok[TOK_TEXT],    "keyword still its own");
    ok(m.tok[TOK_COMMENT]  != m.tok[TOK_TEXT],    "comment still its own");
    ok(m.tok[TOK_COMMENT]  != m.tok[TOK_STRING],  "comment is not string");

    note_theme_reduce(&m, &kDark, p4, 4);
    ok(m.ntok == 3, "four colours carry three token colours");
    ok(m.tok[TOK_KEYWORD] == m.tok[TOK_TEXT], "keyword falls back to text");
    ok(m.tok[TOK_TYPE]    == m.tok[TOK_TEXT], "and takes type with it");
    ok(m.tok[TOK_PREPROC] == m.tok[TOK_TEXT], "and preproc");
    ok(m.tok[TOK_NUMBER]  == m.tok[TOK_STRING], "literals stay together");
    ok(m.tok[TOK_STRING]  != m.tok[TOK_TEXT], "literals stay their own");
    /* The last distinction to go, and it does not go here. */
    ok(m.tok[TOK_COMMENT] != m.tok[TOK_TEXT],   "comment outlives keyword");
    ok(m.tok[TOK_COMMENT] != m.tok[TOK_STRING], "comment outlives literals");

    check_map(&kDark,  p4, 4, "dark/4");
    check_map(&kLight, p4, 4, "light/4");
    check_map(&kDark,  p5, 5, "dark/5");
}

/* Palettes that are not what they claim: two colours, one colour, sixteen
 * entries holding four.  None of these may read out of range or loop. */
/* A theme that colours three kinds and lets the rest fall back to the
 * foreground, which is what note_theme_add does with a .theme file that omits
 * a key.  The kinds that fell back must stay with the text and must not drag
 * the text colour off with them when the palette forces further merges. */
static void test_half_written(void)
{
    static const note_color p4[4] = { 0x000000UL, 0xFFFFFFUL,
                                      0xFF0000UL, 0x00FF00UL };
    note_theme t;
    note_theme_map m;
    int i;

    printf("a theme that colours only some kinds\n");

    t.name = 0; t.dark = 1;
    t.bg = 0x1E1E1EUL; t.fg = 0xD4D4D4UL;
    t.gutter_bg = 0x1E1E1EUL; t.gutter_fg = 0x6E7681UL;
    t.sel_bg = 0x264F78UL; t.caret = t.fg; t.ui_bg = 0x252526UL; t.ui_fg = 0xCCCCCCUL;
    for (i = 0; i < TOK_COUNT; i++) t.tok[i] = t.fg;
    t.tok[TOK_KEYWORD] = 0x569CD6UL;
    t.tok[TOK_COMMENT] = 0x6A9955UL;
    t.tok[TOK_STRING]  = 0xCE9178UL;

    note_theme_reduce(&m, &t, note_pal_ega16, 16);
    ok(m.ntok == 4, "four colours asked for, four given");
    ok(m.tok[TOK_TEXT] == m.fg, "text is still the foreground");
    ok(m.tok[TOK_TYPE] == m.fg, "an uncoloured kind stays with the text");
    ok(m.tok[TOK_NUMBER] == m.fg, "and so does another");

    /* Four colours: types are already text, so "merge types into keywords"
     * must not take the text colour with them. */
    note_theme_reduce(&m, &t, p4, 4);
    ok(m.tok[TOK_TEXT] == m.fg, "text survives the merges");
    ok(m.tok[TOK_COMMENT] != m.tok[TOK_TEXT], "comment is still its own");

    check_map(&t, note_pal_ega16, 16, "half/ega");
    check_map(&t, p4, 4, "half/4");
    check_map(&t, note_pal_c64, 16, "half/c64");
}

static void test_degenerate(void)
{
    static const note_color p1[1]  = { 0x336699UL };
    static const note_color p2[2]  = { 0x000000UL, 0xFFFFFFUL };
    note_color dup[16];
    note_theme_map m;
    int i;

    printf("degenerate palettes\n");

    note_theme_reduce(&m, &kDark, p1, 1);
    ok(m.ncolors == 1 && m.bg == 0 && m.fg == 0, "one colour is one colour");
    ok(m.ntok == 1, "and one token colour");

    note_theme_reduce(&m, &kDark, p2, 2);
    ok(m.ncolors == 2 && m.fg != m.bg, "two colours separate fg from bg");
    ok(m.ntok == 1, "leaving nothing for a second token colour");
    for (i = 0; i < TOK_COUNT; i++)
        ok(m.tok[i] != m.bg, "no token lands on the background");

    /* Sixteen entries, four colours: the reduction must count colours, not
     * entries, or it will hand two kinds different indices that paint the
     * same pixel. */
    for (i = 0; i < 16; i++) dup[i] = p2[0];
    for (i = 4; i < 16; i++) dup[i] = note_pal_ega16[i & 3];
    dup[0] = 0x000000UL; dup[1] = 0xFFFFFFUL;
    dup[2] = 0xFF0000UL; dup[3] = 0x00FF00UL;
    for (i = 4; i < 16; i++) dup[i] = dup[i & 3];
    note_theme_reduce(&m, &kDark, dup, 16);
    ok(m.ncolors == 4, "sixteen entries, four colours");
    ok(m.ntok == 3, "and three token colours");
    check_map(&kDark, dup, 16, "dark/dup16");

    note_theme_reduce(&m, &kDark, note_pal_ega16, 0);
    ok(m.ncolors == 1, "an empty palette is clamped to one entry");
}

static void test_stable(void)
{
    note_theme_map a, b;

    printf("the same input gives the same answer\n");
    note_theme_reduce(&a, &kDark, note_pal_ega16, 16);
    note_theme_reduce(&b, &kDark, note_pal_ega16, 16);
    ok(memcmp(&a, &b, sizeof a) == 0, "reduction is deterministic");
}

/* The property claim itself: any theme, any palette of four colours or more.
 * The themes are random RGB, which is harsher than any theme a person would
 * write — colours that are all nearly the same, backgrounds brighter than
 * their text — and the palettes are random too. */
static void test_fuzz(void)
{
    note_theme t;
    note_color pal[16];
    int iter, i, npal;

    printf("fuzz: random themes on random palettes\n");

    for (iter = 0; iter < 4000; iter++) {
        gen_theme(&t);
        npal = 4 + (int)(rnd() % 13);
        for (i = 0; i < npal; i++) pal[i] = rnd();
        if (uniq_count(pal, npal) < 4) continue;
        check_map(&t, pal, npal, "fuzz");
    }

    /* Again, but on the palettes that actually exist. */
    for (iter = 0; iter < 2000; iter++) {
        gen_theme(&t);
        check_map(&t, (iter & 1) ? note_pal_c64 : note_pal_ega16, 16,
                  "fuzz/hardware");
    }

    /* And with themes drawn from a narrow band, where every colour is a near
     * neighbour of every other and keeping them apart costs the most. */
    for (iter = 0; iter < 2000; iter++) {
        gen_theme(&t);
        t.bg = 0x101010UL;
        for (i = 1; i < TOK_COUNT; i++)
            t.tok[i] = 0x303030UL + (rnd() & 0x0F0F0FUL);
        t.fg = 0x383838UL;
        t.tok[TOK_TEXT] = t.fg;
        check_map(&t, note_pal_ega16, 16, "fuzz/narrow");
    }
}

int main(void)
{
    printf("note_theme reduction tests\n");

    test_builtins();
    test_no_collapse();
    test_caret();
    test_exact();
    test_merge_order();
    test_half_written();
    test_degenerate();
    test_stable();
    test_fuzz();

    printf("\n%d checks, %d failed\n", checks, failures);
    if (failures) { printf("FAILED\n"); return 1; }
    printf("all passed\n");
    return 0;
}
