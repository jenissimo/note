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
#if NOTE_EMBEDDED_PACKS
#include "note_pack.h"
#endif

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

#if NOTE_EMBEDDED_PACKS
/* Registers the one language in `blob` that claims `path`'s extension, and
 * nothing else in the pack.  `scratch` is where the definition is assembled
 * and need only hold the largest one, not the pack.  Returns the language
 * index, or -1 if nothing claims the extension or it will not fit.
 *
 * For a backend that wants the language of the file it is opening rather than
 * every language there is -- which is every backend without a language picker,
 * and is the only shape the 16-bit builds can afford at all.  See note_pack.h
 * for why the pack is never held whole. */
int note_syntax_add_from_pack(note_arena *arena, const unsigned char *blob,
                              unsigned long len, const nchar *path,
                              nchar *scratch, long cap);
#endif

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
 * number of spans written.
 *
 * The run is assumed to start outside every construct, so this is only right
 * for a run that starts at the top of the document or at a point some other
 * means has proved safe.  note_tokenize_from() is the general form. */
int note_tokenize(int lang, const nchar *text, int len, int base,
                  note_span *out, int max);

/* Lexer state ------------------------------------------------------------- */

/* What the lexer was in the middle of when a run ended.
 *
 * This is the whole point of the incremental interface: the condition the
 * scanner carries from one line to the next is a single byte, so a caller can
 * remember it instead of re-deriving it.  NORMAL means outside everything;
 * BLOCK means inside a block comment; NOTE_SYN_STRING + i means inside a
 * string opened by the i'th character of the language's `quotes` -- strings
 * are told apart by their quote because only the matching one closes them.
 *
 * A run handed to the lexer must begin at the start of a line and include the
 * line terminators inside it.  A backslash at the end of a line continues a
 * string onto the next one, and a run cut before its newline would hide that.
 */
typedef unsigned char note_syn_state;

#define NOTE_SYN_NORMAL 0
#define NOTE_SYN_BLOCK  1
#define NOTE_SYN_STRING 2

/* As note_tokenize, but the run starts in `st` rather than in NORMAL, and the
 * state the run ends in is written to `end` when `end` is not NULL.
 *
 * When `end` is NULL this stops as soon as the span array fills, as
 * note_tokenize does.  When it is not, scanning continues past that point
 * without emitting, because the end state has to describe the whole run.
 * `out` may be NULL with `max` 0 to scan for the state alone. */
int note_tokenize_from(int lang, const nchar *text, int len, int base,
                       note_span *out, int max,
                       note_syn_state st, note_syn_state *end);

/* The state a run ends in, without producing spans. */
note_syn_state note_syntax_advance(int lang, const nchar *text, int len,
                                   note_syn_state st);

/* Checkpoints ------------------------------------------------------------- */

/* The state at the start of every K'th line, so that lexing any line costs at
 * most K lines rather than the document above it.
 *
 * One state per line would be correct and is what a desktop editor does, but
 * it is memory linear in the document, which is exactly the shape of cost this
 * is meant to remove -- a C64 has no hundred kilobytes to spend on a hundred
 * thousand lines.  So the table is a fixed array the caller owns, and K is
 * whatever makes the document fit it: a larger file gets coarser checkpoints
 * and a little more catch-up lexing, never a failure.
 *
 * Slot i holds the state at the start of line i*every.  Slot 0 is line 0 and
 * is always NORMAL.  `valid` counts the slots at the front that are known;
 * everything past it has not been lexed yet, so a lookup falls back to the
 * last slot that is.
 */
#if NOTE_LINE_CHECKPOINTS
/* The sparse checkpoint table answers "what state does line N start in"
 * for a document far too large to keep a state per line.  A machine that
 * cannot hold such a document does not need the machinery either -- and on
 * a 6502 it is a kilobyte of code that would never earn its place.  The
 * resumable lexer itself, note_tokenize_from and note_syntax_advance, is
 * in both profiles: that is the part every backend wants. */
typedef struct {
    note_syn_state *slots;   /* caller-owned storage         */
    int             cap;     /* slots available              */
    int             every;   /* K: lines between checkpoints */
    int             valid;   /* leading slots that are known */
} note_syn_ckpts;

