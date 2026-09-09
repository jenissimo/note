/* note_conf.h — the tiny key/value format that syntax and theme files share.
 *
 * Deliberately not JSON, YAML or TextMate plists: a text editor this size
 * should not carry a parser bigger than its editor.  The format is
 *
 *     # comment
 *     key = value with spaces
 *     keywords = if else while
 *
 * Blank lines and #/; comments are skipped, whitespace around key and value
 * is trimmed, and the first '=' splits the line.  The built-in languages and
 * themes are written in this same format and parsed through this same code,
 * so anything the built-ins express, a user's file can express too.
 */
#ifndef NOTE_CONF_H
#define NOTE_CONF_H

#include "note_core.h"

/* Strings parsed out of definition files live here for the process lifetime.
 * A bump arena keeps it to one allocation and no ownership questions.  Its
 * size comes from the build profile in note_config.h. */
typedef struct {
    nchar buf[NOTE_ARENA_CHARS];
    int   used;
} note_arena;

void         note_arena_reset(note_arena *a);
/* Copies `len` chars (or up to a NUL when len < 0) and returns a stable,
 * NUL-terminated pointer, or NULL when the arena is full. */
const nchar *note_arena_put(note_arena *a, const nchar *s, int len);

/* Walks one key/value pair per call.  *p is advanced past the line consumed.
 * Returns 1 while a pair was produced, 0 at end of text. */
int note_conf_next(const nchar **p, nchar *key, int kcap, nchar *val, int vcap);

/* Value helpers. */
int      note_conf_bool (const nchar *v);            /* yes/true/on/1 */
unsigned note_conf_color(const nchar *v);            /* "#RRGGBB" -> 0xRRGGBB */

/* Is `word` (length `len`) present in a space-separated list? */
int note_word_in_list(const nchar *list, const nchar *word, int len, int nocase);

#endif /* NOTE_CONF_H */
