/* test_regex.c — a host program that exercises note_regex.
 *
 * Unlike the core, this file is an ordinary CRT program: it prints, times and
 * counts.  Build it with, from a VS command prompt:
 *
 *   cl /nologo /W4 /TC tests\test_regex.c src\core\note_regex.c /Fe:test_regex.exe
 *
 * It exits non-zero if anything failed.
 */

#define _CRT_SECURE_NO_WARNINGS 1

#include <stdio.h>
#include <time.h>

#include "../src/core/note_regex.h"

static int g_run;
static int g_failed;
static const char *g_case = "";

static void ok(int cond, const char *what)
{
    g_run++;
    if (!cond) {
        g_failed++;
        printf("  FAIL  %-28s %s\n", g_case, what);
    }
}

/* Prints an nchar string, assuming the ASCII the tests are written in. */
static void nprint(const nchar *s)
{
    while (*s) {
        putchar((int)(unsigned char)*s);
        s++;
    }
}

/* ------------------------------------------------------------------------ */

static note_regex g_re;

/* Compiles, expecting success. */
static int compile(const nchar *pat, unsigned flags)
{
    if (note_regex_compile(&g_re, pat, flags)) return 1;
    g_run++;
    g_failed++;
    printf("  FAIL  %-28s pattern ", g_case);
    nprint(pat);
    printf(" did not compile: ");
    nprint(note_regex_error(&g_re));
    printf("\n");
    return 0;
}

/* pat must match text at exactly [ms,me). */
static void match_at(const nchar *pat, unsigned flags, const nchar *text,
                     int ms, int me)
{
    int s = -2, e = -2;
    if (!compile(pat, flags)) return;
    if (!note_regex_search(&g_re, text, -1, 0, &s, &e, 0, 0)) {
        g_run++;
        g_failed++;
        printf("  FAIL  %-28s ", g_case);
        nprint(pat);
        printf(" found nothing in \"");
        nprint(text);
        printf("\"\n");
        return;
    }
    g_run++;
    if (s != ms || e != me) {
        g_failed++;
        printf("  FAIL  %-28s ", g_case);
        nprint(pat);
        printf(" on \"");
        nprint(text);
        printf("\" gave %d..%d, wanted %d..%d\n", s, e, ms, me);
    }
}

static void no_match(const nchar *pat, unsigned flags, const nchar *text)
{
    int s = -2, e = -2;
    if (!compile(pat, flags)) return;
    g_run++;
    if (note_regex_search(&g_re, text, -1, 0, &s, &e, 0, 0)) {
        g_failed++;
        printf("  FAIL  %-28s ", g_case);
        nprint(pat);
        printf(" matched \"");
        nprint(text);
        printf("\" at %d..%d, wanted no match\n", s, e);
    }
}

/* A pattern the compiler must reject, with a reason. */
static void bad(const nchar *pat)
{
    g_run++;
    if (note_regex_compile(&g_re, pat, 0)) {
        g_failed++;
        printf("  FAIL  %-28s ", g_case);
        nprint(pat);
        printf(" compiled, but is malformed\n");
        return;
    }
    if (!note_regex_error(&g_re) || !*note_regex_error(&g_re)) {
        g_failed++;
        printf("  FAIL  %-28s ", g_case);
        nprint(pat);
        printf(" was rejected with no message\n");
    }
}

/* Checks one capture group's offsets. */
static void group_is(const int *g, int k, int s, int e)
{
    char buf[64];
    sprintf(buf, "group %d == %d..%d (got %d..%d)", k, s, e, g[k * 2], g[k * 2 + 1]);
    ok(g[k * 2] == s && g[k * 2 + 1] == e, buf);
}

/* ------------------------------------------------------------------------ */

static void test_literals(void)
{
    g_case = "literals";
    match_at(N("hello"), 0, N("say hello there"), 4, 9);
    match_at(N("a"), 0, N("a"), 0, 1);
    match_at(N(""), 0, N("abc"), 0, 0);
    no_match(N("hello"), 0, N("say hell there"));
    no_match(N("abc"), 0, N(""));
    /* Escaped punctuation is a literal. */
    match_at(N("a\\.b"), 0, N("axb a.b"), 4, 7);
    match_at(N("\\$\\(x\\)"), 0, N("y $(x)"), 2, 6);
    match_at(N("a\\tb"), 0, N("a\tb"), 0, 3);
    match_at(N("a\\\\b"), 0, N("a\\b"), 0, 3);
}

