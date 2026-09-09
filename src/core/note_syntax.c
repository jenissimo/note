/* note_syntax.c — language registry and lexer.
 *
 * The built-in languages below are written in the same note_conf format a
 * user drops into <exe>\syntax or %LOCALAPPDATA%\note\syntax, and are parsed
 * by the same note_syntax_add().  There is deliberately only one code path.
 */

#include "note_syntax.h"
#include "note_regex.h"

/* ==========================================================================
 * Built-in definitions
 * ========================================================================== */

/* No languages are compiled in.
 *
 * They used to be, as a fallback for a build with no pack beside it — but the
 * curated pack is now compiled into the executable as a resource, which is
 * strictly better: the same format, more languages, and compressed to a fifth
 * of the space these took as C string literals.  Keeping both would have been
 * six kilobytes of an executable aiming at sixty-four, to say the same thing
 * twice.
 *
 * If neither the resource nor a pack on disk is present, note simply has no
 * languages and highlights nothing, which is what a plain text editor does.
 */
static const char *const note_builtin_syntax[] = { 0 };

/* ==========================================================================
 * Registry
 * ========================================================================== */

static note_lang g_langs[NOTE_MAX_LANGS];
static int       g_nlangs;

int note_lang_count(void) { return g_nlangs; }

const note_lang *note_lang_get(int lang)
{
    if (lang < 0 || lang >= g_nlangs) return &g_langs[0];
    return &g_langs[lang];
}

/* Splits a block_comment value such as "SLASHSTAR STARSLASH" into its
 * opening and closing halves. */
static void split_pair(note_arena *ar, const nchar *v,
                       const nchar **open, const nchar **close)
{
    const nchar *p = v;
    int n;
    *open = *close = 0;
    while (*p && *p != (nchar)' ') p++;
    n = (int)(p - v);
    if (!n) return;
    *open = note_arena_put(ar, v, n);
    while (*p == (nchar)' ') p++;
    if (*p) *close = note_arena_put(ar, p, -1);
}

/* "operator \s+pattern" -> the kind and the expression that follows it. */
static const struct { const nchar *name; unsigned char kind; } kRuleKinds[] = {
    { N("keyword"),  TOK_KEYWORD  },
    { N("type"),     TOK_TYPE     },
    { N("comment"),  TOK_COMMENT  },
    { N("string"),   TOK_STRING   },
    { N("number"),   TOK_NUMBER   },
    { N("preproc"),  TOK_PREPROC  },
    { N("operator"), TOK_OPERATOR }
};

static void add_rule(note_arena *ar, note_lang *L, const nchar *val)
{
    nchar word[32];
    const nchar *p = val;
    int i, n = 0;

    if (L->nrules >= NOTE_MAX_RULES) return;

    while (*p && *p != (nchar)' ' && n < 31) word[n++] = *p++;
    word[n] = 0;
    while (*p == (nchar)' ') p++;
    if (!*p) return;

    for (i = 0; i < (int)(sizeof(kRuleKinds) / sizeof(kRuleKinds[0])); i++) {
        if (n_eq(word, kRuleKinds[i].name)) {
            const nchar *pat = note_arena_put(ar, p, -1);
            if (!pat) return;
            L->rules[L->nrules].pattern = pat;
            L->rules[L->nrules].kind    = kRuleKinds[i].kind;
            L->nrules++;
            return;
        }
    }
}

/* Defined with the rule cache further down.  The registry has to be able to
 * invalidate it, and the cache needs the registry's types, so the two are
 * declared here and defined there. */
static int g_nrules;
static int g_rules_lang;

int note_syntax_add(note_arena *ar, const nchar *text)
{
    nchar key[64], val[4096];
    const nchar *p = text;
    note_lang L;
    int i, slot = -1;

    for (i = 0; i < (int)sizeof(L); i++) ((unsigned char *)&L)[i] = 0;

    while (note_conf_next(&p, key, 64, val, 4096)) {
        if      (n_eq(key, N("name")))          L.name         = note_arena_put(ar, val, -1);
        else if (n_eq(key, N("extensions")))    L.exts         = note_arena_put(ar, val, -1);
        else if (n_eq(key, N("keywords")))      L.keywords     = note_arena_put(ar, val, -1);
        else if (n_eq(key, N("types")))         L.types        = note_arena_put(ar, val, -1);
        else if (n_eq(key, N("line_comment")))  L.line_comment = note_arena_put(ar, val, -1);
        else if (n_eq(key, N("quotes")))        L.quotes       = note_arena_put(ar, val, -1);
        else if (n_eq(key, N("block_comment"))) split_pair(ar, val, &L.block_open, &L.block_close);
        else if (n_eq(key, N("rule")))          add_rule(ar, &L, val);
        else if (n_eq(key, N("preproc")))   { if (note_conf_bool(val)) L.flags |= SYN_PREPROC;  }
        else if (n_eq(key, N("tags")))      { if (note_conf_bool(val)) L.flags |= SYN_TAGS;     }
        else if (n_eq(key, N("headings")))  { if (note_conf_bool(val)) L.flags |= SYN_HEADINGS; }
        else if (n_eq(key, N("case_sensitive"))) { if (!note_conf_bool(val)) L.flags |= SYN_NOCASE; }
    }

    if (!L.name) return -1;

    /* A file that names an existing language replaces it. */
    for (i = 1; i < g_nlangs; i++)
        if (g_langs[i].name && n_eq(g_langs[i].name, L.name)) { slot = i; break; }

    if (slot < 0) {
        if (g_nlangs >= NOTE_MAX_LANGS) return -1;
        slot = g_nlangs++;
    }
    g_langs[slot] = L;

    /* Replacing a language invalidates any rules compiled from the old one. */
    if (slot == g_rules_lang) g_rules_lang = -1;

    return slot;
}

