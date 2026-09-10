/* note_pack.c — LZSS decompressor and the definition finder over it.
 *
 * See note_pack.h for the format and for why note owns one at all.
 */

#include "note_pack.h"

unsigned long note_pack_size(const note_pack *z)
{
    return z->outlen;
}

int note_pack_open(note_pack *z, const unsigned char NOTE_PACK_FAR *blob,
                   unsigned long len)
{
    z->src = blob;
    z->srclen = z->srcpos = 0;
    z->outlen = z->outpos = 0;
    z->flags = z->nflags = 0;
    z->wpos = z->run = z->rpos = 0;

#if NOTE_PACK_FARMEM
    /* The window is the caller's here; a reader without one would write four
     * kilobytes through a null far pointer, which on a real-mode machine is
     * the interrupt vector table. */
    if (!z->win) return 0;
#endif
    if (!blob || len < NOTE_PACK_HEADER) return 0;
    if (blob[0] != 'N' || blob[1] != 'P' || blob[2] != 'K' || blob[3] != '1')
        return 0;

    z->outlen = (unsigned long)blob[4]        | ((unsigned long)blob[5] << 8) |
               ((unsigned long)blob[6] << 16) | ((unsigned long)blob[7] << 24);
    z->srclen = len;
    z->srcpos = NOTE_PACK_HEADER;

    /* The window starts as zeroes and a well-formed stream never reads a byte
     * of it that it has not written, so there is no need to clear it -- but a
     * corrupt one would, and reading whatever the last pack left there is a
     * worse failure than reading zeroes.  It costs 4 KB of stores once. */
    {
        unsigned int i;
        for (i = 0; i < NOTE_PACK_WINDOW; i++) z->win[i] = 0;
    }
    return 1;
}

int note_pack_get(note_pack *z)
{
    unsigned int b;

    if (z->outpos >= z->outlen) return -1;

    if (z->run) {
        b = z->win[z->rpos];
        z->rpos = (z->rpos + 1) & (NOTE_PACK_WINDOW - 1);
        z->run--;
    } else {
        if (!z->nflags) {
            if (z->srcpos >= z->srclen) return -1;
            z->flags  = z->src[z->srcpos++];
            z->nflags = 8;
        }
        b = z->flags & 1u;
        z->flags >>= 1;
        z->nflags--;

        if (b) {
            if (z->srcpos >= z->srclen) return -1;
            b = z->src[z->srcpos++];
        } else {
            unsigned int b0, b1, code, dist, len;

            if (z->srcpos + 2 > z->srclen) return -1;
            b0 = z->src[z->srcpos++];
            b1 = z->src[z->srcpos++];

            dist = 1 + (b0 | ((b1 >> 4) << 8));
            code = b1 & 0x0Fu;
            if (code == 15) {
                if (z->srcpos >= z->srclen) return -1;
                len = 18 + z->src[z->srcpos++];
            } else {
                len = code + 3;
            }

            /* Unsigned arithmetic is modulo a power of two and the window is
             * one, so the mask is the whole of the wrap-around: no branch and
             * no signed underflow to reason about. */
            z->rpos = (z->wpos - dist) & (NOTE_PACK_WINDOW - 1);
            z->run  = len - 1;
            b = z->win[z->rpos];
            z->rpos = (z->rpos + 1) & (NOTE_PACK_WINDOW - 1);
        }
    }

    z->win[z->wpos] = (unsigned char)b;
    z->wpos = (z->wpos + 1) & (NOTE_PACK_WINDOW - 1);
    z->outpos++;
    return (int)b;
}

/* --------------------------------------------------------------------------
 * Finding one definition
 *
 * Not compiled where the window is the caller's (NOTE_PACK_FARMEM): the
 * reader below is a file-scope one, so it would need a window of its own
 * before anything had a chance to hand it one.  See note_pack.h.
 * -------------------------------------------------------------------------- */
#if !NOTE_PACK_FARMEM

/* The longest line this has to understand is an `extensions` or a `name`, and
 * in the shipped packs those run to 52 and 28 characters.  A keyword list is
 * an order of magnitude longer and is never tested, so a line that overflows
 * is not truncated and compared -- it is marked and skipped, because a
 * truncated compare is how a definition claims an extension it does not
 * have. */
