/* test_gbbasic.c -- exercises the Game Boy BASIC's tokeniser and evaluator.
 *
 * The point of gb_basic.c having no hardware in it is that it can be tested
 * here, at speed, instead of by squinting at an emulator.  A host program, so
 * the CRT is fair game; the code under test still may not use it.
 *
 *   cl /nologo /W4 /TC tests\test_gbbasic.c src\platform\gb\gb_basic.c
 */

#include <stdio.h>
#include <string.h>

#include "../src/platform/gb/gb_basic.h"

static int checks, failures;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL  %s\n", what); }
}

/* ---- output capture ----------------------------------------------------- */

static char g_out[256];
static int  g_outn;

static void capture(char c)
{
    if (g_outn < (int)sizeof(g_out) - 1) g_out[g_outn++] = c;
    g_out[g_outn] = 0;
}

static bas_int fake_joy(void) { return 5; }

static bas_vm       g_vm;
static unsigned char g_storage[512];

static void reset(void)
{
    int i;
    for (i = 0; i < 26; i++) g_vm.var[i] = 0;
    g_vm.out = capture;
    g_vm.joy = fake_joy;
    g_vm.err = BAS_OK;
    bas_prog_init(&g_vm, g_storage, (int)sizeof(g_storage));
    g_outn = 0;
    g_out[0] = 0;
}

/* ---- helpers ------------------------------------------------------------ */

static unsigned char g_tok[128];
static int           g_toklen;
static bas_int       g_num;

static void tok(const char *src)
{
    g_toklen = bas_tokenise(src, g_tok, (int)sizeof(g_tok), &g_num);
}

/* Round-trips a line through both directions and compares with `want`, which
 * is the canonical spacing rather than what was typed. */
static void roundtrip(const char *src, const char *want)
{
    char out[128];

    tok(src);
    checks++;
    if (g_toklen < 0) { failures++; printf("  FAIL  %s (did not fit)\n", src); return; }

    bas_detokenise(g_tok, g_toklen, out, (int)sizeof(out));
    if (strcmp(out, want) != 0) {
        failures++;
        printf("  FAIL  round trip of %s\n        want %-28s got %s\n", src, want, out);
    }
}

/* Tokenised as an expression -- null `number` -- so a leading digit stays part
 * of the sum.  The error is cleared first because it is sticky within one
 * evaluation on purpose: without that, the first bad expression here would
 * make every later one look broken too. */
static bas_int eval(const char *expr)
{
    int at = 0;
    g_toklen = bas_tokenise(expr, g_tok, (int)sizeof(g_tok), 0);
    g_vm.err = BAS_OK;
    return bas_eval(&g_vm, g_tok, g_toklen, &at);
}

static void expect_out(const char *src, const char *want)
{
    g_outn = 0; g_out[0] = 0;
    tok(src);
    bas_exec_line(&g_vm, g_tok, g_toklen);
    checks++;
    if (strcmp(g_out, want) != 0) {
        failures++;
        printf("  FAIL  %s\n        want %-24s got %s\n", src, want, g_out);
    }
}

/* ------------------------------------------------------------------ */

static void test_tokenise(void)
{
    reset();

    tok("10 PRINT \"HI\"");
    ok(g_num == 10, "the leading number is the line number");
    ok(g_tok[0] == T_PRINT, "PRINT became one byte");
    ok(g_toklen == 5, "and the whole line is five bytes");

    tok("PRINT 1");
    ok(g_num == -1, "a line with no number is a direct command");

    /* The saving that justifies storing tokens at all. */
    tok("10 FOR I=1 TO 1000");
    ok(g_toklen == 10, "FOR I=1 TO 1000 is ten bytes, not sixteen");

    /* A keyword that is the front of a longer name is not a keyword. */
    tok("TOTAL=1");
    ok(g_tok[0] == 'T' && g_tok[1] == 'O', "TOTAL is a name, not TO followed by TAL");
    tok("GOTO 10");
    ok(g_tok[0] == T_GOTO, "GOTO still tokenises");

    /* Case does not matter, and is not preserved. */
    tok("print a");
    ok(g_tok[0] == T_PRINT, "lower-case keywords tokenise");
    ok(g_tok[1] == 'A', "identifiers are upper-cased");

    /* Numbers are two bytes whatever they looked like. */
    tok("A=32767");
    ok(g_toklen == 5, "a five-digit literal still costs three bytes");
}

