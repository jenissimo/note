/* test_buffer.c — exercises the gap buffer the DOS and C64 ports edit with.
 *
 * A host program, so the CRT is fair game here; the code under test is not.
 *   cl /nologo /W4 /TC tests\test_buffer.c src\core\note_buffer.c
 *
 * Ends with a benchmark over documents of 100 KB, 1 MB and 10 MB, which needs
 * a couple of seconds and about 20 MB; pass "quick" to skip it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/core/note_buffer.h"

/* note_buffer leans on one helper from note_core.c; linking that in would
 * drag the whole application along, so the test supplies its own. */
int n_len(const nchar *s)
{
    int i = 0;
    if (!s) return 0;
    while (s[i]) i++;
    return i;
}

static int checks, failures;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL  %s\n", what); }
}

static const nchar *W(const char *s)
{
    static nchar buf[8][4096];
    static int slot;
    nchar *out = buf[slot = (slot + 1) % 8];
    int i = 0;
    while (s[i] && i < 4095) { out[i] = (nchar)(unsigned char)s[i]; i++; }
    out[i] = 0;
    return out;
}

/* The buffer's contents as a C string, so a test can just compare. */
static const char *text_of(note_buffer *b)
{
    static char out[4096];
    static nchar tmp[4096];
    int n = note_buffer_copy(b, 0, note_buffer_len(b), tmp, 4096), i;
    for (i = 0; i < n; i++) out[i] = (char)tmp[i];
    out[n] = 0;
    return out;
}

static void expect(note_buffer *b, const char *want, const char *what)
{
    const char *got = text_of(b);
    checks++;
    if (strcmp(got, want) != 0) {
        failures++;
        printf("  FAIL  %s\n        want %-24s got %s\n", what, want, got);
    }
}

/* One buffer, reused, with everything it needs. */
static nchar     g_text[8192];
static note_edit g_undo[128];
static nchar     g_utext[4096];
static int       g_lines[512];

static void reset(note_buffer *b, const char *initial)
{
    note_buffer_init(b, g_text, 8192, g_undo, 128, g_utext, 4096, g_lines, 512);
    if (initial) note_buffer_set(b, W(initial));
}

/* ------------------------------------------------------------------ */

static void test_basics(void)
{
    note_buffer b;

    reset(&b, "hello");
    ok(note_buffer_len(&b) == 5, "length after set");
    ok(note_buffer_at(&b, 0) == (nchar)'h', "first character");
    ok(note_buffer_at(&b, 4) == (nchar)'o', "last character");
    ok(note_buffer_at(&b, 5) == 0, "past the end reads as nothing");
    ok(note_buffer_at(&b, -1) == 0, "before the start reads as nothing");
    expect(&b, "hello", "set then read back");

    reset(&b, 0);
    ok(note_buffer_len(&b) == 0, "a fresh buffer is empty");
    note_buffer_insert(&b, W("abc"), -1);
    expect(&b, "abc", "insert into an empty buffer");
}

static void test_insert_delete(void)
{
    note_buffer b;

    reset(&b, "hello world");

    note_buffer_caret_set(&b, 5, 0);
    note_buffer_insert(&b, W(","), -1);
    expect(&b, "hello, world", "insert in the middle");
    ok(b.caret == 6, "the caret follows the insertion");

    note_buffer_caret_set(&b, 0, 0);
    note_buffer_insert(&b, W(">> "), -1);
    expect(&b, ">> hello, world", "insert at the start");

    note_buffer_caret_set(&b, note_buffer_len(&b), 0);
    note_buffer_insert(&b, W("!"), -1);
    expect(&b, ">> hello, world!", "insert at the end");

    /* Forward delete, backward delete, and deleting a selection. */
    note_buffer_caret_set(&b, 0, 0);
    note_buffer_delete(&b, 3);
    expect(&b, "hello, world!", "forward delete");

    note_buffer_caret_set(&b, note_buffer_len(&b), 0);
    note_buffer_delete(&b, -1);
    expect(&b, "hello, world", "backward delete");

    note_buffer_caret_set(&b, 5, 0);
    note_buffer_caret_set(&b, 7, 1);
    ok(note_buffer_has_sel(&b), "a selection exists");
    note_buffer_delete(&b, 0);
    expect(&b, "helloworld", "deleting a selection");
    ok(!note_buffer_has_sel(&b), "the selection is gone afterwards");

    /* Typing over a selection replaces it. */
    note_buffer_caret_set(&b, 0, 0);
    note_buffer_caret_set(&b, 5, 1);
    note_buffer_insert(&b, W("HELLO"), -1);
    expect(&b, "HELLOworld", "typing replaces the selection");

    /* Deleting past either end must clamp, not corrupt. */
    note_buffer_caret_set(&b, 0, 0);
    note_buffer_delete(&b, -5);
    expect(&b, "HELLOworld", "backward delete at the start does nothing");
    note_buffer_caret_set(&b, note_buffer_len(&b), 0);
    note_buffer_delete(&b, 5);
    expect(&b, "HELLOworld", "forward delete at the end does nothing");
}

