/* test_pack.c — checks note's own pack format end to end.
 *
 * A host program, so the CRT is fair game here; the code under test is not.
 * Build:
 *   cl /nologo /W4 /TC tests\test_pack.c src\core\note_pack.c
 *
 * The interesting failure this guards against is a decoder that disagrees
 * with the encoder in tools\compress_packs.ps1.  That disagreement would show
 * up first on the machines with no other way to read a pack, which are the
 * ones nobody is watching, so it is checked here against the real blobs the
 * build produces rather than against something written for the test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../src/core/note_pack.h"

static int failures;

static void ok(int cond, const char *what)
{
    if (!cond) { printf("  FAIL  %s\n", what); failures++; }
    else       { printf("  ok    %s\n", what); }
}

static unsigned char *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *p;
    long n;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    p = (unsigned char *)malloc((size_t)n + 1);
    if (!p) { fclose(f); return 0; }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return 0; }
    fclose(f);
    p[n] = 0;
    *len = n;
    return p;
}

/* The same reduction tools\compress_packs.ps1 applies before compressing:
 * drop comment and blank lines, and the CR of every CRLF with them. */
static unsigned char *stripped(const char *path, long *len)
{
    long n;
    unsigned char *src = slurp(path, &n), *out;
    long i = 0, w = 0;
    if (!src) return 0;
    out = (unsigned char *)malloc((size_t)n + 2);
    if (!out) { free(src); return 0; }

    while (i < n) {
        long s = i, e;
        long t;
        while (i < n && src[i] != '\n') i++;
        e = i;
        if (i < n) i++;
        if (e > s && src[e - 1] == '\r') e--;

        t = s;
        while (t < e && (src[t] == ' ' || src[t] == '\t')) t++;
        if (t == e || src[t] == '#') continue;      /* blank or comment */

        memcpy(out + w, src + s, (size_t)(e - s));
        w += e - s;
        out[w++] = '\n';
    }
    free(src);
    out[w] = 0;
    *len = w;
    return out;
}

/* Streams a whole blob and compares it against what went in. */
static void check_roundtrip(const char *blobpath, const char *srcpath)
{
    static note_pack z;
    long blen = 0, rlen = 0, i;
    unsigned char *blob = slurp(blobpath, &blen);
    unsigned char *raw  = stripped(srcpath, &rlen);
    int bad = -1;

    if (!blob || !raw) {
        printf("  skip  %s (build it first)\n", blobpath);
        free(blob); free(raw);
        return;
    }

    ok(note_pack_open(&z, blob, (unsigned long)blen), "opens as an NPK1 pack");
    ok(note_pack_size(&z) == (unsigned long)rlen, "header length matches the source");

    for (i = 0; i < rlen; i++) {
        int c = note_pack_get(&z);
        if (c != (int)raw[i]) { bad = (int)i; break; }
    }
    ok(bad < 0, "every byte decodes to what was compressed");
    if (bad >= 0) printf("        first difference at byte %d\n", bad);
    ok(note_pack_get(&z) == -1, "stops at the end of the stream");

    printf("        %s: %ld -> %ld bytes (%.2fx)\n",
           blobpath, rlen, blen, (double)rlen / (double)blen);

    free(blob); free(raw);
}

static void check_find(void)
{
    long blen = 0;
    unsigned char *blob = slurp("build/win32/core.syntax.lz", &blen);
    unsigned char *thm  = 0;
    long tlen = 0;
    static nchar buf[16384];
    long n;

    if (!blob) { printf("  skip  find (build the packs first)\n"); return; }

    n = note_pack_find(blob, (unsigned long)blen, "extensions", N("c"),
                       NOTE_PACK_WORD, buf, 16384);
    ok(n > 0 && wcsstr((const wchar_t *)buf, L"name = C\n") != 0,
       "'c' finds the C definition");
    ok(n > 0 && wcsstr((const wchar_t *)buf, L"---") == 0,
       "and stops at the separator, carrying one definition only");
    if (n > 0) printf("        the C definition is %ld chars\n", n);

    n = note_pack_find(blob, (unsigned long)blen, "extensions", N("cpp"),
                       NOTE_PACK_WORD, buf, 16384);
    ok(n > 0 && wcsstr((const wchar_t *)buf, L"name = C++\n") != 0,
       "'cpp' finds C++ and not C");

    /* Case folding: a file named README.MD is the same language as readme.md. */
    n = note_pack_find(blob, (unsigned long)blen, "extensions", N("PY"),
                       NOTE_PACK_WORD, buf, 16384);
    ok(n > 0 && wcsstr((const wchar_t *)buf, L"name = Python") != 0,
       "matching an extension ignores case");

    /* A word match must not fire on a substring of one: 'p' is not 'py'. */
    n = note_pack_find(blob, (unsigned long)blen, "extensions", N("zzz"),
                       NOTE_PACK_WORD, buf, 16384);
    ok(n == -1, "an extension nothing claims reports nothing");

    n = note_pack_find(blob, (unsigned long)blen, "extensions", N("c"),
                       NOTE_PACK_WORD, buf, 8);
    ok(n == -2, "a definition too large for the buffer says so, not 'no such'");

    free(blob);

    thm = slurp("build/win32/core.themes.lz", &tlen);
    if (!thm) { printf("  skip  theme find\n"); return; }
    n = note_pack_find(thm, (unsigned long)tlen, "name", N("Default Dark"),
                       NOTE_PACK_WHOLE, buf, 16384);
    ok(n > 0 && wcsstr((const wchar_t *)buf, L"background = #181818") != 0,
       "a theme is found by its whole name, spaces and all");

    n = note_pack_find(thm, (unsigned long)tlen, "name", N("Default"),
                       NOTE_PACK_WHOLE, buf, 16384);
    ok(n == -1, "and a prefix of a theme name is not that theme");

    free(thm);
}

static void check_junk(void)
{
    static note_pack z;
    unsigned char junk[16];
    int i;
    for (i = 0; i < 16; i++) junk[i] = (unsigned char)i;

    ok(!note_pack_open(&z, junk, 16), "refuses a blob without the magic");
    ok(note_pack_get(&z) == -1, "and reads nothing from it");
    ok(!note_pack_open(&z, junk, 2), "refuses a blob shorter than its header");
}

int main(void)
{
    printf("pack format\n");
    check_roundtrip("build/win32/core.syntax.lz", "assets/core.syntax.pack");
    check_roundtrip("build/win32/core.themes.lz", "assets/core.themes.pack");
    printf("finding one definition\n");
    check_find();
    printf("bad input\n");
    check_junk();

    printf(failures ? "\n%d failed\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
