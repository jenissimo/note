/* note_conf.c — parser for the shared key/value definition format. */

#include "note_conf.h"

void note_arena_reset(note_arena *a)
{
    a->used = 0;
}

const nchar *note_arena_put(note_arena *a, const nchar *s, int len)
{
    nchar *dst;
    int i;

    if (len < 0) len = n_len(s);
    if (a->used + len + 1 > NOTE_ARENA_CHARS) return 0;

    dst = a->buf + a->used;
    for (i = 0; i < len; i++) dst[i] = s[i];
    dst[len] = 0;
    a->used += len + 1;
    return dst;
}

static int is_space(nchar c)
{
    return c == (nchar)' ' || c == (nchar)'\t' ||
           c == (nchar)'\r' || c == (nchar)'\n';
}

int note_conf_next(const nchar **p, nchar *key, int kcap, nchar *val, int vcap)
{
    const nchar *s = *p;

    for (;;) {
        const nchar *line, *eq, *end;
        int i;

        while (*s == (nchar)'\r' || *s == (nchar)'\n' ||
               *s == (nchar)' '  || *s == (nchar)'\t') s++;
        if (!*s) { *p = s; return 0; }

        line = s;
        while (*s && *s != (nchar)'\n') s++;
        end = s;
        if (*s) s++;
        while (end > line && is_space(end[-1])) end--;

        if (*line == (nchar)'#' || *line == (nchar)';') continue;

        eq = line;
        while (eq < end && *eq != (nchar)'=') eq++;
        if (eq == end) continue;                 /* not a key = value line */

        {   /* key */
            const nchar *ke = eq;
            while (ke > line && is_space(ke[-1])) ke--;
            for (i = 0; line + i < ke && i < kcap - 1; i++) key[i] = line[i];
            key[i] = 0;
        }
        {   /* value */
            const nchar *vs = eq + 1;
            while (vs < end && is_space(*vs)) vs++;
            for (i = 0; vs + i < end && i < vcap - 1; i++) val[i] = vs[i];
            val[i] = 0;
        }

        if (!key[0]) continue;
        *p = s;
        return 1;
    }
}

int note_conf_bool(const nchar *v)
{
    return v[0] == (nchar)'1' ||
           v[0] == (nchar)'y' || v[0] == (nchar)'Y' ||
           v[0] == (nchar)'t' || v[0] == (nchar)'T' ||
           v[0] == (nchar)'o' || v[0] == (nchar)'O';   /* on */
}

/* Only a theme says #RRGGBB, so a build that never reads one at run time has
 * no caller for this and no reason to carry it -- and on the two targets that
 * is true of, three hundred and forty bytes of dead 6502 is not a rounding
 * error.  See NOTE_THEME_PARSE in note_config.h, which is this question and
 * not the question of whether there is a registry: the 16-bit MS-DOS build
 * parses themes and has no registry. */
#if NOTE_THEME_PARSE

static int hex_digit(nchar c)
{
    if (c >= (nchar)'0' && c <= (nchar)'9') return c - (nchar)'0';
    if (c >= (nchar)'a' && c <= (nchar)'f') return c - (nchar)'a' + 10;
    if (c >= (nchar)'A' && c <= (nchar)'F') return c - (nchar)'A' + 10;
    return -1;
}

note_color note_conf_color(const nchar *v)
{
    note_color c = 0;
    int i, d, n = 0;

    if (*v == (nchar)'#') v++;
    for (i = 0; i < 6 && v[i]; i++) {
        d = hex_digit(v[i]);
        if (d < 0) break;
        c = (c << 4) | (note_color)d;
        n++;
    }
    if (n == 3) {   /* #abc -> #aabbcc */
        note_color r = (c >> 8) & 0xF, g = (c >> 4) & 0xF, b = c & 0xF;
        c = (r << 20) | (r << 16) | (g << 12) | (g << 8) | (b << 4) | b;
    }
    return c;
}

#endif  /* NOTE_THEME_PARSE */

/* The distance from 'A' to 'a' is 32 in ASCII and 128 in PETSCII, so the gap
 * is computed from the literals rather than written down.  cc65 maps both to
 * the target's own character set, which makes this correct on a C64 without
 * knowing anything about a C64. */
static nchar lower(nchar c)
{
    return (c >= (nchar)'A' && c <= (nchar)'Z')
         ? (nchar)(c - (nchar)'A' + (nchar)'a')
         : c;
}

int note_word_in_list(const nchar *list, const nchar *word, int len, int nocase)
{
    const nchar *p = list;

    if (!list || len <= 0) return 0;

    while (*p) {
        const nchar *start;
        int n;

        while (*p == (nchar)' ') p++;
        if (!*p) break;

        start = p;
        while (*p && *p != (nchar)' ') p++;
        n = (int)(p - start);

        if (n == len) {
            int i;
            for (i = 0; i < n; i++) {
                nchar a = start[i], b = word[i];
                if (nocase) { a = lower(a); b = lower(b); }
                if (a != b) break;
            }
            if (i == n) return 1;
        }
    }
    return 0;
}