static void test_undo(void)
{
    note_buffer b;
    int i;

    reset(&b, "base");

    note_buffer_caret_set(&b, 4, 0);
    note_buffer_insert(&b, W("-one"), -1);
    expect(&b, "base-one", "an edit to undo");

    ok(note_buffer_undo(&b), "undo reports it did something");
    expect(&b, "base", "undo removes the insertion");
    ok(note_buffer_redo(&b), "redo reports it did something");
    expect(&b, "base-one", "redo puts it back");

    /* A deletion comes back with its text. */
    note_buffer_caret_set(&b, 0, 0);
    note_buffer_caret_set(&b, 4, 1);
    note_buffer_delete(&b, 0);
    expect(&b, "-one", "delete a selection");
    note_buffer_undo(&b);
    expect(&b, "base-one", "undo restores deleted text");

    /* A run of typing undoes as one step, because each character is in the
     * same group until the caret moves. */
    reset(&b, "");
    for (i = 0; i < 5; i++) note_buffer_insert(&b, W("x"), 1);
    expect(&b, "xxxxx", "five characters typed");
    note_buffer_undo(&b);
    expect(&b, "", "a typing run undoes in one step");

    /* Moving the caret ends the run, so the two halves undo separately. */
    reset(&b, "");
    note_buffer_insert(&b, W("aa"), -1);
    note_buffer_caret_set(&b, note_buffer_len(&b), 0);
    note_buffer_insert(&b, W("bb"), -1);
    note_buffer_undo(&b);
    expect(&b, "aa", "moving the caret splits the undo run");

    /* Undo with nothing to undo is not an error. */
    reset(&b, "x");
    ok(!note_buffer_undo(&b), "undo on a fresh buffer does nothing");
    ok(!note_buffer_redo(&b), "redo with no undo does nothing");

    /* A new edit discards the redo branch. */
    reset(&b, "");
    note_buffer_insert(&b, W("a"), -1);
    note_buffer_undo(&b);
    note_buffer_insert(&b, W("b"), -1);
    ok(!note_buffer_redo(&b), "editing after undo drops the redo branch");
    expect(&b, "b", "and leaves the new edit in place");
}

static void test_lines(void)
{
    note_buffer b;

    reset(&b, "one\ntwo\nthree");
    ok(note_buffer_lines(&b) == 3, "three lines");
    ok(note_buffer_line_start(&b, 0) == 0, "first line starts at 0");
    ok(note_buffer_line_start(&b, 1) == 4, "second line start");
    ok(note_buffer_line_at(&b, 5) == 1, "offset maps to its line");
    ok(note_buffer_line_len(&b, 0) == 3, "line length excludes the break");
    ok(note_buffer_line_len(&b, 2) == 5, "last line length");

    /* CRLF is one break, not two. */
    reset(&b, "a\r\nb\r\nc");
    ok(note_buffer_lines(&b) == 3, "CRLF counts as one break");
    ok(note_buffer_line_len(&b, 0) == 1, "CRLF is not part of the line");

    /* A lone CR too, which is what some of these machines produce. */
    reset(&b, "a\rb\rc");
    ok(note_buffer_lines(&b) == 3, "a lone CR is a break");

    /* The index must survive editing. */
    reset(&b, "one\ntwo");
    note_buffer_caret_set(&b, 3, 0);
    note_buffer_insert(&b, W("\nmid"), -1);
    ok(note_buffer_lines(&b) == 3, "the index follows an edit");
    expect(&b, "one\nmid\ntwo", "and the text is right");

    reset(&b, "");
    ok(note_buffer_lines(&b) == 1, "an empty buffer is one line");
}

