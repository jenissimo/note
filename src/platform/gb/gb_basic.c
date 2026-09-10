/* gb_basic.c -- tokeniser, detokeniser and integer evaluator.  See gb_basic.h.
 *
 * No CRT: the two or three string helpers it wants are four lines each, and
 * pulling in SDCC's would cost more ROM than writing them.
 */

#include "gb_basic.h"

/* ==========================================================================
 * The keyword table.
 *
 * Longest first within a shared prefix, so a linear scan matching greedily
 * cannot take TO out of the front of a variable called TOTAL... which it would
 * anyway: see match_keyword, where the check that a keyword is not the start
 * of a longer identifier actually lives.
 * ========================================================================== */

typedef struct { const char *word; unsigned char token; } keyword;

static const keyword kWords[] = {
    { "RETURN", T_RETURN }, { "GOSUB", T_GOSUB }, { "PRINT", T_PRINT },
    { "INPUT",  T_INPUT  }, { "PAUSE", T_PAUSE }, { "STEP",  T_STEP  },
    { "NEXT",   T_NEXT   }, { "THEN",  T_THEN  }, { "GOTO",  T_GOTO  },
    { "STOP",   T_STOP   }, { "LET",   T_LET   }, { "REM",   T_REM   },
    { "END",    T_END    }, { "CLS",   T_CLS   }, { "FOR",   T_FOR   },
    { "AND",    T_AND    }, { "NOT",   T_NOT   }, { "MOD",   T_MOD   },
    { "RND",    T_RND    }, { "ABS",   T_ABS   }, { "SGN",   T_SGN   },
    { "JOY",    T_JOY    }, { "IF",    T_IF    }, { "TO",    T_TO    },
    { "OR",     T_OR     },
    { 0, 0 }
};