static void test_dot(void)
{
    g_case = "dot";
    match_at(N("h.llo"), 0, N("hzllo"), 0, 5);
    match_at(N("a.c"), 0, N("xxabc"), 2, 5);
    no_match(N("a.c"), 0, N("ac"));
    no_match(N("a.c"), 0, N("a\nc"));
    match_at(N("a.c"), NRE_DOTALL, N("a\nc"), 0, 3);
    match_at(N("^.*$"), 0, N("abc"), 0, 3);
}

static void test_classes(void)
{
    g_case = "classes";
    match_at(N("[abc]+"), 0, N("xxcabz"), 2, 5);
    match_at(N("[a-z]+"), 0, N("12abc34"), 2, 5);
    match_at(N("[a-cx-z]+"), 0, N("qqabxyzqq"), 2, 7);
    match_at(N("[^0-9]+"), 0, N("12abc34"), 2, 5);
    match_at(N("[]]"), 0, N("a]b"), 1, 2);
    match_at(N("[a-]+"), 0, N("x-a-x"), 1, 4);
    match_at(N("[\\]\\[]+"), 0, N("x[]y"), 1, 3);
    match_at(N("[\\d]+"), 0, N("ab123"), 2, 5);
    match_at(N("[\\dA-F]+"), 0, N("zz1F2z"), 2, 5);
    match_at(N("[^\\s]+"), 0, N("  ab  "), 2, 4);
    match_at(N("[\\t ]+"), 0, N("a \t b"), 1, 4);
    no_match(N("[^a-z]"), 0, N("abc"));

    g_case = "escapes";
    match_at(N("\\d+"), 0, N("ab 4711 cd"), 3, 7);
    match_at(N("\\D+"), 0, N("42abc"), 2, 5);
    match_at(N("\\w+"), 0, N("  a_b9 "), 2, 6);
    match_at(N("\\W+"), 0, N("ab  ,cd"), 2, 5);
    match_at(N("\\s+"), 0, N("ab \t cd"), 2, 5);
    match_at(N("\\S+"), 0, N("  ab  "), 2, 4);
    match_at(N("a\\nb"), 0, N("a\nb"), 0, 3);
    match_at(N("a\\r\\nb"), 0, N("a\r\nb"), 0, 4);
}

static void test_anchors(void)
{
    g_case = "anchors";
    match_at(N("^abc"), 0, N("abcabc"), 0, 3);
    no_match(N("^bc"), 0, N("abc"));
    match_at(N("abc$"), 0, N("xabc"), 1, 4);
    no_match(N("ab$"), 0, N("abc"));
    match_at(N("^$"), 0, N(""), 0, 0);
    no_match(N("^$"), 0, N("a"));

    /* Line relative only under NRE_MULTILINE. */
    no_match(N("^b"), 0, N("a\nb"));
    match_at(N("^b"), NRE_MULTILINE, N("a\nb"), 2, 3);
    match_at(N("a$"), NRE_MULTILINE, N("a\nb"), 0, 1);
    match_at(N("a$"), NRE_MULTILINE, N("xa\r\nb"), 1, 2);
    match_at(N("^\\w+$"), NRE_MULTILINE, N("+\nword\n+"), 2, 6);

    g_case = "word boundary";
    match_at(N("\\bcat\\b"), 0, N("a cat here"), 2, 5);
    no_match(N("\\bcat\\b"), 0, N("concatenate"));
    match_at(N("\\bcat"), 0, N("the catalog"), 4, 7);
    match_at(N("\\Bcat"), 0, N("concat"), 3, 6);
    no_match(N("\\Bcat"), 0, N("a cat"));
    match_at(N("\\ba"), 0, N(" a"), 1, 2);          /* must not stop at pos 0 */
    match_at(N("\\bx\\b"), 0, N("a x b"), 2, 3);
    /* An empty string has no word in it, so it is all \B and no \b.  This is
     * what Perl, PCRE and Python 3.12 onwards say. */
    no_match(N("\\b"), 0, N(""));
    match_at(N("\\B"), 0, N(""), 0, 0);
    match_at(N("\\B"), 0, N("ab"), 1, 1);
    no_match(N("\\b\\B"), 0, N("ab"));
}

/* A lazy quantifier at the head of an alternation branch used to be able to
 * jump into the other branch, because inserting the alternation's split in
 * front of the branch moved the loop's own back-jump target.  These pin the
 * shape down. */
