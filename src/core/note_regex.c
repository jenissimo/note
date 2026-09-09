/* note_regex.c — compiler and Pike VM for note_regex.h.
 *
 * The compiler is a plain recursive descent over the pattern that emits
 * instructions straight into re->prog.  Quantifiers are applied after the
 * atom they follow has been emitted, by inserting a split in front of the
 * atom's code or by copying that code, so no separate syntax tree is needed
 * and no memory is allocated anywhere.
 *
 * The matcher is a Pike VM.  Every thread that is alive at a text position is
 * held in one list; a position is stepped once and each program counter is
 * entered at most once per position, so the work is bounded by
 * (length of text) * (length of program) and a pattern like (a+)+b can never
 * explode.  Threads carry their capture slots with them, and a thread that
 * reaches NRE_OP_MATCH kills every lower-priority thread behind it, which is
 * what makes the result leftmost-first — the same match a backtracker would
 * have found, only without the backtracking.
 */

#include "note_regex.h"

/* ==========================================================================
 * Instruction set
 * ========================================================================== */

enum {
    NRE_OP_CHAR = 0,  /* x = code unit                                     */
    NRE_OP_ANY,       /* '.'                                               */
    NRE_OP_CLASS,     /* x = index into re->cls                            */
    NRE_OP_CAT,       /* x = NRE_CAT_* mask, from \d \D \w \W \s \S        */
    NRE_OP_SPLIT,     /* try x first, then y                               */
    NRE_OP_JMP,       /* continue at x                                     */
    NRE_OP_SAVE,      /* x = capture slot; record the position             */
    NRE_OP_BOL,       /* '^'                                               */
    NRE_OP_EOL,       /* '$'                                               */
    NRE_OP_WORDB,     /* \b                                                */
    NRE_OP_NWORDB,    /* \B                                                */
    NRE_OP_MATCH
};

/* Category bits.  A set is the union of its bits, so [\D\s] reads naturally. */
enum {
    NRE_CAT_D  = 1,  NRE_CAT_ND = 2,
    NRE_CAT_W  = 4,  NRE_CAT_NW = 8,
    NRE_CAT_S  = 16, NRE_CAT_NS = 32
};

/* A branch target that has not been filled in yet.  It is never relocated. */
#define NRE_HOLE 0xFFFFu

/* ==========================================================================
 * Code units
 *
 * nchar is unsigned short on Windows and plain char elsewhere, where it may
 * be signed, so every comparison goes through nre_uc() and works on values,
 * never on the raw nchar.
 * ========================================================================== */

static unsigned nre_uc(nchar c)
{
#if NOTE_NCHAR_UTF16
    return (unsigned)c;
#else
    return (unsigned)(unsigned char)c;
#endif
}

/* n_len() from the core does the same thing, but keeping the module free of
 * link-time dependencies lets it be built and tested on its own. */