static char up(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static int is_alpha(char c)
{
    c = up(c);
    return c >= 'A' && c <= 'Z';
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int is_ident(char c)
{
    return is_alpha(c) || is_digit(c) || c == '_';
}

/* ==========================================================================
 * Tokenising
 * ========================================================================== */

/* Returns the token, and advances *at, when text at *at is a keyword that is
 * not merely the front of a longer name.  TO matches in "GOTO 10" only because
 * GOTO was tried first and won; it does not match in "TOTAL" because the
 * character after it continues an identifier. */
static unsigned char match_keyword(const char *s, int *at)
{
    int i, k;

    for (i = 0; kWords[i].word; i++) {
        const char *w = kWords[i].word;
        for (k = 0; w[k]; k++)
            if (up(s[*at + k]) != w[k]) break;
        if (w[k]) continue;                       /* ran out of match */

        /* A keyword followed by more identifier is a name, not a keyword. */
        if (is_ident(s[*at + k])) continue;

        *at += k;
        return kWords[i].token;
    }
    return 0;
}

int bas_tokenise(const char *text, unsigned char *out, int cap, bas_int *number)
{
    int at = 0, n = 0;

    if (number) *number = -1;

    while (text[at] == ' ') at++;

    /* A leading number is the line number -- but only if a statement follows
     * it.  "10 PRINT" is line ten; "1+2*3" is an expression whose first term
     * happens to be a number, and eating that 1 would quietly change the sum.
     * The test is what comes after the digits: a letter starts a statement,
     * an operator continues an expression, and nothing at all is a bare
     * expression rather than an empty line.
     *
     * That still leaves "7 MOD 3", which reads as line 7 by this rule and as
     * an expression to anyone typing it.  Both readings are defensible, so the
     * caller decides: passing a null `number` means "this is an expression",
     * and nothing is stripped.  The editor passes an address, the evaluator
     * does not. */
    if (number && is_digit(text[at])) {
        int look = at;
        bas_int v = 0;

        while (is_digit(text[look])) look++;
        while (text[look] == ' ') look++;

        if (is_alpha(text[look])) {
            while (is_digit(text[at])) v = (bas_int)(v * 10 + (text[at++] - '0'));
            if (number) *number = v;
            while (text[at] == ' ') at++;
        }
    }

    while (text[at]) {
        char c = text[at];

        if (c == ' ') { at++; continue; }

        if (n >= cap) return -1;

        /* A string literal is copied through with its quotes: the evaluator
         * needs to know where it ends, and the detokeniser needs to put it
         * back exactly as typed. */
        if (c == '"') {
            out[n++] = (unsigned char)c;
            at++;
            while (text[at] && text[at] != '"') {
                if (n >= cap) return -1;
                out[n++] = (unsigned char)text[at++];
            }
            if (text[at] == '"') {
                if (n >= cap) return -1;
                out[n++] = (unsigned char)text[at++];
            }
            continue;
        }

        if (is_digit(c)) {
            /* Two bytes whatever the literal looked like, which is where most
             * of the saving over storing source text comes from. */
            long v = 0;
            while (is_digit(text[at])) {
                v = v * 10 + (text[at] - '0');
                if (v > 32767L) v = 32767L;       /* clamp; range is the VM's */
                at++;
            }
            if (n + 3 > cap) return -1;
            out[n++] = T_NUM;
            out[n++] = (unsigned char)(v & 0xFF);
            out[n++] = (unsigned char)((v >> 8) & 0xFF);
            continue;
        }

        /* The two-character relational operators, before the single ones. */
        if (c == '<' && text[at + 1] == '=') { out[n++] = T_LE; at += 2; continue; }
        if (c == '>' && text[at + 1] == '=') { out[n++] = T_GE; at += 2; continue; }
        if (c == '<' && text[at + 1] == '>') { out[n++] = T_NE; at += 2; continue; }

        if (is_alpha(c)) {
            unsigned char t = match_keyword(text, &at);
            if (t) {
                out[n++] = t;
                /* REM swallows the rest of the line untouched: it is a comment,
                 * and tokenising a comment would change what it says. */
                if (t == T_REM) {
                    while (text[at]) {
                        if (n >= cap) return -1;
                        out[n++] = (unsigned char)text[at++];
                    }
                }
                continue;
            }
            /* An identifier, upper-cased so that a and A are one variable. */
            while (is_ident(text[at])) {
                if (n >= cap) return -1;
                out[n++] = (unsigned char)up(text[at++]);
            }
            continue;
        }

        out[n++] = (unsigned char)c;
        at++;
    }
    return n;
}

/* ==========================================================================
 * Detokenising
 * ========================================================================== */

static int put(char *out, int cap, int n, char c)
{
    if (n < cap - 1) out[n] = c;
    return n + 1;
}

static int put_str(char *out, int cap, int n, const char *s)
{
    while (*s) n = put(out, cap, n, *s++);
    return n;
}

static int put_num(char *out, int cap, int n, bas_int v)
{
    char tmp[7];
    int  k = 0;
    long x = v;                       /* so -32768 has somewhere to go */

    if (x < 0) { n = put(out, cap, n, '-'); x = -x; }
    do { tmp[k++] = (char)('0' + (x % 10)); x /= 10; } while (x);
    while (k) n = put(out, cap, n, tmp[--k]);
    return n;
}

static const char *word_for(unsigned char t)
{
    int i;
    for (i = 0; kWords[i].word; i++)
        if (kWords[i].token == t) return kWords[i].word;
    if (t == T_LE) return "<=";
    if (t == T_GE) return ">=";
    if (t == T_NE) return "<>";
    return "?";
}

int bas_detokenise(const unsigned char *tok, int len, char *out, int cap)
{
    int i = 0, n = 0;
    int prev_word = 0;

    while (i < len) {
        unsigned char t = tok[i];

        if (t == T_NUM) {
            bas_int v = (bas_int)((unsigned short)tok[i + 1] |
                                  ((unsigned short)tok[i + 2] << 8));
            if (prev_word) n = put(out, cap, n, ' ');
            n = put_num(out, cap, n, v);
            prev_word = 0;
            i += 3;
            continue;
        }

        /* The relational operators are keywords only in the storage sense.
         * They are punctuation to a reader, so they get no spaces: A=B<>C is
         * how it was typed and how it should come back. */
        if (t == T_LE || t == T_GE || t == T_NE) {
            n = put_str(out, cap, n, word_for(t));
            prev_word = 0;
            i++;
            continue;
        }

        if (t >= 0x80) {
            /* Keywords get a space in front of them unless they open the line,
             * and one after, so LIST reads like something a person typed
             * rather than like the bytes it is stored as. */
            if (n) n = put(out, cap, n, ' ');
            n = put_str(out, cap, n, word_for(t));
            i++;
            if (t == T_REM) {
                /* The comment is stored exactly as typed, space and all, so
                 * adding one here would grow it by a character per LIST. */
                while (i < len) n = put(out, cap, n, (char)tok[i++]);
                break;
            }
            prev_word = 1;
            continue;
        }

        if (t == '"') {
            if (prev_word) n = put(out, cap, n, ' ');
            n = put(out, cap, n, '"');
            i++;
            while (i < len && tok[i] != '"') n = put(out, cap, n, (char)tok[i++]);
            if (i < len) { n = put(out, cap, n, '"'); i++; }
            prev_word = 0;
            continue;
        }

        if (prev_word && is_ident((char)t)) n = put(out, cap, n, ' ');
        n = put(out, cap, n, (char)t);
        prev_word = 0;
        i++;
    }

    if (cap > 0) out[(n < cap - 1) ? n : cap - 1] = 0;
    return n;
}

/* ==========================================================================
 * Evaluating
 *
 * Recursive descent, one function per precedence level, which on this machine
 * is affordable because the levels are shallow and every frame is a few bytes.
 * Everything is a 16-bit signed integer; there is no other type.
 * ========================================================================== */

static bas_int expr_or(bas_vm *vm, const unsigned char *t, int len, int *at);

static bas_int num_at(const unsigned char *t, int *at)
{
    bas_int v = (bas_int)((unsigned short)t[*at + 1] |
                          ((unsigned short)t[*at + 2] << 8));
    *at += 3;
    return v;
}

/* A bare A..Z that is not the start of a longer name.  Longer names are legal
 * to type and are simply not variables yet; they read as zero rather than as a
 * syntax error, which is what makes a half-typed program still LIST. */
static bas_int var_at(bas_vm *vm, const unsigned char *t, int len, int *at)
{
    int start = *at;
    char c = up((char)t[start]);

    while (*at < len && is_ident((char)t[*at])) (*at)++;
    if (*at - start == 1 && c >= 'A' && c <= 'Z') return vm->var[c - 'A'];
    return 0;
}

static bas_int primary(bas_vm *vm, const unsigned char *t, int len, int *at)
{
    unsigned char c;

    if (*at >= len) { vm->err = BAS_SYNTAX; return 0; }
    c = t[*at];

    if (c == T_NUM) return num_at(t, at);

    if (c == '(') {
        bas_int v;
        (*at)++;
        v = expr_or(vm, t, len, at);
        if (*at < len && t[*at] == ')') (*at)++;
        else vm->err = BAS_SYNTAX;
        return v;
    }

    if (c == '-') { (*at)++; return (bas_int)(-primary(vm, t, len, at)); }
    if (c == '+') { (*at)++; return primary(vm, t, len, at); }

    if (c == T_NOT) { (*at)++; return (bas_int)(primary(vm, t, len, at) ? 0 : 1); }

    if (c == T_RND || c == T_ABS || c == T_SGN || c == T_JOY) {
        bas_int v = 0;
        (*at)++;
        if (*at < len && t[*at] == '(') {
            (*at)++;
            if (*at < len && t[*at] != ')') v = expr_or(vm, t, len, at);
            if (*at < len && t[*at] == ')') (*at)++;
            else vm->err = BAS_SYNTAX;
        }
        switch (c) {
        case T_ABS: return (bas_int)(v < 0 ? -v : v);
        case T_SGN: return (bas_int)(v > 0 ? 1 : (v < 0 ? -1 : 0));
        case T_JOY: return vm->joy ? vm->joy() : 0;
        default: {
            /* A linear congruential step, kept in sixteen bits.  Not a good
             * generator; a good one is not what a BASIC game needs. */
            static unsigned short seed = 0x1234;
            seed = (unsigned short)(seed * 25173u + 13849u);
            if (v <= 0) return (bas_int)(seed & 0x7FFF);
            return (bas_int)((seed & 0x7FFF) % (unsigned short)v);
        }
        }
    }

    if (is_alpha((char)c)) return var_at(vm, t, len, at);

    vm->err = BAS_SYNTAX;
    return 0;
}

static bas_int expr_mul(bas_vm *vm, const unsigned char *t, int len, int *at)
{
    bas_int v = primary(vm, t, len, at);

    while (*at < len && !vm->err) {
        unsigned char op = t[*at];
        bas_int r;

        if (op != '*' && op != '/' && op != T_MOD) break;
        (*at)++;
        r = primary(vm, t, len, at);

        if ((op == '/' || op == T_MOD) && r == 0) { vm->err = BAS_DIV_ZERO; return 0; }
        if (op == '*')      v = (bas_int)(v * r);
        else if (op == '/') v = (bas_int)(v / r);
        else                v = (bas_int)(v % r);
    }
    return v;
}

static bas_int expr_add(bas_vm *vm, const unsigned char *t, int len, int *at)
{
    bas_int v = expr_mul(vm, t, len, at);

    while (*at < len && !vm->err) {
        unsigned char op = t[*at];
        if (op != '+' && op != '-') break;
        (*at)++;
        if (op == '+') v = (bas_int)(v + expr_mul(vm, t, len, at));
        else           v = (bas_int)(v - expr_mul(vm, t, len, at));
    }
    return v;
}

static bas_int expr_cmp(bas_vm *vm, const unsigned char *t, int len, int *at)
{
    bas_int v = expr_add(vm, t, len, at);

    while (*at < len && !vm->err) {
        unsigned char op = t[*at];
        bas_int r;

        if (op != '=' && op != '<' && op != '>' &&
            op != T_LE && op != T_GE && op != T_NE) break;
        (*at)++;
        r = expr_add(vm, t, len, at);

        /* True is 1, false is 0 -- not -1.  The Spectrum's -1 exists to make
         * AND arithmetic work; here AND is logical, so 1 is the honest answer
         * and IF X=1 does what it looks like. */
        switch (op) {
        case '=':   v = (bas_int)(v == r); break;
        case '<':   v = (bas_int)(v <  r); break;
        case '>':   v = (bas_int)(v >  r); break;
        case T_LE:  v = (bas_int)(v <= r); break;
        case T_GE:  v = (bas_int)(v >= r); break;
        default:    v = (bas_int)(v != r); break;
        }
    }
    return v;
}

static bas_int expr_and(bas_vm *vm, const unsigned char *t, int len, int *at)
{
    bas_int v = expr_cmp(vm, t, len, at);

    while (*at < len && t[*at] == T_AND && !vm->err) {
        bas_int r;
        (*at)++;
        r = expr_cmp(vm, t, len, at);
        v = (bas_int)(v && r);
    }
    return v;
}

static bas_int expr_or(bas_vm *vm, const unsigned char *t, int len, int *at)
{
    bas_int v = expr_and(vm, t, len, at);

    while (*at < len && t[*at] == T_OR && !vm->err) {
        bas_int r;
        (*at)++;
        r = expr_and(vm, t, len, at);
        v = (bas_int)(v || r);
    }
    return v;
}

bas_int bas_eval(bas_vm *vm, const unsigned char *tok, int len, int *at)
{
    return expr_or(vm, tok, len, at);
}

/* ==========================================================================
 * Program storage
 * ========================================================================== */

void bas_prog_init(bas_vm *vm, unsigned char *storage, int cap)
{
    vm->prog = storage;
    vm->prog_cap = cap;
    vm->prog_len = 0;
}

static bas_int line_number_at(const bas_vm *vm, int off)
{
    return (bas_int)((unsigned short)vm->prog[off] |
                     ((unsigned short)vm->prog[off + 1] << 8));
}

int bas_prog_find(bas_vm *vm, bas_int number)
{
    int off = 0;

    while (off < vm->prog_len) {
        if (line_number_at(vm, off) == number) return off;
        off += 3 + vm->prog[off + 2];
    }
    return -1;
}

int bas_prog_set(bas_vm *vm, bas_int number, const unsigned char *tok, int len)
{
    int off = 0, old = -1, oldsz = 0, need, i;

    /* Ascending order, so this is the first line at or past the number. */
    while (off < vm->prog_len) {
        bas_int n = line_number_at(vm, off);
        if (n == number) { old = off; oldsz = 3 + vm->prog[off + 2]; break; }
        if (n > number) break;
        off += 3 + vm->prog[off + 2];
    }
    if (old < 0) old = off;

    need = len ? 3 + len : 0;
    if (len > 255) return 0;
    if (vm->prog_len - oldsz + need > vm->prog_cap) return 0;

    /* One move for the tail, whether this is an insert, a replace or a
     * delete: the three differ only in the sizes. */
    if (need != oldsz) {
        int tail = vm->prog_len - (old + oldsz);
        if (need > oldsz) {
            for (i = tail - 1; i >= 0; i--)
                vm->prog[old + need + i] = vm->prog[old + oldsz + i];
        } else {
            for (i = 0; i < tail; i++)
                vm->prog[old + need + i] = vm->prog[old + oldsz + i];
        }
        vm->prog_len += need - oldsz;
    }

    if (need) {
        vm->prog[old]     = (unsigned char)(number & 0xFF);
        vm->prog[old + 1] = (unsigned char)((number >> 8) & 0xFF);
        vm->prog[old + 2] = (unsigned char)len;
        for (i = 0; i < len; i++) vm->prog[old + 3 + i] = tok[i];
    }
    return 1;
}

/* ==========================================================================
 * Running one line
 *
 * Immediate mode only for now: enough that PRINT and LET are useful in the
 * editor before the program runner exists.  A statement that would need to
 * change the program counter reports BAS_STOPPED rather than pretending.
 * ========================================================================== */

static void emit(bas_vm *vm, char c)
{
    if (vm->out) vm->out(c);
}

static void emit_num(bas_vm *vm, bas_int v)
{
    char buf[8];
    int  n = put_num(buf, (int)sizeof(buf), 0, v), i;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    buf[n] = 0;
    for (i = 0; i < n; i++) emit(vm, buf[i]);
}

static void do_print(bas_vm *vm, const unsigned char *t, int len, int *at)
{
    int newline = 1;

    while (*at < len && !vm->err) {
        if (t[*at] == '"') {
            (*at)++;
            while (*at < len && t[*at] != '"') emit(vm, (char)t[(*at)++]);
            if (*at < len) (*at)++;
        } else if (t[*at] == ';') {
            (*at)++;
            newline = 0;
            continue;
        } else if (t[*at] == ',') {
            (*at)++;
            emit(vm, ' ');
            continue;
        } else {
            emit_num(vm, bas_eval(vm, t, len, at));
        }
        newline = 1;
        if (*at < len && (t[*at] == ';' || t[*at] == ',')) continue;
        break;
    }
    if (newline) emit(vm, '\n');
}

int bas_exec_line(bas_vm *vm, const unsigned char *tok, int len)
{
    int at = 0;

    vm->err = BAS_OK;
    if (!len) return BAS_OK;

    switch (tok[0]) {
    case T_REM:
        return BAS_OK;

    case T_END:
    case T_STOP:
        vm->err = BAS_STOPPED;
        return vm->err;

    case T_PRINT:
        at = 1;
        do_print(vm, tok, len, &at);
        return vm->err;

    case T_LET:
        at = 1;
        break;

    default:
        /* Assignment without LET, which is how everyone actually writes it. */
        at = 0;
        break;
    }

    if (at < len && is_alpha((char)tok[at])) {
        char name = up((char)tok[at]);
        int  after = at;

        while (after < len && is_ident((char)tok[after])) after++;
        if (after - at == 1 && after < len && tok[after] == '=') {
            bas_int v;
            after++;
            v = bas_eval(vm, tok, len, &after);
            if (!vm->err) vm->var[name - 'A'] = v;
            return vm->err;
        }
    }

    vm->err = BAS_SYNTAX;
    return vm->err;
}