/* Binds caller storage to a table and marks line 0 known.  `cap` must be at
 * least 1. */
void note_syn_ckpts_init(note_syn_ckpts *t, note_syn_state *slots, int cap);

/* Widens K until a document of `lines` lines fits the table.  Existing
 * checkpoints are kept: K only ever doubles, so every second slot of the old
 * table is still a checkpoint of the new one. */
void note_syn_ckpts_fit(note_syn_ckpts *t, int lines);

/* Forgets everything the text from `line` onwards decided.  An edit on line L
 * cannot change the state at the start of any line at or before L. */
void note_syn_ckpts_dirty(note_syn_ckpts *t, int line);

/* The nearest known checkpoint at or before `line`: returns the line it sits
 * on and writes its state to `st`.  Lexing forward from there reaches `line`
 * correctly, and costs at most K lines unless the table has not caught up. */
int note_syn_ckpts_state(const note_syn_ckpts *t, int line, note_syn_state *st);

/* Records the state at the start of `line`, if that line carries a checkpoint
 * and the one before it is already known.  Out of order records are dropped:
 * `valid` is a prefix, so a gap in it would let a lookup return a state that
 * nothing established. */
void note_syn_ckpts_record(note_syn_ckpts *t, int line, note_syn_state st);

/* Runs the state machine over `text`, which must begin at the start of line
 * `first_line`, and records a checkpoint at every checkpoint line it crosses.
 * Returns the state at the end of the run.
 *
 * This is how the table gets filled.  A caller that scrolls fills it a screen
 * at a time; a caller that jumps to the end of a cold document feeds it the
 * text in as many chunks as its copy buffer needs, each starting on a line
 * boundary, and pays for that scan once.
 */
note_syn_state note_syn_ckpts_scan(note_syn_ckpts *t, int lang,
                                   const nchar *text, int len,
                                   int first_line, note_syn_state st);

/* What a redraw looks like ------------------------------------------------
 *
 *   static note_syn_state slots[MAX_CKPTS];   -- the backend's own storage
 *   static note_syn_ckpts marks;
 *
 *   at startup, and again whenever a document is opened:
 *       note_syn_ckpts_init(&marks, slots, MAX_CKPTS);
 *
 *   after an edit on line L:
 *       note_syn_ckpts_dirty(&marks, L);
 *
 *   to colour the `rows` lines from `top`:
 *       note_syn_ckpts_fit(&marks, lines_in_document);
 *       from = note_syn_ckpts_state(&marks, top, &st);
 *       if (from < top) {
 *           copy the text of lines [from, top) into a scratch buffer
 *           st = note_syn_ckpts_scan(&marks, lang, buf, got, from, st);
 *       }
 *       copy the text of lines [top, top + rows) into a scratch buffer
 *       nspans = note_tokenize_from(lang, buf, got, offset_of_top,
 *                                   spans, MAX_SPANS, st, &after);
 *       note_syn_ckpts_record(&marks, top + rows, after);
 *
 * Nothing in that is proportional to the document: the lookup is a division,
 * the catch-up is bounded by K lines, and the last step is the screen.  A
 * backend too small to hold both copies at once can do the catch-up in as
 * many chunks as its buffer needs, feeding each to note_syn_ckpts_scan.
 */
#endif  /* NOTE_LINE_CHECKPOINTS */

/* Legacy ------------------------------------------------------------------ */

/* Where lexing may begin so that the region at `at` still comes out right.
 *
 * This is what a backend used before the state above existed: it scans
 * backwards for the nearest block-comment delimiter and returns a point that
 * is, as far as it can see, outside every construct.  As far as it can see is
 * the catch -- the scan stops after `window` characters, and a block comment
 * opened before that is simply not found, so the region comes out coloured as
 * code when it is comment.  It is also work proportional to the window on
 * every repaint, which the checkpoint table does not need at all.
 *
 * Kept because backends still call it while they move across; new code should
 * use note_syn_ckpts_state() and note_tokenize_from(), which are both cheaper
 * and, unlike this, always right.
 */
int note_syntax_safe_start(int lang, const nchar *text, int len,
                           int at, int window);

#endif /* NOTE_SYNTAX_H */
