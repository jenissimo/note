/* test_syntax.c — exercises the language registry and the lexer.
 *
 * A host program, so the CRT is fair game here; the code under test is not.
 * Build:
 *   cl /nologo /W4 /TC tests\test_syntax.c src\core\note_syntax.c ^
 *      src\core\note_conf.c src\core\note_regex.c src\core\note_pack.c
 *
 * Not note_core.c: it defines n_len and friends that this file supplies for
 * itself, and drags in the theme registry, which the lexer has no part in.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/core/note_syntax.h"
#include "../src/core/note_conf.h"
#include "../src/core/note_regex.h"

/* note_syntax.c and note_conf.c lean on three helpers that live in
 * note_core.c, and linking that in would drag the whole application with it.
 * They are a few lines each, so the test supplies its own. */
int n_len(const nchar *s)
{
    int i = 0;
    if (!s) return 0;
    while (s[i]) i++;
    return i;
}

int n_eq(const nchar *a, const nchar *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

const nchar *note_basename(const nchar *path)
{
    const nchar *p = path, *last = path;
    for (; *p; p++)
        if (*p == (nchar)'\\' || *p == (nchar)'/') last = p + 1;
    return last;
}

static int checks, failures;
static note_arena arena;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

/* Widen an ASCII literal so tests can be written in plain C strings. */
static const nchar *W(const char *s)
{
    static nchar buf[8][8192];
    static int   slot;
    nchar *out = buf[slot = (slot + 1) % 8];
    int i = 0;
    while (s[i] && i < 8191) { out[i] = (nchar)(unsigned char)s[i]; i++; }
    out[i] = 0;
    return out;
}

/* The kind covering `at`, or TOK_TEXT when no span does. */
static int kind_at(const note_span *sp, int n, int at)
{
    int i;
    for (i = 0; i < n; i++)
        if (at >= sp[i].start && at < sp[i].start + sp[i].len)
            return sp[i].kind;
    return TOK_TEXT;
}

static void expect_kind(const note_span *sp, int n, const nchar *text,
                        const char *needle, int kind, const char *what)
{
    int i, j, at = -1, nl = (int)strlen(needle);

    for (i = 0; text[i]; i++) {
        for (j = 0; j < nl && text[i + j] == (nchar)(unsigned char)needle[j]; j++) {}
        if (j == nl) { at = i; break; }
    }
    if (at < 0) { checks++; failures++; printf("  FAIL  %s (not in text)\n", what); return; }

    ok(kind_at(sp, n, at) == kind, what);
}

/* ------------------------------------------------------------------ */

static void test_registry(void)
{
    int c, unknown;

    /* The registry starts empty on purpose: languages used to be compiled in
     * as well as shipped in the pack, which meant maintaining the same
     * definitions twice.  A fresh registry now holds nothing but LANG_NONE,
     * and everything it knows arrives through note_syntax_add. */
    note_syntax_init(&arena);
    ok(note_lang_count() == 1, "a fresh registry holds only LANG_NONE");
    ok(note_lang_from_path(W("C:\\src\\main.c")) == LANG_NONE,
       "and therefore recognises nothing");

    note_syntax_add(&arena,
        W("name = C\n"
          "extensions = c h\n"
          "keywords = if else return\n"));

    c = note_lang_from_path(W("C:\\src\\main.c"));
    ok(c != LANG_NONE, "a .c file finds a language once one is added");
    ok(note_lang_get(c)->keywords != 0, "that language has keywords");
    ok(note_lang_from_path(W("header.h")) == c, "a second extension maps to it");

    unknown = note_lang_from_path(W("notes.zzz"));
    ok(unknown == LANG_NONE, "an unknown extension finds nothing");
    ok(note_lang_from_path(W("plain")) == LANG_NONE, "no extension finds nothing");

    ok(note_lang_count() == 2, "the registry holds what was added");
}

static void test_added_language(void)
{
    int lang;

    note_syntax_init(&arena);
    lang = note_syntax_add(&arena,
        W("name = Toy\n"
          "extensions = toy\n"
          "line_comment = //\n"
          "block_comment = /* */\n"
          "quotes = \"\n"
          "keywords = if else while\n"
          "types = int void\n"));

    ok(lang > 0, "a definition is accepted");
    ok(note_lang_from_path(W("a.toy")) == lang, "and claims its extension");
    ok(n_eq(note_lang_get(lang)->name, W("Toy")), "and keeps its name");

    /* Same name again: replaced, not duplicated. */
    ok(note_syntax_add(&arena, W("name = Toy\nextensions = toy2\n")) == lang,
       "a definition with an existing name replaces it");

    ok(note_syntax_add(&arena, W("extensions = nope\n")) < 0,
       "a definition without a name is rejected");
}

static void test_scanner(void)
{
    note_span sp[256];
    const nchar *text;
    int lang, n;

    note_syntax_init(&arena);
    lang = note_syntax_add(&arena,
        W("name = Toy\nextensions = toy\n"
          "line_comment = //\nblock_comment = /* */\nquotes = \"'\n"
          "keywords = if else return\ntypes = int\n"));

    text = W("int x = 42; // tail\nif (x) return \"hi\";\n");
    n = note_tokenize(lang, text, n_len(text), 0, sp, 256);

    expect_kind(sp, n, text, "int",     TOK_TYPE,    "a type is a type");
    expect_kind(sp, n, text, "42",      TOK_NUMBER,  "a number is a number");
    expect_kind(sp, n, text, "// tail", TOK_COMMENT, "a line comment is a comment");
    expect_kind(sp, n, text, "if",      TOK_KEYWORD, "a keyword is a keyword");
    expect_kind(sp, n, text, "\"hi\"",  TOK_STRING,  "a string is a string");

    /* A line comment must stop at the break, whichever break it is. */
    text = W("// one\rint a;\r");
    n = note_tokenize(lang, text, n_len(text), 0, sp, 256);
    expect_kind(sp, n, text, "int", TOK_TYPE,
                "a comment ends at a lone CR, not at end of buffer");

    text = W("// one\r\nint a;\r\n");
    n = note_tokenize(lang, text, n_len(text), 0, sp, 256);
    expect_kind(sp, n, text, "int", TOK_TYPE, "and at a CRLF");

    /* An unterminated quote must not colour the rest of the file. */
    text = W("char c = 'x\rint after;\r");
    n = note_tokenize(lang, text, n_len(text), 0, sp, 256);
    expect_kind(sp, n, text, "int", TOK_TYPE,
                "an unclosed quote stops at the line break");

    /* base is added to every span, so a backend can pass a slice. */
    text = W("int x;");
    n = note_tokenize(lang, text, n_len(text), 1000, sp, 256);
    ok(n > 0 && sp[0].start >= 1000, "base offsets the spans");
}

static void test_rules(void)
{
    note_span sp[256];
    const nchar *text;
    int lang, n;

    note_syntax_init(&arena);
    lang = note_syntax_add(&arena,
        W("name = Ruled\nextensions = rl\n"
          "line_comment = //\nquotes = \"\n"
          "keywords = if return\n"
          "rule = operator [-+*/%=<>!]\n"
          "rule = keyword \\b[A-Z_][0-9A-Z_]+\\b\n"
          "rule = number \\b0[Xx][0-9A-Fa-f]+\\b\n"));

    ok(note_lang_get(lang)->nrules == 3, "three rules are parsed");

    {   /* Say which expressions the engine turned down, and why. */
        const note_lang *L = note_lang_get(lang);
        note_regex re;
        int r, j;
        for (r = 0; r < (int)L->nrules; r++) {
            if (note_regex_compile(&re, L->rules[r].pattern, 0)) continue;
            printf("  rule %d rejected: ", r);
            for (j = 0; L->rules[r].pattern[j]; j++)
                putchar((char)L->rules[r].pattern[j]);
            printf("  --  ");
            for (j = 0; note_regex_error(&re)[j]; j++)
                putchar((char)note_regex_error(&re)[j]);
            putchar('\n');
        }
    }

    text = W("a += MAX_LEN + 0xFF;");
    n = note_tokenize(lang, text, n_len(text), 0, sp, 256);

    expect_kind(sp, n, text, "+=",      TOK_OPERATOR, "an operator rule fires");
    expect_kind(sp, n, text, "MAX_LEN", TOK_KEYWORD,  "a capitalised identifier rule fires");
    expect_kind(sp, n, text, "0xFF",    TOK_NUMBER,   "a hex literal rule fires");

    /* Comments and strings are consumed whole, so no rule can reach inside. */
    text = W("// a += MAX_LEN\r\"b -= OTHER\"\r");
    n = note_tokenize(lang, text, n_len(text), 0, sp, 256);
    expect_kind(sp, n, text, "+=",    TOK_COMMENT, "rules do not fire inside a comment");
    expect_kind(sp, n, text, "OTHER", TOK_STRING,  "rules do not fire inside a string");

    /* A rule that cannot compile is dropped, and the language still works. */
    note_syntax_init(&arena);
    lang = note_syntax_add(&arena,
        W("name = Bad\nextensions = bad\nkeywords = if\n"
          "rule = operator [unclosed\n"
          "rule = operator \\+\n"));
    text = W("if a + b");
    n = note_tokenize(lang, text, n_len(text), 0, sp, 256);
    expect_kind(sp, n, text, "if", TOK_KEYWORD, "a broken rule does not break the language");
    expect_kind(sp, n, text, "+",  TOK_OPERATOR, "and the rules after it still work");
}

/* The safe restart point is what keeps scrolling a large file from re-lexing
 * everything above the caret, so it has to be both cheap and right. */
static void test_safe_start(void)
{
    const nchar *text;
    int lang, at, s;
    note_span sp[256], sp2[256];
    int n1, n2, i;

    note_syntax_init(&arena);
    lang = note_syntax_add(&arena,
        W("name = Toy\nextensions = toy\n"
          "line_comment = //\nblock_comment = /* */\nquotes = \"\n"
          "keywords = if else\ntypes = int\n"));

    text = W("int a;\rint b;\rint c;\r");
    ok(note_syntax_safe_start(lang, text, n_len(text), 0, 4096) == 0,
       "the start of the document is its own safe point");

    at = 14;                                   /* start of the third line */
    s = note_syntax_safe_start(lang, text, n_len(text), at, 4096);
    ok(s <= at, "the safe point never runs past the target");
    ok(s == 14 || s == 0, "outside any comment it is a line boundary");

    /* Inside an open block comment the scan must go back to the opener. */
    text = W("int a;\r/* still\ropen\rhere\r");
    s = note_syntax_safe_start(lang, text, n_len(text), 18, 4096);
    ok(s == 7, "inside a block comment it starts at the opener");

    /* After the comment closes it must not: that would re-lex needlessly. */
    text = W("/* a */\rint b;\rint c;\r");
    s = note_syntax_safe_start(lang, text, n_len(text), 16, 4096);
    ok(s >= 8, "after a block comment closes it starts below it");

    /* A window of zero still has to produce something usable. */
    text = W("int a;\rint b;\r");
    s = note_syntax_safe_start(lang, text, n_len(text), 8, 0);
    ok(s >= 0 && s <= 8, "a zero window still lands in range");

    /* The point of the whole thing: starting there gives the same colours in
     * the region of interest as starting from the top. */
    text = W("/* head */\rint a;\r// note\rint b;\rint c;\r\"str\"\rint d;\r");
    at = 30;
    s = note_syntax_safe_start(lang, text, n_len(text), at, 4096);
    n1 = note_tokenize(lang, text, n_len(text), 0, sp, 256);
    n2 = note_tokenize(lang, text + s, n_len(text) - s, s, sp2, 256);

    {   /* Every span at or after `at` must match one from the full scan. */
        int mismatched = 0, checked = 0;
        for (i = 0; i < n2; i++) {
            int j, found = 0;
            if (sp2[i].start < at) continue;
            checked++;
            for (j = 0; j < n1; j++)
                if (sp[j].start == sp2[i].start && sp[j].len == sp2[i].len &&
                    sp[j].kind == sp2[i].kind) { found = 1; break; }
            if (!found) mismatched++;
        }
        ok(checked > 0, "the partial scan produced spans to compare");
        ok(mismatched == 0, "a partial scan colours the region as a full one does");
    }
}

/* --------------------------------------------------------------------------
 * Incremental lexing.
 *
 * The property under test is equivalence: whatever a caller does with
 * checkpoints, the colours it ends up with must be the ones a single scan of
 * the whole document from the top would have produced.  Anything less and the
 * screen is quietly wrong somewhere below a block comment.
 * -------------------------------------------------------------------------- */

/* One block of a document that has every construct the state machine has to
 * carry across a line: a block comment, a string that looks like a comment, an
 * escaped quote, a string continued by a backslash, a comment holding a quote,
 * and a raw string that spans lines on its own. */
static const char kBlock[] =
    "/* header comment\n"
    " * spanning lines\n"
    " */\n"
    "int a = 1;\n"
    "char *s = \"a string with /* not a comment */ inside\";\n"
    "char q = '\\'';\n"
    "const char *cont = \"continued \\\n"
    "onto the next line\";\n"
    "// line comment with \" quote and /* opener\n"
    "/* another\n"
    "block\n"
    "comment */ int b;\n"
    "`raw\n"
    "backtick\n"
    "string`\n"
    "int c = 0x2A;\n";

static int toy_lang(void)
{
    return note_syntax_add(&arena,
        W("name = Toy\nextensions = toy\n"
          "line_comment = //\nblock_comment = /* */\nquotes = \"'`\n"
          "keywords = if else return const\ntypes = int char\n"
          "rule = operator [-+*=<>]\n"
          "rule = number \\b0[Xx][0-9A-Fa-f]+\\b\n"));
}

/* The document, its line index, and room to lex it whole. */
static nchar  g_text[600000];
static int    g_len;
static int    g_ls[40000];
static int    g_nlines;
static note_span g_full[200000];
static int    g_nfull;

static void doc_build(int blocks)
{
    int i, j;
    g_len = 0;
    for (i = 0; i < blocks; i++)
        for (j = 0; kBlock[j]; j++)
            if (g_len < (int)(sizeof(g_text) / sizeof(g_text[0])) - 1)
                g_text[g_len++] = (nchar)(unsigned char)kBlock[j];
    g_text[g_len] = 0;
}

static void doc_index(void)
{
    int i;
    g_nlines = 0;
    g_ls[g_nlines++] = 0;
    for (i = 0; i < g_len; i++)
        if (g_text[i] == (nchar)'\n' && g_nlines < 40000)
            g_ls[g_nlines++] = i + 1;
}

static int line_end(int line)
{
    return line + 1 < g_nlines ? g_ls[line + 1] : g_len;
}

/* --------------------------------------------------------------------------
 * The call sequence a backend is meant to use.
 *
 * Everything a redraw does is here: ask the table where it may start, lex
 * forward to the top of the view if that is not the top of the view already,
 * then lex the view itself and hand the state after it back to the table.
 * The first step is a division, the second is bounded by K lines, and the
 * third is the screen -- none of the three is proportional to the document.
 * -------------------------------------------------------------------------- */
static int screen_lex(int lang, note_syn_ckpts *t, int top, int rows,
                      note_span *out, int max)
{
    note_syn_state st, after;
    int from, start, end, n;

    from = note_syn_ckpts_state(t, top, &st);

    if (from < top)
        st = note_syn_ckpts_scan(t, lang, g_text + g_ls[from],
                                 g_ls[top] - g_ls[from], from, st);

    start = g_ls[top];
    end   = (top + rows < g_nlines) ? g_ls[top + rows] : g_len;

    n = note_tokenize_from(lang, g_text + start, end - start, start,
                           out, max, st, &after);
    note_syn_ckpts_record(t, top + rows, after);
    return n;
}

/* Compares the colour of every character of [from,to) against the full scan.
 * Per character rather than per span, because the first span of a partial
 * scan is legitimately the tail of one the full scan started higher up. */
static int same_colours(const note_span *sp, int n, int from, int to)
{
    int at;
    for (at = from; at < to; at++)
        if (kind_at(sp, n, at) != kind_at(g_full, g_nfull, at)) return at;
    return -1;
}

static void test_incremental(void)
{
    static note_syn_state slots[128];
    static note_span sp[8192];
    note_syn_ckpts t;
    int lang, top, n, bad, worst = -1;
    const int ROWS = 25;

    note_syntax_init(&arena);
    lang = toy_lang();

    doc_build(120);
    doc_index();
    g_nfull = note_tokenize(lang, g_text, g_len, 0, g_full, 200000);
    ok(g_nfull > 0 && g_nfull < 200000, "the whole document lexes in one go");

    /* A hundred and twenty-eight slots for nineteen hundred lines is the C64's
     * ratio: K comes out at 16, not 1. */
    note_syn_ckpts_init(&t, slots, 128);
    note_syn_ckpts_fit(&t, g_nlines);
    ok(t.every > 1, "a document larger than the table gets coarser checkpoints");
    ok(g_nlines / t.every < 128, "and then fits it");

    /* Scrolling down a line at a time, which is how the table fills. */
    bad = 0;
    for (top = 0; top + ROWS < g_nlines; top++) {
        n = screen_lex(lang, &t, top, ROWS, sp, 8192);
        if (same_colours(sp, n, g_ls[top], line_end(top + ROWS - 1)) >= 0) {
            if (worst < 0) worst = top;
            bad++;
        }
    }
    ok(bad == 0, "scrolling forward colours every screen as the full scan does");
    if (bad) printf("    first wrong screen at line %d\n", worst);

    /* Jumping about, with the table already warm.  A jump backwards is the
     * case a window-based scan gets wrong: nothing above the view was
     * re-examined, so the state has to have been remembered. */
    bad = 0;
    for (top = g_nlines - ROWS - 1; top > 0; top -= 37) {
        n = screen_lex(lang, &t, top, ROWS, sp, 8192);
        if (same_colours(sp, n, g_ls[top], line_end(top + ROWS - 1)) >= 0) bad++;
    }
    ok(bad == 0, "jumping backwards colours every screen as the full scan does");

    /* A cold table jumping straight to the end: the checkpoint scan has to
     * produce the same state the scroll did. */
    {
        static note_syn_state cold_slots[128];
        note_syn_ckpts c;
        note_syn_state a, b;
        int la, lb;
        note_syn_ckpts_init(&c, cold_slots, 128);
        note_syn_ckpts_fit(&c, g_nlines);
        top = g_nlines - ROWS - 1;
        n = screen_lex(lang, &c, top, ROWS, sp, 8192);
        ok(same_colours(sp, n, g_ls[top], line_end(top + ROWS - 1)) < 0,
           "a cold table reaches the same colours as a warm one");
        la = note_syn_ckpts_state(&c, top, &a);
        lb = note_syn_ckpts_state(&t, top, &b);
        ok(la == lb && a == b, "and arrives at the same checkpoint state");
    }

    /* Edits.  Only what follows the edited line may be forgotten, and lexing
     * after that has to agree with a fresh full scan of the new text. */
    {
        int at[5], k;
        at[0] = 5; at[1] = 300; at[2] = g_nlines / 2; at[3] = g_nlines - 200;
        at[4] = g_nlines - ROWS - 2;

        for (k = 0; k < 5; k++) {
            int edit = at[k], pos;
            if (edit < 1 || edit + ROWS >= g_nlines) continue;

            /* Open a block comment on the edited line by overwriting the two
             * characters after its start.  Every line of kBlock is at least
             * that long, and it changes the state of everything below. */
            pos = g_ls[edit];
            g_text[pos] = (nchar)'/';
            g_text[pos + 1] = (nchar)'*';

            g_nfull = note_tokenize(lang, g_text, g_len, 0, g_full, 200000);
            note_syn_ckpts_dirty(&t, edit);

            n = screen_lex(lang, &t, edit, ROWS, sp, 8192);
            ok(same_colours(sp, n, g_ls[edit], line_end(edit + ROWS - 1)) < 0,
               "after an edit the edited screen matches a full scan");

            top = edit + ROWS + 3;
            if (top + ROWS < g_nlines) {
                n = screen_lex(lang, &t, top, ROWS, sp, 8192);
                ok(same_colours(sp, n, g_ls[top], line_end(top + ROWS - 1)) < 0,
                   "and so does a screen below it");
            }
        }
    }

    /* An edit far above must not cost the table everything it knows. */
    {
        int before;
        note_syn_ckpts_fit(&t, g_nlines);
        for (top = 0; top + ROWS < g_nlines; top += ROWS)
            screen_lex(lang, &t, top, ROWS, sp, 8192);
        before = t.valid;
        note_syn_ckpts_dirty(&t, g_nlines - 1);
        ok(t.valid >= before - 1, "an edit on the last line keeps the table");
        note_syn_ckpts_dirty(&t, 0);
        ok(t.valid == 1, "an edit on the first line keeps only line zero");
    }
}

/* The state itself, in isolation: it has to say which construct is open, and
 * strings have to be told apart by their quote. */
static void test_state(void)
{
    note_syn_state st;
    int lang;

    note_syntax_init(&arena);
    lang = toy_lang();

    st = note_syntax_advance(lang, W("int a;\n"), 7, NOTE_SYN_NORMAL);
    ok(st == NOTE_SYN_NORMAL, "an ordinary line ends in the ordinary state");

    st = note_syntax_advance(lang, W("/* open\n"), 8, NOTE_SYN_NORMAL);
    ok(st == NOTE_SYN_BLOCK, "an unclosed block comment carries over");

    st = note_syntax_advance(lang, W(" still */ int a;\n"), 17, NOTE_SYN_BLOCK);
    ok(st == NOTE_SYN_NORMAL, "and is closed by the line that ends it");

    st = note_syntax_advance(lang, W("x = \"unterminated\n"), 18, NOTE_SYN_NORMAL);
    ok(st == NOTE_SYN_NORMAL, "a quote that dies at the line break carries nothing");

    st = note_syntax_advance(lang, W("x = \"tail \\\n"), 12, NOTE_SYN_NORMAL);
    ok(st >= NOTE_SYN_STRING, "a backslash at the line end continues the string");

    st = note_syntax_advance(lang, W("x = `raw\n"), 9, NOTE_SYN_NORMAL);
    ok(st >= NOTE_SYN_STRING, "a raw string carries over");
    ok(note_syntax_advance(lang, W("more\n"), 5, st) == st,
       "and stays open over a line that does not close it");
    ok(note_syntax_advance(lang, W("done` int a;\n"), 13, st) == NOTE_SYN_NORMAL,
       "until one does");

    /* Two quotes that both span lines are different states, or a backtick
     * would be closed by a double quote. */
    {
        note_syn_state a = note_syntax_advance(lang, W("`one\n"), 5, NOTE_SYN_NORMAL);
        note_syn_state b = note_syntax_advance(lang, W("\"two \\\n"), 7, NOTE_SYN_NORMAL);
        ok(a != b, "strings are told apart by their quote");
    }

    /* A run lexed with no room for spans still reports the right state. */
    {
        note_syn_state e = NOTE_SYN_NORMAL;
        note_span one;
        note_tokenize_from(lang, W("/* a\n"), 5, 0, &one, 1, NOTE_SYN_NORMAL, &e);
        ok(e == NOTE_SYN_BLOCK, "a full span array does not lose the end state");
    }
}

/* --------------------------------------------------------------------------
 * The bug the checkpoints replace.
 *
 * note_syntax_safe_start only looks back `window` characters, so a block
 * comment opened before that is invisible to it and the region below comes
 * out coloured as code.  The C64's window is 192 characters -- three or four
 * lines.  This shows the old answer being wrong and the new one being right on
 * exactly the same text.
 * -------------------------------------------------------------------------- */
static void test_hl_back(void)
{
    static note_syn_state slots[64];
    static note_span sp[4096];
    note_syn_ckpts t;
    note_syn_state st;
    int lang, i, top, from, n, start, end;
    const int HL_BACK = 192;            /* the C64's, from console_main.c */
    const int ROWS = 4;

    note_syntax_init(&arena);
    lang = toy_lang();

    /* A block comment opened on line 0 and still open forty lines later. */
    g_len = 0;
    {
        const char *head = "/* a comment that stays open\n";
        const char *body = "int a = 1;\n";
        const char *tail = "*/ int b;\n";
        for (i = 0; head[i]; i++) g_text[g_len++] = (nchar)(unsigned char)head[i];
        for (i = 0; i < 40; i++) {
            int j;
            for (j = 0; body[j]; j++) g_text[g_len++] = (nchar)(unsigned char)body[j];
        }
        for (i = 0; tail[i]; i++) g_text[g_len++] = (nchar)(unsigned char)tail[i];
        g_text[g_len] = 0;
    }
    doc_index();

    g_nfull = note_tokenize(lang, g_text, g_len, 0, g_full, 200000);
    top   = 30;
    start = g_ls[top];
    end   = line_end(top + ROWS - 1);
    ok(start - HL_BACK > 0, "the view is further into the file than the window");
    ok(kind_at(g_full, g_nfull, start) == TOK_COMMENT,
       "a full scan knows the view is inside the comment");

    /* The old way: scan back a window, lex from there. */
    from = note_syntax_safe_start(lang, g_text, g_len, start, HL_BACK);
    n = note_tokenize(lang, g_text + from, end - from, from, sp, 4096);
    ok(same_colours(sp, n, start, end) >= 0,
       "safe_start misses a comment opened before its window (the old bug)");

    /* The new way, with a table that has never seen the file: the fit-and-scan
     * pass starts at line 0, so there is nothing to miss. */
    note_syn_ckpts_init(&t, slots, 64);
    note_syn_ckpts_fit(&t, g_nlines);
    n = screen_lex(lang, &t, top, ROWS, sp, 4096);
    ok(same_colours(sp, n, start, end) < 0,
       "checkpoints colour it as the full scan does");
    from = note_syn_ckpts_state(&t, top, &st);
    ok(st == NOTE_SYN_BLOCK, "and know the view is inside a block comment");
}

/* The shipped pack is the real input; make sure it parses and that its
 * heaviest expressions actually fit the compiled program. */
static void test_pack(const char *path)
{
    static nchar text[900000];
    FILE *f = fopen(path, "rb");
    long size;
    int i, ch, before, added = 0, rules = 0, uncompilable = 0;
    note_regex re;

    if (!f) { printf("  (no pack at %s, skipping)\n", path); return; }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 890000) size = 890000;

    /* The pack is UTF-8; every byte we care about here is ASCII. */
    i = 0;
    while (i < size && (ch = fgetc(f)) != EOF) text[i++] = (nchar)(unsigned char)ch;
    text[i] = 0;
    fclose(f);

    note_syntax_init(&arena);
    before = note_lang_count();

    {   /* Split on the pack's dashed separator, as the core does. */
        nchar *p = text, *seg = 0;
        while (*p) {
            nchar *line = p, *eol;
            while (*p && *p != (nchar)'\n') p++;
            eol = p;
            if (*p) p++;
            if (eol - line >= 3 && line[0] == (nchar)'-' &&
                line[1] == (nchar)'-' && line[2] == (nchar)'-') {
                if (seg) { *line = 0; if (note_syntax_add(&arena, seg) >= 0) added++; }
                seg = p;
            }
        }
        if (seg && *seg && note_syntax_add(&arena, seg) >= 0) added++;
    }

    printf("  pack: %d definitions, registry %d -> %d\n",
           added, before, note_lang_count());
    ok(added > 50, "the pack parses into many languages");
    ok(note_lang_from_path(W("x.py")) != LANG_NONE, "the pack claims .py");
    ok(note_lang_from_path(W("x.c"))  != LANG_NONE, "the pack claims .c");

    for (i = 1; i < note_lang_count(); i++) {
        const note_lang *L = note_lang_get(i);
        int r;
        for (r = 0; r < (int)L->nrules; r++) {
            rules++;
            if (note_regex_compile(&re, L->rules[r].pattern, 0)) continue;
            uncompilable++;
            if (uncompilable <= 5) {
                int j;
                printf("    rejected in %-14s ", "");
                for (j = 0; L->name[j] && j < 14; j++) putchar((char)L->name[j]);
                printf(": ");
                for (j = 0; L->rules[r].pattern[j] && j < 60; j++)
                    putchar((char)L->rules[r].pattern[j]);
                putchar('\n');
            }
        }
    }
    if (rules) {
        printf("  rules: %d total, %d would not compile\n", rules, uncompilable);
        ok(uncompilable * 5 <= rules, "at most a fifth of the rules are rejected");
    }

    /* End to end on the real definitions: the shipped C rules must actually
     * colour the things word lists never could. */
    {
        int c = note_lang_from_path(W("main.c"));
        const nchar *src = W("int n = 0x1F;\rn += MAX_LEN * 2;\r");
        note_span sp[256];
        int n;

        ok(c != LANG_NONE, "the pack provides C");
        n = note_tokenize(c, src, n_len(src), 0, sp, 256);
        expect_kind(sp, n, src, "0x1F",    TOK_NUMBER,   "pack C colours a hex literal");
        expect_kind(sp, n, src, "+=",      TOK_OPERATOR, "pack C colours an operator");
        expect_kind(sp, n, src, "MAX_LEN", TOK_TYPE,     "pack C colours an all-caps name");
        expect_kind(sp, n, src, "int",     TOK_TYPE,     "pack C colours a basic type");
    }
}

