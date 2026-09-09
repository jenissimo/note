/* note_syntax.h — portable syntax lexer over loadable language definitions.
 *
 * A language is data, not code: every definition — including the built-in
 * ones — is a note_conf document, so a user's .syntax file on disk goes
 * through exactly the same parser and can express exactly the same things.
 *
 * The lexer itself is pure core: it turns a run of text into coloured spans
 * and has no idea how colour is applied.  A backend asks for the spans
 * covering the range it is about to repaint and pushes them into its native
 * control.
 */
#ifndef NOTE_SYNTAX_H
#define NOTE_SYNTAX_H

#include "note_core.h"
#include "note_conf.h"

/* Token kinds.  TOK_TEXT is never emitted — it is the default the backend
 * paints first, and spans only ever override it. */
enum {
    TOK_TEXT = 0,
    TOK_KEYWORD,
    TOK_TYPE,
    TOK_COMMENT,
    TOK_STRING,
    TOK_NUMBER,
    TOK_PREPROC,
    TOK_OPERATOR,
    TOK_COUNT
};

typedef struct {
    int           start;   /* offset in the document */
    int           len;
    unsigned char kind;    /* TOK_* */
} note_span;

/* Per-language behaviour flags, set by the definition file. */
enum {
    SYN_PREPROC  = 1,   /* a leading # on a line is a preprocessor line */
    SYN_NOCASE   = 2,   /* keywords match case-insensitively            */
    SYN_TAGS     = 4,   /* colour <...> markup as keywords              */
    SYN_HEADINGS = 8    /* colour a leading # run as a heading          */
};

/* A pattern rule: a regular expression and the colour its matches take.
 *
 * Word lists cover words, which is most of a language and costs a lookup
 * rather than an NFA.  Rules cover what a list cannot say: operators, the
 * shape of a number literal, "an identifier in capitals is a constant".  The
 * expressions are kept as text and only compiled for the language of the
 * document actually on screen. */
typedef struct {
    const nchar  *pattern;
    unsigned char kind;          /* TOK_* */
} note_rule;

typedef struct {
    const nchar  *name;
    const nchar  *exts;          /* space separated, no dots: "c h cpp" */
    const nchar  *keywords;      /* space separated                     */
    const nchar  *types;
    const nchar  *line_comment;  /* NULL if none                        */
    const nchar  *block_open;    /* NULL if none                        */
    const nchar  *block_close;
    const nchar  *quotes;        /* characters that open a string       */
    note_rule     rules[NOTE_MAX_RULES];
    unsigned char nrules;
    unsigned char flags;
} note_lang;

/* LANG_NONE is always index 0 and highlights nothing. */
#define LANG_NONE 0

/* Registry ---------------------------------------------------------------- */

/* Clears the registry and parses the compiled-in definitions.  Every string
 * is copied into `arena`, which must outlive the registry. */
void note_syntax_init(note_arena *arena);

/* Parses one definition document and adds or replaces a language (matched by
 * name).  Returns its index, or -1 if the registry or arena is full. */
int note_syntax_add(note_arena *arena, const nchar *text);

int              note_lang_count(void);
const note_lang *note_lang_get(int lang);
int              note_lang_from_path(const nchar *path);

/* Widens an ASCII definition blob into a shared buffer.  Built-ins are
 * stored a byte per character to keep them out of the executable twice
 * over; only startup uses this, one definition at a time. */
const nchar     *note_syntax_widen(const char *s);

/* Lexer ------------------------------------------------------------------- */

/* Scan text[0..len) and write at most `max` spans.  `base` is added to every
 * span start so a backend can hand over a slice of the document.  Returns the
 * number of spans written. */
int note_tokenize(int lang, const nchar *text, int len, int base,
                  note_span *out, int max);

/* Where lexing may begin so that the region at `at` still comes out right.
 *
 * A backend only ever repaints what is on screen, but it cannot simply start
 * the lexer at the top of the view: a block comment or a multi-line string
 * may have opened far above it.  Lexing from the start of the document is
 * correct and is what a small file can afford, but it turns scrolling a large
 * one into re-lexing everything above the caret on every frame.
 *
 * This finds a point that is known to be outside any construct, by scanning
 * backwards for the nearest block-comment delimiter -- a plain character scan,
 * orders of magnitude cheaper than lexing.  It never looks further back than
 * `window` characters and always lands on a line boundary.
 */
int note_syntax_safe_start(int lang, const nchar *text, int len,
                           int at, int window);

#endif /* NOTE_SYNTAX_H */