/* The built-in definitions are stored a byte per character and widened here.
 * They are pure ASCII, so as wide literals they were exactly twice the size
 * they needed to be -- six kilobytes of an executable that is trying to fit
 * in sixty-four.  One shared buffer is enough because they are parsed one at
 * a time, at startup, before anything else runs. */
static nchar g_widen[NOTE_BUILTIN_MAX];

const nchar *note_syntax_widen(const char *s)
{
    int i = 0;
    while (s[i] && i < NOTE_BUILTIN_MAX - 1) {
        g_widen[i] = (nchar)(unsigned char)s[i];
        i++;
    }
    g_widen[i] = 0;
    return g_widen;
}

void note_syntax_init(note_arena *ar)
{
    int i;

    note_arena_reset(ar);
    for (i = 0; i < NOTE_MAX_LANGS; i++) {
        int j;
        for (j = 0; j < (int)sizeof(note_lang); j++)
            ((unsigned char *)&g_langs[i])[j] = 0;
    }
    g_langs[0].name = N("Plain Text");
    g_nlangs = 1;                       /* LANG_NONE */

    /* Indices are handed out afresh, so a cache keyed on one is now stale. */
    g_rules_lang = -1;
    g_nrules     = 0;

    for (i = 0; note_builtin_syntax[i]; i++)
        note_syntax_add(ar, note_syntax_widen(note_builtin_syntax[i]));
}

int note_lang_from_path(const nchar *path)
{
    const nchar *base = note_basename(path);
    const nchar *dot = 0, *p;
    int len, i;

    for (p = base; *p; p++) if (*p == (nchar)'.') dot = p;
    if (!dot || !dot[1]) return LANG_NONE;

    dot++;
    len = n_len(dot);

    /* Scan backwards: built-ins are registered first, then packs, then the
     * user's own files, so the last definition to claim an extension is the
     * most specific one and should win. */
    for (i = g_nlangs - 1; i >= 1; i--)
        if (note_word_in_list(g_langs[i].exts, dot, len, 1)) return i;

    return LANG_NONE;
}

/* ==========================================================================
 * Lexer
 * ========================================================================== */

static int is_ident(nchar c)
{
    return (c >= (nchar)'a' && c <= (nchar)'z') ||
           (c >= (nchar)'A' && c <= (nchar)'Z') ||
           (c >= (nchar)'0' && c <= (nchar)'9') ||
            c == (nchar)'_';
}

static int is_ident_start(nchar c)
{
    return is_ident(c) && !(c >= (nchar)'0' && c <= (nchar)'9');
}

static int is_digit(nchar c)
{
    return c >= (nchar)'0' && c <= (nchar)'9';
}

/* A line ends at either character.  The core never sees a single convention:
 * files arrive as CRLF, LF or CR, and a native text control may hand back a
 * lone CR for a paragraph break.  Treating only LF as a terminator would let
 * a line comment swallow the rest of the document. */
static int is_eol(nchar c)
{
    return c == (nchar)'\n' || c == (nchar)'\r';
}

/* Does text[i..] start with s? */
static int starts(const nchar *text, int len, int i, const nchar *s)
{
    int k = 0;
    if (!s || !*s) return 0;
    while (s[k]) {
        if (i + k >= len || text[i + k] != s[k]) return 0;
        k++;
    }
    return k;
}

static int chr_in(const nchar *set, nchar c)
{
    if (!set) return 0;
    while (*set) if (*set++ == c) return 1;
    return 0;
}