static void test_roundtrip(void)
{
    reset();

    roundtrip("10 PRINT \"HI\"",      "PRINT \"HI\"");
    roundtrip("20 GOTO 10",           "GOTO 10");
    roundtrip("30 LET A=B+1",         "LET A=B+1");
    roundtrip("40 IF A>10 THEN GOTO 5", "IF A>10 THEN GOTO 5");
    roundtrip("50 FOR I=1 TO 10 STEP 2", "FOR I=1 TO 10 STEP 2");
    roundtrip("60 PRINT A;B",         "PRINT A;B");
    roundtrip("70 A=B<>C",            "A=B<>C");
    roundtrip("80 A=B<=C",            "A=B<=C");

    /* A comment is text, and tokenising it would change what it says. */
    roundtrip("90 REM keep This as-is", "REM keep This as-is");

    /* A string keeps exactly what was typed, including keywords inside it. */
    roundtrip("100 PRINT \"GOTO 10\"", "PRINT \"GOTO 10\"");

    /* Spacing is canonical, not as typed. */
    roundtrip("110 PRINT   A",        "PRINT A");
    /* Real eight-bit BASICs read PRINTA as PRINT A, which is also why their
     * variables were one or two characters: the rule that lets PRINTA split
     * is the rule that splits TOTAL into TO and TAL.  Names win here. */
    roundtrip("120 PRINT A",          "PRINT A");
    tok("PRINTA");
    ok(g_tok[0] == 'P' && g_toklen == 6, "PRINTA is one name, not PRINT A");
}

static void test_eval(void)
{
    reset();

    ok(eval("1+2*3") == 7,        "multiplication binds tighter");
    ok(eval("(1+2)*3") == 9,      "parentheses");
    ok(eval("7/2") == 3,          "integer division truncates");
    ok(eval("-5+2") == -3,        "unary minus");
    ok(eval("7 MOD 3") == 1,      "MOD");
    ok(eval("10-2-3") == 5,       "subtraction is left-associative");

    ok(eval("2>1") == 1,          "true is 1");
    ok(eval("1>2") == 0,          "false is 0");
    ok(eval("2<>3") == 1,         "<>");
    ok(eval("2<=2") == 1,         "<=");
    ok(eval("1=1 AND 2=2") == 1,  "AND");
    ok(eval("1=2 OR 2=2") == 1,   "OR");
    ok(eval("NOT 0") == 1,        "NOT");

    g_vm.var['A' - 'A'] = 7;
    ok(eval("A*2") == 14,         "a variable reads back");
    ok(eval("Z") == 0,            "an untouched variable is zero");
    ok(eval("ABS(0-9)") == 9,     "ABS");
    ok(eval("SGN(0-3)") == -1,    "SGN");
    ok(eval("JOY()") == 5,        "JOY comes from the host");

    /* Sixteen bits, and the tests are written in a type that cannot quietly
     * have more range than the hardware. */
    g_vm.var['A' - 'A'] = 30000;
    ok(eval("A+30000") == (bas_int)60000, "arithmetic wraps at sixteen bits");

    g_vm.err = BAS_OK;
    eval("1/0");
    ok(g_vm.err == BAS_DIV_ZERO,  "division by zero is an error");

    g_vm.err = BAS_OK;
    eval("1+");
    ok(g_vm.err == BAS_SYNTAX,    "a truncated expression is a syntax error");
}