static void test_find(void)
{
    note_buffer b;

    reset(&b, "the quick brown fox jumps over the lazy dog");

    ok(note_buffer_find(&b, W("quick"), 0, FIND_DOWN) == 4, "find forwards");
    ok(note_buffer_find(&b, W("the"), 1, FIND_DOWN) == 31, "find skips before the start");
    ok(note_buffer_find(&b, W("nothere"), 0, FIND_DOWN) == -1, "absent text is not found");

    /* Wrapping, in both directions. */
    ok(note_buffer_find(&b, W("the"), 35, FIND_DOWN) == 0, "forward search wraps");
    ok(note_buffer_find(&b, W("dog"), 5, 0) == 40, "backward search wraps");

    ok(note_buffer_find(&b, W("QUICK"), 0, FIND_DOWN) == 4, "case-insensitive by default");
    ok(note_buffer_find(&b, W("QUICK"), 0, FIND_DOWN | FIND_MATCHCASE) == -1,
       "case-sensitive when asked");

    reset(&b, "cat concatenate cat");
    ok(note_buffer_find(&b, W("cat"), 0, FIND_DOWN | FIND_WHOLEWORD) == 0,
       "whole word finds the standalone one");
    ok(note_buffer_find(&b, W("cat"), 1, FIND_DOWN | FIND_WHOLEWORD) == 16,
       "whole word skips the one inside a word");
}

/* The gap has to end up in the right place however the caret wanders. */
static void test_gap_movement(void)
{
    note_buffer b;
    int i;

    reset(&b, "0123456789");
    for (i = 0; i <= 10; i++) {
        note_buffer_caret_set(&b, i, 0);
        note_buffer_insert(&b, W("."), 1);
        note_buffer_delete(&b, -1);
    }
    expect(&b, "0123456789", "inserting and removing at every position");

    /* Fill the buffer to its capacity and check the refusal is clean. */
    reset(&b, 0);
    for (i = 0; i < 8192; i++) {
        if (!note_buffer_insert(&b, W("x"), 1)) break;
        note_buffer_caret_set(&b, note_buffer_len(&b), 0);
    }
    ok(note_buffer_len(&b) == 8192, "the buffer fills to capacity");
    ok(!note_buffer_insert(&b, W("y"), 1), "and then refuses politely");
    ok(note_buffer_len(&b) == 8192, "without corrupting what is there");
}

/* ------------------------------------------------------------------ *
 * The index, against a full rescan
 *
 * The index is now maintained edit by edit rather than rebuilt, and the way
 * that goes wrong is silently: a line number that is off by one and stays off
 * for the rest of the session.  Nothing short of comparing every line start
 * against a scan of the whole document catches that, so that is what this
 * does, after every single edit of a long random run.
 * ------------------------------------------------------------------ */

#define FUZZ_TEXT 4096

/* The same rule the buffer uses, written out the obvious way: a line starts
 * at 0 and after every break, and CRLF is one break. */
static int ref_starts(const char *s, int *starts, int cap)
{
    int i = 0, n = 0;

    starts[n++] = 0;
    while (s[i]) {
        if (s[i] == '\n' || (s[i] == '\r' && s[i + 1] != '\n')) {
            if (n < cap) starts[n] = i + 1;
            n++;
        }
        i++;
    }
    return n;
}

static char g_fuzz_seen[FUZZ_TEXT + 1];
static int  g_fuzz_ref[FUZZ_TEXT + 2];

/* Reports at most one failure per call: a drifted index fails every line, and
 * the first one is the only informative one. */