static void test_branch_heads(void)
{
    g_case = "branch heads";
    match_at(N("\\s*?[abc]{2}|[a-c]"), 0, N("\ta"), 1, 2);
    match_at(N("\\s*?[abc]{2}|[a-c]"), 0, N("\tab"), 0, 3);
    match_at(N("a*b|c"), 0, N("xc"), 1, 2);
    match_at(N("a*?b|c"), 0, N("xc"), 1, 2);
    match_at(N("a+b|c"), 0, N("aac"), 2, 3);
    match_at(N("(?:a*)*b|z"), 0, N("qz"), 1, 2);
    match_at(N("a{0,2}b|z"), 0, N("qz"), 1, 2);
    match_at(N("a{2,}b|z"), 0, N("qz"), 1, 2);
    match_at(N("x|a*?b"), 0, N("qaab"), 1, 4);
    match_at(N("x|a*b|y"), 0, N("qy"), 1, 2);
    no_match(N("a*b|c"), 0, N("aaq"));
    match_at(N("\\s*?[abc]{2}|[^a-z]"), 0, N("\ta"), 0, 1);   /* the tab */
    no_match(N("\\s*?[abc]{2}|[q-z]"), 0, N("\ta"));
}

static void test_groups(void)
{
    int g[NOTE_REGEX_SLOTS];
    int s, e;

    g_case = "groups";
    match_at(N("(abc)"), 0, N("xabc"), 1, 4);
    match_at(N("(?:abc)+"), 0, N("xabcabc"), 1, 7);
    match_at(N("a(bc)?d"), 0, N("ad"), 0, 2);
    match_at(N("(a|b)(c|d)"), 0, N("zbd"), 1, 3);
    match_at(N("(a(b(c)))"), 0, N("abc"), 0, 3);

    g_case = "captures";
    if (compile(N("(\\w+)@(\\w+)"), 0)) {
        ok(note_regex_groups(&g_re) == 2, "two groups reported");
        ok(note_regex_search(&g_re, N("mail bob@host x"), -1, 0, &s, &e,
                             g, NOTE_REGEX_GROUPS), "found bob@host");
        ok(s == 5 && e == 13, "whole match 5..13");
        group_is(g, 0, 5, 13);
        group_is(g, 1, 5, 8);
        group_is(g, 2, 9, 13);
    }

    g_case = "unset group";
    if (compile(N("(a)(b)?c"), 0)) {
        ok(note_regex_search(&g_re, N("zac"), -1, 0, &s, &e, g,
                             NOTE_REGEX_GROUPS), "matched ac");
        ok(s == 1 && e == 3, "whole match 1..3");
        group_is(g, 1, 1, 2);
        group_is(g, 2, -1, -1);
    }

    g_case = "repeated group";
    if (compile(N("(?:(a)|(b))+"), 0)) {
        ok(note_regex_search(&g_re, N("aba"), -1, 0, &s, &e, g,
                             NOTE_REGEX_GROUPS), "matched aba");
        ok(s == 0 && e == 3, "whole match 0..3");
        group_is(g, 1, 2, 3);   /* last iteration wins */
        group_is(g, 2, 1, 2);
    }

    g_case = "start offset";
    if (compile(N("a+"), 0)) {
        ok(note_regex_search(&g_re, N("aa bb aaa"), -1, 3, &s, &e, 0, 0),
           "searched from 3");
        ok(s == 6 && e == 9, "second run found");
        ok(!note_regex_search(&g_re, N("aa bb"), -1, 3, &s, &e, 0, 0),
           "nothing after 3");
    }
}

static void test_alternation(void)
{
    g_case = "alternation";
    match_at(N("cat|dog"), 0, N("a dog here"), 2, 5);
    match_at(N("cat|dog|bird"), 0, N("a bird"), 2, 6);
    match_at(N("a|ab"), 0, N("ab"), 0, 1);          /* leftmost, first branch */
    match_at(N("ab|a"), 0, N("ab"), 0, 2);
    match_at(N("x|(?:a|b)y"), 0, N("zby"), 1, 3);
    match_at(N("^(?:foo|bar)$"), 0, N("bar"), 0, 3);
    match_at(N("(a|)b"), 0, N("b"), 0, 1);
    no_match(N("cat|dog"), 0, N("a cow"));
}

