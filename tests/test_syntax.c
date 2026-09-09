/* test_syntax.c — exercises the language registry and the lexer.
 *
 * A host program, so the CRT is fair game here; the code under test is not.
 * Build:
 *   cl /nologo /W4 /TC tests\test_syntax.c src\core\note_syntax.c ^
 *      src\core\note_conf.c src\core\note_regex.c src\core\note_core.c
 */

#include <stdio.h>
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

    note_syntax_init(&arena);

    c = note_lang_from_path(W("C:\\src\\main.c"));
    ok(c != LANG_NONE, "a .c file finds a language");
    ok(note_lang_get(c)->keywords != 0, "that language has keywords");

    unknown = note_lang_from_path(W("notes.zzz"));
    ok(unknown == LANG_NONE, "an unknown extension finds nothing");
    ok(note_lang_from_path(W("plain")) == LANG_NONE, "no extension finds nothing");

    ok(note_lang_count() > 1, "the registry holds the built-ins");
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

int main(int argc, char **argv)
{
    printf("note_syntax tests\n");

    test_registry();
    test_added_language();
    test_scanner();
    test_rules();
    test_safe_start();
    test_pack(argc > 1 ? argv[1] : "assets/syntax.pack");

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
