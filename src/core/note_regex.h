/* note_regex.h — a small regular-expression engine for Find/Replace.
 *
 * The engine is a Pike VM: the pattern is compiled once into a Thompson NFA
 * program and the whole thread set is stepped across the text in lock-step,
 * so matching is linear in the length of the text no matter what the pattern
 * looks like.  A backtracking engine would let a pattern such as (a+)+b hang
 * the editor on a long line; this one cannot.
 *
 * The caller owns every byte: a note_regex holds the compiled program in
 * fixed-size arrays and the matcher keeps its thread lists on the stack.
 * Nothing here allocates, and the only include is note_core.h.
 *
 * Matches are leftmost-first: the same match a backtracker would report, and
 * the same submatches, with one documented exception.  When a capturing group
 * whose body can match nothing sits inside a repetition — (a?)* and the like
 * — a backtracker runs the body one last empty time and leaves the group set
 * to an empty span, while this engine enters each program counter only once
 * per position and so leaves it unset.  That single-visit rule is exactly what
 * buys the linear time bound, RE2 answers the same way, and no pattern anyone
 * types into a Find box depends on it.
 *
 * Text is nchar — UTF-16 code units on Windows, UTF-8 bytes elsewhere.  The
 * engine treats an nchar as an opaque code unit: literals, classes and ranges
 * compare code unit against code unit, so a pattern written in ASCII behaves
 * identically on both, and a non-ASCII literal in the pattern matches the same
 * sequence of code units in the text.  Case folding and \d \w \s are ASCII.
 */
#ifndef NOTE_REGEX_H
#define NOTE_REGEX_H

#include "note_core.h"

/* --------------------------------------------------------------------------
 * Fixed limits.  A profile may shrink them; the defaults are sized so that a
 * note_regex is under 1 KB and the matcher's stack frame stays around 22 KB,
 * and so that every count stays far inside a 16-bit int.
 * -------------------------------------------------------------------------- */
#ifndef NOTE_REGEX_PROG
  #define NOTE_REGEX_PROG       128   /* instructions in one program        */
#endif
#ifndef NOTE_REGEX_CLASSES
  #define NOTE_REGEX_CLASSES     16   /* distinct [...] sets in one pattern */
#endif
#ifndef NOTE_REGEX_RANGES
  #define NOTE_REGEX_RANGES      96   /* ranges shared by all those sets    */
#endif
#ifndef NOTE_REGEX_GROUPS
  #define NOTE_REGEX_GROUPS      10   /* group 0 is the whole match         */
#endif
#ifndef NOTE_REGEX_MAX_REPEAT
  #define NOTE_REGEX_MAX_REPEAT  32   /* largest m or n in {m,n}            */
#endif

#define NOTE_REGEX_SLOTS (NOTE_REGEX_GROUPS * 2)

/* Compile flags. */
enum {
    NRE_ICASE     = 1,   /* ASCII-insensitive literals and classes         */
    NRE_MULTILINE = 2,   /* ^ and $ also match at line breaks              */
    NRE_DOTALL    = 4    /* . also matches CR and LF                       */
};

/* --------------------------------------------------------------------------
 * The compiled program.  Public only so the caller can declare one; treat the
 * members as private.
 * -------------------------------------------------------------------------- */
typedef struct {
    unsigned char  op;   /* NRE_OP_*                                       */
    unsigned short x;    /* code unit, class index, slot or branch target  */
    unsigned short y;    /* second branch target of NRE_OP_SPLIT           */
} note_regex_inst;

typedef struct {
    unsigned short first;   /* first range in note_regex.rng               */
    unsigned short count;
    unsigned char  neg;     /* [^...]                                      */
    unsigned char  cats;    /* \d \D \w \W \s \S written inside the set    */
} note_regex_class;

typedef struct {
    unsigned short lo, hi;
} note_regex_range;

typedef struct note_regex note_regex;

struct note_regex {
    note_regex_inst  prog[NOTE_REGEX_PROG];
    note_regex_class cls[NOTE_REGEX_CLASSES];
    note_regex_range rng[NOTE_REGEX_RANGES];
    int          nprog;
    int          ncls;
    int          nrng;
    int          ngroups;   /* capturing groups, not counting group 0      */
    unsigned     flags;
    const nchar *err;       /* a static literal, never allocated           */
};

/* --------------------------------------------------------------------------
 * API.  Both calls return 1 on success and 0 on failure.
 * -------------------------------------------------------------------------- */

/* Compiles `pattern` into `re`.  On failure note_regex_error(re) says why. */
int note_regex_compile(note_regex *re, const nchar *pattern, unsigned flags);

/* Finds the leftmost match at or after `start`.  `len` is the length of the
 * text in nchars, or -1 to measure it.  `match_start` / `match_end` and
 * `groups` may all be NULL.  `groups` receives 2*ngroups offsets: [0],[1] are
 * the whole match, [2k],[2k+1] are group k, and a group that did not take
 * part is -1,-1. */
int note_regex_search(const note_regex *re, const nchar *text, int len,
                      int start, int *match_start, int *match_end,
                      int *groups, int ngroups);

/* The reason the last compile failed, or an empty string. */
const nchar *note_regex_error(const note_regex *re);

/* How many capturing groups the pattern has (0 if it has none). */
int note_regex_groups(const note_regex *re);

#endif /* NOTE_REGEX_H */