/* --------------------------------------------------------------------------
 * Compiled rules for one language.
 *
 * Compiling every language's rules would cost megabytes for a registry of a
 * hundred-odd languages, and all but one of them is for a file nobody has
 * open.  So the cache holds exactly the language being displayed, and is
 * rebuilt when that changes.  A rule whose expression will not compile — too
 * long for the program, or simply malformed — is dropped rather than being
 * allowed to break the whole language.
 * -------------------------------------------------------------------------- */

typedef struct {
    note_regex    re;
    unsigned char kind;
    int           pos;      /* where the next search for this rule starts  */
    int           ms, me;   /* its pending match, or ms < 0 for none       */
} compiled_rule;

static compiled_rule g_rules[NOTE_MAX_RULES];

static void compile_rules(const note_lang *L, int lang)
{
    int i;

    if (g_rules_lang == lang) return;

    g_nrules = 0;
    for (i = 0; i < (int)L->nrules; i++) {
        unsigned flags = (L->flags & SYN_NOCASE) ? NRE_ICASE : 0;
        if (note_regex_compile(&g_rules[g_nrules].re, L->rules[i].pattern, flags)) {
            g_rules[g_nrules].kind = L->rules[i].kind;
            g_nrules++;
        }
    }
    g_rules_lang = lang;
}

/* Rules are searched once over the whole run, then consulted position by
 * position as the scanner walks it, so each expression is applied linearly
 * rather than re-run at every character. */
static void rules_begin(const nchar *text, int len, int from)
{
    int i;
    for (i = 0; i < g_nrules; i++) {
        g_rules[i].pos = from;
        g_rules[i].ms  = -1;
        if (note_regex_search(&g_rules[i].re, text, len, from,
                              &g_rules[i].ms, &g_rules[i].me, 0, 0)) {
            g_rules[i].pos = (g_rules[i].me > g_rules[i].ms)
                           ? g_rules[i].me : g_rules[i].ms + 1;
        } else {
            g_rules[i].ms = -1;
        }
    }
}

/* The rule matching exactly at `at`, if any: earlier rules win ties, which is
 * the order the definition file lists them in. */
static int rules_at(const nchar *text, int len, int at, int *out_len)
{
    int i, best = -1;

    for (i = 0; i < g_nrules; i++) {
        /* Advance past matches the scanner has already walked over — they
         * fell inside a comment, a string or another rule's span. */
        while (g_rules[i].ms >= 0 && g_rules[i].ms < at) {
            if (!note_regex_search(&g_rules[i].re, text, len, g_rules[i].pos,
                                   &g_rules[i].ms, &g_rules[i].me, 0, 0)) {
                g_rules[i].ms = -1;
                break;
            }
            g_rules[i].pos = (g_rules[i].me > g_rules[i].ms)
                           ? g_rules[i].me : g_rules[i].ms + 1;
        }
        if (g_rules[i].ms == at && g_rules[i].me > at && best < 0) best = i;
    }

    if (best < 0) return -1;
    *out_len = g_rules[best].me - at;
    return (int)g_rules[best].kind;
}

/* Does text[i..] start with s?  Declared here because the safe-start scan
 * below needs it and it is defined with the lexer further down. */
static int starts(const nchar *text, int len, int i, const nchar *s);

static int line_start_at(const nchar *text, int at)
{
    while (at > 0 && text[at - 1] != (nchar)'\n' && text[at - 1] != (nchar)'\r')
        at--;
    return at;
}

int note_syntax_safe_start(int lang, const nchar *text, int len,
                           int at, int window)
{
    const note_lang *L = note_lang_get(lang);
    int floor_at, i, last_open = -1, last_close = -1;

    if (at <= 0 || !text) return 0;
    if (at > len) at = len;

    floor_at = at - (window > 0 ? window : 0);
    if (floor_at < 0) floor_at = 0;

    /* Without block comments nothing survives a line break -- line comments
     * stop at one and an unterminated quote is cut off at one -- so the start
     * of the line is always safe, however large the file. */
    if (lang <= LANG_NONE || !L->block_open || !L->block_close)
        return line_start_at(text, at);

    /* With them, find the last delimiter before `at`.  If an opener came last
     * we are inside a comment and must begin at it; if a closer came last we
     * are outside one and the line after it is safe. */
    for (i = floor_at; i < at; i++) {
        if (starts(text, len, i, L->block_open))  last_open  = i;
        if (starts(text, len, i, L->block_close)) last_close = i;
    }

    if (last_open >= 0 && last_open > last_close) return last_open;

    if (last_close >= 0) {
        /* Everything after the closing delimiter is outside a comment.  Take
         * the latest safe point, not the earliest: the start of the target's
         * own line if that line begins after the comment ended, otherwise the
         * position just past the delimiter -- the target's line start would
         * otherwise fall inside the comment we just closed. */
        int past = last_close + n_len(L->block_close);
        int ls   = line_start_at(text, at);
        return ls >= past ? ls : past;
    }

    /* Nothing either way within the window.  The floor is then as good a
     * guess as we can make cheaply; from the very top it is also correct. */
    return floor_at == 0 ? 0 : line_start_at(text, floor_at);
}