static void test_quantifiers(void)
{
    int g[NOTE_REGEX_SLOTS];
    int s, e;

    g_case = "greedy";
    match_at(N("a*"), 0, N("aaa"), 0, 3);
    match_at(N("a*"), 0, N("bbb"), 0, 0);
    match_at(N("a+"), 0, N("bbaaab"), 2, 5);
    match_at(N("a?b"), 0, N("xab"), 1, 3);
    match_at(N("<.*>"), 0, N("<a><b>"), 0, 6);
    match_at(N("[a-z]*[0-9]"), 0, N("abc1"), 0, 4);

    g_case = "lazy";
    match_at(N("a+?"), 0, N("aaa"), 0, 1);
    match_at(N("a*?b"), 0, N("aab"), 0, 3);
    match_at(N("<.*?>"), 0, N("<a><b>"), 0, 3);
    match_at(N("<.+?>"), 0, N("<a><b>"), 0, 3);
    match_at(N("a??b"), 0, N("ab"), 0, 2);

    g_case = "bounds";
    match_at(N("a{3}"), 0, N("aaaaa"), 0, 3);
    no_match(N("a{3}"), 0, N("aa"));
    match_at(N("^a{3}$"), 0, N("aaa"), 0, 3);
    match_at(N("a{2,}"), 0, N("baaaa"), 1, 5);
    no_match(N("a{2,}"), 0, N("bab"));
    match_at(N("a{2,4}"), 0, N("aaaaaa"), 0, 4);
    match_at(N("a{0,2}b"), 0, N("aaab"), 1, 4);
    match_at(N("a{0}b"), 0, N("ab"), 1, 2);
    match_at(N("a{2,4}?"), 0, N("aaaaaa"), 0, 2);
    match_at(N("a{2,}?b"), 0, N("aaab"), 0, 4);
    match_at(N("(ab){2}"), 0, N("xababab"), 1, 5);
    match_at(N("(?:ab|cd){2}"), 0, N("xcdab"), 1, 5);
    match_at(N("^\\d{1,3}(?:\\.\\d{1,3}){3}$"), 0, N("192.168.0.11"), 0, 12);

    g_case = "bounds captures";
    if (compile(N("(a){2,3}"), 0)) {
        ok(note_regex_search(&g_re, N("aaa"), -1, 0, &s, &e, g,
                             NOTE_REGEX_GROUPS), "matched aaa");
        ok(s == 0 && e == 3, "whole match 0..3");
        group_is(g, 1, 2, 3);
    }
}

static void test_icase(void)
{
    g_case = "icase";
    match_at(N("hello"), NRE_ICASE, N("say HeLLo"), 4, 9);
    match_at(N("HELLO"), NRE_ICASE, N("say hello"), 4, 9);
    no_match(N("hello"), 0, N("say HeLLo"));
    match_at(N("[a-z]+"), NRE_ICASE, N("12ABC34"), 2, 5);
    match_at(N("[A-Z]+"), NRE_ICASE, N("12abc34"), 2, 5);
    no_match(N("[^a-z]+"), NRE_ICASE, N("abc"));
    match_at(N("(?:cat|DOG)+"), NRE_ICASE, N("CatDog"), 0, 6);
    match_at(N("a{2}"), NRE_ICASE, N("xAa"), 1, 3);
}

static void test_errors(void)
{
    g_case = "malformed";
    bad(N("("));
    bad(N("(a"));
    bad(N(")"));
    bad(N("a)"));
    bad(N("[a"));
    bad(N("[a-"));
    bad(N("[]"));
    bad(N("[^]"));
    bad(N("[z-a]"));
    bad(N("*"));
    bad(N("+a"));
    bad(N("?a"));
    bad(N("a**"));
    bad(N("a*+"));
    bad(N("a{2}{3}"));
    bad(N("a{2,1}"));
    bad(N("a{2,"));
    bad(N("a{2,3"));
    bad(N("a{999}"));
    bad(N("a\\"));
    bad(N("\\q"));
    bad(N("[\\q]"));
    bad(N("[a-\\d]"));
    bad(N("(?i)a"));
    bad(N("(?=a)"));
    bad(N("((((((((((a))))))))))"));       /* more than NOTE_REGEX_GROUPS */
    bad(N("(((((((((((((((((((((((((((((((((((((((((a")); /* program overflow */

    g_case = "errors are reported";
    ok(note_regex_compile(&g_re, N("[a"), 0) == 0, "compile failed");
    ok(*note_regex_error(&g_re) != 0, "error message present");
    ok(note_regex_compile(&g_re, N("ab"), 0) == 1, "next compile succeeds");
    ok(*note_regex_error(&g_re) == 0, "error cleared on success");

    /* A pattern that fails half way leaves branches unpatched behind it, so a
     * failed compile must leave the note_regex inert rather than runnable. */
    g_case = "safe after failure";
    {
        static const nchar *const half[] = {
            N("|]-(*)"), N("a(b"), N("(a|b"), N("a|b|(c"), N("(a)(b)(*"),
            N("a{2,3}(x"), N("[a-z]+("), N("(a|"), 0
        };
        int i, s, e, g[NOTE_REGEX_SLOTS];
        for (i = 0; half[i]; i++) {
            ok(note_regex_compile(&g_re, half[i], 0) == 0, "half-built rejected");
            ok(note_regex_groups(&g_re) == 0, "no groups after failure");
            ok(note_regex_search(&g_re, N("aaa bbb (c)"), -1, 0, &s, &e,
                                 g, NOTE_REGEX_GROUPS) == 0,
               "a failed regex never matches");
        }
    }
}