static int nre_len(const nchar *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int nre_is_digit(unsigned u)
{
    return u >= (unsigned)'0' && u <= (unsigned)'9';
}

/* A word character: ASCII alphanumerics and '_', plus anything outside ASCII.
 * Treating every high code unit as a word character is what a user editing
 * accented or CJK text expects from \b and \w, and it is the same answer for
 * a UTF-8 lead byte, a UTF-8 continuation byte and a UTF-16 code unit. */
static int nre_is_word(unsigned u)
{
    return (u >= (unsigned)'a' && u <= (unsigned)'z') ||
           (u >= (unsigned)'A' && u <= (unsigned)'Z') ||
           nre_is_digit(u) || u == (unsigned)'_' || u >= 0x80u;
}

static int nre_is_space(unsigned u)
{
    return u == (unsigned)' '  || u == (unsigned)'\t' || u == (unsigned)'\n' ||
           u == (unsigned)'\r' || u == (unsigned)'\v' || u == (unsigned)'\f';
}

static unsigned nre_lower(unsigned u)
{
    if (u >= (unsigned)'A' && u <= (unsigned)'Z') return u + 32u;
    return u;
}

static unsigned nre_upper(unsigned u)
{
    if (u >= (unsigned)'a' && u <= (unsigned)'z') return u - 32u;
    return u;
}

static int nre_cat_match(unsigned mask, unsigned u)
{
    if ((mask & NRE_CAT_D)  && nre_is_digit(u)) return 1;
    if ((mask & NRE_CAT_ND) && !nre_is_digit(u)) return 1;
    if ((mask & NRE_CAT_W)  && nre_is_word(u)) return 1;
    if ((mask & NRE_CAT_NW) && !nre_is_word(u)) return 1;
    if ((mask & NRE_CAT_S)  && nre_is_space(u)) return 1;
    if ((mask & NRE_CAT_NS) && !nre_is_space(u)) return 1;
    return 0;
}

/* ==========================================================================
 * Compiler
 * ========================================================================== */

typedef struct {
    note_regex  *re;
    const nchar *p;      /* the cursor into the pattern */
    int          ngroup; /* capturing groups opened so far */
    int          failed;
} nre_comp;

static int nre_fail(nre_comp *c, const nchar *msg)
{
    if (!c->failed) {
        c->failed = 1;
        c->re->err = msg;
    }
    return 0;
}

static int nre_emit(nre_comp *c, int op, unsigned x, unsigned y)
{
    note_regex_inst *in;
    if (c->re->nprog >= NOTE_REGEX_PROG)
        return nre_fail(c, N("pattern is too complex"));
    in = &c->re->prog[c->re->nprog++];
    in->op = (unsigned char)op;
    in->x  = (unsigned short)x;
    in->y  = (unsigned short)y;
    return 1;
}

/* Is this operand field a branch target that relocation must follow? */
static int nre_is_target(const note_regex_inst *in, int which)
{
    if (in->op == NRE_OP_JMP)   return which == 0;
    if (in->op == NRE_OP_SPLIT) return 1;
    return 0;
}

/* Opens a slot at `at` for a split, pushing the block that starts there up by
 * one and fixing the branch targets.
 *
 * A target that was exactly `at` is ambiguous, and which way it goes depends
 * on who is pointing: an instruction inside the block that loops back to the
 * block's first instruction has to follow that instruction up to at+1, while
 * an instruction outside — the enclosing alternation's split, say — means
 * "the start of this block", and the block now starts with the new split, so
 * it stays at `at`.  Getting this wrong lets a lazy star at the head of an
 * alternation branch jump into the other branch.  Holes waiting to be patched
 * are never relocated. */
static int nre_insert_split(nre_comp *c, int at)
{
    note_regex *re = c->re;
    int i;

    if (re->nprog >= NOTE_REGEX_PROG)
        return nre_fail(c, N("pattern is too complex"));

    for (i = re->nprog; i > at; i--) re->prog[i] = re->prog[i - 1];
    re->nprog++;

    for (i = 0; i < re->nprog; i++) {
        note_regex_inst *in = &re->prog[i];
        int lim = (i > at) ? at : at + 1;   /* relocate targets >= lim */
        if (nre_is_target(in, 0) && in->x != NRE_HOLE && (int)in->x >= lim)
            in->x = (unsigned short)(in->x + 1);
        if (nre_is_target(in, 1) && in->y != NRE_HOLE && (int)in->y >= lim)
            in->y = (unsigned short)(in->y + 1);
    }

    re->prog[at].op = NRE_OP_SPLIT;
    re->prog[at].x  = NRE_HOLE;
    re->prog[at].y  = NRE_HOLE;
    return 1;
}

/* Appends another copy of prog[from..to), moving any branch target that lands
 * inside the block along with it.  A finished atom never branches out of
 * itself, so nothing else needs fixing. */
static int nre_copy_block(nre_comp *c, int from, int to)
{
    note_regex *re = c->re;
    int i, delta;

    delta = re->nprog - from;
    if (re->nprog + (to - from) > NOTE_REGEX_PROG)
        return nre_fail(c, N("pattern is too complex"));

    for (i = from; i < to; i++) {
        note_regex_inst in = re->prog[i];
        if (nre_is_target(&in, 0) && (int)in.x >= from && (int)in.x <= to)
            in.x = (unsigned short)((int)in.x + delta);
        if (nre_is_target(&in, 1) && (int)in.y >= from && (int)in.y <= to)
            in.y = (unsigned short)((int)in.y + delta);
        re->prog[re->nprog++] = in;
    }
    return 1;
}

/* Fills in a split.  Greedy prefers the loop or the body, lazy prefers the
 * way out; that single swap is the whole of the '?' suffix. */
static void nre_set_split(note_regex *re, int at, int enter, int leave, int lazy)
{
    re->prog[at].x = (unsigned short)(lazy ? leave : enter);
    re->prog[at].y = (unsigned short)(lazy ? enter : leave);
}

/* Wraps the code already emitted at prog[start..nprog) in a repetition.
 * `max` < 0 means open ended. */
static int nre_repeat(nre_comp *c, int start, int min, int max, int lazy)
{
    note_regex *re = c->re;
    int patch[NOTE_REGEX_MAX_REPEAT + 1];
    int npatch = 0;
    int from, to, blen, opt, end, last, i;

    from = start;
    to   = re->nprog;
    blen = to - from;

    if (min == 0 && max == 0) {          /* X{0} — drop the atom entirely */
        re->nprog = start;
        return 1;
    }

    if (min == 0 && max < 0) {           /* X* */
        if (!nre_insert_split(c, start)) return 0;
        if (!nre_emit(c, NRE_OP_JMP, (unsigned)start, 0)) return 0;
        end = re->nprog;
        nre_set_split(re, start, start + 1, end, lazy);
        return 1;
    }

    if (min > 0) {
        for (i = 1; i < min; i++)
            if (!nre_copy_block(c, from, to)) return 0;
        last = from + (min - 1) * blen;
        if (max < 0) {                   /* X{m,} — loop on the last copy */
            if (!nre_emit(c, NRE_OP_SPLIT, NRE_HOLE, NRE_HOLE)) return 0;
            end = re->nprog;
            nre_set_split(re, end - 1, last, end, lazy);
            return 1;
        }
        opt = max - min;
    } else {                             /* X{0,n} — the atom itself is optional */
        if (!nre_insert_split(c, start)) return 0;
        patch[npatch++] = start;
        from = start + 1;
        to   = re->nprog;
        opt  = max - 1;
    }

    for (i = 0; i < opt; i++) {
        int sp = re->nprog;
        if (!nre_emit(c, NRE_OP_SPLIT, NRE_HOLE, NRE_HOLE)) return 0;
        patch[npatch++] = sp;
        if (!nre_copy_block(c, from, to)) return 0;
    }

    end = re->nprog;
    for (i = 0; i < npatch; i++)
        nre_set_split(re, patch[i], patch[i] + 1, end, lazy);
    return 1;
}

/* -------------------------------------------------------------------------- */

static int nre_alt(nre_comp *c);

/* One escape after the backslash.  Returns 1 and sets *out to a code unit, or
 * returns 2 and sets *cat to a category mask, or 0 on a bad escape. */
static int nre_escape(nre_comp *c, unsigned *out, unsigned *cat)
{
    unsigned u;

    if (!*c->p) { nre_fail(c, N("trailing backslash")); return 0; }
    u = nre_uc(*c->p++);

    switch (u) {
    case 'd': *cat = NRE_CAT_D;  return 2;
    case 'D': *cat = NRE_CAT_ND; return 2;
    case 'w': *cat = NRE_CAT_W;  return 2;
    case 'W': *cat = NRE_CAT_NW; return 2;
    case 's': *cat = NRE_CAT_S;  return 2;
    case 'S': *cat = NRE_CAT_NS; return 2;
    case 't': *out = 9;  return 1;
    case 'n': *out = 10; return 1;
    case 'v': *out = 11; return 1;
    case 'f': *out = 12; return 1;
    case 'r': *out = 13; return 1;
    case '0': *out = 0;  return 1;
    case 'a': *out = 7;  return 1;
    case 'e': *out = 27; return 1;
    default:  break;
    }

    /* A backslash before punctuation means that character, literally. */
    if ((u >= (unsigned)'a' && u <= (unsigned)'z') ||
        (u >= (unsigned)'A' && u <= (unsigned)'Z') ||
        nre_is_digit(u)) {
        nre_fail(c, N("unknown escape sequence"));
        return 0;
    }
    *out = u;
    return 1;
}

static int nre_add_range(nre_comp *c, unsigned lo, unsigned hi)
{
    note_regex *re = c->re;
    if (re->nrng >= NOTE_REGEX_RANGES)
        return nre_fail(c, N("too many ranges in a character class"));
    re->rng[re->nrng].lo = (unsigned short)lo;
    re->rng[re->nrng].hi = (unsigned short)hi;
    re->nrng++;
    return 1;
}

/* [...] — the cursor sits just past the '['. */
static int nre_class(nre_comp *c)
{
    note_regex *re = c->re;
    note_regex_class *k;
    int idx;

    if (re->ncls >= NOTE_REGEX_CLASSES)
        return nre_fail(c, N("too many character classes"));

    idx = re->ncls++;
    k = &re->cls[idx];
    k->first = (unsigned short)re->nrng;
    k->count = 0;
    k->neg   = 0;
    k->cats  = 0;

    if (nre_uc(*c->p) == (unsigned)'^') { k->neg = 1; c->p++; }
    /* A ']' straight after the '[' or the '^' is an ordinary character. */
    if (nre_uc(*c->p) == (unsigned)']') {
        if (!nre_add_range(c, (unsigned)']', (unsigned)']')) return 0;
        c->p++;
    }

    while (*c->p && nre_uc(*c->p) != (unsigned)']') {
        unsigned lo = 0, hi, cat = 0;
        int kind;

        if (nre_uc(*c->p) == (unsigned)'\\') {
            c->p++;
            kind = nre_escape(c, &lo, &cat);
            if (!kind) return 0;
            if (kind == 2) { k->cats |= (unsigned char)cat; continue; }
        } else {
            lo = nre_uc(*c->p++);
        }

        hi = lo;
        if (nre_uc(*c->p) == (unsigned)'-' && c->p[1] &&
            nre_uc(c->p[1]) != (unsigned)']') {
            c->p++;
            if (nre_uc(*c->p) == (unsigned)'\\') {
                c->p++;
                kind = nre_escape(c, &hi, &cat);
                if (!kind) return 0;
                if (kind == 2)
                    return nre_fail(c, N("a class shorthand cannot end a range"));
            } else {
                hi = nre_uc(*c->p++);
            }
            if (hi < lo) return nre_fail(c, N("reversed range in [...]"));
        }
        if (!nre_add_range(c, lo, hi)) return 0;
    }

    if (nre_uc(*c->p) != (unsigned)']')
        return nre_fail(c, N("unterminated [...]"));
    c->p++;

    k->count = (unsigned short)(re->nrng - (int)k->first);
    if (!k->count && !k->cats)
        return nre_fail(c, N("empty character class"));

    return nre_emit(c, NRE_OP_CLASS, (unsigned)idx, 0);
}

static int nre_atom(nre_comp *c)
{
    unsigned u, cat = 0;
    int kind;

    u = nre_uc(*c->p);

    if (u == (unsigned)'(') {
        int capture = 1, group = 0;
        c->p++;
        if (nre_uc(*c->p) == (unsigned)'?') {
            if (nre_uc(c->p[1]) != (unsigned)':')
                return nre_fail(c, N("only (?:...) is supported"));
            c->p += 2;
            capture = 0;
        }
        if (capture) {
            if (c->ngroup + 1 >= NOTE_REGEX_GROUPS)
                return nre_fail(c, N("too many capturing groups"));
            group = ++c->ngroup;
            if (!nre_emit(c, NRE_OP_SAVE, (unsigned)(group * 2), 0)) return 0;
        }
        if (!nre_alt(c)) return 0;
        if (nre_uc(*c->p) != (unsigned)')')
            return nre_fail(c, N("missing )"));
        c->p++;
        if (capture)
            return nre_emit(c, NRE_OP_SAVE, (unsigned)(group * 2 + 1), 0);
        return 1;
    }

    if (u == (unsigned)'[') { c->p++; return nre_class(c); }
    if (u == (unsigned)'.') { c->p++; return nre_emit(c, NRE_OP_ANY, 0, 0); }
    if (u == (unsigned)'^') { c->p++; return nre_emit(c, NRE_OP_BOL, 0, 0); }
    if (u == (unsigned)'$') { c->p++; return nre_emit(c, NRE_OP_EOL, 0, 0); }

    if (u == (unsigned)'*' || u == (unsigned)'+' || u == (unsigned)'?')
        return nre_fail(c, N("nothing to repeat"));

    if (u == (unsigned)'\\') {
        c->p++;
        if (nre_uc(*c->p) == (unsigned)'b') {
            c->p++;
            return nre_emit(c, NRE_OP_WORDB, 0, 0);
        }
        if (nre_uc(*c->p) == (unsigned)'B') {
            c->p++;
            return nre_emit(c, NRE_OP_NWORDB, 0, 0);
        }
        kind = nre_escape(c, &u, &cat);
        if (!kind) return 0;
        if (kind == 2) return nre_emit(c, NRE_OP_CAT, cat, 0);
        return nre_emit(c, NRE_OP_CHAR, u, 0);
    }

    c->p++;
    return nre_emit(c, NRE_OP_CHAR, u, 0);
}

/* Reads {m}, {m,} or {m,n}.  Returns 1 if one was there and consumed, 0 if
 * the '{' was not a quantifier at all, and -1 on a malformed one. */
static int nre_bounds(nre_comp *c, int *min, int *max)
{
    const nchar *p = c->p;
    int m = 0, n, digits = 0;

    if (nre_uc(*p) != (unsigned)'{' || !nre_is_digit(nre_uc(p[1]))) return 0;
    p++;

    while (nre_is_digit(nre_uc(*p))) {
        m = m * 10 + (int)(nre_uc(*p++) - (unsigned)'0');
        if (++digits > 3 || m > NOTE_REGEX_MAX_REPEAT) {
            nre_fail(c, N("repeat count is too large"));
            return -1;
        }
    }

    n = m;
    if (nre_uc(*p) == (unsigned)',') {
        p++;
        if (nre_uc(*p) == (unsigned)'}') {
            n = -1;                       /* {m,} */
        } else {
            n = 0;
            digits = 0;
            while (nre_is_digit(nre_uc(*p))) {
                n = n * 10 + (int)(nre_uc(*p++) - (unsigned)'0');
                if (++digits > 3 || n > NOTE_REGEX_MAX_REPEAT) {
                    nre_fail(c, N("repeat count is too large"));
                    return -1;
                }
            }
            if (!digits) { nre_fail(c, N("malformed {m,n}")); return -1; }
            if (n < m)   { nre_fail(c, N("{m,n} has n below m")); return -1; }
        }
    }

    if (nre_uc(*p) != (unsigned)'}') {
        nre_fail(c, N("unterminated {m,n}"));
        return -1;
    }

    c->p  = p + 1;
    *min  = m;
    *max  = n;
    return 1;
}

static int nre_piece(nre_comp *c)
{
    int start = c->re->nprog;
    int min, max, lazy, got;
    unsigned u;

    if (!nre_atom(c)) return 0;

    u = nre_uc(*c->p);
    if (u == (unsigned)'*')      { min = 0; max = -1; c->p++; }
    else if (u == (unsigned)'+') { min = 1; max = -1; c->p++; }
    else if (u == (unsigned)'?') { min = 0; max =  1; c->p++; }
    else {
        got = nre_bounds(c, &min, &max);
        if (got < 0) return 0;
        if (!got) return 1;                    /* no quantifier here */
    }

    lazy = 0;
    if (nre_uc(*c->p) == (unsigned)'?') { lazy = 1; c->p++; }

    if (!nre_repeat(c, start, min, max, lazy)) return 0;

    /* a** and friends are almost always a typo, and would only produce a
     * program that does the same work twice. */
    u = nre_uc(*c->p);
    if (u == (unsigned)'*' || u == (unsigned)'+' ||
        (u == (unsigned)'?') ||
        (u == (unsigned)'{' && nre_is_digit(nre_uc(c->p[1]))))
        return nre_fail(c, N("a quantifier cannot follow a quantifier"));

    return 1;
}

static int nre_concat(nre_comp *c)
{
    while (*c->p && nre_uc(*c->p) != (unsigned)'|' &&
                    nre_uc(*c->p) != (unsigned)')') {
        if (!nre_piece(c)) return 0;
    }
    return 1;
}

/* branch | branch | branch, folded left so the leftmost branch keeps the
 * highest priority. */
static int nre_alt(nre_comp *c)
{
    note_regex *re = c->re;
    int start = re->nprog;

    if (!nre_concat(c)) return 0;

    while (nre_uc(*c->p) == (unsigned)'|') {
        int jmp, rhs;
        c->p++;
        if (!nre_insert_split(c, start)) return 0;
        jmp = re->nprog;
        if (!nre_emit(c, NRE_OP_JMP, NRE_HOLE, 0)) return 0;
        rhs = re->nprog;
        re->prog[start].x = (unsigned short)(start + 1);
        re->prog[start].y = (unsigned short)rhs;
        if (!nre_concat(c)) return 0;
        re->prog[jmp].x = (unsigned short)re->nprog;
    }
    return 1;
}

/* A last look over the finished program.  Nothing should be able to leave a
 * hole behind, but this runs once per compile and the alternative — the
 * matcher stepping to instruction 0xFFFF — is not something a binary without
 * a CRT recovers from. */
static int nre_verify(nre_comp *c)
{
    note_regex *re = c->re;
    int i;

    for (i = 0; i < re->nprog; i++) {
        const note_regex_inst *in = &re->prog[i];
        if (nre_is_target(in, 0) && (int)in->x >= re->nprog)
            return nre_fail(c, N("internal error: unresolved branch"));
        if (nre_is_target(in, 1) && (int)in->y >= re->nprog)
            return nre_fail(c, N("internal error: unresolved branch"));
        if (in->op == NRE_OP_CLASS && (int)in->x >= re->ncls)
            return nre_fail(c, N("internal error: bad class"));
        if (in->op == NRE_OP_SAVE && (int)in->x >= NOTE_REGEX_SLOTS)
            return nre_fail(c, N("internal error: bad capture slot"));
    }
    return 1;
}

static int nre_compile(nre_comp *c)
{
    if (!c->p) return nre_fail(c, N("no pattern"));

    if (!nre_emit(c, NRE_OP_SAVE, 0, 0)) return 0;
    if (!nre_alt(c)) return 0;

    if (nre_uc(*c->p) == (unsigned)')') return nre_fail(c, N("unmatched )"));
    if (*c->p) return nre_fail(c, N("unexpected character in pattern"));

    if (!nre_emit(c, NRE_OP_SAVE, 1, 0)) return 0;
    if (!nre_emit(c, NRE_OP_MATCH, 0, 0)) return 0;

    return nre_verify(c);
}

int note_regex_compile(note_regex *re, const nchar *pattern, unsigned flags)
{
    nre_comp c;

    if (!re) return 0;

    re->nprog   = 0;
    re->ncls    = 0;
    re->nrng    = 0;
    re->ngroups = 0;
    re->flags   = flags;
    re->err     = N("");

    c.re     = re;
    c.p      = pattern;
    c.ngroup = 0;
    c.failed = 0;

    if (!nre_compile(&c) || c.failed) {
        /* Leave nothing runnable behind: a half-built program still has
         * unpatched branches in it, and note_regex_search must be safe to
         * call on a note_regex whose compile failed. */
        re->nprog   = 0;
        re->ncls    = 0;
        re->nrng    = 0;
        re->ngroups = 0;
        return 0;
    }

    re->ngroups = c.ngroup;
    return 1;
}

const nchar *note_regex_error(const note_regex *re)
{
    if (!re || !re->err) return N("");
    return re->err;
}

int note_regex_groups(const note_regex *re)
{
    return re ? re->ngroups : 0;
}

/* ==========================================================================
 * Pike VM
 * ========================================================================== */

typedef struct {
    int pc;
    int slots[NOTE_REGEX_SLOTS];
} nre_thread;

typedef struct {
    nre_thread    th[NOTE_REGEX_PROG];
    int           n;
    unsigned long gen;                 /* which round these marks belong to */
    unsigned long mark[NOTE_REGEX_PROG];
} nre_list;

/* The two thread lists are about 22 KB together, which is more than belongs
 * on the stack of a binary built without a CRT: that build turns stack probes
 * off (there is no __chkstk to call), so a frame several pages deep could step
 * over the guard page.  They live in BSS instead, which costs one search at a
 * time — fine for Find/Replace, which is driven from one thread.  Define
 * NOTE_REGEX_VM_ON_STACK to 1 to get a re-entrant matcher back. */
#ifndef NOTE_REGEX_VM_ON_STACK
  #define NOTE_REGEX_VM_ON_STACK 0
#endif

#if NOTE_REGEX_VM_ON_STACK
  #define NRE_LISTS nre_list la, lb
#else
  #define NRE_LISTS static nre_list la, lb
#endif

typedef struct {
    const note_regex *re;
    const nchar      *text;
    int               len;
    unsigned long     gen;
} nre_vm;

static int nre_class_match(const note_regex *re, int idx, unsigned u)
{
    const note_regex_class *k = &re->cls[idx];
    unsigned v[3];
    int nv = 1, i, j, hit = 0;

    v[0] = u;
    if (re->flags & NRE_ICASE) {
        unsigned lo = nre_lower(u), up = nre_upper(u);
        if (lo != u) v[nv++] = lo;
        if (up != u) v[nv++] = up;
    }

    for (i = 0; i < nv && !hit; i++) {
        if (k->cats && nre_cat_match((unsigned)k->cats, v[i])) { hit = 1; break; }
        for (j = 0; j < (int)k->count; j++) {
            const note_regex_range *r = &re->rng[(int)k->first + j];
            if (v[i] >= (unsigned)r->lo && v[i] <= (unsigned)r->hi) { hit = 1; break; }
        }
    }

    return k->neg ? !hit : hit;
}

static int nre_at_bol(const nre_vm *vm, int pos)
{
    if (pos == 0) return 1;
    if (!(vm->re->flags & NRE_MULTILINE)) return 0;
    return nre_uc(vm->text[pos - 1]) == (unsigned)'\n';
}

static int nre_at_eol(const nre_vm *vm, int pos)
{
    unsigned u;
    if (pos == vm->len) return 1;
    if (!(vm->re->flags & NRE_MULTILINE)) return 0;
    u = nre_uc(vm->text[pos]);
    if (u == (unsigned)'\n') return 1;
    /* A CRLF buffer, which is what a Windows edit control hands us. */
    if (u == (unsigned)'\r' &&
        (pos + 1 == vm->len || nre_uc(vm->text[pos + 1]) == (unsigned)'\n'))
        return 1;
    return 0;
}

static int nre_at_wordb(const nre_vm *vm, int pos)
{
    int before = pos > 0 && nre_is_word(nre_uc(vm->text[pos - 1]));
    int after  = pos < vm->len && nre_is_word(nre_uc(vm->text[pos]));
    return before != after;
}

/* Walks the epsilon closure of `pc` and puts every thread that must wait for
 * a character into `l`.  A pc is entered once per round, which is what bounds
 * the work and what makes an empty loop such as (a*)* terminate. */
static void nre_add(nre_vm *vm, nre_list *l, int pc, int pos, int *slots)
{
    const note_regex_inst *in;
    int save, i;

    if (l->mark[pc] == l->gen) return;
    l->mark[pc] = l->gen;

    in = &vm->re->prog[pc];
    switch (in->op) {
    case NRE_OP_JMP:
        nre_add(vm, l, (int)in->x, pos, slots);
        return;
    case NRE_OP_SPLIT:
        nre_add(vm, l, (int)in->x, pos, slots);
        nre_add(vm, l, (int)in->y, pos, slots);
        return;
    case NRE_OP_SAVE:
        save = slots[in->x];
        slots[in->x] = pos;
        nre_add(vm, l, pc + 1, pos, slots);
        slots[in->x] = save;
        return;
    case NRE_OP_BOL:
        if (nre_at_bol(vm, pos)) nre_add(vm, l, pc + 1, pos, slots);
        return;
    case NRE_OP_EOL:
        if (nre_at_eol(vm, pos)) nre_add(vm, l, pc + 1, pos, slots);
        return;
    case NRE_OP_WORDB:
        if (nre_at_wordb(vm, pos)) nre_add(vm, l, pc + 1, pos, slots);
        return;
    case NRE_OP_NWORDB:
        if (!nre_at_wordb(vm, pos)) nre_add(vm, l, pc + 1, pos, slots);
        return;
    default:
        break;
    }

    if (l->n >= NOTE_REGEX_PROG) return;      /* cannot happen: one per pc */
    l->th[l->n].pc = pc;
    for (i = 0; i < NOTE_REGEX_SLOTS; i++) l->th[l->n].slots[i] = slots[i];
    l->n++;
}

static int nre_step_char(const nre_vm *vm, const note_regex_inst *in, unsigned u)
{
    switch (in->op) {
    case NRE_OP_CHAR:
        if (u == (unsigned)in->x) return 1;
        if (vm->re->flags & NRE_ICASE)
            return nre_lower(u) == nre_lower((unsigned)in->x);
        return 0;
    case NRE_OP_ANY:
        if (vm->re->flags & NRE_DOTALL) return 1;
        return u != (unsigned)'\n' && u != (unsigned)'\r';
    case NRE_OP_CLASS:
        return nre_class_match(vm->re, (int)in->x, u);
    case NRE_OP_CAT:
        return nre_cat_match((unsigned)in->x, u);
    default:
        break;
    }
    return 0;
}

int note_regex_search(const note_regex *re, const nchar *text, int len,
                      int start, int *match_start, int *match_end,
                      int *groups, int ngroups)
{
    NRE_LISTS;
    nre_vm    vm;
    nre_list *cl, *nl, *sw;
    int  slots[NOTE_REGEX_SLOTS];
    int  best[NOTE_REGEX_SLOTS];
    int  pos, i, matched = 0;

    if (!re || !text || re->nprog <= 0) return 0;

    if (len < 0) len = nre_len(text);
    if (start < 0) start = 0;
    if (start > len) return 0;

    vm.re   = re;
    vm.text = text;
    vm.len  = len;
    vm.gen  = 0;

    for (i = 0; i < NOTE_REGEX_PROG; i++) { la.mark[i] = 0; lb.mark[i] = 0; }
    for (i = 0; i < NOTE_REGEX_SLOTS; i++) best[i] = -1;

    cl = &la; nl = &lb;
    cl->n = 0; cl->gen = ++vm.gen;

    for (pos = start; ; pos++) {
        const nchar *tp;
        unsigned u = 0;

        /* A fresh attempt starting here, at the lowest priority, so that an
         * earlier start always wins.  Once something has matched we stop
         * seeding: the leftmost match has been found. */
        if (!matched) {
            for (i = 0; i < NOTE_REGEX_SLOTS; i++) slots[i] = -1;
            nre_add(&vm, cl, 0, pos, slots);
        } else if (!cl->n) {
            break;      /* nothing left that could beat what we have */
        }

        nl->n = 0;
        nl->gen = ++vm.gen;

        tp = text + pos;
        if (pos < len) u = nre_uc(*tp);

        for (i = 0; i < cl->n; i++) {
            nre_thread *th = &cl->th[i];
            const note_regex_inst *in = &re->prog[th->pc];
            int j;

            if (in->op == NRE_OP_MATCH) {
                matched = 1;
                for (j = 0; j < NOTE_REGEX_SLOTS; j++) best[j] = th->slots[j];
                break;      /* every thread behind this one is lower priority */
            }
            if (pos < len && nre_step_char(&vm, in, u))
                nre_add(&vm, nl, th->pc + 1, pos + 1, th->slots);
        }

        sw = cl; cl = nl; nl = sw;

        if (pos >= len) break;
    }

    if (!matched) return 0;

    if (match_start) *match_start = best[0];
    if (match_end)   *match_end   = best[1];
    if (groups) {
        for (i = 0; i < ngroups * 2; i++)
            groups[i] = (i < NOTE_REGEX_SLOTS) ? best[i] : -1;
    }
    return 1;
}