#define EMIT(s, l, k)                                    \
    do {                                                 \
        if (n >= max) return n;                          \
        if ((l) > 0) {                                   \
            out[n].start = base + (s);                   \
            out[n].len   = (l);                          \
            out[n].kind  = (unsigned char)(k);           \
            n++;                                         \
        }                                                \
    } while (0)

int note_tokenize(int lang, const nchar *text, int len, int base,
                  note_span *out, int max)
{
    const note_lang *L = note_lang_get(lang);
    int i = 0, n = 0;
    int line_start = 1;
    int nocase;

    if (lang <= LANG_NONE || !L) return 0;
    nocase = (L->flags & SYN_NOCASE) ? 1 : 0;

    compile_rules(L, lang);
    rules_begin(text, len, 0);

    while (i < len) {
        nchar c = text[i];
        int start = i;

        if (is_eol(c)) {
            /* CRLF is one break, not two. */
            if (c == (nchar)'\r' && i + 1 < len && text[i + 1] == (nchar)'\n') i++;
            line_start = 1;
            i++;
            continue;
        }
        if (c == (nchar)' ' || c == (nchar)'\t') { i++; continue; }

        /* Whole-line constructs, only at the first non-blank of a line. */
        if (line_start && c == (nchar)'#' &&
            (L->flags & (SYN_PREPROC | SYN_HEADINGS))) {
            int kind = (L->flags & SYN_PREPROC) ? TOK_PREPROC : TOK_KEYWORD;
            while (i < len && !is_eol(text[i])) i++;
            EMIT(start, i - start, kind);
            line_start = 1;
            continue;
        }
        line_start = 0;

        /* Comments. */
        if (starts(text, len, i, L->line_comment)) {
            while (i < len && !is_eol(text[i])) i++;
            EMIT(start, i - start, TOK_COMMENT);
            continue;
        }
        if (starts(text, len, i, L->block_open)) {
            int k;
            i += n_len(L->block_open);
            while (i < len) {
                k = starts(text, len, i, L->block_close);
                if (k) { i += k; break; }
                i++;
            }
            EMIT(start, i - start, TOK_COMMENT);
            continue;
        }

        /* Strings. */
        if (chr_in(L->quotes, c)) {
            nchar q = c;
            i++;
            while (i < len) {
                if (text[i] == (nchar)'\\' && i + 1 < len) { i += 2; continue; }
                if (text[i] == q) { i++; break; }
                /* An unterminated quote must not run past its line, or one
                 * stray apostrophe would colour the rest of the file. */
                if (is_eol(text[i]) && q != (nchar)'`') break;
                i++;
            }
            EMIT(start, i - start, TOK_STRING);
            continue;
        }

        /* Pattern rules.  Reached only outside comments and strings, which
         * consume their whole span, so those keep precedence over any rule. */
        if (g_nrules) {
            int rlen = 0;
            int kind = rules_at(text, len, i, &rlen);
            if (kind >= 0) {
                i += rlen;
                EMIT(start, rlen, kind);
                continue;
            }
        }

        /* Markup tags: <tag, </tag, <?xml, and the closing >. */
        if ((L->flags & SYN_TAGS) && c == (nchar)'<') {
            i++;
            if (i < len && (text[i] == (nchar)'/' || text[i] == (nchar)'!' ||
                            text[i] == (nchar)'?')) i++;
            while (i < len && is_ident(text[i])) i++;
            EMIT(start, i - start, TOK_KEYWORD);
            continue;
        }
        if ((L->flags & SYN_TAGS) && (c == (nchar)'>' || c == (nchar)'/')) {
            i++;
            EMIT(start, i - start, TOK_KEYWORD);
            continue;
        }

        /* Numbers. */
        if (is_digit(c)) {
            i++;
            while (i < len && (is_ident(text[i]) || text[i] == (nchar)'.')) i++;
            EMIT(start, i - start, TOK_NUMBER);
            continue;
        }

        /* Words. */
        if (is_ident_start(c)) {
            int wlen;
            while (i < len && is_ident(text[i])) i++;
            wlen = i - start;
            if (note_word_in_list(L->keywords, text + start, wlen, nocase))
                EMIT(start, wlen, TOK_KEYWORD);
            else if (note_word_in_list(L->types, text + start, wlen, nocase))
                EMIT(start, wlen, TOK_TYPE);
            continue;
        }

        i++;
    }

    return n;
}