/* --------------------------------------------------------------------------
 * Scrolling benchmark.
 *
 * The cost that matters is not lexing a file once, it is lexing it again for
 * every frame while a key is held down.  This walks a viewport down a real
 * file and times the two strategies against each other: lexing from the top
 * of the document each time, and lexing from the nearest safe point.
 * -------------------------------------------------------------------------- */
static void bench(const char *path)
{
    static nchar text[4 * 1024 * 1024];
    note_span sp[4096];
    FILE *f = fopen(path, "rb");
    long size;
    int len = 0, ch, lang, frame, frames = 200;
    clock_t t0;
    double from_top, from_safe;
    const int VIEW = 4000;              /* about a screenful of a large window */
    const int WINDOW = 32 * 1024;

    if (!f) { printf("  cannot open %s\n", path); return; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    while (len < (int)(sizeof(text) / sizeof(text[0])) - 1 &&
           (ch = fgetc(f)) != EOF)
        text[len++] = (nchar)(unsigned char)ch;
    text[len] = 0;
    fclose(f);

    lang = note_lang_from_path(W(path));
    printf("\n  %s\n", path);
    printf("    %d bytes, language %s\n", (int)size,
           lang == LANG_NONE ? "none" : "detected");
    if (lang == LANG_NONE) return;

    /* Lexing from the top of the document, which is what note does today for
     * anything under its size threshold. */
    t0 = clock();
    for (frame = 0; frame < frames; frame++) {
        int at = (int)((double)frame / frames * (len - VIEW));
        if (at < 0) at = 0;
        note_tokenize(lang, text, at + VIEW, 0, sp, 4096);
    }
    from_top = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / frames;

    /* Lexing from the nearest point known to be outside any construct. */
    t0 = clock();
    for (frame = 0; frame < frames; frame++) {
        int at = (int)((double)frame / frames * (len - VIEW));
        int s;
        if (at < 0) at = 0;
        s = note_syntax_safe_start(lang, text, len, at, WINDOW);
        note_tokenize(lang, text + s, at + VIEW - s, s, sp, 4096);
    }
    from_safe = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / frames;

    printf("    from the top   %8.2f ms per frame\n", from_top);
    printf("    from safe start%8.2f ms per frame   (%.0fx faster)\n",
           from_safe, from_safe > 0 ? from_top / from_safe : 0.0);
    ok(from_safe < 16.0, "a frame stays inside a 60 Hz budget");
}

/* --------------------------------------------------------------------------
 * Scaling.
 *
 * The claim being measured is that colouring one screenful costs the screen
 * and not the document.  The comparison is against the only correct thing the
 * lexer could do before -- start at the top of the file -- because
 * safe_start's shortcut is cheap by being wrong (see test_hl_back), and a
 * shortcut that is wrong is not the thing to beat.
 * -------------------------------------------------------------------------- */
static unsigned long g_rnd = 12345;
static int rnd_below(int n)
{
    g_rnd = g_rnd * 1103515245ul + 12345ul;
    return (int)((g_rnd >> 8) % (unsigned long)n);
}

/* clock() here ticks a millisecond at best, so a frame that costs a tenth of
 * one has to be repeated until the total is worth reading. */
#define BENCH_MS 250
#define BENCH_CAP 400

static void bench_scale(void)
{
    static const long kSizes[3] = { 100L * 1024, 1024L * 1024, 10L * 1024 * 1024 };
    const int ROWS = 40, FIXED = 4096;
    note_syn_state *fixed_slots, *sized_slots;
    note_span *sp;
    nchar *text;
    int *ls;
    int lang, s;

    note_syntax_init(&arena);
    lang = toy_lang();

    fixed_slots = (note_syn_state *)malloc((size_t)FIXED);
    sized_slots = (note_syn_state *)malloc((size_t)(kSizes[2] / 8));
    sp    = (note_span *)malloc(sizeof(note_span) * 20000);
    text  = (nchar *)malloc(sizeof(nchar) * (size_t)(kSizes[2] + 512));
    ls    = (int *)malloc(sizeof(int) * (size_t)(kSizes[2] / 8));
    if (!fixed_slots || !sized_slots || !sp || !text || !ls) {
        printf("  (out of memory, skipping)\n");
        return;
    }

    printf("\n  colouring one %d-line screen at a random point, by document size\n",
           ROWS);
    printf("    %-8s %-8s %10s %10s   %10s %-5s %10s %-5s\n",
           "size", "lines", "from top", "safe_start",
           "4 KB table", "K", "sized", "K");

    for (s = 0; s < 3; s++) {
        note_syn_ckpts fixed, sized;
        long want = kSizes[s];
        int len = 0, nlines = 0, i, k, frames, top;
        clock_t t0;
        double top_ms, safe_ms, fixed_ms = 0, sized_ms = 0;

        while (len < want)
            for (i = 0; kBlock[i]; i++) text[len++] = (nchar)(unsigned char)kBlock[i];
        text[len] = 0;
        for (i = 0; i < len; i++) if (text[i] == (nchar)'\n') nlines++;
        nlines++;
        ls[0] = 0;
        k = 1;
        for (i = 0; i < len; i++) if (text[i] == (nchar)'\n') ls[k++] = i + 1;

        /* Two tables: one the size a small machine would give it, and one
         * sized to the document as a desktop can afford -- a byte per thirty
         * lines is 19 KB for the 10 MB file. */
        note_syn_ckpts_init(&fixed, fixed_slots, FIXED);
        note_syn_ckpts_fit(&fixed, nlines);
        note_syn_ckpts_init(&sized, sized_slots, nlines / 30 + 2);
        note_syn_ckpts_fit(&sized, nlines);

        /* One pass to fill both, as a backend would on open.  It is the only
         * time anything here touches the document whole. */
        note_syn_ckpts_scan(&fixed, lang, text, len, 0, NOTE_SYN_NORMAL);
        note_syn_ckpts_scan(&sized, lang, text, len, 0, NOTE_SYN_NORMAL);

        /* Correct, and proportional to how far down the file the view is:
         * with nothing remembered, everything above the view has to be walked
         * again to know what the view is inside of. */
        t0 = clock(); frames = 0;
        do {
            note_syn_state st;
            top = rnd_below(nlines - ROWS - 1);
            st = note_syntax_advance(lang, text, ls[top], NOTE_SYN_NORMAL);
            note_tokenize_from(lang, text + ls[top], ls[top + ROWS] - ls[top],
                               ls[top], sp, 20000, st, 0);
            frames++;
        } while (frames < BENCH_CAP &&
                 clock() - t0 < (clock_t)(CLOCKS_PER_SEC / 1000 * BENCH_MS));
        top_ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / frames;

        /* Cheap, near enough flat, and wrong below a block comment. */
        t0 = clock(); frames = 0;
        do {
            int from, end;
            top  = rnd_below(nlines - ROWS - 1);
            end  = ls[top + ROWS];
            from = note_syntax_safe_start(lang, text, len, ls[top], 32 * 1024);
            note_tokenize(lang, text + from, end - from, from, sp, 20000);
            frames++;
        } while (frames < BENCH_CAP &&
                 clock() - t0 < (clock_t)(CLOCKS_PER_SEC / 1000 * BENCH_MS));
        safe_ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / frames;

        for (i = 0; i < 2; i++) {
            note_syn_ckpts *t = i ? &sized : &fixed;
            t0 = clock(); frames = 0;
            do {
                note_syn_state st, after;
                int from, end;
                top  = rnd_below(nlines - ROWS - 1);
                end  = ls[top + ROWS];
                from = note_syn_ckpts_state(t, top, &st);
                if (from < top)
                    st = note_syn_ckpts_scan(t, lang, text + ls[from],
                                             ls[top] - ls[from], from, st);
                note_tokenize_from(lang, text + ls[top], end - ls[top], ls[top],
                                   sp, 20000, st, &after);
                frames++;
            } while (frames < BENCH_CAP &&
                     clock() - t0 < (clock_t)(CLOCKS_PER_SEC / 1000 * BENCH_MS));
            if (i) sized_ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / frames;
            else   fixed_ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / frames;
        }

        printf("    %-8s %-8d %8.3fms %8.3fms   %8.3fms %-5d %8.3fms %-5d\n",
               s == 0 ? "100 KB" : (s == 1 ? "1 MB" : "10 MB"),
               nlines, top_ms, safe_ms,
               fixed_ms, fixed.every, sized_ms, sized.every);

        if (s == 2)
            ok(sized_ms < 1.0,
               "a screen of a 10 MB file colours in under a millisecond");
    }

    free(fixed_slots); free(sized_slots); free(sp); free(text); free(ls);
}

int main(int argc, char **argv)
{
    printf("note_syntax tests\n");

    test_registry();
    test_added_language();
    test_scanner();
    test_rules();
    test_safe_start();
    test_state();
    test_incremental();
    test_hl_back();
    test_pack(argc > 1 ? argv[1] : "assets/syntax.pack");
    bench_scale();

    /* Any further arguments are files to benchmark scrolling on. */
    {
        int i;
        for (i = 2; i < argc; i++) bench(argv[i]);
    }

    printf("\n%d checks, %d failed\n", checks, failures);
    if (failures) { printf("FAILED\n"); return 1; }
    printf("all passed\n");
    return 0;
}