#define NOTE_PACK_LINE 128

/* One reader, at file scope, because it is mostly window: 4 KB is a stack
 * frame no 16-bit target would survive and the core allocates nothing.  Packs
 * are read one at a time at startup, so one is enough. */
static note_pack g_reader;

static int ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* Is this line a pack's document separator?  The same rule as note_core.c's
 * is_pack_sep: three or more dashes and nothing else that prints. */
static int is_sep(const char *s, int n)
{
    int i = 0, dashes = 0;
    while (i < n && s[i] == '-') { dashes++; i++; }
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    return dashes >= 3 && i == n;
}

/* Does `line` read `key = <something that matches want>`? */
static int line_claims(const char *line, const char *key,
                       const nchar *want, int mode)
{
    const char *p = line;
    const char *v, *end;
    int i;

    while (*p == ' ' || *p == '\t') p++;
    for (i = 0; key[i]; i++)
        if (p[i] != key[i]) return 0;
    p += i;

    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    v = p;
    end = p;
    while (*end) end++;
    while (end > v && (end[-1] == ' ' || end[-1] == '\t')) end--;

    if (mode == NOTE_PACK_WHOLE) {
        for (i = 0; v + i < end && want[i]; i++)
            if (ascii_lower(v[i]) != ascii_lower((int)want[i])) return 0;
        return v + i == end && !want[i];
    }

    while (v < end) {
        const char *w;
        while (v < end && *v == ' ') v++;
        if (v >= end) break;
        w = v;
        while (v < end && *v != ' ') v++;
        for (i = 0; w + i < v && want[i]; i++)
            if (ascii_lower(w[i]) != ascii_lower((int)want[i])) break;
        if (w + i == v && !want[i]) return 1;
    }
    return 0;
}

long note_pack_find(const unsigned char *blob, unsigned long len,
                    const char *key, const nchar *want, int mode,
                    nchar *out, long cap)
{
    char line[NOTE_PACK_LINE];
    long docstart = 0, linestart = 0;
    long hitstart = -1, hitend = -1;
    int  nline = 0, over = 0, matched = 0, done = 0;
    long need, i;
    int  c;

    if (!want || !want[0] || !out || cap <= 0) return -1;
    if (!note_pack_open(&g_reader, blob, len)) return -1;

    /* Pass one: where does the definition that claims `want` begin and end?
     * The last one to claim it wins, matching note_lang_from_path's backwards
     * scan over the registry, so this runs to the end of the stream rather
     * than stopping at the first hit. */
    while (!done) {
        c = note_pack_get(&g_reader);
        if (c < 0) { done = 1; if (!nline) break; }

        if (done || c == '\n') {
            line[nline] = 0;
            if (is_sep(line, nline)) {
                if (matched) { hitstart = docstart; hitend = linestart; }
                docstart = (long)g_reader.outpos;
                matched = 0;
            } else if (!over && !matched && line_claims(line, key, want, mode)) {
                matched = 1;
            }
            nline = 0;
            over = 0;
            linestart = (long)g_reader.outpos;
        } else if (c != '\r') {
            if (nline < NOTE_PACK_LINE - 1) line[nline++] = (char)c;
            else over = 1;
        }
    }
    if (matched) { hitstart = docstart; hitend = (long)g_reader.outpos; }

    if (hitstart < 0 || hitend <= hitstart) return -1;

    need = hitend - hitstart;
    if (need + 1 > cap) return -2;

    /* Pass two: the same stream again, thrown away until the definition and
     * copied from there.  A byte is an nchar because the blob is ASCII by
     * construction -- see the note in note_pack.h. */
    if (!note_pack_open(&g_reader, blob, len)) return -1;
    for (i = 0; i < hitstart; i++)
        if (note_pack_get(&g_reader) < 0) return -1;
    for (i = 0; i < need; i++) {
        c = note_pack_get(&g_reader);
        if (c < 0) return -1;
        out[i] = (nchar)(unsigned char)c;
    }
    out[need] = 0;
    return need;
}
#endif  /* !NOTE_PACK_FARMEM */