static void test_exec(void)
{
    reset();

    expect_out("PRINT \"HI\"",    "HI\n");
    expect_out("PRINT 6*7",       "42\n");
    expect_out("PRINT 0-42",      "-42\n");
    /* A semicolon between items joins them; only a trailing one suppresses
     * the newline, which is how BASIC has always read. */
    expect_out("PRINT \"A\";\"B\"", "AB\n");
    expect_out("REM nothing",     "");

    reset();
    tok("A=6*7");
    bas_exec_line(&g_vm, g_tok, g_toklen);
    ok(g_vm.var[0] == 42,         "assignment without LET");

    tok("LET B=A+1");
    bas_exec_line(&g_vm, g_tok, g_toklen);
    ok(g_vm.var[1] == 43,         "assignment with LET");

    tok("END");
    ok(bas_exec_line(&g_vm, g_tok, g_toklen) == BAS_STOPPED, "END stops");
}

static void test_program(void)
{
    char out[64];
    int  off;

    reset();

    tok("20 PRINT B");  bas_prog_set(&g_vm, 20, g_tok, g_toklen);
    tok("10 PRINT A");  bas_prog_set(&g_vm, 10, g_tok, g_toklen);
    tok("30 END");      bas_prog_set(&g_vm, 30, g_tok, g_toklen);

    ok(bas_prog_find(&g_vm, 10) == 0, "lines are kept in ascending order");
    ok(bas_prog_find(&g_vm, 20) > 0,  "the second line is found");
    ok(bas_prog_find(&g_vm, 99) < 0,  "a missing line is not found");

    /* Inserting between two lines moves the tail rather than appending. */
    tok("15 PRINT C");
    bas_prog_set(&g_vm, 15, g_tok, g_toklen);
    off = bas_prog_find(&g_vm, 15);
    ok(off > bas_prog_find(&g_vm, 10) && off < bas_prog_find(&g_vm, 20),
       "an inserted line lands between its neighbours");

    /* Replacing with something longer, then shorter. */
    tok("15 PRINT C+D+E+F");
    bas_prog_set(&g_vm, 15, g_tok, g_toklen);
    off = bas_prog_find(&g_vm, 15);
    bas_detokenise(&g_vm.prog[off + 3], g_vm.prog[off + 2], out, (int)sizeof(out));
    ok(strcmp(out, "PRINT C+D+E+F") == 0, "a line can be replaced by a longer one");
    ok(bas_prog_find(&g_vm, 20) > off,    "and its neighbour survives");

    tok("15 END");
    bas_prog_set(&g_vm, 15, g_tok, g_toklen);
    off = bas_prog_find(&g_vm, 15);
    bas_detokenise(&g_vm.prog[off + 3], g_vm.prog[off + 2], out, (int)sizeof(out));
    ok(strcmp(out, "END") == 0,           "and by a shorter one");

    /* A zero-length line is a deletion, which is how BASIC has always done it. */
    bas_prog_set(&g_vm, 15, g_tok, 0);
    ok(bas_prog_find(&g_vm, 15) < 0,      "an empty line deletes");
    ok(bas_prog_find(&g_vm, 20) >= 0,     "and leaves the rest alone");

    /* Filling the store must refuse cleanly rather than run off the end. */
    reset();
    {
        int n = 0;
        tok("1 PRINT \"XXXXXXXXXXXXXXXXXXXX\"");
        while (bas_prog_set(&g_vm, (bas_int)(n + 1), g_tok, g_toklen)) {
            n++;
            if (n > 200) break;
        }
        ok(n > 0 && n <= 200, "the store fills and then refuses");
        ok(g_vm.prog_len <= g_vm.prog_cap, "without running past its capacity");
    }
}

int main(void)
{
    printf("gb BASIC tests\n");

    test_tokenise();
    test_roundtrip();
    test_eval();
    test_exec();
    test_program();

    printf("\n%d checks, %d failed\n", checks, failures);
    if (failures) { printf("FAILED\n"); return 1; }
    printf("all passed\n");
    return 0;
}