static void test_pathological(void)
{
    static nchar text[4001];
    clock_t t0, t1;
    long ms;
    int i, s, e, hit;

    g_case = "pathological";
    for (i = 0; i < 4000; i++) text[i] = (nchar)'a';
    text[4000] = 0;

    if (!compile(N("(a+)+b"), 0)) return;

    t0 = clock();
    hit = note_regex_search(&g_re, text, 4000, 0, &s, &e, 0, 0);
    t1 = clock();
    ms = (long)((t1 - t0) * 1000 / CLOCKS_PER_SEC);

    ok(!hit, "(a+)+b does not match 4000 a's");
    ok(ms < 2000, "and returns promptly");
    printf("  note  (a+)+b over 4000 a's took %ld ms\n", ms);

    /* The same shape, but matching. */
    text[3999] = (nchar)'b';
    t0 = clock();
    hit = note_regex_search(&g_re, text, 4000, 0, &s, &e, 0, 0);
    t1 = clock();
    ms = (long)((t1 - t0) * 1000 / CLOCKS_PER_SEC);
    ok(hit && s == 0 && e == 4000, "(a+)+b matches a...ab");
    ok(ms < 2000, "and that returns promptly too");
    printf("  note  the matching case took %ld ms\n", ms);

    /* Nested stars over an empty body must terminate, not spin. */
    g_case = "empty loops";
    match_at(N("(?:a*)*"), 0, N("aaa"), 0, 3);
    match_at(N("(?:a*)*"), 0, N("bbb"), 0, 0);
    match_at(N("(?:a*)*b"), 0, N("aaab"), 0, 4);
    match_at(N("(?:)*"), 0, N("x"), 0, 0);
    match_at(N("(?:a?)*b"), 0, N("ab"), 0, 2);
    match_at(N("(?:a|)*b"), 0, N("aab"), 0, 3);

    /* A capturing group that can match nothing, inside a repetition: a
     * backtracker runs the body one last empty time and reports an empty
     * span, a Pike VM visits each instruction once per position and leaves
     * the group unset.  Pinned here so the difference stays deliberate. */
    g_case = "empty loop capture";
    {
        int g[NOTE_REGEX_SLOTS];
        if (compile(N("([a-c]?)*d"), 0)) {
            ok(note_regex_search(&g_re, N("xd"), -1, 0, &s, &e, g,
                                 NOTE_REGEX_GROUPS), "matched d");
            ok(s == 1 && e == 2, "whole match 1..2");
            group_is(g, 1, -1, -1);
        }
        if (compile(N("([a-c])*d"), 0)) {
            ok(note_regex_search(&g_re, N("abd"), -1, 0, &s, &e, g,
                                 NOTE_REGEX_GROUPS), "matched abd");
            group_is(g, 1, 1, 2);   /* a non-empty body still captures */
        }
    }
}

static void test_realistic(void)
{
    g_case = "realistic";
    match_at(N("[A-Za-z_]\\w*"), 0, N("  42 foo_bar()"), 5, 12);
    match_at(N("0[xX][0-9A-Fa-f]+"), 0, N("v = 0xBEEF;"), 4, 10);
    match_at(N("/\\*.*?\\*/"), NRE_DOTALL, N("a /* x\ny */ b"), 2, 11);
    match_at(N("\\bTODO\\b.*"), 0, N("  /* TODO: fix */"), 5, 17);
    match_at(N("^\\s*#\\s*include"), 0, N("  # include <x>"), 0, 11);
    match_at(N("[\\w.]+@[\\w.]+\\.[a-z]{2,4}"), 0,
             N("write to a.b@c.example.com now"), 9, 26);
}

int main(void)
{
    printf("note_regex tests\n");
    printf("  program %d instructions, %d classes, %d ranges, %d groups\n",
           (int)NOTE_REGEX_PROG, (int)NOTE_REGEX_CLASSES,
           (int)NOTE_REGEX_RANGES, (int)NOTE_REGEX_GROUPS);

    test_literals();
    test_dot();
    test_classes();
    test_anchors();
    test_groups();
    test_branch_heads();
    test_alternation();
    test_quantifiers();
    test_icase();
    test_errors();
    test_pathological();
    test_realistic();

    printf("\n%d checks, %d failed\n", g_run, g_failed);
    if (g_failed) {
        printf("FAILED\n");
        return 1;
    }
    printf("all passed\n");
    return 0;
}