static int index_matches(note_buffer *b, const char *what)
{
    static nchar tmp[FUZZ_TEXT + 1];
    int n = note_buffer_copy(b, 0, note_buffer_len(b), tmp, FUZZ_TEXT + 1);
    int want, got, i, line;

    for (i = 0; i < n; i++) g_fuzz_seen[i] = (char)tmp[i];
    g_fuzz_seen[n] = 0;

    want = ref_starts(g_fuzz_seen, g_fuzz_ref, FUZZ_TEXT + 2);
    got  = note_buffer_lines(b);
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL  %s: %d lines, want %d\n", what, got, want);
        return 0;
    }

    for (line = 0; line < want; line++) {
        int start = note_buffer_line_start(b, line);
        int len   = note_buffer_line_len(b, line);
        int end   = (line + 1 < want) ? g_fuzz_ref[line + 1] : n;
        int wlen  = end - g_fuzz_ref[line];

        while (wlen > 0 && (g_fuzz_seen[g_fuzz_ref[line] + wlen - 1] == '\r' ||
                            g_fuzz_seen[g_fuzz_ref[line] + wlen - 1] == '\n'))
            wlen--;

        checks++;
        if (start != g_fuzz_ref[line] || len != wlen) {
            failures++;
            printf("  FAIL  %s: line %d starts at %d len %d,"
                   " want %d len %d\n",
                   what, line, start, len, g_fuzz_ref[line], wlen);
            return 0;
        }
        /* And the reverse direction, which uses the cursor differently. */
        checks++;
        if (note_buffer_line_at(b, g_fuzz_ref[line]) != line ||
            note_buffer_line_at(b, g_fuzz_ref[line] + wlen / 2) != line) {
            failures++;
            printf("  FAIL  %s: offset %d maps to line %d, want %d\n",
                   what, g_fuzz_ref[line],
                   note_buffer_line_at(b, g_fuzz_ref[line]), line);
            return 0;
        }
    }

    /* Backwards, where the cached line is behind the question and the search
     * has to fall back to a checkpoint. */
    for (line = want - 1; line >= 0; line--) {
        checks++;
        if (note_buffer_line_start(b, line) != g_fuzz_ref[line]) {
            failures++;
            printf("  FAIL  %s: line %d walked backwards starts at %d,"
                   " want %d\n", what, line,
                   note_buffer_line_start(b, line), g_fuzz_ref[line]);
            return 0;
        }
    }
    return 1;
}

/* Deterministic, so a failure can be run again. */
static unsigned long g_rng = 1u;

static int rnd(int n)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return (int)((g_rng >> 16) % (unsigned long)n);
}

static void test_index_fuzz(void)
{
    /* Everything a random edit can be made of, weighted by nothing in
     * particular except that breaks have to be common enough to collide. */
    static const char *bits[] = {
        "a", "xy", " ", "word ", "\n", "\r\n", "\r", "\n\n", "a\nb",
        "\r\r", "x\r", "\n\r", "long enough to matter", "\r\na"
    };
    static nchar     text[FUZZ_TEXT];
    static note_edit undo[64];
    static nchar     utext[1024];
    /* Sized for the largest capacity any pass below declares.  The passes
     * that declare less than this are the point of the test: an index told it
     * has twelve ints has to coarsen itself rather than truncate, and telling
     * it twelve while handing it more would test nothing. */
    static int       lines[512];

    note_buffer b;
    int step, pass;

    for (pass = 0; pass < 3; pass++) {
        g_rng = 1u + (unsigned long)pass;
        note_buffer_init(&b, text, FUZZ_TEXT, undo, 64, utext, 1024,
                         pass == 2 ? (int *)0 : lines,
                         pass == 2 ? 0 : (pass == 0 ? 12 : 512));
        note_buffer_set(&b, W("one\ntwo\r\nthree\rfour"));
        if (!index_matches(&b, "fuzz start")) return;

        for (step = 0; step < 1200; step++) {
            int len = note_buffer_len(&b);
            int at  = rnd(len + 1);
            int what = rnd(10);

            note_buffer_caret_set(&b, at, 0);
            if (what < 5) {
                const char *s = bits[rnd((int)(sizeof(bits) / sizeof(bits[0])))];
                if (len + (int)strlen(s) < FUZZ_TEXT)
                    note_buffer_insert(&b, W(s), -1);
            } else if (what < 8) {
                note_buffer_delete(&b, rnd(2) ? rnd(4) + 1 : -(rnd(4) + 1));
            } else if (what == 8) {
                note_buffer_caret_set(&b, rnd(len + 1), 1);
                note_buffer_delete(&b, 0);
            } else {
                if (rnd(2)) note_buffer_undo(&b); else note_buffer_redo(&b);
            }

            if (!index_matches(&b, "fuzz")) {
                printf("        pass %d step %d\n", pass, step);
                return;
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 * What an edit costs as the document grows
 * ------------------------------------------------------------------ */

static double us_per(clock_t t0, clock_t t1, long n)
{
    return ((double)(t1 - t0) * 1000000.0 / (double)CLOCKS_PER_SEC) / (double)n;
}

/* Fills `text` with `want` characters of forty-column prose and hands it to
 * the buffer.  note_buffer_set copies text[i] to buf[i], so the source may be
 * the buffer's own storage -- which is how the DOS port loads a file too. */
static void fill(note_buffer *b, nchar *text, int cap, int want)
{
    static const char row[] = "the quick brown fox jumps over a dog\n";
    int i = 0, k = 0;

    while (i < want) { text[i++] = (nchar)(unsigned char)row[k]; k = row[k+1] ? k+1 : 0; }
    text[i] = 0;
    note_buffer_set(b, text);
    (void)cap;
}

static void bench_size(int bytes, const char *label)
{
    int cap = bytes + 300000;   /* room for everything the run inserts */
    nchar     *text  = (nchar *)malloc((size_t)cap * sizeof(nchar));
    int       *lines = (int *)malloc(8192 * sizeof(int));
    note_buffer b;
    clock_t t0, t1;
    long i, n;
    int mid;

    if (!text || !lines) { printf("  %-8s (out of memory, skipped)\n", label);
                           free(text); free(lines); return; }

    note_buffer_init(&b, text, cap, (note_edit *)0, 0, (nchar *)0, 0, lines, 8192);
    fill(&b, text, cap, bytes);

    t0 = clock();
    (void)note_buffer_lines(&b);
    t1 = clock();
    printf("  %-8s %8d lines  load+index %7.1f ms",
           label, note_buffer_lines(&b), (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC);

    /* Typing: a character at a time at a caret that stays put, which is the
     * keystroke the whole design is about. */
    mid = note_buffer_len(&b) / 2;
    note_buffer_caret_set(&b, mid, 0);
    n = 100000;
    t0 = clock();
    /* A refused insert is free, and a benchmark that quietly measures
     * refusals is worse than no benchmark, so stop at the first one. */
    for (i = 0; i < n; i++)
        if (!note_buffer_insert(&b, W("x"), 1)) { n = i; break; }
    t1 = clock();
    printf("  type %6.3f us", us_per(t0, t1, n));

    /* Pressing Enter, which changes the line count and so has to touch the
     * index rather than skip it. */
    n = 50000;
    t0 = clock();
    for (i = 0; i < n; i++)
        if (!note_buffer_insert(&b, W("\n"), 1)) { n = i; break; }
    t1 = clock();
    printf("  enter %6.3f us", us_per(t0, t1, n));

    /* One screenful drawn from a line nowhere near the last one, which is the
     * binary search plus the scan the sparse index leaves to do. */
    n = 5000;
    t0 = clock();
    for (i = 0; i < n; i++) {
        int line = (int)(((unsigned long)i * 7919u) % (unsigned long)(note_buffer_lines(&b) - 25));
        int row;
        (void)note_buffer_line_at(&b, note_buffer_line_start(&b, line));
        for (row = 0; row < 25; row++) {
            (void)note_buffer_line_start(&b, line + row);
            (void)note_buffer_line_len(&b, line + row);
        }
    }
    t1 = clock();
    printf("  redraw %7.3f us\n", us_per(t0, t1, n));

    free(text);
    free(lines);
}

static void bench(void)
{
    printf("\nper-edit cost against document size"
           " (one screenful is 25 lines)\n");
    bench_size(100 * 1024,        "100 KB");
    bench_size(1024 * 1024,       "1 MB");
    bench_size(10 * 1024 * 1024,  "10 MB");
}

int main(int argc, char **argv)
{
    int quick = argc > 1 && strcmp(argv[1], "quick") == 0;

    printf("note_buffer tests\n");

    test_basics();
    test_insert_delete();
    test_undo();
    test_lines();
    test_find();
    test_gap_movement();
    test_index_fuzz();

    printf("\n%d checks, %d failed\n", checks, failures);
    if (failures) { printf("FAILED\n"); return 1; }
    printf("all passed\n");

    if (!quick) bench();
    return 0;
}
